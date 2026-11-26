import ctypes
import math

import numpy as np
import psutil
import soundfile as sf

from conftest import write_wav
from test_cache import point_cache

FP = ctypes.POINTER(ctypes.c_float)

ROOT, TRANSPOSE, FINE, VOLUME, PAN = 0, 1, 2, 3, 4
ATTACK, DECAY, SUSTAIN, RELEASE = 5, 6, 7, 8
VELOCITY_SENS, MIN_VOLUME, MODE = 9, 10, 11
NOTE_LO, NOTE_HI, PITCH_LO, VEL_LO, VEL_HI = 12, 13, 14, 15, 16
OBEY_NOTE_OFF, LOOP, LOOP_START, START, END = 17, 18, 19, 20, 21
BEND_RANGE, PITCH_HI, REVERSE, KEEP_LENGTH = 22, 23, 24, 25
VOICES, PROBABILITY, SPEED = 26, 27, 28
GLIDE, GLIDE_MODE = 29, 30


def sine(freq, secs, rate=44100, amp=0.5):
    t = np.arange(int(round(secs * rate))) / rate
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def detect(lib, x, rate):
    x = np.ascontiguousarray(x, dtype=np.float32)
    return lib.smp_detect_pitch(x.ctypes.data_as(FP), len(x), rate)


def midi(f):
    return 69 + 12 * math.log2(f / 440)


def rms_db(x):
    return 20 * math.log10(math.sqrt(float(np.mean(np.square(x, dtype=np.float64)))) + 1e-12)


class Engine:
    def __init__(self, lib, tmp_path, data, rate=44100, name="tone A3.wav", host_rate=48000, max_block=512,
                 declick=True):
        self.lib, self.tmp, self.host_rate = lib, tmp_path, host_rate
        point_cache(lib, tmp_path / "cache", declick=declick)   # off: the output is the data, frame for frame
        f = tmp_path / name
        write_wav(f, data, rate)
        err = ctypes.create_string_buffer(512)
        s = lib.smp_sample_load(str(f).encode(), err, 512)
        assert s, err.value
        self.root = s.contents.root
        self.e = lib.smp_engine_new(host_rate, max_block)
        assert self.e
        lib.smp_engine_set_sample(self.e, s)  # the engine owns the sample now
        self.set(KEEP_LENGTH, 0)
        self.n = 0

    def set(self, pid, v):
        self.lib.smp_engine_set_param(self.e, pid, v)

    def render(self, events, secs, block=256):
        self.n += 1
        out = self.tmp / f"out{self.n}.wav"
        frames = int(round(secs * self.host_rate))
        assert self.lib.smp_engine_render_test(self.e, events.encode(), frames, block, str(out).encode()) == 0
        y, r = sf.read(str(out), dtype="float32")
        assert r == self.host_rate and y.shape == (frames, 2)
        return y

    def free(self):
        self.lib.smp_engine_free(self.e)


def secs_to(e, t):
    return int(round(t * e.host_rate))


def test_engine_defaults_and_clamp(lib, tmp_path):
    e = Engine(lib, tmp_path, sine(220, 1.0))
    assert e.lib.smp_engine_get_param(e.e, ROOT) == 57      # the sample's root
    assert e.lib.smp_engine_get_param(e.e, RELEASE) == 30
    e.set(VOLUME, 99)
    assert e.lib.smp_engine_get_param(e.e, VOLUME) == 12
    e.free()


def test_note_mode_pitch(lib, tmp_path):
    e = Engine(lib, tmp_path, sine(220, 1.0))
    assert e.root == 57
    y = e.render("on 64 127 0", 0.8)
    m = detect(lib, y[secs_to(e, 0.1):secs_to(e, 0.6), 0], e.host_rate)
    assert abs(m - midi(329.63)) < 0.1, m
    e.free()


def test_sample_mode_ignores_key(lib, tmp_path):
    e = Engine(lib, tmp_path, sine(220, 1.0))
    e.set(MODE, 0)
    for key in (40, 57, 90):
        y = e.render(f"on {key} 127 0", 0.6)
        m = detect(lib, y[secs_to(e, 0.1):secs_to(e, 0.5), 0], e.host_rate)
        assert abs(m - midi(220)) < 0.1, (key, m)
    e.free()


def test_free_mode(lib, tmp_path):
    e = Engine(lib, tmp_path, sine(220, 1.0))
    e.set(MODE, 2)
    e.set(NOTE_LO, 48); e.set(NOTE_HI, 72); e.set(PITCH_LO, -12); e.set(PITCH_HI, 12)
    y = e.render("on 60 127 0", 0.6)
    assert abs(detect(lib, y[secs_to(e, 0.1):secs_to(e, 0.5), 0], e.host_rate) - midi(220)) < 0.1
    y = e.render("on 72 127 0", 0.6)
    assert abs(detect(lib, y[secs_to(e, 0.1):secs_to(e, 0.5), 0], e.host_rate) - midi(440)) < 0.1
    y = e.render("on 47 127 0", 0.3)                     # outside note_lo..note_hi: ignored
    assert np.max(np.abs(y)) == 0
    e.free()


def declick_pair(lib, tmp_path, data, events, secs, setup=None):
    """The same note with declick on and off (44.1 kHz in and out, so the frames line up)."""
    ys = []
    for on in (True, False):
        d = tmp_path / ("on" if on else "off")
        d.mkdir()
        e = Engine(lib, d, data, host_rate=44100)
        point_cache(lib, d / "cache", declick=on)
        e.set(OBEY_NOTE_OFF, 0)                           # plays to the end
        if setup:
            setup(e)
        ys.append(e.render(events % e.root, secs)[:, 0])
        e.free()
    point_cache(lib, tmp_path / "cache")
    return ys


FADE = int(0.002 * 44100)                                 # declick fades over 2 ms


def test_declick_fades_in_a_start_mid_wave(lib, tmp_path):
    n = np.arange(44100)
    on, off = declick_pair(lib, tmp_path, (0.5 * np.cos(2 * np.pi * n / 100)).astype(np.float32), "on %d 127 0", 1.2)
    assert abs(off[0]) > 0.4                              # without it: full level at once
    assert abs(on[0]) < 0.02 and np.all(np.abs(on[:FADE]) <= np.abs(off[:FADE]) + 1e-6)
    assert abs(on[FADE // 2]) < 0.6 * abs(off[FADE // 2])
    assert np.abs(on[FADE + 2:40000] - off[FADE + 2:40000]).max() < 1e-6   # after the fade: untouched


def test_declick_leaves_a_clean_start_alone(lib, tmp_path):
    on, off = declick_pair(lib, tmp_path, sine(441, 1.0, 44100), "on %d 127 0", 1.2)
    assert np.abs(on[:40000] - off[:40000]).max() < 1e-6  # starts at zero: its attack stays sharp


def test_declick_fades_out_an_end_cut_mid_wave(lib, tmp_path):
    n = np.arange(44125)                                  # ends a quarter period in: at the peak
    on, off = declick_pair(lib, tmp_path, (0.5 * np.sin(2 * np.pi * n / 100)).astype(np.float32), "on %d 127 0", 1.2)
    end = int(last_sound(off[:, None], 1))
    assert abs(off[end - 3]) > 0.4                         # without it: stops dead at the peak
    assert int(last_sound(on[:, None], 1)) <= end
    assert np.abs(on[end - 3:end + 3]).max() < 0.03
    assert np.abs(on[:end - FADE - 4] - off[:end - FADE - 4]).max() < 1e-6


def test_declick_start_point_mid_file(lib, tmp_path):
    n = np.arange(40000)
    data = (0.5 * np.sin(2 * np.pi * n / 100)).astype(np.float32)
    on, off = declick_pair(lib, tmp_path, data, "on %d 127 0", 1.0, lambda e: e.set(START, 10.0626))   # frame 4025: a peak
    assert abs(off[0]) > 0.4
    assert abs(on[0]) < 0.02
    assert np.abs(on[FADE + 2:30000] - off[FADE + 2:30000]).max() < 1e-6


def test_declick_reversed_start(lib, tmp_path):
    n = np.arange(40025)                                  # reversed, the note starts at the file's last frame: a peak
    data = (0.5 * np.sin(2 * np.pi * n / 100)).astype(np.float32)
    on, off = declick_pair(lib, tmp_path, data, "on %d 127 0", 1.0, lambda e: e.set(REVERSE, 1))
    assert abs(off[0]) > 0.4
    assert abs(on[0]) < 0.02


def last_sound(y, rate, floor=1e-4):
    idx = np.nonzero(np.abs(y[:, 0]) > floor)[0]
    return (idx[-1] + 1) / rate if len(idx) else 0.0


def test_rate_conversion(lib, tmp_path):
    res = []
    for hr in (48000, 96000):
        d = tmp_path / str(hr)
        d.mkdir()
        e = Engine(lib, d, sine(220, 1.0), host_rate=hr)
        e.set(OBEY_NOTE_OFF, 0)
        y = e.render("on 57 127 0", 1.4)
        f = 440 * 2 ** ((detect(lib, y[secs_to(e, 0.1):secs_to(e, 0.6), 0], hr) - 69) / 12)
        res.append((f, last_sound(y, hr)))
        e.free()
    (f1, l1), (f2, l2) = res
    assert abs(f1 / 220 - 1) < 0.005 and abs(f2 / 220 - 1) < 0.005, res
    assert abs(l1 - 1.0) < 0.005 and abs(l2 - 1.0) < 0.005, res


def test_envelope(lib, tmp_path):
    e = Engine(lib, tmp_path, sine(220, 2.0, amp=0.5))
    e.set(ATTACK, 100)
    y = e.render("on 57 127 0", 0.3)[:, 0]
    a = rms_db(y[:secs_to(e, 0.010)])
    b = rms_db(y[secs_to(e, 0.100):secs_to(e, 0.110)])
    assert b - a >= 20, (a, b)
    e.set(ATTACK, 0)
    e.set(RELEASE, 200)
    off = secs_to(e, 0.5)
    y = e.render(f"on 57 127 0\noff 57 {off}", 1.0)[:, 0]
    before = rms_db(y[off - secs_to(e, 0.05):off])
    after = rms_db(y[off + secs_to(e, 0.190):off + secs_to(e, 0.200)])
    assert before - after >= 30, (before, after)
    assert np.max(np.abs(y[off + secs_to(e, 0.21):])) == 0
    e.free()


def test_decay_sustain(lib, tmp_path):
    e = Engine(lib, tmp_path, sine(220, 2.0, amp=0.5))
    e.set(DECAY, 100); e.set(SUSTAIN, -20)
    y = e.render("on 57 127 0", 0.4)[:, 0]
    a = rms_db(y[:secs_to(e, 0.005)])
    b = rms_db(y[secs_to(e, 0.2):secs_to(e, 0.3)])
    mid = rms_db(y[secs_to(e, 0.045):secs_to(e, 0.055)])
    assert abs((a - b) - 20) < 1, (a, b)
    assert abs((a - mid) - 10) < 1.5, (a, mid)          # linear in dB: half way at half the decay
    e.free()


def test_obey_note_off_off(lib, tmp_path):
    e = Engine(lib, tmp_path, sine(220, 1.0))
    e.set(OBEY_NOTE_OFF, 0)
    y = e.render(f"on 57 127 0\noff 57 {secs_to(e, 0.1)}", 1.3)
    assert abs(last_sound(y, e.host_rate) - 1.0) < 0.005
    assert rms_db(y[secs_to(e, 0.85):secs_to(e, 0.95), 0]) > -12
    e.free()


def test_velocity(lib, tmp_path):
    e = Engine(lib, tmp_path, sine(220, 1.0))
    loud = rms_db(e.render("on 57 127 0", 0.5)[secs_to(e, 0.1):secs_to(e, 0.4), 0])
    soft = rms_db(e.render("on 57 1 0", 0.5)[secs_to(e, 0.1):secs_to(e, 0.4), 0])
    assert abs((loud - soft) - 40) <= 1, (loud, soft)
    e.set(VELOCITY_SENS, 0)
    soft = rms_db(e.render("on 57 1 0", 0.5)[secs_to(e, 0.1):secs_to(e, 0.4), 0])
    assert abs(loud - soft) < 0.01
    e.set(VELOCITY_SENS, 100); e.set(MIN_VOLUME, -20)
    soft = rms_db(e.render("on 57 1 0", 0.5)[secs_to(e, 0.1):secs_to(e, 0.4), 0])
    assert abs((loud - soft) - 20) <= 0.5
    e.set(MIN_VOLUME, -60); e.set(VEL_LO, 10)
    assert np.max(np.abs(e.render("on 57 5 0", 0.2))) == 0
    e.free()


def longest_gap(x, rate, floor=1e-4):
    quiet = np.abs(x) < floor
    best = run = 0
    for q in quiet:
        run = run + 1 if q else 0
        best = max(best, run)
    return best / rate


def test_loop_forward_and_pingpong(lib, tmp_path):
    # 0.5 s (held whole in memory) and 2.5 s (streamed, loop start past the head)
    for secs, loop_start in ((0.5, 0), (2.5, 50)):
        for mode in (1, 2):
            d = tmp_path / f"{secs}_{mode}"
            d.mkdir()
            data = sine(220, secs) + sine(330, secs, amp=0.2)
            e = Engine(lib, d, data)
            e.set(LOOP, mode); e.set(LOOP_START, loop_start)
            y = e.render("on 57 127 0", 3.0, block=512)[:, 0]
            assert rms_db(y[secs_to(e, 2.9):]) > -20, (secs, mode)
            assert longest_gap(y[secs_to(e, 0.01):], e.host_rate) < 0.001, (secs, mode)
            assert lib.smp_engine_underruns(e.e) == 0
            e.free()


def worst_step(x, freq, rate=44100):
    """The largest jump between frames, against the sine's own steepest step."""
    ideal = np.percentile(np.abs(x), 99.9) * 2 * np.pi * freq / rate
    return np.abs(np.diff(x)).max() / ideal


def loop_pair(lib, tmp_path, data, loop_start, secs=3.0, rev=0):
    """A held note on a forward loop, with declick on and off (44.1 kHz in and out)."""
    ys = []
    for on in (True, False):
        d = tmp_path / ("on" if on else "off")
        d.mkdir()
        e = Engine(lib, d, data, host_rate=44100, declick=on)
        e.set(LOOP, 1); e.set(LOOP_START, loop_start); e.set(REVERSE, rev)
        ys.append(e.render("on %d 127 0" % e.root, secs)[:, 0])
        assert lib.smp_engine_underruns(e.e) == 0
        e.free()
    point_cache(lib, tmp_path / "cache")
    return ys


def test_declick_loop_from_the_start_dips_at_the_wrap(lib, tmp_path):
    """The loop starts at the file's first frame (nothing before it to crossfade with) and the file ends at a peak:
    every wrap jumps by the whole amplitude. Declick fades out the 2 ms before each wrap and in the 2 ms after."""
    n = np.arange(40025)
    on, off = loop_pair(lib, tmp_path, (0.5 * np.sin(2 * np.pi * n / 100)).astype(np.float32), 0)
    assert worst_step(off[1000:], 441) > 5                # without it: a click at every wrap
    assert worst_step(on[1000:], 441) < 1.5
    wraps = [40025 * k for k in (1, 2, 3)]
    for w in wraps:                                        # down to silence at the wrap, full level 2 ms either side
        assert np.abs(on[w - 3:w + 3]).max() < 0.05, w
        assert np.abs(on[w + FADE + 10:w + FADE + 110]).max() > 0.45, w
    assert np.abs(on[FADE + 2:40025 - FADE - 2] - off[FADE + 2:40025 - FADE - 2]).max() < 1e-6   # elsewhere untouched


def test_declick_loop_with_room_crossfades(lib, tmp_path):
    """A loop from 50 % whose end doesn't meet its start: the end crossfades into the frames before the loop start,
    so it doesn't dip."""
    n = np.arange(40025)
    on, off = loop_pair(lib, tmp_path, (0.5 * np.sin(2 * np.pi * n / 100)).astype(np.float32), 50)
    assert worst_step(off[1000:], 441) > 3
    assert worst_step(on[1000:], 441) < 1.5
    first = 40025
    loop = 40025 - int(0.5 * 40025)
    for w in (first, first + loop, first + 2 * loop):
        assert np.abs(on[w - 200:w + 200]).max() > 0.45, w   # no dip: a 441 Hz sine still reaches its peaks
    cut = 40025 - int(0.085 * 44100) - 10
    assert np.abs(on[FADE + 2:cut] - off[FADE + 2:cut]).max() < 1e-6


def test_declick_leaves_a_seamless_loop_alone(lib, tmp_path):
    n = np.arange(40000)                                  # 400 periods; from 50 %: the loop is exactly 200
    on, off = loop_pair(lib, tmp_path, (0.5 * np.sin(2 * np.pi * n / 100)).astype(np.float32), 50)
    assert np.abs(on - off).max() < 1e-6


def test_declick_loop_reversed(lib, tmp_path):
    n = np.arange(40025)                                  # reversed, from 0 %: the reversed file ends at frame 0 (0)
    data = (0.5 * np.sin(2 * np.pi * n / 100 + 0.3)).astype(np.float32)   # and starts at a peak: a jump
    on, off = loop_pair(lib, tmp_path, data, 0, rev=1)
    assert worst_step(off[1000:], 441) > 3
    assert worst_step(on[1000:], 441) < 1.5


def test_declick_loop_realtime(lib, tmp_path):
    """Played live, the loop's fix is built in the background and the note picks it up before its first wrap."""
    n = np.arange(44125)
    e = Engine(lib, tmp_path, (0.5 * np.sin(2 * np.pi * n / 100)).astype(np.float32), host_rate=44100)
    e.set(LOOP, 1); e.set(LOOP_START, 0)
    lib.smp_engine_note_on(e.e, e.root, 127, 0)
    blocks = 3 * 44100 // 256
    y = play_live(lib, e, blocks, speed=2.0, keep=blocks)
    assert worst_step(y[1000:], 441) < 1.5
    e.free()


def test_reverse(lib, tmp_path):
    rate = 48000                                           # same rate as the engine: output equals the data
    rng = np.random.default_rng(3)
    data = (0.3 * rng.standard_normal(rate * 2)).astype(np.float32)   # 2 s: streamed after the head
    e = Engine(lib, tmp_path, data, rate=rate, name="noise A3.wav", declick=False)
    e.set(REVERSE, 1)
    y = e.render("on 57 127 0", 1.5)[:, 0]
    want = data[::-1][:len(y)]
    assert np.max(np.abs(y - want)) < 1e-5
    assert lib.smp_engine_underruns(e.e) == 0
    e.set(REVERSE, 0)
    y = e.render("on 57 127 0", 0.5)[:, 0]
    assert np.max(np.abs(y - data[:len(y)])) < 1e-5
    e.free()


def test_start_end(lib, tmp_path):
    rate = 48000
    rng = np.random.default_rng(4)
    data = (0.3 * rng.standard_normal(rate * 4)).astype(np.float32)
    e = Engine(lib, tmp_path, data, rate=rate, name="noise A3.wav", declick=False)
    e.set(START, 50); e.set(END, 75); e.set(OBEY_NOTE_OFF, 0)
    # in offline renders the streamer has time to fill a ring that starts past the head
    y = e.render("on 57 127 0", 1.5)[:, 0]
    n = rate                                               # 25 % of 4 s
    assert np.max(np.abs(y[1:n] - data[2 * rate + 1:2 * rate + n])) < 1e-5
    assert np.max(np.abs(y[n + 2:])) == 0
    e.free()


def test_voices_limit(lib, tmp_path):
    e = Engine(lib, tmp_path, sine(220, 1.0))
    e.set(VOICES, 2)
    e.render("on 57 127 0\non 60 127 100\non 64 127 200", 0.3)
    assert lib.smp_engine_active_voices(e.e) == 2
    e.free()


def test_probability(lib, tmp_path):
    e = Engine(lib, tmp_path, sine(220, 0.2))
    e.set(PROBABILITY, 50); e.set(RELEASE, 0); e.set(VOICES, 64)
    lib.smp_engine_seed(e.e, 1)
    l = np.zeros(256, dtype=np.float32); r = np.zeros(256, dtype=np.float32)
    lp, rp = l.ctypes.data_as(FP), r.ctypes.data_as(FP)
    sounding = 0
    for _ in range(1000):
        lib.smp_engine_note_on(e.e, 57, 100, 0)
        lib.smp_engine_process(e.e, lp, rp, 256)
        sounding += lib.smp_engine_active_voices(e.e)
        lib.smp_engine_note_off(e.e, 57, 0)
        for _ in range(4):
            lib.smp_engine_process(e.e, lp, rp, 256)
        assert lib.smp_engine_active_voices(e.e) == 0
    assert 450 <= sounding <= 550, sounding
    e.free()


def test_sample_swap_while_playing(lib, tmp_path):
    e = Engine(lib, tmp_path, sine(220, 3.0))
    l = np.zeros(256, dtype=np.float32); r = np.zeros(256, dtype=np.float32)
    lp, rp = l.ctypes.data_as(FP), r.ctypes.data_as(FP)
    lib.smp_engine_note_on(e.e, 57, 100, 0)
    for _ in range(10):
        lib.smp_engine_process(e.e, lp, rp, 256)
    f = tmp_path / "other C4.wav"
    write_wav(f, sine(262, 2.0), 44100)
    err = ctypes.create_string_buffer(512)
    s = lib.smp_sample_load(str(f).encode(), err, 512)
    lib.smp_engine_set_sample(e.e, s)
    assert lib.smp_engine_get_param(e.e, ROOT) == 60
    for _ in range(20):
        lib.smp_engine_process(e.e, lp, rp, 256)
    assert lib.smp_engine_active_voices(e.e) == 0   # the old sample's voices faded out
    lib.smp_engine_collect(e.e)
    y = e.render("on 60 127 0", 0.5)
    assert abs(detect(lib, y[secs_to(e, 0.1):secs_to(e, 0.4), 0], e.host_rate) - midi(262)) < 0.1
    e.free()


def hermite(x, pos):
    i = np.floor(pos).astype(np.int64)
    t = (pos - i).astype(np.float64)
    xp = np.concatenate([[0.0], x.astype(np.float64), [0.0, 0.0, 0.0]])
    x0, x1, x2, x3 = xp[i], xp[i + 1], xp[i + 2], xp[i + 3]
    c1 = 0.5 * (x2 - x0)
    c2 = x0 - 2.5 * x1 + 2 * x2 - 0.5 * x3
    c3 = 0.5 * (x3 - x0) + 1.5 * (x1 - x2)
    return ((c3 * t + c2) * t + c1) * t + x1


def test_streaming_long_sample(lib, tmp_path):
    rate, secs = 44100, 60
    t = np.arange(rate * secs) / rate
    data = (0.3 * np.sin(2 * np.pi * 440 * t) + 0.2 * np.sin(2 * np.pi * 1234.5 * t)
            + 0.1 * np.sin(2 * np.pi * 3.3 * t)).astype(np.float32)
    proc = psutil.Process()
    e = Engine(lib, tmp_path, data, rate=rate, name="long A4.wav", declick=False)
    del t
    before = proc.memory_info().rss
    out = tmp_path / "long_out.wav"
    frames = secs * e.host_rate
    assert lib.smp_engine_render_test(e.e, b"on 69 127 0", frames, 256, str(out).encode()) == 0
    grown = proc.memory_info().rss - before
    assert lib.smp_engine_underruns(e.e) == 0
    y, _ = sf.read(str(out), dtype="float32")
    y = y[:, 0]
    assert len(y) == frames
    e.free()
    assert grown < 20 * 1024 * 1024, grown / 1e6
    ratio = rate / e.host_rate
    ref = hermite(data, np.arange(len(y)) * ratio)
    err = rms_db(y - ref) - rms_db(ref)
    assert err < -60, err


def play_live(lib, e, blocks, speed=2.0, block=256, keep=None, between=None):
    """Real-time style: process called directly, paced at `speed` times real time, never waiting on the streamer.
    Returns the left channel of the first `keep` blocks."""
    import time
    l = np.zeros(block, dtype=np.float32); r = np.zeros(block, dtype=np.float32)
    lp, rp = l.ctypes.data_as(FP), r.ctypes.data_as(FP)
    out = []
    t0 = time.perf_counter()
    for b in range(blocks):
        if between:
            between(b)
        lib.smp_engine_process(e.e, lp, rp, block)
        if keep is not None and b < keep:
            out.append(l.copy())
        while time.perf_counter() - t0 < (b + 1) * block / e.host_rate / speed:
            pass
    return np.concatenate(out) if out else None


def test_start_past_head_realtime(lib, tmp_path):
    import time
    rate = 48000
    rng = np.random.default_rng(5)
    data = (0.3 * rng.standard_normal(rate * 4)).astype(np.float32)
    e = Engine(lib, tmp_path, data, rate=rate, name="noise A3.wav", declick=False)
    for rev in (0, 1):
        e.set(REVERSE, rev); e.set(START, 50)
        time.sleep(0.3)                                   # the user set start a moment before playing
        lib.smp_engine_note_on(e.e, 57, 127, 0)
        y = play_live(lib, e, 40, keep=40)
        src = data[2 * rate:] if not rev else data[::-1][:2 * rate]   # reversed: region [0, 50 %) mirrored
        n = rate // 100
        assert rms_db(y[1:n] - src[1:n]) - rms_db(src[1:n]) < -60, rev
        assert lib.smp_engine_underruns(e.e) == 0, rev
        lib.smp_engine_note_off(e.e, 57, 0)
        play_live(lib, e, 20)
    e.free()


def test_loop_ignored_when_note_off_ignored(lib, tmp_path):
    e = Engine(lib, tmp_path, sine(220, 0.5))
    e.set(LOOP, 1); e.set(OBEY_NOTE_OFF, 0)
    y = e.render(f"on 57 127 0\noff 57 {secs_to(e, 0.1)}", 0.8)
    assert np.max(np.abs(y[secs_to(e, 0.6):])) == 0
    assert lib.smp_engine_active_voices(e.e) == 0
    e.free()


def test_reverse_from_audio_thread(lib, tmp_path):
    import threading, time
    rate = 44100
    t = np.arange(rate * 60) / rate
    data = (0.3 * np.sin(2 * np.pi * 220 * t) * (1 + 0.5 * np.sin(2 * np.pi * 0.25 * t))).astype(np.float32)
    e = Engine(lib, tmp_path, data, rate=rate, name="long A3.wav", host_rate=rate)
    for k in range(8):
        lib.smp_engine_note_on(e.e, 57 + 24 + k, 100, 0)   # two octaves up and more: rings drain fast

    # at 6x real time the rings have ~30 ms of slack: a reversed copy made on the streamer (~30 ms) would drain them
    def flip(b):
        if b == 20:
            lib.smp_engine_set_param(e.e, REVERSE, 1)       # from the audio thread, between process calls

    th = threading.Thread(target=play_live, args=(lib, e, rate * 3 // 256), kwargs={"between": flip, "speed": 6.0})
    th.start(); th.join()
    assert lib.smp_engine_active_voices(e.e) == 8
    assert lib.smp_engine_underruns(e.e) == 0
    for _ in range(200):                                     # the reversed copy arrives in the background
        y = e.render("on 57 127 0", 0.05)[:, 0]
        if abs(y[100] - data[-101]) < 1e-3 and abs(y[1000] - data[-1001]) < 1e-3:
            break
        time.sleep(0.05)
    else:
        raise AssertionError("reversed copy never became ready")
    e.free()


def test_streaming_long_sample_realtime(lib, tmp_path):
    rate, secs = 44100, 60
    data = (0.3 * np.sin(2 * np.pi * 440 * np.arange(rate * secs) / rate)).astype(np.float32)
    e = Engine(lib, tmp_path, data, rate=rate, name="long A4.wav")
    lib.smp_engine_note_on(e.e, 69, 127, 0)
    play_live(lib, e, (secs - 1) * e.host_rate // 256, speed=4.0)
    assert lib.smp_engine_active_voices(e.e) == 1
    assert lib.smp_engine_underruns(e.e) == 0
    e.free()


def test_start_changes_without_process_never_stall(lib, tmp_path):
    # No process calls (a deactivated plugin): the audio thread never finishes a block, so replaced start heads
    # can't be freed. The worker must not sleep-wait on a full retire list.
    import time
    lib.smp_test_worker_wait.restype = ctypes.c_int
    lib.smp_test_worker_wait.argtypes = [ctypes.c_void_p, ctypes.c_int]
    rate = 48000
    rng = np.random.default_rng(9)
    data = (0.3 * rng.standard_normal(rate * 4)).astype(np.float32)
    e = Engine(lib, tmp_path, data, rate=rate, name="noise A3.wav", declick=False)
    t0 = time.perf_counter()
    for i in range(150):
        e.set(START, 30 + i * 0.2)
        assert lib.smp_test_worker_wait(e.e, 3000) == 0, i
    assert time.perf_counter() - t0 < 20
    e.set(START, 50)
    y = e.render("on 57 127 0", 0.05)[:, 0]               # and the start head still comes right once blocks run
    n = 2000
    assert rms_db(y[1:n] - data[2 * rate + 1:2 * rate + n]) - rms_db(data[2 * rate + 1:2 * rate + n]) < -60
    e.free()


def test_idle_engine_memory(lib, tmp_path):
    # rings are committed only for the Voices parameter's slots (16 by default), and more only when it grows
    import gc, time
    gc.collect()
    proc = psutil.Process()
    before = proc.memory_full_info().private
    e = lib.smp_engine_new(48000, 512)
    time.sleep(0.2)
    grown = proc.memory_full_info().private - before
    print(f"idle engine: {grown / 1e6:.1f} MB")
    assert grown < 12 * 1024 * 1024, grown / 1e6
    lib.smp_engine_set_param(e, VOICES, 64)               # from this (main) thread: committed at once
    grown64 = proc.memory_full_info().private - before
    assert grown64 >= grown + 47 * 512 * 1024, (grown, grown64)
    lib.smp_engine_free(e)


def test_voices_raised_off_main_thread(lib, tmp_path):
    # Voices raised from another thread (the audio thread, in a host): the worker commits the rings; notes beyond
    # the committed ones are simply not started until then, never allocated on the calling thread
    import threading, time
    e = Engine(lib, tmp_path, sine(220, 0.5))
    th = threading.Thread(target=lambda: lib.smp_engine_set_param(e.e, VOICES, 40))
    th.start(); th.join()
    time.sleep(0.2)                                       # a worker pass
    ev = "\n".join(f"on {40 + i} 127 0" for i in range(40))
    e.render(ev, 0.01)
    assert lib.smp_engine_active_voices(e.e) == 40
    e.free()


def test_underrun_fades(lib, tmp_path):
    # the streamer is held still, so the ring runs dry after the 0.25 s head: the voice fades out over 64 samples
    # before the gap and back in after it, instead of cutting
    lib.smp_debug_stream_pause.restype = None
    lib.smp_debug_stream_pause.argtypes = [ctypes.c_void_p, ctypes.c_int]
    rate = 48000
    data = sine(220, 4.0, rate)
    e = Engine(lib, tmp_path, data, rate=rate, name="tone A3.wav")
    lib.smp_debug_stream_pause(e.e, 1)
    lib.smp_engine_note_on(e.e, 57, 127, 0)

    def resume(b):
        if b == 80:                                       # about 0.43 s: well into the gap
            lib.smp_debug_stream_pause(e.e, 0)

    y = play_live(lib, e, 160, speed=1.0, keep=160, between=resume)
    lib.smp_debug_stream_pause(e.e, 0)
    assert lib.smp_engine_underruns(e.e) > 0
    head = rate // 4
    assert np.max(np.abs(y[head + 100:head + 4000])) == 0          # the gap
    assert np.max(np.abs(y[-4000:])) > 0.4                         # and the sound came back
    normal = np.max(np.abs(np.diff(y[1000:head - 1000])))
    steps = np.max(np.abs(np.diff(y)))
    assert steps <= normal * 1.25, (steps, normal)                 # no click: a cut would step by up to 0.5
    print(f"normal step {normal:.4f}, largest {steps:.4f}")
    e.free()


def freq_at(e, y, t0, t1):
    """The pitch of a clean sine between t0 and t1 seconds, from its zero crossings."""
    x = y[secs_to(e, t0):secs_to(e, t1), 0]
    ups = np.nonzero((x[:-1] <= 0) & (x[1:] > 0))[0]
    return (len(ups) - 1) / ((ups[-1] - ups[0]) / e.host_rate)


def st(f, ref=220.0):
    return 12 * math.log2(f / ref)


def test_glide(lib, tmp_path):
    e = Engine(lib, tmp_path, sine(220, 2.0))
    e.set(GLIDE, 200)
    notes = f"on 57 127 0\noff 57 {secs_to(e, 0.25)}\non 69 127 {secs_to(e, 0.3)}"
    y = e.render(notes, 0.7)
    assert st(freq_at(e, y, 0.30, 0.32)) < 1.5                  # starts from the last note, A3
    assert abs(st(freq_at(e, y, 0.39, 0.41)) - 6) < 0.6         # half way at half the time, in semitones
    assert abs(st(freq_at(e, y, 0.55, 0.65)) - 12) < 0.05       # there: A4
    e.set(GLIDE_MODE, 1)                                         # legato: the notes don't overlap, so no glide
    y = e.render(notes, 0.7)
    assert abs(st(freq_at(e, y, 0.30, 0.32)) - 12) < 0.1
    e.set(GLIDE, 0)                                              # off: no glide in either mode
    e.set(GLIDE_MODE, 0)
    y = e.render(notes, 0.7)
    assert abs(st(freq_at(e, y, 0.30, 0.32)) - 12) < 0.1
    e.free()


def test_glide_legato_overlap(lib, tmp_path):
    e = Engine(lib, tmp_path, sine(220, 2.0))
    e.set(GLIDE, 100); e.set(GLIDE_MODE, 1)
    y = e.render(f"on 57 127 0\non 69 127 {secs_to(e, 0.3)}\noff 57 {secs_to(e, 0.35)}", 0.6)
    assert 6 < st(freq_at(e, y, 0.36, 0.38)) < 11               # the old key was held: it glides
    assert abs(st(freq_at(e, y, 0.45, 0.55)) - 12) < 0.05
    y = e.render(f"on 57 127 0\noff 57 {secs_to(e, 0.3)}\non 69 127 {secs_to(e, 0.3)}", 0.6)
    assert abs(st(freq_at(e, y, 0.31, 0.33)) - 12) < 0.1        # let go at the same moment: not held, no glide
    e.free()


def test_glide_mono_legato(lib, tmp_path):
    # Voices 1, legato: a key pressed while another is held carries the sounding note on (no new attack) and slides
    # to it; letting it go slides back to the key still held
    e = Engine(lib, tmp_path, sine(220, 2.0))
    e.set(VOICES, 1); e.set(GLIDE_MODE, 1); e.set(GLIDE, 50); e.set(ATTACK, 100)
    y = e.render(f"on 57 127 0\non 64 127 {secs_to(e, 0.3)}\noff 64 {secs_to(e, 0.7)}\noff 57 {secs_to(e, 1.0)}", 1.2)
    assert abs(st(freq_at(e, y, 0.2, 0.3))) < 0.05
    assert abs(st(freq_at(e, y, 0.45, 0.65)) - 7) < 0.05        # E4 (7 semitones up)
    assert abs(st(freq_at(e, y, 0.85, 0.95))) < 0.05            # back to A3
    a = rms_db(y[secs_to(e, 0.25):secs_to(e, 0.3), 0])
    b = rms_db(y[secs_to(e, 0.3):secs_to(e, 0.33), 0])
    assert abs(a - b) < 1, (a, b)                                # no new attack
    assert last_sound(y, e.host_rate) < 1.05                     # both let go: it ends
    e.set(GLIDE, 0)                                              # no glide time: it jumps, still without an attack
    y = e.render(f"on 57 127 0\non 64 127 {secs_to(e, 0.3)}", 0.5)
    assert abs(st(freq_at(e, y, 0.31, 0.35)) - 7) < 0.05
    assert abs(rms_db(y[secs_to(e, 0.3):secs_to(e, 0.32), 0]) - a) < 1
    e.set(GLIDE_MODE, 0)                                         # always: Voices 1 starts the new note again
    y = e.render(f"on 57 127 0\non 64 127 {secs_to(e, 0.3)}", 0.5)
    assert rms_db(y[secs_to(e, 0.305):secs_to(e, 0.315), 0]) < a - 10
    e.free()
