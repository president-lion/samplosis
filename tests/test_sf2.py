import ctypes
import os
import random
import time

import numpy as np
import pytest

from conftest import SmpSfSample, write_wav
from sf2make import write_sf2

REAL_FONT = r"E:\samplepacks\soundfonts\just_t4_fixed.sf2"


def tone(n, freq=440.0, rate=22050):
    t = np.arange(n) / rate
    return (12000 * np.sin(2 * np.pi * freq * t)).astype(np.int16)


SAMPLES = [
    dict(name="Piano C4", data=tone(300), rate=22050, pitch=60, correction=-7, loop=(100, 280)),
    dict(name="Guitar A3", data=tone(250, 220.0), rate=44100, pitch=57, correction=12, loop=None),
    dict(name="Kick", data=tone(120, 60.0), rate=32000, pitch=36, correction=0, loop=None),
]


def zone(sample, keys=(0, 127), vels=(0, 127), gens=None, mods=None):
    return dict(keys=keys, vels=vels, sample=sample, gens=gens or {}, mods=mods or [])


PRESETS = [
    # written out of order on purpose: the font lists them sorted
    dict(name="Standard", bank=128, program=0, gens={}, pmods=[],
         instruments=[dict(name="Kit", gens={}, mods=[], zones=[zone(2, keys=(36, 36))])]),
    dict(name="Guitar", bank=0, program=25, gens={51: -12}, pmods=[(2, 48, 100, 0, 0)],
         instruments=[dict(name="Nylon", gens={17: 100}, mods=[(0x0502, 8, -2400, 0, 0)],
                           zones=[zone(1, keys=(0, 63)), zone(1, keys=(64, 127), gens={51: 12})])]),
    dict(name="Piano", bank=0, program=0, gens={}, pmods=[],
         instruments=[dict(name="Grand", gens={}, mods=[], zones=[zone(0, vels=(1, 100)), zone(0, vels=(101, 127))])]),
]


def make_font(path, sf3=False):
    write_sf2(str(path), PRESETS, SAMPLES, sf3=sf3)
    return str(path)


def sf_open(lib, path):
    err = ctypes.create_string_buffer(512)
    f = lib.smp_sf_open(str(path).encode(), err, 512)
    return f, err.value.decode()


def presets(lib, f):
    out = []
    bank, prog, name = ctypes.c_int(), ctypes.c_int(), ctypes.create_string_buffer(64)
    for i in range(lib.smp_sf_preset_count(f)):
        assert lib.smp_sf_preset_info(f, i, ctypes.byref(bank), ctypes.byref(prog), name, 64)
        out.append((bank.value, prog.value, name.value.decode("latin-1")))
    return out


def test_open_lists_presets(lib, tmp_path):
    f, err = sf_open(lib, make_font(tmp_path / "t.sf2"))
    assert f, err
    try:
        assert presets(lib, f) == [(0, 0, "Piano"), (0, 25, "Guitar"), (128, 0, "Standard")]
        assert lib.smp_sf_find_preset(f, 0, 25) == 1
        assert lib.smp_sf_find_preset(f, 0, 0) == 0
        assert lib.smp_sf_find_preset(f, 128, 0) == 2
        assert lib.smp_sf_find_preset(f, 0, 99) == -1
        assert lib.smp_sf_preset_info(f, 3, None, None, None, 0) == 0
        assert lib.smp_sf_preset_info(f, -1, None, None, None, 0) == 0
    finally:
        lib.smp_sf_close(f)


def test_samples_listed(lib, tmp_path):
    f, err = sf_open(lib, make_font(tmp_path / "t.sf2"))
    assert f, err
    try:
        assert lib.smp_sf_sample_count(f) == 3
        assert not lib.smp_sf_sample(f, 3)
        assert not lib.smp_sf_sample(f, -1)
        pos = 0
        for i, s in enumerate(SAMPLES):
            h = lib.smp_sf_sample(f, i).contents
            n = len(s["data"])
            assert h.name.decode() == s["name"]
            assert (h.start, h.end) == (pos, pos + n)
            if s["loop"]:
                assert (h.loop_start, h.loop_end) == (pos + s["loop"][0], pos + s["loop"][1])
            assert (h.rate, h.pitch, h.correction) == (s["rate"], s["pitch"], s["correction"])
            assert h.type == 1
            pos += n + 46
    finally:
        lib.smp_sf_close(f)


def test_sf3_headers(lib, tmp_path):
    f, err = sf_open(lib, make_font(tmp_path / "t.sf3", sf3=True))
    assert f, err
    try:
        assert presets(lib, f) == [(0, 0, "Piano"), (0, 25, "Guitar"), (128, 0, "Standard")]
        prev_end = 0
        for i in range(3):
            h = lib.smp_sf_sample(f, i).contents
            assert h.type & 0x10
            assert h.start == prev_end and h.end > h.start   # byte offsets of each Ogg stream
            prev_end = h.end
        assert lib.smp_sf_sample(f, 0).contents.loop_start == 100
    finally:
        lib.smp_sf_close(f)


def test_not_a_soundfont(lib, tmp_path):
    wav = tmp_path / "song.wav"
    write_wav(wav, np.zeros(1000, np.float32), 44100)
    empty = tmp_path / "empty.sf2"
    empty.write_bytes(b"")
    for path, why in ((wav, "isn't a SoundFont"), (empty, "isn't a SoundFont"),
                      (tmp_path / "missing.sf2", "can't open")):
        f, err = sf_open(lib, path)
        assert not f
        assert why in err, err
        assert os.path.basename(str(path)) in err, err


def test_bad_index_fails_plainly(lib, tmp_path):
    bad = [dict(PRESETS[2], instruments=[dict(name="Grand", gens={}, mods=[], zones=[zone(7)])])]
    path = tmp_path / "bad.sf2"
    write_sf2(str(path), bad, SAMPLES)
    f, err = sf_open(lib, path)
    assert not f
    assert "bad.sf2 isn't a SoundFont this can read (" in err, err


def walk_font(lib, f):
    """Reads everything the API offers about an opened font and checks every index is in range.
    Task 2 adds each preset's regions here."""
    n = lib.smp_sf_preset_count(f)
    ns = lib.smp_sf_sample_count(f)
    assert 0 <= n < 65536 and 0 <= ns < 65536
    bank, prog, name = ctypes.c_int(), ctypes.c_int(), ctypes.create_string_buffer(32)
    for i in range(n):
        assert lib.smp_sf_preset_info(f, i, ctypes.byref(bank), ctypes.byref(prog), name, 32)
        j = lib.smp_sf_find_preset(f, bank.value, prog.value)
        assert 0 <= j < n
    for i in range(ns):
        h = lib.smp_sf_sample(f, i).contents
        assert h.start <= h.end
        assert len(h.name) <= 20


def test_truncated_and_corrupt_files(lib, tmp_path):
    good = make_font(tmp_path / "good.sf2")
    data = open(good, "rb").read()
    variants = [data[:cut] for cut in range(0, len(data), 97)]
    rng = random.Random(1234)
    for _ in range(300):
        b = bytearray(data)
        for _ in range(8):
            b[rng.randrange(len(b))] = rng.randrange(256)
        variants.append(bytes(b))
    path = tmp_path / "bad.sf2"
    opened = 0
    for v in variants:
        path.write_bytes(v)
        f, err = sf_open(lib, path)
        if f:
            opened += 1
            walk_font(lib, f)
            lib.smp_sf_close(f)
        else:
            assert err
    assert opened > 0   # some changes only touch sample data or names


@pytest.mark.skipif(not os.path.exists(REAL_FONT), reason="the Tyros font isn't here")
def test_open_real_font_fast(lib):
    import psutil
    proc = psutil.Process()
    before = proc.memory_info().private
    t = time.perf_counter()
    f, err = sf_open(lib, REAL_FONT)
    took = time.perf_counter() - t
    assert f, err
    try:
        grew = proc.memory_info().private - before
        assert took < 0.5, took
        assert lib.smp_sf_preset_count(f) > 100
        assert grew < 20 * 1024 * 1024, grew
        walk_font(lib, f)
    finally:
        lib.smp_sf_close(f)
