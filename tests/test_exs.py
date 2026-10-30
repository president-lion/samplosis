"""Logic EXS (.exs): one preset whose regions come from its zones, narrowed and set by their groups and the
instrument's parameters. Every test writes a small .exs (little-endian, or the older big-endian layout) and its
samples into tmp_path, opens it through smp_sf_open, and checks the regions and how notes sound; the samples are
found where Logic's own folders would put them, not where the .exs says (a Mac path)."""
import math
import random
import struct

from test_sf2 import load_all, presets, regions, sf_open
from test_sfz import (OUT, RATE, Player, cents, ignored, note, off, on, opened, peak_hz, rms, sine, tone, walk,
                      write_wav)


def block(kind, name, content, be=False, flag=0, index=0):
    e = ">" if be else "<"
    size = len(content) | (0x8000 if flag & 0x80 else 0)
    head = bytes([0 if be else 1, 1, 0, kind | flag]) + struct.pack(e + "III", size, index, 0)
    head += (b"SOBT" if be else b"TBOS") + name.encode("latin-1")[:64].ljust(64, b"\0")
    return head + content


def zone(key=60, lo=None, hi=None, vel=None, fine=0, coarse=0, pan=0, vol=0, opts=0, start=0, end=0, loop=None,
         loopopts=0, loopdir=0, xfade=0, group=-1, sample=0, be=False):
    e = ">" if be else "<"
    lo = key if lo is None else lo
    hi = key if hi is None else hi
    if vel:
        opts |= 8
    vlo, vhi = vel or (0, 127)
    ls, le = loop or (0, 0)
    if loop:
        loopopts |= 1
    b = bytes([opts, key, fine & 255, pan & 255, vol & 255, 0, lo, hi, 0, vlo, vhi, 0])
    b += struct.pack(e + "5I", start, end, ls, le, xfade) + bytes([0, loopopts, loopdir]) + bytes(42)
    b += bytes([0, 0, 0, coarse & 255, 0, 0]) + bytes(5) + struct.pack(e + "iI", group, sample)
    assert len(b) == 96
    return b


def group(vol=0, pan=0, opts=0, excl=0, vmin=0, vmax=127, rnd=0, rt=0, by=0, by_note=0, rr=-1, cc=(0, 0, 127),
          keys=(0, 127), be=False, short=False):
    e = ">" if be else "<"
    b = bytes([vol & 255, pan & 255, 0, opts, excl, vmin, vmax, rnd]) + bytes(8) + struct.pack(e + "H", 0)
    b += bytes(14) + bytes([128, 0, 0, 128]) + bytes(2) + bytes([80, 140, 0, 0, 0, 0]) + bytes(12) + bytes(16)
    b += bytes([0, rt, 0, by_note])
    assert len(b) == 76
    if short:
        return b
    b += bytes(4) + struct.pack(e + "i", rr) + bytes([by, cc[0], cc[1], cc[2], keys[0], keys[1], 0, 0])
    return b


def sample(name, folder="/Users/someone/Music/Audio Music Apps/Samples/Piano", be=False, full=True):
    e = ">" if be else "<"
    b = struct.pack(e + "6I", 0, RATE, RATE, 16, 1, 1) + bytes(4) + b"WAVE" + struct.pack(e + "II", 0, 0) + bytes(40)
    b += folder.encode("utf-8")[:255].ljust(256, b"\0")
    if full:
        b += name.encode("utf-8")[:255].ljust(256, b"\0")
    return b


def params(pairs, be=False, wide=()):
    e = ">" if be else "<"
    ids = bytes([p[0] for p in pairs]).ljust(100, b"\0")
    vals = b"".join(struct.pack(e + "h", p[1]) for p in pairs).ljust(200, b"\0")
    b = struct.pack(e + "I", 100) + ids + vals
    if wide:
        b += struct.pack(e + "I", len(wide)) + b"".join(struct.pack(e + "HH", i, v) for i, v in wide)
    return b


def write_exs(path, zones, groups=(), samples=(), pars=None, be=False, flag=0):
    out = block(0, path.stem, struct.pack("<10I", 0, len(zones), len(groups), len(samples), 1, 0, 0, 0, 0, 0), be,
                flag)
    for z in zones:
        out += block(1, "zone", z, be, flag)
    for g in groups:
        out += block(2, "group", g, be, flag)
    for s in samples:
        out += block(3, "s", s, be, flag)
    if pars is not None:
        out += block(4, "params", pars, be, flag)
    path.write_bytes(out)
    return path


def env_param(seconds):
    return int(round(127 * (seconds / 10) ** 0.25))


def env_seconds(seconds):
    """the time an envelope parameter written for seconds stands for"""
    return 10 * (env_param(seconds) / 127) ** 4


def test_exs_mapping(lib, tmp_path):
    """Zones become regions: keys, velocities (narrowed by the group's), tuning with the instrument's transpose,
    volume and pan with the group's added, start and end, loops (with points, whole, sustain), one-shots, no pitch
    tracking; the amplitude envelope, velocity sensitivity and bend come from the parameters; a muted group is
    left out; what it can't play is counted; samples are found in Logic's Samples folder, a level up."""
    inst = tmp_path / "Sampler Instruments"
    samp = tmp_path / "Samples" / "Piano"
    inst.mkdir()
    samp.mkdir(parents=True)
    write_wav(samp / "a.wav", sine(RATE // 2, 440.0))
    write_wav(samp / "lp.wav", sine(RATE // 2, 440.0), loop=(2000, 2999))
    zones = [
        zone(key=62, lo=60, hi=64, vel=(10, 100), fine=-30, coarse=1, pan=-25, vol=-3, group=0, sample=0),
        zone(key=70, start=100, end=10000, loop=(1000, 5000), opts=2, sample=0),
        zone(key=72, loop=(0, 0), sample=0),
        zone(key=73, sample=1),                                    # no loop: the file's own loop doesn't count
        zone(key=74, opts=1, sample=0),                            # one-shot
        zone(key=75, loop=(1000, 5000), loopopts=4, sample=0),     # plays to the end on release
        zone(key=76, vel=(1, 50), group=1, sample=0),              # group velocity 60..127: nothing left
        zone(key=77, group=2, sample=0),                           # muted group
        zone(key=78, opts=4, sample=0),                            # reversed: counted, plays forward
        zone(key=79, sample=2),                                    # missing
    ]
    groups = [group(vol=-4, pan=10, vmin=20), group(vmin=60), group(opts=16)]
    samples = [sample("a.wav"), sample("lp.wav"), sample("gone.wav")]
    pars = params([(0x52, env_param(0.01)), (0x54, env_param(0.2)), (0x51, 64), (0x55, env_param(0.5)),
                   (0x5a, -30), (0x03, 12), (0x04, -1), (0x2d, 2), (0x0f, 5), (0x2c, 1)])
    path = write_exs(inst / "Grand.exs", zones, groups, samples, pars)
    f = opened(lib, str(path))
    try:
        assert presets(lib, f) == [(0, 0, "Grand")]
        r = regions(lib, f, 0)
        assert [x.lokey for x in r] == [60, 70, 72, 73, 74, 75, 78]
        a = r[0]
        assert (a.lokey, a.hikey, a.lovel, a.hivel, a.gen[58]) == (60, 64, 20, 100, 62)
        assert cents(a) == 100 - 30 + 200 + 5                      # coarse, fine, transpose 2, fine 5
        assert a.gen[48] == 175                                    # -3 dB + group -4 dB, x 25
        assert a.gen[17] == -150                                   # (-25 + 10) x 2 percent
        assert abs(a.gen[34] - 1200 * math.log2(env_seconds(0.01))) <= 1
        assert abs(a.gen[36] - 1200 * math.log2(env_seconds(0.2))) <= 1
        assert a.gen[37] == round(-200 * math.log10(64 / 127))      # sustain 64 of 127, as attenuation
        assert abs(a.gen[38] - 1200 * math.log2(env_seconds(0.5))) <= 1
        assert abs(a.cond.amp_veltrack - 50) < 1e-3
        b = r[1]
        assert b.gen[0] == 100 and b.cond.end_frame == 10000 and b.gen[54] == 1 and b.gen[56] == 0
        assert b.gen[2] + 32768 * b.gen[45] == 1000 and b.gen[3] + 32768 * b.gen[50] == 5000
        assert r[2].gen[54] == 1 and r[2].gen[3] + 32768 * r[2].gen[50] == RATE // 2   # the whole sample
        assert r[3].gen[54] == 0                                   # the .exs's loop setting wins over the file's
        assert r[4].cond.one_shot == 1
        assert r[5].gen[54] == 3                                   # loop_sustain
        walk(lib, f)
        n, first = ignored(lib, f)
        assert n == 2 and first == "reversed zones", (n, first)    # reversed zones, filter
        assert "Missing 1 sample (first: gone.wav)" in lib.smp_sf_problem(f).decode()
        x, nv = note(lib, f, 62)
        assert nv == 1 and abs(peak_hz(x[4000:20000, 0], OUT) - 440 * 2 ** (275 / 1200)) < 3
        assert rms(x[4000:20000, 0]) > 1.3 * rms(x[4000:20000, 1])   # panned left
        x, nv = note(lib, f, 76, vel=40)
        assert nv == 0
        x, nv = note(lib, f, 77)
        assert nv == 0
    finally:
        lib.smp_sf_close(f)


def test_exs_big_endian_and_detection(lib, tmp_path):
    """The older big-endian layout reads the same, with the 0x80 type flag and its size bit masked; a file is
    told by its first block whatever its name; a sample block without the file name field uses its block name."""
    write_wav(tmp_path / "a.wav", sine(RATE // 2, 440.0))
    for be, flag in ((True, 0), (True, 0x80), (False, 0x40)):
        out = block(0, "i", bytes(40), be, flag)
        out += block(1, "z", zone(key=60, lo=48, hi=72, vol=-6, sample=0, be=be), be, flag)
        out += block(3, "a.wav", sample("ignored.wav", be=be, full=False), be, flag)
        path = tmp_path / ("x%d%d.bin" % (be, flag))
        path.write_bytes(out)
        f = opened(lib, str(path))
        try:
            r = regions(lib, f, 0)
            assert [(x.lokey, x.hikey, x.gen[58], x.gen[48]) for x in r] == [(48, 72, 60, 150)], (be, flag)
            x, nv = note(lib, f, 72)
            assert nv == 1 and abs(peak_hz(x[4000:20000, 0], OUT) - 880) < 4
        finally:
            lib.smp_sf_close(f)


def test_exs_round_robin_random_and_switches(lib, tmp_path):
    """Round-robin groups cycle by their positions (-1 is the first); random select splits the draw among zones
    with the same ranges; groups enabled by a note are keyswitches (the lowest by group order the default);
    groups enabled by a controller get its range; a release-trigger group plays at note-off; an exclusive group
    chokes itself."""
    freqs = [300.0, 500.0, 700.0]
    for fq in freqs:
        write_wav(tmp_path / ("s%d.wav" % fq), sine(RATE, fq))
    zones = [zone(key=60, group=i, sample=i) for i in range(3)]
    zones += [zone(key=62, group=3, sample=0), zone(key=62, group=3, sample=1)]
    zones += [zone(key=64, lo=64, hi=70, group=4, sample=0), zone(key=64, lo=64, hi=70, group=5, sample=1)]
    zones += [zone(key=66, group=6, sample=2), zone(key=67, group=7, sample=0), zone(key=68, group=8, sample=0)]
    groups = [group(by=2, rr=-1), group(by=2, rr=0), group(by=2, rr=1), group(rnd=1),
              group(by=1, by_note=24), group(by=1, by_note=25), group(by=3, cc=(1, 64, 127)), group(rt=1),
              group(excl=3)]
    path = write_exs(tmp_path / "rr.exs", zones, groups, [sample("s%d.wav" % fq) for fq in freqs], params([]))
    f = opened(lib, str(path))
    try:
        r = regions(lib, f, 0)
        assert [(x.cond.seq_len, x.cond.seq_pos) for x in r[:3]] == [(3, 1), (3, 2), (3, 3)]
        assert [(round(x.cond.lorand, 4), round(x.cond.hirand, 4)) for x in r[3:5]] == [(0, 0.5), (0.5, 1)]
        assert [(x.cond.sw_lokey, x.cond.sw_hikey, x.cond.sw_last, x.cond.sw_default) for x in r[5:7]] == \
            [(24, 25, 24, 24), (24, 25, 25, 24)]
        assert r[7].cond.locc_n == 1 and (r[7].cond.locc[0].cc, r[7].cond.locc[0].lo, r[7].cond.locc[0].hi) == (1, 64, 127)
        assert r[8].cond.trigger == 1
        assert (r[9].cond.group, r[9].cond.off_by) == (3, 3)
        walk(lib, f)
        assert ignored(lib, f)[0] == 0
    finally:
        lib.smp_sf_close(f)
    p = Player(lib, tmp_path, str(path))
    try:
        ev = []
        for i in range(4):
            ev += [on(60, 100, 0.5 * i), off(60, 0.5 * i + 0.2)]
        x = p.render(ev, 2.2)
        got = []
        for i in range(4):
            w = x[int((0.5 * i + 0.05) * OUT):int((0.5 * i + 0.18) * OUT)]
            got.append([fq for fq in freqs if tone(w, fq) > 0.02])
        assert got == [[300.0], [500.0], [700.0], [300.0]]
        # the default switch plays s300 on 64; after key 25, s500
        x = p.render([on(64, 100, 0.0), off(64, 0.2), on(25, 100, 0.4), off(25, 0.45), on(64, 100, 0.5),
                      off(64, 0.7)], 0.9)
        assert tone(x[int(0.05 * OUT):int(0.18 * OUT)], 300.0) > 0.02 and tone(x[int(0.05 * OUT):int(0.18 * OUT)], 500.0) < 0.01
        assert tone(x[int(0.55 * OUT):int(0.68 * OUT)], 500.0) > 0.02 and tone(x[int(0.55 * OUT):int(0.68 * OUT)], 300.0) < 0.01
    finally:
        p.free()


def test_exs_sample_search(lib, tmp_path):
    """Samples are found beside the .exs, in its subfolders, and in folders up to three levels above; where two
    files share a name, the one in a folder named like the sample's Mac folder wins; a quote matches an
    underscore; a name with no extension matches an audio file; a drive's root is never searched."""
    lib_dir = tmp_path / "Library"
    inst = lib_dir / "Instruments" / "Keys" / "Deep"
    inst.mkdir(parents=True)
    (lib_dir / "Samples" / "Rhodes").mkdir(parents=True)
    (lib_dir / "Samples" / "Other").mkdir(parents=True)
    write_wav(lib_dir / "Samples" / "Other" / "c3.wav", sine(RATE // 2, 300.0))
    write_wav(lib_dir / "Samples" / "Rhodes" / "c3.wav", sine(RATE // 2, 500.0))
    write_wav(lib_dir / "Samples" / "Rhodes" / "Bob_s d3.wav", sine(RATE // 2, 500.0))
    write_wav(lib_dir / "Samples" / "Rhodes" / "e3.aif", sine(RATE // 2, 500.0))   # a WAV named .aif: it probes
    zones = [zone(key=60, sample=0), zone(key=62, sample=1), zone(key=64, sample=2)]
    samples = [sample("c3.wav", folder="/Volumes/HD/Samples/Rhodes/"), sample("Bob's d3.wav"), sample("e3")]
    path = write_exs(inst / "Rhodes.exs", zones, (), samples, params([]))
    f = opened(lib, str(path))
    try:
        assert lib.smp_sf_problem(f) in (None, b"")
        assert len(regions(lib, f, 0)) == 3
        for k in (60, 62, 64):
            x, nv = note(lib, f, k)
            assert nv == 1 and abs(peak_hz(x[4000:20000, 0], OUT) - 500) < 4, k
    finally:
        lib.smp_sf_close(f)
    # four levels up is too far
    far = tmp_path / "far" / "a" / "b" / "c" / "d"
    far.mkdir(parents=True)
    write_wav(tmp_path / "far" / "x.wav", sine(RATE // 2, 300.0))
    write_wav(tmp_path / "far" / "y.wav", sine(RATE // 2, 300.0))
    write_wav(far / "y.wav", sine(RATE // 2, 300.0))
    path = write_exs(far / "Far.exs", [zone(key=60, sample=0), zone(key=61, sample=1)], (),
                     [sample("x.wav"), sample("y.wav")], params([]))
    f = opened(lib, str(path))
    try:
        assert "Missing 1 sample (first: x.wav)" in lib.smp_sf_problem(f).decode()
    finally:
        lib.smp_sf_close(f)


def test_exs_refusals_and_damage(lib, tmp_path):
    """Files that aren't an EXS, with no zones, or every sample missing don't open and say why; damaged files
    (cut short, bytes changed, huge sizes and indexes) never crash."""
    deep = tmp_path / "a" / "b" / "c"                              # the search stays inside tmp_path
    deep.mkdir(parents=True)
    write_wav(deep / "a.wav", sine(RATE // 4, 440.0))
    p = deep / "x.exs"
    p.write_bytes(b"not an exs at all" * 10)
    f, err = sf_open(lib, p)
    assert not f and "x.exs" in err and "EXS" in err, err
    write_exs(p, [], (), [sample("a.wav")], params([]))
    f, err = sf_open(lib, p)
    assert not f and "no zones" in err, err
    write_exs(p, [zone(sample=0)], (), [sample("gone.wav")], params([]))
    f, err = sf_open(lib, p)
    assert not f and "Missing 1 sample (first: gone.wav)" in err, err
    good = write_exs(deep / "good.exs",
                     [zone(key=60, lo=0, hi=127, loop=(10, 2000), group=0, sample=0), zone(key=64, sample=5),
                      zone(key=65, group=99, sample=0)],
                     [group(by=2, rr=7), group(short=True)], [sample("a.wav"), sample("é" * 300)],
                     params([(0x52, 127), (0x51, -5)], wide=[(0x16d, 65535)])).read_bytes()
    rng = random.Random(7)
    opened_n = 0
    variants = [good[:n] for n in (84, 85, 100, 179, 180, 200, len(good) - 1)]
    for _ in range(150):
        b = bytearray(good)
        for _ in range(rng.randint(1, 12)):
            i = rng.randrange(len(b))
            b[i] = rng.choice([0, 255, 0x7F, 0x80, rng.randrange(256)])
        variants.append(bytes(b))
    big = bytearray(good)
    big[4:8] = struct.pack("<I", 0xFFFFFFF0)
    variants.append(bytes(big))
    for k, v in enumerate(variants):
        p.write_bytes(v)
        f, err = sf_open(lib, p)
        if f:
            opened_n += 1
            walk(lib, f)
            ignored(lib, f)
            lib.smp_sf_problem(f)
            if k % 9 == 0:
                load_all(lib, f)
            lib.smp_sf_close(f)
        else:
            assert err
    assert opened_n > 0
