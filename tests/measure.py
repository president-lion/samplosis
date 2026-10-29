"""Measures Samplosis CPU and memory through the exported engine API. Run: python tests/measure.py
CPU: 16 held voices, 48 kHz, 256-frame blocks, through smp_engine_render_test (percent of real time).
Memory: private bytes of this process from before engine_new: a new idle engine, then a 60 s sample loaded with
its keep-length copies (one octave each way of the root) rendered.
Uses a temporary settings file and cache folder; nothing of the user's is touched."""
import atexit, ctypes, os, shutil, sys, tempfile, time
import numpy as np, psutil, soundfile as sf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
work = tempfile.mkdtemp(prefix="samplosis_measure_", dir=os.environ.get("MEASURE_DIR"))
atexit.register(shutil.rmtree, work, True)   # the cache copies are gigabytes; do not leave them
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

# ---- SoundFont (just_t4_fixed.sf2, read only; skipped when missing) ----
FONT = r"E:\samplepacks\soundfonts\just_t4_fixed.sf2"
if not os.path.exists(FONT):
    print("SoundFont measurements skipped: no", FONT)
    sys.exit(0)
try: proc.nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)
except Exception: pass
u8, u32, f32p = ctypes.c_ubyte, ctypes.c_uint32, ctypes.POINTER(ctypes.c_float)
lib.smp_sf_open.restype = vp; lib.smp_sf_open.argtypes = [cp, cp, ci]
lib.smp_sf_close.argtypes = [vp]
lib.smp_sf_preset_count.argtypes = [vp]
lib.smp_sf_find_preset.argtypes = [vp, ci, ci]
lib.smp_sf_use.argtypes = [vp, ci, ci]
lib.smp_sf_wait_idle.argtypes = [vp, ci]
lib.smp_sf_loaded_bytes.restype = ctypes.c_uint64; lib.smp_sf_loaded_bytes.argtypes = [vp]
lib.smp_synth_new.restype = vp; lib.smp_synth_new.argtypes = [cd, ci]
lib.smp_synth_free.argtypes = [vp]
lib.smp_synth_load.argtypes = [vp, cp, cp, ci]
lib.smp_synth_midi.argtypes = [vp, ctypes.c_char_p, ci, ci]
lib.smp_synth_process.argtypes = [vp, ctypes.POINTER(f32p), ci]
lib.smp_synth_set_param.argtypes = [vp, ci, cd]
lib.smp_synth_set_offline.argtypes = [vp, ci]
lib.smp_synth_wait_loaded.argtypes = [vp, ci]
lib.smp_synth_voices.argtypes = [vp, ctypes.POINTER(ci)]
SF_EFFECTS = 33

def priv():
    gc.collect(); time.sleep(0.3)
    return proc.memory_full_info().private

print(f"\nSoundFont {os.path.basename(FONT)} ({os.path.getsize(FONT) / 1e6:.0f} MB), measured at below-normal priority")
m0 = priv(); t0 = time.perf_counter()
err = ctypes.create_string_buffer(512)
font = lib.smp_sf_open(FONT.encode(), err, 512); assert font, err.value
dt = time.perf_counter() - t0
print(f"Open (headers only): {dt * 1000:.0f} ms, {lib.smp_sf_preset_count(font)} presets, private bytes grew {(priv() - m0) / 1e6:.1f} MB")
for bank, prog in ((0, 0), (0, 25), (128, 0)):
    idx = lib.smp_sf_find_preset(font, bank, prog); assert idx >= 0
    m0 = priv(); t0 = time.perf_counter()
    lib.smp_sf_use(font, idx, 1)
    assert lib.smp_sf_wait_idle(font, 120000) == 0
    dt = time.perf_counter() - t0
    print(f"Load preset {bank}:{prog}: {dt * 1000:.0f} ms, samples {lib.smp_sf_loaded_bytes(font) / 1e6:.1f} MB in all, private bytes grew {(priv() - m0) / 1e6:.1f} MB")
    # The release and the use again come back to back, well inside the loader's 2 s grace, so the preset is never
    # freed: each preset stays loaded while the next one loads. That is why "samples ... MB in all" is cumulative
    # (piano, then piano and guitar, then all three), while "private bytes grew" is each preset's own growth.
    lib.smp_sf_use(font, idx, -1)
    lib.smp_sf_use(font, idx, 1)

def new_synth():
    s = lib.smp_synth_new(float(RATE), BLOCK)
    assert lib.smp_synth_load(s, FONT.encode(), err, 512) == 1, err.value
    return s

# CPU: organ notes held on channel 1 (drawbar organ, program 17, which loops), 48 kHz, 256-frame blocks
NOTES = 33                                              # the organ layers two regions a note: about 64 voices
bufs = [np.zeros(BLOCK, np.float32) for _ in range(32)]
outs = (f32p * 32)(*[b.ctypes.data_as(f32p) for b in bufs])
for fx in (0, 1):
    s = new_synth()
    lib.smp_synth_set_param(s, SF_EFFECTS, fx)
    lib.smp_synth_set_offline(s, 1)
    lib.smp_synth_midi(s, bytes([0xC0, 16]), 2, 0)      # drawbar organ: loops, so every note is held
    for k in range(NOTES): lib.smp_synth_midi(s, bytes([0x90, 40 + k, 100]), 3, 0)
    lib.smp_synth_process(s, outs, BLOCK)               # starts them (waits for the samples)
    nb = SECS * RATE // BLOCK
    t0 = time.perf_counter()
    for _ in range(nb): lib.smp_synth_process(s, outs, BLOCK)
    dt = time.perf_counter() - t0
    pk = ci(); nv = lib.smp_synth_voices(s, ctypes.byref(pk))
    print(f"CPU, {NOTES} organ notes held = {nv} voices sounding, reverb and chorus {'on' if fx else 'off'}: {dt:.3f} s for {SECS} s of audio = {100 * dt / SECS:.2f} % of real time")
    lib.smp_synth_free(s)

# a second instance on the same font: private bytes of the second synth_new + load, samples already shared
s1 = new_synth(); lib.smp_synth_midi(s1, bytes([0x90, 60, 100]), 3, 0)
lib.smp_synth_set_offline(s1, 1); lib.smp_synth_process(s1, outs, BLOCK)
m0 = priv()
s2 = new_synth(); lib.smp_synth_midi(s2, bytes([0x90, 60, 100]), 3, 0)
lib.smp_synth_set_offline(s2, 1); lib.smp_synth_process(s2, outs, BLOCK)
print(f"A second instance on the same font (same preset playing): private bytes grew {(priv() - m0) / 1e6:.1f} MB (target under 2 MB)")
lib.smp_synth_free(s2); lib.smp_synth_free(s1)
lib.smp_sf_close(font)
