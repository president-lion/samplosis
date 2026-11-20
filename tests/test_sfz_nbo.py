"""SFZ, 2026-11-20, from playing Bandshed's No Budget Band and No Budget Orchestra against sfizz 1.2.3: an SFZ has
no send to the shared reverb until CC91 asks; gain_ccN may raise a voice above unity; a keyswitch range silences
its keys only in a preset that switches on it (sw_last); controller crossfades (xfin_loccN and the rest), which
follow the controller while a note is held."""
import ctypes

import numpy as np
import soundfile as sf

from test_sfz import OUT, RATE, Player, db, ignored, off, on, opened, rms, sine, write_sfz, write_wav


class FxPlayer(Player):
    """a Player with the reverb and chorus left as they start (Player turns them off)"""
    def __init__(self, lib, tmp_path, path):
        self.lib, self.tmp = lib, tmp_path
        err = ctypes.create_string_buffer(512)
        self.s = lib.smp_synth_new(float(OUT), 256)
        assert lib.smp_synth_load(self.s, path.encode(), err, 512) == 1, err.value


def cc(n, v, t):
    return "midi 176 %d %d %d" % (n, v, int(round(t * OUT)))


def test_sfz_no_reverb_until_cc91(lib, tmp_path):
    """A note with a short release is silent 0.3 s after its note-off: no reverb tail from GM's default send of 40.
    CC91 sent (or set_cc91) still sends it."""
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0))
    plain = write_sfz(tmp_path / "p.sfz", "<region> sample=a.wav key=60 pitch_keycenter=60 ampeg_release=0.02\n")
    p = FxPlayer(lib, tmp_path, plain)
    try:
        dry = p.render([on(60, 100, 0), off(60, 0.2)], 1.0)
        wet = p.render([cc(91, 127, 0), on(60, 100, 0.01), off(60, 0.2)], 1.0)
    finally:
        p.free()
    assert rms(dry[:int(0.15 * OUT)]) > 0.05
    assert rms(dry[int(0.5 * OUT):]) < 1e-6
    assert rms(wet[int(0.5 * OUT):]) > 1e-4
    sent = write_sfz(tmp_path / "s.sfz", "<control> set_cc91=100\n"
                     "<region> sample=a.wav key=60 pitch_keycenter=60 ampeg_release=0.02\n")
    p = FxPlayer(lib, tmp_path, sent)
    try:
        y = p.render([on(60, 100, 0), off(60, 0.2)], 1.0)
    finally:
        p.free()
    assert rms(y[int(0.5 * OUT):]) > 1e-4


def test_sfz_gain_cc_boosts(lib, tmp_path):
    """gain_cc25=12 is +12 dB at CC25 127 (SF2's attenuation stops at 0; SFZ's gain doesn't), +6 dB at 64."""
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0, amp=0.05))
    base = write_sfz(tmp_path / "b.sfz", "<region> sample=a.wav key=60 pitch_keycenter=60 amp_veltrack=0\n")
    boost = write_sfz(tmp_path / "g.sfz", "<region> sample=a.wav key=60 pitch_keycenter=60 amp_veltrack=0 gain_cc25=12\n")
    out = {}
    for name, path, val in (("base", base, 0), ("full", boost, 127), ("half", boost, 64)):
        p = Player(lib, tmp_path, path)
        try:
            out[name] = rms(p.render([cc(25, val, 0), on(60, 100, 0.01)], 0.4)[int(0.1 * OUT):])
        finally:
            p.free()
    assert abs(db(out["full"], out["base"]) - 12) < 0.3
    assert abs(db(out["half"], out["base"]) - 12 * 64 / 127) < 0.5


def test_sfz_keyswitch_range_needs_sw_last(lib, tmp_path):
    """A range with nothing switching on it (No Budget Band's harmonica gives its release group sw_lokey=0
    sw_hikey=127) leaves the keys playing, as sfizz does; in a preset with an sw_last, the range's keys stay
    keyswitches and never sound."""
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0))
    write_wav(tmp_path / "b.wav", sine(RATE, 660.0))
    loose = write_sfz(tmp_path / "l.sfz", """
<region> sample=a.wav lokey=48 hikey=72 pitch_keycenter=60
<region> sample=b.wav lokey=48 hikey=72 pitch_keycenter=60 trigger=release sw_lokey=0 sw_hikey=127
""")
    f = opened(lib, loose)
    try:
        assert ignored(lib, f)[0] == 0
    finally:
        lib.smp_sf_close(f)
    p = Player(lib, tmp_path, loose)
    try:
        y = p.render([on(60, 100, 0), off(60, 0.3)], 0.6)
    finally:
        p.free()
    assert rms(y[int(0.05 * OUT):int(0.25 * OUT)]) > 0.05      # the attack region plays
    assert rms(y[int(0.32 * OUT):int(0.45 * OUT)]) > 0.05      # and its release
    switched = write_sfz(tmp_path / "s.sfz", """
<region> sample=a.wav lokey=48 hikey=72 pitch_keycenter=60 sw_lokey=36 sw_hikey=50 sw_last=36 sw_default=36
""")
    p = Player(lib, tmp_path, switched)
    try:
        key_in_range = p.render([on(49, 100, 0)], 0.3)
        played = p.render([on(60, 100, 0)], 0.3)
    finally:
        p.free()
    assert rms(key_in_range) < 1e-6
    assert rms(played[int(0.05 * OUT):]) > 0.05


def test_sfz_controller_crossfades(lib, tmp_path):
    """xfin_locc1=0 xfin_hicc1=127: silent at CC1 0, full at 127, sqrt(64/127) (power) at 64, and a held note
    follows CC1; xfout_ fades the other way; xf_cccurve=gain is linear. A soft-pedal layer (xfin_locc67=64
    xfin_hicc67=127, as Bandshed's bright_rick_full.sfz) stays silent until the pedal goes down."""
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0))

    def level(extra, events, secs=0.4, frm=0.1):
        path = write_sfz(tmp_path / "x.sfz", "<region> sample=a.wav key=60 pitch_keycenter=60 amp_veltrack=0 %s\n" % extra)
        f = opened(lib, path)
        try:
            assert ignored(lib, f)[0] == 0
        finally:
            lib.smp_sf_close(f)
        p = Player(lib, tmp_path, path)
        try:
            y = p.render(events, secs)
        finally:
            p.free()
        return y

    full = rms(level("", [on(60, 100, 0.01)])[int(0.1 * OUT):])
    fin = "xfin_locc1=0 xfin_hicc1=127"
    assert rms(level(fin, [cc(1, 0, 0), on(60, 100, 0.01)])[int(0.1 * OUT):]) < 1e-6
    assert abs(db(rms(level(fin, [cc(1, 127, 0), on(60, 100, 0.01)])[int(0.1 * OUT):]), full)) < 0.2
    half = rms(level(fin, [cc(1, 64, 0), on(60, 100, 0.01)])[int(0.1 * OUT):])
    assert abs(db(half, full) - 10 * np.log10(64 / 127)) < 0.3
    lin = rms(level(fin + " xf_cccurve=gain", [cc(1, 64, 0), on(60, 100, 0.01)])[int(0.1 * OUT):])
    assert abs(db(lin, full) - 20 * np.log10(64 / 127)) < 0.3
    out = rms(level("xfout_locc1=0 xfout_hicc1=127", [cc(1, 127, 0), on(60, 100, 0.01)])[int(0.1 * OUT):])
    assert out < 1e-6
    held = level(fin, [cc(1, 0, 0), on(60, 100, 0.01), cc(1, 127, 0.3)], secs=0.6)
    assert rms(held[int(0.1 * OUT):int(0.28 * OUT)]) < 1e-6
    assert abs(db(rms(held[int(0.4 * OUT):]), full)) < 0.5
    pedal = "xfin_locc67=64 xfin_hicc67=127"
    assert rms(level(pedal, [on(60, 100, 0.01)])[int(0.1 * OUT):]) < 1e-6
    assert abs(db(rms(level(pedal, [cc(67, 127, 0), on(60, 100, 0.01)])[int(0.1 * OUT):]), full)) < 0.2
