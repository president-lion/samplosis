import ctypes
import math

import numpy as np
import pytest


def detect(lib, x, rate):
    x = np.ascontiguousarray(x, dtype=np.float32)
    return lib.smp_detect_pitch(x.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), len(x), rate)


def midi(f):
    return 69 + 12 * math.log2(f / 440)


@pytest.mark.parametrize("rate", [48000, 44100])
@pytest.mark.parametrize("f", [110, 220, 261.63, 1000])
def test_sine(lib, rate, f):
    t = np.arange(rate * 2) / rate
    assert abs(detect(lib, 0.5 * np.sin(2 * np.pi * f * t), rate) - midi(f)) < 0.1


def test_pluck(lib):
    rate, f = 44100, 196.0
    n = int(rate / f)
    rng = np.random.default_rng(1)
    buf = list(rng.uniform(-1, 1, n))
    out = np.zeros(rate * 2)
    # Karplus-Strong with a fractional-free delay line; the effective pitch is rate / (n + 0.5)
    for i in range(len(out)):
        out[i] = buf[i % n]
        buf[i % n] = 0.498 * (buf[i % n] + buf[(i + 1) % n])
    assert abs(detect(lib, out, rate) - midi(rate / (n + 0.5))) < 0.2


def test_noise_and_silence(lib):
    rng = np.random.default_rng(2)
    assert detect(lib, rng.uniform(-0.5, 0.5, 96000), 48000) == -1
    assert detect(lib, np.zeros(96000), 48000) == -1
    assert detect(lib, np.zeros(10), 48000) == -1
