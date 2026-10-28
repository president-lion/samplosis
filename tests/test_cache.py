import ctypes
import os

import numpy as np

from conftest import SmpSettings

FP = ctypes.POINTER(ctypes.c_float)


def get(lib):
    s = SmpSettings()
    lib.smp_settings_get(ctypes.byref(s))
    return s


def point_cache(lib, folder, gb=10.0):
    s = SmpSettings()
    s.cache_dir = str(folder).encode()
    s.cache_limit_gb = gb
    assert lib.smp_settings_set(ctypes.byref(s)) == 0


def ramp(frames):
    x = np.arange(frames * 2, dtype=np.float32) / 1e6
    return np.ascontiguousarray(x)


def write(lib, key, data, rate=44100):
    return lib.smp_cache_write(key, data.ctypes.data_as(FP), len(data) // 2, rate)


def test_settings_defaults_and_roundtrip(lib, settings_file, tmp_path):
    if os.path.exists(settings_file):
        os.remove(settings_file)
    s = get(lib)
    assert s.cache_dir == rb"D:\samplosis\cache"
    assert s.cache_limit_gb == 10
    point_cache(lib, tmp_path, 0.001)
    s = get(lib)
    assert s.cache_dir == str(tmp_path).encode()
    assert s.cache_limit_gb == 0.001
    text = open(settings_file).read().splitlines()
    assert text[0].startswith("cache_dir ")
    assert "cache_limit_gb 0.001" in text


def test_cache_write_open_read(lib, tmp_path):
    point_cache(lib, tmp_path)
    key = lib.smp_key(b"src|a.wav|1|2")
    assert key == lib.smp_key(b"src|a.wav|1|2") and key != lib.smp_key(b"src|a.wav|1|3")
    data = ramp(3 * 44100)
    assert write(lib, key, data) == 0
    assert os.path.exists(tmp_path / ("%016x.smpc" % key))
    assert not list(tmp_path.glob("*.part"))
    e = lib.smp_cache_open(key)
    assert e
    e2 = lib.smp_cache_open(key)  # same key twice
    assert e2
    try:
        assert lib.smp_entry_frames(e) == 3 * 44100
        assert lib.smp_entry_rate(e) == 44100
        assert lib.smp_entry_whole(e) == 0
        n = ctypes.c_uint64()
        head = lib.smp_entry_head(e, ctypes.byref(n))
        assert n.value == 11025
        assert np.array_equal(np.ctypeslib.as_array(head, (n.value * 2,)), data[:n.value * 2])
        out = np.zeros(2000, dtype=np.float32)
        assert lib.smp_entry_read(e2, 50000, out.ctypes.data_as(FP), 1000) == 1000
        assert np.array_equal(out, data[100000:102000])
        # a read past the end returns what is left
        assert lib.smp_entry_read(e, 3 * 44100 - 10, out.ctypes.data_as(FP), 1000) == 10
    finally:
        lib.smp_cache_close(e)
        lib.smp_cache_close(e2)
    assert lib.smp_cache_open(lib.smp_key(b"missing")) is None
    k2 = lib.smp_key(b"short")
    short = ramp(22050)
    assert write(lib, k2, short) == 0
    e = lib.smp_cache_open(k2)
    n = ctypes.c_uint64()
    head = lib.smp_entry_head(e, ctypes.byref(n))
    assert lib.smp_entry_whole(e) == 1 and n.value == 22050
    assert np.array_equal(np.ctypeslib.as_array(head, (n.value * 2,)), short)
    lib.smp_cache_close(e)


def test_cache_trim_lru(lib, tmp_path):
    point_cache(lib, tmp_path)
    frames = 131072  # 1 MB of float32 stereo
    keys = [lib.smp_key(b"trim%d" % i) for i in range(5)]
    for i, k in enumerate(keys):
        assert write(lib, k, ramp(frames)) == 0
        p = tmp_path / ("%016x.smpc" % k)
        os.utime(p, (1_000_000 + i * 100, 1_000_000 + i * 100))
    size = os.path.getsize(tmp_path / ("%016x.smpc" % keys[0]))
    # opening a file that is longer than 1 s keeps the oldest alive; whole ones get touched
    e = lib.smp_cache_open(keys[0])
    assert e
    t0 = os.path.getmtime(tmp_path / ("%016x.smpc" % keys[0]))
    assert t0 > 1_000_000 + 400  # touched, now the newest
    # restage so the opened one is the oldest by time
    for i, k in enumerate(keys):
        os.utime(tmp_path / ("%016x.smpc" % k), (1_000_000 + i * 100,) * 2)
    left = lib.smp_cache_trim(int(2.5 * size))
    names = {p.name for p in tmp_path.glob("*.smpc")}
    want = {"%016x.smpc" % keys[i] for i in (0, 3, 4)}
    assert names == want
    assert left == 3 * size
    lib.smp_cache_close(e)
    left = lib.smp_cache_trim(int(2.5 * size))
    assert left == 2 * size
    assert {p.name for p in tmp_path.glob("*.smpc")} == {"%016x.smpc" % keys[i] for i in (3, 4)}


def test_bad_cache_dir_falls_back(lib):
    point_cache(lib, r"Q:\nope")
    why = ctypes.create_string_buffer(256)
    assert lib.smp_cache_ok(why, 256) == 0
    assert rb"Q:\nope" in why.value
    assert write(lib, lib.smp_key(b"x"), ramp(100)) != 0


def test_rewrite_open_key_and_concurrent_writers(lib, tmp_path):
    import threading
    point_cache(lib, tmp_path)
    data = ramp(44100 * 2)
    k = lib.smp_key(b"dup")
    assert write(lib, k, data) == 0
    e = lib.smp_cache_open(k)
    assert write(lib, k, data) == 0
    out = np.zeros(2000, dtype=np.float32)
    assert lib.smp_entry_read(e, 30000, out.ctypes.data_as(FP), 1000) == 1000
    assert np.array_equal(out, data[60000:62000])
    lib.smp_cache_close(e)
    k2 = lib.smp_key(b"race")
    res = []
    ts = [threading.Thread(target=lambda: res.append(write(lib, k2, data))) for _ in range(4)]
    [t.start() for t in ts]
    [t.join() for t in ts]
    assert res == [0, 0, 0, 0]
    assert not list(tmp_path.glob("*.part"))


def test_cache_ok_concurrent(lib, tmp_path):
    # every instance checks the folder with a probe of its own: none sees another's probe as "can't be written"
    import threading
    point_cache(lib, tmp_path)
    res, go = [], threading.Barrier(8)

    def check():
        go.wait()
        for _ in range(50):
            res.append(lib.smp_cache_ok(None, 0))

    ts = [threading.Thread(target=check) for _ in range(8)]
    [t.start() for t in ts]
    [t.join() for t in ts]
    assert res == [1] * 400
    assert not [p for p in os.listdir(tmp_path) if p.startswith(".probe")]


def test_trim_deletes_stale_part_files(lib, tmp_path):
    import time
    point_cache(lib, tmp_path)
    old = tmp_path / ("%016x.1.2.3.part" % 7)
    new = tmp_path / ("%016x.1.2.4.part" % 8)
    old.write_bytes(b"x" * 100); new.write_bytes(b"x" * 100)
    t = time.time() - 2 * 3600
    os.utime(old, (t, t))
    lib.smp_cache_trim(10 * 1024 ** 3)
    assert not old.exists()                 # left by a crash: gone
    assert new.exists()                     # could be a write in progress: kept


def test_open_copy_holds_only_quarter_second(lib, tmp_path):
    lib.smp_cache_open_copy.restype = ctypes.c_void_p
    lib.smp_cache_open_copy.argtypes = [ctypes.c_uint64]
    point_cache(lib, tmp_path)
    k = lib.smp_key(b"copy 1 s")
    assert write(lib, k, ramp(44100)) == 0
    n = ctypes.c_uint64()
    e = lib.smp_cache_open(k)                # an imported file: whole up to 1 s
    assert lib.smp_entry_whole(e) == 1
    lib.smp_cache_close(e)
    e = lib.smp_cache_open_copy(k)           # a keep-length copy: just the head
    lib.smp_entry_head(e, ctypes.byref(n))
    assert lib.smp_entry_whole(e) == 0 and n.value == 11025
    lib.smp_cache_close(e)
    k2 = lib.smp_key(b"copy 0.2 s")
    assert write(lib, k2, ramp(8820)) == 0
    e = lib.smp_cache_open_copy(k2)
    assert lib.smp_entry_whole(e) == 1
    lib.smp_cache_close(e)
