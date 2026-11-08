"""Decent Sampler bundles (.dslibrary): a zip of one or more .dspreset files and their samples. Opening one unpacks
it once into the cache folder (<cache>\\dslib\\<key>\\) and builds one font with a preset per .dspreset. Every
test writes its own bundle of generated samples into tmp_path, with the cache pointed into tmp_path too."""
import ctypes
import glob
import io
import os
import time
import zipfile

import numpy as np

from test_cache import point_cache
from test_ds import write_ds
from test_sf2 import peak_hz, presets, sf_open
from test_sfz import OUT, RATE, sine, write_wav

KIT = """<?xml version="1.0" encoding="UTF-8"?>
<DecentSampler minVersion="1.0.0">
  <groups><group>
    <sample path="Samples/%s" rootNote="60" loNote="%d" hiNote="%d"/>
  </group></groups>
</DecentSampler>
"""


def wav_bytes(tmp_path, freq, name="w.wav"):
    p = tmp_path / ("_" + name)
    write_wav(p, sine(RATE // 2, freq))
    return p.read_bytes()


def make_bundle(path, files, compression=zipfile.ZIP_DEFLATED):
    """files: entry name -> bytes or str"""
    with zipfile.ZipFile(str(path), "w", compression) as z:
        for name, data in files.items():
            z.writestr(name, data.encode() if isinstance(data, str) else data)
    return str(path)


def kit(tmp_path, name="Kit.dslibrary", compression=zipfile.ZIP_DEFLATED, extra=None):
    files = {"Kit.dspreset": KIT % ("a.wav", 60, 60), "Samples/a.wav": wav_bytes(tmp_path, 440.0)}
    files.update(extra or {})
    return make_bundle(tmp_path / name, files, compression)


def play(lib, f, preset, key, n=24000):
    out = (ctypes.c_float * (2 * n))()
    assert lib.smp_sf_test_note(f, preset, key, 100, float(OUT), n, -1, out) >= 1
    return np.frombuffer(out, dtype=np.float32).reshape(n, 2)[:, 0].copy()


def unpack_dirs(tmp_path):
    return sorted(d for d in glob.glob(str(tmp_path / "cache" / "dslib" / "*")) if os.path.isdir(d))


def files_under(d):
    out = []
    for root, _, names in os.walk(d):
        for n in names:
            out.append(os.path.relpath(os.path.join(root, n), d))
    return sorted(out)


def used_bytes(lib):
    lib.smp_cache_used_bytes.restype = ctypes.c_uint64
    return lib.smp_cache_used_bytes()


def test_dslib_opens_deflated_and_stored(lib, tmp_path):
    for comp, nm in ((zipfile.ZIP_DEFLATED, "d.dslibrary"), (zipfile.ZIP_STORED, "s.dslibrary")):
        point_cache(lib, tmp_path / "cache")
        f, err = sf_open(lib, kit(tmp_path, nm, comp))
        assert f, err
        assert presets(lib, f) == [(0, 0, "Kit")]
        assert abs(peak_hz(play(lib, f, 0, 60)[2000:20000], OUT) - 440) < 2
        lib.smp_sf_close(f)


def test_dslib_skips_junk(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    path = kit(tmp_path, extra={"__MACOSX/._Kit.dspreset": b"\0\5\26\7junk", "Samples/._a.wav": b"\0\5\26\7junk",
                                "Samples/a.wav.reapeaks": b"peaks", "Images/bg.jpg": b"\xff\xd8\xff",
                                "Samples/.dsp_cache": b"cache"})
    f, err = sf_open(lib, path)
    assert f, err
    dirs = unpack_dirs(tmp_path)
    assert len(dirs) == 1
    assert files_under(dirs[0]) == ["Kit.dspreset", os.path.join("Samples", "a.wav")]
    lib.smp_sf_close(f)


def test_dslib_several_presets(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    path = make_bundle(tmp_path / "Two.dslibrary", {
        "Presets/B.dspreset": KIT.replace("Samples/", "../Samples/") % ("b.wav", 62, 62),
        "Presets/a.dspreset": KIT.replace("Samples/", "../Samples/") % ("a.wav", 60, 60),
        "Samples/a.wav": wav_bytes(tmp_path, 440.0, "a.wav"),
        "Samples/b.wav": wav_bytes(tmp_path, 660.0, "b.wav"),
    })
    f, err = sf_open(lib, path)
    assert f, err
    assert presets(lib, f) == [(0, 0, "a"), (0, 1, "B")]
    assert abs(peak_hz(play(lib, f, 0, 60)[2000:20000], OUT) - 440) < 2
    assert abs(peak_hz(play(lib, f, 1, 62)[2000:20000], OUT) - 660 * 2 ** (2 / 12)) < 2   # root 60, played at 62
    lib.smp_sf_close(f)


def test_dslib_reopen_and_change(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    path = kit(tmp_path)
    f, err = sf_open(lib, path)
    assert f, err
    lib.smp_sf_close(f)
    d1 = unpack_dirs(tmp_path)
    assert len(d1) == 1
    w = os.path.join(d1[0], "Samples", "a.wav")
    m1 = os.stat(w).st_mtime_ns
    time.sleep(0.05)
    f, err = sf_open(lib, path)
    assert f, err
    lib.smp_sf_close(f)
    assert unpack_dirs(tmp_path) == d1 and os.stat(w).st_mtime_ns == m1   # not unpacked again
    time.sleep(0.05)
    kit(tmp_path)                                                         # the bundle rewritten: a new mtime
    os.utime(path, (time.time() + 5, time.time() + 5))
    f, err = sf_open(lib, path)
    assert f, err
    lib.smp_sf_close(f)
    assert len(unpack_dirs(tmp_path)) == 2


def test_dslib_bad(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    p = tmp_path / "noise.dslibrary"
    p.write_bytes(os.urandom(5000))
    f, err = sf_open(lib, p)
    assert not f and "isn't a zip file" in err, err
    good = open(kit(tmp_path), "rb").read()
    p = tmp_path / "cut.dslibrary"
    p.write_bytes(good[:len(good) // 2])
    f, err = sf_open(lib, p)
    assert not f and "its zip is damaged" in err, err
    f, err = sf_open(lib, make_bundle(tmp_path / "evil.dslibrary", {"Kit.dspreset": KIT % ("a.wav", 60, 60),
                                                                    "../evil.wav": wav_bytes(tmp_path, 440.0)}))
    assert not f and "it holds an unsafe path" in err, err
    f, err = sf_open(lib, make_bundle(tmp_path / "none.dslibrary", {"Samples/a.wav": wav_bytes(tmp_path, 440.0)}))
    assert not f and "it has no .dspreset in it" in err, err


def test_dslib_cache_counts_and_trims(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    before = used_bytes(lib)
    f, err = sf_open(lib, kit(tmp_path))
    assert f, err
    unpacked = sum(os.path.getsize(os.path.join(unpack_dirs(tmp_path)[0], n)) for n in files_under(unpack_dirs(tmp_path)[0]))
    assert used_bytes(lib) >= before + unpacked
    lib.smp_cache_trim(1)
    assert len(unpack_dirs(tmp_path)) == 1                               # open: kept
    lib.smp_sf_close(f)
    lib.smp_cache_trim(1)
    assert unpack_dirs(tmp_path) == []
