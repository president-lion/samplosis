"""GigaStudio (.gig) through libgig (gigshim.cpp): instruments become presets, dimension regions become regions with
conditions, uncompressed waves play from the file and compressed ones are decoded by libgig. The .gig files are
written by libgig itself, through tests\\gigmake.exe (built by build.bat test)."""
import ctypes
import math
import os
import random
import struct
import subprocess
import sys

import numpy as np
import pytest
import soundfile as sf

from conftest import SmpSfCond, SmpSfRegion, SmpSfSample
from test_sf2 import load_all, mods, peak_hz, presets, regions, sf_open
from test_sffont import pcm

HERE = os.path.dirname(os.path.abspath(__file__))
GIGMAKE = os.path.join(HERE, "gigmake.exe")
RATE = 44100
OUT = 48000


def sine(n, freq, rate=RATE, amp=0.5):
    return amp * np.sin(2 * np.pi * freq * np.arange(n) / rate)


def raw16(x):
    return np.clip(np.round(np.asarray(x) * 32767), -32768, 32767).astype("<i2").tobytes()


def raw24(x):
    v = np.clip(np.round(np.asarray(x).reshape(-1) * 8388607), -8388608, 8388607).astype("<i4")
    return b"".join(int(s).to_bytes(4, "little", signed=True)[:3] for s in v)


def make_gig(tmp_path, name, samples, lines):
    """samples: dicts of name, data (frames, or frames x 2), bits, and extra words; lines: the instrument lines"""
    desc = []
    for i, s in enumerate(samples):
        data = np.asarray(s["data"], dtype=np.float64)
        ch = 1 if data.ndim == 1 else data.shape[1]
        bits = s.get("bits", 16)
        rawp = tmp_path / ("%s_w%d.raw" % (name, i))
        rawp.write_bytes(raw16(data) if bits == 16 else raw24(data))
        desc.append("sample name=%s rate=%d channels=%d bits=%d data=%s %s" % (
            s["name"], s.get("rate", RATE), ch, bits, rawp.name, s.get("extra", "")))
    desc += lines
    dp = tmp_path / (name + ".txt")
    dp.write_text("\n".join(desc) + "\n", encoding="utf-8")
    out = tmp_path / (name + ".gig")
    r = subprocess.run([GIGMAKE, str(dp), str(out)], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    return str(out)


def opened(lib, path):
    f, err = sf_open(lib, path)
    assert f, err
    return f


def note(lib, f, preset, key, vel=100, n=24000, release_at=-1):
    out = (ctypes.c_float * (2 * n))()
    nv = lib.smp_sf_test_note(f, preset, key, vel, float(OUT), n, release_at, out)
    assert nv >= 0
    return np.frombuffer(out, dtype=np.float32).reshape(n, 2).astype(np.float64).copy(), nv


def rms(x):
    return float(np.sqrt(np.mean(np.square(x)) + 1e-30))


def db(a, b):
    return 20 * math.log10(a / b)


def ignored(lib, f):
    first = ctypes.create_string_buffer(128)
    n = lib.smp_sf_ignored(f, first, 128)
    return n, first.value.decode("utf-8", "replace")


def head(lib, f, preset, sample):
    """a sample's loaded head in int16 units, with its preset in use"""
    lib.smp_sf_use(f, preset, 1)
    try:
        assert lib.smp_sf_wait_idle(f, 10000) == 0
        got = pcm(lib, f, sample)
        assert got is not None
        return np.asarray(got[0], dtype=np.float64)
    finally:
        lib.smp_sf_use(f, preset, -1)


def tc(seconds):
    return round(1200 * math.log2(seconds))


def walk(lib, f):
    ns = lib.smp_sf_sample_count(f)
    for i in range(lib.smp_sf_preset_count(f)):
        assert lib.smp_sf_preset_info(f, i, None, None, None, 0)
        for r in regions(lib, f, i):
            assert r.lokey <= r.hikey <= 127 and r.lovel <= r.hivel <= 127
            assert 0 <= r.sample < ns
            assert len(mods(r.imod, r.nimod)) == r.nimod


# ---- the presets-and-pitch file ----

def pitch_gig(tmp_path):
    samples = [
        # 0: 16-bit mono, 441 Hz, unity A4, looped over whole cycles
        dict(name="Sine441", data=sine(RATE, 441.0), extra="unity=69 loop=4410,30869"),
        # 1: 24-bit stereo: 330.75 Hz left, 661.5 Hz right, unity E4
        dict(name="Stereo24", bits=24, data=np.stack([sine(22050, 330.75), sine(22050, 661.5)], axis=1),
             extra="unity=64"),
        # 2: compressed 16-bit mono, 220.5 Hz, unity A3 (a second and a half, so it streams from the cache)
        dict(name="Packed", data=sine(66150, 220.5, amp=0.25), extra="unity=57 compressed=1"),
    ]
    lines = [
        # written out of order: the font lists them by bank, then program
        "instrument name=Kit program=0 drum=1",
        "region lo=36 hi=36 keygroup=1",
        "dimreg i=0 sample=2 unity=36",
        "instrument name=Strings program=48 bank=2 bend=12",
        "region lo=0 hi=127",
        "dimreg i=0 sample=1 fine=50 att=6",
        "instrument name=Piano program=0",
        "region lo=0 hi=127",
        "dimreg i=0 sample=0 attack=0.2 release=0.5 sustain=500 decay1=1.0 loop=4410,26460",
        "instrument name=Lsb program=3 banklsb=5",
        "region lo=0 hi=127",
        "dimreg i=0 sample=0 pitchtrack=0 eg3=300 lfo3=5:40",
    ]
    return make_gig(tmp_path, "pitch", samples, lines)


def test_gig_presets_and_pitch(lib, tmp_path):
    f = opened(lib, pitch_gig(tmp_path))
    try:
        assert presets(lib, f) == [(0, 0, "Piano"), (2, 48, "Strings"), (5, 3, "Lsb"), (128, 0, "Kit")]
        assert lib.smp_sf_sample_count(f) == 4                     # one per channel
        names = [lib.smp_sf_sample(f, i).contents.name.decode() for i in range(4)]
        assert names == ["Sine441", "Stereo24", "Stereo24", "Packed"]
        h = lib.smp_sf_sample(f, 0).contents
        assert (h.start, h.end, h.rate, h.pitch, h.loop_start, h.loop_end) == (0, RATE, RATE, 69, 4410, 30870)
        walk(lib, f)

        (r,) = regions(lib, f, 0)                                  # Piano
        g = r.gen
        assert g[58] == 69 and g[54] == 1                          # root key, looped
        assert (g[2], g[3]) == (0, 26460 + 4410 - 30870)            # the dimension region's loop through offsets
        assert g[34] == tc(0.2) and g[38] == tc(0.5)               # EG1 attack and release
        assert g[37] == round(-200 * math.log10(0.5))              # sustain 50 %: 60 cB
        assert g[36] == tc(1.0 * 96 / 6.0)                         # decay 1 reaches the sustain in 1 s
        assert r.cond.amp_veltrack == 100 and r.cond.amp_velcurve
        curve = [r.cond.amp_velcurve[v] for v in range(128)]
        assert all(0 <= c <= 1 for c in curve) and curve[127] > curve[40] > curve[1]
        wheel = [m for m in mods(r.imod, r.nimod) if m[0] == 0x020E]
        assert wheel == [(0x020E, 59, 200, 0, 0)]                  # the instrument's bend range, 2 semitones

        left, right = note(lib, f, 0, 69, n=24000)[0].T
        assert abs(peak_hz(left[2400:], OUT) - 441.0) < 441 * 0.004
        assert abs(rms(left[12000:]) - rms(right[12000:])) < 1e-3  # a mono wave sits in the middle
        # the wave's data comes straight from the file, exact
        x = head(lib, f, 0, 0)
        want = np.round(sine(RATE, 441.0) * 32767)
        assert len(x) >= 11025 and np.max(np.abs(x - want[:len(x)])) <= 1

        rs = regions(lib, f, 1)                                    # Strings: a 24-bit stereo pair
        assert len(rs) == 2 and [x.sample for x in rs] == [1, 2]
        assert (rs[0].gen[17], rs[1].gen[17]) == (-500, 500)
        assert (rs[0].cond.pair, rs[1].cond.pair) == (0, 1)
        assert 100 * rs[0].gen[51] + rs[0].gen[52] == 50            # fine tune 50 cents
        assert abs(rs[0].gen[48] * 0.4 - 60) <= 1                  # 6 dB of attenuation
        assert [m for m in mods(rs[0].imod, rs[0].nimod) if m[0] == 0x020E] == [(0x020E, 59, 1200, 0, 0)]
        (lr, _) = note(lib, f, 1, 64, n=24000)
        want = 330.75 * 2 ** (50 / 1200)
        assert abs(peak_hz(lr[2400:, 0], OUT) - want) < want * 0.004
        assert abs(peak_hz(lr[2400:, 1], OUT) - 2 * want) < 2 * want * 0.004

        (r,) = regions(lib, f, 3)                                  # Kit: the compressed wave, decoded by libgig
        assert r.gen[57] == 1                                      # the key group
        (lr, _) = note(lib, f, 3, 36, n=24000)
        assert abs(peak_hz(lr[2400:, 0], OUT) - 220.5) < 220.5 * 0.004
        x = head(lib, f, 3, 3)
        want = np.round(sine(66150, 220.5, amp=0.25) * 32767)
        assert len(x) >= 11025 and np.max(np.abs(x - want[:len(x)])) <= 1     # lossless

        (r,) = regions(lib, f, 2)                                  # Lsb: bank from the LSB, pitch tracking off
        assert r.gen[56] == 0 and r.gen[6] == 40 and abs(r.gen[24] - round(1200 * math.log2(5 / 8.176))) <= 1
        n, first = ignored(lib, f)
        assert n == 1 and first == "EG3 (pitch envelope)"
    finally:
        lib.smp_sf_close(f)


# ---- dimensions ----

class Player:
    def __init__(self, lib, tmp_path, path):
        self.lib, self.tmp = lib, tmp_path
        err = ctypes.create_string_buffer(512)
        self.s = lib.smp_synth_new(float(OUT), 256)
        lib.smp_synth_set_param(self.s, 33, 0)                               # no effects
        assert lib.smp_synth_load(self.s, path.encode(), err, 512) == 1, err.value

    def render(self, events, secs):
        out = self.tmp / "r.wav"
        assert self.lib.smp_synth_render_test(self.s, "\n".join(events).encode(), int(secs * OUT), 256,
                                              str(out).encode(), 0) == 0
        x, _ = sf.read(str(out), dtype="float32", always_2d=True)
        return x[:, 0]

    def program(self, prog):
        return "midi %d %d 0 0" % (0xC0, prog)

    def free(self):
        self.lib.smp_synth_free(self.s)


def on(k, v, t):
    return "midi %d %d %d %d" % (0x90, k, v, int(round(t * OUT)))


def off(k, t):
    return "midi %d %d 0 %d" % (0x80, k, int(round(t * OUT)))


def tone(x, f):
    w = np.hanning(len(x))
    t = np.arange(len(x)) / OUT
    return 2 * abs(np.sum(x * w * np.exp(-2j * np.pi * f * t))) / np.sum(w)


FREQS = [300.0, 500.0, 700.0, 900.0, 1100.0, 1300.0]


def tones_gig(tmp_path, name, lines):
    samples = [dict(name="T%d" % fq, data=sine(RATE, fq), extra="unity=60") for fq in FREQS]
    return make_gig(tmp_path, name, samples, lines)


def test_gig_velocity_and_round_robin(lib, tmp_path):
    """A velocity split (gig3 upper limit 40) with a round robin of two inside each zone: four dimension regions,
    velocity ranges from libgig's own lookup, seq_len 2 and seq_pos 1 and 2, and the player cycling them. Layers
    all sound, random splits 0..1, keyswitches become sw_* per key."""
    path = tones_gig(tmp_path, "vel", [
        "instrument name=VelRR program=0",
        "region lo=60 hi=60",
        "dim type=velocity bits=1 zones=2",
        "dim type=roundrobin bits=1 zones=2",
        # index = velocity zone (bit 0) + 2 x round robin zone (bit 1)
        "dimreg i=0 sample=0 upper=0:40", "dimreg i=1 sample=1 upper=0:127",
        "dimreg i=2 sample=2 upper=0:40", "dimreg i=3 sample=3 upper=0:127",
        "instrument name=Layers program=1",
        "region lo=60 hi=60",
        "dim type=layer bits=1 zones=2",
        "dimreg i=0 sample=0", "dimreg i=1 sample=4",
        "instrument name=Random program=2",
        "region lo=60 hi=60",
        "dim type=random bits=2 zones=4",
        "dimreg i=0 sample=0", "dimreg i=1 sample=1", "dimreg i=2 sample=2", "dimreg i=3 sample=3",
        "instrument name=Switch program=3 swlo=24 swhi=25",
        "region lo=60 hi=60",
        "dim type=keyboard bits=1 zones=2",
        "dimreg i=0 sample=0", "dimreg i=1 sample=5",
        "instrument name=Pair program=4",
        "region lo=60 hi=60",
        "dim type=samplechannel bits=1 zones=2",
        "dimreg i=0 sample=0", "dimreg i=1 sample=1",
        "instrument name=Wheel program=5",
        "region lo=60 hi=60",
        "dim type=modwheel bits=1 zones=2",
        "dimreg i=0 sample=0", "dimreg i=1 sample=2",
    ])
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        got = sorted((x.lovel, x.hivel, x.cond.seq_len, x.cond.seq_pos, x.sample) for x in r)
        assert got == [(0, 40, 2, 1, 0), (0, 40, 2, 2, 2), (41, 127, 2, 1, 1), (41, 127, 2, 2, 3)]
        r = regions(lib, f, 1)
        assert sorted(x.sample for x in r) == [0, 4] and all(x.cond.seq_len == 0 for x in r)
        r = regions(lib, f, 2)
        assert [(x.cond.lorand, x.cond.hirand) for x in r] == [(0, 0.25), (0.25, 0.5), (0.5, 0.75), (0.75, 1.0)]
        r = regions(lib, f, 3)
        assert sorted((x.cond.sw_lokey, x.cond.sw_hikey, x.cond.sw_last, x.cond.sw_default, x.sample) for x in r) == \
            [(24, 25, 24, 24, 0), (24, 25, 25, 24, 5)]
        r = regions(lib, f, 4)                     # two mono waves in a samplechannel dimension: a left-right pair
        assert [(x.sample, x.gen[17], x.cond.pair) for x in r] == [(0, -500, 0), (1, 500, 1)]
        r = regions(lib, f, 5)                     # the mod wheel dimension: CC1 ranges
        got = [(x.cond.locc_n, x.cond.locc[0].cc, x.cond.locc[0].lo, x.cond.locc[0].hi, x.sample) for x in r]
        assert got == [(1, 1, 0, 63, 0), (1, 1, 64, 127, 2)]
        walk(lib, f)
    finally:
        lib.smp_sf_close(f)
    p = Player(lib, tmp_path, path)
    try:
        def heard(ev, secs, starts, cands):
            x = p.render([p.program(prog)] + ev, secs)
            return [[fq for fq in cands if tone(x[int((t + 0.05) * OUT):int((t + 0.18) * OUT)], fq) > 0.02]
                    for t in starts]
        prog = 0
        ev = []
        for i in range(4):
            ev += [on(60, 100 if i < 2 else 20, 0.5 * i), off(60, 0.5 * i + 0.2)]
        assert heard(ev, 2.2, [0, 0.5, 1.0, 1.5], FREQS[:4]) == [[500.0], [900.0], [300.0], [700.0]]
        prog = 1
        assert heard([on(60, 100, 0), off(60, 0.3)], 0.5, [0], FREQS) == [[300.0, 1100.0]]
        prog = 3
        ev = [on(60, 100, 0), off(60, 0.2), on(25, 100, 0.4), off(25, 0.45), on(60, 100, 0.5), off(60, 0.7),
              on(24, 100, 0.9), off(24, 0.95), on(60, 100, 1.0), off(60, 1.2)]
        assert heard(ev, 1.4, [0, 0.5, 1.0], FREQS) == [[300.0], [1300.0], [300.0]]
        prog = 5
        ev = [on(60, 100, 0), off(60, 0.2), "midi %d 1 100 %d" % (0xB0, int(0.4 * OUT)), on(60, 100, 0.5), off(60, 0.7)]
        assert heard(ev, 0.9, [0, 0.5], FREQS) == [[300.0], [700.0]]
    finally:
        p.free()


def test_gig_release_trigger(lib, tmp_path):
    """A release trigger dimension: zone 0 plays at note-on, zone 1 at note-off with GigaStudio's release trigger
    decay as a straight fall of 0.01053 x (256 >> 4) of full level a second held (as LinuxSampler plays it)."""
    path = tones_gig(tmp_path, "rel", [
        "instrument name=Rel program=0",
        "region lo=60 hi=60",
        "dim type=releasetrigger bits=1 zones=2",
        "dimreg i=0 sample=1", "dimreg i=1 sample=3 rtdecay=4",
    ])
    f = opened(lib, path)
    k = 0.01053 * (256 >> 4)
    try:
        r = regions(lib, f, 0)
        assert [(x.cond.trigger, x.sample) for x in r] == [(0, 1), (1, 3)]
        assert abs(r[1].cond.rt_linear - k) < 1e-6 and r[0].cond.rt_linear == 0
    finally:
        lib.smp_sf_close(f)
    p = Player(lib, tmp_path, path)
    try:
        x = p.render([on(60, 100, 0), off(60, 0.3)], 0.7)
        assert tone(x[int(0.05 * OUT):int(0.25 * OUT)], 500.0) > 0.02
        assert tone(x[int(0.05 * OUT):int(0.25 * OUT)], 900.0) < 0.005    # not before the note-off
        assert tone(x[int(0.35 * OUT):int(0.55 * OUT)], 900.0) > 0.02
        a = p.render([on(60, 100, 0), off(60, 0.1)], 0.6)
        b = p.render([on(60, 100, 0), off(60, 3.1)], 3.6)
        la = tone(a[int(0.2 * OUT):int(0.4 * OUT)], 900.0)
        lb = tone(b[int(3.2 * OUT):int(3.4 * OUT)], 900.0)
        want = 20 * math.log10((1 - k * 0.1) / (1 - k * 3.1))
        assert abs(db(la, lb) - want) < 0.7, (db(la, lb), want)
    finally:
        p.free()


def structure_gig(tmp_path):
    """a small file that is nearly all structure (tiny waves, many dimensions), so damage lands in the headers"""
    samples = [dict(name="A", data=sine(300, 441.0), extra="unity=69 loop=10,200"),
               dict(name="B", bits=24, data=np.stack([sine(200, 330.0), sine(200, 660.0)], axis=1)),
               dict(name="C", data=sine(5000, 220.5, amp=0.25), extra="compressed=1")]
    lines = ["instrument name=VelRR program=0", "region lo=60 hi=60", "dim type=velocity bits=1 zones=2",
             "dim type=roundrobin bits=1 zones=2", "dimreg i=0 sample=0 upper=0:40", "dimreg i=1 sample=1 upper=0:127",
             "dimreg i=2 sample=2 upper=0:40", "dimreg i=3 sample=0 upper=0:127",
             "instrument name=Rel program=1 swlo=24 swhi=25", "region lo=0 hi=127",
             "dim type=releasetrigger bits=1 zones=2", "dim type=keyboard bits=1 zones=2", "dimreg i=0 sample=1",
             "dimreg i=1 sample=2 rtdecay=3", "dimreg i=2 sample=0 lfo3=5:30 vcf=100:20", "dimreg i=3 sample=2 eg3=100",
             "instrument name=Ch program=2", "region lo=0 hi=127", "dim type=samplechannel bits=1 zones=2",
             "dimreg i=0 sample=0", "dimreg i=1 sample=0"]
    return make_gig(tmp_path, "structure", samples, lines)


@pytest.mark.parametrize("kind", ["data", "structure"])
def test_gig_corrupt(lib, tmp_path, kind):
    """Every 97th truncation and 300 seeded corruptions of a small .gig with a compressed wave (one mostly wave data,
    one mostly headers): libgig's exceptions are caught, and each opens or gives NULL with a reason, never a crash or
    a hang, and without needing the last-resort guard; every variant that opens loads all its samples too (so the
    compressed wave goes through libgig's decoder damaged)."""
    data = open(pitch_gig(tmp_path) if kind == "data" else structure_gig(tmp_path), "rb").read()
    step = 97 if kind == "data" else 31
    variants = [data[:cut] for cut in range(0, len(data), step)]
    rng = random.Random(1234)
    for _ in range(300 if kind == "data" else 600):
        b = bytearray(data)
        for _ in range(8):
            b[rng.randrange(len(b))] = rng.randrange(256)
        variants.append(bytes(b))
    path = tmp_path / "bad.gig"
    opened_n = refused = 0
    guards = lib.smp_sf_test_gig_guards()
    for k, v in enumerate(variants):
        path.write_bytes(v)
        f, err = sf_open(lib, path)
        if f:
            opened_n += 1
            walk(lib, f)
            load_all(lib, f)                            # every one that opens: the PCM reads and libgig's decode
            lib.smp_sf_close(f)
        else:
            refused += 1
            assert err.startswith("bad.gig isn't a GigaStudio file this can read (") or                 err.startswith("bad.gig isn't a SoundFont") or err.startswith("can't open"), err
            assert err.endswith(")") or "SoundFont" in err or "open" in err
    assert opened_n > 0 and refused > 0
    assert lib.smp_sf_test_gig_guards() == guards       # libgig itself handled them: no last-resort guard was needed


def test_gig_named_dls_giga_chunks_go_to_libgig(lib, tmp_path):
    """A RIFF "DLS " file with Giga chunks but another name goes to libgig too (the DLS loader hands it over)."""
    src = pitch_gig(tmp_path)
    other = tmp_path / "renamed.dls"
    other.write_bytes(open(src, "rb").read())
    f = opened(lib, str(other))
    try:
        assert len(presets(lib, f)) == 4
    finally:
        lib.smp_sf_close(f)


# ---- review round 1: bounded memory, unbounded writes, the crash flag, paths ----

CHILD = os.path.join(HERE, "gigchild.py")
CRASHED = "A GigaStudio file crashed the reader; save your project under a new name and restart REAPER"


def child(tmp_path, *args):
    """runs gigchild.py in a fresh process with its own settings (cache under tmp_path) and returns its JSON"""
    import json
    settings = tmp_path / "child_settings.txt"
    if not settings.exists():
        (tmp_path / "child_cache").mkdir(exist_ok=True)
        settings.write_text("cache_dir %s\n" % (tmp_path / "child_cache"))
    env = dict(os.environ, SAMPLOSIS_SETTINGS=str(settings))
    r = subprocess.run([sys.executable, "-I", CHILD] + [str(a) for a in args], capture_output=True, text=True,
                       timeout=120, env=env)
    assert r.returncode == 0, (r.returncode, r.stderr[-2000:])     # a crash would end the child with an error code
    return json.loads(r.stdout.strip().splitlines()[-1])


def set32(data, at, value):
    struct.pack_into("<I", data, at, value)


def damaged(tmp_path, name, src, fn):
    data = bytearray(open(src, "rb").read())
    hits = fn(data)
    assert hits, name
    p = tmp_path / (name + ".gig")
    p.write_bytes(bytes(data))
    return p


def each(data, cid):
    at, out = 0, []
    while True:
        at = data.find(cid, at)
        if at < 0:
            return out
        out.append(at)
        at += 4


@pytest.mark.parametrize("field", ["ptbl_count", "wsmp_loops", "3lnk_size_and_count", "info_size", "3gnm_size",
                                   "3ewg_triggers", "riff_size", "compressed_tail"])
def test_gig_field_damage_bounded(lib, tmp_path, field):
    """A damaged count or size field never allocates what it claims or writes past an array: in a child process,
    the open (and loading every sample) grows peak committed memory by under 64 MB, and the child doesn't crash."""
    src = pitch_gig(tmp_path) if field == "compressed_tail" else structure_gig(tmp_path)

    def fn(data):
        hits = 0
        if field == "compressed_tail":                  # a compressed wave ending 1 byte after a frame's mode byte
            return compressed_tail(data)
        if field == "ptbl_count":                       # WavePoolCount (was: 4 bytes x the count allocated, twice)
            for i in each(data, b"ptbl"):
                set32(data, i + 12, 0xFFFFFFFF); hits += 1
        elif field == "wsmp_loops":                     # SampleLoops (was: 16 bytes x the count)
            for i in each(data, b"wsmp"):
                set32(data, i + 8 + 16, 0x7FFFFFFF); hits += 1
        elif field == "3lnk_size_and_count":            # DimensionRegions past 256 (was: a heap overflow), size past the file
            for i in each(data, b"3lnk"):
                set32(data, i + 8, 0xFFFFFFFF); set32(data, i + 4, 0x7FFFFFF0); hits += 1
        elif field == "info_size":                      # an INFO string chunk claiming far more than the file holds
            for i in each(data, b"INAM"):
                set32(data, i + 4, 0x7FFFFFF0); hits += 1
        elif field == "3gnm_size":                      # a group name claiming a huge size
            for i in each(data, b"3gnm"):
                set32(data, i + 4, 0x7FFFFFF0); hits += 1
        elif field == "3ewg_triggers":                  # a MIDI rule with 255 triggers (the array has 32)
            for i in each(data, b"3ewg"):
                if struct.unpack_from("<I", data, i + 4)[0] >= 40:
                    data[i + 8 + 32] = 4; data[i + 8 + 33] = 16; data[i + 8 + 36] = 255; hits += 1
        else:                                           # the RIFF size itself
            set32(data, 4, 0xFFFFFFF0); hits = 1
        return hits

    p = damaged(tmp_path, field, src, fn)
    got = child(tmp_path, "open", p)
    assert got["grow_mb"] < 64, got
    if not got["opened"]:
        assert got["err"].startswith(field + ".gig isn't a GigaStudio file this can read ("), got


def compressed_tail(data):
    """Rewrites the compressed wave's frame modes so its data chunk ends 1 byte after the last frame's mode byte, with
    that frame in mode 5 (a 12-byte header that can't fit): libgig counted about 2^61 samples there (the subtraction
    wrapped), which became a 256 MB decode. Returns 1 when done."""
    import collections
    i = data.find(b"ewav")
    ci = data.rfind(b"data", 0, i)                     # the compressed wave's data chunk comes before its ewav
    size = struct.unpack_from("<I", data, ci + 4)[0]
    body, target = ci + 8, size - 2
    step = {m: 1 + b for m, b in enumerate([4096, 2052, 768, 524, 396, 268])}
    reach = {0: None}
    q = collections.deque([0])
    while q:                                           # frame modes whose sizes add up to the target
        at = q.popleft()
        for m in (1, 5, 4, 3, 2):
            n = at + step[m]
            if n <= target and n not in reach:
                reach[n] = (at, m)
                q.append(n)
    if target not in reach:
        return 0
    at = target
    while at:
        prev, m = reach[at]
        data[body + prev] = m
        at = prev
    data[body + target] = 5
    return 1


def test_gig_crash_flag_parse(lib, tmp_path):
    """A crash caught by the last-resort guard (made on purpose inside the parse) refuses that file and every later
    one in the process, with the plain reason."""
    good = structure_gig(tmp_path)
    got = child(tmp_path, "fault", "parse", good)
    assert got["first_opened"] is False and got["first_err"] == CRASHED, got
    assert got["second_opened"] is False and got["second_err"] == CRASHED, got
    assert got["guards"] == 1


def test_gig_crash_flag_decode(lib, tmp_path):
    """The same for a crash while the loader opens libgig to decode a compressed wave: the font says the reason as
    its problem, and the next open is refused."""
    good = pitch_gig(tmp_path)
    got = child(tmp_path, "fault", "decode", good)
    assert got["first_opened"] is True and got["problem"] == CRASHED, got
    assert got["second_opened"] is False and got["second_err"] == CRASHED, got
    assert got["guards"] == 1


def add_top_chunk(data, cid, body):
    """appends a top-level chunk to a RIFF file (after everything, so no offset inside it moves)"""
    out = bytearray(data) + cid + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")
    struct.pack_into("<I", out, 4, len(out) - 8)
    return bytes(out)


def test_gig_extension_file_names(lib, tmp_path):
    """Extension file names (the xfil chunk) are plain names in the .gig's folder: one with a folder in it is
    refused, and a missing one gives libgig's plain reason."""
    src = open(pitch_gig(tmp_path), "rb").read()
    for name, want in [(b"..\\evil.gx01", "Extension file name not allowed"), (b"sub/x.gx01", "not allowed"),
                       (b"missing.gx01", "missing.gx01")]:
        body = struct.pack("<I", 1) + name.ljust(128, b"\0") + b"\0" * 16
        p = tmp_path / "x.gig"
        p.write_bytes(add_top_chunk(src, b"xfil", body))
        f, err = sf_open(lib, p)
        assert not f and err.startswith("x.gig isn't a GigaStudio file this can read (") and want in err, err


def test_gig_unicode_path(lib, tmp_path):
    """A file under a folder whose name isn't in the system code page opens, and its compressed wave decodes
    (libgig opens UTF-8 paths with CreateFileW)."""
    d = tmp_path / "日本語 Жито"
    d.mkdir()
    p = d / "Γκιγκ.gig"
    os.replace(pitch_gig(d), p)                 # written there by gigmake itself (wide paths, UTF-8 description)
    f = opened(lib, str(p))
    try:
        assert len(presets(lib, f)) == 4
        x = head(lib, f, 3, 3)
        want = np.round(sine(66150, 220.5, amp=0.25) * 32767)
        assert len(x) >= 11025 and np.max(np.abs(x - want[:len(x)])) <= 1
    finally:
        lib.smp_sf_close(f)


def model(lib, f):
    """Everything a player sees of a font, copied out: presets, each region's fields, conditions, modulators and
    curve, and the samples' headers."""
    out = [presets(lib, f)]
    for p in range(lib.smp_sf_preset_count(f)):
        for r in regions(lib, f, p):
            c = r.cond
            curve = [c.amp_velcurve[v] for v in range(128)] if c.amp_velcurve else None
            out.append((r.lokey, r.hikey, r.lovel, r.hivel, r.sample, tuple(r.gen), mods(r.imod, r.nimod),
                        r.npmod, curve, c.trigger, c.seq_len, c.seq_pos, c.sw_last, c.pair, r.rr_set))
    for i in range(lib.smp_sf_sample_count(f)):
        h = lib.smp_sf_sample(f, i).contents
        out.append((h.name, h.start, h.end, h.rate, h.pitch, h.loop_start, h.loop_end))
    return out


def test_gig_parse_cache(lib, tmp_path):
    """The second open of an unchanged .gig reads its parse from the cache folder (no libgig) and gives the same
    model; a changed file, or a damaged cache file, is parsed again."""
    from test_cache import point_cache
    cache = tmp_path / "cache"
    point_cache(lib, cache)
    lib.smp_sf_test_gig_cache_hits.restype = ctypes.c_long
    path = pitch_gig(tmp_path)
    f = opened(lib, path)
    try:
        first = model(lib, f)
    finally:
        lib.smp_sf_close(f)
    gigp = [p for p in os.listdir(cache) if p.endswith(".gigp")]
    assert len(gigp) == 1
    hits = lib.smp_sf_test_gig_cache_hits()
    f = opened(lib, path)
    try:
        assert lib.smp_sf_test_gig_cache_hits() == hits + 1
        assert model(lib, f) == first
        left = note(lib, f, 0, 69, n=12000)[0][:, 0]                    # and it plays the same
        assert abs(peak_hz(left[2400:], OUT) - 441.0) < 441 * 0.004
    finally:
        lib.smp_sf_close(f)
    (cache / gigp[0]).write_bytes(b"SGPC" + bytes(range(256)) * 4)       # damaged: parsed again, the same
    f = opened(lib, path)
    try:
        assert lib.smp_sf_test_gig_cache_hits() == hits + 1
        assert model(lib, f) == first
    finally:
        lib.smp_sf_close(f)
    st = os.stat(path)
    os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns + 10 ** 9))       # changed: another key, parsed again
    f = opened(lib, path)
    try:
        assert lib.smp_sf_test_gig_cache_hits() == hits + 1
        assert model(lib, f) == first
    finally:
        lib.smp_sf_close(f)


class SfPhdr(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char * 21), ("program", ctypes.c_uint16), ("bank", ctypes.c_uint16),
                ("bag", ctypes.c_uint16)]


class SmpSrc(ctypes.Structure):
    u8 = ctypes.c_uint8
    _fields_ = [("kind", ctypes.c_int), ("path_id", ctypes.c_int), ("offset", ctypes.c_uint64),
                ("length", ctypes.c_uint64), ("enc", u8), ("channels", u8), ("channel", u8),
                ("key", ctypes.c_uint64), ("wrap", u8), ("skip", ctypes.c_uint32), ("gen", u8), ("guessed", u8),
                ("giga", u8), ("index", ctypes.c_uint32)]


def fnv64(data):
    h = 0xcbf29ce484222325
    for x in data:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def gigp_layout(b):
    """Where a .gigp's parts are: the header (magic, version, the payload's FNV-1a), the struct sizes and counts, the
    paths, then the presets, sample headers, sources, preset starts and regions (each followed by two indices)."""
    assert b[:4] == b"SGPC" and struct.unpack_from("<I", b, 4)[0] == 2
    sizes = struct.unpack_from("<5I", b, 16)
    assert sizes == (ctypes.sizeof(SfPhdr), ctypes.sizeof(SmpSfRegion), 10, ctypes.sizeof(SmpSfSample),
                     ctypes.sizeof(SmpSrc)), sizes
    np_, nr, ns, npaths = struct.unpack_from("<4I", b, 36)
    nmods, ncurve = struct.unpack_from("<2Q", b, 52)
    at = 80 + 64                                         # the flags, then the first ignored setting's name
    for _ in range(npaths):
        at += 4 + struct.unpack_from("<I", b, at)[0] + 16
    L = dict(np=np_, nr=nr, ns=ns, npaths=npaths, nmods=nmods, phdr=at)
    L["shdr"] = at + np_ * sizes[0]
    L["src"] = L["shdr"] + ns * sizes[3]
    L["rstart"] = L["src"] + ns * sizes[4]
    L["regs"] = L["rstart"] + (np_ + 1) * 4
    L["mods"] = L["regs"] + nr * (sizes[1] + 16)
    return L


def resealed(b):
    """b with its check made right again, so only the field checks stand between it and the players"""
    b = bytearray(b)
    struct.pack_into("<Q", b, 8, fnv64(b[16:]))
    return bytes(b)


def gigp_damage(b, L):
    """The fields the review named, each set to a value the parse never makes, with the check made right: name ->
    bytes"""
    src_at = lambda i, field: L["src"] + i * ctypes.sizeof(SmpSrc) + getattr(SmpSrc, field).offset
    shdr_at = lambda i, field: L["shdr"] + i * ctypes.sizeof(SmpSfSample) + getattr(SmpSfSample, field).offset
    reg_at = lambda i, field: L["regs"] + i * (ctypes.sizeof(SmpSfRegion) + 16) + getattr(SmpSfRegion, field).offset
    cond_at = lambda i, field: reg_at(i, "cond") + getattr(SmpSfCond, field).offset
    stereo = next(i for i in range(L["ns"])
                  if struct.unpack_from("<B", b, src_at(i, "channels"))[0] == 2)   # a stereo wave's channel
    out = {}

    def put(name, fmt, at, v):
        d = bytearray(b)
        struct.pack_into(fmt, d, at, v)
        out[name] = resealed(d)

    for i in (0, stereo):
        put("channels0_%d" % i, "<B", src_at(i, "channels"), 0)
        put("channel_past_%d" % i, "<B", src_at(i, "channel"), struct.unpack_from("<B", b, src_at(i, "channels"))[0])
    d = bytearray(b)
    struct.pack_into("<BB", d, src_at(stereo, "channels"), 3, 2)        # channels then channel: a third channel
    out["channel_2_of_3"] = resealed(d)
    put("kind", "<i", src_at(0, "kind"), 7)
    put("enc", "<B", src_at(0, "enc"), 9)
    put("path_neg", "<i", src_at(0, "path_id"), -1)
    put("path_past", "<i", src_at(0, "path_id"), L["npaths"])
    put("end_past_file", "<I", shdr_at(0, "end"), 0x3FFFFFF)
    put("name_unended", "21s", L["phdr"], b"A" * 21)
    put("bank", "<H", L["phdr"] + SfPhdr.bank.offset, 129)
    for r in range(min(L["nr"], 3)):
        put("locc_n5_%d" % r, "<B", cond_at(r, "locc_n"), 5)
        put("locc_n255_%d" % r, "<B", cond_at(r, "locc_n"), 255)
        put("sample_neg_%d" % r, "<i", reg_at(r, "sample"), -1)
        put("sample_past_%d" % r, "<i", reg_at(r, "sample"), L["ns"])
        put("sw_lokey_%d" % r, "<b", cond_at(r, "sw_lokey"), -100)
        put("sw_last_%d" % r, "<b", cond_at(r, "sw_last"), -2)
        d = bytearray(b)                                                  # a range set at one end only
        struct.pack_into("<bb", d, cond_at(r, "sw_lokey"), -1, 50)        # (sw_lokey, then sw_hikey)
        out["sw_half_%d" % r] = resealed(d)
        put("hikey_%d" % r, "<B", reg_at(r, "hikey"), 200)
        put("nimod_%d" % r, "<i", reg_at(r, "nimod"), -5)
        put("trigger_%d" % r, "<B", cond_at(r, "trigger"), 9)
        put("gen_out_of_range_%d" % r, "<h", reg_at(r, "gen") + 2 * 47, 500)   # velocity: -1..127
    put("rstart_back", "<i", L["rstart"] + 4, L["nr"] + 1)
    if L["nmods"]:
        put("mod_dest", "<H", L["mods"] + 2, 999)
    return out


@pytest.mark.parametrize("which", ["pitch", "structure"])
def test_gig_parse_cache_damage(lib, tmp_path, which):
    """A damaged .gigp never reaches the players: bit flips at seeded offsets fail the payload's check, and the
    fields the parse guarantees (channels, the channel, the CC count, the sample index, keyswitch keys, ranges and
    the rest), set to bad values with the check made right, fail the field checks. Either way the cache is ignored,
    the file is parsed, the font is the same as before, and the cache is written again (the next open reads it).
    Random damage with the check made right may be believed when every field is still in range: then the font
    must walk, load and play, never crash."""
    from test_cache import point_cache
    point_cache(lib, tmp_path / "cache")
    lib.smp_sf_test_gig_cache_hits.restype = ctypes.c_long
    path = pitch_gig(tmp_path) if which == "pitch" else structure_gig(tmp_path)
    f = opened(lib, path)
    try:
        first = model(lib, f)
    finally:
        lib.smp_sf_close(f)
    gigp = [p for p in os.listdir(tmp_path / "cache") if p.endswith(".gigp")]
    assert len(gigp) == 1
    gigp = tmp_path / "cache" / gigp[0]
    good = gigp.read_bytes()
    assert struct.unpack_from("<Q", good, 8)[0] == fnv64(good[16:])
    L = gigp_layout(good)

    def reopen():
        """opens the file with the cache as it is: (believed, the model)"""
        hits = lib.smp_sf_test_gig_cache_hits()
        f = opened(lib, path)
        try:
            return lib.smp_sf_test_gig_cache_hits() == hits + 1, model(lib, f)
        finally:
            lib.smp_sf_close(f)

    assert reopen() == (True, first)                     # the good cache is believed (every field passes)

    rng = random.Random(4242)
    flips = []
    for _ in range(150):                                 # bit flips: the check fails
        d = bytearray(good)
        for _ in range(rng.randint(1, 4)):
            d[rng.randrange(16, len(d))] ^= 1 << rng.randrange(8)
        flips.append(bytes(d))
    for d in flips:
        gigp.write_bytes(d)
        assert reopen() == (False, first)
    assert reopen() == (True, first)                     # written again by the last parse

    named = gigp_damage(good, L)
    assert len(named) > 30
    for name, d in named.items():
        gigp.write_bytes(d)
        believed, got = reopen()
        assert not believed, name
        assert got == first, name
        assert reopen() == (True, first), name           # and written again

    believed_n = 0
    for _ in range(200):                                 # random damage with the check made right
        d = bytearray(good)
        for _ in range(rng.randint(1, 3)):
            d[rng.randrange(16, len(d))] = rng.randrange(256)
        gigp.write_bytes(resealed(d))
        hits = lib.smp_sf_test_gig_cache_hits()
        f, err = sf_open(lib, path)
        assert f, err
        try:
            if lib.smp_sf_test_gig_cache_hits() == hits + 1:
                believed_n += 1
                walk(lib, f)
                load_all(lib, f, 3000)
                out = (ctypes.c_float * 4800)()
                for p in range(lib.smp_sf_preset_count(f)):
                    lib.smp_sf_test_note(f, p, 60, 100, float(OUT), 2400, -1, out)
            else:
                assert model(lib, f) == first
        finally:
            lib.smp_sf_close(f)
    assert believed_n > 0                                # names and levels can change and still be in range
