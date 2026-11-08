"""The Decent Sampler effects chain (sfdsfx.c): resonant filters, a Freeverb reverb with the preset's room size
and damping, and a stereo delay synced to the tempo, through smp_dsfx_test (mono in, stereo out)."""
import ctypes

import numpy as np

RATE = 48000


def run(lib, desc, x, bpm=120.0, sets=""):
    fp = ctypes.POINTER(ctypes.c_float)
    lib.smp_dsfx_test.argtypes = [ctypes.c_char_p, fp, fp, fp, ctypes.c_int, ctypes.c_double, ctypes.c_double,
                                  ctypes.c_char_p]
    lib.smp_dsfx_test.restype = ctypes.c_int
    x = np.ascontiguousarray(x, dtype=np.float32)
    l = np.zeros(len(x), np.float32)
    r = np.zeros(len(x), np.float32)
    assert lib.smp_dsfx_test(desc.encode(), x.ctypes.data_as(fp), l.ctypes.data_as(fp), r.ctypes.data_as(fp), len(x),
                             float(RATE), bpm, sets.encode()) == 0
    return l, r


def band(x, lo, hi):
    s = np.abs(np.fft.rfft(x)) ** 2
    f = np.fft.rfftfreq(len(x), 1 / RATE)
    return float(np.sum(s[(f >= lo) & (f < hi)]) + 1e-30)


def db(a, b):
    return 10 * np.log10(a / b)


def noise(n=RATE):
    return np.random.default_rng(5).standard_normal(n).astype(np.float32) * 0.1


def impulse(n):
    x = np.zeros(n, np.float32)
    x[0] = 1
    return x


def test_dsfx_filters(lib):
    x = noise()
    l, _ = run(lib, "lp freq=1000 q=0.7", x)
    assert db(band(l, 500 * 0, 500), band(l, 4000, 20000)) > 24 + db(band(x, 0, 500), band(x, 4000, 20000))
    l, _ = run(lib, "hp freq=850 q=0.7", x)
    assert db(band(l, 4000, 20000), band(l, 0, 100)) > 20 + db(band(x, 4000, 20000), band(x, 0, 100))
    t = np.sin(2 * np.pi * 10000 * np.arange(RATE) / RATE).astype(np.float32) * 0.5
    l, r = run(lib, "lp freq=22000 q=0.7", t)
    assert abs(20 * np.log10(np.std(l[1000:]) / np.std(t[1000:]))) < 0.5
    assert np.allclose(l, r)                                     # a filter alone keeps mono mono


def test_dsfx_reverb(lib):
    x = impulse(RATE * 2)
    l, r = run(lib, "reverb room=0.9 damp=0.1 wet=0", x)
    assert np.array_equal(l, x) and np.array_equal(r, x)        # wet 0: dry only
    l, _ = run(lib, "reverb room=0.9 damp=0.1 wet=0.5", x)
    tail = np.sqrt(np.mean(l[int(0.5 * RATE):int(0.55 * RATE)] ** 2))
    assert 20 * np.log10(tail + 1e-30) > -60, tail
    big, _ = run(lib, "reverb room=0.9 damp=0.1 wet=0.5", x)
    small, _ = run(lib, "reverb room=0.2 damp=0.1 wet=0.5", x)

    def decay(y):
        a = np.sqrt(np.mean(y[int(0.2 * RATE):int(0.25 * RATE)] ** 2))
        b = np.sqrt(np.mean(y[int(1.0 * RATE):int(1.05 * RATE)] ** 2)) + 1e-30
        return 20 * np.log10(a / b)
    assert decay(big) < decay(small)                             # a bigger room rings longer


def echoes(y, n=3, gap=0.05):
    """the first n peaks after the impulse, each at least gap seconds after the one before"""
    out, start = [], int(0.01 * RATE)
    y = np.abs(y)
    for _ in range(n):
        i = start + int(np.argmax(y[start:]))
        out.append((i, float(y[i])))
        y = y.copy()
        y[max(0, i - int(gap * RATE)):i + int(gap * RATE)] = 0
        start = i + 1
    return sorted(out)


def test_dsfx_delay_musical(lib):
    x = impulse(int(RATE * 2.5))
    l, r = run(lib, "delay time=13 musical=1 feedback=0.5 offset=0 wet=1", x, 120)
    e = echoes(l, 2)
    assert abs(e[0][0] - 0.5 * RATE) <= 1 and abs(e[1][0] - 1.0 * RATE) <= 1, e
    assert abs(20 * np.log10(e[1][1] / e[0][1]) - 20 * np.log10(0.5)) < 0.5, e
    l, _ = run(lib, "delay time=13 musical=1 feedback=0.5 offset=0 wet=1", x, 90)
    assert abs(echoes(l, 1)[0][0] - RATE * 60 / 90) <= 1
    l, _ = run(lib, "delay time=10 musical=1 feedback=0 offset=0 wet=1", x, 120)
    assert abs(echoes(l, 1)[0][0] - 0.25 * RATE) <= 1


def test_dsfx_delay_offset_and_bypass(lib):
    x = impulse(RATE)
    l, r = run(lib, "delay time=0.3 musical=0 feedback=0 offset=0.01 wet=1", x)
    li, ri = echoes(l, 1)[0][0], echoes(r, 1)[0][0]
    assert abs((ri - li) - 0.005 * RATE) <= 1 and abs(ri - 0.3 * RATE) <= 1, (li, ri)
    y = noise()
    l, r = run(lib, "lp freq=500 q=0.7\ndelay time=0.3 musical=0 feedback=0.5 offset=0 wet=1\nreverb room=0.5 damp=0.5 wet=1",
               y, sets="effects 0")
    f, _ = run(lib, "lp freq=500 q=0.7", y)
    assert np.allclose(l, f, atol=1e-6) and np.allclose(r, f, atol=1e-6)   # off: the filter stays, the rest is bypassed


def test_dsfx_sets(lib):
    """sf_chain_set moves a parameter: the lowpass at 22000 Hz set to 300 Hz cuts 5 kHz."""
    x = noise()
    l, _ = run(lib, "lp freq=22000 q=0.7", x, sets="0 freq 300")
    assert db(band(x, 4000, 8000), band(l, 4000, 8000)) > 30
