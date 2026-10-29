"""Region conditions in the SoundFont player (Task 8): round robin, random, keyswitches, release triggers,
first and legato triggers, CC ranges, crossfades, group and off_by, and amp_veltrack. Each test builds a font in
memory with the test-only smp_sf_test_font (samples are sines generated in C), loads it into a player and plays
it through smp_synth_render_test. Every sample is a sine of its own frequency, so which regions sound is read
from the spectrum."""
import ctypes
import itertools
import math

import numpy as np
import pytest
import soundfile as sf

OUT = 48000     # the output rate
P_EFFECTS = 33
_count = itertools.count()

# the samples every test font has: sine frequencies at root key 60, so key 60 plays them as they are
FREQS = [300.0, 500.0, 700.0, 900.0]


def sample_lines(secs=1.0, freqs=FREQS):
    return "\n".join("sample s%d %d 44100 60 sine %g 0.5" % (i, int(secs * 44100), f) for i, f in enumerate(freqs))


@pytest.fixture
def T(lib, tmp_path):
    class H:
        synths = []

        def synth(self, regions, samples=None, seed=None):
            """a player on a one-preset font (bank 0 program 0) with these region lines"""
            desc = (samples or sample_lines()) + "\npreset 0 0 Test\n" + "\n".join("region " + r for r in regions) + "\n"
            path = tmp_path / ("t%d.smptest" % next(_count))
            err = ctypes.create_string_buffer(512)
            f = lib.smp_sf_test_font(desc.encode(), str(path).encode(), err, 512)
            assert f, err.value
            s = lib.smp_synth_new(float(OUT), 256)
            assert s
            self.synths.append(s)
            lib.smp_synth_set_param(s, P_EFFECTS, 0)
            assert lib.smp_synth_load(s, str(path).encode(), err, 512) == 1, err.value
            assert lib.smp_synth_font(s) == f          # the registry gave the player the same font
            lib.smp_sf_close(f)                         # the player holds its own reference
            if seed is not None:
                lib.smp_synth_test_seed(s, seed)
            return s

        def render(self, s, events, secs):
            out = tmp_path / ("r%d.wav" % next(_count))
            frames = int(secs * OUT)
            assert lib.smp_synth_render_test(s, events.encode(), frames, 256, str(out).encode(), 0) == 0
            x, rate = sf.read(str(out), dtype="float32", always_2d=True)
            assert np.all(np.isfinite(x))
            return x[:, 0]

    h = H()
    yield h
    for s in h.synths:
        lib.smp_synth_free(s)


def fr(t):
    return int(round(t * OUT))


def on(key, vel, t, ch=0):
    return "midi %d %d %d %d" % (0x90 | ch, key, vel, fr(t))


def off(key, t, ch=0):
    return "midi %d %d 0 %d" % (0x80 | ch, key, fr(t))


def cc(n, v, t, ch=0):
    return "midi %d %d %d %d" % (0xB0 | ch, n, v, fr(t))


def tone(x, f):
    """the amplitude of frequency f in x (Hann-weighted)"""
    w = np.hanning(len(x))
    t = np.arange(len(x)) / OUT
    return 2 * abs(np.sum(x * w * np.exp(-2j * np.pi * f * t))) / np.sum(w)


def win(x, a, b):
    return x[fr(a):fr(b)]


def heard(x, a, b, th=0.02):
    """the sample indices whose frequency sounds in [a, b)"""
    w = win(x, a, b)
    return [i for i, f in enumerate(FREQS) if tone(w, f) > th]


def test_round_robin_seq(T):
    # key 60: three regions in turn; key 62: a set of two (its own counter)
    s = T.synth(["sample=0 lokey=60 hikey=60 seq_len=3 seq_pos=1",
                 "sample=1 lokey=60 hikey=60 seq_len=3 seq_pos=2",
                 "sample=2 lokey=60 hikey=60 seq_len=3 seq_pos=3",
                 "sample=0 lokey=62 hikey=62 seq_len=2 seq_pos=1 g51=0",
                 "sample=1 lokey=62 hikey=62 seq_len=2 seq_pos=2"])
    keys = [60, 60, 62, 60, 60, 62, 62, 60, 60]
    ev = []
    for i, k in enumerate(keys):
        ev += [on(k, 100, 0.2 * i), off(k, 0.2 * i + 0.12)]
    x = T.render(s, "\n".join(ev), 0.2 * len(keys) + 0.1)
    got = [heard(x, 0.2 * i + 0.03, 0.2 * i + 0.11) for i in range(len(keys))]
    # key 60's set cycles 0 1 2 0 1 2; key 62's 0 1 0, untouched by the other set's notes. Key 62 plays its
    # samples a whole tone up (62 vs root 60), so they are read at their own frequencies scaled
    exp60 = iter([0, 1, 2, 0, 1, 2])
    exp62 = iter([0, 1, 0])
    for i, k in enumerate(keys):
        w = win(x, 0.2 * i + 0.03, 0.2 * i + 0.11)
        if k == 60:
            assert got[i] == [next(exp60)], (i, got)
        else:
            want = next(exp62)
            up = 2 ** (2 / 12)
            amps = [tone(w, f * up) for f in FREQS]
            assert amps[want] > 0.02 and all(a < 0.005 for j, a in enumerate(amps) if j != want), (i, amps)


def test_round_robin_counts_note_ons_not_regions(T):
    """two layers in each step of the same set: the counter still advances once per note-on"""
    s = T.synth(["sample=0 seq_len=2 seq_pos=1", "sample=2 seq_len=2 seq_pos=1",
                 "sample=1 seq_len=2 seq_pos=2", "sample=3 seq_len=2 seq_pos=2"])
    ev = []
    for i in range(4):
        ev += [on(60, 100, 0.2 * i), off(60, 0.2 * i + 0.12)]
    x = T.render(s, "\n".join(ev), 0.9)
    assert [heard(x, 0.2 * i + 0.03, 0.2 * i + 0.11) for i in range(4)] == [[0, 2], [1, 3], [0, 2], [1, 3]]


def test_random_split(lib, T):
    regions = ["sample=0 lorand=0 hirand=0.5", "sample=1 lorand=0.5 hirand=1",
               "sample=2 lorand=0 hirand=0.5"]          # the same range as sample 0: the draw is shared
    n = 40

    def play(seed):
        s = T.synth(regions, seed=seed)
        ev = []
        for i in range(n):
            ev += [on(60, 100, 0.1 * i), off(60, 0.1 * i + 0.07)]
        x = T.render(s, "\n".join(ev), 0.1 * n + 0.05)
        return [tuple(heard(x, 0.1 * i + 0.015, 0.1 * i + 0.065)) for i in range(n)]

    got = play(1234)
    assert all(g in ((0, 2), (1,)) for g in got), got     # exactly one side, and 0 always with 2
    lows = sum(1 for g in got if g == (0, 2))
    assert 8 <= lows <= 32, lows
    assert play(1234) == got                              # seeded: the same draws again
    assert play(99) != got


def test_keyswitch_last(T):
    sw = "sw_lokey=24 sw_hikey=26 sw_default=24 lokey=48 hikey=96"
    s = T.synth(["sample=0 sw_last=24 " + sw, "sample=1 sw_last=25 " + sw,
                 "sample=2 lokey=0 hikey=127"])          # a plain region over every key, keyswitches included
    ev = [on(60, 100, 0.0), off(60, 0.1),                  # nothing pressed yet: sw_default (sample 0)
          on(25, 100, 0.2), off(25, 0.3),                  # a keyswitch: silent, even under the plain region
          on(60, 100, 0.4), off(60, 0.5),                  # now sample 1
          on(24, 100, 0.6), off(24, 0.65),
          on(60, 100, 0.8), off(60, 0.9)]                  # back to sample 0
    x = T.render(s, "\n".join(ev), 1.0)
    assert heard(x, 0.02, 0.09) == [0, 2]
    assert heard(x, 0.22, 0.29, th=0.002) == []
    assert heard(x, 0.42, 0.49) == [1, 2]
    assert heard(x, 0.82, 0.89) == [0, 2]


def test_keyswitch_down(T):
    s = T.synth(["sample=0 sw_down=30 lokey=48 hikey=96",
                 "sample=1 sw_up=30 lokey=48 hikey=96",
                 "sample=2 sw_previous=62 lokey=48 hikey=96"])
    ev = [on(60, 100, 0.0), off(60, 0.1),                  # 30 up: sample 1
          on(30, 100, 0.2),                                # held from here (silent)
          on(60, 100, 0.3), off(60, 0.4),                  # 30 down: sample 0
          off(30, 0.5),
          on(62, 100, 0.6), off(62, 0.65),                 # then 60 after 62: sw_previous adds sample 2
          on(60, 100, 0.7), off(60, 0.8)]
    x = T.render(s, "\n".join(ev), 0.9)
    assert heard(x, 0.02, 0.09) == [1]
    assert heard(x, 0.21, 0.29, th=0.002) == []
    assert heard(x, 0.32, 0.39) == [0]
    assert heard(x, 0.72, 0.79) == [1, 2]


def test_release_trigger(T):
    rel = "sample=1 trigger=release rt_decay=%g g38=-12000"
    regions = ["sample=0 g38=-12000", rel % 6.0]
    s = T.synth(regions, samples=sample_lines(2.0))
    # held 0.2 s, then 1.2 s: the release sample plays at note-off, 6 dB quieter per second held
    x = T.render(s, "\n".join([on(60, 100, 0.0), off(60, 0.2)]), 0.8)
    assert heard(x, 0.03, 0.19) == [0]                    # the attack region alone while held
    assert heard(x, 0.23, 0.35) == [1]                    # the release sample after note-off
    assert tone(win(x, 0.6, 0.75), 500) > 0.02             # not released by its own note-off: it plays on
    a_short = tone(win(x, 0.23, 0.35), 500)
    x = T.render(s, "\n".join([on(60, 100, 0.0), off(60, 1.2)]), 1.5)
    assert heard(x, 1.0, 1.15) == [0]
    a_long = tone(win(x, 1.23, 1.35), 500)
    assert abs(20 * math.log10(a_long / a_short) - (-6.0)) < 0.3, (a_short, a_long)

    # at the note-on's velocity: with no decay, the release sample is exactly as loud as the attack sample was
    s0 = T.synth(["sample=0 g38=-12000", rel % 0.0], samples=sample_lines(2.0))
    for vel in (127, 50):
        x = T.render(s0, "\n".join([on(60, vel, 0.0), off(60, 0.2)]), 0.4)
        att, rl = tone(win(x, 0.05, 0.18), 300), tone(win(x, 0.23, 0.35), 500)
        assert abs(20 * math.log10(rl / att)) < 0.2, (vel, att, rl)

    # under the sustain pedal it waits for the pedal to come up, and the time held counts to then
    x = T.render(s, "\n".join([cc(64, 127, 0.0), on(60, 100, 0.0), off(60, 0.1), cc(64, 0, 0.6)]), 1.0)
    assert tone(win(x, 0.15, 0.55), 500) < 0.002
    a_ped = tone(win(x, 0.63, 0.75), 500)
    assert abs(20 * math.log10(a_ped / a_short) - (-6.0 * 0.4)) < 0.3, (a_short, a_ped)


def test_trigger_first_legato(T):
    s = T.synth(["sample=0 trigger=first", "sample=1 trigger=legato"])
    ev = [on(60, 100, 0.0),                                # alone: first
          on(64, 100, 0.2),                                # 60 still held: legato
          off(60, 0.3), off(64, 0.35),
          on(67, 100, 0.5), off(67, 0.6)]                  # alone again: first
    x = T.render(s, "\n".join(ev), 0.7)
    up = 2 ** (4 / 12)
    w = win(x, 0.03, 0.18)
    assert tone(w, 300) > 0.02 and tone(w, 500) < 0.005
    w = win(x, 0.22, 0.29)                                 # key 64 legato (500 Hz up 4 semitones) and 60's first
    assert tone(w, 500 * up) > 0.02 and tone(w, 300 * up) < 0.005 and tone(w, 300) > 0.02
    up7 = 2 ** (7 / 12)
    w = win(x, 0.52, 0.59)
    assert tone(w, 300 * up7) > 0.02 and tone(w, 500 * up7) < 0.005


def test_cc_range(T):
    s = T.synth(["sample=0 locc1=0 hicc1=63", "sample=1 locc1=64 hicc1=127",
                 "sample=2 locc1=64 hicc1=127 locc11=0 hicc11=100"])   # two ranges: both must hold
    ev = [on(60, 100, 0.0), off(60, 0.1),                  # CC1 0 after the reset
          cc(1, 100, 0.15), on(60, 100, 0.2), off(60, 0.3),
          cc(11, 90, 0.35), on(60, 100, 0.4),
          cc(1, 0, 0.45),                                  # checked at note-on only: the note keeps sounding
          off(60, 0.55)]
    x = T.render(s, "\n".join(ev), 0.65)
    assert heard(x, 0.02, 0.09) == [0]
    assert heard(x, 0.22, 0.29) == [1]
    assert heard(x, 0.42, 0.44) == [1, 2]
    assert heard(x, 0.47, 0.54) == [1, 2]


def test_crossfade_velocity(T):
    xf = "xfout_lovel=40 xfout_hivel=100 amp_veltrack=0"
    xi = "xfin_lovel=40 xfin_hivel=100 amp_veltrack=0"
    for curve, mid in (("power", math.sqrt(0.5)), ("gain", 0.5)):
        s = T.synth(["sample=0 %s xf_curve=%s" % (xf, curve), "sample=1 %s xf_curve=%s" % (xi, curve)])
        amps = {}
        for i, vel in enumerate((30, 70, 110, 55)):
            x = T.render(s, "\n".join([on(60, vel, 0.0), off(60, 0.2)]), 0.25)
            w = win(x, 0.03, 0.18)
            amps[vel] = (tone(w, 300), tone(w, 500))
        full = amps[30][0]
        assert full > 0.05 and amps[30][1] < 1e-3            # below the fade: the fade-out region alone
        assert amps[110][0] < 1e-3 and abs(amps[110][1] / full - 1) < 0.01
        a, b = amps[70]                                      # the midpoint
        assert abs(a / full - mid) < 0.01 and abs(b / full - mid) < 0.01, (curve, a / full, b / full)
        if curve == "power":                                 # equal power at every point of the fade
            for vel in (70, 55):
                a, b = amps[vel]
                assert abs((a * a + b * b) / (full * full) - 1) < 0.02, (vel, a, b)


def test_group_off_by(T):
    # key 46 is an open hi-hat (group 1, turned off by group 2); key 42 a closed one (group 2)
    for mode, gone in (("fast", True), ("normal", False)):
        s = T.synth(["sample=0 lokey=46 hikey=46 g58=46 group=1 off_by=2 off_mode=%s g38=0" % mode,
                     "sample=1 lokey=42 hikey=42 g58=42 group=2",
                     "sample=2 lokey=50 hikey=50 g58=50 group=3"])   # another group: never turned off
        ev = [on(46, 100, 0.0), on(50, 100, 0.0), on(42, 100, 0.2)]
        x = T.render(s, "\n".join(ev), 0.5)
        assert tone(win(x, 0.05, 0.18), 300) > 0.02
        assert tone(win(x, 0.25, 0.4), 500) > 0.02
        assert tone(win(x, 0.25, 0.4), 700) > 0.02
        a = tone(win(x, 0.22, 0.3), 300)
        if gone:
            assert a < 1e-3, a                                 # the 5 ms kill
        else:
            assert 0.002 < a < tone(win(x, 0.05, 0.18), 300), a   # its 1 s release, falling


def test_amp_veltrack(T):
    for vt, want_db in ((0, 0.0), (100, 40 * math.log10(30 / 127)), (50, 20 * math.log10(1 - 0.5 * (1 - (30 / 127) ** 2)))):
        s = T.synth(["sample=0 amp_veltrack=%g" % vt])
        amp = []
        for vel in (127, 30):
            x = T.render(s, "\n".join([on(60, vel, 0.0), off(60, 0.2)]), 0.25)
            amp.append(tone(win(x, 0.03, 0.18), 300))
        assert abs(20 * math.log10(amp[1] / amp[0]) - want_db) < 0.1, (vt, amp)


def test_test_font_regions_default_conditions(lib, T):
    """a region with no condition reads back with every condition at its "always" default"""
    s = T.synth(["sample=0"])
    f = lib.smp_synth_font(s)
    from conftest import SmpSfRegion
    out = ctypes.POINTER(SmpSfRegion)()
    assert lib.smp_sf_regions(f, 0, ctypes.byref(out)) == 1
    c = out[0].cond
    assert c.trigger == 0 and c.seq_len == 0 and c.lorand == 0 and c.hirand == 1
    assert (c.sw_lokey, c.sw_hikey, c.sw_last, c.sw_down, c.sw_up, c.sw_previous, c.sw_default) == (-1,) * 7
    assert c.locc_n == 0 and (c.xfin_lokey, c.xfin_hikey, c.xfout_lokey, c.xfout_hikey) == (-1,) * 4
    assert (c.xfin_lovel, c.xfin_hivel, c.xfout_lovel, c.xfout_hivel) == (0, 0, 127, 127)
    assert c.group == 0 and c.off_by == 0 and c.rt_decay == 0 and math.isnan(c.amp_veltrack)


def test_sf2_regions_default_conditions(lib, tmp_path):
    """SF2 regions carry the defaults, so the SoundFont family plays as before"""
    from sf2make import write_sf2
    from conftest import SmpSfRegion
    data = (16000 * np.sin(np.arange(4410) / 10)).astype(np.int16)
    p = tmp_path / "one.sf2"
    write_sf2(p, [dict(name="P", bank=0, program=0, gens={}, pmods=[],
                       instruments=[dict(name="I", gens={}, mods=[], zones=[dict(keys=(0, 127), vels=(0, 127), sample=0, gens={}, mods=[])])])],
              [dict(name="s", data=data, rate=44100, pitch=60, loop=None)])
    err = ctypes.create_string_buffer(256)
    f = lib.smp_sf_open(str(p).encode(), err, 256)
    assert f, err.value
    try:
        out = ctypes.POINTER(SmpSfRegion)()
        assert lib.smp_sf_regions(f, 0, ctypes.byref(out)) == 1
        c = out[0].cond
        assert c.seq_len == 0 and c.hirand == 1 and c.sw_last == -1 and c.xfout_hivel == 127
        assert math.isnan(c.amp_veltrack) and out[0].rr_set == -1
    finally:
        lib.smp_sf_close(f)
