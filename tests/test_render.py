"""Task 6: keep length. Rubber Band R3 copies per key, rendered in the background."""
import ctypes
import math
import time

import numpy as np
import soundfile as sf

from conftest import SmpSample, write_wav
from test_cache import point_cache
from test_engine import (FP, KEEP_LENGTH, MODE, NOTE_HI, NOTE_LO, SPEED, detect, midi, play_live)

SP = ctypes.POINTER(SmpSample)


def setup(lib):
    vp, c_int, c_dbl = ctypes.c_void_p, ctypes.c_int, ctypes.c_double
    lib.smp_render_shift.restype = c_int
    lib.smp_render_shift.argtypes = [SP, c_int, c_int, c_dbl]
    lib.smp_engine_render_status.restype = c_int
    lib.smp_engine_render_status.argtypes = [vp, ctypes.POINTER(c_int), ctypes.POINTER(c_int), ctypes.c_char_p, c_int]
    lib.smp_engine_wait_renders.restype = c_int
    lib.smp_engine_wait_renders.argtypes = [vp, c_int]
    lib.smp_debug_renders.restype = ctypes.c_long
    lib.smp_debug_renders.argtypes = []


def pluck(freq, secs, rate=44100, seed=1):
    """Karplus-Strong plucked string."""
    rng = np.random.default_rng(seed)
    n = int(round(secs * rate))
    period = int(round(rate / freq))
    buf = rng.uniform(-1, 1, period)
    out = np.zeros(n)
    for i in range(n):
        j = i % period
        out[i] = buf[j]
        buf[j] = 0.996 * 0.5 * (buf[j] + buf[(j + 1) % period])
    return (0.5 * out / np.max(np.abs(out))).astype(np.float32)


def decaying(freq, secs, rate=44100, tau=0.35):
    t = np.arange(int(round(secs * rate))) / rate
    x = sum(a * np.sin(2 * np.pi * freq * h * t) for h, a in ((1, 1.0), (2, 0.5), (3, 0.25), (4, 0.12)))
    env = np.exp(-t / tau) * np.minimum(1, t / 0.002)
    return (0.4 * x * env).astype(np.float32)


def env_db(x, rate, win=0.01):
    n = int(win * rate)
    m = len(x) // n
    r = np.sqrt(np.mean(np.square(x[:m * n].astype(np.float64)).reshape(m, n), axis=1))
    return 20 * np.log10(r + 1e-12), n


def time_to_minus40(x, rate):
    db, n = env_db(x, rate)
    p = int(np.argmax(db))
    below = np.nonzero(db[p:] < db[p] - 40)[0]
    assert len(below), "never fell 40 dB"
    return below[0] * n / rate


def length_secs(x, rate, floor=1e-6):
    nz = np.nonzero(np.abs(x) > floor)[0]
    return (nz[-1] + 1) / rate if len(nz) else 0.0


def load(lib, tmp_path, data, name, rate=44100):
    f = tmp_path / name
    write_wav(f, data, rate)
    err = ctypes.create_string_buffer(512)
    s = lib.smp_sample_load(str(f).encode(), err, 512)
    assert s, err.value
    return s


class KL:
    """An engine with keep length on and a key range limited to what the test plays (fewer renders)."""

    def __init__(self, lib, tmp_path, s, lo, hi, host_rate=48000, speed=None, set_first=True):
        self.lib, self.tmp, self.host_rate, self.n = lib, tmp_path, host_rate, 0
        self.e = lib.smp_engine_new(host_rate, 512)
        assert self.e
        self.set(NOTE_LO, lo); self.set(NOTE_HI, hi)
        if speed is not None:
            self.set(SPEED, speed)
        assert lib.smp_engine_get_param(self.e, KEEP_LENGTH) == 1   # on by default
        if set_first:
            lib.smp_engine_set_sample(self.e, s)

    def set(self, pid, v):
        self.lib.smp_engine_set_param(self.e, pid, v)

    def status(self):
        ready, total = ctypes.c_int(), ctypes.c_int()
        why = ctypes.create_string_buffer(512)
        r = self.lib.smp_engine_render_status(self.e, ctypes.byref(ready), ctypes.byref(total), why, 512)
        return r, ready.value, total.value, why.value.decode()

    def wait(self, ms=120000):
        assert self.lib.smp_engine_wait_renders(self.e, ms) == 0

    def render(self, events, secs, block=256):
        self.n += 1
        out = self.tmp / f"kl{self.n}.wav"
        frames = int(round(secs * self.host_rate))
        assert self.lib.smp_engine_render_test(self.e, events.encode(), frames, block, str(out).encode()) == 0
        y, _ = sf.read(str(out), dtype="float32")
        return y[:, 0]

    def free(self):
        self.lib.smp_engine_free(self.e)


def read_entry(lib, key):
    ent = lib.smp_cache_open(key)
    assert ent, hex(key)
    n = lib.smp_entry_frames(ent)
    buf = np.zeros(2 * n, dtype=np.float32)
    assert lib.smp_entry_read(ent, 0, buf.ctypes.data_as(FP), n) == n
    lib.smp_cache_close(ent)
    return buf[0::2]


def kl_key(lib, base, semis, speed_percent=100.0):
    return lib.smp_key(f"kl|{base:016x}|{semis}|{speed_percent:.2f}|r3".encode())


def test_render_shift(lib, tmp_path):
    setup(lib)
    point_cache(lib, tmp_path / "cache")
    rate = 44100
    data = pluck(196, 2.0, rate)
    s = load(lib, tmp_path, data, "pluck G3.wav")
    t0 = time.perf_counter()
    assert lib.smp_render_shift(s, 0, 7, 1.0) == 0
    took = time.perf_counter() - t0
    y = read_entry(lib, kl_key(lib, s.contents.src_key, 7))
    assert len(y) == len(data)
    assert abs(detect(lib, y, rate) - midi(293.66)) < 0.15
    a, n = env_db(data, rate)
    b, _ = env_db(y, rate)
    for at in (0.5, 1.0, 1.5):
        i = int(at * rate) // n
        assert abs(a[i] - b[i]) < 2, (at, a[i], b[i])
    print(f"render +7 of 2 s: {took:.3f} s")
    lib.smp_sample_free(s)


def test_render_shift_speed_and_reverse(lib, tmp_path):
    setup(lib)
    point_cache(lib, tmp_path / "cache")
    rate = 44100
    data = pluck(196, 1.0, rate)
    s = load(lib, tmp_path, data, "pluck G3.wav")
    assert lib.smp_render_shift(s, 0, 0, 0.5) == 0
    y = read_entry(lib, kl_key(lib, s.contents.src_key, 0, 50.0))
    assert len(y) == 2 * len(data)
    assert abs(detect(lib, y, rate) - midi(196)) < 0.15
    assert lib.smp_render_shift(s, 1, -5, 1.0) == 0
    assert s.contents.rev_key
    y = read_entry(lib, kl_key(lib, s.contents.rev_key, -5))
    assert len(y) == len(data)
    assert lib.smp_render_shift(s, 0, 49, 1.0) != 0      # past 4 octaves: refused (Rubber Band throws at +67)
    lib.smp_sample_free(s)


def test_chord_notes_end_together(lib, tmp_path):
    setup(lib)
    point_cache(lib, tmp_path / "cache")
    s = load(lib, tmp_path, decaying(261.63, 2.0), "tone C4.wav")
    e = KL(lib, tmp_path, s, 60, 67)
    e.wait()
    r, ready, total, why = e.status()
    assert r == 1 and ready == total == 8 and why == "", (r, ready, total, why)
    on = {k: time_to_minus40(e.render(f"on {k} 127 0", 2.5), e.host_rate) for k in (60, 64, 67)}
    assert max(on.values()) <= min(on.values()) * 1.03, on
    chord = e.render("on 60 127 0\non 64 127 0\non 67 127 0", 2.5)
    assert length_secs(chord, e.host_rate) > 1.9       # every note lasts the whole sample
    e.set(KEEP_LENGTH, 0)
    off = {k: time_to_minus40(e.render(f"on {k} 127 0", 2.5), e.host_rate) for k in (60, 64, 67)}
    assert off[67] <= off[60] * 0.85, off
    print("keep length on", on, "off", off)
    e.free()


def test_note_before_render_uses_resample(lib, tmp_path):
    setup(lib)
    point_cache(lib, tmp_path / "cache")             # a fresh, empty cache
    rate = 44100
    data = decaying(220, 2.0, rate, tau=5.0)          # A3, barely decaying
    s = load(lib, tmp_path, data, "tone A3.wav")
    e = KL(lib, tmp_path, s, 57, 69)
    lib.smp_engine_note_on(e.e, 69, 127, 0)          # +12 at once: no copy yet
    y = play_live(lib, e, int(1.5 * e.host_rate / 256), keep=int(1.5 * e.host_rate / 256))
    first = np.nonzero(np.abs(y) > 1e-4)[0][0]
    assert first < 0.005 * e.host_rate, first
    assert abs(detect(lib, y[int(0.05 * e.host_rate):int(0.5 * e.host_rate)], e.host_rate) - midi(440)) < 0.1
    assert abs(length_secs(y, e.host_rate) - 1.0) < 0.02   # resampled: half the length, for the whole note
    e.wait()
    y = e.render("on 69 127 0", 2.5)
    assert abs(detect(lib, y[int(0.05 * e.host_rate):int(0.5 * e.host_rate)], e.host_rate) - midi(440)) < 0.15
    copy = read_entry(lib, kl_key(lib, s.contents.src_key, 12))
    assert len(copy) == len(data)
    assert abs(length_secs(y, e.host_rate, 1e-4) - len(copy) / rate) < 0.02
    e.free()


def test_renders_cached(lib, tmp_path):
    setup(lib)
    point_cache(lib, tmp_path / "cache")
    f = tmp_path / "tone C4.wav"
    write_wav(f, decaying(261.63, 1.5), 44100)
    err = ctypes.create_string_buffer(512)
    e1 = KL(lib, tmp_path, lib.smp_sample_load(str(f).encode(), err, 512), 58, 63)
    e1.wait()
    assert e1.status()[1:3] == (6, 6)
    e1.free()
    runs = lib.smp_debug_renders()
    e2 = KL(lib, tmp_path, lib.smp_sample_load(str(f).encode(), err, 512), 58, 63)
    t0 = time.perf_counter()
    e2.wait(5000)
    assert time.perf_counter() - t0 < 1.0
    assert e2.status()[:3] == (1, 6, 6)
    assert lib.smp_debug_renders() == runs           # no renders ran
    e2.free()


def test_bad_cache_dir_falls_back_engine(lib, tmp_path):
    setup(lib)
    point_cache(lib, tmp_path / "cache")
    s = load(lib, tmp_path, decaying(220, 2.0, tau=5.0), "tone A3.wav")
    point_cache(lib, "Q:\\nope")
    e = KL(lib, tmp_path, s, 57, 69)
    e.wait(5000)
    r, ready, total, why = e.status()
    assert r == 0 and "Q:\\nope" in why, (r, ready, total, why)
    y = e.render("on 69 127 0", 2.5)
    assert abs(detect(lib, y[int(0.05 * e.host_rate):int(0.5 * e.host_rate)], e.host_rate) - midi(440)) < 0.1
    assert abs(length_secs(y, e.host_rate) - 1.0) < 0.02    # resampled
    e.free()
    point_cache(lib, tmp_path / "cache")


def test_speed_with_keep_length(lib, tmp_path):
    setup(lib)
    point_cache(lib, tmp_path / "cache")
    data = decaying(220, 1.0, tau=5.0)
    s = load(lib, tmp_path, data, "tone A3.wav")
    e = KL(lib, tmp_path, s, 57, 62, speed=50)
    e.wait()
    for k, f in ((57, 220.0), (62, 220.0 * 2 ** (5 / 12))):
        y = e.render(f"on {k} 127 0", 3.0)
        assert abs(length_secs(y, e.host_rate) - 2.0) < 0.03, k
        assert abs(detect(lib, y[int(0.1 * e.host_rate):int(1.0 * e.host_rate)], e.host_rate) - midi(f)) < 0.15, k
    e.set(SPEED, 100)                                  # back to 100 %: renders again (or from the cache)
    e.wait()
    y = e.render("on 62 127 0", 2.0)
    assert abs(length_secs(y, e.host_rate) - 1.0) < 0.02
    e.free()


def test_start_past_head_uses_copy(lib, tmp_path):
    setup(lib)
    point_cache(lib, tmp_path / "cache")
    data = decaying(220, 2.0, tau=5.0)
    s = load(lib, tmp_path, data, "tone A3.wav")
    e = KL(lib, tmp_path, s, 57, 62)
    e.set(20, 50)                                      # start at 50 %: past every copy's 0.25 s head
    e.wait()
    y = e.render("on 62 127 0", 1.5)                   # the copy's start head is built for this note
    assert abs(length_secs(y, e.host_rate) - 1.0) < 0.02   # the copy's second half, not a resampled 0.75 s
    assert abs(detect(lib, y[int(0.05 * e.host_rate):int(0.5 * e.host_rate)], e.host_rate) - midi(220 * 2 ** (5 / 12))) < 0.15
    assert lib.smp_engine_underruns(e.e) == 0
    e.free()


def test_free_while_rendering(lib, tmp_path):
    setup(lib)
    point_cache(lib, tmp_path / "cache")
    s = load(lib, tmp_path, decaying(220, 30.0, tau=5.0), "long A3.wav")
    e = KL(lib, tmp_path, s, 50, 70)
    time.sleep(0.5)                                    # a 30 s render is under way
    r, ready, total, why = e.status()
    assert ready < total, (ready, total)
    t0 = time.perf_counter()
    e.free()                                           # cancels it between chunks
    assert time.perf_counter() - t0 < 0.5


def smpc_files(folder):
    import os
    return {int(f[:16], 16): os.path.getsize(os.path.join(folder, f))
            for f in os.listdir(folder) if f.endswith(".smpc")}


def test_cache_limit_enforced(lib, tmp_path):
    setup(lib)
    cache = tmp_path / "cache"
    point_cache(lib, cache, gb=0.002)                   # about 2 MB; one 2 s copy is 0.7 MB
    data = decaying(261.63, 2.0, tau=5.0)
    s = load(lib, tmp_path, data, "tone C4.wav")
    src_key = s.contents.src_key
    e = KL(lib, tmp_path, s, 58, 63)
    e.wait()
    for speed in (50, 75):                              # each speed makes a new set; the old set is retired
        e.set(SPEED, speed)
        e.render("on 60 127 0", 0.1)                    # the audio thread moves on, so retired copies close
        e.wait()
    e.render("on 60 127 0", 0.1)
    time.sleep(0.2)
    e.set(NOTE_HI, 64)                                  # one more copy: a trim with only the current set open
    e.wait()
    files = smpc_files(cache)
    open_keys = {src_key} | {kl_key(lib, src_key, n, 75.0) for n in range(-2, 5)}
    limit = 0.002 * 1024 ** 3
    assert sum(files.values()) <= limit + sum(v for k, v in files.items() if k in open_keys), files
    assert sum(1 for n in range(-2, 4) if kl_key(lib, src_key, n, 100.0) in files) < 5   # old copies were trimmed
    r, ready, total, why = e.status()
    assert (r, ready, total) == (1, 7, 7), (r, ready, total, why)
    e.set(SPEED, 100)                                   # renders again where trimmed
    e.wait()
    for k in range(58, 65):
        y = e.render(f"on {k} 127 0", 0.6)
        f = 261.63 * 2 ** ((k - 60) / 12)
        assert abs(detect(lib, y[int(0.05 * e.host_rate):int(0.5 * e.host_rate)], e.host_rate) - midi(f)) < 0.15, k
    e.free()
    point_cache(lib, cache)


def test_tiny_samples(lib, tmp_path):
    setup(lib)
    point_cache(lib, tmp_path / "cache")
    for rate in (22050, 96000):
        for frames in (1, 3):
            s = load(lib, tmp_path, np.array([0.3, -0.2, 0.1][:frames], np.float32), f"tiny{frames}_{rate} C4.wav", rate)
            for semis in (12, -12):
                r = lib.smp_render_shift(s, 0, semis, 1.0)
                if r == 0:
                    assert len(read_entry(lib, kl_key(lib, s.contents.src_key, semis))) == frames
            lib.smp_sample_free(s)


def test_rubber_band_exceptions_caught(lib, tmp_path):
    setup(lib)
    lib.smp_debug_render_flags.restype = None
    lib.smp_debug_render_flags.argtypes = [ctypes.c_long]
    point_cache(lib, tmp_path / "cache")
    s = load(lib, tmp_path, pluck(196, 0.1), "short G3.wav")
    lib.smp_debug_render_flags(2)                       # past the 4-octave limit: Rubber Band itself throws at -127
    try:
        assert lib.smp_render_shift(s, 0, -127, 1.0) == 1
    finally:
        lib.smp_debug_render_flags(0)
    lib.smp_sample_free(s)
    data = decaying(220, 1.0, tau=5.0)
    s = load(lib, tmp_path, data, "tone A3.wav")
    lib.smp_debug_render_flags(1)                       # every render throws inside the guard
    try:
        assert lib.smp_render_shift(s, 0, 5, 1.0) == 1
        e = KL(lib, tmp_path, s, 57, 62)
        e.wait()
        r, ready, total, why = e.status()
        assert r == 0 and ready == 1 and total == 6 and "Rubber Band failed" in why, (r, ready, total, why)
        y = e.render("on 62 127 0", 1.5)                # that key resamples
        assert abs(length_secs(y, e.host_rate) - 1.0 / 2 ** (5 / 12)) < 0.02
        assert abs(detect(lib, y[int(0.05 * e.host_rate):int(0.5 * e.host_rate)], e.host_rate) - midi(220 * 2 ** (5 / 12))) < 0.1
        e.free()
    finally:
        lib.smp_debug_render_flags(0)


def test_too_long_refused(lib, tmp_path):
    setup(lib)
    point_cache(lib, tmp_path / "cache")
    rate = 8000                                         # few bytes on disk; the frame count is what matters
    s = load(lib, tmp_path, decaying(220, 900.0, rate, tau=5.0), "long A3.wav", rate)   # 7.2 M frames
    assert lib.smp_render_shift(s, 0, 0, 0.25) == 4     # 36 M frames of output: past 512 MB
    lib.smp_sample_free(s)
