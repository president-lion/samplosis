import ctypes
import os
import shutil
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
