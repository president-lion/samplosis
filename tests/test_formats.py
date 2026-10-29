import ctypes
import os
import shutil
import struct
import subprocess

import numpy as np
import pytest

from conftest import SmpSettings, write_wav
from test_cache import point_cache
from test_sample import load, whole

FFMPEG = shutil.which("ffmpeg")
need_ffmpeg = pytest.mark.skipif(not FFMPEG, reason="ffmpeg isn't installed")


def two_tones(frames, rate=44100):
    """A stereo test signal, a different tone on each side, frames x 2."""
    t = np.arange(frames) / rate
    return np.stack([0.5 * np.sin(2 * np.pi * 220 * t), 0.3 * np.sin(2 * np.pi * 331 * t)], axis=1).astype(np.float32)


def ffmpeg(*args):
    subprocess.run([FFMPEG, "-nostdin", "-hide_banner", "-loglevel", "error", "-y"] + [str(a) for a in args],
                   check=True, timeout=60)


def settings(lib, **kw):
    """The settings now, with kw changed, saved."""
    s = SmpSettings()
    lib.smp_settings_get(ctypes.byref(s))
    for k, v in kw.items():
        setattr(s, k, v.encode() if isinstance(v, str) else v)
    assert lib.smp_settings_set(ctypes.byref(s)) == 0
    return s


@need_ffmpeg
@pytest.mark.parametrize("codec", ["pcm_s16be", "pcm_s24le", "pcm_f32be", "pcm_f32le"])
def test_caf_pcm_and_float(lib, tmp_path, codec):
    point_cache(lib, tmp_path / "cache")
    src = two_tones(22050)
    w = tmp_path / "src.wav"
    write_wav(w, src, 44100)
    f = tmp_path / ("tone %s.caf" % codec)
    ffmpeg("-i", w, "-c:a", codec, f)
    assert f.read_bytes()[:4] == b"caff"
    s, err = load(lib, f)
    assert s, err
    try:
        assert s.contents.rate == 44100
        assert abs(int(s.contents.frames) - len(src)) <= 1
        got = whole(lib, s.contents.fwd)
        n = min(len(got), len(src))
        assert np.max(np.abs(got[:n] - src[:n])) < 1e-4
    finally:
        lib.smp_sample_free(s)


@need_ffmpeg
@pytest.mark.parametrize("ext,args", [(".w64", []), (".wav", ["-rf64", "always"]), (".aifc", ["-c:a", "pcm_f32be"])])
def test_wav_family_containers(lib, tmp_path, ext, args):
    # dr_wav reads Wave64, RF64 and AIFF-C already: these only confirm it
    point_cache(lib, tmp_path / "cache")
    src = two_tones(11025)
    w = tmp_path / "src.wav"
    write_wav(w, src, 44100)
    f = tmp_path / ("x" + ext)
    ffmpeg("-i", w, *args, *(["-f", "aiff"] if ext == ".aifc" else []), f)
    s, err = load(lib, f)
    assert s, err
    try:
        got = whole(lib, s.contents.fwd)
        assert abs(len(got) - len(src)) <= 1
        n = min(len(got), len(src))
        assert np.max(np.abs(got[:n] - src[:n])) < 1e-4
    finally:
        lib.smp_sample_free(s)


def test_raw_pcm_uses_settings(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    src = two_tones(4800, 48000)
    q = np.round(src * 8388607).astype(np.int32)                  # 24-bit, big-endian, interleaved
    b = bytearray()
    for v in q.reshape(-1):
        b += int(v & 0xFFFFFF).to_bytes(3, "big")
    f = tmp_path / "take.raw"
    f.write_bytes(bytes(b))
    s, err = load(lib, f)                                          # the defaults: 44100 Hz stereo s16 little-endian
    assert s, err
    assert s.contents.rate == 44100 and s.contents.frames == len(b) // 4
    lib.smp_sample_free(s)
    try:
        settings(lib, raw_rate=48000, raw_channels=2, raw_format=1, raw_endian=1)   # s24 big-endian
        s, err = load(lib, f)                                     # not the copy cached above: the settings changed
        assert s, err
        try:
            assert s.contents.rate == 48000 and s.contents.frames == len(src)
            got = whole(lib, s.contents.fwd)
            assert np.max(np.abs(got - src)) < 1e-4
        finally:
            lib.smp_sample_free(s)
        # .pcm is raw too, here mono
        settings(lib, raw_channels=1)
        g = tmp_path / "mono.pcm"
        g.write_bytes(bytes(b))
        s, err = load(lib, g)
        assert s, err
        assert s.contents.frames == 2 * len(src)
        lib.smp_sample_free(s)
    finally:
        point_cache(lib, tmp_path / "cache")


def test_audio_extensions_listed(lib):
    lib.smp_audio_extensions.restype = ctypes.c_int
    lib.smp_audio_extensions.argtypes = [ctypes.POINTER(ctypes.POINTER(ctypes.c_wchar_p))]
    p = ctypes.POINTER(ctypes.c_wchar_p)()
    n = lib.smp_audio_extensions(ctypes.byref(p))
    have = {p[i] for i in range(n)}
    for e in (".wav", ".flac", ".mp3", ".ogg", ".aif", ".aiff",
              ".caf", ".raw", ".pcm", ".opus", ".wv", ".ape", ".m4a", ".aac", ".mp4", ".wma", ".ac3", ".amr",
              ".3gp", ".alac", ".w64", ".rf64", ".aifc"):
        assert e in have, e


def test_ffmpeg_found(lib, tmp_path):
    lib.smp_ffmpeg_find.restype = ctypes.c_int
    lib.smp_ffmpeg_find.argtypes = [ctypes.c_char_p, ctypes.c_int]
    out = ctypes.create_string_buffer(1100)
    try:
        fake = tmp_path / "my ffmpeg.exe"                          # the Settings path first, when it is there
        fake.write_bytes(b"MZ")
        settings(lib, ffmpeg_path=str(fake))
        assert lib.smp_ffmpeg_find(out, 1100) == 1 and out.value.decode() == str(fake)
        settings(lib, ffmpeg_path=str(tmp_path / "gone.exe"))      # not there: searched for instead
        if FFMPEG:
            assert lib.smp_ffmpeg_find(out, 1100) == 1
            assert os.path.basename(out.value.decode()).lower() == "ffmpeg.exe" and os.path.exists(out.value.decode())
    finally:
        settings(lib, ffmpeg_path="")


def caf(rate=44100.0, flags=2, bpp=4, fpp=1, chans=2, bits=16, data=b"\0\0\0\0" + b"\1\0\2\0" * 8,
        data_size=None, fmt=b"lpcm"):
    """A CAF file made by hand: header, desc, data (edit count first). data_size overrides the data chunk's size."""
    desc = struct.pack(">d4sIIIII", rate, fmt, flags, bpp, fpp, chans, bits)
    size = len(data) if data_size is None else data_size
    return (b"caff" + struct.pack(">HH", 1, 0) + b"desc" + struct.pack(">q", len(desc)) + desc +
            b"data" + struct.pack(">q", size) + data)


def try_load(lib, path):
    """Loads; returns the frame count, or None when it was refused (with a reason)."""
    s, err = load(lib, path)
    if not s:
        assert err, "refused without a reason"
        return None
    n = int(s.contents.frames)
    lib.smp_sample_free(s)
    return n


def test_caf_malformed(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    good = caf()
    f = tmp_path / "ok.caf"
    f.write_bytes(good)
    assert try_load(lib, f) == 8
    bad = {
        "empty data": caf(data=b"\0\0\0\0"),
        "data under one frame": caf(data=b"\0\0\0\0\1\0"),
        "nan rate": caf(rate=float("nan")),
        "inf rate": caf(rate=float("inf")),
        "zero rate": caf(rate=0.0),
        "bpp mismatch": caf(bpp=6),
        "no channels": caf(chans=0, bpp=0),
        "alac": caf(fmt=b"alac"),
        "huge data size": caf(data_size=1 << 62),
        "zero data size": caf(data_size=0),
        "minus one, empty": caf(data=b"\0\0\0\0", data_size=-1),
        "desc too small": good[:8] + b"desc" + struct.pack(">q", 8) + good[20:],
        "desc huge": good[:8] + b"desc" + struct.pack(">q", -2) + good[20:],
        "no desc": good[:8] + good[8 + 12 + 32:],
    }
    for i, (name, b) in enumerate(bad.items()):
        g = tmp_path / ("bad%d.caf" % i)
        g.write_bytes(b)
        n = try_load(lib, g)
        assert n is None or n > 0, name
    for name in ("empty data", "nan rate", "bpp mismatch", "no desc"):
        g = tmp_path / ("bad%d.caf" % list(bad).index(name))
        assert try_load(lib, g) is None, name
    # huge and -1 sizes read to the end of the file: still the 8 frames there
    g = tmp_path / "m1.caf"
    g.write_bytes(caf(data_size=-1))
    assert try_load(lib, g) == 8
    for cut in range(0, len(good), 3):                            # cut short anywhere: refused or some audio
        g = tmp_path / ("cut%d.caf" % cut)
        g.write_bytes(good[:cut])
        n = try_load(lib, g)
        assert n is None or n > 0, cut


def test_caf_float_non_finite_is_silence(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    vals = [0.5, float("nan"), float("inf"), -float("inf"), -0.25, 0.0]
    d = b"\0\0\0\0" + struct.pack(">6f", *vals)
    f = tmp_path / "nan.caf"
    f.write_bytes(caf(flags=1, bpp=4, chans=1, bits=32, data=d))
    s, err = load(lib, f)
    assert s, err
    got = whole(lib, s.contents.fwd)[:, 0]
    lib.smp_sample_free(s)
    assert np.all(np.isfinite(got))
    assert list(got) == [0.5, 0, 0, 0, -0.25, 0]
    d = b"\0\0\0\0" + struct.pack(">3d", 0.5, float("nan"), 1e300)  # float64 too, and too big for a float
    f = tmp_path / "nan64.caf"
    f.write_bytes(caf(flags=1, bpp=8, chans=1, bits=64, data=d))
    s, err = load(lib, f)
    assert s, err
    got = whole(lib, s.contents.fwd)[:, 0]
    lib.smp_sample_free(s)
    assert list(got) == [0.5, 0, 0]


def test_raw_that_looks_like_mp3(lib, tmp_path):
    # a raw file can start with bytes like an MPEG frame sync or "ID3": still read as raw
    point_cache(lib, tmp_path / "cache")
    for i, head in enumerate((b"\xff\xfb\x90\x00", b"ID3\x03\x00\x00\x00\x00\x00\x00")):
        f = tmp_path / ("look%d.raw" % i)
        body = head + bytes(range(256)) * 64
        body += b"\0" * (-len(body) % 4)
        f.write_bytes(body)
        s, err = load(lib, f)
        assert s, err
        assert s.contents.rate == 44100 and s.contents.frames == len(body) // 4   # the default raw settings
        lib.smp_sample_free(s)


@need_ffmpeg
def test_raw_holding_real_mp3_bytes(lib, tmp_path):
    # real MP3 frames (starting with a frame sync, no tag) named .raw: read as raw, as the name says
    point_cache(lib, tmp_path / "cache")
    w = tmp_path / "src.wav"
    write_wav(w, two_tones(44100), 44100)
    m = tmp_path / "x.mp3"
    ffmpeg("-i", w, "-c:a", "libmp3lame", "-id3v2_version", "0", "-write_xing", "0", m)
    b = m.read_bytes()
    assert b[0] == 0xFF and (b[1] & 0xE0) == 0xE0
    f = tmp_path / "x.raw"
    f.write_bytes(b)
    s, err = load(lib, f)
    assert s, err
    assert s.contents.frames == len(b) // 4                      # raw s16 stereo, not about 44100 decoded frames
    lib.smp_sample_free(s)
