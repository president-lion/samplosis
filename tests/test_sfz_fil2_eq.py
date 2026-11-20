"""SFZ, 2026-11-20: the second filter (fil2_type, cutoff2, resonance2, their controllers: Karoryfer's Pastabass has a
high-pass on cutoff2_cc115) and the three EQ bands (eqN_freq, eqN_bw, eqN_gain, their controllers and curves:
Headroom Piano has one EQ per mic on CCs), heard on sines."""
from test_sfz import OUT, RATE, Player, db, ignored, on, opened, rms, sine, write_sfz, write_wav
from test_sfz_nbo import cc

R = "<region> sample=%s key=60 pitch_keycenter=60 amp_veltrack=0 "


def level(lib, tmp_path, path, events=(), t0=0.1, t1=0.4):
    p = Player(lib, tmp_path, path)
    try:
        y = p.render(list(events) + [on(60, 100, 0.01)], t1)
    finally:
        p.free()
    return rms(y[int(t0 * OUT):int(t1 * OUT)])


def none_ignored(lib, path):
    f = opened(lib, path)
    try:
        assert ignored(lib, f)[0] == 0, ignored(lib, f)
    finally:
        lib.smp_sf_close(f)


def test_sfz_fil2_highpass(lib, tmp_path):
    """fil2_type=hpf_2p cutoff2=2000: a 200 Hz sine falls about 40 dB (12 dB an octave, 3.3 octaves), a 8 kHz one
    passes; the first filter (none here) is untouched."""
    write_wav(tmp_path / "lo.wav", sine(RATE, 200.0))
    write_wav(tmp_path / "hi.wav", sine(RATE, 8000.0))
    out = {}
    for s in ("lo", "hi"):
        dry = write_sfz(tmp_path / ("d%s.sfz" % s), R % (s + ".wav") + "\n")
        hp = write_sfz(tmp_path / ("h%s.sfz" % s), R % (s + ".wav") + "fil2_type=hpf_2p cutoff2=2000\n")
        none_ignored(lib, hp)
        out[s] = db(level(lib, tmp_path, hp), level(lib, tmp_path, dry))
    assert -46 < out["lo"] < -34
    assert abs(out["hi"]) < 0.5


def test_sfz_cutoff2_cc_moves_live(lib, tmp_path):
    """Pastabass's form: hpf_2p at 1200 Hz with cutoff2_cc115=-4800 and CC115 starting at 127 is a 75 Hz high-pass,
    so a 300 Hz sine passes; CC115 sent to 0 while the note holds moves it to 1200 Hz and the sine drops."""
    write_wav(tmp_path / "a.wav", sine(RATE, 300.0))
    path = write_sfz(tmp_path / "c.sfz", "<control> set_cc115=127\n" + R % "a.wav" +
                     "fil2_type=hpf_2p cutoff2=1200 cutoff2_cc115=-4800\n")
    none_ignored(lib, path)
    dry = level(lib, tmp_path, write_sfz(tmp_path / "d.sfz", R % "a.wav" + "\n"))
    p = Player(lib, tmp_path, path)
    try:
        y = p.render([on(60, 100, 0.01), cc(115, 0, 0.3)], 0.7)
    finally:
        p.free()
    assert abs(db(rms(y[int(0.1 * OUT):int(0.28 * OUT)]), dry)) < 1.0
    assert db(rms(y[int(0.45 * OUT):int(0.7 * OUT)]), dry) < -15


def test_sfz_eq_band_gain(lib, tmp_path):
    """eq2_freq=1000 eq2_bw=1 eq2_gain=12: +12 dB on a 1 kHz sine, nearly nothing two octaves away (250 Hz)."""
    write_wav(tmp_path / "k.wav", sine(RATE, 1000.0))
    write_wav(tmp_path / "q.wav", sine(RATE, 250.0))
    for s, lo, hi in (("k", 11.5, 12.5), ("q", -0.1, 1.5)):
        dry = write_sfz(tmp_path / ("d%s.sfz" % s), R % (s + ".wav") + "\n")
        eq = write_sfz(tmp_path / ("e%s.sfz" % s), R % (s + ".wav") + "eq2_freq=1000 eq2_bw=1 eq2_gain=12\n")
        none_ignored(lib, eq)
        g = db(level(lib, tmp_path, eq), level(lib, tmp_path, dry))
        assert lo < g < hi, (s, g)


def test_sfz_eq_gain_on_a_bipolar_cc(lib, tmp_path):
    """Headroom's form: eq1_gain_oncc86=12 with eq1_gain_curvecc86=1 (bipolar) and eq1_freq_oncc80=380 from 20 Hz:
    CC86 at 64 is flat; at 127 +12 dB and at 0 -12 dB on a 400 Hz sine (CC80 at 127: 400 Hz)."""
    write_wav(tmp_path / "a.wav", sine(RATE, 400.0))
    body = "eq1_freq=20 eq1_bw=1 eq1_freq_oncc80=380 eq1_gain_oncc86=12 eq1_gain_curvecc86=1\n"
    dry = level(lib, tmp_path, write_sfz(tmp_path / "d.sfz", R % "a.wav" + "\n"))
    out = {}
    for v in (0, 64, 127):
        path = write_sfz(tmp_path / ("e%d.sfz" % v), "<control> set_cc80=127 set_cc86=%d\n" % v + R % "a.wav" + body)
        none_ignored(lib, path)
        out[v] = db(level(lib, tmp_path, path), dry)
    assert abs(out[64]) < 0.3
    assert 11 < out[127] < 12.5
    assert -12.5 < out[0] < -11


def test_sfz_resonance_cc(lib, tmp_path):
    """resonance_cc118=24 (Pastabass's first-filter resonance knob): a CC118-to-Q modulator of 240 cB."""
    from test_sf2 import mods, regions
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0))
    path = write_sfz(tmp_path / "r.sfz", R % "a.wav" + "cutoff=2000 resonance_cc118=24\n")
    f = opened(lib, path)
    try:
        assert ignored(lib, f)[0] == 0
        r = regions(lib, f, 0)
        assert (0x80 | 118, 9, 240, 0, 0) in mods(r[0].imod, r[0].nimod)
    finally:
        lib.smp_sf_close(f)
