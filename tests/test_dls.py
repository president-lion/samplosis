"""DLS level 1 and 2 (sfdls.c): instruments become presets, regions become regions, art1 and art2 become SF2
generators and modulators, and the wave pool's PCM and ADPCM waves become samples."""
import ctypes
import os
import random

import numpy as np
import pytest

import dlsmake as D
from test_sf2 import load_all, mods, peak_hz, presets, regions, sf_open, walk_font
from test_sffont import pcm

GM_DLS = r"C:\Windows\System32\drivers\gm.dls"


def sine(n, freq, rate, amp=12000):
    t = np.arange(n) / rate
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.int16)


def tc(seconds):
    return 1200 * np.log2(seconds)


WAVES = [
    # 0: 16-bit, 441 Hz (100 frames a cycle), unity A4, looped over whole cycles
    dict(name="Sine 441", data=sine(44100, 441.0, 44100), rate=44100, wsmp=dict(unity=69, loop=(4400, 30000, 0))),
    # 1: 8-bit unsigned, 220.5 Hz, unity A3, 6 dB quieter by its gain
    dict(name="Sine 8", data=(128 + sine(11025, 220.5, 22050) / 256).astype(np.uint8), rate=22050,
         wsmp=dict(unity=57, gain=-60)),
    # 2: Microsoft ADPCM, 330.75 Hz, no wsmp of its own (each region gives one)
    dict(name="Adpcm", data=sine(11025, 330.75, 22050), rate=22050, adpcm=True),
    # 3: a short drum
    dict(name="Drum", data=sine(2205, 110.25, 22050), rate=22050, wsmp=dict(unity=36)),
]

INSTRUMENTS = [
    # written out of order on purpose: the font lists them sorted by bank, then program
    dict(name="Drums", program=0, drum=True,
         regions=[dict(keys=(36, 36), wave=3, keygroup=1), dict(keys=(42, 42), wave=3, keygroup=2)]),
    dict(name="Organ", program=16, bank=8,
         regions=[dict(keys=(0, 127), wave=2, wsmp=dict(unity=64, fine=50))]),
    dict(name="Piano", program=0, regions=[dict(keys=(0, 127), wave=0)]),
    dict(name="Guitar", program=25, regions=[dict(keys=(0, 127), wave=1)]),
    dict(name="Lsb", program=3, lsb=3, regions=[dict(keys=(0, 127), wave=0)]),
]


def make_dls(path):
    D.write_dls(str(path), INSTRUMENTS, WAVES)
    return str(path)


def note(lib, f, preset, key, n=24000, release_at=-1, vel=100):
    out = np.zeros(2 * n, np.float32)
    assert lib.smp_sf_test_note(f, preset, key, vel, 48000.0, n, release_at,
                                out.ctypes.data_as(ctypes.POINTER(ctypes.c_float))) == 1
    return out[0::2].astype(np.float64), out[1::2].astype(np.float64)


def rms(x):
    return float(np.sqrt(np.mean(x ** 2)))


def test_dls_presets_and_pitch(lib, tmp_path):
    f, err = sf_open(lib, make_dls(tmp_path / "t.dls"))
    assert f, err
    try:
        assert presets(lib, f) == [(0, 0, "Piano"), (0, 25, "Guitar"), (3, 3, "Lsb"), (8, 16, "Organ"),
                                   (128, 0, "Drums")]
        assert lib.smp_sf_sample_count(f) == 4
        names = [lib.smp_sf_sample(f, i).contents.name.decode() for i in range(4)]
        assert names == ["Sine 441", "Sine 8", "Adpcm", "Drum"]
        h = lib.smp_sf_sample(f, 0).contents
        assert (h.start, h.end, h.rate, h.pitch) == (0, 44100, 44100, 69)
        assert (h.loop_start, h.loop_end) == (4400, 34400)
        walk_font(lib, f)

        (r,) = regions(lib, f, 0)                     # Piano: the wave's own wsmp
        assert (r.sample, r.gen[58], r.gen[54], r.gen[48]) == (0, 69, 1, 0)
        (r,) = regions(lib, f, 1)                     # Guitar: 8-bit, gain -6 dB is 60 cB (150 before the 0.4)
        assert (r.sample, r.gen[58], r.gen[54], r.gen[48]) == (1, 57, 0, 150)
        (r,) = regions(lib, f, 3)                     # Organ: the region's wsmp replaces the wave's
        assert (r.sample, r.gen[58], r.gen[52]) == (2, 64, 50)

        for preset, key, hz in ((0, 69, 441.0), (0, 81, 882.0), (1, 57, 220.5), (1, 69, 441.0),
                                (3, 64, 330.75 * 2 ** (50 / 1200)), (3, 76, 661.5 * 2 ** (50 / 1200))):
            left, right = note(lib, f, preset, key)
            x = left[2400:]
            assert rms(x) > 0.01, (preset, key)
            got = peak_hz(x, 48000)
            assert abs(got - hz) < hz * 0.004, (preset, key, got, hz)
        # the data: 16-bit exact, 8-bit unsigned exact (as 16-bit units), ADPCM decoded within -30 dB
        for i in range(4):
            lib.smp_sf_use(f, i, 1)
        assert lib.smp_sf_wait_idle(f, 5000) == 0
        head = pcm(lib, f, 0)[0]                             # a 1 s wave: its head is in memory, the rest streams
        assert np.array_equal(head, WAVES[0]["data"][:len(head)])
        assert np.array_equal(pcm(lib, f, 1)[0], (WAVES[1]["data"].astype(np.int32) - 128) * 256)
        got, want = pcm(lib, f, 2)[0].astype(np.float64), WAVES[2]["data"].astype(np.float64)
        assert len(got) >= len(want)
        err = got[:len(want)] - want
        assert 20 * np.log10(rms(err) / rms(want)) < -30
        for i in range(4):
            lib.smp_sf_use(f, i, -1)
        # the 8-bit wave's gain: 6 dB under the 16-bit one at the same level
        a = rms(note(lib, f, 0, 69)[0][2400:9600])           # (the 8-bit wave lasts a quarter second at key 69)
        b = rms(note(lib, f, 1, 69)[0][2400:9600])
        assert abs(20 * np.log10(b / a) + 6.0) < 0.5
    finally:
        lib.smp_sf_close(f)


def envelope(x, win=96):
    n = len(x) // win
    return np.abs(x[:n * win]).reshape(n, win).max(axis=1), win / 48000.0


def crossing(env, level, start=0, falling=False):
    """The first fractional window index at or after start where env crosses level."""
    for i in range(max(start, 1), len(env)):
        a, b = env[i - 1], env[i]
        if (not falling and a < level <= b) or (falling and a > level >= b):
            return i - 1 + (level - a) / (b - a)
    raise AssertionError("no crossing")


ENV_INS = [dict(name="Env", program=0, art=[(D.SRC_NONE, 0, D.DST_EG1_ATTACK, 0, tc(0.2)),
                                             (D.SRC_NONE, 0, D.DST_EG1_RELEASE, 0, tc(0.5)),
                                             (D.SRC_NONE, 0, D.DST_EG1_SUSTAIN, 0, 1000)],
                regions=[dict(keys=(0, 127), wave=0)])]


def test_dls_envelope_from_art1(lib, tmp_path):
    path = str(tmp_path / "env.dls")
    D.write_dls(path, ENV_INS, WAVES[:1])
    f, err = sf_open(lib, path)
    assert f, err
    try:
        (r,) = regions(lib, f, 0)
        assert r.gen[34] == round(tc(0.2)) and r.gen[38] == round(tc(0.5)) and r.gen[37] == 0
        left, _ = note(lib, f, 0, 69, n=96000, release_at=48000)
        env, dt = envelope(left)
        peak = env[int(0.5 / dt):int(0.95 / dt)].mean()
        t10, t90 = crossing(env, 0.1 * peak), crossing(env, 0.9 * peak)
        attack = (t90 - t10) * dt / 0.8                     # linear in amplitude
        assert abs(attack - 0.2) < 0.2 * 0.05, attack
        rel = int(1.0 / dt)
        a = crossing(env, peak * 10 ** (-6 / 20), rel, falling=True)
        b = crossing(env, peak * 10 ** (-46 / 20), rel, falling=True)
        release = (b - a) * dt * 96 / 40                    # 96 dB in the release time, linear in dB
        assert abs(release - 0.5) < 0.5 * 0.05, release
    finally:
        lib.smp_sf_close(f)


def test_dls_articulation_units(lib, tmp_path):
    """The units of art1 and art2: key to decay scaling, sustain, the DLS2 filter, the LFOs (DLS1's one LFO is the
    vibrato), velocity and controller modulators, and region articulation replacing the instrument's whole."""
    art1 = [(D.SRC_NONE, 0, D.DST_LFO_FREQ, 0, -783), (D.SRC_NONE, 0, D.DST_LFO_DELAY, 0, -1902),
            (D.SRC_LFO, 0, D.DST_PITCH, 0, 12), (D.SRC_LFO, D.SRC_CC1, D.DST_PITCH, 0, 47.2),
            (D.SRC_VELOCITY, 0, D.DST_GAIN, 1, -96 * 10),          # DLS1's concave velocity curve, no invert bit
            (D.SRC_NONE, 0, D.DST_EG1_ATTACK, 0, tc(0.2)), (D.SRC_NONE, 0, D.DST_EG1_RELEASE, 0, tc(0.5)),
            (D.SRC_NONE, 0, D.DST_EG1_SUSTAIN, 0, 0), (D.SRC_NONE, 0, D.DST_EG2_ATTACK, 0, ("raw", -0x80000000))]
    art2 = [(D.SRC_NONE, 0, D.DST_EG1_ATTACK, 0, tc(0.05)), (D.SRC_NONE, 0, D.DST_EG1_DECAY, 0, 6387),
            (D.SRC_KEY, 0, D.DST_EG1_DECAY, 0, -3979), (D.SRC_NONE, 0, D.DST_EG1_SUSTAIN, 0, 500),
            (D.SRC_NONE, 0, D.DST_FILTER_CUTOFF, 0, 6000), (D.SRC_NONE, 0, D.DST_FILTER_Q, 0, 60),
            (D.SRC_NONE, 0, D.DST_LFO_FREQ, 0, -500), (D.SRC_LFO, 0, D.DST_PITCH, 0, 20),
            (D.SRC_EG2, 0, D.DST_FILTER_CUTOFF, 0, 2400), (D.SRC_NONE, 0, D.DST_PAN, 0, -250),
            (D.SRC_NONE, 0, D.DST_GAIN, 0, -20), (D.SRC_NONE, 0, D.DST_PITCH, 0, 130),
            (0x9999, 0, D.DST_PITCH, 0, 5)]                       # an unknown source: ignored
    ins = [dict(name="A", program=0, art=art1, level=1,
                regions=[dict(keys=(0, 63), wave=0), dict(keys=(64, 127), wave=0, art=art2, level=2)])]
    path = str(tmp_path / "u.dls")
    D.write_dls(path, ins, WAVES[:1])
    f, err = sf_open(lib, path)
    assert f, err
    try:
        lo, hi = regions(lib, f, 0)
        # DLS1: one LFO, the vibrato; the depth and the CC1 modulator go there
        assert (lo.gen[24], lo.gen[23], lo.gen[6], lo.gen[5]) == (-783, -1902, 12, 0)
        assert (lo.gen[34], lo.gen[38], lo.gen[37], lo.gen[26]) == (round(tc(0.2)), round(tc(0.5)), 1000, -12000)
        ml = mods(lo.imod, lo.nimod)
        assert (0x0081, 6, 47, 0, 0) in ml
        assert (0x0502, 48, 960, 0, 0) in ml                     # the same as SF2's default velocity modulator
        assert (0x00DB, 16, 1000, 0, 0) in ml and (0x00DD, 15, 1000, 0, 0) in ml   # DLS's reverb and chorus
        # DLS2 region articulation: replaces the instrument's (attack set, release back to the default)
        assert (hi.gen[34], hi.gen[38]) == (round(tc(0.05)), -12000)
        assert hi.gen[40] == round(3979 / 128)                   # key to decay: -scale / 128 per key below 60
        assert hi.gen[36] == 6387 + round(60 * -3979 / 128)      # and the decay at key 0 moved to key 60
        assert hi.gen[37] == 500
        assert (hi.gen[8], hi.gen[9]) == (6000, 60)
        assert (hi.gen[22], hi.gen[5], hi.gen[6], hi.gen[11]) == (-500, 20, 0, 2400)
        assert hi.gen[17] == -250
        assert hi.gen[48] == 50                                  # 20 cB of gain cut, before the 0.4
        assert (hi.gen[51], hi.gen[52]) == (1, 30)               # 130 cents
    finally:
        lib.smp_sf_close(f)


def test_dls_drums_bank(lib, tmp_path):
    f, err = sf_open(lib, make_dls(tmp_path / "d.dls"))
    assert f, err
    try:
        p = lib.smp_sf_find_preset(f, 128, 0)
        assert p == 4
        rs = regions(lib, f, p)
        assert [(r.lokey, r.hikey, r.gen[57]) for r in rs] == [(36, 36, 1), (42, 42, 2)]
        assert lib.smp_sf_find_preset(f, 8, 16) == 3
        assert lib.smp_sf_find_preset(f, 3, 3) == 2
        left, _ = note(lib, f, p, 36, n=4800)
        assert rms(left) > 0.01
    finally:
        lib.smp_sf_close(f)


def test_dls_conditional_chunks(lib, tmp_path):
    """cdl chunks: a region whose condition is false is left out, and so is an instrument; true ones stay."""
    ins = [dict(name="Cond", program=0,
                regions=[dict(keys=(0, 59), wave=0, cdl=[("query", D.GUID_SUPPORTS_DLS2)]),
                         dict(keys=(60, 127), wave=0, cdl=[("query", D.GUID_GM_IN_HARDWARE)])]),
           dict(name="Gone", program=1, cdl=[("query", D.GUID_SUPPORTS_DLS1), (D.CDL_NOT,)],
                regions=[dict(keys=(0, 127), wave=0)]),
           dict(name="Kept", program=2, cdl=[("supported", D.GUID_SUPPORTS_DLS2), ("const", 1), (D.CDL_AND,)],
                regions=[dict(keys=(0, 127), wave=0)])]
    path = str(tmp_path / "c.dls")
    D.write_dls(path, ins, WAVES[:1])
    f, err = sf_open(lib, path)
    assert f, err
    try:
        assert presets(lib, f) == [(0, 0, "Cond"), (0, 2, "Kept")]
        assert [(r.lokey, r.hikey) for r in regions(lib, f, 0)] == [(0, 59)]
    finally:
        lib.smp_sf_close(f)


def test_dls_not_readable(lib, tmp_path):
    path = tmp_path / "bad.dls"
    good = open(make_dls(tmp_path / "g.dls"), "rb").read()
    path.write_bytes(good[:12] + b"colh\x04\x00\x00\x00\x00\x00\x00\x00")
    f, err = sf_open(lib, path)
    assert not f
    assert err.startswith("bad.dls isn't a DLS file this can read ("), err


def test_dls_corrupt(lib, tmp_path):
    """Every 97th truncation and seeded random corruption: a font or NULL with a reason, never a crash; every 7th
    variant that opens loads its samples too (the ADPCM wave decodes through decode_memory)."""
    data = open(make_dls(tmp_path / "good.dls"), "rb").read()
    variants = [data[:cut] for cut in range(0, len(data), 97)]
    rng = random.Random(4321)
    for _ in range(300):
        b = bytearray(data)
        for _ in range(8):
            b[rng.randrange(len(b))] = rng.randrange(256)
        variants.append(bytes(b))
    path = tmp_path / "bad.dls"
    opened = 0
    for k, v in enumerate(variants):
        path.write_bytes(v)
        f, err = sf_open(lib, path)
        if f:
            opened += 1
            walk_font(lib, f)
            if k % 7 == 0:
                load_all(lib, f)
            lib.smp_sf_close(f)
        else:
            assert err
    assert opened > 0


@pytest.mark.skipif(not os.path.exists(GM_DLS), reason="Windows' gm.dls isn't here")
def test_windows_gm_dls_opens(lib):
    f, err = sf_open(lib, GM_DLS)
    assert f, err
    try:
        ps = presets(lib, f)
        assert len(ps) >= 200
        assert (0, 0) in [(b, p) for b, p, _ in ps]
        assert 128 in [b for b, _, _ in ps]
        walk_font(lib, f)
        p = lib.smp_sf_find_preset(f, 0, 0)
        left, right = note(lib, f, p, 60, n=48000, release_at=36000)
        assert rms(left[:36000]) > 0.01 and rms(right[:36000]) > 0.01
        got = peak_hz(left[2400:36000], 48000)
        assert any(abs(got - 261.63 * h) < 261.63 * h * 0.02 for h in (1, 2, 3)), got
    finally:
        lib.smp_sf_close(f)
