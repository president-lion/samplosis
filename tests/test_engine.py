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
    def __init__(self, lib, tmp_path, data, rate=44100, name="tone A3.wav", host_rate=48000, max_block=512):
        self.lib, self.tmp, self.host_rate = lib, tmp_path, host_rate
        point_cache(lib, tmp_path / "cache")
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


def test_reverse(lib, tmp_path):
    rate = 48000                                           # same rate as the engine: output equals the data
    rng = np.random.default_rng(3)
    data = (0.3 * rng.standard_normal(rate * 2)).astype(np.float32)   # 2 s: streamed after the head
    e = Engine(lib, tmp_path, data, rate=rate, name="noise A3.wav")
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
    e = Engine(lib, tmp_path, data, rate=rate, name="noise A3.wav")
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
    e = Engine(lib, tmp_path, data, rate=rate, name="long A4.wav")
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
    e = Engine(lib, tmp_path, data, rate=rate, name="noise A3.wav")
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
    e = Engine(lib, tmp_path, data, rate=rate, name="noise A3.wav")
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
