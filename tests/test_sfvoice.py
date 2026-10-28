"""One SoundFont voice (sfvoice.c): pitch, loops, envelopes, LFOs, filter, gain and pan, rendered with
smp_sf_test_note from small fonts written by sf2make."""
import ctypes
import itertools

import numpy as np
import pytest
from scipy import signal

from sf2make import write_sf2

SR = 44100      # the samples' rate
OUT = 48000     # the output rate
_count = itertools.count()

# generator ids (SF2.04)
START, END, LOOPSTART, LOOPEND, STARTCOARSE = 0, 1, 2, 3, 4
VIBLFO_PITCH, MODENV_PITCH, FC, Q = 6, 7, 8, 9
PAN = 17
DELAY_VIB, FREQ_VIB = 23, 24
DELAY_MODENV, ATTACK_MODENV, HOLD_MODENV, DECAY_MODENV, SUSTAIN_MODENV, RELEASE_MODENV = 25, 26, 27, 28, 29, 30
DELAY_VOL, ATTACK_VOL, HOLD_VOL, DECAY_VOL, SUSTAIN_VOL, RELEASE_VOL = 33, 34, 35, 36, 37, 38
ATTEN, LOOPENDCOARSE, COARSE, FINE, MODES, SCALE, ROOTKEY = 48, 50, 51, 52, 54, 56, 58


def tc(seconds):
    """seconds as timecents"""
    return int(round(1200 * np.log2(seconds)))


def sine(freq, secs, rate=SR, amp=16000):
    t = np.arange(int(secs * rate)) / rate
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.int16)


# 440 Hz at 44.1 kHz repeats exactly every 2205 frames (22 cycles), so loops on multiples of 2205 are seamless
LOOP440 = (2205, 2205 * 19)


def render(lib, tmp_path, sample, gens=None, key=69, vel=127, rate=OUT, secs=1.5, release=None):
    """Write a one-zone font with this sample and zone generators, play one note, return (frames, 2) float."""
    path = tmp_path / ("v%d.sf2" % next(_count))
    zone = dict(keys=(0, 127), vels=(0, 127), sample=0, gens=dict(gens or {}), mods=[])
    write_sf2(path, [dict(name="P", bank=0, program=0, gens={}, pmods=[],
                          instruments=[dict(name="I", gens={}, mods=[], zones=[zone])])], [sample])
    err = ctypes.create_string_buffer(512)
    f = lib.smp_sf_open(str(path).encode(), err, 512)
    assert f, err.value.decode()
    try:
        frames = int(secs * rate)
        out = np.zeros(frames * 2, dtype=np.float32)
        rel = -1 if release is None else int(release * rate)
        n = lib.smp_sf_test_note(f, 0, key, vel, float(rate), frames, rel,
                                 out.ctypes.data_as(ctypes.POINTER(ctypes.c_float)))
        assert n == 1
    finally:
        lib.smp_sf_close(f)
    out = out.reshape(-1, 2)
    assert np.all(np.isfinite(out))
    return out


def sine_sample(freq=440.0, pitch=69, correction=0, secs=1.0, loop=LOOP440, rate=SR):
    return dict(name="sine", data=sine(freq, secs, rate), rate=rate, pitch=pitch, correction=correction, loop=loop)


def dc_sample(secs=1.0, loop=(1000, 40000)):
    return dict(name="dc", data=np.full(int(secs * SR), 16000, dtype=np.int16), rate=SR, pitch=60, loop=loop)


def peak_freq(x, rate):
    """the strongest frequency: Hann window, 8x zero padding, parabolic interpolation on log magnitude"""
    x = x - np.mean(x)
    n = len(x)
    spec = np.abs(np.fft.rfft(x * np.hanning(n), 8 * n)) + 1e-12
    k = int(np.argmax(spec))
    a, b, c = np.log(spec[k - 1:k + 2])
    d = 0.5 * (a - c) / (a - 2 * b + c)
    return (k + d) * rate / (8 * n)


def cents(f, ref):
    return 1200 * np.log2(f / ref)


def pitch_of(out, rate, t0=0.2, t1=1.2):
    return peak_freq(out[int(t0 * rate):int(t1 * rate), 0], rate)


def inst_cents(x, rate, ref):
    """the pitch at each upward zero crossing, in cents from ref: (times, cents)"""
    s = np.signbit(x)
    i = np.nonzero(s[:-1] & ~s[1:])[0]
    t = (i + x[i] / (x[i] - x[i + 1])) / rate
    f = 1.0 / np.diff(t)
    return t[1:], cents(f, ref)


def db(x):
    return 20 * np.log10(x)


def test_pitch_root_and_tuning(lib, tmp_path):
    def check(expected, key=69, gens=None, **kw):
        out = render(lib, tmp_path, sine_sample(**kw), gens, key=key)
        got = pitch_of(out, OUT)
        assert abs(cents(got, expected)) < 2, (expected, got, gens, kw)

    check(880.0, key=81)
    check(880.0, key=69, gens={ROOTKEY: 57})
    check(440.0, key=81, gens={COARSE: -12})
    check(440.0 * 2 ** (50 / 1200), key=69, gens={FINE: 50})
    assert abs(440.0 * 2 ** (50 / 1200) - 452.9) < 0.05
    check(440.0, key=100, gens={SCALE: 0})
    check(440.0, key=30, gens={SCALE: 0})
    check(440.0 * 2 ** (-10 / 1200), key=69, correction=-10)


def test_loop_modes(lib, tmp_path):
    # mode 1 keeps sounding for the whole 2 s; mode 0 stops after the sample's 1 s
    out = render(lib, tmp_path, sine_sample(), {MODES: 1}, secs=2.0)
    assert np.sqrt(np.mean(out[int(1.8 * OUT):, 0] ** 2)) > 0.05
    out = render(lib, tmp_path, sine_sample(), {MODES: 0}, secs=2.0)
    assert np.sqrt(np.mean(out[int(0.8 * OUT):int(0.9 * OUT), 0] ** 2)) > 0.05
    assert np.max(np.abs(out[int(1.01 * OUT):])) == 0
    out = render(lib, tmp_path, sine_sample(), {MODES: 2}, secs=2.0)
    assert np.max(np.abs(out[int(1.01 * OUT):])) == 0

    # mode 3: 0.5 s of 440 Hz (looped from 0.05 s to 0.5 s), then a 0.5 s tail at 660 Hz. Released at 1.0 s with
    # an 8 s release, it plays on through the tail and stops at the sample's end, long before the release ends.
    data = np.concatenate([sine(440.0, 0.5), sine(660.0, 0.5)])
    s = dict(name="m3", data=data, rate=SR, pitch=69, loop=(2205, 22050))
    out = render(lib, tmp_path, s, {MODES: 3, RELEASE_VOL: tc(8.0)}, secs=3.0, release=1.0)
    held = out[int(0.6 * OUT):int(1.0 * OUT), 0]
    assert abs(peak_freq(held, OUT) - 440) < 2
    tail = np.nonzero(np.abs(out[:, 0]) > 0)[0][-1] / OUT
    assert 1.4 < tail < 2.0, tail
    after = out[int(1.0 * OUT):, 0]
    spec = np.abs(np.fft.rfft(after))
    fr = np.fft.rfftfreq(len(after), 1 / OUT)
    assert spec[np.abs(fr - 660) < 5].max() > 0.1 * spec.max()    # the tail was heard
    # mode 1 with the same release still sounds at 2.5 s
    s1 = dict(s, name="m1")
    out = render(lib, tmp_path, s1, {MODES: 1, RELEASE_VOL: tc(8.0)}, secs=3.0, release=1.0)
    assert np.max(np.abs(out[int(2.4 * OUT):int(2.5 * OUT), 0])) > 0.01


def env_of(out):
    return out[:, 0].astype(np.float64)


def first_at_or_above(e, level):
    return np.nonzero(e >= level)[0][0] / OUT


def test_volume_envelope(lib, tmp_path):
    # attack 0.1 s reaches the top at 0.1 s
    e = env_of(render(lib, tmp_path, dc_sample(), {MODES: 1, ATTACK_VOL: tc(0.1)}, secs=0.5))
    top = e.max()
    assert abs(first_at_or_above(e, 0.999 * top) - 0.1) < 0.005
    assert abs(first_at_or_above(e, 0.5 * top) - 0.05) < 0.005     # linear in amplitude
    # delay 0.1 s shifts it by 0.1 s
    e = env_of(render(lib, tmp_path, dc_sample(), {MODES: 1, ATTACK_VOL: tc(0.1), DELAY_VOL: tc(0.1)}, secs=0.5))
    assert np.max(np.abs(e[:int(0.095 * OUT)])) < 1e-4
    assert abs(first_at_or_above(e, 0.999 * e.max()) - 0.2) < 0.005
    # sustain 200 cB: -20 dB once the decay is done; hold 0.2 s holds the top for 0.2 s after the attack
    e = env_of(render(lib, tmp_path, dc_sample(),
                      {MODES: 1, ATTACK_VOL: tc(0.1), HOLD_VOL: tc(0.2), DECAY_VOL: tc(1.0), SUSTAIN_VOL: 200},
                      secs=1.0))
    top = e.max()
    fall = np.nonzero((e < 0.99 * top) & (np.arange(len(e)) > 0.1 * OUT))[0][0] / OUT
    assert abs(fall - 0.3) < 0.005, fall
    # the decay is linear in dB: 100 dB per decay time, so -20 dB takes 0.2 s
    assert abs(db(e[int(0.4 * OUT)] / top) - (-10)) < 0.5
    assert abs(db(np.mean(e[int(0.8 * OUT):]) / top) - (-20)) < 0.5
    # release 0.5 s from full level reaches -100 dB (and the voice ends) 0.5 s after the note-off
    out = render(lib, tmp_path, dc_sample(), {MODES: 1, RELEASE_VOL: tc(0.5)}, secs=1.5, release=0.5)
    e = env_of(out)
    top = e[int(0.4 * OUT)]
    end = np.nonzero(np.abs(e) > 0)[0][-1] / OUT
    assert abs(end - 1.0) < 0.01, end
    assert abs(db(e[int(0.75 * OUT)] / top) - (-50)) < 1.0    # linear in dB on the way down


def test_attenuation_and_velocity(lib, tmp_path):
    a = env_of(render(lib, tmp_path, dc_sample(), {MODES: 1}, secs=0.5))[int(0.3 * OUT)]
    b = env_of(render(lib, tmp_path, dc_sample(), {MODES: 1, ATTEN: 100}, secs=0.5))[int(0.3 * OUT)]
    assert abs(db(b / a) - (-4.0)) < 0.1, db(b / a)
    # the default velocity modulator: 960 cB through the negative concave curve. The spec's concave curve at
    # velocity v gives 960 * (-20/96) * log10(((v)/127)^2) cB, that is 40 * log10(v / 127) dB.
    c = env_of(render(lib, tmp_path, dc_sample(), {MODES: 1}, vel=64, secs=0.5))[int(0.3 * OUT)]
    want = 40 * np.log10(64 / 127)
    assert abs(db(c / a) - want) < 0.5, (db(c / a), want)


def noise_sample():
    rng = np.random.default_rng(5)
    # at the output rate and played at its root, so the voice reads it sample for sample
    return dict(name="noise", data=(rng.standard_normal(4 * OUT) * 6000).astype(np.int16), rate=OUT, pitch=60,
                loop=(100, 4 * OUT - 100))


def response(lib, tmp_path, gens):
    s = noise_sample()
    out = render(lib, tmp_path, s, {**gens, MODES: 1}, key=60, secs=3.5)[:, 0]
    x = s["data"].astype(np.float64)
    n = int(3.0 * OUT)
    y = out[int(0.2 * OUT):int(0.2 * OUT) + n].astype(np.float64)
    xin = x[int(0.2 * OUT):int(0.2 * OUT) + n]
    f, pxy = signal.csd(xin, y, OUT, nperseg=16384)
    _, pxx = signal.welch(xin, OUT, nperseg=16384)
    return f, np.abs(pxy / pxx)


def test_filter(lib, tmp_path):
    f, h = response(lib, tmp_path, {FC: 6000})
    fc = 8.176 * 2 ** (6000 / 1200)
    assert abs(fc - 262) < 1
    ref = np.mean(h[(f > 20) & (f < 60)])
    above = np.nonzero((f > 40) & (h < ref * 10 ** (-3 / 20)))[0][0]
    f3 = f[above]
    assert abs(f3 - fc) < 0.1 * fc, f3
    # an octave above the cutoff, 2 poles: about -12 dB or lower
    assert db(np.mean(h[(f > 2 * fc - 10) & (f < 2 * fc + 10)]) / ref) < -9
    # Q 100 cB: a peak about 10 dB over the passband at the cutoff
    f, h = response(lib, tmp_path, {FC: 6000, Q: 100})
    ref = np.mean(h[(f > 20) & (f < 60)])
    near = (f > 0.7 * fc) & (f < 1.3 * fc)
    peak = db(h[near].max() / ref)
    assert abs(peak - 10) < 2, peak


def test_lfo_vibrato(lib, tmp_path):
    out = render(lib, tmp_path, sine_sample(), {MODES: 1, VIBLFO_PITCH: 50, FREQ_VIB: 0}, secs=2.5)
    t, c = inst_cents(out[:, 0].astype(np.float64), OUT, 440.0)
    sel = (t > 0.3) & (t < 2.4)
    t, c = t[sel], c[sel]
    assert 44 < c.max() < 56 and -56 < c.min() < -44, (c.min(), c.max())
    up = np.nonzero((c[:-1] < 0) & (c[1:] >= 0))[0]
    rate = (len(up) - 1) / (t[up[-1]] - t[up[0]])
    assert abs(rate - 8.2) < 0.3, rate
    # a 0.5 s delay keeps the pitch still until then
    out = render(lib, tmp_path, sine_sample(), {MODES: 1, VIBLFO_PITCH: 50, FREQ_VIB: 0, DELAY_VIB: tc(0.5)},
                 secs=1.5)
    t, c = inst_cents(out[:, 0].astype(np.float64), OUT, 440.0)
    still = c[(t > 0.02) & (t < 0.48)]
    assert np.max(np.abs(still)) < 2, np.max(np.abs(still))
    assert np.max(np.abs(c[(t > 0.6) & (t < 1.2)])) > 40


def test_mod_envelope_to_pitch(lib, tmp_path):
    out = render(lib, tmp_path, sine_sample(),
                 {MODES: 1, MODENV_PITCH: 1200, DECAY_MODENV: tc(0.5), SUSTAIN_MODENV: 1000}, secs=1.0)
    t, c = inst_cents(out[:, 0].astype(np.float64), OUT, 440.0)
    early = c[(t > 0.005) & (t < 0.015)]
    assert np.all(early > 1150), early
    mid = np.median(c[(t > 0.24) & (t < 0.26)])
    assert abs(mid - 600) < 40, mid
    late = c[(t > 0.52) & (t < 0.9)]
    assert np.max(np.abs(late)) < 3, np.max(np.abs(late))


def test_pan(lib, tmp_path):
    left = render(lib, tmp_path, dc_sample(), {MODES: 1, PAN: -500}, secs=0.5)[int(0.3 * OUT)]
    assert left[0] > 0
    assert abs(left[1]) < left[0] * 1e-3      # right below -60 dB
    mid = render(lib, tmp_path, dc_sample(), {MODES: 1, PAN: 0}, secs=0.5)[int(0.3 * OUT)]
    assert abs(db(mid[0] / mid[1])) < 0.01
    assert abs(db(mid[0] / left[0]) - (-3.01)) < 0.1


def test_bad_loop_points_clamped(lib, tmp_path):
    data = sine(440.0, 0.25)
    n = len(data)
    cases = [
        (dict(name="b1", data=data, rate=SR, pitch=69, loop=(100, n + 5000)), {MODES: 1}),
        (dict(name="b2", data=data, rate=SR, pitch=69, loop=(3000, 1000)), {MODES: 1}),
        (dict(name="b3", data=data, rate=SR, pitch=69, loop=(100, 3000)), {MODES: 1, LOOPEND: 30000}),
        (dict(name="b4", data=data, rate=SR, pitch=69, loop=(100, 3000)), {MODES: 3, LOOPSTART: 20000}),
        (dict(name="b5", data=data, rate=SR, pitch=69, loop=(100, 3000)), {MODES: 1, LOOPENDCOARSE: 32767}),
        (dict(name="b6", data=data, rate=SR, pitch=69, loop=(100, 3000)), {MODES: 1, START: 32000, STARTCOARSE: 5}),
        (dict(name="b7", data=data, rate=SR, pitch=69, loop=(100, 3000)), {MODES: 1, END: -32000}),
        (dict(name="b8", data=data, rate=SR, pitch=69, loop=None), {MODES: 1}),
    ]
    for s, g in cases:
        out = render(lib, tmp_path, s, g, secs=0.6, release=0.3)
        assert np.max(np.abs(out)) < 4, (s["name"], np.max(np.abs(out)))


@pytest.mark.parametrize("rate", [44100, 48000, 96000])
def test_host_rates(lib, tmp_path, rate):
    out = render(lib, tmp_path, sine_sample(), {MODES: 1}, key=81, rate=rate)
    assert abs(cents(pitch_of(out, rate), 880.0)) < 2
