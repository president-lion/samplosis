"""SFZ, 2026-11-20, from Karoryfer's Pastabass (combo_platter_enhanced.sfz): amplitude_onccN is a layer's fader
(its level times X / 100 times the controller over 127), and CC91 or CC93 that a file's own opcodes use (locc91,
amplitude_oncc93...) are its controls, not GM's reverb and chorus sends."""
from test_sf2 import mods, regions
from test_sfz import OUT, RATE, Player, db, ignored, on, opened, rms, sine, write_sfz, write_wav
from test_sfz_nbo import FxPlayer, cc


def level(lib, tmp_path, path, events):
    p = Player(lib, tmp_path, path)
    try:
        return rms(p.render(events + [on(60, 100, 0.01)], 0.4)[int(0.1 * OUT):])
    finally:
        p.free()


def test_sfz_amplitude_oncc_is_a_linear_fader(lib, tmp_path):
    """amplitude_oncc30=100: full at CC30 127, -6 dB at 64 (64 / 127), -12 dB at 32; amplitude_oncc30=50 halves
    it again. A concave CC-to-attenuation modulator of 480 cB, nothing ignored."""
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0))
    base = write_sfz(tmp_path / "b.sfz", "<region> sample=a.wav key=60 pitch_keycenter=60 amp_veltrack=0\n")
    fad = write_sfz(tmp_path / "f.sfz", "<region> sample=a.wav key=60 pitch_keycenter=60 amp_veltrack=0 amplitude_oncc30=100\n")
    half = write_sfz(tmp_path / "h.sfz", "<region> sample=a.wav key=60 pitch_keycenter=60 amp_veltrack=0 amplitude_oncc30=50\n")
    f = opened(lib, fad)
    try:
        assert ignored(lib, f)[0] == 0
        r = regions(lib, f, 0)
        assert (0x0580 | 30, 48, 480, 0, 0) in mods(r[0].imod, r[0].nimod)
    finally:
        lib.smp_sf_close(f)
    ref = level(lib, tmp_path, base, [])
    assert abs(db(level(lib, tmp_path, fad, [cc(30, 127, 0)]), ref)) < 0.3
    assert abs(db(level(lib, tmp_path, fad, [cc(30, 64, 0)]), ref) - db(64, 127)) < 0.5
    assert abs(db(level(lib, tmp_path, fad, [cc(30, 32, 0)]), ref) - db(32, 127)) < 0.5
    assert abs(db(level(lib, tmp_path, half, [cc(30, 127, 0)]), ref) - db(50, 100)) < 0.3


def test_sfz_amplitude_oncc7_keeps_the_volume_default(lib, tmp_path):
    """amplitude_oncc7=100 (Pastabass's master volume) is what CC7 does anyway: no second CC7 modulator, so the
    level at CC7 90 is the same as without it."""
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0))
    plain = write_sfz(tmp_path / "p.sfz", "<region> sample=a.wav key=60 pitch_keycenter=60 amp_veltrack=0\n")
    vol = write_sfz(tmp_path / "v.sfz", "<global> amplitude_oncc7=100\n<region> sample=a.wav key=60 pitch_keycenter=60 amp_veltrack=0\n")
    f = opened(lib, vol)
    try:
        assert ignored(lib, f)[0] == 0
        r = regions(lib, f, 0)
        assert [m for m in mods(r[0].imod, r[0].nimod) if m[0] & 0xFF == 0x87] == [(0x0587, 48, 960, 0, 0)]
    finally:
        lib.smp_sf_close(f)
    a = level(lib, tmp_path, plain, [cc(7, 90, 0)])
    b = level(lib, tmp_path, vol, [cc(7, 90, 0)])
    assert abs(db(b, a)) < 0.2


def test_sfz_own_cc91_is_no_reverb_send(lib, tmp_path):
    """Pastabass's set_cc91=127 is a layer fader (locc91, amplitude_oncc91): no reverb tail, in the regions that
    use it and in the ones that don't. A plain set_cc91 still sends."""
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0))
    own = write_sfz(tmp_path / "o.sfz", "<control> set_cc91=127\n"
                    "<region> sample=a.wav key=60 pitch_keycenter=60 ampeg_release=0.02\n"
                    "<region> sample=a.wav key=60 pitch_keycenter=60 ampeg_release=0.02 locc91=1 amplitude_oncc91=100\n")
    p = FxPlayer(lib, tmp_path, own)
    try:
        y = p.render([on(60, 100, 0), "midi 128 60 0 %d" % int(0.2 * OUT)], 1.0)
    finally:
        p.free()
    assert rms(y[:int(0.15 * OUT)]) > 0.05
    assert rms(y[int(0.5 * OUT):]) < 1e-6


def test_sfz_level_volumes_add(lib, tmp_path):
    """group_volume (G-Town Church's Organ, -2 on its release group), master_volume and global_volume add to a
    region's own volume rather than being replaced by it: -2 + -3 + -1 + volume=-4 is -10 dB."""
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0))
    base = write_sfz(tmp_path / "b.sfz", "<region> sample=a.wav key=60 pitch_keycenter=60 amp_veltrack=0\n")
    lv = write_sfz(tmp_path / "l.sfz", "<global> global_volume=-1\n<master> master_volume=-3\n<group> group_volume=-2\n"
                   "<region> sample=a.wav key=60 pitch_keycenter=60 amp_veltrack=0 volume=-4\n")
    f = opened(lib, lv)
    try:
        assert ignored(lib, f)[0] == 0
    finally:
        lib.smp_sf_close(f)
    assert abs(db(level(lib, tmp_path, lv, []), level(lib, tmp_path, base, [])) + 10) < 0.3
