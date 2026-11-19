import ctypes
import os
import shutil
import struct
import subprocess
import sys
import time

import numpy as np
import pytest
import soundfile as sf

from conftest import ROOT, SmpSettings, write_wav
from test_cache import point_cache
from test_plugin import P  # noqa: F401 (the fixture)
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
    # our CAF reader alone (FFmpeg, the last stage, is off: it reads some of these)
    point_cache(lib, tmp_path / "cache")
    lib.smp_test_only_decode_flags.restype = None
    lib.smp_test_only_decode_flags.argtypes = [ctypes.c_long]
    lib.smp_test_only_decode_flags(256)
    try:
        caf_malformed(lib, tmp_path)
    finally:
        lib.smp_test_only_decode_flags(0)


def caf_malformed(lib, tmp_path):
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


# --- library decoders: Opus (opusfile), WavPack, Monkey's Audio ---

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")


def tone_ape_source():
    """What fixtures/tone.ape holds: two_tones' signal in float64, 22050 frames at 44100 Hz, rounded to 16 bits (see
    fixtures/README.md)."""
    t = np.arange(22050) / 44100
    x = np.stack([0.5 * np.sin(2 * np.pi * 220 * t), 0.3 * np.sin(2 * np.pi * 331 * t)], axis=1)
    return np.round(x * 32767).astype(np.int16)


def decode_mem(lib, b):
    """smp_decode_memory on bytes: (frames x channels float32, rate), or (None, the reason)."""
    lib.smp_decode_memory.restype = ctypes.POINTER(ctypes.c_float)
    lib.smp_decode_memory.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_int),
                                  ctypes.POINTER(ctypes.c_long), ctypes.POINTER(ctypes.c_int), ctypes.c_char_p, ctypes.c_int]
    lib.smp_free.restype = None
    lib.smp_free.argtypes = [ctypes.c_void_p]
    ch, n, r = ctypes.c_int(), ctypes.c_long(), ctypes.c_int()
    err = ctypes.create_string_buffer(300)
    p = lib.smp_decode_memory(b, len(b), ctypes.byref(ch), ctypes.byref(n), ctypes.byref(r), err, 300)
    if not p:
        return None, err.value.decode()
    x = np.ctypeslib.as_array(p, shape=(ch.value * n.value,)).copy().reshape(ch.value, n.value).T
    lib.smp_free(p)
    return x, r.value


def corr(a, b):
    return float(np.dot(a, b) / np.sqrt(np.dot(a, a) * np.dot(b, b)))


@need_ffmpeg
@pytest.mark.parametrize("chans", [1, 2])
def test_opus(lib, tmp_path, chans):
    point_cache(lib, tmp_path / "cache")
    src = two_tones(48000, 48000)[:, :chans]
    w = tmp_path / "src.wav"
    write_wav(w, src, 48000)
    f = tmp_path / ("tone %d.opus" % chans)
    ffmpeg("-i", w, "-c:a", "libopus", "-b:a", "160k", f)
    b = f.read_bytes()
    assert b[:4] == b"OggS" and b"OpusHead" in b[:100]
    s, err = load(lib, f)
    assert s, err
    try:
        assert s.contents.rate == 48000 and s.contents.by == 1
        assert abs(int(s.contents.frames) - len(src)) <= 312       # the encoder's pre-skip at most
        got = whole(lib, s.contents.fwd)
        n = min(len(got), len(src))
        for c in range(chans):
            assert corr(got[:n, c], src[:n, c]) > 0.99
    finally:
        lib.smp_sample_free(s)
    x, r = decode_mem(lib, b)                                      # from memory too (SoundFont streams)
    assert x is not None, r
    assert r == 48000 and x.shape[1] == chans and abs(len(x) - len(src)) <= 312


@need_ffmpeg
@pytest.mark.parametrize("subtype", ["PCM_16", "PCM_24", "FLOAT"])
def test_wavpack(lib, tmp_path, subtype):
    point_cache(lib, tmp_path / "cache")
    w = tmp_path / "src.wav"
    sf.write(str(w), two_tones(22050), 44100, subtype=subtype)
    src, _ = sf.read(str(w), dtype="float32")                      # exactly what the lossless file holds
    f = tmp_path / ("tone %s.wv" % subtype)
    ffmpeg("-i", w, "-c:a", "wavpack", f)
    b = f.read_bytes()
    assert b[:4] == b"wvpk"
    s, err = load(lib, f)
    assert s, err
    try:
        assert s.contents.rate == 44100 and s.contents.frames == len(src) and s.contents.by == 1
        assert np.array_equal(whole(lib, s.contents.fwd), src)     # bit-exact
    finally:
        lib.smp_sample_free(s)
    x, r = decode_mem(lib, b)
    assert x is not None, r
    assert r == 44100 and np.array_equal(x, src)


def test_ape(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    src = tone_ape_source().astype(np.float32) / 32768
    s, err = load(lib, os.path.join(FIXTURES, "tone.ape"))
    assert s, err
    try:
        assert s.contents.rate == 44100 and s.contents.frames == len(src) and s.contents.by == 1
        assert np.array_equal(whole(lib, s.contents.fwd), src)     # bit-exact
    finally:
        lib.smp_sample_free(s)


def test_ape_long_silence(lib):
    # 60 s of silence in 736 bytes (over 14000:1): a real file, not a lying one, so it loads in full
    b = open(os.path.join(FIXTURES, "silence60.ape"), "rb").read()
    assert len(b) < 1024
    x, r = decode_mem(lib, b)
    assert x is not None, r
    assert r == 44100 and x.shape == (44100 * 60, 2) and not np.any(x)


def small_files(tmp_path):
    """The three library formats, small: tone.ape, and an .opus and a .wv made by ffmpeg (bytes by extension)."""
    out = {".ape": open(os.path.join(FIXTURES, "tone.ape"), "rb").read()}
    w = tmp_path / "src.wav"
    write_wav(w, two_tones(24000, 48000), 48000)
    for ext, codec in ((".opus", "libopus"), (".wv", "wavpack")):
        f = tmp_path / ("good" + ext)
        ffmpeg("-i", w, "-c:a", codec, f)
        out[ext] = f.read_bytes()
    return out


@need_ffmpeg
def test_library_decoders_refuse_damage(lib, tmp_path):
    # cut short at every 97th byte, or scribbled on: refused with a reason or some audio, never a crash, and quickly
    rnd = np.random.default_rng(7)
    slowest = 0.0
    for ext, good in small_files(tmp_path).items():
        tries = [good[:cut] for cut in range(0, len(good), 97)]
        for i in range(40):
            b = bytearray(good)
            for k in rnd.integers(64, len(b), 1 + i):
                b[k] = int(rnd.integers(0, 256))
            tries.append(bytes(b))
        for b in tries:
            t = time.perf_counter()
            x, r = decode_mem(lib, b)
            slowest = max(slowest, time.perf_counter() - t)
            assert (x is None and r) or (x is not None and len(x) > 0), ext
    for magic in (b"wvpk", b"MAC ", b"OggS\0\2" + b"\0" * 20 + b"\x01\x13OpusHead"):
        x, r = decode_mem(lib, magic + bytes(range(256)) * 4)
        assert (x is None and r) or len(x) > 0
    assert slowest < 0.25, slowest


def ogg_crc(page):
    """An Ogg page's CRC (polynomial 0x04C11DB7, not reflected), computed with the CRC field zeroed."""
    crc = 0
    for byte in page:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) if crc & 0x80000000 else crc << 1
            crc &= 0xFFFFFFFF
    return crc


def ogg_last_granule(b, granule):
    """The Ogg file b with its last page's granule position set to granule (and the CRC fixed)."""
    at = b.rindex(b"OggS")
    page = bytearray(b[at:])
    page[6:14] = struct.pack("<q", granule)
    page[22:26] = b"\0\0\0\0"
    page[22:26] = struct.pack("<I", ogg_crc(page))
    return b[:at] + bytes(page)


def lying_headers(tmp_path):
    """Tiny files whose headers claim about 2^31 frames (name, bytes)."""
    w = tmp_path / "src.wav"
    write_wav(w, two_tones(4800, 48000), 48000)
    made = {}
    for ext, args in ((".opus", ["-c:a", "libopus"]), (".wv", ["-c:a", "wavpack"]), (".ogg", ["-c:a", "libvorbis"]),
                      (".flac", ["-c:a", "flac"]), (".pcm.wav", ["-c:a", "pcm_s16le"])):
        f = tmp_path / ("small" + ext)
        ffmpeg("-i", w, *args, f)
        made[ext] = f.read_bytes()
    big = 0x7FFFFFF0
    out = {}
    out["opus granule"] = ogg_last_granule(made[".opus"], big)
    out["vorbis granule"] = ogg_last_granule(made[".ogg"], big)
    wv = bytearray(made[".wv"])                    # WavPack: total_samples (offset 12), then block_samples (offset 20)
    wv[10:16] = b"\0\0" + struct.pack("<I", big)
    out["wavpack total"] = bytes(wv)
    wv = bytearray(made[".wv"])
    wv[20:24] = struct.pack("<I", big)
    out["wavpack block"] = bytes(wv)
    fl = bytearray(made[".flac"])                  # FLAC STREAMINFO: 36-bit total samples at bytes 21..25 (low 4 bits of 21)
    fl[21] = (fl[21] & 0xF0) | 0x0F
    fl[22:26] = b"\xff\xff\xff\xff"
    out["flac total"] = bytes(fl)
    wav = bytearray(made[".pcm.wav"])              # WAV: the data chunk's size
    at = wav.index(b"data")
    wav[at + 4:at + 8] = struct.pack("<I", 0xFFFFFFF0)
    out["wav data size"] = bytes(wav)
    ape = bytearray(open(os.path.join(FIXTURES, "tone.ape"), "rb").read())
    hdr = struct.unpack_from("<I", ape, 8)[0]      # APE_HEADER after the descriptor: blocks per frame, final, frames
    a = bytearray(ape)
    struct.pack_into("<I", a, hdr + 4, 1 << 30)
    out["ape blocks per frame"] = bytes(a)
    a = bytearray(ape)
    struct.pack_into("<II", a, hdr + 8, 73728, 30000)
    out["ape frames"] = bytes(a)
    a = bytearray(ape)                             # and 8 bits per sample, mono: the old check sized for bytes
    struct.pack_into("<IIIHH", a, hdr + 4, 1 << 30, 1 << 30, 2, 8, 1)
    out["ape 8-bit"] = bytes(a)
    # an APE tag footer claiming 200 MB of fields (the SDK allocates what it says before reading)
    out["ape tag size"] = bytes(ape) + b"APETAGEX" + struct.pack("<IIII", 2000, 200 << 20, 1, 0) + b"\0" * 8
    return out


CHILD = r"""
import ctypes, sys, time, psutil
dll = ctypes.CDLL(sys.argv[1])
f = dll.smp_decode_memory
f.restype = ctypes.POINTER(ctypes.c_float)
f.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_long),
              ctypes.POINTER(ctypes.c_int), ctypes.c_char_p, ctypes.c_int]
dll.smp_free.argtypes = [ctypes.c_void_p]
p = psutil.Process()
for path in sys.argv[2:]:
    b = open(path, "rb").read()
    before = p.memory_info().peak_pagefile
    ch, n, r = ctypes.c_int(), ctypes.c_long(), ctypes.c_int()
    err = ctypes.create_string_buffer(300)
    t = time.perf_counter()
    x = f(b, len(b), ctypes.byref(ch), ctypes.byref(n), ctypes.byref(r), err, 300)
    secs = time.perf_counter() - t
    grew = p.memory_info().peak_pagefile - before
    print("%s|%d|%d|%d|%.3f|%s" % (path, bool(x), n.value if x else 0, grew, secs, err.value.decode()))
    if x:
        dll.smp_free(x)
"""


@need_ffmpeg
def test_lying_headers_cost_nothing(tmp_path):
    # a tiny file claiming about 2^31 frames: refused or small, quickly, and never a large allocation (each file in a
    # fresh process, so its peak private memory is its own)
    files = []
    for name, b in lying_headers(tmp_path).items():
        f = tmp_path / (name.replace(" ", "_") + ".bin")
        f.write_bytes(b)
        files.append(f)
    for f in files:
        r = subprocess.run([sys.executable, "-I", "-c", CHILD, os.path.join(ROOT, "samplosis.clap"), str(f)],
                           capture_output=True, text=True, timeout=60)
        assert r.returncode == 0, f.name + r.stdout + r.stderr
        path, ok, frames, grew, secs, why = r.stdout.strip().split("|", 5)
        assert int(grew) < 64 << 20, (f.name, int(grew) >> 20, "MB")
        assert float(secs) < 1.0, (f.name, secs)
        assert (ok == "0" and why) or int(frames) < 10 ** 6, (f.name, frames, why)


@need_ffmpeg
def test_too_long_is_refused_plainly(lib, tmp_path):
    # past the size limit (lowered by the test hook to 65536 floats; really 2 GiB), every decoder says so plainly
    point_cache(lib, tmp_path / "cache")
    w = tmp_path / "src.wav"
    write_wav(w, two_tones(48000, 48000), 48000)                  # 1 s stereo: 96000 floats
    files = {"silence60.ape": open(os.path.join(FIXTURES, "silence60.ape"), "rb").read()}
    for ext, args in ((".opus", ["-c:a", "libopus"]), (".wv", ["-c:a", "wavpack"]), (".ogg", ["-c:a", "libvorbis"]),
                      (".flac", ["-c:a", "flac"]), (".mp3", ["-c:a", "libmp3lame"]), (".caf", ["-c:a", "pcm_s16le"])):
        f = tmp_path / ("x" + ext)
        ffmpeg("-i", w, *args, f)
        files[ext] = f.read_bytes()
    files[".wav"] = w.read_bytes()
    lib.smp_test_only_decode_flags.restype = None
    lib.smp_test_only_decode_flags.argtypes = [ctypes.c_long]
    lib.smp_test_only_decode_flags(2)
    try:
        for name, b in files.items():
            x, r = decode_mem(lib, b)
            assert x is None and r == "it's too long to load (over 2 GB decoded)", (name, r)
        g = tmp_path / "x.raw"                                    # and a raw file, through the sampler
        g.write_bytes(b"\0" * 400000)
        s, err = load(lib, g)
        assert not s and err == "it's too long to load (over 2 GB decoded)", err
    finally:
        lib.smp_test_only_decode_flags(0)
    x, r = decode_mem(lib, files[".opus"])
    assert x is not None, r


def test_ape_exception_is_caught(lib, tmp_path):
    # a C++ exception inside Monkey's Audio never crosses into C: the load is refused with a plain reason
    point_cache(lib, tmp_path / "cache")
    lib.smp_test_only_decode_flags.restype = None
    lib.smp_test_only_decode_flags.argtypes = [ctypes.c_long]
    lib.smp_test_only_decode_flags(1 | 256)                        # FFmpeg off: it reads Monkey's Audio too
    try:
        s, err = load(lib, os.path.join(FIXTURES, "tone.ape"))
        assert not s and "Monkey's Audio" in err, err
    finally:
        lib.smp_test_only_decode_flags(0)
    s, err = load(lib, os.path.join(FIXTURES, "tone.ape"))
    assert s, err
    lib.smp_sample_free(s)


@need_ffmpeg
def test_status_says_library(P, tmp_path):
    # a library decode says nothing extra: not Windows, not FFmpeg
    w = tmp_path / "src.wav"
    write_wav(w, two_tones(24000, 48000), 48000)
    files = [os.path.join(FIXTURES, "tone.ape")]
    for ext, codec in ((".opus", "libopus"), (".wv", "wavpack")):
        f = tmp_path / ("tone A3" + ext)
        ffmpeg("-i", w, "-c:a", codec, f)
        files.append(str(f))
    for f in files:
        y, d, r = P.play(P.script(f), "on 57 100 0\noff 57 4800", 9600)
        st = d["status"]
        assert st == "Ready: " + os.path.basename(f), st
        assert "Windows" not in st and "FFmpeg" not in st


def test_float_wav_non_finite_is_silence(lib, tmp_path):
    # NaN, infinities and absurd values from a damaged file never reach the cache or the engine
    point_cache(lib, tmp_path / "cache")
    vals = np.array([0.5, np.nan, np.inf, -np.inf, 2e6, -0.25, -3e9, 0.0], dtype=np.float32)
    f = tmp_path / "nan.wav"
    write_wav(f, vals, 44100)
    s, err = load(lib, f)
    assert s, err
    got = whole(lib, s.contents.fwd)[:, 0]
    lib.smp_sample_free(s)
    assert list(got) == [0.5, 0, 0, 0, 0, -0.25, 0, 0]


# Windows Media Foundation: the stage after the libraries, for files nothing earlier reads
BY_WINDOWS, BY_FFMPEG = 2, 3


def windows_file(tmp_path, name, args, rate=44100, frames=44100):
    """ffmpeg encodes two_tones (frames at rate) into tmp_path/name with args: (path, the source)."""
    src = two_tones(frames, rate)
    w = tmp_path / "src.wav"
    write_wav(w, src, rate)
    f = tmp_path / name
    ffmpeg("-i", w, *args, f)
    return f, src


def check_tones(lib, f, src, rate, by=BY_WINDOWS, corr_min=0.98, max_lag=64):
    """f loads, decoded by by, at rate, its length within 2048 frames of src, and each channel matches src above
    corr_min where they line up: the start must be within max_lag frames (an MPEG-4 file's start delay is cut by its
    edit list). Returns the lag."""
    s, err = load(lib, f)
    assert s, err
    try:
        assert s.contents.by == by, (s.contents.by, err)
        assert s.contents.rate == rate
        assert abs(int(s.contents.frames) - len(src)) <= 2048, (int(s.contents.frames), len(src))
        got = whole(lib, s.contents.fwd)
    finally:
        lib.smp_sample_free(s)

    def lined_up(k, c):
        """the correlation of channel c when got is k frames late (k < 0: early); the last 2048 frames are left out"""
        a, b = got[max(k, 0):, c], src[max(-k, 0):, c]
        n = min(len(a), len(b)) - 2048
        return corr(a[:n], b[:n])

    best = max(range(-max_lag, max_lag + 1), key=lambda k: lined_up(k, 0))
    for c in range(src.shape[1]):
        assert lined_up(best, c) > corr_min, (c, best, lined_up(best, c))
    return best


@need_ffmpeg
def test_m4a_aac(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    f, src = windows_file(tmp_path, "tone.m4a", ["-c:a", "aac", "-b:a", "192k"])
    assert f.read_bytes()[4:8] == b"ftyp"
    check_tones(lib, f, src, 44100)


@need_ffmpeg
def test_mp4_audio(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    f, src = windows_file(tmp_path, "tone.mp4", ["-c:a", "aac", "-b:a", "192k"], rate=48000, frames=48000)
    check_tones(lib, f, src, 48000)


@need_ffmpeg
def test_wma(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    f, src = windows_file(tmp_path, "tone.wma", ["-c:a", "wmav2", "-b:a", "192k"])
    assert f.read_bytes()[:4] == bytes.fromhex("3026b275")      # the ASF header GUID
    check_tones(lib, f, src, 44100)


@need_ffmpeg
def test_windows_path_in_any_language(lib, tmp_path):
    # the wide path reaches Media Foundation as it is: spaces, accents, '#' and '%' are a file name, not a URL
    point_cache(lib, tmp_path / "cache")
    f, src = windows_file(tmp_path, "tóne #1 50% üж.m4a", ["-c:a", "aac", "-b:a", "192k"])
    check_tones(lib, f, src, 44100)


@need_ffmpeg
def test_windows_not_from_memory(lib, tmp_path):
    # SoundFont streams (decode_memory) never go through Windows
    f, _ = windows_file(tmp_path, "tone.m4a", ["-c:a", "aac"])
    x, why = decode_mem(lib, f.read_bytes())
    assert x is None and why


@need_ffmpeg
def test_alac_m4a(lib, tmp_path, record_property):
    # Windows 10 and 11 usually have an ALAC decoder (MSAlacDecoder.dll); without one, FFmpeg takes it in a later
    # stage. Either passes, and the test records which; when it loads it must be lossless.
    point_cache(lib, tmp_path / "cache")
    w = tmp_path / "src.wav"
    sf.write(str(w), two_tones(22050), 44100, subtype="PCM_16")
    src, _ = sf.read(str(w), dtype="float32")
    f = tmp_path / "tone alac.m4a"
    ffmpeg("-i", w, "-c:a", "alac", f)
    s, err = load(lib, f)
    if not s:
        record_property("decoded_by", "nothing: " + err)
        assert err and "0x" not in err, err
        pytest.skip("this machine can't decode ALAC yet (FFmpeg comes in a later stage): " + err)
    try:
        record_property("decoded_by", {BY_WINDOWS: "Windows", BY_FFMPEG: "FFmpeg"}.get(s.contents.by, s.contents.by))
        assert s.contents.by in (BY_WINDOWS, BY_FFMPEG) and s.contents.rate == 44100
        assert np.array_equal(whole(lib, s.contents.fwd), src)     # bit-exact, start and length too
    finally:
        lib.smp_sample_free(s)


@need_ffmpeg
def test_amr_3gp(lib, tmp_path, record_property):
    # AMR-NB is 8000 Hz mono speech; a tone survives it only roughly
    point_cache(lib, tmp_path / "cache")
    r = subprocess.run([FFMPEG, "-hide_banner", "-encoders"], capture_output=True, text=True)
    if "libopencore_amrnb" not in r.stdout:
        pytest.skip("this ffmpeg has no AMR-NB encoder")
    src = two_tones(8000, 8000)[:, :1]
    w = tmp_path / "src.wav"
    write_wav(w, src, 8000)
    f = tmp_path / "tone.3gp"
    ffmpeg("-i", w, "-c:a", "libopencore_amrnb", "-b:a", "12.2k", f)
    s, err = load(lib, f)
    if not s:
        record_property("decoded_by", "nothing: " + err)
        assert err and "0x" not in err, err
        pytest.skip("this machine can't decode AMR-NB yet: " + err)
    try:
        record_property("decoded_by", s.contents.by)
        assert s.contents.by in (BY_WINDOWS, BY_FFMPEG) and s.contents.rate == 8000
        assert abs(int(s.contents.frames) - 8000) <= 2048
    finally:
        lib.smp_sample_free(s)


@need_ffmpeg
def test_ac3(lib, tmp_path, record_property):
    # Windows 11 24H2 removed its AC-3 decoder: refused plainly, or decoded right
    point_cache(lib, tmp_path / "cache")
    f, src = windows_file(tmp_path, "tone.ac3", ["-c:a", "ac3", "-b:a", "384k"], 48000, 48000)
    s, err = load(lib, f)
    if not s:
        record_property("decoded_by", "nothing: " + err)
        assert err and "0x" not in err, err
        pytest.skip("this machine can't decode AC-3: " + err)
    by = s.contents.by
    lib.smp_sample_free(s)
    record_property("decoded_by", by)
    assert by in (BY_WINDOWS, BY_FFMPEG), by
    # a bare AC-3 stream doesn't say how long the encoder's delay is (256 frames or so), so it stays
    check_tones(lib, f, src, 48000, by=by, corr_min=0.98, max_lag=512)


@need_ffmpeg
def test_windows_refuses_cleanly(lib, tmp_path):
    # damaged files Windows is offered: a plain reason (never an HRESULT) or some audio, quickly, and no crash
    point_cache(lib, tmp_path / "cache")
    f, _ = windows_file(tmp_path, "good.m4a", ["-c:a", "aac"])
    b = f.read_bytes()
    rnd = np.random.default_rng(11)
    cases = {"empty.m4a": b"", "ftyp only.m4a": b[:32], "garbage.m4a": bytes(rnd.integers(0, 256, 5000, dtype=np.uint8)),
             "header then garbage.m4a": b[:64] + bytes(rnd.integers(0, 256, 5000, dtype=np.uint8)),
             "half.m4a": b[:len(b) // 2], "no moov.m4a": b.replace(b"moov", b"mxxv"), "fake.wma": bytes.fromhex(
                 "3026b2758e66cf11a6d900aa0062ce6c") + bytes(200)}
    for i in range(6):                                            # scribbles over the real file
        c = bytearray(b)
        for at in rnd.integers(0, len(c), 20):
            c[at] = int(rnd.integers(0, 256))
        cases["scribbled %d.m4a" % i] = bytes(c)
    must_refuse = {"empty.m4a", "ftyp only.m4a", "garbage.m4a", "fake.wma"}
    for name, data in cases.items():
        g = tmp_path / name
        g.write_bytes(data)
        t = time.perf_counter()
        s, err = load(lib, g)
        secs = time.perf_counter() - t
        assert secs < 5, (name, secs)
        if s:
            assert name not in must_refuse, name
            lib.smp_sample_free(s)
        else:
            assert err and "0x" not in err and "HRESULT" not in err, (name, err)


@need_ffmpeg
def test_status_says_windows(P, tmp_path):
    w = tmp_path / "src.wav"
    write_wav(w, two_tones(24000, 48000), 48000)
    f = tmp_path / "tone A3.m4a"
    ffmpeg("-i", w, "-c:a", "aac", f)
    y, d, r = P.play(P.script(str(f)), "on 57 100 0\noff 57 4800", 9600)
    assert d["status"] == "Ready: tone A3.m4a (decoded by Windows)", d["status"]
    assert np.max(np.abs(y)) > 0.01


@need_ffmpeg
def test_windows_too_long(lib, tmp_path):
    # the same size limit holds for what Windows decodes (lowered here to 65536 floats by the test flag)
    point_cache(lib, tmp_path / "cache")
    f, _ = windows_file(tmp_path, "long.m4a", ["-c:a", "aac"])
    lib.smp_test_only_decode_flags.restype = None
    lib.smp_test_only_decode_flags.argtypes = [ctypes.c_long]
    lib.smp_test_only_decode_flags(2)
    try:
        s, err = load(lib, f)
        assert not s and err == "it's too long to load (over 2 GB decoded)", err
    finally:
        lib.smp_test_only_decode_flags(0)
    s, err = load(lib, f)
    assert s, err
    lib.smp_sample_free(s)


def mf_flags(lib, f):
    lib.smp_test_only_decode_flags.restype = None
    lib.smp_test_only_decode_flags.argtypes = [ctypes.c_long]
    lib.smp_test_only_decode_flags(f)


@need_ffmpeg
def test_windows_deadline(lib, tmp_path):
    # a stuck Windows decoder (the helper sleeps 3 s; the deadline is 0.5 s): the load returns quickly with a plain
    # reason, and the helper finishes later on its own without harm
    point_cache(lib, tmp_path / "cache")
    f, _ = windows_file(tmp_path, "stuck.m4a", ["-c:a", "aac"])
    lib.smp_test_only_mf_helpers.restype = ctypes.c_long
    mf_flags(lib, 4 | 256)                                         # FFmpeg off: it would read the file
    try:
        t = time.perf_counter()
        s, err = load(lib, f)
        secs = time.perf_counter() - t
    finally:
        mf_flags(lib, 0)
    assert not s and err == "Windows took too long to decode it", err
    assert secs < 2, secs
    assert lib.smp_test_only_mf_helpers() == 1                    # still asleep
    t = time.perf_counter()
    while lib.smp_test_only_mf_helpers() and time.perf_counter() - t < 10:
        time.sleep(0.05)
    assert lib.smp_test_only_mf_helpers() == 0                    # it woke, saw it was abandoned, and freed its job
    s, err = load(lib, f)
    assert s and s.contents.by == BY_WINDOWS, err
    lib.smp_sample_free(s)


@need_ffmpeg
def test_windows_buffer_fails(lib, tmp_path):
    # a sample whose buffer can't be read is a read error: nothing before it gives "damaged"; some before it keeps
    # exactly that much (no hole that shifts what follows)
    point_cache(lib, tmp_path / "cache")
    f, _ = windows_file(tmp_path, "tone.m4a", ["-c:a", "aac"])
    s, err = load(lib, f)
    assert s, err
    full = whole(lib, s.contents.fwd)
    lib.smp_sample_free(s)
    for flag, name in ((16, "first.m4a"), (32, "third.m4a")):
        g = tmp_path / name
        g.write_bytes(f.read_bytes())                              # a new name, so not the cached copy
        mf_flags(lib, flag | 256)                                  # FFmpeg off: it would read the file
        try:
            s, err = load(lib, g)
        finally:
            mf_flags(lib, 0)
        if flag == 16:
            assert not s and err == "Windows couldn't decode it; it may be damaged", err
        else:
            assert s, err
            part = whole(lib, s.contents.fwd)
            lib.smp_sample_free(s)
            assert 0 < len(part) < len(full)
            assert np.array_equal(part, full[:len(part)])


@need_ffmpeg
def test_windows_missing(lib, tmp_path):
    # no Media Foundation (a Windows N edition): the usual Windows names say so, other names get the plain message,
    # and it is tried again on the next load
    point_cache(lib, tmp_path / "cache")
    f, _ = windows_file(tmp_path, "tone.m4a", ["-c:a", "aac"])
    other = tmp_path / "tone.xyz"
    other.write_bytes(f.read_bytes())
    mf_flags(lib, 8 | 256)                                         # no FFmpeg either, or it would read both
    try:
        s, err = load(lib, f)
        assert not s and err == "Windows Media Foundation isn't installed", err
        s, err = load(lib, other)
        assert not s and err == "FFmpeg isn't installed; this format needs it", err
    finally:
        mf_flags(lib, 0)
    s, err = load(lib, f)
    assert s, err
    lib.smp_sample_free(s)


@need_ffmpeg
def test_windows_from_sta_thread(lib, tmp_path):
    # a caller whose thread is already a single-threaded apartment (a window's thread)
    import threading
    point_cache(lib, tmp_path / "cache")
    f, src = windows_file(tmp_path, "tone.m4a", ["-c:a", "aac", "-b:a", "192k"])
    out = {}

    def run():
        ole = ctypes.WinDLL("ole32")
        ole.CoInitializeEx.restype = ctypes.c_long
        out["co"] = ole.CoInitializeEx(None, 2)                     # COINIT_APARTMENTTHREADED
        try:
            s, err = load(lib, f)
            out["by"], out["frames"], out["err"] = (s.contents.by, s.contents.frames, err) if s else (None, 0, err)
            if s:
                lib.smp_sample_free(s)
        finally:
            ole.CoUninitialize()

    t = threading.Thread(target=run)
    t.start()
    t.join(30)
    assert out["co"] == 0                                          # S_OK: the thread really was made an STA
    assert out["by"] == BY_WINDOWS and out["frames"] == len(src), out


def id3_tag():
    """a small ID3v2.3 tag with a title"""
    frame = b"TIT2" + struct.pack(">I", 6) + b"\0\0" + b"\0tone!"
    n = len(frame)
    return b"ID3\x03\0\0" + bytes([(n >> 21) & 127, (n >> 14) & 127, (n >> 7) & 127, n & 127]) + frame


@need_ffmpeg
def test_id3_adts(lib, tmp_path):
    # an ADTS .aac with an ID3 tag in front: not taken for MP3, decoded by Windows
    point_cache(lib, tmp_path / "cache")
    f, src = windows_file(tmp_path, "plain.aac", ["-c:a", "aac", "-b:a", "192k", "-f", "adts"])
    g = tmp_path / "tagged.aac"
    g.write_bytes(id3_tag() + f.read_bytes())
    s, err = load(lib, g)
    assert s, err
    try:
        assert s.contents.by == BY_WINDOWS and s.contents.rate == 44100
    finally:
        lib.smp_sample_free(s)
    x, why = decode_mem(lib, g.read_bytes())                       # and from memory it isn't read as MP3 junk
    assert x is None and why


def mp4_box(t, payload=b"", size=None):
    return struct.pack(">I", len(payload) + 8 if size is None else size) + t + payload


def mp4_edit(lib, b):
    lib.smp_test_only_mp4_edit.restype = ctypes.c_int
    lib.smp_test_only_mp4_edit.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_double),
                                           ctypes.POINTER(ctypes.c_double)]
    s, d = ctypes.c_double(-7), ctypes.c_double(-7)
    r = lib.smp_test_only_mp4_edit(b, len(b), ctypes.byref(s), ctypes.byref(d))
    assert r in (0, 1)
    if r:
        assert np.isfinite(s.value) and s.value >= 0 and np.isfinite(d.value) and d.value >= 0, (s.value, d.value)
    return r, s.value, d.value


def test_mp4_edit_hostile(lib):
    # the edit-list reader on hostile boxes: never reads outside the bytes, never a crazy answer
    def hdr(v, ts, n=None):
        """mvhd or mdhd: version v, timescale ts (n cuts it short)"""
        body = bytes([v, 0, 0, 0]) + (bytes(16) + struct.pack(">I", ts) + bytes(8) if v == 1 else
                                      bytes(8) + struct.pack(">I", ts) + bytes(4))
        return body if n is None else body[:n]

    def elst(v=0, entries=((44100, 1024),), count=None):
        e = b"".join(struct.pack(">QqI" if v == 1 else ">IiI", seg, mt, 1 << 16) for seg, mt in entries)
        return bytes([v, 0, 0, 0]) + struct.pack(">I", len(entries) if count is None else count) + e

    def movie(mvhd=hdr(0, 44100), mdhd=hdr(0, 44100), el=elst(), moov_size=None, handler=b"soun"):
        trak = mp4_box(b"trak", mp4_box(b"mdia", mp4_box(b"hdlr", bytes(8) + handler + bytes(12)) +
                                        mp4_box(b"mdhd", mdhd)) + mp4_box(b"edts", mp4_box(b"elst", el)))
        body = mp4_box(b"mvhd", mvhd) + trak
        return mp4_box(b"ftyp", b"M4A " + bytes(4)) + mp4_box(b"moov", body, moov_size)

    good = movie()
    r, s, d = mp4_edit(lib, good)
    assert r == 1 and abs(s - 1024 / 44100) < 1e-12 and abs(d - 1.0) < 1e-12
    assert mp4_edit(lib, movie(el=elst(1, ((44100, 1024),))))[:2] == (1, 1024 / 44100)     # version 1
    assert mp4_edit(lib, movie(mvhd=hdr(1, 44100), mdhd=hdr(1, 44100)))[0] == 1          # version 1 headers
    assert mp4_edit(lib, movie(moov_size=0))[0] == 1                                       # size 0: to the end
    assert mp4_edit(lib, movie(handler=b"vide"))[0] == 0                                   # no sound track
    assert mp4_edit(lib, movie(el=elst(entries=((100, -1), (44100, 2112)))))[1] == 2112 / 44100   # an empty edit first
    must_refuse = {
        "huge 64-bit size": good[:16] + struct.pack(">I", 1) + b"moov" + struct.pack(">Q", 1 << 63) + good[24:],
        "64-bit size under its header": good[:16] + struct.pack(">I", 1) + b"moov" + struct.pack(">Q", 9) + good[24:],
        "size 4": good[:16] + struct.pack(">I", 4) + good[20:],
        "size past the end": good[:16] + struct.pack(">I", 0xFFFFFFF0) + good[20:],
        "elst version 2": movie(el=bytes([2]) + elst()[1:]),
        "elst version 255": movie(el=bytes([255]) + elst()[1:]),
        "elst too short": movie(el=elst()[:6]),
        "mdhd version 2": movie(mdhd=bytes([2]) + hdr(0, 44100)[1:]),
        "mdhd v1 too short": movie(mdhd=hdr(1, 44100, 24)),
        "mvhd v0 too short": movie(mvhd=hdr(0, 44100, 16)),
        "timescale 0": movie(mdhd=hdr(0, 0)),
        "ftyp only": good[:16],
        "count 0xFFFFFFFF, only empty edits": movie(el=elst(entries=((1, -1),) * 3, count=0xFFFFFFFF)),
    }
    for name, b in must_refuse.items():
        assert mp4_edit(lib, b)[0] == 0, name
    for b in (movie(el=elst(count=0xFFFFFFFF)), movie(el=elst(1, count=0xFFFFFFFF)),
              movie(el=elst(1, ((1, 1 << 62),)))):
        mp4_edit(lib, b)                                           # any answer, as long as it is sane
    rnd = np.random.default_rng(5)
    for cut in range(len(good)):                                   # cut short at every byte
        mp4_edit(lib, good[:cut])
    for _ in range(3000):                                          # scribbled
        c = bytearray(good)
        for at in rnd.integers(0, len(c), int(rnd.integers(1, 8))):
            c[at] = int(rnd.integers(0, 256))
        mp4_edit(lib, bytes(c))


UNLOAD_CHILD = r"""
import ctypes, os, sys, time
from ctypes import wintypes
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.FreeLibrary.argtypes = [wintypes.HMODULE]
k32.GetModuleHandleW.restype = wintypes.HMODULE
k32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
path, m4a = sys.argv[1], sys.argv[2]
dll = ctypes.CDLL(path)
dll.smp_test_only_decode_flags.argtypes = [ctypes.c_long]
dll.smp_test_only_decode_flags(4 | 256)            # the helper sleeps 3 s; the stall timer is 0.5 s; no FFmpeg
err = ctypes.create_string_buffer(512)
dll.smp_sample_load.restype = ctypes.c_void_p
s = dll.smp_sample_load(m4a.encode(), err, 512)
print("load", bool(s), err.value.decode())
name = os.path.basename(path)
h = dll._handle
del dll
assert k32.FreeLibrary(h)                          # the host lets go of the plugin (its one reference)
print("pinned", bool(k32.GetModuleHandleW(name)))  # the sleeping helper still holds it
time.sleep(4.5)                                    # the helper wakes, sees it was abandoned and exits
print("unloaded", not k32.GetModuleHandleW(name))
"""


@need_ffmpeg
def test_windows_helper_outlives_unload(tmp_path):
    # a host that unloads the plugin while an abandoned helper is still stuck in Windows: the helper holds the DLL
    # until it ends, then lets go, and nothing crashes
    f, _ = windows_file(tmp_path, "stuck.m4a", ["-c:a", "aac"])
    plugin = tmp_path / "samplosis_copy.clap"                      # a copy, so its name is this process's alone
    shutil.copyfile(os.path.join(ROOT, "samplosis.clap"), plugin)
    env = dict(os.environ, SAMPLOSIS_SETTINGS=str(tmp_path / "settings.ini"))
    r = subprocess.run([sys.executable, "-I", "-c", UNLOAD_CHILD, str(plugin), str(f)], capture_output=True, text=True,
                       timeout=60, env=env)
    assert r.returncode == 0, r.stdout + r.stderr
    lines = r.stdout.split("\n")
    assert lines[0] == "load False Windows took too long to decode it", r.stdout
    assert lines[1] == "pinned True" and lines[2] == "unloaded True", r.stdout


@need_ffmpeg
def test_windows_busy(lib, tmp_path):
    # another helper stuck starting Media Foundation: this load gives up quickly with a plain reason
    point_cache(lib, tmp_path / "cache")
    f, _ = windows_file(tmp_path, "tone.m4a", ["-c:a", "aac"])
    mf_flags(lib, 64 | 256)                                        # FFmpeg off: it would read the file
    try:
        t = time.perf_counter()
        s, err = load(lib, f)
        secs = time.perf_counter() - t
    finally:
        mf_flags(lib, 0)
    assert not s and err == "Windows decoding is busy or stuck", err
    assert secs < 2, secs
    s, err = load(lib, f)
    assert s, err
    lib.smp_sample_free(s)


# --- FFmpeg: the last stage, for files nothing earlier reads ---
FAKEFF = os.path.join(ROOT, "tests", "fakeff.exe")
FF_STALL_SHORT, FF_MISSING, FF_SLOW_START = 128, 256, 512      # smp_test_only_decode_flags bits
NOT_SOUND = "it isn't a sound file Samplosis can read"


def write_dsf(path, secs=0.1, freqs=(220, 331), amp=0.5):
    """A DSD64 .dsf file (ffmpeg can't write one): a first-order sigma-delta of a tone on each side."""
    rate, blk = 2822400, 4096
    n = int(rate * secs)
    chans = []
    for fr in freqs:
        x = (amp * np.sin(2 * np.pi * fr * np.arange(n) / rate)).tolist()
        bits = bytearray(n)
        acc = 0.0
        for i, v in enumerate(x):
            y = 1.0 if acc >= 0 else -1.0
            bits[i] = y > 0
            acc += v - y
        nb = (n + 7) // 8
        pad = (nb + blk - 1) // blk * blk
        chans.append(np.packbits(np.pad(np.frombuffer(bytes(bits), np.uint8), (0, pad * 8 - n)), bitorder="little"))
    data = b"".join(c[i * blk:(i + 1) * blk].tobytes() for i in range(len(chans[0]) // blk) for c in chans)
    body = (b"fmt " + struct.pack("<QIIIIIIQII", 52, 1, 0, 2, 2, rate, 1, n, blk, 0) +
            b"data" + struct.pack("<Q", 12 + len(data)) + data)
    open(path, "wb").write(b"DSD " + struct.pack("<QQQ", 28, 28 + len(body), 0) + body)


def fake_ffmpeg(tmp_path, mode):
    """A folder named mode holding the fake as ffmpeg.exe and ffprobe.exe (the folder's name says how it behaves)."""
    d = tmp_path / mode
    d.mkdir()
    for n in ("ffmpeg.exe", "ffprobe.exe"):
        shutil.copyfile(FAKEFF, d / n)
    return d


def pids_running(folder):
    """(running, all) of the processes the fake started from folder"""
    p = folder / "pids.txt"
    pids = [int(x) for x in p.read_text().split()] if p.exists() else []
    k32 = ctypes.WinDLL("kernel32")
    k32.OpenProcess.restype = ctypes.c_void_p
    k32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    k32.CloseHandle.argtypes = [ctypes.c_void_p]
    running = 0
    for pid in pids:
        h = k32.OpenProcess(0x00100000, False, pid)                # SYNCHRONIZE
        if h:
            running += k32.WaitForSingleObject(h, 0) == 0x102      # WAIT_TIMEOUT: still going
            k32.CloseHandle(h)
    return running, len(pids)


@need_ffmpeg
@pytest.mark.parametrize("name,args", [("tone.tta", ["-c:a", "tta"]), ("tone wavpack.mka", ["-c:a", "wavpack"]),
                                       ("tóne #1 50% üж.tta", ["-c:a", "tta"])])
def test_ffmpeg_decodes_odd_format(lib, tmp_path, name, args):
    point_cache(lib, tmp_path / "cache")
    f, src = windows_file(tmp_path, name, args)
    check_tones(lib, f, src, 44100, by=BY_FFMPEG, corr_min=0.9999, max_lag=0)


@need_ffmpeg
def test_ffmpeg_decodes_dsf(lib, tmp_path):
    # DSD at its own rate over 8 (352800 Hz), as FFmpeg decodes it
    point_cache(lib, tmp_path / "cache")
    f = tmp_path / "tone A3.dsf"
    write_dsf(f)
    s, err = load(lib, f)
    assert s, err
    try:
        assert s.contents.by == BY_FFMPEG and s.contents.rate == 352800
        got = whole(lib, s.contents.fwd)
    finally:
        lib.smp_sample_free(s)
    assert abs(len(got) - 35280) <= 4096
    t = np.arange(len(got)) / 352800
    for c, fr in enumerate((220, 331)):
        assert corr(got[:, c], np.sin(2 * np.pi * fr * t)) > 0.99, c


@need_ffmpeg
def test_ffmpeg_refuses_junk(lib, tmp_path):
    # random bytes FFmpeg can't open either: the plain message, not FFmpeg's
    point_cache(lib, tmp_path / "cache")
    f = tmp_path / "junk.tta"
    f.write_bytes(bytes(np.random.default_rng(3).integers(0, 256, 3000, dtype=np.uint8)))
    s, err = load(lib, f)
    assert not s and err == NOT_SOUND, err


@need_ffmpeg
def test_ffmpeg_missing(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    f, _ = windows_file(tmp_path, "tone.tta", ["-c:a", "tta"])
    mf_flags(lib, FF_MISSING)
    try:
        s, err = load(lib, f)
    finally:
        mf_flags(lib, 0)
    assert not s and err == "FFmpeg isn't installed; this format needs it", err
    s, err = load(lib, f)
    assert s and s.contents.by == BY_FFMPEG, err
    lib.smp_sample_free(s)


def test_ffmpeg_timeout(lib, tmp_path):
    # an FFmpeg that never writes: the reason within the (shortened) stall timer, and no process left behind
    point_cache(lib, tmp_path / "cache")
    d = fake_ffmpeg(tmp_path, "hang")
    f = tmp_path / "stuck.tta"
    f.write_bytes(b"not anything" * 100)
    settings(lib, ffmpeg_path=str(d / "ffmpeg.exe"))
    mf_flags(lib, FF_STALL_SHORT)
    try:
        t = time.perf_counter()
        s, err = load(lib, f)
        secs = time.perf_counter() - t
    finally:
        mf_flags(lib, 0)
        settings(lib, ffmpeg_path="")
    assert not s and err == "FFmpeg took too long to decode it", err
    assert secs < 3, secs
    assert pids_running(d) == (0, 2)                              # ffprobe ended; ffmpeg was killed


def test_ffmpeg_garbage(lib, tmp_path):
    # an FFmpeg that writes random bytes and fails: its own reason (trimmed), and no audio
    point_cache(lib, tmp_path / "cache")
    d = fake_ffmpeg(tmp_path, "garbage")
    f = tmp_path / "bad.tta"
    f.write_bytes(b"not anything" * 100)
    settings(lib, ffmpeg_path=str(d / "ffmpeg.exe"))
    try:
        s, err = load(lib, f)
    finally:
        settings(lib, ffmpeg_path="")
    assert not s and err == "Error while decoding stream: Invalid data found when processing input", err
    assert pids_running(d) == (0, 2)


@need_ffmpeg
def test_ffmpeg_too_long(lib, tmp_path):
    # the same 2 GB ceiling (lowered by the test flag): FFmpeg is stopped, and the plain too-long message
    point_cache(lib, tmp_path / "cache")
    f, _ = windows_file(tmp_path, "long.tta", ["-c:a", "tta"])
    mf_flags(lib, 2)
    try:
        s, err = load(lib, f)
    finally:
        mf_flags(lib, 0)
    assert not s and err == "it's too long to load (over 2 GB decoded)", err


def test_ffmpeg_missing_message_by_name(lib, tmp_path):
    # without FFmpeg, "FFmpeg isn't installed" only for a name our own decoders and Windows don't claim
    point_cache(lib, tmp_path / "cache")
    junk = bytes(np.random.default_rng(3).integers(0, 256, 3000, dtype=np.uint8))
    mf_flags(lib, FF_MISSING)
    try:
        for name in ("junk.wav", "junk.flac", "junk.ogg", "junk.caf", "junk.m4a"):
            f = tmp_path / name
            f.write_bytes(junk)
            s, err = load(lib, f)
            assert not s and err == NOT_SOUND, (name, err)
        f = tmp_path / "junk.dsf"
        f.write_bytes(junk)
        s, err = load(lib, f)
        assert not s and err == "FFmpeg isn't installed; this format needs it", err
    finally:
        mf_flags(lib, 0)


def test_ffprobe_hangs(lib, tmp_path):
    # a hanging ffprobe is killed after the (shortened) stall timer
    point_cache(lib, tmp_path / "cache")
    d = fake_ffmpeg(tmp_path, "probehang")
    f = tmp_path / "stuck.tta"
    f.write_bytes(b"not anything" * 100)
    settings(lib, ffmpeg_path=str(d / "ffmpeg.exe"))
    mf_flags(lib, FF_STALL_SHORT)
    try:
        t = time.perf_counter()
        s, err = load(lib, f)
        secs = time.perf_counter() - t
    finally:
        mf_flags(lib, 0)
        settings(lib, ffmpeg_path="")
    assert not s and err == "FFmpeg took too long to decode it", err
    assert secs < 3, secs
    assert pids_running(d) == (0, 1)


def test_ffprobe_missing(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    d = tmp_path / "fail"
    d.mkdir()
    shutil.copyfile(FAKEFF, d / "ffmpeg.exe")
    f = tmp_path / "x.tta"
    f.write_bytes(b"not anything" * 100)
    settings(lib, ffmpeg_path=str(d / "ffmpeg.exe"))
    try:
        s, err = load(lib, f)
    finally:
        settings(lib, ffmpeg_path="")
    assert not s and err == "FFmpeg's ffprobe.exe isn't beside ffmpeg.exe, and it's needed", err


def test_ffmpeg_ends_while_pipe_held(lib, tmp_path):
    # FFmpeg exits 0 but something it started still holds the pipe: the decode finishes at once (not after the
    # 60 s stall timer), and the job leaves nothing running
    point_cache(lib, tmp_path / "cache")
    d = fake_ffmpeg(tmp_path, "orphan")
    f = tmp_path / "x.tta"
    f.write_bytes(b"not anything" * 100)
    settings(lib, ffmpeg_path=str(d / "ffmpeg.exe"))
    try:
        t = time.perf_counter()
        s, err = load(lib, f)
        secs = time.perf_counter() - t
    finally:
        settings(lib, ffmpeg_path="")
    assert s, err
    assert s.contents.by == BY_FFMPEG and s.contents.frames == 44100
    lib.smp_sample_free(s)
    assert secs < 5, secs
    assert pids_running(d) == (0, 3)                              # ffprobe, ffmpeg, and its orphan (killed)


@need_ffmpeg
def test_foreign_formats_skip_the_built_in_decoders(lib, tmp_path):
    # An AU file (or DTS, AC-3, Matroska and others the built-in decoders don't read) whose audio happens to hold
    # MPEG frame syncs: before, dr_mp3 skipped the header and "read" a few frames, a sample of noise a fraction of
    # a second long, and the system decoder or FFmpeg never saw the file. Now the first bytes send it straight
    # there: the AU loads at its full length (16-bit mono PCM), or, on a machine where nothing reads it, not at all.
    point_cache(lib, tmp_path / "cache")
    w = tmp_path / "src.wav"
    write_wav(w, two_tones(44100), 44100)
    m = tmp_path / "x.mp3"
    ffmpeg("-i", w, "-c:a", "libmp3lame", "-id3v2_version", "0", "-write_xing", "0", m)
    body = m.read_bytes()
    body = body[:len(body) & ~1]
    au = tmp_path / "x.au"
    au.write_bytes(struct.pack(">4sIIIII", b".snd", 24, len(body), 3, 44100, 1) + body)
    s, err = load(lib, au)
    if s:
        assert s.contents.by != 0, "a built-in decoder took the AU file"
        assert s.contents.frames == len(body) // 2, (s.contents.frames, len(body) // 2)
        lib.smp_sample_free(s)
    dts = tmp_path / "x.dts"
    dts.write_bytes(b"\x7f\xfe\x80\x01" + body)
    s, err = load(lib, dts)
    if s:
        assert s.contents.by != 0, "a built-in decoder took the DTS file"
        lib.smp_sample_free(s)
