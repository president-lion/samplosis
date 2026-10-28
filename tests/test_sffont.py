"""The font registry, preset use counts, the loader thread and sample data (sffont.c)."""
import ctypes
import os
import random
import struct
import threading
import time

import numpy as np
import pytest

from sf2make import write_sf2

REAL_FONT = r"E:\samplepacks\soundfonts\just_t4_fixed.sf2"
GRACE_MS = 2000   # the default grace before an unneeded sample is freed


def tone(n, freq=440.0, rate=22050, amp=12000):
    t = np.arange(n) / rate
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.int16)


def zone(sample, keys=(0, 127)):
    return dict(keys=keys, vels=(0, 127), sample=sample, gens={}, mods=[])


def preset(name, program, samples):
    zones = [zone(s, keys=(k * 10, k * 10 + 9)) for k, s in enumerate(samples)]
    return dict(name=name, bank=0, program=program, gens={}, pmods=[],
                instruments=[dict(name=name + " inst", gens={}, mods=[], zones=zones)])


# four samples; preset 1 (0:1) uses samples 1 and 2, the others one each
SAMPLES = [
    dict(name="S0", data=tone(400, 330.0), rate=22050, pitch=60, loop=(50, 350)),
    dict(name="S1", data=tone(3000, 440.0), rate=22050, pitch=60, loop=(1000, 2900)),
    dict(name="S2", data=tone(2500, 550.0), rate=22050, pitch=62, loop=None),
    dict(name="S3", data=tone(700, 660.0), rate=22050, pitch=64, loop=(10, 690)),
]
PRESETS = [preset("Two", 2, [3]), preset("Zero", 0, [0]), preset("One", 1, [1, 2])]


def sf_open(lib, path):
    err = ctypes.create_string_buffer(512)
    f = lib.smp_sf_open(str(path).encode(), err, 512)
    assert f, err.value.decode()
    return f


def pcm(lib, f, i):
    """The sample's loaded data as int16 (copied while held), its loop points, or None when not loaded."""
    n = ctypes.c_uint32()
    p = lib.smp_sf_pcm_acquire(f, i, ctypes.byref(n))
    if not p:
        return None
    try:
        data = np.ctypeslib.as_array(p, shape=(n.value,)).copy()
        ls, le = ctypes.c_uint32(), ctypes.c_uint32()
        assert lib.smp_sf_pcm_loop(f, i, ctypes.byref(ls), ctypes.byref(le))
        return data, (ls.value, le.value)
    finally:
        lib.smp_sf_pcm_release(f, i)


def poll(cond, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.02)
    return cond()


def test_samples_load_only_when_used(lib, tmp_path):
    path = str(tmp_path / "t.sf2")
    write_sf2(path, PRESETS, SAMPLES)
    f = sf_open(lib, path)
    try:
        assert lib.smp_sf_find_preset(f, 0, 1) == 1
        assert lib.smp_sf_loaded_samples(f) == 0
        assert lib.smp_sf_loaded_bytes(f) == 0
        assert lib.smp_sf_wait_idle(f, 1000) == 0
        lib.smp_sf_use(f, 1, 1)
        assert lib.smp_sf_wait_idle(f, 3000) == 0
        assert lib.smp_sf_loaded_samples(f) == 2
        assert lib.smp_sf_loaded_bytes(f) >= 2 * (3000 + 2500)
        assert pcm(lib, f, 0) is None and pcm(lib, f, 3) is None
        for i in (1, 2):
            data, loop = pcm(lib, f, i)
            assert np.array_equal(data, SAMPLES[i]["data"])
            assert loop == (SAMPLES[i]["loop"] or (0, 0))
        lib.smp_sf_use(f, 1, -1)
        time.sleep(0.5)
        assert lib.smp_sf_loaded_samples(f) == 2      # still inside the grace period
        assert poll(lambda: lib.smp_sf_loaded_samples(f) == 0, 4.0)
        assert lib.smp_sf_loaded_bytes(f) == 0
        assert pcm(lib, f, 1) is None
        # out-of-range presets and samples are ignored
        lib.smp_sf_use(f, 99, 1)
        lib.smp_sf_use(f, -1, 1)
        n = ctypes.c_uint32(7)
        assert not lib.smp_sf_pcm_acquire(f, 99, ctypes.byref(n))
        assert not lib.smp_sf_pcm_acquire(f, -1, ctypes.byref(n))
        assert lib.smp_sf_wait_idle(f, 1000) == 0
        assert lib.smp_sf_loaded_samples(f) == 0
    finally:
        lib.smp_sf_close(f)


def test_instances_share_samples(lib, tmp_path):
    path = str(tmp_path / "Shared.sf2")
    write_sf2(path, PRESETS, SAMPLES)
    other = str(tmp_path / "other.sf2")
    write_sf2(other, PRESETS, SAMPLES)
    lib.smp_sf_test_grace(10)
    f1 = sf_open(lib, path)
    f2 = sf_open(lib, path.upper().replace("\\", "/"))   # the same full path, compared case-insensitively
    f3 = sf_open(lib, other)
    try:
        assert f1 == f2
        assert f3 != f1
        lib.smp_sf_use(f1, 1, 1)
        assert lib.smp_sf_wait_idle(f1, 3000) == 0
        one = lib.smp_sf_loaded_bytes(f1)
        assert one > 0
        lib.smp_sf_use(f2, 1, 1)                       # the second synth on the same preset
        assert lib.smp_sf_wait_idle(f2, 3000) == 0
        assert lib.smp_sf_loaded_bytes(f2) == one      # one copy
        assert lib.smp_sf_loaded_samples(f3) == 0
        # the first synth lets go and closes; the second keeps playing
        lib.smp_sf_use(f1, 1, -1)
        lib.smp_sf_close(f1)
        time.sleep(0.3)                                 # well past the 10 ms grace
        assert lib.smp_sf_wait_idle(f2, 3000) == 0
        assert lib.smp_sf_loaded_samples(f2) == 2
        data, _ = pcm(lib, f2, 1)
        assert np.array_equal(data, SAMPLES[1]["data"])
        lib.smp_sf_use(f2, 1, -1)
        assert poll(lambda: lib.smp_sf_loaded_samples(f2) == 0, 2.0)
    finally:
        lib.smp_sf_close(f2)
        lib.smp_sf_close(f3)
        lib.smp_sf_test_grace(GRACE_MS)
    # after the last close, a new open is a new font that loads again
    f4 = sf_open(lib, path)
    try:
        assert lib.smp_sf_loaded_samples(f4) == 0
        lib.smp_sf_use(f4, 1, 1)
        assert lib.smp_sf_wait_idle(f4, 3000) == 0
        assert lib.smp_sf_loaded_samples(f4) == 2
        lib.smp_sf_use(f4, 1, -1)
    finally:
        lib.smp_sf_close(f4)


def test_sf3_matches_sf2(lib, tmp_path):
    samples = [dict(name="T%d" % i, data=tone(n, fr), rate=22050, pitch=60, loop=lp)
               for i, (n, fr, lp) in enumerate([(6000, 440.0, (1200, 5800)), (4000, 220.0, None),
                                                 (5000, 330.0, (17, 4999))])]
    presets = [preset("All", 0, [0, 1, 2])]
    a, b = str(tmp_path / "t.sf2"), str(tmp_path / "t.sf3")
    write_sf2(a, presets, samples)
    write_sf2(b, presets, samples, sf3=True)
    fa, fb = sf_open(lib, a), sf_open(lib, b)
    try:
        for f in (fa, fb):
            lib.smp_sf_use(f, 0, 1)
            assert lib.smp_sf_wait_idle(f, 5000) == 0
            assert lib.smp_sf_loaded_samples(f) == 3
        for i, s in enumerate(samples):
            da, la = pcm(lib, fa, i)
            db, lb = pcm(lib, fb, i)
            assert abs(len(da) - len(db)) <= 2048          # within one Ogg packet
            assert len(da) == len(s["data"])
            n = min(len(da), len(db))
            x, y = da[:n].astype(np.float64), db[:n].astype(np.float64)
            err_db = 20 * np.log10(np.sqrt(np.mean((x - y) ** 2)) / np.sqrt(np.mean(x ** 2)))
            assert err_db < -30, (i, err_db)
            want = s["loop"] or (0, 0)
            assert la == want and lb == want
        for f in (fa, fb):
            lib.smp_sf_use(f, 0, -1)
    finally:
        lib.smp_sf_close(fa)
        lib.smp_sf_close(fb)


def test_acquire_after_unpublish(lib, tmp_path):
    known = 0x1234
    samples = [dict(name="Known", data=np.full(2000, known, np.int16), rate=22050, pitch=60, loop=None)]
    path = str(tmp_path / "k.sf2")
    write_sf2(path, [preset("K", 0, [0])], samples)
    lib.smp_sf_test_grace(10)
    f = sf_open(lib, path)
    stop = threading.Event()
    stats = dict(hits=0, misses=0, bad=0)

    def hammer():
        n = ctypes.c_uint32()
        while not stop.is_set():
            p = lib.smp_sf_pcm_acquire(f, 0, ctypes.byref(n))
            if not p:
                stats["misses"] += 1
                continue
            if n.value != 2000 or p[0] != known or p[999] != known or p[1999] != known:
                stats["bad"] += 1
            stats["hits"] += 1
            lib.smp_sf_pcm_release(f, 0)

    t = threading.Thread(target=hammer)
    t.start()
    rnd = random.Random(3)
    try:
        end = time.time() + 2.0
        while time.time() < end:
            lib.smp_sf_use(f, 0, 1)
            time.sleep(rnd.uniform(0, 0.03))
            lib.smp_sf_use(f, 0, -1)
            time.sleep(rnd.uniform(0, 0.04))
    finally:
        stop.set()
        t.join()
    try:
        assert stats["bad"] == 0, stats
        assert stats["hits"] > 0 and stats["misses"] > 0, stats
        assert poll(lambda: lib.smp_sf_loaded_samples(f) == 0, 2.0)
    finally:
        lib.smp_sf_close(f)
        lib.smp_sf_test_grace(GRACE_MS)


def patch_shdr(path, i, start=None, end=None, loop_start=None, loop_end=None):
    """Rewrite fields of sample header i in place (bypassing sf2make's sane values)."""
    b = bytearray(open(path, "rb").read())
    at = b.index(b"shdr") + 8 + 46 * i + 20
    vals = list(struct.unpack_from("<IIII", b, at))
    for k, v in enumerate((start, end, loop_start, loop_end)):
        if v is not None:
            vals[k] = v
    struct.pack_into("<IIII", b, at, *vals)
    open(path, "wb").write(bytes(b))


def test_bad_sample_bounds_clamped(lib, tmp_path):
    samples = [dict(name="B%d" % i, data=tone(500 + 100 * i), rate=22050, pitch=60, loop=(50, 450))
               for i in range(5)]
    path = str(tmp_path / "bad.sf2")
    write_sf2(path, [preset("Bad", 0, [0, 1, 2, 3, 4])], samples)
    smpl_frames = None
    f = sf_open(lib, path)
    try:
        h = [lib.smp_sf_sample(f, i).contents for i in range(5)]
        starts = [x.start for x in h]
        smpl_frames = h[4].end + 46
    finally:
        lib.smp_sf_close(f)
    patch_shdr(path, 1, end=10_000_000)                                    # end past smpl
    patch_shdr(path, 2, loop_start=starts[2] + 5000, loop_end=0xFFFFFFF0)  # loop outside the sample
    patch_shdr(path, 3, end=starts[3])                                     # zero length
    patch_shdr(path, 4, loop_start=starts[4] + 300, loop_end=starts[4] + 100)  # loop backwards
    f = sf_open(lib, path)
    try:
        lib.smp_sf_use(f, 0, 1)
        assert lib.smp_sf_wait_idle(f, 3000) == 0
        assert lib.smp_sf_loaded_samples(f) == 4
        d0, l0 = pcm(lib, f, 0)
        assert len(d0) == 500 and l0 == (50, 450)
        d1, l1 = pcm(lib, f, 1)
        assert len(d1) == smpl_frames - starts[1]                       # clamped to the end of the data
        assert np.array_equal(d1[:600], samples[1]["data"])
        assert 0 <= l1[0] <= l1[1] <= len(d1)
        d2, l2 = pcm(lib, f, 2)
        assert len(d2) == 700 and 0 <= l2[0] <= l2[1] <= 700
        assert pcm(lib, f, 3) is None                                    # empty: never loaded
        d4, l4 = pcm(lib, f, 4)
        assert len(d4) == 900 and 0 <= l4[0] <= l4[1] <= 900
        # the broken sample isn't retried: another use stays idle and still has 4
        lib.smp_sf_use(f, 0, 1)
        assert lib.smp_sf_wait_idle(f, 3000) == 0
        assert lib.smp_sf_loaded_samples(f) == 4
        lib.smp_sf_use(f, 0, -2)
    finally:
        lib.smp_sf_close(f)


@pytest.mark.skipif(not os.path.exists(REAL_FONT), reason="the Tyros font isn't here")
def test_real_font_preset_memory(lib):
    f = sf_open(lib, REAL_FONT)
    try:
        p = lib.smp_sf_find_preset(f, 0, 0)
        assert p >= 0
        t0 = time.perf_counter()
        lib.smp_sf_use(f, p, 1)
        assert lib.smp_sf_wait_idle(f, 10000) == 0
        dt = time.perf_counter() - t0
        loaded = lib.smp_sf_loaded_bytes(f)
        assert lib.smp_sf_loaded_samples(f) > 0
        assert loaded < 60 * 1024 * 1024, loaded
        assert dt < 2.0, dt
        lib.smp_sf_use(f, p, -1)
    finally:
        lib.smp_sf_close(f)
