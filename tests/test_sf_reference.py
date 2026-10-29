"""The SoundFont player against FluidSynth (Task 7): the same MIDI through both players on the real General MIDI
font just_t4_fixed.sf2, reverb and chorus off on both sides. Skipped without the font or fluidsynth.exe.

FluidSynth runs as `fluidsynth.exe -ni -q -R 0 -C 0 -g 1.0 -r 48000 -O float -F out.wav <sf2> <mid>`: the brief's
command plus a float file, because its default 16-bit file has a dither floor near -94 dBFS, which a release tail
60 dB under a quiet note goes below (the piano's would measure the floor, not the release). Ours renders the
same events through smp_synth_render_test with sf_effects off. Each note is analysed alone, with 1.5 s gaps
between notes. Run with -s to see the measured table."""
import ctypes
import itertools
import os
import subprocess

import numpy as np
import pytest
import soundfile as sf

FLUID = r"E:\pypy\fluidsynth\fluidsynth-v2.6.1-win10-x64-cpp11\bin\fluidsynth.exe"
FONT = r"E:\samplepacks\soundfonts\just_t4_fixed.sf2"
RATE = 48000
P_EFFECTS = 33
HELD, GAP = 1.0, 1.5          # each note is held 1 s, then 1.5 s of nothing before the next
_count = itertools.count()

pytestmark = pytest.mark.skipif(not (os.path.exists(FLUID) and os.path.exists(FONT)),
                                reason="needs fluidsynth.exe and just_t4_fixed.sf2")

# (name, channel, program, key): the brief's presets, each on its own channel, and the drum keys on channel 10
PRESETS = [("piano", 0, 0, 60), ("steel guitar", 1, 25, 60), ("finger bass", 2, 33, 60), ("violin", 3, 40, 60),
           ("trumpet", 4, 56, 60), ("flute", 5, 73, 60), ("square lead", 6, 80, 60), ("pad", 7, 88, 60),
           ("kick", 9, None, 36), ("snare", 9, None, 38), ("closed hat", 9, None, 42)]


# ---- the two players ----

def write_mid(path, events, end):
    """events: (seconds, bytes); one tick is 1 ms. A harmless controller on channel 16 at `end` makes FluidSynth
    render up to there."""
    import mido
    mid = mido.MidiFile(type=0, ticks_per_beat=1000)
    tr = mido.MidiTrack()
    mid.tracks.append(tr)
    tr.append(mido.MetaMessage("set_tempo", tempo=1000000, time=0))
    evs = sorted(events, key=lambda e: e[0]) + [(end, (0xBF, 7, 100))]
    now = 0
    for t, b in evs:
        tick = int(round(t * 1000))
        msg = mido.Message.from_bytes(list(b))
        msg.time = tick - now
        now = tick
        tr.append(msg)
    tr.append(mido.MetaMessage("end_of_track", time=0))
    mid.save(str(path))


def render_fluid(tmp, events, end, font=FONT):
    mid = tmp / ("f%d.mid" % next(_count))
    out = tmp / ("f%d.wav" % next(_count))
    write_mid(mid, events, end)
    subprocess.run([FLUID, "-ni", "-q", "-R", "0", "-C", "0", "-g", "1.0", "-r", str(RATE), "-O", "float", "-F", str(out),
                    str(font), str(mid)], check=True, capture_output=True, timeout=300,
                   creationflags=getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0))
    x, rate = sf.read(str(out), dtype="float64", always_2d=True)
    assert rate == RATE
    n = int(round(end * RATE))
    if len(x) < n:
        x = np.vstack([x, np.zeros((n - len(x), 2))])
    return x[:n]


def render_ours(lib, synth, tmp, events, end):
    out = tmp / ("o%d.wav" % next(_count))
    lines = ["midi %s %d" % (" ".join(str(v) for v in b), int(round(t * RATE))) for t, b in events]
    n = int(round(end * RATE))
    assert lib.smp_synth_render_test(synth, "\n".join(lines).encode(), n, 256, str(out).encode(), 0) == 0
    x, rate = sf.read(str(out), dtype="float64", always_2d=True)
    assert rate == RATE and len(x) == n and np.all(np.isfinite(x))
    return x


@pytest.fixture(scope="module")
def synth(lib):
    s = lib.smp_synth_new(float(RATE), 256)
    assert s
    lib.smp_synth_set_param(s, P_EFFECTS, 0)
    err = ctypes.create_string_buffer(512)
    assert lib.smp_synth_load(s, FONT.encode(), err, 512) == 1, err.value
    yield s
    lib.smp_synth_free(s)


@pytest.fixture
def both(lib, synth, tmp_path):
    def run(events, end):
        return render_fluid(tmp_path, events, end), render_ours(lib, synth, tmp_path, events, end)
    return run


# ---- measures ----

def level_db(x):
    return 10 * np.log10(np.mean(x ** 2) + 1e-30)


def seg(x, t0, t1):
    return x[int(round(t0 * RATE)):int(round(t1 * RATE))]


def tail_time(x, t_off, t_end, win=0.005):
    """Seconds from the note-off until the level stays 60 dB under the last 50 ms before it. 0 when the note had
    already ended by itself (a drum). A tail still above that at t_end (a long release, like the pad's) is
    extrapolated from the straight line its level in dB makes after the first 50 ms."""
    ref = level_db(seg(x, t_off - 0.05, t_off))
    if ref < -200:
        return 0.0
    w = int(win * RATE)
    y = seg(x, t_off, t_end)
    nw = len(y) // w
    lv = 10 * np.log10(np.mean(y[:nw * w].reshape(nw, w, -1) ** 2, axis=(1, 2)) + 1e-30)
    above = np.nonzero(lv > ref - 60)[0]
    if len(above) == 0:
        return 0.0
    if above[-1] < nw - 1:
        return (above[-1] + 1) * win
    t = (np.arange(nw) + 0.5) * win
    keep = t > 0.05
    slope, icpt = np.polyfit(t[keep], lv[keep], 1)
    return float("inf") if slope >= 0 else float((ref - 60 - icpt) / slope)


def centroid(x):
    m = x.mean(axis=1)
    spec = np.abs(np.fft.rfft(m * np.hanning(len(m))))
    f = np.fft.rfftfreq(len(m), 1.0 / RATE)
    return float(np.sum(f * spec) / (np.sum(spec) + 1e-30))


def pitch(lib, x):
    m = np.ascontiguousarray(x.mean(axis=1), dtype=np.float32)
    return lib.smp_detect_pitch(m.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), len(m), RATE)


def lag(fl, us, t0, span=0.2, most=128):
    """FluidSynth's delay in samples against ours for the note at t0: it starts notes at its next 64-sample
    block, so its note sits 0 to 64 samples later (searched up to 128). Found by cross-correlation, so each
    player is analysed over the same stretch of its own note."""
    a = seg(fl, t0, t0 + span + (most + 1) / RATE).mean(axis=1)
    b = seg(us, t0, t0 + span).mean(axis=1)
    c = [np.dot(a[k:k + len(b)], b) for k in range(most + 1)]
    return int(np.argmax(c))


def notes_events(notes):
    """notes: (channel, program or None, key, velocity, extra events before the note); one every HELD + GAP s,
    the first at 0.5 s. Returns the events, the note-on times and the end."""
    ev, times = [], []
    t = 0.5
    for ch, prog, key, vel, pre in notes:
        if prog is not None:
            ev.append((t - 0.4, (0xC0 | ch, prog)))
        for b in pre:
            ev.append((t - 0.2, b))
        ev.append((t, (0x90 | ch, key, vel)))
        ev.append((t + HELD, (0x80 | ch, key, 0)))
        times.append(t)
        t += HELD + GAP
    return ev, times, t


# ---- the tests ----

def test_reference_presets(lib, both):
    ev, times, end = notes_events([(ch, prog, key, 100, []) for _, ch, prog, key in PRESETS])
    fl, us = both(ev, end)
    bad = []
    print()
    print("%-12s %8s %8s %6s %7s %7s %6s %6s %8s %8s %6s" % ("preset", "fl dB", "us dB", "d dB", "fl st", "us st",
          "d ct", "fl t60", "us t60", "fl cen", "us cen"))
    for (name, ch, prog, key), t0 in zip(PRESETS, times):
        tf = t0 + lag(fl, us, t0) / RATE      # the same note in FluidSynth's render
        a, b = seg(fl, tf, tf + HELD), seg(us, t0, t0 + HELD)
        la, lb = level_db(a), level_db(b)
        pa, pb = pitch(lib, a), pitch(lib, b)
        ta = tail_time(fl, tf + HELD, tf + HELD + GAP - 0.01)
        tb = tail_time(us, t0 + HELD, t0 + HELD + GAP - 0.01)
        ca, cb = centroid(seg(fl, tf, tf + 0.5)), centroid(seg(us, t0, t0 + 0.5))
        dc = (pb - pa) * 100 if pa > 0 and pb > 0 else float("nan")
        print("%-12s %8.2f %8.2f %6.2f %7.2f %7.2f %6.1f %6.3f %8.3f %8.0f %8.0f" % (name, la, lb, lb - la, pa, pb,
              dc, ta, tb, ca, cb))
        if abs(lb - la) > 2:
            bad.append("%s: level %.2f dB vs FluidSynth %.2f dB" % (name, lb, la))
        if (pa > 0) != (pb > 0) or (pa > 0 and abs(dc) > 5):
            bad.append("%s: pitch %.3f vs FluidSynth %.3f" % (name, pb, pa))
        if abs(tb - ta) > 0.2 * ta:
            bad.append("%s: release tail %.3f s vs FluidSynth %.3f s" % (name, tb, ta))
        if abs(cb - ca) > 0.15 * ca:
            bad.append("%s: centroid %.0f Hz vs FluidSynth %.0f Hz" % (name, cb, ca))
    assert not bad, "\n".join(bad)


def test_reference_velocity_and_cc(lib, both):
    cases = [("vel 100", 100, []), ("vel 30", 30, []), ("vel 64", 64, []), ("vel 127", 127, []),
             ("CC7 50", 100, [(0xB0, 7, 50)]), ("CC11 80", 100, [(0xB0, 7, 100), (0xB0, 11, 80)])]
    notes = [(0, 0 if i == 0 else None, 60, vel, pre) for i, (_, vel, pre) in enumerate(cases)]
    ev, times, end = notes_events(notes)
    fl, us = both(ev, end)
    tf = [t + lag(fl, us, t) / RATE for t in times]       # the same notes in FluidSynth's render
    la = [level_db(seg(fl, t, t + HELD)) for t in tf]
    lb = [level_db(seg(us, t, t + HELD)) for t in times]
    bad = []
    print()
    for (name, _, _), a, b in zip(cases, la, lb):
        da, db = a - la[0], b - lb[0]
        print("%-8s FluidSynth %+7.2f dB  ours %+7.2f dB  (absolute %7.2f / %7.2f)" % (name, da, db, a, b))
        if abs(db - da) > 1:
            bad.append("%s: %+.2f dB vs FluidSynth %+.2f dB" % (name, db, da))
    assert not bad, "\n".join(bad)


def test_reference_pan(lib, tmp_path):
    """CC10 at 0, 64 and 127 on a centred mono sine, against FluidSynth: the left-right balance of each. At 127
    the default pan modulator's bipolar source keeps its top value (127/128, not 2 * 127/128 - 1), so the pan is
    496 of 500 and the left channel is about -44 dB, not silent. That is FluidSynth's reading of SF2.04's
    127/128 ranges (fluid_mod.c, fluid_mod_transform_source_value: `val = (val_norm == (range - 1) / range) ?
    val_norm : -1.0f + 2.0f * val_norm`), and ours."""
    from sf2make import write_sf2
    sr = 44100
    data = (16000 * np.sin(2 * np.pi * 441 * np.arange(sr) / sr)).astype(np.int16)
    font = tmp_path / "pan.sf2"
    write_sf2(str(font), [dict(name="p", bank=0, program=0, gens={}, pmods=[], instruments=[dict(
        name="i", gens={}, mods=[], zones=[dict(keys=(0, 127), vels=(0, 127), sample=0, gens={54: 1}, mods=[])])])],
        [dict(name="s", data=data, rate=sr, pitch=69, loop=(100, 100 + 100 * 400))])
    s = lib.smp_synth_new(float(RATE), 256)
    assert s
    try:
        lib.smp_synth_set_param(s, P_EFFECTS, 0)
        err = ctypes.create_string_buffer(512)
        assert lib.smp_synth_load(s, str(font).encode(), err, 512) == 1, err.value
        ev, times, end = notes_events([(0, None, 69, 100, [(0xB0, 10, v)]) for v in (0, 64, 127)])
        fl = render_fluid(tmp_path, ev, end, font)
        us = render_ours(lib, s, tmp_path, ev, end)
    finally:
        lib.smp_synth_free(s)
    bad = []
    print()
    for v, t in zip((0, 64, 127), times):
        tf = t + lag(fl, us, t) / RATE
        a, b = seg(fl, tf + 0.1, tf + HELD), seg(us, t + 0.1, t + HELD)
        bal = lambda x: float(np.clip(level_db(x[:, 1]) - level_db(x[:, 0]), -80, 80))   # a silent side: 80 dB
        da, db = bal(a), bal(b)
        print("CC10 %3d  right - left: FluidSynth %+8.2f dB  ours %+8.2f dB" % (v, da, db))
        if abs(db - da) > 0.5:
            bad.append("CC10 %d: %+.2f dB vs FluidSynth %+.2f dB" % (v, db, da))
    assert not bad, "\n".join(bad)


def song_events():
    """20 s of General MIDI at 120 BPM: piano chords with the sustain pedal, a bass line, strings with CC7
    automation, a trumpet that becomes a flute (a program change mid-song), a guitar, a pad, a square lead with
    pitch bends, and drums."""
    ev = []
    beat = 0.5
    progs = {0: 0, 1: 33, 2: 48, 3: 56, 4: 25, 5: 88, 6: 80}
    for ch, p in progs.items():
        ev.append((0.0, (0xC0 | ch, p)))
    chords = [(48, 52, 55, 60), (45, 48, 52, 57), (41, 45, 48, 53), (43, 47, 50, 55)]
    for bar in range(10):                       # 2 s bars, 4 beats each
        t = bar * 2.0
        ch = chords[bar % 4]
        ev.append((t, (0xB0, 64, 127)))         # piano pedal down for the bar, up just before the next
        ev.append((t + 1.95, (0xB0, 64, 0)))
        for b in range(4):
            for k in ch:
                ev.append((t + b * beat, (0x90, k + 12, 70 + 10 * (b % 2))))
                ev.append((t + b * beat + 0.2, (0x80, k + 12, 0)))
        for b in range(8):                      # bass eighths
            k = ch[0] - 12 + (7 if b % 4 == 2 else 0)
            ev.append((t + b * 0.25, (0x91, k, 100)))
            ev.append((t + b * 0.25 + 0.2, (0x81, k, 0)))
        for k in ch[1:]:                        # strings, the whole bar
            ev.append((t, (0x92, k, 80)))
            ev.append((t + 1.9, (0x82, k, 0)))
        if bar % 2 == 0:                        # pad, two bars
            ev.append((t, (0x95, ch[0] + 12, 60)))
            ev.append((t + 3.8, (0x85, ch[0] + 12, 0)))
        for b in range(4):                      # guitar on the off-beats
            ev.append((t + b * beat + 0.25, (0x94, ch[2] + 12, 90)))
            ev.append((t + b * beat + 0.45, (0x84, ch[2] + 12, 0)))
        mel = [72, 74, 76, 79]
        if bar >= 2:                            # trumpet, then flute from bar 6
            for b in range(4):
                ev.append((t + b * beat, (0x93, mel[(b + bar) % 4], 95)))
                ev.append((t + b * beat + 0.4, (0x83, mel[(b + bar) % 4], 0)))
        if bar >= 4 and bar % 2 == 0:           # square lead, bent up a tone and back
            ev.append((t + 0.0, (0x96, 79, 90)))
            for i in range(9):
                v = 8192 + int(4095 * np.sin(np.pi * i / 8))
                ev.append((t + 0.2 + i * 0.1, (0xE6, v & 127, v >> 7)))
            ev.append((t + 1.6, (0x86, 79, 0)))
        for b in range(4):                      # drums: kick on 1 and 3, snare on 2 and 4, eighth hats
            ev.append((t + b * beat, (0x99, 36 if b % 2 == 0 else 38, 110)))
            ev.append((t + b * beat + 0.1, (0x89, 36 if b % 2 == 0 else 38, 0)))
            for h in range(2):
                ev.append((t + b * beat + h * 0.25, (0x99, 42, 80)))
                ev.append((t + b * beat + h * 0.25 + 0.05, (0x89, 42, 0)))
    ev.append((12.0, (0xC3, 73)))               # the trumpet becomes a flute
    for i in range(41):                         # strings swell and fade, CC7 from 40 to 120 and back
        ev.append((i * 0.5, (0xB2, 7, int(40 + 80 * np.sin(np.pi * i / 40)))))
    return ev


def test_reference_song(both):
    fl, us = both(song_events(), 20.0)
    bad, rows = 0, []
    for w in range(20):
        a, b = level_db(seg(fl, w, w + 1)), level_db(seg(us, w, w + 1))
        rows.append((w, a, b))
        if abs(b - a) > 3:
            bad += 1
    print()
    for w, a, b in rows:
        print("window %2d  FluidSynth %7.2f dB  ours %7.2f dB  %+6.2f" % (w, a, b, b - a))
    assert bad <= 2, "%d of 20 windows differ by more than 3 dB" % bad
