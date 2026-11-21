"""SFZ, 2026-11-20, from Malaclypse's Kalimbass (lfo01_pitch_oncc1, lfo01_freq, lfo01_freq_oncc3, lfo01_fade_oncc4) and
Pastabass: SFZ v2's LFOs, lfoN_freq / wave / delay / fade / phase and lfoN_pitch / cutoff / volume / amplitude with
their controllers. A square wave (lfoN_wave=3) on pitch makes them measurable: +depth for the first half of each
period, -depth for the second."""
from test_sfz_flex_eg import cents, hz_at, none_ignored, render, setup
from test_sfz import write_sfz, on


def test_lfo_square_on_pitch(lib, tmp_path):
    """1 Hz square, 100 cents: a semitone up for the first half second, a semitone down for the next."""
    r = setup(tmp_path)
    path = write_sfz(tmp_path / "s.sfz", r + "key=69 lfo1_wave=3 lfo1_freq=1 lfo1_pitch=100\n")
    none_ignored(lib, path)
    y = render(lib, tmp_path, path, [on(69, 100, 0)], 1.0)
    assert abs(cents(hz_at(y, 0.1, 0.4)) - 100) < 8
    assert abs(cents(hz_at(y, 0.6, 0.9)) + 100) < 8


def test_lfo_depth_on_a_cc(lib, tmp_path):
    """Kalimbass's mod wheel: lfo01_pitch_oncc1=100 and no depth of its own, so CC1 at 0 leaves the pitch alone."""
    r = setup(tmp_path)
    path = write_sfz(tmp_path / "d.sfz", r + "key=69 lfo01_wave=3 lfo01_freq=1 lfo01_pitch_oncc1=100\n")
    none_ignored(lib, path)
    up = render(lib, tmp_path, path, ["midi 176 1 127 0", on(69, 100, 0.01)], 0.5)
    flat = render(lib, tmp_path, path, ["midi 176 1 0 0", on(69, 100, 0.01)], 0.5)
    assert abs(cents(hz_at(up, 0.1, 0.4)) - 100) < 8
    assert abs(cents(hz_at(flat, 0.1, 0.4))) < 8


def test_lfo_rate_on_a_cc(lib, tmp_path):
    """lfo1_freq_oncc3=2 on a rate of 0: CC3 at 127 makes it 2 Hz (down by 0.3 s); at 0 it never moves on."""
    r = setup(tmp_path)
    path = write_sfz(tmp_path / "f.sfz", r + "key=69 lfo1_wave=3 lfo1_freq=0 lfo1_freq_oncc3=2 lfo1_pitch=100\n")
    none_ignored(lib, path)
    fast = render(lib, tmp_path, path, ["midi 176 3 127 0", on(69, 100, 0.01)], 0.5)
    still = render(lib, tmp_path, path, ["midi 176 3 0 0", on(69, 100, 0.01)], 0.5)
    assert abs(cents(hz_at(fast, 0.3, 0.45)) + 100) < 8
    assert abs(cents(hz_at(still, 0.3, 0.45)) - 100) < 8


def test_lfo_delay_and_fade(lib, tmp_path):
    """lfo1_delay=0.5: nothing for the first half second, then the square's first half (up). lfo1_fade_oncc4 is read."""
    r = setup(tmp_path)
    path = write_sfz(tmp_path / "w.sfz", r + "key=69 lfo1_wave=3 lfo1_freq=1 lfo1_pitch=100 lfo1_delay=0.5 "
                     "lfo1_fade_oncc4=3\n")
    none_ignored(lib, path)
    y = render(lib, tmp_path, path, ["midi 176 4 0 0", on(69, 100, 0.01)], 1.0)
    assert abs(cents(hz_at(y, 0.1, 0.4))) < 8
    assert abs(cents(hz_at(y, 0.65, 0.95)) - 100) < 8


def test_lfo_phase_and_rate_from_another(lib, tmp_path):
    """lfo1_phase=0.5 starts the square in its second half (down). lfo2_freq_lfo1 is read (a rate-only LFO)."""
    r = setup(tmp_path)
    path = write_sfz(tmp_path / "p.sfz", r + "key=69 lfo1_wave=3 lfo1_freq=1 lfo1_phase=0.5 lfo1_pitch=100 "
                     "lfo2_freq=0.1 lfo1_freq_lfo2=0\n")
    none_ignored(lib, path)
    y = render(lib, tmp_path, path, [on(69, 100, 0)], 0.5)
    assert abs(cents(hz_at(y, 0.1, 0.4)) + 100) < 8
