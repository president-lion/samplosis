# Samplosis Stage 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Samplosis CLAP that Mike can load in REAPER to play one sample with everything ReaSamplOmatic5000 does for one sample. It adds keep length (pitched notes last as long as the original), an accessible window with a Settings page, and its state saved as text.

**Architecture:**
- **Engine.** A plain C engine (`smp_*` functions) does the work: decoding, pitch, the cache, rendering, streaming and voices. It's exported from samplosis.clap itself, so pytest drives it through ctypes, the way the skit tools drive cosmos_binaural.clap.
- **Cache.** Every sound the engine plays is a raw float file in the disk cache: the decoded original, a reversed copy, and Rubber Band keep-length copies. Only the first 0.25 s of each is held in memory. A streamer thread reads the rest into per-voice ring buffers, so E: (a USB hard disk) is read once, at import.
- **Plugin.** plugin.c wraps the engine as a CLAP instrument. gui.c is the Win32 window.

**Tech Stack:**
- C11 and C++ (Rubber Band 3.3.0, R3 engine) with MSVC cl, in Visual Studio 18 Community.
- The CLAP SDK headers at E:\samplepacks\_build\clap\include.
- dr_wav, dr_flac, dr_mp3 and stb_vorbis, through decode.c.
- pytest 9, numpy and ctypes for the tests.
- tests/host.c, a small CLAP host for plugin-level and window tests.

**Spec:** E:\samplepacks\claudemusic\samplosis\docs\superpowers\specs\2026-10-28-samplosis-design.md

**Later stages** each get their own plan. This stage's interfaces must leave room for them:
- **2:** many samples with zones, auto-mapping, velocity layers and round robins, drag and drop, and the zone list.
- **3:** Ableton-style shaping: the filter, the pitch and filter envelopes, LFOs, modulation routings, glide, legato, choke and crossfades.
- **4:** SoundFonts (SF2 and SF3, one-instrument and all-channels modes).
- **5:** the `samplosis` terminal tool: live control and render without REAPER.
- **6:** tempo lock and the 16 outputs.

## Global Constraints

- **Location.** The project root is E:\samplepacks\claudemusic\samplosis, its own git repo, branch main. It's never pushed unless Mike asks. Commit messages end with "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>".
- **Naming.** The plugin is "Samplosis" by vendor "audioskit", id `com.audioskit.samplosis`, version 0.1.0. The file is samplosis.clap, installed to D:\clap.
- **Licence.** It's GPL because of Rubber Band. Copy COPYING from cosmos_clap/rubberband.
- **Audio thread.**
  - It never allocates, locks, waits, or touches the disk or the file system.
  - Its only kernel call is SetEvent, to wake the streamer.
  - Everything shared with it is handed over by atomic pointer or atomic value.
- **Defaults.** The cache folder defaults to D:\samplosis\cache with a 10 GB limit. Settings live in %APPDATA%\Samplosis\settings.txt, or the file named by the environment variable SAMPLOSIS_SETTINGS (tests use that).
- **Keep length** is on by default for loaded samples.
- **The window:**
  - Only plain Win32 controls.
  - Tab and Shift+Tab are handled by every control, and leave the plugin past the ends (gui.c's tab_out).
  - Focus is never lost.
  - Choices are combo boxes.
  - Up and Down stop at the limits. Shift moves 10 times as far and Control a tenth.
  - Every change applies at once.
  - Values are read in words ("C4", "-6.0 dB", "120 ms").
- **Copying code.** Copy decode.c, decode.h, decode/, rb.cpp and rubberband/ from E:\samplepacks\claudemusic\skittools\cosmos_clap without changing them. Read gui.c and fxplug.c there for the Tab and focus patterns. Never modify anything in skittools.
- **Machine rules:**
  - Build and test outputs stay in the project folder or the system temp folder, never on C: user folders beyond that.
  - Tests that open windows send keys by message, never SendInput (Mike uses the desktop).
  - Keep window test runs few, because NVDA may speak.

## Review Focus

The most likely ways this could fail for a person, with the test that pins each:

1. **A note played while its keep-length copy is still rendering** must sound at the right pitch at once, through resampling. It must never be silent or late. Pinned by `test_note_before_render_uses_resample` in Task 6.
2. **A sample on the USB disk that's deleted or moved after loading** must keep playing from the cache, and reload with a clear message in the window. It must never crash. Pinned by `test_source_missing_plays_from_cache` in Task 4 and `test_state_with_missing_file` in Task 8.
3. **A full cache folder or a cache on a missing drive** (D: unplugged, or a typo in Settings) means keep length quietly falls back to resampling. The status line says why, and the plugin never crashes. Pinned by `test_bad_cache_dir_falls_back` in Task 3 and Task 6.
4. **A host sample rate other than the file's** (44.1 kHz file, 48 or 96 kHz project) must keep the correct pitch and length. Pinned by `test_rate_conversion` in Task 5.
5. **Big and odd files** (a 10-minute wav, a 1-frame file, a mono file, a 24-bit or mp3 file, a silent file with no pitch) must import without long stalls or crashes. Pitch falls back to C4 (60) when nothing is found. Pinned by `test_odd_files` in Task 4.

---

## File structure

All paths are relative to E:\samplepacks\claudemusic\samplosis.

**Build and housekeeping:**
- `build.bat`: builds samplosis.clap, and tests\host.exe with `build.bat test`.
- `install.bat`: copies samplosis.clap to D:\clap.
- `COPYING`, `README.md` and `.gitignore` (build outputs, `*.obj`, `__pycache__`).

**Source, in src\:**
- `samplosis.h`: the public `smp_*` C API every other file shares, with `SMP_API` for dllexport.
- `decode.c`, `decode.h`, `decode\`, `rb.cpp` and `rubberband\`: copied as they are.
- `notes.c`: note names, and parsing and printing values in words.
- `pitch.c`: YIN pitch detection.
- `settings.c`: the settings file.
- `cache.c`: the cache files, keys, heads and LRU trimming.
- `sample.c`: importing a file into the cache, the root note, and reversed copies.
- `render.c`: keep-length renders (Rubber Band offline, R3) on a worker thread.
- `stream.c`: the streamer thread and the per-voice ring buffers.
- `engine.c`: voices, parameters, envelopes and the process loop.
- `script.c`: the instrument as text, written and parsed.
- `plugin.c`: the CLAP entry and the extensions: note ports, audio ports, params, state, gui and latency.
- `gui.c`: the window, with its main page and Settings page.

**Tests, in tests\:**
- `conftest.py`: loads samplosis.clap with ctypes, declares the signatures, sets SAMPLOSIS_SETTINGS to a temp file, and writes fixtures.
- `test_notes.py`, `test_pitch.py`, `test_cache.py`, `test_sample.py`, `test_render.py`, `test_engine.py`, `test_script.py` and `test_plugin.py`.
- `host.c`: the CLAP host for tests. Its commands are `play`, `state` and `gui`.

## Parameters (stage 1)

Every engine parameter has an id, and its script name equals its REAPER parameter name in lower case with spaces as underscores. Each entry below gives id, name, range, default and how it reads.

- **Pitch:**
  - 0 `root`: 0 to 127, default from the file. Reads as a note name ("C3"). The note the sample plays at its own pitch.
  - 1 `transpose`: -48 to 48 semitones, default 0. Reads "+7 st".
  - 2 `fine`: -100 to 100 cents, default 0. Reads "-12 cents".
  - 22 `bend_range`: 0 to 48 semitones, default 2.
- **Volume and pan:**
  - 3 `volume`: -60 to +12 dB, default 0. Reads "-6.0 dB". -60 reads "off".
  - 4 `pan`: -100 to 100, default 0. Reads "center", "30 left" or "30 right".
- **Envelope:**
  - 5 `attack`: 0 to 10000 ms, default 0. Reads "120 ms" or "1.20 s".
  - 6 `decay`: 0 to 10000 ms, default 0.
  - 7 `sustain`: -60 to 0 dB, default 0.
  - 8 `release`: 0 to 10000 ms, default 30.
- **Velocity:**
  - 9 `velocity_sens`: 0 to 100 %, default 100. How far velocity 1 is below velocity 127: 100 % means 1 is -40 dB, and 0 % means velocity is ignored.
  - 10 `min_volume`: -60 to 0 dB, default -60. ReaSamplOmatic's minimum volume, a floor for soft notes.
  - 15 `vel_lo` and 16 `vel_hi`: 1 to 127, defaults 1 and 127.
- **Mode and ranges:**
  - 11 `mode`: a choice, default `note`.
    - `sample`: every note plays the sample at its own pitch.
    - `note`: shifted by semitones from the root.
    - `free`: note_lo to note_hi maps linearly onto pitch_lo to pitch_hi.
  - 12 `note_lo` and 13 `note_hi`: 0 to 127, defaults 0 and 127.
  - 14 `pitch_lo` and 23 `pitch_hi`: -48 to 48 semitones, defaults -12 and 12. Used in free mode.
- **Playback:**
  - 17 `obey_note_off`: off or on, default on. Off means the note plays to the end of the sample.
  - 18 `loop`: a choice, `off`, `forward` or `pingpong`, default off.
  - 19 `loop_start`: a position, default 0.
  - 20 `start` and 21 `end`: 0 to 100 % of the sample, defaults 0 and 100. Reads "0.000 s (0.0 %)".
  - 24 `reverse`: off or on, default off.
  - 26 `voices`: 1 to 64, default 16. The oldest voice is stolen with a 5 ms fade.
  - 27 `probability`: 0 to 100 %, default 100. Uses a seeded xorshift, reseeded by `smp_engine_seed`.
- **Speed:**
  - 25 `keep_length`: off or on, default on.
  - 28 `speed`: 25 to 400 %, default 100. ReaSamplOmatic's speed: it scales the playback rate on top of pitch. With keep length on, it changes only the length, and the pitch stays.

`SMP_NPARAMS` is 29. Every parameter is a REAPER parameter.

---

### Task 1: Project, build and test harness

**Files:**
- Create: `build.bat`, `install.bat`, `.gitignore`, `COPYING`, `README.md`, `src\samplosis.h`, `src\engine.c` (version only for now), the copied files listed in Global Constraints, and `tests\conftest.py`.
- Test: `tests\test_notes.py` (first test only).

**Interfaces:**
- Produces:
  - `SMP_API const char *smp_version(void)`, returning "0.1.0".
  - `conftest.lib`, the ctypes CDLL.
  - The `conftest.settings_file` fixture, a temp settings path set in SAMPLOSIS_SETTINGS before the DLL loads.
  - `conftest.write_wav(path, data: np.ndarray, rate: int)`, which writes float32 wavs through soundfile.

- [ ] **Step 1: Create the folder and git init.** Copy the vendored files and COPYING. Write `.gitignore`.
- [ ] **Step 2: Write `build.bat`.**
  - Find VS with vswhere, as cosmos_clap/build.bat does.
  - Compile `rb.cpp` with the same flags as cosmos_clap.
  - Then run `cl /nologo /D_CRT_SECURE_NO_WARNINGS /O2 /W3 /LD /MT /I E:\samplepacks\_build\clap\include src\*.c rb.obj /Fe:samplosis.clap /link user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib`.
  - `build.bat test` also builds `tests\host.exe` from `tests\host.c`.
  - `CLAP_SDK` overrides the include folder.
- [ ] **Step 3: Write the failing test.**

```python
def test_version(lib):
    assert lib.smp_version() == b"0.1.0"
```

- [ ] **Step 4: Run it.** `python -m pytest tests -q` should fail, because samplosis.clap is missing or has no smp_version.
- [ ] **Step 5:** Implement `smp_version` in `engine.c`, and run `build.bat`.
- [ ] **Step 6: Run the tests.** Expected: 1 passed.
- [ ] **Step 7: Commit.** "Samplosis: project, build and test harness".

### Task 2: Note names, values in words, and pitch detection

**Files:**
- Create: `src\notes.c` and `src\pitch.c`.
- Test: `tests\test_notes.py` and `tests\test_pitch.py`.

**Interfaces:**
- `int smp_note_parse(const char *s)` parses a note name: C-1 is 0, C4 is 60, sharps and flats are allowed, and so is a bare number from 0 to 127. It returns -1 if it can't.
- `void smp_note_name(int n, char *out, int cap)` uses sharps: 61 is "C#4".
- `int smp_note_from_filename(const char *path)` returns the last note-like token in the file's base name, or -1.
  - A token is a note name standing alone between separators (space, `_`, `-`, `.`, brackets), case-insensitive.
  - Examples: "Guitar C3.wav" is 48, "piano_f#2_soft.flac" is 42, "Bb4 pluck.wav" is 70.
  - "Cello A 440.wav" is 69. A bare "440" or "A440" means A4.
  - "Take2.wav" is -1, and so is "Chord.wav".
- `double smp_detect_pitch(const float *mono, long n, int rate)` returns a fractional MIDI note, or -1 when it can't tell.
  - It uses YIN with a 0.15 threshold over the loudest 0.5 s of the first 3 s, in 2048-sample windows, with the median of the frames that have pitch.
  - Its range is 30 Hz to 4 kHz.
  - The skit tools' E:\samplepacks\_build\profmark\yin.py is a reference.
- `int smp_value_text(int param, double v, char *out, int cap)` and `int smp_value_parse(int param, const char *s, double *v)` turn values into words and back, for every parameter in the list above. Parsing accepts the words it writes ("-6 dB", "-6", "C3", "on", "30 left").

- [ ] **Step 1: Write the failing tests.**
  - `test_note_parse`: "C-1" is 0, "C4" is 60, "a4" is 69, "Db4" is 61, "B#3" is 60, "61" is 61, and "H2" is -1.
  - `test_note_name`: 61 is "C#4" and 0 is "C-1".
  - `test_note_from_filename`: the examples above, plus "E:/x/Kick 01.wav" is -1.
  - `test_detect_pitch`:
    - Sines at 110, 220, 261.63 and 1000 Hz at 48000 and 44100, within 0.1 semitone.
    - A plucked string (a Karplus-Strong at 196 Hz, written in the test) within 0.2.
    - White noise gives -1, and so does silence.
  - `test_value_roundtrip`: for each parameter, 5 values across its range turned to words and parsed back are equal within its display precision.
- [ ] **Step 2: Run them.** They fail (missing symbols).
- [ ] **Step 3:** Implement notes.c and pitch.c.
- [ ] **Step 4: Run the tests.** They pass.
- [ ] **Step 5: Commit.** "Note names, values in words, YIN pitch".

### Task 3: Settings and the disk cache

**Files:**
- Create: `src\settings.c` and `src\cache.c`.
- Test: `tests\test_cache.py`.

**Interfaces:**
- **Settings:**
  - `typedef struct { char cache_dir[1024]; double cache_limit_gb; } SmpSettings;`
  - `void smp_settings_get(SmpSettings *s)` loads them, cached and reloaded when the file changes.
  - `int smp_settings_set(const SmpSettings *s)` saves them, returning 0 when it works.
  - The file has one `name value` per line: `cache_dir D:\samplosis\cache` and `cache_limit_gb 10`. A missing file means the defaults.
- **Cache files.** Each is `<16 hex digits>.smpc`.
  - The header is 32 bytes:
    - "SMPC"
    - uint32 version 1
    - uint32 channels, always 2
    - uint32 rate
    - uint64 frames
    - 8 reserved bytes
  - Then float32 stereo, interleaved.
- **Keys.** `uint64_t smp_key(const char *desc)` is a 64-bit FNV-1a hash of a description string. The descriptions are:
  - `src|<path>|<size>|<mtime>` for an import.
  - `rev|<src key>` for a reversed copy.
  - `kl|<src key or rev key>|<shift semitones>|r3` for a keep-length copy.
- **Writing and opening:**
  - `int smp_cache_write(uint64_t key, const float *lr, uint64_t frames, int rate)`: writes `.part`, then renames.
  - `typedef struct SmpEntry SmpEntry;`
  - `SmpEntry *smp_cache_open(uint64_t key)` returns NULL if the entry is missing.
    - An entry holds an open file handle, rate, frames, and a head: the first `min(frames, 0.25 s × rate)` frames in memory.
    - Files up to 1 s long are held whole. Opening one touches its last-write time, which drives the LRU order.
  - `void smp_cache_close(SmpEntry*)`.
  - `uint64_t smp_entry_frames(const SmpEntry*)`, `int smp_entry_rate(const SmpEntry*)`, `int smp_entry_whole(const SmpEntry*)`, and `const float *smp_entry_head(const SmpEntry*, uint64_t *head_frames)`.
  - `long smp_entry_read(SmpEntry*, uint64_t frame, float *lr, long frames)` reads from the file. It's for the streamer thread only.
- **Trimming.** `uint64_t smp_cache_trim(uint64_t limit_bytes)` deletes the oldest `.smpc` files until the total is at most the limit. It never deletes a file that's open. It returns the bytes left.
- **Errors.** `int smp_cache_ok(char *why, int cap)` returns 1 if the cache folder exists or can be created and written. Otherwise it returns 0 and a plain reason, such as "the cache folder D:\x can't be created".

- [ ] **Step 1: Write the failing tests.**
  - `test_settings_defaults_and_roundtrip`:
    - With no file, the defaults are D:\samplosis\cache and 10.
    - After setting tmp_path and 0.001, reading gives the same.
  - `test_cache_write_open_read`:
    - Write 3 s of a known ramp at 44100.
    - Open it: frames match, and the head is 0.25 s and equals the start of the ramp.
    - Reading 1000 frames at frame 50000 equals the ramp.
    - A 0.5 s entry is held whole.
  - `test_cache_trim_lru`:
    - Write 5 entries of 1 MB with stepped last-write times, then open the oldest.
    - Trimming to 2.5 MB keeps the opened one and the two newest.
  - `test_bad_cache_dir_falls_back`: set the cache folder to `Q:\nope` (a drive that doesn't exist). `smp_cache_ok` gives 0 and a reason containing "Q:\\nope". The writes return non-zero.
- [ ] **Step 2: Run them.** They fail.
- [ ] **Step 3:** Implement them, using Win32 file APIs with UTF-8 paths converted to wide strings.
- [ ] **Step 4: Run the tests.** They pass.
- [ ] **Step 5: Commit.** "Settings file and the sample cache".

### Task 4: Importing a sample

**Files:**
- Create: `src\sample.c`.
- Test: `tests\test_sample.py`.

**Interfaces:**
- Consumes: decode_audio, smp_key, smp_cache_write, smp_cache_open, smp_note_from_filename and smp_detect_pitch.
- Produces:

```c
typedef struct SmpSample {
   char path[1024]; char name[256];      /* name: the file's base name */
   uint64_t src_key, rev_key;            /* rev_key is made on first use */
   int rate; uint64_t frames;
   int root;                             /* from the file name, else detected, else 60 */
   int root_from;                        /* 0 name, 1 detected, 2 default */
   SmpEntry *fwd, *rev;                  /* rev NULL until needed */
} SmpSample;
SmpSample *smp_sample_load(const char *path, char *err, int cap);   /* NULL and a plain message on failure */
SmpEntry *smp_sample_reversed(SmpSample *s);                        /* makes or opens the reversed copy */
void smp_sample_free(SmpSample *s);
```

- **Import.**
  - If the `src|...` key is already in the cache, the source file isn't decoded again. It's only stat'ed.
  - Otherwise decode_audio decodes it: mono is doubled to both sides, and more than 2 channels are cut to the first two.
  - It's written to the cache at its own rate.
  - Pitch detection runs on the mono mix, only when the file name gives no note.
  - The root and how it was found are kept in a sidecar, `<src key>.txt`, so detection runs once per file.
- **A missing source.** If the source file no longer exists but its last known `src|` key is passed by the caller (state reload), the cache entry is used. See `smp_sample_load_known(path, src_key, root, err, cap)`, also exported.

- [ ] **Step 1: Write the failing tests.**
  - `test_load_wav_root_from_name`: "Guitar C3.wav" of a 220 Hz sine gets root 48 from the name, root_from 0, and the right frames and rate.
  - `test_load_detects_root`: "pluck.wav" at 196 Hz gets root 55 and root_from 1. A second load doesn't decode (time it at under 20 ms, or check a debug counter, `smp_debug_decodes()`).
  - `test_odd_files`:
    - A 1-frame wav loads.
    - A mono 24-bit wav loads with both sides equal.
    - An mp3 made with ffmpeg loads.
    - A silent wav gets root 60 and root_from 2.
    - A 10-minute 44.1 kHz wav loads in under 5 s on a cold cache and under 50 ms warm.
    - A missing path returns NULL with a message containing the file name.
  - `test_reversed`: the reversed entry's frames equal the forward one's, reversed.
  - `test_source_missing_plays_from_cache`: load, delete the source, then `smp_sample_load_known` with the same key works, and its head equals the earlier one.
- [ ] **Step 2: Run them.** They fail.
- [ ] **Step 3:** Implement sample.c.
- [ ] **Step 4: Run the tests.** They pass.
- [ ] **Step 5: Commit.** "Import samples into the cache, root by name or pitch".

### Task 5: Streaming voices and the basic engine

**Files:**
- Create: `src\stream.c` and `src\engine.c`.
- Test: `tests\test_engine.py`.

**Interfaces:**
- Consumes: SmpSample and SmpEntry.
- Produces:

```c
typedef struct SmpEngine SmpEngine;
SmpEngine *smp_engine_new(double rate, int max_block);   /* starts the streamer thread; preallocates 64 voices */
void smp_engine_free(SmpEngine *e);
void smp_engine_set_sample(SmpEngine *e, SmpSample *s);  /* main thread; atomic swap, old one freed after the
                                                            audio thread has passed a block boundary */
void smp_engine_set_param(SmpEngine *e, int id, double v);   /* any thread; atomic */
double smp_engine_get_param(const SmpEngine *e, int id);
void smp_engine_note_on(SmpEngine *e, int key, int vel, int at);  /* at: frame offset in the next block */
void smp_engine_note_off(SmpEngine *e, int key, int at);
void smp_engine_bend(SmpEngine *e, double semis_fraction /* -1..1 */, int at);
void smp_engine_process(SmpEngine *e, float *l, float *r, int n);  /* audio thread */
int smp_engine_active_voices(const SmpEngine *e);
long smp_engine_underruns(const SmpEngine *e);
void smp_engine_seed(SmpEngine *e, unsigned seed);
/* test helper: plays notes offline in blocks and writes a stereo float32 wav; events "on key vel frame", "off key frame" */
int smp_engine_render_test(SmpEngine *e, const char *events, long frames, int block, const char *wav_out);
```

- **The streamer.**
  - Each voice reads from the entry's head in memory first, then from a 65536-frame ring buffer.
  - The streamer thread refills any ring that's less than half full, reading through `smp_entry_read`. It handles start, end, loop wrapping and pingpong as reading positions.
  - The audio thread calls SetEvent after each block, and the streamer also polls every 5 ms.
  - When a ring runs dry, the voice plays silence and `underruns` counts it.
  - Whole entries need no ring.
- **Playback rate.** It's `2^(semitones/12) × entry_rate / host_rate × speed`, where semitones comes from the mode, root, transpose, fine and bend. It's read with cubic Hermite interpolation.
- **Gain.** The ADSR uses linear segments in dB for decay and release, and linear amplitude for attack. Velocity and min_volume are applied as in the parameter list. Pan is equal-power.
- **Voices.** A new voice beyond `voices` steals the oldest with a 5 ms fade. Notes outside note_lo to note_hi or vel_lo to vel_hi are ignored. The probability check happens at note-on.

- [ ] **Step 1: Write the failing tests.** They render with `smp_engine_render_test`.
  - `test_note_mode_pitch`: a 220 Hz sample with root A3 (57). Note E4 (64) plays at 329.6 Hz within 0.1 semitone, and keep length is off for this test.
  - `test_sample_mode_ignores_key`: in sample mode, any note gives 220 Hz.
  - `test_free_mode`: note_lo 48, note_hi 72, pitch_lo -12, pitch_hi 12. Note 60 plays at the root pitch, and 72 plays at +12.
  - `test_rate_conversion`: a 44.1 kHz file played by a 48 kHz engine and by a 96 kHz engine has the same pitch and length in seconds, within 0.5 %.
  - `test_envelope`:
    - With attack 100 ms, the RMS of the first 10 ms is at least 20 dB below that from 100 to 110 ms.
    - With release 200 ms after note-off, the level is at least 30 dB down 200 ms later.
  - `test_obey_note_off_off`: a note off has no effect, and the note plays to the end.
  - `test_velocity`: with velocity_sens 100, velocity 1 is 40 ± 1 dB below 127. With 0 they're equal.
  - `test_loop_forward_and_pingpong`: a 0.5 s sample looped, with a note held for 3 s, still sounds at 2.9 s and has no gap over 1 ms.
  - `test_reverse`: the output starts with the sample's last part.
  - `test_voices_limit`: with voices 2 and 3 notes, there are 2 active.
  - `test_probability`: at 50 % with seed 1, 1000 notes give 450 to 550 sounding.
  - `test_streaming_long_sample`:
    - A 60 s file played for 60 s in 256-frame blocks has 0 underruns, and its output matches an offline reference resample within -60 dB.
    - The process memory grows by less than 20 MB, checked with psutil.
- [ ] **Step 2: Run them.** They fail.
- [ ] **Step 3:** Implement stream.c and engine.c.
- [ ] **Step 4: Run the tests.** They pass.
- [ ] **Step 5: Commit.** "Streaming voices: modes, envelope, loop, reverse, probability".

### Task 6: Keep length

**Files:**
- Create: `src\render.c`.
- Modify: `src\engine.c`.
- Test: `tests\test_render.py`.

**Interfaces:**
- **Rendering:**
  - `int smp_render_shift(SmpSample *s, int reversed, int semis)` renders one keep-length copy now, on the calling thread, into the cache (key `kl|...|semis|r3`). It returns 0 when it works.
  - Rubber Band runs with `RubberBandOptionProcessOffline | RubberBandOptionEngineFiner | RubberBandOptionChannelsTogether`, time ratio 1.0 and pitch scale 2^(semis/12). It studies the whole sample, then processes it, then retrieves everything, and trims or pads to exactly the source's frame count.
- **The engine:**
  - It owns a render worker thread at below-normal priority.
  - `smp_engine_set_sample` queues renders for every integer shift that note_lo to note_hi reaches (from mode, root and transpose), nearest the root first.
  - A note-on whose shift isn't ready moves that shift to the front of the queue.
  - Finished copies are opened with `smp_cache_open` and published into a 256-slot table (shifts -127 to 127) by atomic pointer.
- **Playing:**
  - With keep_length on, a note plays the ready copy for the nearest integer shift. The remainder (fine, bend, free mode) is resampled at `2^(rest/12)`, and speed scales the time, not the pitch.
  - A note-on whose copy isn't ready plays by resampling for that whole note.
  - A bad cache folder means no renders, resampling, and a status reason.
- **Status.**
  - `int smp_engine_render_status(SmpEngine*, int *ready, int *total, char *why, int cap)` feeds the window's status line.
  - `int smp_engine_wait_renders(SmpEngine*, int timeout_ms)` is for tests.

- [ ] **Step 1: Write the failing tests.**
  - `test_render_shift`: a 2 s plucked 196 Hz sample rendered at +7 has a pitch of 293.7 Hz within 0.15 semitone, exactly the same frame count, and an RMS envelope within 2 dB of the original's at 0.5, 1.0 and 1.5 s.
  - `test_chord_notes_end_together`:
    - With keep length on, play C, E and G together on a 2 s decaying sample with root C.
    - Isolate each note by rendering it alone. Each note's time to fall to -40 dB from its peak is within 3 % of the others'.
    - With keep length off, G ends first (its time is shorter than C's by at least 15 %).
  - `test_note_before_render_uses_resample`:
    - Clear the cache, set the sample, and immediately play +12.
    - The output is non-silent within 5 ms and at the right pitch.
    - After `wait_renders`, the same note matches the rendered copy's frame count.
  - `test_renders_cached`: a second engine with the same sample has every shift ready immediately, with no renders run (status total equals ready at once).
  - `test_bad_cache_dir_falls_back` (engine): with the cache folder at Q:\nope, notes sound by resampling and the status reason mentions the folder.
  - `test_speed_with_keep_length`: speed 50 % doubles the note's length and keeps its pitch.
- [ ] **Step 2: Run them.** They fail.
- [ ] **Step 3:** Implement render.c and the engine changes.
- [ ] **Step 4: Run the tests, including the earlier ones.** They pass.
- [ ] **Step 5: Commit.** "Keep length: Rubber Band R3 copies per key, rendered in the background".

### Task 7: The instrument as text

**Files:**
- Create: `src\script.c`.
- Test: `tests\test_script.py`.

**Interfaces:**
- `int smp_script_write(const SmpEngine *e, char *out, int cap)` returns the length needed.
- `int smp_script_apply(SmpEngine *e, const char *text, char *err, int cap)` returns 0 when it works. On an error, nothing is changed, and err says the line number and why.
- **The format.** One setting per line, as `name value`, with values in the same words as the window (smp_value_text). `#` starts a comment. The sample line is:

```
# Samplosis 0.1.0
sample "E:\samples\Guitar C3.wav" key=8f2a6c01d3e4b5a7
root C3
keep_length on
mode note
volume -6.0 dB
attack 5 ms
```

- **Writing and reading.** The writer writes every parameter, in id order, after the sample line. The parser accepts any order and any subset, and leaves the rest at their defaults. `key=` lets the cached sample load when the file has gone. Stage 2 adds `zone ... end` blocks, so unknown names are an error that names the line.

- [ ] **Step 1: Write the failing tests.**
  - `test_roundtrip`: random values for every parameter, written, applied to a fresh engine and written again, give an identical text.
  - `test_partial_and_order`: "volume -3\nroot A3" sets those two and leaves the rest at their defaults.
  - `test_errors`: "volme -3" gives an error with "line 1" and "volme", and the engine is unchanged. So does "root H9".
  - `test_comments_and_blank_lines`.
- [ ] **Step 2: Run them.** They fail.
- [ ] **Step 3:** Implement script.c.
- [ ] **Step 4: Run the tests.** They pass.
- [ ] **Step 5: Commit.** "Instruments as text".

### Task 8: The CLAP plugin

**Files:**
- Create: `src\plugin.c` and `tests\host.c`.
- Modify: `build.bat` (the `test` target).
- Test: `tests\test_plugin.py`, which runs host.exe.

**Interfaces:**
- **The plugin.**
  - The id is `com.audioskit.samplosis` and the features are `instrument`, `sampler` and `stereo`.
  - Extensions:
    - note-ports: one input, CLAP and MIDI dialects.
    - audio-ports: one stereo output.
    - params: all `SMP_NPARAMS` parameters, automatable, with value text through smp_value_text and smp_value_parse, and choices as stepped parameters.
    - state: the script text.
    - gui (Task 9).
    - latency: 0.
- **Process.** It reads note on and off, MIDI note and pitch-bend messages, and param value events, at sample accuracy, splitting the block at events.
- **State.**
  - Saving writes `smp_script_write`.
  - Loading applies the script. The sample load happens on the main thread, synchronously when the source or cache is present.
  - A missing file and missing cache leave the plugin silent, with the status "Missing: <path>".
- **host.c commands** (it prints results as plain lines):
  - `host.exe samplosis.clap play <state.txt> <events> <out.wav>`
  - `host.exe samplosis.clap state <state.txt>`, which loads the state, saves it and prints it.
  - `host.exe samplosis.clap gui <state.txt>`, in Task 9.

- [ ] **Step 1: Write the failing tests.**
  - `test_descriptor`: `host state` prints the plugin name "Samplosis", vendor "audioskit" and version 0.1.0.
  - `test_play_through_clap`: a C4 note through the plugin equals `smp_engine_render_test` with the same state, within -90 dB.
  - `test_state_roundtrip`: loading a script and saving it gives the same text.
  - `test_param_events`: a volume automation event of -12 dB mid-block changes the level at that frame.
  - `test_state_with_missing_file`: if the path is gone but the cache key is present, it plays. If both are gone, the status is "Missing: ..." and the output is silent, with no crash.
- [ ] **Step 2: Run them.** They fail.
- [ ] **Step 3:** Implement plugin.c and host.c, then run `build.bat test`.
- [ ] **Step 4: Run the tests.** They pass.
- [ ] **Step 5: Commit.** "Samplosis as a CLAP instrument".

### Task 9: The accessible window

**Files:**
- Create: `src\gui.c`.
- Modify: `src\plugin.c` (the gui extension) and `tests\host.c` (the `gui` command).
- Test: `tests\test_plugin.py::test_gui_*`.

**Interfaces:**
- **The main page, in Tab order:**
  1. A "Sample" read-only edit box: "Guitar C3.wav, 2.40 s, root C3 from the file name", or "No sample loaded".
  2. Buttons:
     - Browse: a file dialog for audio files.
     - Previous file and Next file: the neighbouring audio files in the same folder, alphabetically, wrapping round.
  3. "Keep length", a combo box (Off or On).
  4. "Mode", a combo box (Sample, Note, Free).
  5. Then a label and edit box (or combo box for choices) for each remaining parameter, in parameter id order.
  6. A "Status" read-only edit box: "Keep length: 40 of 128 keys ready", or a reason.
  7. A "Settings" button.
- **Edit boxes.**
  - They show smp_value_text.
  - Enter or leaving the box parses the text. A bad entry beeps and puts the old value back.
  - Up and Down step: 1 semitone, 1 dB, 10 ms (envelopes), 1 % and 1 cent. Shift multiplies by 10 and Control divides by 10, and the value stops at its limits.
  - Changes apply at once, through smp_engine_set_param and the CLAP param host notify (`request_flush` or gesture events) so REAPER's automation sees them.
- **Drag and drop.** Dropping a file anywhere on the window loads it (through RegisterDragDrop with CF_HDROP). This is ReaSamplOmatic's drag in from Explorer and the Media Explorer.
- **The Settings page.** It replaces the main page in the same window:
  - "Cache folder", with a Browse folder button.
  - "Cache size limit", in GB, with Up and Down.
  - "Cache in use", read-only: "3.2 GB of 10 GB".
  - "Clear cache".
  - "Back".
  - Settings are saved at once.
- **Focus.** Tab and Shift+Tab move through the page's controls, and leave the plugin past the ends (copy tab_out from cosmos_clap/gui.c). When a page switches, focus goes to its first control. When REAPER focuses the plugin's window, focus goes on to the last focused control.
- **The status box** is refreshed by a 250 ms timer, only when its text changes, so NVDA isn't interrupted.
- **`host gui`.** It opens the window in a hidden top-level window with a button before and after it (as E:\samplepacks\_build\guitest\host.c does), sends keys by message, and prints each focused control's class, name and value.

- [ ] **Step 1: Write the failing tests.**
  - `test_gui_tab_order`: Tab from the first control visits every control in the order above, and the next Tab focuses the host's "after" button. Shift+Tab from the first goes to "before".
  - `test_gui_values_in_words`: Root reads "C3", Volume "0.0 dB", and Pan "center".
  - `test_gui_up_down_limits`: Up on Volume 13 times from +10 stops at "12.0 dB". Down on Voices at 1 stays at 1.
  - `test_gui_typed_value`: typing "-6" and Enter in Volume makes the engine param -6, and the box reads "-6.0 dB". Typing "loud" beeps and the box goes back.
  - `test_gui_next_file`: in a temp folder with a.wav, b.wav and c.wav loaded at b, Next loads c and Next again loads a. The Sample box updates.
  - `test_gui_settings_page`: Settings, then Tab through, changing the limit to 5 writes the file, and Back focuses the Settings button again.
  - `test_gui_keep_length_combo`: setting the combo to Off sets the parameter to 0.
- [ ] **Step 2: Run them.** They fail.
- [ ] **Step 3:** Implement gui.c and the host's gui command, then run `build.bat test`.
- [ ] **Step 4: Run the tests.** They pass, and the whole suite passes.
- [ ] **Step 5: Commit.** "Accessible window and Settings page".

### Task 10: Install and Mike's first try

**Files:**
- Create: `install.bat` and `README.md` (what stage 1 does, its controls, the script format and the settings).

- [ ] **Step 1:** Run `build.bat` then `install.bat`. Expected: D:\clap\samplosis.clap exists. If the copy fails as busy, ask Mike to close REAPER.
- [ ] **Step 2:** Run the whole suite: `python -m pytest tests -q`. Everything should pass. Report the counts.
- [ ] **Step 3: Measure.**
  - CPU time for 16 voices at 48 kHz, 256-frame blocks, through `smp_engine_render_test`. Expected: less than 2 % of real time.
  - Memory for a loaded 60 s sample with all keep-length copies ready. Expected: under 40 MB.
  - Put both numbers in README.md.
- [ ] **Step 4: Commit.** "Install script and README".
- [ ] **Step 5: Hand over to Mike.** Tell him to rescan plugins in REAPER, add Samplosis to a track, load a guitar note, and play a chord with keep length on and off. Ask him how NVDA reads the window.
