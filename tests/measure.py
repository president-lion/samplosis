"""Measures Samplosis CPU and memory through the exported engine API. Run: python tests/measure.py
CPU: 16 held voices, 48 kHz, 256-frame blocks, through smp_engine_render_test (percent of real time).
Memory: private bytes of this process from before engine_new: a new idle engine, then a 60 s sample loaded with
its keep-length copies (one octave each way of the root) rendered.
Uses a temporary settings file and cache folder; nothing of the user's is touched."""
import ctypes, os, sys, tempfile, time
import numpy as np, psutil, soundfile as sf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
work = tempfile.mkdtemp(prefix="samplosis_measure_", dir=os.environ.get("MEASURE_DIR"))
os.environ["SAMPLOSIS_SETTINGS"] = os.path.join(work, "settings.txt")
lib = ctypes.CDLL(os.path.join(ROOT, "samplosis.clap"))

class Settings(ctypes.Structure):
    _fields_ = [("cache_dir", ctypes.c_char * 1024), ("cache_limit_gb", ctypes.c_double),
                ("stretcher", ctypes.c_int), ("keep_tone", ctypes.c_int)]
vp, ci, cd, cp = ctypes.c_void_p, ctypes.c_int, ctypes.c_double, ctypes.c_char_p
lib.smp_settings_set.argtypes = [ctypes.POINTER(Settings)]
lib.smp_sample_load.restype = vp; lib.smp_sample_load.argtypes = [cp, cp, ci]
lib.smp_engine_new.restype = vp; lib.smp_engine_new.argtypes = [cd, ci]
lib.smp_engine_free.argtypes = [vp]
lib.smp_engine_set_sample.argtypes = [vp, vp]
lib.smp_engine_set_param.argtypes = [vp, ci, cd]
lib.smp_engine_wait_renders.argtypes = [vp, ci]
lib.smp_engine_render_test.argtypes = [vp, cp, ctypes.c_long, ci, cp]
lib.smp_engine_render_status.argtypes = [vp, ctypes.POINTER(ci), ctypes.POINTER(ci), cp, ci]
ROOT_P, KEEP, VOICES, MODE = 0, 25, 26, 11

s = Settings(); s.cache_dir = os.path.join(work, "cache").encode(); s.cache_limit_gb = 10
assert lib.smp_settings_set(ctypes.byref(s)) == 0

def sample(secs, name, rate=44100):
    t = np.arange(int(secs * rate)) / rate
    x = (0.3 * np.sin(2 * np.pi * 220 * t) + 0.2 * np.sin(2 * np.pi * 661 * t)).astype(np.float32)
    p = os.path.join(work, name); sf.write(p, np.stack([x, x * 0.9], 1), rate, subtype="FLOAT")
    err = ctypes.create_string_buffer(512)
    h = lib.smp_sample_load(p.encode(), err, 512); assert h, err.value
    return h

# CPU
RATE, BLOCK, SECS = 48000, 256, 10
for keep in (0, 1):
    e = lib.smp_engine_new(RATE, 512)
    lib.smp_engine_set_param(e, VOICES, 16)
    lib.smp_engine_set_param(e, KEEP, keep)
    lib.smp_engine_set_param(e, 12, 48); lib.smp_engine_set_param(e, 13, 63)   # note range for the copies
    lib.smp_engine_set_sample(e, sample(30, f"cpu{keep}.wav"))
    if keep: assert lib.smp_engine_wait_renders(e, 600000) == 0
    ev = "\n".join(f"on {48 + i} 100 0" for i in range(16)).encode()
    t0 = time.perf_counter()
    assert lib.smp_engine_render_test(e, ev, SECS * RATE, BLOCK, os.path.join(work, "o.wav").encode()) == 0
    dt = time.perf_counter() - t0
    print(f"CPU, 16 voices, keep length {'on' if keep else 'off'}: {dt:.3f} s for {SECS} s of audio = {100 * dt / SECS:.2f} % of real time")
    lib.smp_engine_free(e)

# memory: the baseline is taken before engine_new, so the engine's rings count
import gc
proc = psutil.Process()
src = sample(60, "mem.wav")                           # loaded (decoded into the cache) before the baseline
gc.collect()
before = proc.memory_full_info().private
e = lib.smp_engine_new(RATE, 512)
time.sleep(0.5)
idle = proc.memory_full_info().private
print(f"Memory, a new idle engine (16 voices): private bytes grew {(idle - before) / 1e6:.1f} MB")
lib.smp_engine_set_sample(e, src)                     # keep length on by default, default key range
t0 = time.perf_counter()
assert lib.smp_engine_wait_renders(e, 1800000) == 0
ready, total, why = ci(), ci(), ctypes.create_string_buffer(512)
ok = lib.smp_engine_render_status(e, ctypes.byref(ready), ctypes.byref(total), why, 512)
time.sleep(0.5)
after = proc.memory_full_info().private
print(f"Memory, 60 s sample, copies ready {ready.value} of {total.value} (all ready: {ok}) after {time.perf_counter() - t0:.0f} s: "
      f"private bytes grew {(after - before) / 1e6:.1f} MB from before engine_new")
lib.smp_engine_free(e)
