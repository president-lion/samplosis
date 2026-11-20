"""SFZ, 2026-11-20, from Unreal Instruments' Standard Guitar (its trills, bends, slides and legato are flex envelopes
on pitch: egN_timeK, egN_levelK, egN_curveK, egN_sustain, egN_pitch, egN_timeK_onccN, egN_pitch_onccN and
egN_pitch_oncc140, ARIA's key delta) and the smaller opcodes it uses: delay_ccN, offset_random, fileg_depthccN."""
import math

from test_sf2 import mods, peak_hz, regions
from test_sfz import OUT, RATE, Player, ignored, off, on, opened, rms, sine, write_sfz, write_wav

# a long looped 440 Hz sine on key 69, so every key's pitch is 440 x 2^((key - 69) / 12)
R = "<region> sample=a.wav pitch_keycenter=69 loop_mode=loop_continuous loop_start=0 loop_end=%d amp_veltrack=0 "


def setup(tmp_path):
    n = 44100 * 4 // 440 * 440 // 100 * 100       # whole periods: 4 s
    write_wav(tmp_path / "a.wav", sine(n, 440.0), loop=(0, n - 1))
    return R % (n - 1)


def hz_at(y, t0, t1):
    return peak_hz(y[int(t0 * OUT):int(t1 * OUT)], OUT)


def cents(hz, ref=440.0):
    return 1200 * math.log2(hz / ref)


def none_ignored(lib, path):
    f = opened(lib, path)
    try:
        assert ignored(lib, f)[0] == 0, ignored(lib, f)
    finally:
        lib.smp_sf_close(f)


def render(lib, tmp_path, path, events, secs):
    p = Player(lib, tmp_path, path)
    try:
        return p.render(events, secs)
    finally:
        p.free()


def test_flex_eg_pitch_steps_and_sustain(lib, tmp_path):
    """eg1: point 0 at -1 at once, held 0.4 s at -1 (point 1), then to 0 in 0.01 s (point 2, the sustain):
    with eg1_pitch=-100 the note starts a semitone up (level -1 x -100 cents) and falls back to 440 Hz."""
    r = setup(tmp_path)
    path = write_sfz(tmp_path / "t.sfz", r + "key=69 eg1_pitch=-100 eg1_sustain=2 eg1_time0=0 eg1_level0=-1 "
                     "eg1_time1=0.4 eg1_level1=-1 eg1_time2=0.01 eg1_level2=0\n")
    none_ignored(lib, path)
    y = render(lib, tmp_path, path, [on(69, 100, 0)], 1.0)
    assert abs(cents(hz_at(y, 0.05, 0.35)) - 100) < 8
    assert abs(cents(hz_at(y, 0.55, 0.95))) < 8


def test_flex_eg_release_goes_past_the_sustain(lib, tmp_path):
    """Held at point 0 (sustain=0, level 0) until the note-off, then point 1 (level 1, at once) with eg1_pitch=1200:
    an octave up only once released (the amplitude envelope's release keeps it sounding)."""
    r = setup(tmp_path)
    path = write_sfz(tmp_path / "r.sfz", r + "key=69 ampeg_release=1 eg1_pitch=1200 eg1_sustain=0 eg1_time0=0 "
                     "eg1_level0=0 eg1_time1=0 eg1_level1=1\n")
    none_ignored(lib, path)
    y = render(lib, tmp_path, path, [on(69, 100, 0), off(69, 0.4)], 0.8)
    assert abs(cents(hz_at(y, 0.05, 0.35))) < 8
    assert abs(cents(hz_at(y, 0.45, 0.75)) - 1200) < 8


def test_flex_eg_time_on_a_cc(lib, tmp_path):
    """egN_time1_onccN adds seconds at note-on: point 1 (still -1) lasts 0.1 s plus 0.5 s at CC20 127, so the
    semitone up is still there at 0.4 s with CC20 at 127 and gone with it at 0."""
    r = setup(tmp_path)
    body = (r + "key=69 eg1_pitch=-100 eg1_sustain=2 eg1_level0=-1 eg1_time1=0.1 eg1_time1_oncc20=0.5 eg1_level1=-1 "
            "eg1_time2=0.01 eg1_level2=0\n")
    path = write_sfz(tmp_path / "c.sfz", body)
    none_ignored(lib, path)
    long_ = render(lib, tmp_path, path, ["midi 176 20 127 0", on(69, 100, 0.01)], 0.6)
    short = render(lib, tmp_path, path, ["midi 176 20 0 0", on(69, 100, 0.01)], 0.6)
    assert abs(cents(hz_at(long_, 0.3, 0.5)) - 100) < 8
    assert abs(cents(hz_at(short, 0.3, 0.5))) < 8


def test_flex_eg_key_delta_glide(lib, tmp_path):
    """Standard Guitar's legato: eg1_pitch_oncc140=100 from level -1 to 0 over 0.3 s. A note two semitones above the
    last starts two semitones down (at the last note's pitch) and slides up; the first note (no last) doesn't."""
    r = setup(tmp_path)
    path = write_sfz(tmp_path / "k.sfz", r + "lokey=60 hikey=80 eg1_pitch_oncc140=100 eg1_sustain=1 eg1_time0=0 "
                     "eg1_level0=-1 eg1_time1=0.3 eg1_level1=0\n")
    none_ignored(lib, path)
    y = render(lib, tmp_path, path, [on(69, 100, 0), off(69, 0.5), on(71, 100, 0.5)], 1.2)
    assert abs(cents(hz_at(y, 0.05, 0.45))) < 8                  # the first note: no delta
    assert abs(cents(hz_at(y, 0.5, 0.53)) - 0) < 60               # the second starts near the first's pitch
    assert abs(cents(hz_at(y, 0.9, 1.2)) - 200) < 8               # and ends on its own (two semitones up)


def test_delay_cc_and_offset_random(lib, tmp_path):
    """delay_cc30=0.2: with CC30 at 127 the note starts 0.2 s late. offset_random=1000 is read (not ignored) and the
    note still plays. fileg_depthcc119 is a CC-to-modEnvToFilterFc modulator."""
    r = setup(tmp_path)
    path = write_sfz(tmp_path / "d.sfz", r + "key=69 delay_cc30=0.2 offset_random=1000 cutoff=500 fileg_depth=0 "
                     "fileg_depthcc119=9600\n")
    none_ignored(lib, path)
    y = render(lib, tmp_path, path, ["midi 176 30 127 0", on(69, 100, 0.01)], 0.5)
    assert rms(y[int(0.02 * OUT):int(0.18 * OUT)]) < 1e-6
    assert rms(y[int(0.3 * OUT):]) > 0.01
    f = opened(lib, path)
    try:
        rg = regions(lib, f, 0)
        assert (0x80 | 119, 11, 9600, 0, 0) in mods(rg[0].imod, rg[0].nimod)
    finally:
        lib.smp_sf_close(f)
