"""The SoundFont player (sfsynth.c): channels, MIDI, presets, voices, modes, outputs and its state lines, played
through smp_synth_render_test from a small font written by sf2make."""
import ctypes
import itertools
import os
import threading
import time

import numpy as np
import pytest
import soundfile as sf

from sf2make import write_sf2

SR = 44100      # the samples' rate
OUT = 48000     # the output rate
FP = ctypes.POINTER(ctypes.c_float)
_count = itertools.count()
MAXV = 1088     # voice slots: more than sf_voices' top (1024), so stolen voices can fade

# generator ids (SF2.04)
PAN, RELEASE_VOL, EXCL, MODES, ROOTKEY = 17, 38, 57, 54, 58
P_VOLUME, P_VOICES, P_EFFECTS, P_TRANSPOSE = 31, 32, 33, 34


def sine(freq, secs, amp=16000):
    t = np.arange(int(secs * SR)) / SR
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.int16)


def zone(sample, keys=(0, 127), gens=None):
    return dict(keys=keys, vels=(0, 127), sample=sample, gens=dict(gens or {}), mods=[])


def preset(name, bank, program, zones):
    return dict(name=name, bank=bank, program=program, gens={}, pmods=[],
                instruments=[dict(name=name, gens={}, mods=[], zones=zones)])


def write_font(path, base_pitch=69):
    """The fixture font. At key 69: piano 440 Hz (bank 0 program 0, 1 s release), bass 220 Hz (0:33), the drum
    kit 880 Hz (128:0; key 46 an open hi-hat at 440 Hz and key 42 a closed one at 880 Hz, both exclusive class 1),
    a looping organ at 330 Hz (0:16), a two-region (stereo) piano at 0:1, and a second kit at 128:25 (220 Hz). base_pitch moves the piano's sample (font B in the switch test)."""
    samples = [
        dict(name="piano", data=sine(440, 2.0), rate=SR, pitch=base_pitch, loop=None),
        dict(name="bass", data=sine(440, 2.0), rate=SR, pitch=81, loop=None),
        dict(name="kit", data=sine(440, 2.0), rate=SR, pitch=57, loop=None),
        dict(name="organ", data=sine(330, 1.0), rate=SR, pitch=69, loop=(4410, 4410 * 9)),
    ]
    kit = [zone(2, (0, 41)), zone(2, (43, 45)), zone(2, (47, 127)),
           zone(0, (42, 42), {ROOTKEY: 30, EXCL: 1}),
           zone(0, (46, 46), {ROOTKEY: 46, EXCL: 1})]
    presets = [preset("Piano", 0, 0, [zone(0, gens={RELEASE_VOL: 0})]),
               preset("Bass", 0, 33, [zone(1)]),
               preset("Kit", 128, 0, kit),
               preset("Organ", 0, 16, [zone(3, gens={MODES: 1})]),
               preset("Stereo", 0, 1, [zone(0, gens={PAN: -500}), zone(0, gens={PAN: 500})]),
               preset("Kit2", 128, 25, [zone(1)])]
    write_sf2(path, presets, samples)


@pytest.fixture
def font(tmp_path):
    p = tmp_path / "gm.sf2"
    write_font(p)
    return p


@pytest.fixture
def S(lib, font, tmp_path):
    class H:
        synths = []

        def new(self, load=True, dry=True):
            """dry: sf_effects off, so the tests of notes and controllers hear the voices alone (the effect tests ask for dry=False)"""
            s = lib.smp_synth_new(float(OUT), 256)
            assert s
            self.synths.append(s)
            if dry:
                lib.smp_synth_set_param(s, P_EFFECTS, 0)
            if load:
                err = ctypes.create_string_buffer(512)
                assert lib.smp_synth_load(s, str(font).encode(), err, 512) == 1, err.value
            return s

        def render(self, s, events, secs, all_outputs=0, block=256):
            out = tmp_path / ("r%d.wav" % next(_count))
            frames = int(secs * OUT)
            rc = lib.smp_synth_render_test(s, events.encode(), frames, block, str(out).encode(), all_outputs)
            assert rc == 0
            x, rate = sf.read(str(out), dtype="float32", always_2d=True)
            assert rate == OUT and x.shape == (frames, 32 if all_outputs else 2)
            assert np.all(np.isfinite(x))
            return x

        def write(self, s):
            n = lib.smp_synth_script_write(s, None, 0)
            buf = ctypes.create_string_buffer(n + 1)
            assert lib.smp_synth_script_write(s, buf, n + 1) == n
            return buf.value.decode()

        def apply(self, s, text):
            err = ctypes.create_string_buffer(1024)
            r = lib.smp_synth_script_apply(s, text.encode(), err, 1024)
            return r, err.value.decode()

    h = H()
    yield h
    for s in h.synths:
        lib.smp_synth_free(s)


def peak_freq(x, rate=OUT):
    """the strongest frequency: Hann window, 8x zero padding, parabolic interpolation on log magnitude"""
    x = x - np.mean(x)
    n = len(x)
    spec = np.abs(np.fft.rfft(x * np.hanning(n), 8 * n)) + 1e-12
    k = int(np.argmax(spec))
    a, b, c = np.log(spec[k - 1:k + 2])
    d = 0.5 * (a - c) / (a - 2 * b + c)
    return (k + d) * rate / (8 * n)


def tone(x, f, rate=OUT):
    """the amplitude of frequency f in x (Hann-weighted)"""
    w = np.hanning(len(x))
    t = np.arange(len(x)) / rate
    return 2 * abs(np.sum(x * w * np.exp(-2j * np.pi * f * t))) / np.sum(w)


def seg(x, a, b, ch=0):
    return x[int(a * OUT):int(b * OUT), ch]


def rms(x):
    return float(np.sqrt(np.mean(np.square(x, dtype=np.float64))))


def onset(x, th=1e-3):
    idx = np.nonzero(np.abs(x[:, 0]) > th)[0]
    return int(idx[0]) if len(idx) else None


def freq_of(S, s, events, secs=0.5):
    x = S.render(s, events, secs)
    return peak_freq(seg(x, 0.1, secs - 0.05))


def test_program_change_and_bank(lib, S):
    s = S.new()
    assert abs(freq_of(S, s, "midi 193 33 0\nmidi 145 69 100 0") - 220) < 1      # program 33 on channel 2: bass
    # a drum bank select then program 0 on channel 1: the kit. MIDI's CC0 tops out at 127, the XG drum bank,
    # which the player treats as drums (the font has no bank 127, so it falls back to 128:0)
    assert abs(freq_of(S, s, "midi 176 0 127 0\nmidi 192 0 0\nmidi 144 69 100 0") - 880) < 2
    assert abs(freq_of(S, s, "midi 153 69 100 0") - 880) < 2                     # channel 10: drums by default
    assert abs(freq_of(S, s, "midi 144 69 100 0") - 440) < 1                     # channel 1: the piano


def test_program_fallback(lib, S):
    s = S.new()
    assert abs(freq_of(S, s, "midi 176 0 8 0\nmidi 192 33 0\nmidi 144 69 100 0") - 220) < 1    # bank 0's bass
    assert abs(freq_of(S, s, "midi 201 99 0\nmidi 153 69 100 0") - 880) < 2                    # kit 99: 128:0
    # an XG drum bank (127) with a kit the font has at 128: 128:25 before 128:0
    assert abs(freq_of(S, s, "midi 176 0 127 0\nmidi 192 25 0\nmidi 144 69 100 0") - 220) < 1
    assert abs(freq_of(S, s, "midi 176 0 50 0\nmidi 192 120 0\nmidi 144 69 100 0") - 440) < 1  # the first preset
    # XG style: CC0 0 and CC32 naming a bank the font has (bank 0 here, so a no-op), or one it lacks (ignored)
    assert abs(freq_of(S, s, "midi 176 0 0 0\nmidi 176 32 5 0\nmidi 192 33 0\nmidi 144 69 100 0") - 220) < 1


def test_pending_note_starts_when_loaded(lib, S):
    s = S.new()
    lib.smp_synth_test_offline(s, 0)                    # real time: the render doesn't wait for the loader
    x = S.render(s, "midi 192 33 0\nmidi 144 69 100 0", 0.6)
    t = onset(x)
    assert t is not None and t < 0.3 * OUT, t           # late at most by the load time, never lost
    assert abs(peak_freq(x[t + 2400:t + 2400 + 12000, 0]) - 220) < 1.5
    s2 = S.new()                                        # waiting on (the default): exactly on its frame
    x = S.render(s2, "midi 192 33 1000\nmidi 144 69 100 1000", 0.4)
    t = onset(x)
    assert t is not None and 1000 <= t < 1000 + 200, t


def test_pending_drop_is_real_time(lib, S):
    """a note waiting for its sample is dropped after 1 s of real time, not of rendered frames: a render that
    runs faster than real time without saying it is offline still gets the first notes after a program change"""
    lib.smp_sf_test_pause.argtypes = [ctypes.c_int]
    s = S.new()
    assert lib.smp_synth_wait_loaded(s, 5000) == 0
    bufs = [np.zeros(256, np.float32) for _ in range(32)]
    arr = (FP * 32)(*[bb.ctypes.data_as(FP) for bb in bufs])

    def run(blocks):
        peak = 0.0
        for _ in range(blocks):
            lib.smp_synth_process(s, arr, 256)
            peak = max(peak, float(np.max(np.abs(bufs[0]))))
        return peak

    def until_sound(secs):
        t0 = time.time()
        while time.time() - t0 < secs:
            if run(1) > 0.01:
                return True
            time.sleep(0.005)
        return False

    try:
        lib.smp_sf_test_pause(1)
        lib.smp_synth_midi(s, bytes([0xC1, 33]), 2, 0)          # the bass: not loaded
        lib.smp_synth_midi(s, bytes([0x91, 69, 100]), 3, 0)
        t0 = time.time()
        assert run(400) == 0                                    # 2.1 s of audio, rendered in far less real time
        assert time.time() - t0 < 0.8
        lib.smp_sf_test_pause(0)
        assert until_sound(3)                                   # late, but not lost
        lib.smp_synth_midi(s, bytes([0xB1, 120, 0]), 3, 0)
        run(10)
        # a note that waits longer than 1 s of real time is dropped
        lib.smp_sf_test_pause(1)
        lib.smp_synth_midi(s, bytes([0xC1, 16]), 2, 0)          # the organ: not loaded
        lib.smp_synth_midi(s, bytes([0x91, 69, 100]), 3, 0)
        run(1)
        time.sleep(1.2)
        run(1)
        lib.smp_sf_test_pause(0)
        assert lib.smp_synth_wait_loaded(s, 5000) == 0
        assert run(40) == 0
    finally:
        lib.smp_sf_test_pause(0)


def test_one_preset_mode(lib, S):
    s = S.new()
    r, err = S.apply(s, "sf_mode one\nsf_preset 0 33\nsf_channel 3\n")
    assert r == 0, err
    assert abs(freq_of(S, s, "midi 146 69 100 0") - 220) < 1                     # channel 3 plays the bass
    x = S.render(s, "midi 144 69 100 0", 0.3)
    assert np.max(np.abs(x)) < 1e-6                                              # channel 1 isn't listened to
    assert abs(freq_of(S, s, "midi 194 0 0\nmidi 146 69 100 0") - 220) < 1       # program changes are ignored
    assert abs(freq_of(S, s, "midi 194 16 0\nmidi 146 69 100 4800") - 220) < 1   # and stay ignored for later notes
    r, err = S.apply(s, "sf_mode one\nsf_preset 0 33\nsf_channel 10\n")         # channel 10: no drum rule
    assert r == 0, err
    assert abs(freq_of(S, s, "midi 153 69 100 0") - 220) < 1
    r, err = S.apply(s, "sf_mode one\nsf_preset 127 99\n")                      # bank 127 is drums here too
    assert r == 0, err
    assert abs(freq_of(S, s, "midi 144 69 100 0") - 880) < 2


def test_exclusive_class(lib, S):
    s = S.new()
    x = S.render(s, "midi 153 46 100 0\nmidi 153 42 100 24000", 0.8)
    before = seg(x, 0.25, 0.35)
    after = x[24000 + 480:24000 + 480 + 4800, 0]       # from 10 ms after the closed hi-hat
    assert tone(before, 440) > 0.05
    assert tone(after, 440) < 0.01 * tone(before, 440)
    assert tone(after, 880) > 0.05


def test_sustain_and_sostenuto(lib, S):
    s = S.new()
    x = S.render(s, "midi 192 16 0\nmidi 176 64 127 0\nmidi 144 69 100 0\nmidi 128 69 0 9600\n"
                    "midi 176 64 0 28800", 1.0)
    assert tone(seg(x, 0.4, 0.55), 330) > 0.02          # released under the pedal: still sounding
    assert np.max(np.abs(seg(x, 0.65, 1.0))) < 1e-4     # pedal up: gone
    b = 330 * 2 ** (7 / 12)
    x = S.render(s, "midi 192 16 0\nmidi 144 69 100 0\nmidi 176 66 127 4800\nmidi 144 76 100 7200\n"
                    "midi 128 69 0 14400\nmidi 128 76 0 14400\nmidi 176 66 0 28800", 1.0)
    assert tone(seg(x, 0.18, 0.28), b) > 0.02           # B sounded while held
    w = seg(x, 0.4, 0.55)
    assert tone(w, 330) > 0.02                          # A was down when sostenuto went on: held
    assert tone(w, b) < 0.002                           # B wasn't: let go at its note-off
    assert np.max(np.abs(seg(x, 0.65, 1.0))) < 1e-4


HELD = "midi 192 16 0\nmidi 176 64 127 0\nmidi 144 69 100 0\nmidi 128 69 0 4800\n"


@pytest.mark.parametrize("event,also_held", [
    ("midi 176 120 0 24000", True),
    ("midi 176 121 0 24000", False),
    ("reset 24000", True),
    ("midi 240 126 127 9 1 247 24000", True),                       # GM reset
    ("midi 240 65 16 66 18 64 0 127 0 65 247 24000", True),         # GS reset
    ("midi 240 67 16 76 0 0 126 0 247 24000", True),                # XG reset
])
def test_no_stuck_notes(lib, S, event, also_held):
    s = S.new()
    ev = HELD + ("midi 144 76 100 0\n" if also_held else "") + event
    x = S.render(s, ev, 1.0)
    assert tone(seg(x, 0.3, 0.45), 330) > 0.02
    assert np.max(np.abs(seg(x, 0.6, 1.0))) < 1e-4, event


def test_no_stuck_notes_all_notes_off(lib, S):
    s = S.new()
    x = S.render(s, HELD + "midi 176 123 0 24000\nmidi 176 64 0 38400", 1.2)
    assert tone(seg(x, 0.55, 0.78), 330) > 0.02         # All Notes Off respects the pedal
    assert np.max(np.abs(seg(x, 0.9, 1.2))) < 1e-4      # gone within 0.1 s of the pedal-up


def zero_cross_cents(x, ref):
    """the pitch of each cycle in cents from ref, from upward zero crossings (linearly interpolated)"""
    i = np.nonzero((x[:-1] < 0) & (x[1:] >= 0))[0]
    t = i + (-x[i]) / (x[i + 1] - x[i])
    f = OUT / np.diff(t)
    return 1200 * np.log2(f / ref)


def test_controllers(lib, S):
    s = S.new()
    # CC7 64 against 127, changed mid-note (smp_sfv_update): the concave default modulator, (64/127)^2
    x = S.render(s, "midi 176 7 127 0\nmidi 144 69 100 0\nmidi 176 7 64 24000", 1.0)
    db = 20 * np.log10(rms(seg(x, 0.7, 0.95)) / rms(seg(x, 0.2, 0.45)))
    assert abs(db - 20 * np.log10((64 / 127) ** 2)) < 0.3, db
    x = S.render(s, "midi 176 10 0 0\nmidi 144 69 100 0", 0.5)                  # hard left
    assert rms(seg(x, 0.1, 0.4, 1)) < 1e-3 * rms(seg(x, 0.1, 0.4, 0))
    x = S.render(s, "midi 144 69 100 0", 1.0)
    assert np.max(np.abs(zero_cross_cents(seg(x, 0.1, 0.9), 440))) < 3
    x = S.render(s, "midi 176 1 127 0\nmidi 144 69 100 0", 1.0)                 # vibrato
    assert np.max(np.abs(zero_cross_cents(seg(x, 0.1, 0.9), 440))) >= 20
    # pitch bend through the default pitch wheel modulator: 2 semitones by default, 12 after RPN 0
    assert abs(freq_of(S, s, "midi 224 127 127 0\nmidi 144 69 100 0") - 440 * 2 ** (2 * 8191 / 8192 / 12)) < 1
    rpn0 = "midi 176 101 0 0\nmidi 176 100 0 0\nmidi 176 6 12 0\nmidi 176 38 0 0\n"
    assert abs(freq_of(S, s, rpn0 + "midi 224 127 127 0\nmidi 144 69 100 0") - 440 * 2 ** (12 * 8191 / 8192 / 12)) < 2
    # RPN 1 fine tuning (+50 cents) and RPN 2 coarse tuning (+2 semitones); NRPNs are ignored
    rpn1 = "midi 176 101 0 0\nmidi 176 100 1 0\nmidi 176 6 96 0\nmidi 176 38 0 0\n"
    assert abs(freq_of(S, s, rpn1 + "midi 144 69 100 0") - 440 * 2 ** (50 / 1200)) < 1
    rpn2 = "midi 176 101 0 0\nmidi 176 100 2 0\nmidi 176 6 66 0\n"
    assert abs(freq_of(S, s, rpn2 + "midi 144 69 100 0") - 440 * 2 ** (2 / 12)) < 1
    nrpn = "midi 176 99 0 0\nmidi 176 98 0 0\nmidi 176 6 100 0\n"
    assert abs(freq_of(S, s, nrpn + "midi 144 69 100 0") - 440) < 1
    # CC121 recentres the bend and nulls the RPN selection (a CC6 after it changes nothing); CC122 isn't a note-off
    assert abs(freq_of(S, s, "midi 224 127 127 0\nmidi 176 121 0 0\nmidi 144 69 100 0") - 440) < 1
    assert abs(freq_of(S, s, "midi 176 101 0 0\nmidi 176 100 0 0\nmidi 176 121 0 0\nmidi 176 6 12 0\n"
                             "midi 224 127 127 0\nmidi 144 69 100 0") - 440 * 2 ** (2 * 8191 / 8192 / 12)) < 1
    x = S.render(s, "midi 144 69 100 0\nmidi 176 122 0 4800", 0.5)
    assert rms(seg(x, 0.3, 0.45)) > 0.05
    # a bend arriving mid-note moves the sounding note
    x = S.render(s, "midi 144 69 100 0\nmidi 224 127 127 24000", 1.0)
    assert abs(peak_freq(seg(x, 0.6, 0.95)) - 440 * 2 ** (2 * 8191 / 8192 / 12)) < 1


def voices(lib, s):
    peak = ctypes.c_int()
    n = lib.smp_synth_voices(s, ctypes.byref(peak))
    keys = []
    for i in range(MAXV):
        c, k, rel = ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
        if lib.smp_synth_test_voice(s, i, ctypes.byref(c), ctypes.byref(k), ctypes.byref(rel)):
            keys.append(k.value)
    assert len(keys) == n
    return n, peak.value, sorted(keys)


def test_voice_limit_and_stealing(lib, S):
    s = S.new()
    lib.smp_synth_set_param(s, P_VOICES, 16)
    ev = "\n".join("midi 144 %d 100 %d" % (30 + i, i * 480) for i in range(40))   # each past its attack
    S.render(s, ev, 0.5)
    n, peak, keys = voices(lib, s)
    assert n == 16 and peak == 16 and keys == list(range(54, 70))   # the oldest went first
    # a voice in its release goes before a held one, even a quieter held one
    ev = "\n".join("midi 144 %d %d 0" % (k, 127 if k == 45 else 30) for k in range(40, 56))
    S.render(s, ev + "\nmidi 128 45 0 4800\nmidi 144 70 30 7200", 0.3)
    n, peak, keys = voices(lib, s)
    assert n == 16 and keys == sorted([k for k in range(40, 56) if k != 45] + [70])


def test_chord_at_the_limit(lib, S):
    """a chord at the voice limit keeps every voice it starts: a new voice in its delay or attack isn't 'quiet',
    and a note-on never steals the voices it just started (here 4 keys x 2 regions on channel 2)"""
    s = S.new()
    lib.smp_synth_set_param(s, P_VOICES, 16)
    held = "\n".join("midi 144 %d 100 0" % k for k in range(40, 56))
    chord = "\n".join("midi 145 %d 100 24000" % k for k in (60, 64, 67, 72))
    S.render(s, held + "\nmidi 193 1 0\n" + chord, 0.6)
    n, peak, keys = voices(lib, s)
    chans = []
    for i in range(MAXV):
        c, k, rel = ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
        if lib.smp_synth_test_voice(s, i, ctypes.byref(c), ctypes.byref(k), ctypes.byref(rel)):
            chans.append((c.value, k.value))
    assert n == 16 and peak == 16
    assert sorted(k for c, k in chans if c == 1) == [60, 60, 64, 64, 67, 67, 72, 72]
    # at the very top, stolen voices still have slots to fade in
    lib.smp_synth_set_param(s, P_VOICES, 1024)
    S.render(s, "\n".join("midi 144 %d 100 %d" % (k % 128, k) for k in range(1100)), 0.1)
    assert lib.smp_synth_voices(s, None) <= 1024


def test_used_presets_released_at_reset(lib, S):
    """the state's sf_used presets are let go at a reset; the song's own (the channels' power-on presets, piano
    and the kit) stay, so only the bass's sample goes"""
    lib.smp_sf_test_grace(20)
    try:
        s = S.new()
        r, err = S.apply(s, "sf_used 0:0 0:33 128:0\n")
        assert r == 0, err
        assert lib.smp_synth_wait_loaded(s, 5000) == 0
        f = lib.smp_synth_font(s)
        assert lib.smp_sf_loaded_samples(f) == 3
        S.render(s, "midi 144 69 100 0", 0.1)
        time.sleep(0.3)
        assert lib.smp_sf_loaded_samples(f) == 3       # still held: no reset, and 10 s haven't passed
        S.render(s, "reset 0", 0.1)
        t0 = time.time()
        while lib.smp_sf_loaded_samples(f) != 2 and time.time() - t0 < 3:
            time.sleep(0.01)
        assert lib.smp_sf_loaded_samples(f) == 2
        assert "0:33" not in S.write(s)
    finally:
        lib.smp_sf_test_grace(0)


def test_used_presets_held_until_10s_after_first_note(lib, S):
    """the state's sf_used presets are held until 10 s after the first note-on, not 10 s after the state loaded
    (a project loads long before Play is pressed)"""
    lib.smp_sf_test_grace(20)
    try:
        s = S.new()
        r, err = S.apply(s, "sf_used 0:0 0:33 128:0\n")
        assert r == 0, err
        assert lib.smp_synth_wait_loaded(s, 5000) == 0
        f = lib.smp_synth_font(s)
        assert lib.smp_sf_loaded_samples(f) == 3
        S.render(s, "midi 176 7 100 0", 12.0)              # 12 s without a note: still held
        time.sleep(0.2)
        assert lib.smp_sf_loaded_samples(f) == 3
        assert "0:33" in S.write(s)
        S.render(s, "midi 144 69 100 0\nmidi 128 69 0 4800", 9.5)   # the first note: 9.5 s later, still held
        time.sleep(0.2)
        assert lib.smp_sf_loaded_samples(f) == 3
        S.render(s, "midi 176 7 100 0", 1.0)               # past 10 s: the bass goes
        t0 = time.time()
        while lib.smp_sf_loaded_samples(f) != 2 and time.time() - t0 < 3:
            time.sleep(0.01)
        assert lib.smp_sf_loaded_samples(f) == 2
        assert "0:33" not in S.write(s)
    finally:
        lib.smp_sf_test_grace(0)


def test_dormant_lets_presets_go(lib, S):
    """while the sampler plays (smp_synth_skip each block), the channels' presets and the sf_used ones are let go,
    so their samples are freed; playing again uses the channels' presets again"""
    lib.smp_synth_skip.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.smp_sf_loaded_bytes.restype = ctypes.c_uint64
    lib.smp_sf_test_grace(20)
    try:
        s = S.new()
        r, err = S.apply(s, "sf_used 0:33\n")
        assert r == 0, err
        bufs = [np.zeros(256, np.float32) for _ in range(32)]
        arr = (FP * 32)(*[bb.ctypes.data_as(FP) for bb in bufs])
        lib.smp_synth_midi(s, bytes([0xC1, 16]), 2, 0)          # channel 2: the organ
        lib.smp_synth_process(s, arr, 256)
        assert lib.smp_synth_wait_loaded(s, 5000) == 0
        f = lib.smp_synth_font(s)
        before = lib.smp_sf_loaded_bytes(f)
        assert lib.smp_sf_loaded_samples(f) == 4               # piano, kit, organ, and the held bass
        for _ in range(4):
            lib.smp_synth_skip(s, 256)
        t0 = time.time()
        while lib.smp_sf_loaded_bytes(f) != 0 and time.time() - t0 < 3:
            time.sleep(0.01)
            lib.smp_synth_skip(s, 256)
        assert lib.smp_sf_loaded_bytes(f) == 0
        assert "sf_used 0:0 0:16 128:0" in S.write(s)            # what the channels would play is still saved
        lib.smp_synth_process(s, arr, 256)                       # playing again
        assert lib.smp_synth_wait_loaded(s, 5000) == 0
        assert lib.smp_sf_loaded_samples(f) == 3               # the channels' presets: piano, kit, organ
        assert 0 < lib.smp_sf_loaded_bytes(f) < before
        lib.smp_synth_midi(s, bytes([0x91, 69, 100]), 3, 0)     # and the organ plays
        lib.smp_synth_process(s, arr, 256)
        lib.smp_synth_process(s, arr, 256)
        assert np.max(np.abs(bufs[0])) > 0.01
    finally:
        lib.smp_sf_test_grace(0)


def test_outputs(lib, S):
    s = S.new()
    r, err = S.apply(s, "sf_output 2 3\n")
    assert r == 0, err
    x = S.render(s, "midi 145 69 100 0", 0.4, all_outputs=1)
    for c in range(32):
        if c in (4, 5):
            assert rms(x[:, c]) > 0.01, c
        else:
            assert np.max(np.abs(x[:, c])) < 1e-6, c
    x = S.render(s, "midi 144 69 100 0", 0.4, all_outputs=1)        # channel 1: output 1
    assert rms(x[:, 0]) > 0.01 and np.max(np.abs(x[:, 2:])) < 1e-6
    r, err = S.apply(s, "sf_mode one\nsf_output 2 3\n")              # one preset: everything on output 1
    assert r == 0, err
    x = S.render(s, "midi 145 69 100 0", 0.4, all_outputs=1)
    assert rms(x[:, 0]) > 0.01 and np.max(np.abs(x[:, 2:])) < 1e-6


def test_volume_and_transpose(lib, S):
    s = S.new()
    a = S.render(s, "midi 144 69 100 0", 0.5)
    lib.smp_synth_set_param(s, P_VOLUME, -6)
    lib.smp_synth_set_param(s, P_TRANSPOSE, 12)
    b = S.render(s, "midi 144 69 100 0", 0.5)
    assert abs(20 * np.log10(rms(seg(b, 0.2, 0.45)) / rms(seg(a, 0.2, 0.45))) + 6) < 0.1
    assert abs(peak_freq(seg(b, 0.1, 0.45)) - 880) < 2
    assert lib.smp_synth_get_param(s, P_VOLUME) == -6 and lib.smp_synth_get_param(s, P_TRANSPOSE) == 12


STATE = ("sf_mode one\nsf_preset 0 33\nsf_channel 3\nsf_channel_preset 3 0 25\nsf_channel_preset 10 128 0\n"
         "sf_output 2 3\nsf_output 16 16\nsf_used 0:0 0:33 128:0\n"
         "sf_volume -6.0 dB\nsf_voices 64\nsf_effects off\nsf_transpose +2 st\n")


def test_state_lines_roundtrip(lib, S, font):
    s1 = S.new()
    r, err = S.apply(s1, "# Samplosis 0.1.0\nplays soundfont\nroot C4\nvolume -3.0 dB\n" + STATE)
    assert r == 0, err                                  # the sampler's lines and plays are ignored
    w1 = S.write(s1)
    assert w1 == 'soundfont "%s"\n' % font + STATE
    assert lib.smp_synth_get_param(s1, P_VOICES) == 64 and lib.smp_synth_get_param(s1, P_EFFECTS) == 0
    # sf_used: those presets are used as soon as the state loads (piano, bass and the kit: samples 0, 1 and 2)
    assert lib.smp_synth_wait_loaded(s1, 5000) == 0
    assert lib.smp_sf_loaded_samples(lib.smp_synth_font(s1)) == 3
    s2 = S.new(load=False)
    r, err = S.apply(s2, w1)
    assert r == 0, err
    assert S.write(s2) == w1
    # a bad line: "line N: ..." and nothing changes
    bad = w1.splitlines()
    bad[2] = "sf_mode banana"
    r, err = S.apply(s2, "\n".join(bad))
    assert r != 0 and err.startswith("line 3: "), err
    assert S.write(s2) == w1
    for line in ("sf_bogus 1", "sf_channel 17", "sf_output 2", "sf_used 0-0", "sf_volume loud",
                 'soundfont "unclosed', "sf_channel_preset 0 0 0"):
        r, err = S.apply(s2, "sf_mode all\n" + line + "\n")
        assert r != 0 and err.startswith("line 2: "), (line, err)
        assert S.write(s2) == w1
    # a missing font: the error names the path, and nothing changes
    missing = str(font.parent / "nope" / "missing.sf2")
    r, err = S.apply(s2, 'soundfont "%s"\nsf_mode all\n' % missing)
    assert r != 0 and missing in err, err
    assert S.write(s2) == w1
    # a path too long for the player is an error, not cut short; a long line of ours is checked even indented
    r, err = S.apply(s2, 'sf_mode all\nsoundfont "%s"\n' % ("C:\\" + "x" * 1030 + ".sf2"))
    assert r != 0 and err.startswith("line 2: "), err
    r, err = S.apply(s2, "sf_mode all\n   sf_used " + "0:0 " * 400 + "\n")
    assert r != 0 and err.startswith("line 2: "), err
    assert S.write(s2) == w1
    # defaults: absent lines go back to them; no font yet writes no soundfont line
    s3 = S.new(load=False, dry=False)
    assert S.write(s3) == ("sf_mode all\nsf_preset 0 0\nsf_channel all\n"
                           "sf_volume 0.0 dB\nsf_voices 256\nsf_effects on\nsf_transpose +0 st\n")


def test_script_cfg_only(lib, S):
    """smp_synth_script_cfg (the window's one-line changes): only the configuration lines apply; the parameters
    keep their exact values (no rounding through text) and the font and sf_used stay"""
    lib.smp_synth_script_cfg.restype = ctypes.c_int
    lib.smp_synth_script_cfg.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    s = S.new()
    r, err = S.apply(s, "sf_used 0:33\nsf_output 2 3\n")
    assert r == 0, err
    lib.smp_synth_set_param(s, P_VOLUME, -3.37)
    err = ctypes.create_string_buffer(512)
    assert lib.smp_synth_script_cfg(s, b"sf_mode one\nsf_preset 0 33\nsf_output 2 3\nsf_volume 6\n"
                                    b"sf_used 0:16\nsoundfont \"nope.sf2\"\n", err, 512) == 0, err.value
    assert lib.smp_synth_get_param(s, P_VOLUME) == -3.37
    w = S.write(s)
    assert "sf_mode one\nsf_preset 0 33\n" in w and "sf_output 2 3\n" in w and "0:33" in w and "0:16" not in w
    assert lib.smp_synth_font(s)
    # a bad configuration line: nothing changes
    assert lib.smp_synth_script_cfg(s, b"sf_mode banana\n", err, 512) != 0
    assert err.value.decode().startswith("line 1: ")
    assert S.write(s) == w
    # a missing kind goes back to its default (the window always sends the whole configuration)
    assert lib.smp_synth_script_cfg(s, b"sf_mode one\nsf_preset 0 33\n", err, 512) == 0
    assert "sf_output" not in S.write(s)
    assert lib.smp_synth_get_param(s, P_VOLUME) == -3.37


def test_font_switch_while_playing(lib, S, font, tmp_path):
    """load font B while a thread renders from font A: the audio thread switches at a block, the old font is
    closed only after it let go, and nothing it held stays in use"""
    b_path = tmp_path / "b.sf2"
    write_font(b_path, base_pitch=57)                   # B's piano plays 880 Hz at key 69
    lib.smp_sf_test_grace(20)
    err = ctypes.create_string_buffer(512)
    fa = lib.smp_sf_open(str(font).encode(), err, 512)  # our own reference to A, to watch its samples
    assert fa
    try:
        s = S.new()
        stop = threading.Event()
        out = []
        bufs = [np.zeros(256, np.float32) for _ in range(32)]
        arr = (FP * 32)(*[bb.ctypes.data_as(FP) for bb in bufs])

        def player():
            k = 0
            while not stop.is_set():
                if k % 20 == 0:
                    lib.smp_synth_midi(s, bytes([0x90, 69, 100]), 3, 0)
                if k % 20 == 15:
                    lib.smp_synth_midi(s, bytes([0x80, 69, 0]), 3, 0)
                lib.smp_synth_process(s, arr, 256)
                out.append(bufs[0].copy())
                k += 1
                time.sleep(0.002)

        def blocks_later(n, limit=30):
            # waits until the player has rendered n more blocks: counted, not timed, so a busy machine (the
            # player and the below-normal loader get little CPU) can't leave the windows below without sound
            k0, t0 = len(out), time.time()
            while len(out) < k0 + n and time.time() - t0 < limit:
                time.sleep(0.01)

        th = threading.Thread(target=player)
        th.start()
        try:
            t0 = time.time()
            while lib.smp_sf_loaded_samples(fa) < 1 and time.time() - t0 < 30:
                time.sleep(0.01)
            assert lib.smp_sf_loaded_samples(fa) >= 1  # A's piano is in use
            blocks_later(60)                           # three of A's notes heard
            assert lib.smp_synth_load(s, str(b_path).encode(), err, 512) == 1, err.value
            # (the waits end as soon as the thing happens; they are long only for a busy machine, where the
            # below-normal loader that retires A's samples gets little CPU)
            t0 = time.time()
            while lib.smp_synth_collect(s) and time.time() - t0 < 30:
                time.sleep(0.01)
            assert lib.smp_synth_collect(s) == 0       # the old font was closed once the audio thread let go
            t0 = time.time()
            while lib.smp_sf_loaded_samples(fa) and time.time() - t0 < 30:
                time.sleep(0.01)
            assert lib.smp_sf_loaded_samples(fa) == 0  # and nothing of A is in use any more
            assert lib.smp_synth_wait_loaded(s, 30000) == 0   # B's piano loaded (the loader may be slow when busy)
            blocks_later(60)                           # then three of B's notes heard
        finally:
            stop.set()
            th.join()
        x = np.concatenate(out)
        assert np.all(np.isfinite(x))
        # only windows that sound all through: one whose sound is a single block (the switch silences the rest)
        # has its peak at DC after the Hann window, which made this test fail now and then
        freqs = [peak_freq(x[i:i + 2560]) for i in range(0, len(x) - 2560, 2560)
                 if min(rms(x[j:j + 256]) for j in range(i, i + 2560, 256)) > 0.01]
        assert any(abs(f - 440) < 5 for f in freqs) and any(abs(f - 880) < 5 for f in freqs)
        assert abs(freqs[-1] - 880) < 5
        # freeing the synth with notes sounding lets go of everything it held
        lib.smp_synth_midi(s, bytes([0x90, 69, 100]), 3, 0)
        lib.smp_synth_process(s, arr, 256)
        S.synths.remove(s)
        lib.smp_synth_free(s)
    finally:
        lib.smp_sf_close(fa)
        lib.smp_sf_test_grace(0)


# ---- reverb and chorus (sffx.c) ----
BURST = "midi 192 33 0\nmidi 176 91 {rev} 0\nmidi 176 93 {cho} 0\nmidi 144 69 127 100\nmidi 128 69 0 580\n"


def windows(x, a, b, step=0.1):
    return [rms(seg(x, t, t + step)) for t in np.arange(a, b - 1e-9, step)]


def test_reverb_send(lib, S):
    s = S.new(dry=False)
    ev = BURST.format(rev=127, cho=0)
    x = S.render(s, ev, 1.0)
    top = np.max(np.abs(x[:int(0.02 * OUT)]))           # the burst is over by 0.02 s: after it is all tail
    w = [np.max(np.abs(seg(x, t, t + 0.1))) for t in np.arange(0.05, 0.55, 0.1)]
    assert min(w) > top * 10 ** (-60 / 20), (top, w)    # above -60 dB (re the burst) for 0.5 s
    assert abs(x[:, 0] - x[:, 1]).max() > 1e-5          # the reverb is stereo
    x = S.render(s, BURST.format(rev=0, cho=0), 1.0)
    assert np.max(np.abs(x[int(0.05 * OUT):])) < 1e-6
    lib.smp_synth_set_param(s, P_EFFECTS, 0)
    x = S.render(s, ev, 1.0)
    assert np.max(np.abs(x[int(0.05 * OUT):])) < 1e-6


def drive(lib, S):
    """a synth played straight through smp_synth_process (no render-start clearing): returns it and a block runner"""
    s = S.new(dry=False)
    assert lib.smp_synth_wait_loaded(s, 5000) == 0
    bufs = [np.zeros(256, np.float32) for _ in range(32)]
    arr = (FP * 32)(*[bb.ctypes.data_as(FP) for bb in bufs])

    def run(blocks):
        peak = 0.0
        for _ in range(blocks):
            lib.smp_synth_process(s, arr, 256)
            peak = max(peak, float(np.max(np.abs(bufs[0]))), float(np.max(np.abs(bufs[1]))))
        return peak

    def midi(*b):
        lib.smp_synth_midi(s, bytes(b), len(b), 0)
    run.bufs = bufs
    return s, run, midi


def reverb_tail(lib, S):
    """a short loud note with a full reverb send, then all sound off: only the tail is left"""
    s, run, midi = drive(lib, S)
    midi(0xB0, 91, 127)
    midi(0x90, 69, 127)
    for _ in range(300):                                  # the preset may still be loading: wait for the note
        if run(1) > 0.01:
            break
        time.sleep(0.01)
    else:
        assert False, 'the note never sounded'
    run(3)
    midi(0xB0, 120, 0)
    run(2)
    assert run(2) > 1e-4                                  # the tail is there
    return s, run, midi


def test_effects_off_clears_tail(lib, S):
    s, run, midi = reverb_tail(lib, S)
    lib.smp_synth_set_param(s, P_EFFECTS, 0)
    assert run(1) == 0.0                                  # off: nothing is added
    lib.smp_synth_set_param(s, P_EFFECTS, 1)
    assert run(8) < 1e-9                                  # on again: the old tail does not come back


def test_reset_clears_tail(lib, S):
    """a reset (REAPER's stop, seek and loop flush the same way) fades the tails out over 10 ms, then clears
    them: no click, and nothing comes back"""
    lib.smp_synth_flush.argtypes = [ctypes.c_void_p, ctypes.c_int]
    s, run, midi = reverb_tail(lib, S)
    bufs = run.bufs
    run(1)
    before = float(np.max(np.abs(bufs[0])))
    assert before > 1e-4
    lib.smp_synth_reset(s)
    out = []
    for _ in range(4):
        run(1)
        out.append(bufs[0].copy())
    y = np.concatenate(out)
    fade = int(0.010 * OUT)                                    # 480 frames
    assert np.max(np.abs(y[:16])) > before * 0.2               # it doesn't stop dead
    assert np.max(np.abs(np.diff(y[:fade]))) < before          # no step
    assert np.max(np.abs(y[fade - 48:fade])) < before * 0.15   # nearly gone at the end of the fade
    assert np.max(np.abs(y[fade:])) == 0                       # then cleared
    assert run(8) < 1e-9
    # the plugin's flush (REAPER's reset) fades the same way
    s, run, midi = reverb_tail(lib, S)
    lib.smp_synth_flush(s, 0)
    run(1)
    assert float(np.max(np.abs(run.bufs[0]))) > 0
    run(1)
    assert run(8) < 1e-9
    # and with nothing sounding, the clear is at once
    lib.smp_synth_flush(s, 0)
    assert run(1) == 0.0


def test_effects_follow_volume(lib, S):
    s = S.new(dry=False)
    ev = BURST.format(rev=127, cho=0)
    a = S.render(s, ev, 0.6)
    lib.smp_synth_set_param(s, P_VOLUME, -12)
    b = S.render(s, ev, 0.6)
    ra, rb = rms(seg(a, 0.2, 0.5)), rms(seg(b, 0.2, 0.5))
    assert abs(20 * np.log10(rb / ra) + 12) < 0.1


def test_chorus_send(lib, S):
    s = S.new(dry=False)
    hold = "midi 192 33 0\nmidi 176 91 0 0\nmidi 176 93 {cho} 0\nmidi 144 69 127 100\n"
    x = S.render(s, hold.format(cho=127), 1.2)
    a, b = seg(x, 0.3, 1.1, 0), seg(x, 0.3, 1.1, 1)
    side = rms(a - b)
    assert side > rms(a) * 10 ** (-30 / 20), (side, rms(a))           # the image is wide
    x = S.render(s, hold.format(cho=0), 1.2)
    assert np.max(np.abs(x[:, 0] - x[:, 1])) < 1e-6                   # mono without the send
    # the delay swings 8 ms, so the side signal rises and falls (flat when the tap delays do not move)
    hold = "midi 192 16 0\nmidi 176 91 0 0\nmidi 176 93 127 0\nmidi 144 69 127 100\n"
    x = S.render(s, hold, 3.5)
    side = [rms(seg(x, t, t + 0.05, 0) - seg(x, t, t + 0.05, 1)) for t in np.arange(0.3, 3.4, 0.05)]
    assert max(side) > 1.5 * min(side), (min(side), max(side))


def test_chorus_taps_120_degrees(lib):
    """a ramp through the chorus alone: each tap outputs ramp - delay * slope, so L - R follows the taps' delays.
    Three sines 120 degrees apart (depth 8 ms) give L - R = gain * slope * depth * sqrt(3) * sin(t - 30 degrees);
    taps 30 degrees apart would give less than a third of that."""
    vp, fp = ctypes.c_void_p, FP
    lib.smp_sffx_new.restype = vp
    lib.smp_sffx_new.argtypes = [ctypes.c_double]
    lib.smp_sffx_free.argtypes = [vp]
    lib.smp_sffx_process.argtypes = [vp, fp, fp, fp, fp, ctypes.c_int]
    fx = lib.smp_sffx_new(48000.0)
    n = 48000 * 4
    slope = 1e-3
    ramp = (np.arange(n) * slope).astype(np.float32)
    l, r = np.zeros(n, np.float32), np.zeros(n, np.float32)
    lib.smp_sffx_process(fx, None, ramp.ctypes.data_as(FP), l.ctypes.data_as(FP), r.ctypes.data_as(FP), n)
    lib.smp_sffx_free(fx)
    d = (l - r)[48000 // 5:]                                # past the line's first fill
    want = (2.0 / 3.0) * slope * (0.008 * 48000) * np.sqrt(3)
    assert abs(np.max(d) - want) < 0.05 * want and abs(np.min(d) + want) < 0.05 * want, (np.max(d), np.min(d), want)


def test_fx_cpu(lib):
    vp, fp, c_dbl, c_int = ctypes.c_void_p, FP, ctypes.c_double, ctypes.c_int
    lib.smp_sffx_new.restype = vp
    lib.smp_sffx_new.argtypes = [c_dbl]
    lib.smp_sffx_free.argtypes = [vp]
    lib.smp_sffx_process.argtypes = [vp, fp, fp, fp, fp, c_int]
    fx = lib.smp_sffx_new(48000.0)
    assert fx
    n = 48000
    rng = np.random.default_rng(1)
    rev = (rng.standard_normal(n) * 0.1).astype(np.float32)
    cho = (rng.standard_normal(n) * 0.1).astype(np.float32)
    l, r = np.zeros(n, np.float32), np.zeros(n, np.float32)
    p = lambda a: a.ctypes.data_as(FP)
    best = 1e9
    for _ in range(5):
        t = time.perf_counter()
        lib.smp_sffx_process(fx, p(rev), p(cho), p(l), p(r), n)
        best = min(best, time.perf_counter() - t)
    lib.smp_sffx_free(fx)
    assert np.all(np.isfinite(l)) and np.max(np.abs(l)) > 0
    assert best < 0.005, best
    for rate in (22050.0, 96000.0, 192000.0):               # every supported rate builds and runs
        fx = lib.smp_sffx_new(rate)
        assert fx
        lib.smp_sffx_process(fx, p(rev), p(cho), p(l), p(r), 4096)
        lib.smp_sffx_free(fx)


def test_no_stuck_notes_with_effects(lib, S):
    s = S.new(dry=False)
    hold = "midi 176 91 40 0\nmidi 144 69 127 0\n"
    tail = S.render(s, "midi 176 91 40 0\nmidi 144 69 127 0\nmidi 176 120 0 14400", 1.0)   # all sound off at 0.3 s
    after = np.max(np.abs(tail[int(0.6 * OUT):]))
    held = S.render(s, hold, 1.0)
    assert after < 0.1 * np.max(np.abs(held[int(0.6 * OUT):]))      # the voice is gone, what is left is the tail
    x = S.render(s, hold + "reset 14400", 1.0)                       # a reset clears the tails as well (the voices' 5 ms fade still feeds a little)
    assert np.max(np.abs(x[int(0.4 * OUT):])) < 0.05 * np.max(np.abs(held[int(0.6 * OUT):]))


# ---- streaming (stage 3): samples past their head stream from the font file ----

STREAM_DIR = r"E:\samplepacks\_build\samplosis_stream_test"   # big fonts go here, not to C: (nearly full)


def busy(secs, seed, freq=330.0):
    """A tone with noise on it, so a frame out of place shows."""
    rng = np.random.default_rng(seed)
    n = int(secs * SR)
    t = np.arange(n) / SR
    x = 9000 * np.sin(2 * np.pi * freq * t) + 2500 * rng.standard_normal(n)
    return np.clip(x, -32000, 32000).astype(np.int16)


def render_file(lib, path, events, secs, tmp_path, whole=False, block=256, pause=False, realtime=False, params=(),
                before=None):
    """Renders events on a new synth with path loaded: (stereo out, underruns). Offline unless realtime (then the
    presets are used and loaded first, and the render runs at real-time pace, never waiting). whole: the samples
    are held whole, as in stage 2 (the reference). params: (id, value) pairs. before(s): called just before."""
    lib.smp_sf_test_whole(1 if whole else 0)
    s = lib.smp_synth_new(float(OUT), 256)
    try:
        lib.smp_synth_set_param(s, P_EFFECTS, 0)
        for pid, val in params:
            lib.smp_synth_set_param(s, pid, val)
        err = ctypes.create_string_buffer(512)
        assert lib.smp_synth_load(s, str(path).encode(), err, 512) == 1, err.value
        if pause:
            lib.smp_synth_test_stream_pause(s, 1)
        if realtime:
            warm = tmp_path / ("w%d.wav" % next(_count))
            prog = chr(10).join(l for l in events.split(chr(10)) if l.split()[1:2] and 192 <= int(l.split()[1]) < 208)
            assert lib.smp_synth_render_test(s, prog.encode(), 256, 256, str(warm).encode(), 0) == 0
            assert lib.smp_synth_wait_loaded(s, 20000) == 0
            lib.smp_synth_test_offline(s, 0)
        if before:
            before(s)
        out = tmp_path / ("s%d.wav" % next(_count))
        assert lib.smp_synth_render_test(s, events.encode(), int(secs * OUT), block, str(out).encode(), 0) == 0
        x, _ = sf.read(str(out), dtype="float32", always_2d=True)
        return x, lib.smp_synth_underruns(s)
    finally:
        lib.smp_synth_free(s)
        lib.smp_sf_test_whole(0)


def err_db(ref, x):
    d = np.sqrt(np.mean((ref.astype(np.float64) - x) ** 2))
    return -999.0 if d == 0 else 20 * np.log10(d / np.sqrt(np.mean(ref.astype(np.float64) ** 2)))


def test_long_sample_streams(lib, tmp_path):
    """A 30 s sample keeps only its first quarter second in memory, and a 30 s note of it matches the same note
    with the sample held whole within -90 dB (offline renders wait for the streamer, so nothing underruns)."""
    data = busy(30.0, 1)
    path = tmp_path / "long.sf2"
    write_sf2(str(path), [preset("Long", 0, 0, [zone(0)])],
              [dict(name="long", data=data, rate=SR, pitch=60, loop=None)])
    err = ctypes.create_string_buffer(512)
    f = lib.smp_sf_open(str(path).encode(), err, 512)
    assert f, err.value
    try:
        lib.smp_sf_use(f, 0, 1)
        assert lib.smp_sf_wait_idle(f, 5000) == 0
        held = lib.smp_sf_loaded_bytes(f)
        assert 0.25 * SR * 2 <= held < 0.25 * SR * 2 + 4096, held      # 22 KB, not 2.6 MB
        lib.smp_sf_use(f, 0, -1)
    finally:
        lib.smp_sf_close(f)
    ev = "midi 144 60 100 0\nmidi 128 60 0 %d" % int(29.5 * OUT)
    ref, _ = render_file(lib, path, ev, 30.6, tmp_path, whole=True)
    x, under = render_file(lib, path, ev, 30.6, tmp_path)
    assert under == 0
    assert np.sqrt(np.mean(x[int(28 * OUT):int(29 * OUT)] ** 2)) > 0.05      # the end of the sample plays
    assert err_db(ref, x) < -90, err_db(ref, x)


def test_streamed_loop(lib, tmp_path):
    """Loops past the head stream with wraparound: a 9 s loop played for 30 s (and an octave up, which wraps twice as
    often), and a loop-until-release let go after a few laps, which goes on to the sample's end. Each matches the
    same notes with the sample held whole within -90 dB."""
    data = busy(12.0, 2, 220.0)
    path = tmp_path / "loop.sf2"
    write_sf2(str(path), [preset("Loop", 0, 0, [zone(0, gens={MODES: 1})]),
                          preset("Release", 0, 1, [zone(0, gens={MODES: 3, RELEASE_VOL: 2400})])],
              [dict(name="loop", data=data, rate=SR, pitch=60, loop=(2 * SR, 11 * SR))])
    for ev, secs in (("midi 144 60 100 0", 30.0),
                     ("midi 144 72 100 0", 20.0),
                     ("midi 192 1 0\nmidi 144 72 100 0\nmidi 128 72 0 %d" % int(14.3 * OUT), 22.0)):
        ref, _ = render_file(lib, path, ev, secs, tmp_path, whole=True)
        x, under = render_file(lib, path, ev, secs, tmp_path)
        assert under == 0, ev
        assert np.sqrt(np.mean(ref[-OUT:] ** 2)) > 0.01 or "128" in ev, ev   # still sounding at the end
        assert err_db(ref, x) < -90, (ev, err_db(ref, x))


def test_stream_underrun_fades(lib, tmp_path):
    """With the streamer stopped the note plays its head, then fades out (no click), counts underruns and goes on
    in silence (the render finishes: nothing stalls)."""
    data = sine(330, 3.0)
    path = tmp_path / "u.sf2"
    write_sf2(str(path), [preset("U", 0, 0, [zone(0)])], [dict(name="u", data=data, rate=SR, pitch=60, loop=None)])
    x, under = render_file(lib, path, "midi 144 60 100 0", 1.0, tmp_path, pause=True)
    m = x[:, 0].astype(np.float64)
    head_end = int(0.25 * OUT)
    assert np.max(np.abs(m[head_end - 2000:head_end - 400])) > 0.1           # the head plays
    assert np.max(np.abs(m[head_end + 400:])) < 1e-6                         # then silence
    assert under > 0
    assert np.max(np.abs(np.diff(m))) < 0.06                                 # faded: no step anywhere


def test_streaming_many_voices_no_underrun(lib, tmp_path):
    """64 voices of distinct 20 s samples from one font file, played in real time for 5 s: no ring ever runs dry."""
    os.makedirs(STREAM_DIR, exist_ok=True)
    path = os.path.join(STREAM_DIR, "many_%d.sf2" % os.getpid())
    try:
        samples = [dict(name="m%d" % i, data=busy(20.0, 100 + i, 110.0 + 7 * i), rate=SR, pitch=30 + i, loop=None)
                   for i in range(64)]
        write_sf2(path, [preset("Many", 0, 0, [zone(i, keys=(30 + i, 30 + i)) for i in range(64)])], samples)
        del samples
        s = lib.smp_synth_new(float(OUT), 256)
        try:
            lib.smp_synth_set_param(s, P_EFFECTS, 0)
            err = ctypes.create_string_buffer(512)
            assert lib.smp_synth_load(s, path.encode(), err, 512) == 1, err.value
            warm = tmp_path / "warm.wav"
            assert lib.smp_synth_render_test(s, b"", 256, 256, str(warm).encode(), 0) == 0   # uses the preset
            assert lib.smp_synth_wait_loaded(s, 20000) == 0
            ev = "\n".join("midi 144 %d 100 0" % (30 + i) for i in range(64))
            out = tmp_path / "many.wav"
            lib.smp_synth_test_offline(s, 0)                                 # real time: never waits
            c0, t0 = time.process_time(), time.perf_counter()
            assert lib.smp_synth_render_test(s, ev.encode(), 5 * OUT, 256, str(out).encode(), 0) == 0
            cpu, wall = time.process_time() - c0, time.perf_counter() - t0
            peak = ctypes.c_int()
            lib.smp_synth_voices(s, ctypes.byref(peak))
            under = lib.smp_synth_underruns(s)
            print("\n64 streaming voices, 5 s in real time: %.2f s wall, %.2f s CPU (%.1f %% of one core), underruns %d"
                  % (wall, cpu, 100 * cpu / wall, under))
            assert peak.value == 64
            assert under == 0
            x, _ = sf.read(str(out), dtype="float32")
            assert np.sqrt(np.mean(x[-OUT:] ** 2)) > 0.05                    # still playing past every head
        finally:
            lib.smp_synth_free(s)
    finally:
        if os.path.exists(path):
            os.remove(path)
        try:
            os.rmdir(STREAM_DIR)
        except OSError:
            pass


COARSE = 51


def test_mode3_release_realtime(lib, tmp_path):
    """Loop-until-release in real time: at the release the voice goes on past its loop end from the post-loop head
    while the streamer catches up, so nothing underruns, whatever the phase of the release. Channel 1: a short loop
    past the head; channel 2: a loop inside the head with a long tail. Each matches the samples held whole."""
    a, b = busy(2.5, 3, 260.0), busy(2.5, 4, 190.0)
    path = tmp_path / "m3.sf2"
    write_sf2(str(path), [preset("Short", 0, 0, [zone(0, gens={MODES: 3, RELEASE_VOL: 1200})]),
                          preset("InHead", 0, 1, [zone(1, gens={MODES: 3, RELEASE_VOL: 1200})])],
              [dict(name="short", data=a, rate=SR, pitch=60, loop=(SR // 2, SR // 2 + 300)),
               dict(name="inhead", data=b, rate=SR, pitch=60, loop=(SR // 20, SR // 10))])
    ev = ["midi 193 1 0"]
    for k, key in enumerate((60, 62, 65, 67, 72)):             # released at different phases of the loop
        on = int(k * 0.8 * OUT)
        off = on + int((0.7 + 0.0021 * k) * OUT) + 37 * k
        ev += ["midi 144 %d 100 %d" % (key, on), "midi 128 %d 0 %d" % (key, off),
               "midi 145 %d 100 %d" % (key, on + 11), "midi 129 %d 0 %d" % (key, off + 101)]
    ev = "\n".join(ev)
    secs = 5 * 0.8 + 1.8
    ref, _ = render_file(lib, path, ev, secs, tmp_path, whole=True)
    x, under = render_file(lib, path, ev, secs, tmp_path, realtime=True)
    assert under == 0
    assert np.sqrt(np.mean(ref[int(0.8 * OUT):int(1.4 * OUT)] ** 2)) > 0.05   # the tails after the releases sound
    assert err_db(ref, x) < -90, err_db(ref, x)


def test_stream_steal_reuse(lib, tmp_path):
    """Stealing mid-stream with 16 voices: new notes take over slots whose voices were streaming, and the same keys
    come back into reused slots. Offline it matches the samples held whole exactly; in real time nothing underruns."""
    samples = [dict(name="v%d" % i, data=busy(6.0, 200 + i, 150.0 + 11 * i), rate=SR, pitch=40 + i, loop=None)
               for i in range(24)]
    path = tmp_path / "steal.sf2"
    write_sf2(str(path), [preset("Steal", 0, 0, [zone(i, keys=(40 + i, 40 + i)) for i in range(24)])], samples)
    ev = []
    for k, start in enumerate((0, 8, 16, 0, 8)):
        at = int(k * 0.4 * OUT)
        ev += ["midi 144 %d 100 %d" % (40 + start + j, at + 3 * j) for j in range(8)]
    ev = "\n".join(ev)
    params = [(P_VOICES, 16)]
    ref, _ = render_file(lib, path, ev, 3.0, tmp_path, whole=True, params=params)
    x, under = render_file(lib, path, ev, 3.0, tmp_path, params=params)
    assert under == 0
    assert err_db(ref, x) < -90, err_db(ref, x)
    x, under = render_file(lib, path, ev, 3.0, tmp_path, params=params, realtime=True)
    assert under == 0
    assert err_db(ref, x) < -90, err_db(ref, x)


def test_stream_extreme_pitch_offline(lib, tmp_path):
    """An offline render stays exact at a pitch far past 32 times the speed: its steps shrink with the pitch."""
    path = tmp_path / "hi.sf2"
    write_sf2(str(path), [preset("Hi", 0, 0, [zone(0, gens={COARSE: 60})])],
              [dict(name="hi", data=busy(30.0, 5, 50.0), rate=SR, pitch=60, loop=None)])
    ev = "midi 144 84 100 0"                                   # 84 semitones up: 128 times the speed
    ref, _ = render_file(lib, path, ev, 0.4, tmp_path, whole=True)
    x, under = render_file(lib, path, ev, 0.4, tmp_path)
    assert under == 0
    assert np.sqrt(np.mean(ref[int(0.1 * OUT):int(0.2 * OUT)] ** 2)) > 0.01   # past the head
    assert err_db(ref, x) < -90, err_db(ref, x)


def test_stream_read_failure(lib, tmp_path):
    """The font file is cut short while it plays: the reads that fail are not played as good silence. The voice
    fades out where the data ends, the underruns are counted, and the render finishes."""
    data = busy(6.0, 6, 300.0)
    path = tmp_path / "cut.sf2"
    write_sf2(str(path), [preset("Cut", 0, 0, [zone(0)])], [dict(name="cut", data=data, rate=SR, pitch=60, loop=None)])
    ev = "midi 144 60 100 0"
    ref, _ = render_file(lib, path, ev, 4.0, tmp_path, whole=True)
    cut_at = open(path, "rb").read().index(data[:64].tobytes()) + 2 * 2 * SR   # 2 s into the sample

    def cut(s):
        with open(path, "r+b") as fh:
            fh.truncate(cut_at)

    x, under = render_file(lib, path, ev, 4.0, tmp_path, realtime=True, before=cut)
    assert under > 0
    assert err_db(ref[:int(1.8 * OUT)], x[:int(1.8 * OUT)]) < -90           # everything read plays as it should
    assert np.max(np.abs(x[int(2.1 * OUT):])) < 1e-6                         # then silence, not stale data


def _blocks(lib, s, n, bufs_arr):
    for _ in range(n):
        lib.smp_synth_process(s, bufs_arr, 256)


def test_stream_font_switch_lifetime(lib, tmp_path):
    """A font switch, an unload and a free while long samples are streaming (and once with the streamer paused): the
    old font is closed only after the streamer has let go, and every hold on its samples goes."""
    a = tmp_path / "a.sf2"
    write_sf2(str(a), [preset("A", 0, 0, [zone(i, keys=(60 + i, 60 + i)) for i in range(3)])],
              [dict(name="a%d" % i, data=busy(10.0, 300 + i), rate=SR, pitch=60 + i, loop=None) for i in range(3)])
    bpath = tmp_path / "b.sf2"
    write_font(bpath)
    bufs = [np.zeros(256, np.float32) for _ in range(32)]
    arr = (FP * 32)(*[bb.ctypes.data_as(FP) for bb in bufs])
    err = ctypes.create_string_buffer(512)
    lib.smp_sf_test_grace(20)
    fa = lib.smp_sf_open(str(a).encode(), err, 512)
    assert fa
    try:
        for how in ("switch", "switch paused", "unload", "free"):
            s = lib.smp_synth_new(float(OUT), 256)
            try:
                assert lib.smp_synth_load(s, str(a).encode(), err, 512) == 1
                lib.smp_synth_process(s, arr, 256)
                assert lib.smp_synth_wait_loaded(s, 5000) == 0
                for k in range(3):
                    lib.smp_synth_midi(s, bytes([0x90, 60 + k, 100]), 3, 0)
                _blocks(lib, s, 80, arr)                          # 0.43 s: well past the heads, streaming
                assert lib.smp_synth_underruns(s) == 0
                if how == "switch paused":
                    lib.smp_synth_test_stream_pause(s, 1)
                if how == "free":
                    lib.smp_synth_free(s)
                    s = None
                else:
                    if how == "unload":
                        assert lib.smp_synth_unload(s) == 1
                    else:
                        assert lib.smp_synth_load(s, str(bpath).encode(), err, 512) == 1
                    t0 = time.time()
                    while True:
                        _blocks(lib, s, 1, arr)
                        if lib.smp_synth_collect(s) == 0 or time.time() - t0 > 3:
                            break
                    assert lib.smp_synth_collect(s) == 0, how
                t0 = time.time()
                while lib.smp_sf_loaded_samples(fa) and time.time() - t0 < 3:
                    time.sleep(0.01)
                assert lib.smp_sf_loaded_samples(fa) == 0, how      # nothing of A is held any more
            finally:
                if s:
                    lib.smp_synth_free(s)
    finally:
        lib.smp_sf_close(fa)
        lib.smp_sf_test_grace(0)                                  # back to the default for later tests
        lib.smp_sf_test_grace(0)


def test_offline_waits_past_ten_seconds(lib, S):
    """An offline render waits for a note's sample with no fixed cap while the loader isn't stalled: here the loader
    is held back 11 s (longer than the old 10 s cap) and the note still starts on its frame."""
    s = S.new()
    lib.smp_sf_test_pause(1)
    released = threading.Timer(11.0, lambda: lib.smp_sf_test_pause(0))
    released.start()
    try:
        t = time.perf_counter()
        x = S.render(s, "midi 192 0 0\nmidi 144 69 100 4800", 0.5)
        waited = time.perf_counter() - t
    finally:
        released.cancel()
        lib.smp_sf_test_pause(0)
    assert waited >= 10.5, waited
    assert onset(x) is not None and abs(onset(x) - 4800) < 64, onset(x)
    assert abs(peak_freq(x[6000:20000, 0]) - 440) < 5


def test_stream_collect_never_settled(lib, tmp_path):
    """A font switch while long samples stream and the streamer never settles (as on a cold disk under heavy
    streaming, where a sync that waits for every ring to be topped up may never return): collect still closes the old
    font within a few calls, since it waits only for the ended voices' records to let go, and nothing of it stays
    held."""
    a = tmp_path / "a.sf2"
    write_sf2(str(a), [preset("A", 0, 0, [zone(i, keys=(60 + i, 60 + i)) for i in range(3)])],
              [dict(name="a%d" % i, data=busy(10.0, 300 + i), rate=SR, pitch=60 + i, loop=None) for i in range(3)])
    bpath = tmp_path / "b.sf2"
    write_font(bpath)
    bufs = [np.zeros(256, np.float32) for _ in range(32)]
    arr = (FP * 32)(*[bb.ctypes.data_as(FP) for bb in bufs])
    err = ctypes.create_string_buffer(512)
    lib.smp_sf_test_grace(20)
    fa = lib.smp_sf_open(str(a).encode(), err, 512)
    assert fa
    s = lib.smp_synth_new(float(OUT), 256)
    try:
        assert lib.smp_synth_load(s, str(a).encode(), err, 512) == 1
        lib.smp_synth_process(s, arr, 256)
        assert lib.smp_synth_wait_loaded(s, 5000) == 0
        for k in range(3):
            lib.smp_synth_midi(s, bytes([0x90, 60 + k, 100]), 3, 0)
        _blocks(lib, s, 80, arr)                                  # streaming
        lib.smp_sfs_test_unsettled(1)
        assert lib.smp_synth_load(s, str(bpath).encode(), err, 512) == 1
        for k in range(3):                                        # the new font plays and streams meanwhile
            lib.smp_synth_midi(s, bytes([0x90, 60 + k, 100]), 3, 0)
        t0 = time.time()
        while True:
            _blocks(lib, s, 1, arr)
            if lib.smp_synth_collect(s) == 0 or time.time() - t0 > 3:
                break
        assert lib.smp_synth_collect(s) == 0
        assert time.time() - t0 < 3
        t0 = time.time()
        while lib.smp_sf_loaded_samples(fa) and time.time() - t0 < 3:
            time.sleep(0.01)
        assert lib.smp_sf_loaded_samples(fa) == 0                 # nothing of A is held any more
    finally:
        lib.smp_sfs_test_unsettled(0)
        lib.smp_synth_free(s)
        lib.smp_sf_close(fa)
        lib.smp_sf_test_grace(0)                                  # back to the default for later tests
