import ctypes
import os
import random
import shutil
import struct
import time

import numpy as np
import pytest

from conftest import SmpSfRegion, SmpSfSample, write_wav
from sf2make import write_sbk, write_sf2
from test_sffont import pcm

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


# a few of SF2.04's generator ranges (section 8.1.3), checked on every region walked
CLAMPS = {8: (1500, 13500), 9: (0, 960), 17: (-500, 500), 29: (0, 1000), 34: (-12000, 8000), 37: (0, 1440),
          46: (-1, 127), 51: (-120, 120), 52: (-99, 99), 56: (0, 1200), 58: (-1, 127)}

# SF2.04 section 8.4's default modulators, in the spec's order
DEFAULT_MODS = [
    (0x0502, 48, 960, 0, 0),        # note-on velocity to initial attenuation
    (0x0102, 8, 0, 0, 0),           # velocity to filter cutoff: present but 0, the 2.04 default is treated as an
                                    # erratum (FluidSynth disables it too); a font's identical one still replaces it
    (0x000D, 6, 50, 0, 0),          # channel pressure to vibrato LFO pitch depth
    (0x0081, 6, 50, 0, 0),          # CC1 to vibrato LFO pitch depth
    (0x0587, 48, 960, 0, 0),        # CC7 to initial attenuation
    (0x028A, 17, 500, 0, 0),        # CC10 to pan (500: 64 centre, 0 and 127 the ends)
    (0x058B, 48, 960, 0, 0),        # CC11 to initial attenuation
    (0x00DB, 16, 200, 0, 0),        # CC91 to reverb send
    (0x00DD, 15, 200, 0, 0),        # CC93 to chorus send
    (0x020E, 59, 12700, 0x0010, 0), # pitch wheel to initial pitch, times the bend range
]


def regions(lib, f, preset):
    out = ctypes.POINTER(SmpSfRegion)()
    n = lib.smp_sf_regions(f, preset, ctypes.byref(out))
    return [out[i] for i in range(n)]


def mods(ptr, n):
    return [(ptr[i].src, ptr[i].dest, ptr[i].amount, ptr[i].amt_src, ptr[i].trans) for i in range(n)]


def one_preset_regions(lib, tmp_path, preset, samples=SAMPLES):
    """Writes a font with just this preset and returns its regions as dicts (copied out before the font closes)."""
    path = tmp_path / "r.sf2"
    write_sf2(str(path), [preset], samples)
    f, err = sf_open(lib, path)
    assert f, err
    try:
        return [dict(keys=(r.lokey, r.hikey), vels=(r.lovel, r.hivel), sample=r.sample, gen=list(r.gen),
                     imod=mods(r.imod, r.nimod), pmod=mods(r.pmod, r.npmod)) for r in regions(lib, f, 0)]
    finally:
        lib.smp_sf_close(f)


def inst(zones, gens=None, mods=None, pgens=None, pmods=None, name="I"):
    return dict(name=name, gens=gens or {}, mods=mods or [], zones=zones, pgens=pgens or {}, pmods=pmods or [])


def preset(instruments, gens=None, pmods=None, bank=0, program=0, name="P"):
    return dict(name=name, bank=bank, program=program, gens=gens or {}, pmods=pmods or [], instruments=instruments)


def test_regions_ranges_intersect(lib, tmp_path):
    rs = one_preset_regions(lib, tmp_path, preset([inst([zone(0, keys=(0, 59)), zone(1, keys=(60, 127))],
                                                        pgens={43: (40, 80)})]))
    assert [(r["keys"], r["sample"]) for r in rs] == [((40, 59), 0), ((60, 80), 1)]
    assert all(r["vels"] == (0, 127) for r in rs)
    assert [r["gen"][43] for r in rs] == [40 | 59 << 8, 60 | 80 << 8]
    # velocity ranges intersect the same way, and a pair that doesn't overlap makes no region
    rs = one_preset_regions(lib, tmp_path, preset([inst([zone(0, vels=(1, 64)), zone(1, vels=(65, 127))],
                                                        pgens={43: (0, 30), 44: (50, 70)}),
                                                   inst([zone(2, keys=(31, 127))], pgens={43: (0, 30)})]))
    assert [(r["keys"], r["vels"], r["sample"]) for r in rs] == [((0, 30), (50, 64), 0), ((0, 30), (65, 70), 1)]


def test_regions_global_zones(lib, tmp_path):
    rs = one_preset_regions(lib, tmp_path, preset([inst([zone(0), zone(0, gens={34: -600})], gens={34: -1200})]))
    assert [r["gen"][34] for r in rs] == [-1200, -600]
    # a preset global zone gives the default for the preset zones, and a local one replaces it
    rs = one_preset_regions(lib, tmp_path, preset([inst([zone(0)]), inst([zone(1)], pgens={51: 2})],
                                                  gens={51: 7}))
    assert [r["gen"][51] for r in rs] == [7, 2]


def test_regions_preset_adds(lib, tmp_path):
    rs = one_preset_regions(lib, tmp_path, preset([inst([zone(0, gens={51: 2, 48: 200})],
                                                        pgens={51: 3, 54: 1, 48: 100, 58: 40, 46: 60})]))
    assert len(rs) == 1
    g = rs[0]["gen"]
    assert g[51] == 5
    assert g[54] == 0          # sampleModes is ignored at preset level
    assert g[48] == 300
    assert g[58] == -1 and g[46] == -1   # so are overridingRootKey and keynum


def test_regions_defaults(lib, tmp_path):
    rs = one_preset_regions(lib, tmp_path, preset([inst([zone(0)])]))
    g = rs[0]["gen"]
    assert g[8] == 13500 and g[56] == 100 and g[34] == -12000 and g[58] == -1 and g[46] == -1 and g[47] == -1
    for e in (21, 23, 25, 26, 27, 28, 30, 33, 34, 35, 36, 38):
        assert g[e] == -12000, e
    for e in (5, 6, 7, 9, 13, 17, 29, 37, 48, 51, 52, 54, 57):
        assert g[e] == 0, e


def test_regions_clamped(lib, tmp_path):
    rs = one_preset_regions(lib, tmp_path, preset([inst([zone(0, gens={48: 1400, 17: -450, 8: 2000})],
                                                        pgens={48: 400, 17: -200, 8: -1000})]))
    g = rs[0]["gen"]
    assert g[48] == 1800       # initialAttenuation isn't limited here: the voice limits 0.4 x it (FluidSynth's way)
    assert g[17] == -500
    assert g[8] == 1500
    # an instrument value out of range on its own is clamped too
    rs = one_preset_regions(lib, tmp_path, preset([inst([zone(0, gens={56: 3000, 52: -500})])]))
    assert rs[0]["gen"][56] == 1200 and rs[0]["gen"][52] == -99
    # a negative initialAttenuation is kept (just_t4_fixed.sf2 uses -78 and -26 to make quiet samples louder)
    rs = one_preset_regions(lib, tmp_path, preset([inst([zone(0, gens={48: -78})])]))
    assert rs[0]["gen"][48] == -78


def test_modulators_merged(lib, tmp_path):
    rs = one_preset_regions(lib, tmp_path, preset([inst([zone(0)])]))
    assert rs[0]["imod"] == DEFAULT_MODS
    assert rs[0]["pmod"] == []
    cc7 = (0x0587, 48, 480, 0, 0)
    new = (0x0081, 17, 100, 0, 0)    # CC1 to pan: not identical to any default
    rs = one_preset_regions(lib, tmp_path, preset([inst([zone(0, mods=[cc7]), zone(1, mods=[cc7, new])],
                                                        pmods=[(2, 48, 100, 0, 0)])]))
    want = list(DEFAULT_MODS)
    want[4] = cc7
    assert rs[0]["imod"] == want
    assert rs[1]["imod"] == want + [new]
    assert rs[0]["pmod"] == [(2, 48, 100, 0, 0)] and rs[1]["pmod"] == [(2, 48, 100, 0, 0)]
    # a local modulator replaces an identical global one, at each level
    g = (0x0081, 17, 100, 0, 0)
    rs = one_preset_regions(lib, tmp_path, preset([inst([zone(0, mods=[(0x0081, 17, -30, 0, 0)]), zone(1)], mods=[g],
                                                        pmods=[(0x00DB, 16, 50, 0, 0)])],
                                                  pmods=[(0x00DB, 16, 10, 0, 0), (0x00DD, 15, 20, 0, 0)]))
    assert rs[0]["imod"] == DEFAULT_MODS + [(0x0081, 17, -30, 0, 0)]
    assert rs[1]["imod"] == DEFAULT_MODS + [g]
    assert rs[0]["pmod"] == [(0x00DB, 16, 50, 0, 0), (0x00DD, 15, 20, 0, 0)]


def test_modulators_later_wins_in_a_zone(lib, tmp_path):
    # SF2.04 section 7.4: of two identical modulators in one zone, the later one counts
    cc7 = (0x0587, 48, 480, 0, 0)
    rs = one_preset_regions(lib, tmp_path, preset([inst([zone(0, mods=[(0x0081, 17, 100, 0, 0), cc7,
                                                                       (0x0081, 17, -30, 0, 0), (0x0587, 48, 240, 0, 0)])],
                                                        pmods=[(0x00DB, 16, 10, 0, 0), (0x00DB, 16, 70, 0, 0)])]))
    want = list(DEFAULT_MODS)
    want[4] = (0x0587, 48, 240, 0, 0)
    assert rs[0]["imod"] == want + [(0x0081, 17, -30, 0, 0)]
    assert rs[0]["pmod"] == [(0x00DB, 16, 70, 0, 0)]


def test_invalid_modulators_ignored(lib, tmp_path):
    # SF2.04 section 8.2.1: a modulator with an unknown source, destination or transform is ignored
    bad = [
        (0x0086, 48, 100, 0, 0),        # CC6 (data entry) isn't a source
        (0x00F8, 48, 100, 0, 0),        # nor is CC120 (a channel mode message)
        (0x1002, 48, 100, 0, 0),        # source type 4 doesn't exist
        (0x0005, 48, 100, 0, 0),        # general controller 5 doesn't exist
        (0x0002, 14, 100, 0, 0),        # an unused generator
        (0x0002, 41, 100, 0, 0),        # the instrument generator
        (0x0002, 60, 100, 0, 0),        # past the generators
        (0x0002, 0x8000, 100, 0, 0),    # a link to another modulator
        (0x007F, 48, 100, 0, 0),        # a source of "link"
        (0x0002, 48, 100, 0x0086, 0),   # an amount source of CC6
        (0x0587, 48, 100, 0, 1),        # transform 1 doesn't exist
    ]
    good = (0x0081, 17, 100, 0, 0)
    rs = one_preset_regions(lib, tmp_path, preset([inst([zone(0, mods=bad + [good])], pmods=bad + [good])]))
    assert rs[0]["imod"] == DEFAULT_MODS + [good]
    assert rs[0]["pmod"] == [good]


def test_global_zone_ranges_are_defaults(lib, tmp_path):
    # a global zone's key and velocity ranges hold for the zones after it that don't set their own
    rs = one_preset_regions(lib, tmp_path, preset(
        [inst([zone(0, keys=None, vels=None), zone(1, keys=(0, 127), vels=None)], gens={43: (0, 59), 44: (1, 90)})],
        gens={44: (10, 127)}))
    assert [(r["keys"], r["vels"], r["sample"]) for r in rs] == [((0, 59), (10, 90), 0), ((0, 127), (10, 90), 1)]


def many_mods(n):
    """n different valid modulators, none identical to a default one."""
    ccs = [c for c in range(1, 120) if c not in (6, 32, 38, 98, 99, 100, 101)]
    dests = [5, 7, 9, 10, 11, 13] + list(range(21, 41)) + [51, 52, 56]
    out = []
    for d in dests:
        for c in ccs:
            for bits in range(16):
                out.append((0x80 | c | bits << 8, d, 10, 0, 0))
                if len(out) == n:
                    return out
    raise ValueError(n)


def test_many_modulators_open_fast(lib, tmp_path):
    # a hostile font: 30000 modulators in an instrument's global zone and in a preset's, and 300 zones under each
    big = many_mods(30000)
    zones = [zone(i % 3, keys=(i % 128, i % 128), mods=[(0x0080 | 7, 22, i + 1, 0, 0)]) for i in range(300)]
    others = [inst([zone(0)], pmods=[(0x0080 | 7, 22, i + 1, 0, 0)], name="O%d" % i) for i in range(299)]
    p = preset([inst(zones, mods=big, name="Big")] + others, pmods=big)
    path = tmp_path / "many.sf2"
    write_sf2(str(path), [p], SAMPLES)
    t = time.perf_counter()
    f, err = sf_open(lib, path)
    took = time.perf_counter() - t
    assert f, err
    try:
        assert took < 1.0, took
        rs = regions(lib, f, 0)
        assert len(rs) == 300 + 299
        assert rs[0].nimod == 10 + 1024 + 1      # the first 1024 of a zone's modulators are read
        assert rs[0].npmod == 1024
        assert rs[300].npmod == 1024 + 1
        assert mods(rs[0].imod, rs[0].nimod)[-1] == (0x0087, 22, 1, 0, 0)
    finally:
        lib.smp_sf_close(f)


def test_unreachable_junk_is_ignored(lib, tmp_path):
    path = tmp_path / "junk.sf2"
    write_sf2(str(path), PRESETS, SAMPLES, junk_pgen=[(41, 999)], junk_igen=[(53, 999)])
    data = path.read_bytes()
    assert bytes([41, 0]) + (999).to_bytes(2, "little") in data[data.index(b"pgen"):data.index(b"inst")]
    assert bytes([53, 0]) + (999).to_bytes(2, "little") in data[data.index(b"igen"):data.index(b"shdr")]
    f, err = sf_open(lib, path)
    assert f, err
    try:
        walk_font(lib, f)
    finally:
        lib.smp_sf_close(f)


def test_bad_instrument_in_a_zone_fails(lib, tmp_path):
    # the only preset zone points at instrument 0; patch it to 9 of 1
    path = tmp_path / "badinst.sf2"
    write_sf2(str(path), [PRESETS[2]], SAMPLES)
    data = bytearray(path.read_bytes())
    at = data.index(b"pgen") + 8
    assert data[at:at + 4] == bytes([41, 0, 0, 0])
    data[at + 2] = 9
    path.write_bytes(bytes(data))
    f, err = sf_open(lib, path)
    assert not f
    assert "badinst.sf2 isn't a SoundFont this can read (" in err and "instrument 9" in err, err


def test_bad_stereo_link_is_cleared(lib, tmp_path):
    # a right sample linked to sample 99 of 3: links are never used, so the font opens and the sample is mono
    samples = [dict(SAMPLES[0], type=2, link=99), SAMPLES[1], dict(SAMPLES[2], type=4, link=1)]
    path = tmp_path / "badlink.sf2"
    write_sf2(str(path), PRESETS, samples)
    f, err = sf_open(lib, path)
    assert f, err
    try:
        h = lib.smp_sf_sample(f, 0).contents
        assert (h.type, h.link) == (1, 0)
        h = lib.smp_sf_sample(f, 2).contents
        assert (h.type, h.link) == (4, 1)                  # a good link stays as it is
        walk_font(lib, f)
        piano = lib.smp_sf_find_preset(f, 0, 0)
        lib.smp_sf_use(f, piano, 1)                         # the piano (sample 0) still loads
        assert lib.smp_sf_wait_idle(f, 5000) == 0
        assert lib.smp_sf_loaded_samples(f) == 1
        lib.smp_sf_use(f, piano, -1)
    finally:
        lib.smp_sf_close(f)


def test_duplicate_presets_first_wins(lib, tmp_path):
    path = tmp_path / "dup.sf2"
    write_sf2(str(path), [preset([inst([zone(0)])], name="First"), preset([inst([zone(2)])], name="Second")],
              SAMPLES)
    f, err = sf_open(lib, path)
    assert f, err
    try:
        j = lib.smp_sf_find_preset(f, 0, 0)
        name = ctypes.create_string_buffer(32)
        assert lib.smp_sf_preset_info(f, j, None, None, name, 32)
        assert name.value == b"First"
        assert [r.sample for r in regions(lib, f, j)] == [0]
        assert presets(lib, f) == [(0, 0, "First"), (0, 0, "Second")]
    finally:
        lib.smp_sf_close(f)


def walk_font(lib, f):
    """Reads everything the API offers about an opened font and checks every index is in range,
    including each preset's regions and their modulator lists."""
    n = lib.smp_sf_preset_count(f)
    ns = lib.smp_sf_sample_count(f)
    assert 0 <= n < 65536 and 0 <= ns < 65536
    bank, prog, name = ctypes.c_int(), ctypes.c_int(), ctypes.create_string_buffer(32)
    b2, p2 = ctypes.c_int(), ctypes.c_int()
    for i in range(n):
        assert lib.smp_sf_preset_info(f, i, ctypes.byref(bank), ctypes.byref(prog), name, 32)
        j = lib.smp_sf_find_preset(f, bank.value, prog.value)
        assert 0 <= j < n
        assert lib.smp_sf_preset_info(f, j, ctypes.byref(b2), ctypes.byref(p2), None, 0)
        assert (b2.value, p2.value) == (bank.value, prog.value)
        for r in regions(lib, f, i):
            assert r.lokey <= r.hikey <= 127 and r.lovel <= r.hivel <= 127
            assert 0 <= r.sample < ns
            assert r.nimod >= 10 and r.npmod >= 0
            assert len(mods(r.imod, r.nimod)) == r.nimod and len(mods(r.pmod, r.npmod)) == r.npmod
            for g, (lo, hi) in CLAMPS.items():
                assert lo <= r.gen[g] <= hi, (g, r.gen[g])
    out = ctypes.POINTER(SmpSfRegion)()
    assert lib.smp_sf_regions(f, n, ctypes.byref(out)) == 0 and not out
    assert lib.smp_sf_regions(f, -1, ctypes.byref(out)) == 0 and not out
    for i in range(ns):
        h = lib.smp_sf_sample(f, i).contents
        assert h.start <= h.end
        assert len(h.name) <= 20


def load_all(lib, f, timeout=10000):
    """Uses every preset, waits for the loader, reads every sample that loaded, then lets the presets go."""
    n = lib.smp_sf_preset_count(f)
    for i in range(n):
        lib.smp_sf_use(f, i, 1)
    lib.smp_sf_wait_idle(f, timeout)
    for i in range(lib.smp_sf_sample_count(f)):
        pcm(lib, f, i)
    for i in range(n):
        lib.smp_sf_use(f, i, -1)


def damaged_kind(kind, tmp_path):
    """A good file of one kind, for the damaged-file test."""
    if kind == "sf2":
        return make_font(tmp_path / "good.sf2")
    if kind == "sbk":
        path = str(tmp_path / "good.sbk")
        write_sbk(path, SBK_PRESETS, SBK_SAMPLES, rom=[("ROM Piano", 5000, 9000)])
        return path
    path = str(tmp_path / ("good." + kind))
    write_sf2(path, PRESETS, SAMPLES, sf4=(kind == "sf4"), pack=("flac" if kind == "sf2pack" else None))
    return path


@pytest.mark.parametrize("kind", ["sf2", "sf4", "sf2pack", "sbk"])
def test_truncated_and_corrupt_files(lib, tmp_path, kind):
    """Every 97th truncation and seeded random corruption: each open gives a font or NULL with a reason, never a
    crash. For the new kinds, some damaged fonts that open also load their samples (a bad FLAC stream, a bad pack)."""
    good = damaged_kind(kind, tmp_path)
    data = open(good, "rb").read()
    variants = [data[:cut] for cut in range(0, len(data), 97)]
    rng = random.Random(1234)
    for _ in range(300):
        b = bytearray(data)
        for _ in range(8):
            b[rng.randrange(len(b))] = rng.randrange(256)
        variants.append(bytes(b))
    path = tmp_path / ("bad." + kind)
    opened = 0
    for k, v in enumerate(variants):
        path.write_bytes(v)
        f, err = sf_open(lib, path)
        if f:
            opened += 1
            walk_font(lib, f)
            if kind != "sf2" and k % 7 == 0:
                load_all(lib, f)
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


# ---- SF4, SF2Pack and SoundFont 1 ----

def short_samples():
    """Three samples under 1 s (so a loaded sample is held whole) for comparing formats sample by sample."""
    return [dict(name="T%d" % i, data=tone(n, fr), rate=22050, pitch=60, correction=0, loop=lp)
            for i, (n, fr, lp) in enumerate([(6000, 440.0, (1200, 5800)), (4000, 220.0, None),
                                              (5000, 330.0, (17, 4999))])]


def all_preset(n):
    return [preset([inst([zone(i, keys=(10 * i, 10 * i + 9)) for i in range(n)])], name="All")]


def loaded(lib, path, expect):
    """Opens a font, loads every sample and returns [(int16 data, loop)] and how many decodes it took."""
    d0 = lib.smp_sf_test_decodes()
    f, err = sf_open(lib, path)
    assert f, err
    try:
        lib.smp_sf_use(f, 0, 1)
        assert lib.smp_sf_wait_idle(f, 20000) == 0
        assert lib.smp_sf_loaded_samples(f) == expect
        out = [pcm(lib, f, i) for i in range(expect)]
        lib.smp_sf_use(f, 0, -1)
        return out, lib.smp_sf_test_decodes() - d0
    finally:
        lib.smp_sf_close(f)


@pytest.mark.parametrize("flag", [True, 0x10])
def test_sf4_matches_sf2(lib, tmp_path, flag):
    """SF4 (each sample its own FLAC stream, flagged 0x20 with ifil 4) and FLAC inside SF3 (flagged 0x10) play
    exactly the SF2's samples, with SF3's loop points."""
    samples = short_samples()
    a, b = str(tmp_path / "t.sf2"), str(tmp_path / "t.sf4")
    write_sf2(a, all_preset(3), samples)
    write_sf2(b, all_preset(3), samples, sf4=flag)
    want, _ = loaded(lib, a, 3)
    got, decodes = loaded(lib, b, 3)
    assert decodes == 3
    for i, ((da, la), (db, lb)) in enumerate(zip(want, got)):
        assert len(da) == len(db) == len(samples[i]["data"])
        assert np.array_equal(da.astype(np.int32), db.astype(np.int32)), i
        assert la == lb == (samples[i]["loop"] or (0, 0))


PACK_EXACT = ["flac", "wavpack"]


@pytest.mark.parametrize("codec", PACK_EXACT)
def test_sf2pack_flac_matches_sf2(lib, tmp_path, codec):
    """An SF2Pack (RIFF form sfpk, the whole smpl chunk as one lossless stream) plays exactly the SF2's samples.
    The pack is decoded once, into the cache: the second open decodes nothing."""
    if codec == "wavpack" and not shutil.which("ffmpeg"):
        pytest.skip("ffmpeg isn't installed, so no WavPack pack can be made")
    samples = short_samples()
    a, b = str(tmp_path / "t.sf2"), str(tmp_path / "t.sf2pack")
    write_sf2(a, all_preset(3), samples)
    write_sf2(b, all_preset(3), samples, pack=codec)
    with open(b, "rb") as fh:
        assert fh.read(12)[8:] == b"sfpk"
    want, _ = loaded(lib, a, 3)
    got, decodes = loaded(lib, b, 3)
    assert decodes == 1                                   # one stream for the whole pack
    for i, ((da, la), (db, lb)) in enumerate(zip(want, got)):
        assert np.array_equal(da, db), i
        assert la == lb
    again, decodes = loaded(lib, b, 3)
    assert decodes == 0
    assert all(np.array_equal(x[0], y[0]) for x, y in zip(got, again))


@pytest.mark.parametrize("codec", ["vorbis", "opus"])
def test_sf2pack_vorbis(lib, tmp_path, codec):
    """A lossy pack (Ogg Vorbis, or Opus at 48 kHz) comes within -30 dB of the SF2's samples (-25 dB for Opus: at
    libsndfile's default bitrate the 220 Hz sample comes back at -29.6 dB, from libsndfile's own decoder too)."""
    samples = short_samples()
    a, b = str(tmp_path / "t.sf2"), str(tmp_path / "t.sf2pack")
    write_sf2(a, all_preset(3), samples)
    write_sf2(b, all_preset(3), samples, pack=codec)
    want, _ = loaded(lib, a, 3)
    got, _ = loaded(lib, b, 3)
    for i, ((da, la), (db, lb)) in enumerate(zip(want, got)):
        assert len(da) == len(db)                         # shdr gives the lengths, as in SF2
        x, y = da.astype(np.float64), db.astype(np.float64)
        err_db = 20 * np.log10(np.sqrt(np.mean((x - y) ** 2)) / np.sqrt(np.mean(x ** 2)))
        assert err_db < (-30 if codec == "vorbis" else -25), (i, err_db)
        assert la == lb


def test_sf2pack_damaged_stream(lib, tmp_path):
    """A pack whose stream can't be decoded opens (the headers are fine) but loads nothing, and says so by not
    loading; nothing crashes."""
    samples = short_samples()
    b = tmp_path / "bad.sf2pack"
    write_sf2(str(b), all_preset(3), samples, pack="flac")
    data = bytearray(b.read_bytes())
    at = data.find(b"fLaC")
    data[at:at + 4] = b"junk"
    b.write_bytes(bytes(data))
    f, err = sf_open(lib, b)
    assert f, err
    try:
        lib.smp_sf_use(f, 0, 1)
        assert lib.smp_sf_wait_idle(f, 10000) == 0
        assert lib.smp_sf_loaded_samples(f) == 0
        lib.smp_sf_use(f, 0, -1)
    finally:
        lib.smp_sf_close(f)


SBK_RATE = 44100
# 441 Hz at 44100 Hz: a period of exactly 100 frames
SBK_SAMPLES = [dict(name="Sine 441", data=tone(22050, 441.0, SBK_RATE), loop=(1000, 21000))]
# instrument zone generators in SBK units: root A4 (samplePitch 6900), loop on, pan hard right (127), attack 10 ms,
# release 100 ms, volume sustain at full (127), modulation sustain 48 of 96, chorus 128 of 256
SBK_GENS = {55: 6900, 54: 1, 17: 127, 34: 10, 38: 100, 37: 127, 29: 48, 15: 128}
SBK_PRESETS = [
    dict(name="Sine", bank=0, program=0, gens={},
         instruments=[dict(name="Sine", gens={}, zones=[dict(keys=(0, 127), sample=1, gens=SBK_GENS)])]),
    dict(name="Rom Piano", bank=0, program=1, gens={},
         instruments=[dict(name="Rom", gens={}, zones=[dict(keys=(0, 127), sample=0, gens={55: 6000})])]),
]


def peak_hz(x, rate):
    w = np.hanning(len(x))
    spec = np.abs(np.fft.rfft(x * w, 8 * len(x)))
    return np.argmax(spec) * rate / (8 * len(x))


def test_sbk_minimal(lib, tmp_path):
    """A hand-built SoundFont 1 bank: its generators are converted to SF2 units, the ROM sample's preset has
    nothing to play, and the file's sample plays at the right pitch (root from samplePitch, 44100 Hz)."""
    path = str(tmp_path / "t.sbk")
    write_sbk(path, SBK_PRESETS, SBK_SAMPLES, rom=[("ROM Piano", 5000, 9000)])
    f, err = sf_open(lib, path)
    assert f, err
    try:
        assert presets(lib, f) == [(0, 0, "Sine"), (0, 1, "Rom Piano")]
        assert lib.smp_sf_sample_count(f) == 2
        h = lib.smp_sf_sample(f, 1).contents
        assert h.name.decode() == "Sine 441"
        assert (h.start, h.end, h.rate) == (0, 22050, SBK_RATE)
        assert lib.smp_sf_sample(f, 0).contents.type & 0x8000          # the ROM sample
        assert regions(lib, f, 1) == []                                # nothing in the file to play
        (r,) = regions(lib, f, 0)
        assert r.sample == 1
        assert r.gen[58] == 69                                         # root key from samplePitch
        assert r.gen[17] == 500                                        # pan 127: hard right
        assert r.gen[34] == round(1200 * np.log2(0.010))               # 10 ms in timecents
        assert r.gen[38] == round(1200 * np.log2(0.100))
        assert r.gen[37] == 0                                          # sustain 127: no attenuation
        assert r.gen[29] == 500                                        # mod sustain 48 of 96: half way
        assert r.gen[15] == 500                                        # chorus 128 of 256
        assert r.gen[54] == 1
        walk_font(lib, f)
        lib.smp_sf_use(f, 0, 1)
        assert lib.smp_sf_wait_idle(f, 5000) == 0
        data, loop = pcm(lib, f, 1)
        assert np.array_equal(data, SBK_SAMPLES[0]["data"])
        assert loop == (1001, 21002)                                   # awesfx's SBK loop correction
        lib.smp_sf_use(f, 0, -1)
        for key, hz in ((69, 441.0), (81, 882.0), (57, 220.5)):
            n = 24000
            out = np.zeros(2 * n, np.float32)
            assert lib.smp_sf_test_note(f, 0, key, 100, 48000.0, n, -1,
                                        out.ctypes.data_as(ctypes.POINTER(ctypes.c_float))) == 1
            right = out[1::2].astype(np.float64)
            assert np.sqrt(np.mean(right[4800:] ** 2)) > 0.01
            assert abs(np.sqrt(np.mean(out[0::2][4800:] ** 2))) < 1e-3   # hard right
            got = peak_hz(right[4800:], 48000)
            assert abs(got - hz) < hz * 0.005, (key, got)
    finally:
        lib.smp_sf_close(f)


def test_sbk_rom_only_refused(lib, tmp_path):
    """A bank that only names Sound Blaster ROM samples has nothing to play and is refused plainly."""
    path = str(tmp_path / "rom.sbk")
    write_sbk(path, SBK_PRESETS[1:], [], rom=[("ROM Piano", 5000, 9000), ("ROM Strings", 9000, 12000)])
    f, err = sf_open(lib, path)
    assert not f
    assert err == "this SoundFont uses Sound Blaster ROM samples, which aren't in the file", err


def test_sf2pack_long_sample_streams(lib, tmp_path):
    """A long sample in a pack streams from the decoded pack's cache file: a 3 s note renders exactly as the SF2's."""
    rate = 44100
    samples = [dict(name="Long", data=tone(3 * rate, 330.0, rate), rate=rate, pitch=60, correction=0,
                    loop=(rate, 2 * rate)),
               dict(name="Short", data=tone(rate // 4, 440.0, rate), rate=rate, pitch=72, correction=0, loop=None)]
    a, b = str(tmp_path / "l.sf2"), str(tmp_path / "l.sf2pack")
    write_sf2(a, all_preset(2), samples)
    write_sf2(b, all_preset(2), samples, pack="flac")
    outs = []
    for path in (a, b):
        f, err = sf_open(lib, path)
        assert f, err
        try:
            n = int(3.5 * 48000)
            out = np.zeros(2 * n, np.float32)
            assert lib.smp_sf_test_note(f, 0, 5, 100, 48000.0, n, int(3.2 * 48000),
                                        out.ctypes.data_as(ctypes.POINTER(ctypes.c_float))) == 1
            outs.append(out)
        finally:
            lib.smp_sf_close(f)
    assert np.sqrt(np.mean(outs[0][-48000:] ** 2)) > 0.001
    assert np.array_equal(outs[0], outs[1])


def test_sf2pack_too_big_refused(lib, tmp_path):
    """A pack whose samples reach past what can be unpacked (1 GiB of 16-bit data: decode_memory's 2 GiB of float)
    is refused at open with a plain reason, not after a long decode."""
    path = tmp_path / "big.sf2pack"
    write_sf2(str(path), all_preset(1), short_samples()[:1], pack="flac")
    data = bytearray(path.read_bytes())
    at = data.find(b"shdr") + 8
    data[at + 24:at + 28] = struct.pack("<I", (1 << 29) + 1)   # the first sample's end
    path.write_bytes(bytes(data))
    f, err = sf_open(lib, path)
    assert not f
    assert "big.sf2pack isn't a SoundFont this can read (its packed samples are too big to unpack" in err, err
