"""SFZ, 2026-11-20, from sfzinstruments' Headroom Piano (silent before: its mics are gated on CC74 and CC77, which
only its set_hdccN gave a value): set_hdccN, controller curves (X_curveccN with SFZ's built-in curves and the
file's own <curve>s, which may come after the regions), and amp_veltrack_onccN and width_onccN."""
from test_sf2 import mods, regions
from test_sfz import OUT, RATE, Player, db, ignored, on, opened, rms, sine, write_sfz, write_wav

R = "<region> sample=a.wav key=60 pitch_keycenter=60 amp_veltrack=0 "


def lvl(lib, tmp_path, path, vel=100):
    p = Player(lib, tmp_path, path)
    try:
        return rms(p.render([on(60, vel, 0)], 0.4)[int(0.1 * OUT):])
    finally:
        p.free()


def region_mods(lib, path):
    f = opened(lib, path)
    try:
        assert ignored(lib, f)[0] == 0, ignored(lib, f)
        r = regions(lib, f, 0)
        return mods(r[0].imod, r[0].nimod)
    finally:
        lib.smp_sf_close(f)


def test_sfz_set_hdcc(lib, tmp_path):
    """set_hdcc24=0.5 is CC24 at 64 (a region on locc24=60 hicc24=70 plays); set_hdcc24=1 is 127 (it doesn't)."""
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0))
    half = write_sfz(tmp_path / "h.sfz", "<control> set_hdcc24=0.5\n" + R + "locc24=60 hicc24=70\n")
    full = write_sfz(tmp_path / "f.sfz", "<control> set_hdcc24=1\n" + R + "locc24=60 hicc24=70\n")
    region_mods(lib, half)
    assert lvl(lib, tmp_path, half) > 0.05
    assert lvl(lib, tmp_path, full) < 1e-6


def test_sfz_builtin_curves_are_modulator_sources(lib, tmp_path):
    """pan_curvecc20=1 makes the pan modulator bipolar (centred at 64), 2 negative, 3 both; pan_oncc10 with its
    curve replaces the default CC10 pan rather than adding a second. Amplitude's 2, 4, 5 and 6 are the concave
    curve at 480 rising, 960, 240, and 240 rising."""
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0))
    for k, src in ((0, 0x0080), (1, 0x0280), (2, 0x0180), (3, 0x0380)):
        m = region_mods(lib, write_sfz(tmp_path / ("p%d.sfz" % k), R + "pan_oncc20=100 pan_curvecc20=%d\n" % k))
        assert (src | 20, 17, 500, 0, 0) in m
    m = region_mods(lib, write_sfz(tmp_path / "c10.sfz", R + "pan_oncc10=100 pan_curvecc10=1\n"))
    assert [x for x in m if x[0] & 0xFF == 0x8A] == [(0x028A, 17, 500, 0, 0)]
    for k, want in ((2, (0x0480 | 30, 48, 480, 0, 0)), (4, (0x0580 | 30, 48, 960, 0, 0)),
                    (5, (0x0580 | 30, 48, 240, 0, 0)), (6, (0x0480 | 30, 48, 240, 0, 0))):
        m = region_mods(lib, write_sfz(tmp_path / ("a%d.sfz" % k), R + "amplitude_oncc30=100 amplitude_curvecc30=%d\n" % k))
        assert want in m


def test_sfz_custom_curve_sets_the_start(lib, tmp_path):
    """A <curve> of the file's own (given after the region that uses it) can't be a modulator's: volume_oncc40=-12
    through curve 9 (1 at 0, 0 at 127) is -12 dB at CC40's start of 0 and -6 dB with set_cc40=64, as a fixed
    volume (no CC40 modulator)."""
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0))
    base = write_sfz(tmp_path / "b.sfz", R + "\n")
    cur = "\n<curve> curve_index=9 v000=1 v127=0\n"
    zero = write_sfz(tmp_path / "z.sfz", R + "volume_oncc40=-12 volume_curvecc40=9\n" + cur)
    mid = write_sfz(tmp_path / "m.sfz", "<control> set_cc40=64\n" + R + "volume_oncc40=-12 volume_curvecc40=9\n" + cur)
    assert not [x for x in region_mods(lib, zero) if x[0] & 0xFF == 0x80 | 40]
    ref = lvl(lib, tmp_path, base)
    assert abs(db(lvl(lib, tmp_path, zero), ref) + 12) < 0.3
    assert abs(db(lvl(lib, tmp_path, mid), ref) + 12 * 63 / 127) < 0.3


def test_sfz_amp_veltrack_oncc(lib, tmp_path):
    """amp_veltrack=0 amp_veltrack_oncc73=100 (Headroom's Veltrack knob): with set_hdcc73=1 soft notes are quieter
    than loud ones; with set_hdcc73=0 they're the same."""
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0))
    on_ = write_sfz(tmp_path / "v1.sfz", "<control> set_hdcc73=1\n" + R + "amp_veltrack_oncc73=100\n")
    off_ = write_sfz(tmp_path / "v0.sfz", "<control> set_hdcc73=0\n" + R + "amp_veltrack_oncc73=100\n")
    region_mods(lib, on_)
    assert db(lvl(lib, tmp_path, on_, 40), lvl(lib, tmp_path, on_, 127)) < -6
    assert abs(db(lvl(lib, tmp_path, off_, 40), lvl(lib, tmp_path, off_, 127))) < 0.3
