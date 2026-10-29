"""Measures Samplosis CPU and memory through the exported API. Run: python tests/measure.py

Parts (MEASURE_PARTS, a comma list; all by default):
  sampler  CPU: 16 held voices, 48 kHz, 256-frame blocks, through smp_engine_render_test (percent of real time).
           Memory: private bytes of this process from before engine_new: a new idle engine, then a 60 s sample
           loaded with its keep-length copies rendered.
  sf2      E:\\samplepacks\\soundfonts\\just_t4_fixed.sf2 (read only; skipped when missing): open, three presets'
           loads, CPU of 66 held voices, a second instance.
  stream   A GigaStudio-sized SFZ piano (88 keys x 4 velocity layers = 352 stereo 16-bit files of 17 s, about
           1.05 GB) written to MEASURE_STREAM_DIR (default E:\\samplepacks\\_build\\samplosis_stream_test), pushed
           out of the file cache, then opened, its preset loaded and 64 notes played for 10 s in real time
           (counting underruns). The folder is deleted at the end.
  dls      C:\\Windows\\System32\\drivers\\gm.dls (read only): open, a piano chord and a drum note.
  sfz      MEASURE_SFZ, when set: any .sfz (a real library): open, what it ignores, its first preset loaded and a
           few notes played offline and in real time.

Uses a temporary settings file and cache folder (in MEASURE_DIR, else the system temp folder: set MEASURE_DIR to a
roomy drive, the sampler part needs a few GB for a while); nothing of the user's is touched."""
import atexit, ctypes, gc, os, shutil, sys, tempfile, time
import numpy as np, psutil, soundfile as sf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PARTS = set((os.environ.get("MEASURE_PARTS") or "sampler,sf2,stream,dls,sfz").split(","))
work = tempfile.mkdtemp(prefix="samplosis_measure_", dir=os.environ.get("MEASURE_DIR"))
atexit.register(shutil.rmtree, work, True)   # the cache copies are gigabytes; do not leave them
os.environ["SAMPLOSIS_SETTINGS"] = os.path.join(work, "settings.txt")
lib = ctypes.CDLL(os.path.join(ROOT, "samplosis.clap"))

class Settings(ctypes.Structure):
    _fields_ = [("cache_dir", ctypes.c_char * 1024), ("cache_limit_gb", ctypes.c_double),
                ("stretcher", ctypes.c_int), ("keep_tone", ctypes.c_int),
                ("ffmpeg_path", ctypes.c_char * 1024),
                ("raw_rate", ctypes.c_int), ("raw_channels", ctypes.c_int),
                ("raw_format", ctypes.c_int), ("raw_endian", ctypes.c_int)]
vp, ci, cd, cp = ctypes.c_void_p, ctypes.c_int, ctypes.c_double, ctypes.c_char_p
u8, u32, f32p = ctypes.c_ubyte, ctypes.c_uint32, ctypes.POINTER(ctypes.c_float)
lib.smp_settings_get.argtypes = [ctypes.POINTER(Settings)]
lib.smp_settings_set.argtypes = [ctypes.POINTER(Settings)]
lib.smp_sample_load.restype = vp; lib.smp_sample_load.argtypes = [cp, cp, ci]
lib.smp_engine_new.restype = vp; lib.smp_engine_new.argtypes = [cd, ci]
lib.smp_engine_free.argtypes = [vp]
lib.smp_engine_set_sample.argtypes = [vp, vp]
lib.smp_engine_set_param.argtypes = [vp, ci, cd]
lib.smp_engine_wait_renders.argtypes = [vp, ci]
lib.smp_engine_render_test.argtypes = [vp, cp, ctypes.c_long, ci, cp]
lib.smp_engine_render_status.argtypes = [vp, ctypes.POINTER(ci), ctypes.POINTER(ci), cp, ci]
lib.smp_sf_open.restype = vp; lib.smp_sf_open.argtypes = [cp, cp, ci]
lib.smp_sf_close.argtypes = [vp]
lib.smp_sf_preset_count.argtypes = [vp]
lib.smp_sf_find_preset.argtypes = [vp, ci, ci]
lib.smp_sf_use.argtypes = [vp, ci, ci]
lib.smp_sf_wait_idle.argtypes = [vp, ci]
lib.smp_sf_loaded_bytes.restype = ctypes.c_uint64; lib.smp_sf_loaded_bytes.argtypes = [vp]
lib.smp_sf_problem.restype = cp; lib.smp_sf_problem.argtypes = [vp]
lib.smp_sf_ignored.argtypes = [vp, cp, ci]
lib.smp_sf_regions.argtypes = [vp, ci, ctypes.POINTER(vp)]
lib.smp_sf_sample_count.argtypes = [vp]
lib.smp_sf_preset_info.argtypes = [vp, ci, ctypes.POINTER(ci), ctypes.POINTER(ci), cp, ci]
lib.smp_synth_new.restype = vp; lib.smp_synth_new.argtypes = [cd, ci]
lib.smp_synth_free.argtypes = [vp]
lib.smp_synth_load.argtypes = [vp, cp, cp, ci]
lib.smp_synth_midi.argtypes = [vp, ctypes.c_char_p, ci, ci]
lib.smp_synth_process.argtypes = [vp, ctypes.POINTER(f32p), ci]
lib.smp_synth_set_param.argtypes = [vp, ci, cd]
lib.smp_synth_set_offline.argtypes = [vp, ci]
lib.smp_synth_wait_loaded.argtypes = [vp, ci]
lib.smp_synth_voices.argtypes = [vp, ctypes.POINTER(ci)]
lib.smp_synth_render_test.argtypes = [vp, cp, ctypes.c_long, ci, cp, ci]
lib.smp_synth_test_offline.argtypes = [vp, ci]
lib.smp_synth_underruns.restype = ctypes.c_long; lib.smp_synth_underruns.argtypes = [vp]
ROOT_P, KEEP, VOICES, MODE = 0, 25, 26, 11
SF_EFFECTS, SF_VOICES = 33, 32

s = Settings(); lib.smp_settings_get(ctypes.byref(s))
s.cache_dir = os.path.join(work, "cache").encode(); s.cache_limit_gb = 10
assert lib.smp_settings_set(ctypes.byref(s)) == 0
RATE, BLOCK, SECS = 48000, 256, 10
proc = psutil.Process()

def priv():
    gc.collect(); time.sleep(0.3)
    return proc.memory_full_info().private

def sample(secs, name, rate=44100):
    t = np.arange(int(secs * rate)) / rate
    x = (0.3 * np.sin(2 * np.pi * 220 * t) + 0.2 * np.sin(2 * np.pi * 661 * t)).astype(np.float32)
    p = os.path.join(work, name); sf.write(p, np.stack([x, x * 0.9], 1), rate, subtype="FLOAT")
    err = ctypes.create_string_buffer(512)
    h = lib.smp_sample_load(p.encode(), err, 512); assert h, err.value
    return h

def db(x):
    x = float(x)
    return -999.0 if x <= 0 else 20 * np.log10(x)

def render(syn, events, secs, realtime=False):
    """Renders events on syn (offline, or in real time: never waiting); (stereo out, wall seconds, cpu seconds)."""
    out = os.path.join(work, "r.wav")
    lib.smp_synth_test_offline(syn, 0 if realtime else 1)
    c0, t0 = proc.cpu_times(), time.perf_counter()
    assert lib.smp_synth_render_test(syn, events.encode(), int(secs * RATE), BLOCK, out.encode(), 0) == 0
    wall, c1 = time.perf_counter() - t0, proc.cpu_times()
    x, _ = sf.read(out, dtype="float32", always_2d=True)
    return x, wall, (c1.user - c0.user) + (c1.system - c0.system)

def new_synth(path):
    syn = lib.smp_synth_new(float(RATE), BLOCK)
    err = ctypes.create_string_buffer(512)
    assert lib.smp_synth_load(syn, path.encode(), err, 512) == 1, err.value
    lib.smp_synth_set_param(syn, SF_EFFECTS, 0)
    return syn

def use_presets(syn):
    """render_test with no events uses the presets the channels play; then wait for their samples. Seconds."""
    t0 = time.perf_counter()
    render(syn, "", BLOCK / RATE)
    assert lib.smp_synth_wait_loaded(syn, 600000) == 0
    return time.perf_counter() - t0

def ignored(f):
    first = ctypes.create_string_buffer(256)
    return lib.smp_sf_ignored(f, first, 256), first.value.decode("utf-8", "replace")

# ---- the sampler ----
if "sampler" in PARTS:
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
    src = sample(60, "mem.wav")                           # loaded (decoded into the cache) before the baseline
    before = priv()
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

try: proc.nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)    # the SoundFont parts, as stage 2 measured them
except Exception: pass

# ---- SoundFont 2 (just_t4_fixed.sf2, read only; skipped when missing) ----
FONT = r"E:\samplepacks\soundfonts\just_t4_fixed.sf2"
if "sf2" in PARTS and not os.path.exists(FONT):
    print("SoundFont measurements skipped: no", FONT)
elif "sf2" in PARTS:
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
        print(f"Load preset {bank}:{prog}: {dt * 1000:.0f} ms, samples {lib.smp_sf_loaded_bytes(font) / 1e6:.2f} MB in all, private bytes grew {(priv() - m0) / 1e6:.1f} MB")
        # The release and the use again come back to back, well inside the loader's 2 s grace, so the preset is
        # never freed: each preset stays loaded while the next one loads. That is why "samples ... MB in all" is
        # cumulative (piano, then piano and guitar, then all three), while "private bytes grew" is each preset's own.
        lib.smp_sf_use(font, idx, -1)
        lib.smp_sf_use(font, idx, 1)

    # CPU: organ notes held on channel 1 (drawbar organ, program 17, which loops), 48 kHz, 256-frame blocks
    NOTES = 33                                              # the organ layers two regions a note: about 64 voices
    bufs = [np.zeros(BLOCK, np.float32) for _ in range(32)]
    outs = (f32p * 32)(*[b.ctypes.data_as(f32p) for b in bufs])
    for fx in (0, 1):
        syn = new_synth(FONT)
        lib.smp_synth_set_param(syn, SF_EFFECTS, fx)
        lib.smp_synth_set_offline(syn, 1)
        lib.smp_synth_midi(syn, bytes([0xC0, 16]), 2, 0)      # drawbar organ: loops, so every note is held
        for k in range(NOTES): lib.smp_synth_midi(syn, bytes([0x90, 40 + k, 100]), 3, 0)
        lib.smp_synth_process(syn, outs, BLOCK)               # starts them (waits for the samples)
        nb = SECS * RATE // BLOCK
        t0 = time.perf_counter()
        for _ in range(nb): lib.smp_synth_process(syn, outs, BLOCK)
        dt = time.perf_counter() - t0
        pk = ci(); nv = lib.smp_synth_voices(syn, ctypes.byref(pk))
        print(f"CPU, {NOTES} organ notes held = {nv} voices sounding, reverb and chorus {'on' if fx else 'off'}: {dt:.3f} s for {SECS} s of audio = {100 * dt / SECS:.2f} % of real time")
        lib.smp_synth_free(syn)

    # a second instance on the same font: private bytes of the second synth_new + load, samples already shared
    s1 = new_synth(FONT); lib.smp_synth_midi(s1, bytes([0x90, 60, 100]), 3, 0)
    lib.smp_synth_set_offline(s1, 1); lib.smp_synth_process(s1, outs, BLOCK)
    m0 = priv()
    s2 = new_synth(FONT); lib.smp_synth_midi(s2, bytes([0x90, 60, 100]), 3, 0)
    lib.smp_synth_set_offline(s2, 1); lib.smp_synth_process(s2, outs, BLOCK)
    print(f"A second instance on the same font (same preset playing): private bytes grew {(priv() - m0) / 1e6:.1f} MB (target under 2 MB)")
    lib.smp_synth_free(s2); lib.smp_synth_free(s1)
    lib.smp_sf_close(font)

# ---- streaming a GigaStudio-sized SFZ piano from E: ----
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = vp
k32.CreateFileW.argtypes = [ctypes.c_wchar_p, u32, u32, vp, u32, u32, vp]
k32.CloseHandle.argtypes = [vp]

def purge(paths):
    """Pushes files out of the Windows file cache without admin rights: opening a file unbuffered
    (FILE_FLAG_NO_BUFFERING) while no other handle has it open makes NTFS flush and drop its cached pages."""
    for p in paths:
        h = k32.CreateFileW(p, 0x80000000, 1, None, 3, 0x20000000, None)   # GENERIC_READ, share read, OPEN_EXISTING
        if h and h != ctypes.c_void_p(-1).value: k32.CloseHandle(h)

def read_speed(p):
    t0 = time.perf_counter()
    with open(p, "rb") as fh:
        n = len(fh.read())
    return n / (time.perf_counter() - t0) / 1e6

def disk_reads():
    try: return {k: v.read_bytes for k, v in psutil.disk_io_counters(perdisk=True).items()}
    except Exception: return {}

def disk_delta(before):
    """What the physical disks read since before (MB), the ones that read over 1 MB: shows the reads really went
    to the disk rather than the file cache."""
    now = disk_reads()
    d = {k: (now[k] - before.get(k, 0)) / 1e6 for k in now}
    return ", ".join(f"{k} read {v:.0f} MB" for k, v in d.items() if v > 1) or "no disk read over 1 MB"

def wav_bytes(x, rate):
    """A 16-bit stereo PCM wav (x: frames x 2 int16)."""
    import struct
    data = np.ascontiguousarray(x).tobytes()
    fmt = struct.pack("<HHIIHH", 1, 2, rate, rate * 4, 4, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body

if "stream" in PARTS:
    base_dir = os.environ.get("MEASURE_STREAM_DIR", r"E:\samplepacks\_build\samplosis_stream_test")
    made_base = not os.path.isdir(base_dir)
    os.makedirs(base_dir, exist_ok=True)
    sdir = tempfile.mkdtemp(prefix="bigpiano_", dir=base_dir)
    try:
        SR, LEN, LAYERS = 44100, 17.0, 4
        n = int(SR * LEN)
        t = np.arange(n) / SR
        rng = np.random.default_rng(1)
        bases = []                                      # 8 different decaying piano-like tones, reused by every file
        for b in range(8):
            f0 = 110.0 * 2 ** (b / 8)
            env = np.exp(-t / (2.0 + b * 0.4))
            x = sum(np.sin(2 * np.pi * f0 * h * t + b) / h for h in range(1, 7)) * env * 0.35
            x = x + 0.02 * env * rng.standard_normal(n)
            L = np.clip(x * 32767, -32767, 32767).astype(np.int16)
            R = np.clip(np.roll(x, 7) * 0.95 * 32767, -32767, 32767).astype(np.int16)
            bases.append(wav_bytes(np.stack([L, R], 1), SR))
        os.makedirs(os.path.join(sdir, "samples"))
        files, lines = [], ["<control> default_path=samples/", "<global> ampeg_release=0.3"]
        t0 = time.perf_counter()
        for key in range(21, 109):
            for layer in range(LAYERS):
                name = f"k{key:03d}_v{layer + 1}.wav"
                p = os.path.join(sdir, "samples", name)
                with open(p, "wb") as fh:
                    fh.write(bases[(key + layer) % 8])
                files.append(p)
                lines.append(f"<region> sample={name} key={key} lovel={layer * 32 + 1} hivel={layer * 32 + 32}")
        sfz = os.path.join(sdir, "Big Piano.sfz")
        with open(sfz, "w") as fh:
            fh.write("\n".join(lines) + "\n")
        total = sum(os.path.getsize(p) for p in files)
        print(f"\nStreaming: wrote {len(files)} files, {total / 1e9:.2f} GB, to {sdir} in {time.perf_counter() - t0:.0f} s")
        purge(files + [sfz])
        print(f"After the purge, reading one file ran at {read_speed(files[5]):.0f} MB/s "
              f"(a second read, from the cache: {read_speed(files[5]):.0f} MB/s)")
        purge(files + [sfz])
        err = ctypes.create_string_buffer(512)
        m0 = priv(); t0 = time.perf_counter()
        f = lib.smp_sf_open(sfz.encode(), err, 512); assert f, err.value
        dt = time.perf_counter() - t0
        print(f"Open, cold (every sample file's header probed): {dt * 1000:.0f} ms, {lib.smp_sf_sample_count(f)} samples, "
              f"private bytes grew {(priv() - m0) / 1e6:.1f} MB")
        lib.smp_sf_close(f)
        t0 = time.perf_counter()
        f = lib.smp_sf_open(sfz.encode(), err, 512); assert f, err.value
        print(f"Open again (headers from the probe cache): {(time.perf_counter() - t0) * 1000:.0f} ms")
        lib.smp_sf_close(f)
        # Each run: everything closed and purged (the font's open handles would keep pages cached), the font opened,
        # the heads loaded (cold), then the notes struck within a few milliseconds of each other and played for 10 s
        # in real time. The streamed frames were never read before, so they come from the disk. Then the same notes
        # rendered offline (exact: it waits for the disk) show where the real-time render dropped out.
        for notes in (32, 32, 64, 64):
            purge(files + [sfz])
            m0 = priv(); d0 = disk_reads(); t0 = time.perf_counter()
            f = lib.smp_sf_open(sfz.encode(), err, 512); assert f, err.value
            syn = new_synth(sfz)
            lib.smp_synth_set_param(syn, SF_VOICES, 256)
            dt = time.perf_counter() - t0 + use_presets(syn)
            heads = f"open and heads, cold: {dt * 1000:.0f} ms, {lib.smp_sf_loaded_bytes(f) / 1e6:.1f} MB of heads for " \
                    f"{total / 1e9:.2f} GB, private bytes grew {(priv() - m0) / 1e6:.1f} MB ({disk_delta(d0)})"
            keys = list(range(30, 30 + notes))          # each note a different file (velocities over the layers)
            ev = "\n".join(f"midi 144 {k} {[20, 50, 80, 110][i % 4]} {i * 64}" for i, k in enumerate(keys))
            m0 = priv(); d0 = disk_reads()
            x, wall, cpu = render(syn, ev, SECS, realtime=True)
            dd = disk_delta(d0)
            grew = (priv() - m0) / 1e6
            pk = ci(); lib.smp_synth_voices(syn, ctypes.byref(pk))
            under = lib.smp_synth_underruns(syn)
            ref, _, _ = render(syn, ev, SECS, realtime=False)
            w, bad = RATE // 10, []
            for i in range(SECS * 10):
                r = ref[i * w:(i + 1) * w].astype(np.float64)
                if np.sqrt(np.mean((r - x[i * w:(i + 1) * w]) ** 2)) > 0.1 * np.sqrt(np.mean(r ** 2)): bad.append(i / 10)
            print(f"{notes} stereo notes ({pk.value} voices at the peak), 10 s in real time from the cold disk: "
                  f"underruns {under} (voice-blocks), 100 ms windows more than -20 dB off the exact render: {len(bad)}"
                  + (f" (at {', '.join(f'{b:.1f}' for b in bad[:12])} s)" if bad else "") +
                  f"; {wall:.2f} s wall, {cpu:.2f} s CPU; {dd}; private bytes grew {grew:.1f} MB while playing; {heads}")
            lib.smp_synth_free(syn)
            lib.smp_sf_close(f)
    finally:
        shutil.rmtree(sdir, True)
        if made_base:
            try: os.rmdir(base_dir)
            except OSError: pass

# ---- an instrument and its notes: gm.dls, and a real SFZ library ----
def instrument(path, label, notes):
    """Opens path, reports what it ignores and its problem, loads the first preset and plays notes (MIDI note
    numbers, channel 1) offline for 3 s and in real time."""
    err = ctypes.create_string_buffer(512)
    print(f"\n{label}: {path}")
    m0 = priv(); t0 = time.perf_counter()
    f = lib.smp_sf_open(path.encode(), err, 512)
    if not f:
        print("Can't open:", err.value.decode("utf-8", "replace")); return
    dt = time.perf_counter() - t0
    k, first = ignored(f)
    prob = (lib.smp_sf_problem(f) or b"").decode("utf-8", "replace")
    print(f"Open: {dt * 1000:.0f} ms, {lib.smp_sf_preset_count(f)} presets, {lib.smp_sf_sample_count(f)} samples, "
          f"private bytes grew {(priv() - m0) / 1e6:.1f} MB; ignored {k} settings" + (f" (first: {first})" if k else "") +
          (f"; problem: {prob}" if prob else ""))
    m0 = priv()
    syn = new_synth(path)
    dt = use_presets(syn)
    print(f"Presets in use loaded: {dt * 1000:.0f} ms, {lib.smp_sf_loaded_bytes(f) / 1e6:.1f} MB of samples, private bytes grew {(priv() - m0) / 1e6:.1f} MB")
    for rt in (False, True):
        ev = "\n".join(f"midi 144 {nn} {v} {i * 4800}" for i, (nn, v) in enumerate(notes)) + \
             "\n" + "\n".join(f"midi 128 {nn} 0 {int(2.2 * RATE)}" for nn, _ in notes)
        x, wall, cpu = render(syn, ev, 3.0, realtime=rt)
        pk = ci(); lib.smp_synth_voices(syn, ctypes.byref(pk))
        print(f"{'Real time' if rt else 'Offline'}: {len(notes)} notes, peak {db(np.abs(x).max()):.1f} dBFS, "
              f"RMS over the notes {db(np.sqrt(np.mean(x[:int(2.2 * RATE)] ** 2))):.1f} dB, {pk.value} voices at the peak, "
              f"underruns {lib.smp_synth_underruns(syn)}, finite {bool(np.isfinite(x).all())}")
    lib.smp_synth_free(syn)
    lib.smp_sf_close(f)

GM_DLS = r"C:\Windows\System32\drivers\gm.dls"
if "dls" in PARTS and os.path.exists(GM_DLS):
    instrument(GM_DLS, "DLS", [(60, 100), (64, 90), (67, 80), (72, 110)])
if "sfz" in PARTS and os.environ.get("MEASURE_SFZ"):
    instrument(os.environ["MEASURE_SFZ"], "SFZ library", [(48, 60), (55, 90), (60, 110), (64, 100), (67, 127)])
