import ctypes
import os
import subprocess
import time

import numpy as np
import soundfile as sf

from conftest import write_wav
from test_cache import point_cache

FP = ctypes.POINTER(ctypes.c_float)


def sine(freq, secs, rate=44100):
    t = np.arange(int(secs * rate)) / rate
    return (0.5 * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def load(lib, path):
    err = ctypes.create_string_buffer(512)
    s = lib.smp_sample_load(str(path).encode(), err, 512)
    return s, err.value.decode()


def head(lib, entry, frames=None):
    n = ctypes.c_uint64()
    p = lib.smp_entry_head(entry, ctypes.byref(n))
    k = n.value if frames is None else min(frames, n.value)
    return np.ctypeslib.as_array(p, shape=(k * 2,)).copy().reshape(-1, 2)


def whole(lib, entry):
    n = lib.smp_entry_frames(entry)
    buf = np.zeros(n * 2, dtype=np.float32)
    assert lib.smp_entry_read(entry, 0, buf.ctypes.data_as(FP), n) == n
    return buf.reshape(-1, 2)


def test_load_wav_root_from_name(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    f = tmp_path / "Guitar C3.wav"
    write_wav(f, sine(220, 1.0), 44100)
    s, err = load(lib, f)
    assert s, err
    assert s.contents.root == 48 and s.contents.root_from == 0
    assert s.contents.frames == 44100 and s.contents.rate == 44100
    assert s.contents.name == b"Guitar C3.wav"
    lib.smp_sample_free(s)


def test_load_detects_root(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    f = tmp_path / "pluck.wav"
    write_wav(f, sine(196, 1.5), 44100)
    s, err = load(lib, f)
    assert s, err
    assert s.contents.root == 55 and s.contents.root_from == 1
    lib.smp_sample_free(s)
    before = lib.smp_debug_decodes()
    t = time.perf_counter()
    s, err = load(lib, f)
    assert time.perf_counter() - t < 0.02
    assert lib.smp_debug_decodes() == before
    assert s.contents.root == 55 and s.contents.root_from == 1
    lib.smp_sample_free(s)


def test_odd_files(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    # one frame
    f = tmp_path / "one.wav"
    write_wav(f, np.array([0.25], dtype=np.float32), 44100)
    s, err = load(lib, f)
    assert s, err
    assert s.contents.frames == 1 and s.contents.root_from == 2 and s.contents.root == 60
    lib.smp_sample_free(s)
    # mono 24 bit: both sides equal
    f = tmp_path / "m24.wav"
    sf.write(str(f), sine(330, 0.1, 48000), 48000, subtype="PCM_24")
    s, err = load(lib, f)
    assert s, err
    assert s.contents.rate == 48000
    h = head(lib, s.contents.fwd)
    assert len(h) == 4800 and np.array_equal(h[:, 0], h[:, 1]) and np.abs(h).max() > 0.4
    lib.smp_sample_free(s)
    # mp3
    wav = tmp_path / "src.wav"
    write_wav(wav, sine(440, 1.0), 44100)
    mp3 = tmp_path / "song.mp3"
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(wav), str(mp3)], check=True)
    s, err = load(lib, mp3)
    assert s, err
    assert s.contents.frames > 40000 and s.contents.rate == 44100
    lib.smp_sample_free(s)
    # silence
    f = tmp_path / "silent.wav"
    write_wav(f, np.zeros(44100, dtype=np.float32), 44100)
    s, err = load(lib, f)
    assert s.contents.root == 60 and s.contents.root_from == 2
    lib.smp_sample_free(s)
    # missing
    s, err = load(lib, tmp_path / "nothere.wav")
    assert not s and "nothere.wav" in err


def test_long_file_times(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    f = tmp_path / "long.wav"
    write_wav(f, sine(110, 600.0), 44100)
    t = time.perf_counter()
    s, err = load(lib, f)
    cold = time.perf_counter() - t
    assert s, err
    assert s.contents.frames == 600 * 44100
    lib.smp_sample_free(s)
    t = time.perf_counter()
    s, err = load(lib, f)
    warm = time.perf_counter() - t
    assert s
    lib.smp_sample_free(s)
    assert cold < 5 and warm < 0.05, (cold, warm)


def test_reversed(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    f = tmp_path / "ramp C4.wav"
    x = np.arange(100000, dtype=np.float32) / 100000
    write_wav(f, np.stack([x, -x], axis=1), 44100)
    s, err = load(lib, f)
    assert s, err
    r = lib.smp_sample_reversed(s)
    assert r
    assert lib.smp_entry_frames(r) == s.contents.frames
    assert np.array_equal(whole(lib, r), whole(lib, s.contents.fwd)[::-1])
    assert lib.smp_sample_reversed(s) == r
    lib.smp_sample_free(s)


def test_source_missing_plays_from_cache(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    f = tmp_path / "gone.wav"
    write_wav(f, sine(262, 0.5), 44100)
    s, err = load(lib, f)
    assert s, err
    key, root = s.contents.src_key, s.contents.root
    h1 = head(lib, s.contents.fwd)
    lib.smp_sample_free(s)
    os.remove(f)
    s, err = load(lib, f)
    assert not s
    e = ctypes.create_string_buffer(512)
    s = lib.smp_sample_load_known(str(f).encode(), key, root, e, 512)
    assert s, e.value
    assert s.contents.root == root and s.contents.frames == len(h1)
    assert np.array_equal(head(lib, s.contents.fwd), h1)
    lib.smp_sample_free(s)
