# Samplosis SoundFont Player Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A SoundFont tab in Samplosis that plays SF2 and SF3 files completely: every SF2.04 generator and modulator, one-preset and all-channels (General MIDI) modes, 16 outputs, reverb and chorus. Mike asked for "the soundfont player tab with full soundfont support".

**Architecture:**
- **A separate player.** The SoundFont player sits beside the sampler engine, not inside it: `SmpSynth`, with its own voices and 16 MIDI channels. SoundFont samples are short, so they're held in memory and never streamed.
- **Memory follows what's playing.** The font file's headers are parsed at once. A sample's audio is loaded only while a preset that uses it is in use on some channel.
  - A process-wide font registry shares one parsed font and its loaded samples between every instance that opens the same file. One background loader thread serves them all.
- **The plugin.** It holds both the sampler engine and the synth. "plays" (sampler or soundfont) decides which one the notes go to, and the window's tab sets it.
- **Outputs.** The plugin gets 16 stereo outputs.

**Tech Stack:**
- C11 with MSVC cl, as stage 1 (build.bat).
- stb_vorbis (already in src/decode/) for SF3.
- pytest, numpy, soundfile and ctypes.
- tests/host.c.
- **Reference renders:** FluidSynth 2.6.1 at E:\pypy\fluidsynth\fluidsynth-v2.6.1-win10-x64-cpp11\bin\fluidsynth.exe.
  - FluidSynth is only a reference for the tests. None of its code is copied.

**Spec:** E:\samplepacks\claudemusic\samplosis\docs\superpowers\specs\2026-10-28-samplosis-design.md (section "SoundFonts", and "The window", "Memory and speed", "Outputs").

## Global Constraints

Stage 1's Global Constraints still hold (docs\superpowers\plans\2026-10-28-samplosis-stage1.md). They include:
- the audio thread never allocates, locks, waits, or touches the disk;
- the window rules;
- send keys by message, never SendInput;
- keep window test runs few.

In addition:
- **Branch:** `stage2-soundfont`, cut from stage1 at afe3196. Commit after each task, with messages ending in "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>". Never push.
- **Read-only:** never write into E:\samplepacks\soundfonts, E:\pypy or any game or sound-library folder. Tests write only to pytest's tmp_path.
- **The real font:** E:\samplepacks\soundfonts\just_t4_fixed.sf2 is a 526 MB Yamaha Tyros General MIDI set. Tests that use it, or FluidSynth, are skipped when the file or fluidsynth.exe is missing.
- **Defaults:**
  - plays = sampler, for old projects and new instances, until a SoundFont is loaded or the tab is chosen.
  - SoundFont mode = all channels.
  - sf_volume 0 dB, sf_voices 256, sf_effects on, sf_transpose 0.
- **Program numbers.** The window shows them 1 to 128 (as musicians count). The state and the API use 0 to 127.
- **Audio-thread rules for the loader:**
  - Note-ons, program changes and controller changes only read atomics, use Interlocked operations and call SetEvent.
  - Sample memory is freed only by the loader thread, using the protocol in Task 3.
- **Levels and sound** follow the SF2.04 spec. Where the spec is unclear, match FluidSynth (Task 7 measures it).

## Review Focus

1. **A program change just before the first notes** (every General MIDI song starts like this). The notes must still sound. In real time they may be late by at most the load time; an offline render (REAPER's render to file) must be exact. Pinned by `test_pending_note_starts_when_loaded` (Task 5) and `test_plugin_offline_render_waits` (Task 8).
2. **A damaged or truncated SF2** must never crash anything; a bad file gives a plain error. This covers chunk sizes past the end, indices out of range, a sample end past the data, and loop points outside the sample. Pinned by `test_truncated_and_corrupt_files` (Task 1) and `test_bad_loop_points_clamped` (Task 4).
3. **A bank or program the font doesn't have.** This includes GS or XG bank numbers, and drums on channel 10 with an odd kit number. It must fall back to something that sounds, not to silence. Pinned by `test_program_fallback` (Task 5).
4. **Many instances of the same big font** (Mike puts each track in its own instance). They share one copy of the samples, and closing one never frees what another plays. Pinned by `test_instances_share_samples` (Task 3).
5. **Stuck notes.** No notes may hang after any of these:
   - the sustain pedal held when the transport stops;
   - All Notes Off (CC123) or All Sound Off (CC120);
   - Reset All Controllers (CC121);
   - the plugin's reset.

   Pinned by `test_no_stuck_notes` (Task 5).

---

## File structure

- `src/sf2.h`: internal types shared by the SoundFont files: font, preset, region, sample header, modulator and generator ids.
- `src/sf2.c`: parses an SF2 or SF3 file's RIFF chunks and hydra. Lists the presets and flattens a preset into regions.
- `src/sffont.c`: the process-wide font registry, preset use counts, the loader thread and sample decoding (16-bit PCM, SF3 Ogg).
- `src/sfvoice.c`: one SoundFont voice: generators and modulators to sound, envelopes, LFOs, filter, loop and interpolation.
- `src/sfsynth.c`: the player. It holds the 16 channels and their MIDI state, preset choice, voice allocation, the modes, 16 outputs, the state text lines and the test render.
- `src/sffx.c`: reverb and chorus.
- `src/samplosis.h`: the exported API below. `src/notes.c`: four more parameters. `src/script.c`: ignores SoundFont lines.
- `src/plugin.c`, `src/plugin_int.h`: plays mode, 16 audio ports, routing, state, the render extension.
- `src/gui.c`: the tab control and the SoundFont page.
- `tests/sf2make.py`: writes small SF2 and SF3 files for tests.
- `tests/test_sf2.py`, `tests/test_sffont.py`, `tests/test_sfvoice.py`, `tests/test_sfsynth.py`, `tests/test_sf_reference.py`; additions to `tests/test_plugin.py` and `tests/host.c`.

## Shared definitions (src/sf2.h and samplosis.h)

Generator ids are the SF2.04 numbers (0 startAddrsOffset to 58 overridingRootKey; 60 = SMP_SF_NGEN is the count used for arrays). Region generator values are final per spec (instrument value or its default, plus the preset's relative value), stored as `int16_t gen[SMP_SF_NGEN]`.

```c
typedef struct { uint16_t src, dest; int16_t amount; uint16_t amt_src, trans; } SmpSfMod;  /* as in pmod/imod */
typedef struct {
  char name[21]; uint32_t start, end, loop_start, loop_end;   /* SF2: sample frames into smpl; SF3: see Task 3 */
  uint32_t rate; uint8_t pitch; int8_t correction; uint16_t link, type;
} SmpSfSample;
typedef struct {
  uint8_t lokey, hikey, lovel, hivel; int sample;            /* index into the font's samples */
  int16_t gen[SMP_SF_NGEN];
  const SmpSfMod *imod; int nimod;   /* instrument level, the 10 default modulators merged in (identical ones replaced) */
  const SmpSfMod *pmod; int npmod;   /* preset level, added on top */
} SmpSfRegion;
```

Exported (samplosis.h), all main thread unless noted:

```c
typedef struct SmpFont SmpFont;            /* shared, refcounted (Task 3) */
SMP_API SmpFont *smp_sf_open(const char *path, char *err, int cap);  /* headers only; NULL and a plain reason */
SMP_API void smp_sf_close(SmpFont *f);
SMP_API int smp_sf_preset_count(const SmpFont *f);
SMP_API int smp_sf_preset_info(const SmpFont *f, int i, int *bank, int *program, char *name, int cap); /* sorted by bank, then program */
SMP_API int smp_sf_find_preset(const SmpFont *f, int bank, int program);   /* exact; -1 if none */
SMP_API int smp_sf_regions(const SmpFont *f, int preset, const SmpSfRegion **out);  /* count; any thread */
SMP_API int smp_sf_sample_count(const SmpFont *f);
SMP_API const SmpSfSample *smp_sf_sample(const SmpFont *f, int i);

typedef struct SmpSynth SmpSynth;
SMP_API SmpSynth *smp_synth_new(double rate, int max_block);
SMP_API void smp_synth_free(SmpSynth *s);
SMP_API int smp_synth_load(SmpSynth *s, const char *path, char *err, int cap); /* 1 ok */
SMP_API void smp_synth_midi(SmpSynth *s, const uint8_t *msg, int len, int at);  /* audio thread, queued like note_on */
SMP_API void smp_synth_process(SmpSynth *s, float *const *outs /* 32: L,R per output */, int n);  /* audio thread */
SMP_API void smp_synth_reset(SmpSynth *s);       /* audio thread: every voice off, controllers reset */
SMP_API void smp_synth_set_param(SmpSynth *s, int id, double v);   /* ids SMP_P_SF_*, any thread */
SMP_API double smp_synth_get_param(const SmpSynth *s, int id);
SMP_API int smp_synth_script_write(const SmpSynth *s, char *out, int cap);     /* like snprintf */
SMP_API int smp_synth_script_apply(SmpSynth *s, const char *text, char *err, int cap);  /* 0 ok; nothing changes on error */
SMP_API int smp_synth_render_test(SmpSynth *s, const char *events, long frames, int block, const char *wav_out, int all_outputs);
SMP_API int smp_synth_wait_loaded(SmpSynth *s, int timeout_ms);   /* tests: 0 once every preset in use is loaded */
```

Parameters (notes.c table grows; `SMP_NPARAMS` stays 31 for the sampler, `SMP_NALLPARAMS` = 35 is the table size and the CLAP parameter count):

- 31 `sf_volume`, "SoundFont volume": -60 to 12 dB, default 0, SMP_K_DB.
- 32 `sf_voices`, "SoundFont voices": 16 to 1024, default 256, SMP_K_COUNT.
- 33 `sf_effects`, "Reverb and chorus": off/on, default on, SMP_K_BOOL.
- 34 `sf_transpose`, "SoundFont transpose": -24 to 24 st, default 0, SMP_K_SEMI.

State lines (written by smp_synth_script_write after the sampler's lines; the plugin writes `plays` first):

```
plays soundfont                      sampler (the default) or soundfont
soundfont "D:\fonts\GM.sf2"
sf_mode all                          one or all
sf_preset 0 0                        one mode: bank program
sf_channel all                       one mode: all, or 1 to 16
sf_channel_preset 3 0 25             all mode: channel 3 always plays bank 0 program 25 (absent: from the song)
sf_output 3 2                        all mode: channel 3 to output 2 (absent: output 1, the main one)
sf_used 0:0 0:25 128:0               presets in use when saved: loaded as soon as the state loads
sf_volume 0.0 dB                     then the four parameters, as `name value-in-words`
```

---

### Task 1: SF2 parsing, preset list, and the test font writer

**Files:**
- Create: `src/sf2.h`, `src/sf2.c`, `tests/sf2make.py`, `tests/test_sf2.py`
- Modify: `src/samplosis.h`, `tests/conftest.py` (ctypes signatures)

**Interfaces:**
- Produces: `smp_sf_open/close/preset_count/preset_info/find_preset/sample_count/sample` (above). Internally, `SmpFont` keeps the path, file size and mtime, the sdta `smpl` offset and size (and `sm24`), the hydra arrays (phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr), and an `is_sf3` flag (any sample with type bit 0x10). In this task, open/close are a plain malloc and free; Task 3 turns them into a registry.
- Produces: `tests/sf2make.py` with `write_sf2(path, presets, samples, sf3=False)`.
  - `samples` is a list of dicts: name, data (int16 numpy array), rate, pitch, correction, loop (start, end) or None.
  - `presets` is a list of dicts: name, bank, program, gens (preset-level dict {gen id: value}), pmods, and instruments. Each instrument has a name, global gens and mods, and zones. Each zone has its key and velocity ranges, a sample index, gens and mods.
  - With sf3=True, sample data is written as Ogg Vorbis (via soundfile's OGG support), the type gets bit 0x10, and start and end are byte offsets.

- [ ] **Step 1: Write failing tests** in tests/test_sf2.py:
  - `test_open_lists_presets`: a font with presets (bank 0 program 0 "Piano"), (0, 25 "Guitar") and (128, 0 "Standard") lists them sorted (0:0, 0:25, 128:0). Names come back trimmed of NULs. `smp_sf_find_preset(f, 0, 25) == 1` and `smp_sf_find_preset(f, 0, 99) == -1`.
  - `test_samples_listed`: sample headers come back with name, start, end, loop, rate, pitch and correction as written.
  - `test_not_a_soundfont`: a wav file, an empty file and a missing file each give NULL. The reason contains "isn't a SoundFont", "isn't a SoundFont" and "can't open" respectively.
  - `test_truncated_and_corrupt_files`: a valid fixture truncated at every 97th byte, plus 300 copies each with 8 random bytes changed (seeded). smp_sf_open returns NULL or a font. For every font returned, every preset's regions are walked (after Task 2) and every sample header is read. The test process must not crash, and every index the API returns must be in range.
  - `test_open_real_font_fast` (skipped without the Tyros file): the font opens in under 0.5 s with over 100 presets, and the process's private bytes grow by under 20 MB.
- [ ] **Step 2: Run** `python -m pytest tests/test_sf2.py -q`: it fails (no smp_sf_open).
- [ ] **Step 3: Implement sf2.c.**
  - Read only the RIFF structure and the pdta chunk; record smpl and sm24 offsets without reading sample data.
  - Validate every count against chunk sizes, and every index (bag ranges, instrument and sample ids) against the arrays. A bad index makes the file fail with "<name> isn't a SoundFont this can read (<what was wrong>)" rather than being skipped silently.
  - Drop the terminal "EOP", "EOI" and "EOS" records from the counts.
  - Build the font's sorted preset index once.
- [ ] **Step 4: Run** `python -m pytest tests/test_sf2.py -q` (Task 2 completes the region walk in the corrupt-file test; until then that test walks presets and samples only). Expected: PASS.
- [ ] **Step 5: Commit.**

### Task 2: Flattening a preset into regions (generators and modulators per SF2.04)

**Files:**
- Modify: `src/sf2.c`, `tests/test_sf2.py`, `tests/sf2make.py` if needed

**Interfaces:**
- Consumes: Task 1's hydra.
- Produces: `smp_sf_regions(f, preset, &regions)`. Regions are built for all presets at open time, in one allocation. The pointer stays valid until the font is closed, so the audio thread reads them without locks.

Rules to implement (SF2.04 sections 8 and 9):
- A preset zone × instrument zone pair is a region when their key and velocity ranges intersect. The region's ranges are the intersection.
- Global zones (a first zone with no instrument or sample generator) give defaults for the zones that follow at the same level. A local generator replaces the global one.
- Instrument generators are absolute: the value, else the global one, else the spec default. The spec defaults that aren't zero are:
  - initialFilterFc 13500;
  - the six envelope times (delay, attack, hold, decay, release, for both envelopes) -12000;
  - delayModLFO and delayVibLFO -12000;
  - keynum and velocity -1;
  - scaleTuning 100;
  - overridingRootKey -1.
- Preset generators are added to the instrument value. Ranges intersect instead. The sample-only generators (offsets, keynum, velocity, sampleModes, exclusiveClass, overridingRootKey) are ignored at preset level.
- The sum is clamped to the spec's range for that generator.
- **Modulators:**
  - The 10 default modulators (SF2.04 section 8.4) start the instrument list.
  - An instrument modulator identical in its source, destination, amount source and transform to an earlier one replaces it; a local one replaces a global one the same way.
  - Preset modulators make a second list, with the same replacement rules between preset global and local zones.
  - SF2.04's "velocity to filter cutoff" default (-2400 cents) is used, as in the 2.04 spec.

- [ ] **Step 1: Write failing tests** `test_regions_*` in tests/test_sf2.py:
  - `test_regions_ranges_intersect`: a preset zone with keys 40 to 80 over instrument zones 0–59 and 60–127 gives regions 40–59 and 60–80.
  - `test_regions_global_zones`: an instrument global with attackVolEnv -1200 and a zone without one gives -1200. A zone that sets -600 gives -600.
  - `test_regions_preset_adds`: instrument coarseTune 2 plus preset coarseTune 3 gives 5. Preset sampleModes is ignored. Preset initialAttenuation 100 plus instrument 200 gives 300.
  - `test_regions_defaults`: an empty zone has initialFilterFc 13500, scaleTuning 100, attackVolEnv -12000, overridingRootKey -1 and keynum -1.
  - `test_regions_clamped`: instrument initialAttenuation 1400 plus preset 400 gives 1440 (the maximum).
  - `test_modulators_merged`: a region with no modulators lists the 10 defaults. An instrument modulator identical to "CC7 to initialAttenuation" but with amount 480 replaces it, still 10 in total. A new one makes 11. Preset modulators are listed apart.
- [ ] **Step 2: Run them**: they fail.
- [ ] **Step 3: Implement it in sf2.c.**
- [ ] **Step 4: Run** `python -m pytest tests/test_sf2.py -q`. Expected: PASS, including the region walk in `test_truncated_and_corrupt_files`.
- [ ] **Step 5: Commit.**

### Task 3: The font registry, preset use, the loader thread and sample data

**Files:**
- Create: `src/sffont.c`, `tests/test_sffont.py`
- Modify: `src/sf2.c` (open and close go through the registry), `src/sf2.h`, `src/samplosis.h`

**Interfaces:**
- Consumes: Tasks 1–2.
- **The registry.** `smp_sf_open` now returns the shared font when the same file (the same full path, compared case-insensitively, the same size and the same mtime) is already open, and adds a reference. `smp_sf_close` drops it, and the last close frees everything. Only the main thread opens and closes.
- **Preset use, safe from any thread including audio:**
  - `void smp_sf_use(SmpFont *f, int preset, int delta)`: Interlocked on `f->users[preset]`, then SetEvent on the loader.
  - `const int16_t *smp_sf_pcm_acquire(SmpFont *f, int sample, uint32_t *frames)`: InterlockedIncrement on `f->voices[sample]`, then reads `f->pcm[sample]`. When that is NULL it decrements again and returns NULL.
  - `void smp_sf_pcm_release(SmpFont *f, int sample)`.
- **Tests:** `SMP_API int smp_sf_wait_idle(SmpFont *f, int timeout_ms)` returns 0 once the loader has nothing left to do for f. `SMP_API int smp_sf_loaded_samples(const SmpFont *f)` and `SMP_API uint64_t smp_sf_loaded_bytes(const SmpFont *f)`.
- **The loader** is one process-wide thread, started with the first font, below-normal priority. Each pass, for each font:
  - Load every sample that a preset with users > 0 needs and that isn't loaded. Sample data is int16 mono (or one side of a stereo pair: a linked sample is its own sample). Read with ReadFile at its offset into smpl. sm24 is ignored (its lowest 8 bits are inaudible here).
  - SF3: decode the sample's Ogg bytes (start and end are byte offsets into smpl) with stb_vorbis to int16. Loop points are then counted from the decoded start, as in the SF3 format used by MuseScore and Polyphone.
  - Publish the sample with InterlockedExchangePointer.
  - A loaded sample that no preset in use needs, and that has stayed unneeded for 2 s, is unpublished (exchanged to NULL) into a retire list. It is freed when `voices[sample]` is 0.
  - Before freeing, check that the sample is still unneeded; if it's needed again, publish it again. This stops quick program changes from thrashing.
- A sample whose start, end or loop is outside smpl is clamped at load: end to the data, loop inside start..end. A sample that would be empty is left NULL and counted as broken, so it is never loaded again.

- [ ] **Step 1: Write failing tests** in tests/test_sffont.py:
  - `test_samples_load_only_when_used`: right after open, smp_sf_loaded_samples == 0. use(preset 1, +1), then wait_idle, loads exactly that preset's samples. Their PCM equals what sf2make wrote. use(1, -1) frees them after the 2 s grace period: poll for up to 4 s.
  - `test_instances_share_samples`: two smp_sf_open of the same path return the same pointer. The second's use keeps the samples. Closing the first leaves the second's samples readable and loaded. Two synths on the same font with the same preset hold one copy (loaded_bytes doesn't double).
  - `test_sf3_matches_sf2`: the same samples written as SF2 and SF3 decode within -30 dB RMS of each other, with the same frame counts to within one Ogg packet. Loops are honoured: the loop points read back equal.
  - `test_acquire_after_unpublish`: a thread hammering acquire and release on one sample while the main thread toggles use for 2 s, under a 10 ms grace (`smp_sf_test_grace(10)`, a test export). There must be no crash, and no read of freed memory (the hammering thread checks a known value in the data every time it gets a pointer).
  - `test_bad_sample_bounds_clamped`: a header with an end past smpl, or a loop outside the sample, loads clamped and does not crash. A zero-length sample stays NULL.
  - `test_real_font_preset_memory` (skipped without the file): using program 0 (piano) loads under 60 MB, in under 2 s.
- [ ] **Step 2: Run them**: they fail.
- [ ] **Step 3: Implement sffont.c.**
- [ ] **Step 4: Run** `python -m pytest tests/test_sf2.py tests/test_sffont.py -q`. Expected: PASS.
- [ ] **Step 5: Commit.**

### Task 4: One SoundFont voice

**Files:**
- Create: `src/sfvoice.c`, `src/sfvoice.h`, `tests/test_sfvoice.py`

**Interfaces:**
- Consumes: SmpSfRegion and the acquire and release calls from Task 3.
- **Produces, in sfvoice.h:**
  - `typedef struct SmpSfChan { uint8_t cc[128]; int bend /* 0..16383 */, bend_range_cents, pressure, poly[128]; int fine_cents, coarse_semis; } SmpSfChan;`. RPN fine and coarse tuning also come into the pitch.
  - `typedef struct SmpSfVoice SmpSfVoice;`, and the following functions:
    - `int smp_sfv_start(SmpSfVoice *v, SmpFont *f, const SmpSfRegion *r, int key, int vel, const SmpSfChan *c, double rate, double transpose)`, which returns 0 when the sample isn't loaded;
    - `void smp_sfv_release(SmpSfVoice *v)`;
    - `void smp_sfv_kill(SmpSfVoice *v, double fade_ms)`;
    - `void smp_sfv_update(SmpSfVoice *v, const SmpSfChan *c)`, which works out the modulators again when a controller changed;
    - `int smp_sfv_render(SmpSfVoice *v, float *l, float *r, float *rev, float *cho, int n)`, which adds into the buffers and returns 0 once the voice has ended;
    - `double smp_sfv_level(const SmpSfVoice *v)`, the voice's envelope level, for stealing.
- **Test export:** `SMP_API int smp_sf_test_note(SmpFont *f, int preset, int key, int vel, double rate, long frames, long release_at, float *out_lr /* frames*2 */)`. It renders every region of the preset that the key and velocity match, with a default channel. The default channel is all controllers at their GM reset values: CC7 100, CC10 64, CC11 127, bend centre, range 200 cents.

What the voice must do (SF2.04 section 9):
- **Pitch.** The key comes from keynum when set, else the key played. The root comes from overridingRootKey when set, else the sample's pitch. Then:
  - pitch in cents = (key − root) × scaleTuning + coarseTune × 100 + fineTune + correction + channel bend + RPN tuning + transpose × 100;
  - plus the modulation envelope × modEnvToPitch, modLfoToPitch and vibLfoToPitch;
  - the playback ratio also includes sample rate / output rate.
- **Interpolation.** 4-point Hermite on int16, as the sampler's fetch does on floats.
- **Address offsets.** The four start, end and loop offsets, plus their coarse versions × 32768.
- **sampleModes:**
  - 0: no loop;
  - 1: loop continuously;
  - 3: loop until release, then play on to the end;
  - 2: no loop.
- **Volume envelope.** Delay, attack (linear in amplitude), hold, decay and release, in timecents. Sustain is in centibels of attenuation. Decay and release are linear in dB; release falls to -100 dB.
  - keynumToVolEnvHold and keynumToVolEnvDecay scale hold and decay by (60 − key) × value timecents.
  - The voice ends when the release reaches -100 dB, or at the sample's end without a loop.
- **Modulation envelope.** The same shape on a 0..1 scale, to pitch and filter cutoff.
- **Mod and vibrato LFOs.** Triangles, with delay and frequency in absolute cents (8.176 Hz × 2^(cents/1200)). The mod LFO goes to pitch, filter cutoff and volume; the vibrato LFO to pitch.
- **Filter.** A 2-pole resonant lowpass (an RBJ biquad): cutoff = 8.176 × 2^(cents/1200) Hz, clamped to 20 Hz … 0.45 × rate; Q from initialFilterQ in centibels above 0 dB.
  - Bypassed when the cutoff is ≥ 13500 cents and Q is 0, as FluidSynth does.
  - Coefficients are worked out again at the control rate and smoothed.
- **Gain.**
  - initialAttenuation in centibels × 0.4, the EMU factor that FluidSynth uses (Task 7 checks this), plus the modulators' attenuation.
  - Pan is constant power from the pan generator (-500..500).
  - Effect sends: reverbEffectsSend and chorusEffectsSend, in 0.1 % units, go to rev and cho.
- **Modulators.** Evaluated per SF2.04 section 8.2: the source with its type (linear, concave, convex, switch), polarity and direction; times the amount source; then the transform (linear, absolute).
  - General controller sources: no controller, note-on velocity, note-on key, poly pressure, channel pressure, pitch wheel, pitch wheel sensitivity. Plus any MIDI CC (the CC flag bit) except the ones the spec excludes.
  - Results are added to the destination generators. Instrument and preset lists are both summed.
- **Control rate.** Envelopes, LFOs and modulators are worked out every 32 samples. Gain and pitch are interpolated in between.
- **Silence until loaded.** A voice whose sample becomes available after start is not this task's job: start returns 0 (Task 5 handles waiting).

- [ ] **Step 1: Write failing tests** in tests/test_sfvoice.py. Each one builds a font with sf2make and renders with smp_sf_test_note at 48 kHz from 44.1 kHz samples.
  - `test_pitch_root_and_tuning`: a 440 Hz sine sample with pitch 69, played at 81, measures 880 Hz within 2 cents. Also:
    - overridingRootKey 57 at key 69 gives 880;
    - coarseTune -12 gives 440;
    - fineTune 50 gives 452.9;
    - scaleTuning 0 at any key gives 440;
    - correction -10 gives -10 cents.
  - `test_loop_modes`: a sample with a loop held for 2 s keeps sounding with mode 1, and is silent after the sample's length with mode 0. Mode 3 released at 1.0 s plays the part after the loop and then stops, before the release ends.
  - `test_volume_envelope`: attack 0.1 s (timecents 1200 × log2(0.1) ≈ -3986) reaches its top at 0.1 s ± 5 ms. Sustain 200 cB sits at -20 dB ± 0.5. A release of 0.5 s goes from sustain down to -100 dB at 0.5 s ± 10 ms. Hold 0.2 s and delay 0.1 s each shift the timing by their length.
  - `test_attenuation_and_velocity`: initialAttenuation 100 cB is 4 dB quieter than 0 (the × 0.4 factor). Velocity 64 against 127, through the default concave velocity modulator, matches 20·log10(…) of the spec's concave curve within 0.5 dB.
  - `test_filter`: initialFilterFc 6000 cents (about 262 Hz) on white noise puts the -3 dB point within 10 % of 262 Hz. Q 100 cB peaks about 10 dB above the passband at the cutoff, ± 2 dB.
  - `test_lfo_vibrato`: vibLfoToPitch 50 at freqVibLFO 0 cents (8.176 Hz) swings the pitch ±50 cents at 8.2 Hz ± 0.3. delayVibLFO 0.5 s keeps the pitch still before then.
  - `test_mod_envelope_to_pitch`: modEnvToPitch 1200 with attack 0, decay 0.5 s and sustain 1000 (0) starts an octave up and falls back to the root by 0.5 s.
  - `test_pan`: pan -500 is hard left (right below -60 dB); pan 0 is equal, at -3 dB each.
  - `test_bad_loop_points_clamped`: loop end past the sample, or loop start after loop end, still renders, with no crash and no NaN.
  - `test_host_rates`: 44.1, 48 and 96 kHz output rates all keep the pitch within 2 cents.
- [ ] **Step 2: Run them**: they fail.
- [ ] **Step 3: Implement sfvoice.c.**
- [ ] **Step 4: Run** `python -m pytest tests/test_sfvoice.py -q`. Expected: PASS.
- [ ] **Step 5: Commit.**

### Task 5: The player: channels, MIDI, presets, voices, modes and outputs

**Files:**
- Create: `src/sfsynth.c`, `tests/test_sfsynth.py`
- Modify: `src/samplosis.h`, `src/notes.c` (parameters 31–34, `SMP_NALLPARAMS`), `src/script.c` (ignores the lines `plays`, `soundfont` and any starting with `sf_`), `tests/test_notes.py` and `tests/test_script.py` (counts)

**Interfaces:**
- Consumes: Tasks 3–4.
- Produces: everything `smp_synth_*` in Shared definitions except the effects (Task 6 fills rev and cho), and `smp_synth_*` state lines except `plays` (the plugin's).

**Behaviour:**
- **The event queue.** `smp_synth_midi` is queued like the sampler's `push`, and handled at its frame in process.
  - Messages: note on and off; CC; program change; channel pressure; poly pressure; pitch bend.
  - GM, GS and XG reset SysEx: `F0 7E 7F 09 01 F7`, `F0 41 .. 42 12 40 00 7F 00 41 F7` and `F0 43 10 4C 00 00 7E 00 F7`. Each resets all channels: programs 0, banks 0, channel 10 drums, controllers reset.
- **Modes:**
  - One preset: the chosen preset (`sf_preset`) plays on every channel the instance listens to (`sf_channel` all, or one). Program changes are ignored, and channel 10 has no drum rule.
  - All channels: each channel has a bank and program from bank select and program change, unless `sf_channel_preset` pins it.
  - Bank select: CC0 is the bank. CC32 is used only when CC0 is 0 and the font has that bank (XG style). Channel 10 uses bank 128 unless a bank select of 127 or 128 came in, in which case it uses that.
- **Program fallback** (`test_program_fallback`):
  1. the exact bank and program;
  2. bank 0 with the same program (for drums: bank 128, program 0);
  3. the first preset in the font.
  - The status reports nothing for this; it's silent and normal.
- **Preset use.** When a channel's preset changes, the old one is released and the new one used (smp_sf_use, on the audio thread). Mode or preset changes from the main thread go through an atomic hand-off read at the start of process, like the sampler's parameters.
  - The state's `sf_used` presets are used at load and released once the song's own program changes have settled: 10 s after the state loaded, or when the plugin is reset.
- **Notes.** A note-on finds every region of the channel's preset that matches the key and velocity, and starts a voice for each.
  - **A sample not loaded yet:** the voice is made "pending" with its note, and starts on the first block its sample is there (late, from the beginning). A note-off before then drops it. Pending notes older than 1 s are dropped.
  - **Exclusive class:** a new note with exclusiveClass ≠ 0 kills (5 ms fade) the voices on that channel with the same class.
  - **The same key again on a channel:** the earlier voices of that key are released (as FluidSynth does).
- **Controllers:**
  - CC1, 7, 10, 11, 91 and 93 and everything else reach the modulators (smp_sfv_update on the channel's voices).
  - CC64 sustain holds note-offs. CC66 sostenuto holds the notes down at the moment it's pressed.
  - CC120 All Sound Off: kill with a 5 ms fade. CC123 All Notes Off: release them, respecting sustain as the MIDI spec says. CC121 Reset All Controllers, as in GM's recommended practice.
  - RPN 0 (bend range), RPN 1 (fine tuning) and RPN 2 (coarse tuning) through CC101, 100, 6 and 38. NRPNs are ignored.
- **Voices.** Up to `sf_voices` sound at once.
  - Stealing picks, in order: released voices first, then the quietest (`smp_sfv_level`), then the oldest.
  - A stolen voice fades out over 5 ms.
  - The voice array is 1024 entries, allocated once.
- **Outputs.** `outs` has 32 buffers (16 stereo outputs). In all-channels mode, a channel goes to its `sf_output` (1–16). In one-preset mode everything goes to output 1.
  - Then sf_volume and sf_transpose apply.
  - Effect sends only from channels on output 1 (separated channels stay dry for REAPER). Task 6 adds the effects; this task leaves the send buffers summed and unused.
- **The test render.** `smp_synth_render_test` takes events one per line, `midi <status> <d1> [<d2>] <frame>` (decimal), like smp_engine_render_test's format.
  - Before each block it waits for pending loads, so offline renders are exact.
  - It writes output 1, or all 32 channels when all_outputs is 1.

- [ ] **Step 1: Write failing tests** in tests/test_sfsynth.py. The fixture font has: piano at (0, 0); bass at (0, 33); a drum kit at (128, 0), with two hi-hats sharing exclusive class 1; and a looping organ at (0, 16).
  - `test_program_change_and_bank`: program 33 on channel 2 plays the bass. Bank select 128 then program 0 on channel 1 plays drums. Channel 10 plays drums by default.
  - `test_program_fallback`: bank 8 program 33 plays the bass (bank 0 fallback); channel 10 kit 99 plays the kit at 128:0; bank 50 program 120 (absent) plays the first preset.
  - `test_pending_note_starts_when_loaded`: in real time (with the test render's waiting off, through `smp_synth_test_offline(s, 0)`), a program change and a note on the same frame sound within 300 ms, never silently lost. With waiting on, the note starts on its exact frame.
  - `test_one_preset_mode`: with `sf_mode one`, `sf_preset 0 33` and `sf_channel 3`, a note on channel 3 plays the bass, a note on channel 1 is silent, and a program change on channel 3 changes nothing.
  - `test_exclusive_class`: open hi-hat then closed hi-hat: the open one is gone within 10 ms of the closed one starting.
  - `test_sustain_and_sostenuto`: notes released under CC64 keep sounding until the pedal is up. Sostenuto holds only the notes down when it was pressed.
  - `test_no_stuck_notes`: an organ (looping) held with the sustain pedal down. Each of CC123 (then pedal up), CC120, reset (`smp_synth_reset`) and a GM reset SysEx ends all sound within 0.1 s of the event, or of the pedal-up for CC123.
  - `test_controllers`: CC7 64 is 20·log10((64/127)²) ≈ -5.96 dB ± 0.3 (the concave default CC7 modulator per the spec). CC10 0 is hard left. CC1 127 adds vibrato (pitch moves at least 20 cents). RPN 0 set to 12 semitones makes bend 16383 go up 12 semitones.
  - `test_voice_limit_and_stealing`: sf_voices 16 with 40 notes held never sounds more than 16 voices. A voice in its release goes before a held one.
  - `test_outputs`: in all mode with `sf_output 2 3`, channel 2's notes come out on output 3 only (outputs 1 and 2 are silent), with all_outputs = 1.
  - `test_state_lines_roundtrip`: write and apply the state again; the result is identical. A bad line gives "line N: …" and changes nothing. A missing font file gives an error that names the path.
- [ ] **Step 2: Run them**: they fail.
- [ ] **Step 3: Implement sfsynth.c**, plus the notes.c, script.c and test count changes.
- [ ] **Step 4: Run** `python -m pytest tests -q -k "sf or notes or script"`. Expected: PASS.
- [ ] **Step 5: Commit.**

### Task 6: Reverb and chorus

**Files:**
- Create: `src/sffx.c`, `src/sffx.h`; Modify: `src/sfsynth.c`; Test: `tests/test_sfsynth.py`

**Interfaces:**
- Produces: `SmpSfFx *smp_sffx_new(double rate)`, `void smp_sffx_free(SmpSfFx *)`, and `void smp_sffx_process(SmpSfFx *, const float *rev_in, const float *cho_in, float *l, float *r, int n)`. It adds its output into l and r, and allocates nothing after new.
- **The design (our call; the sound should be close to FluidSynth's defaults):**
  - **Reverb:** a stereo Freeverb-style design with 8 comb filters and 4 allpasses per side. Room 0.2, damp 0.0, width 0.5, level 0.9.
  - **Chorus:** 3 voices, 2 ms depth, 0.3 Hz, level 2.0, delay about 8 ms, sine modulation.
  - Both off when `sf_effects` is off. Their tails then end at once.

- [ ] **Step 1: Write failing tests:**
  - `test_reverb_send`: a short click with CC91 127 has a tail above -60 dB for at least 0.5 s. With CC91 0 it has none. With sf_effects off it has none.
  - `test_chorus_send`: a sine note with CC93 127 shows more than one spectral peak within ±10 cents of its pitch, or widens the stereo image (L−R energy above -30 dB relative). With CC93 0 the image is mono.
  - `test_fx_cpu`: one second of effects at 48 kHz renders in under 5 ms.
- [ ] **Step 2: Run them**: they fail. **Step 3: Implement.** **Step 4: Run** `python -m pytest tests/test_sfsynth.py -q`: PASS. **Step 5: Commit.**

### Task 7: Sounding like the author meant: comparison with FluidSynth

**Files:**
- Create: `tests/test_sf_reference.py`
- Modify: whatever src/sf*.c the comparison shows to be wrong

**Interfaces:**
- Consumes: Tasks 1–6 through `smp_synth_render_test`.
- It writes small MIDI files with `mido`. If mido is missing, install it into the same Python with pip; it's a test-only dependency.

The test renders the same MIDI through both players on just_t4_fixed.sf2 (skipped without the file or fluidsynth.exe). FluidSynth runs as `fluidsynth.exe -ni -q -R 0 -C 0 -g 1.0 -r 48000 -F out.wav <sf2> <mid>` (reverb and chorus off; ours with sf_effects off). Each note is analysed alone, with 1.5 s gaps between notes.

- [ ] **Step 1: Write the tests:**
  - `test_reference_presets`: one note each, C4 velocity 100, held 1 s, on programs 0 (piano), 25 (steel guitar), 33 (finger bass), 40 (violin), 56 (trumpet), 73 (flute), 80 (square lead), 88 (pad), and drum keys 36, 38 and 42 on channel 10. For each:
    - the RMS level over the note is within 2 dB of FluidSynth's;
    - the pitch is within 5 cents (the sampler's YIN, `smp_detect_pitch`);
    - the time for the release tail to fall to -60 dB is within 20 %;
    - the spectral centroid over the first 0.5 s is within 15 %.
  - `test_reference_velocity_and_cc`: piano at velocities 30, 64 and 127, and with CC7 50 and CC11 80, each stays within 1 dB of FluidSynth's level difference.
  - `test_reference_song`: a 20-second GM file made by the test (8 channels with program changes, drums, bend, sustain and CC7 automation) renders through both. The loudness of each 1 s window is within 3 dB, for 90 % of the windows.
- [ ] **Step 2: Run** `python -m pytest tests/test_sf_reference.py -q`. Find and fix each difference in sfvoice.c or sfsynth.c until it passes.
  - Fix the cause (a wrong curve, a wrong default, a missing factor), never a tolerance.
  - Record each cause and fix in the task report.
- [ ] **Step 3: Commit.**

### Task 8: The plugin: plays mode, 16 outputs, state and offline rendering

**Files:**
- Modify: `src/plugin.c`, `src/plugin_int.h`, `tests/host.c`, `tests/test_plugin.py`

**Interfaces:**
- Consumes: smp_synth_* and the parameters 31–34.
- **Produces for the window (plugin_int.h):**
  - `int smp_plug_plays(Plug *P)`, which returns 0 for the sampler and 1 for the SoundFont player, and `void smp_plug_set_plays(Plug *P, int which)`. Changing it marks the project dirty and silences the other player's voices (reset).
  - `int smp_plug_load_font(Plug *P, const char *path, char *err, int cap)`, which also sets plays to the SoundFont player.
  - `SmpSynth *smp_plug_synth(Plug *P)`.
  - `void smp_plug_font_text(Plug *P, char *out, int cap)`, giving "GM.sf2, 412 presets", "No SoundFont loaded" or "Missing: <path>".
  - `void smp_plug_sf_set(Plug *P, const char *line)`, which applies one state line such as `sf_mode one`, `sf_preset 0 25`, `sf_channel_preset 3 0 25`, `sf_channel_preset 3 song` or `sf_output 3 2`, through smp_synth_script_apply, then marks the project dirty.
  - `int smp_plug_sf_channel_now(Plug *P, int ch, int *bank, int *program)`, the preset a channel plays at the moment (all mode).
  - smp_plug_set_param and smp_plug_get_param now take ids up to `SMP_NALLPARAMS`; ids 31–34 go to the synth.
- **Behaviour:**
  - **Audio ports:** 16 stereo outputs, "Output 1" (main) to "Output 16". The sampler writes output 1 and the rest are zero.
  - **Note port:** the preferred dialect is MIDI, still supporting CLAP notes. CLAP note events go to the synth as MIDI on their channel.
  - Events go to whichever player plays. Both players keep their state.
  - **State:**
    - the line `plays sampler|soundfont` first;
    - the sampler's text;
    - the synth's lines.
    - Old states (no `plays`) load as the sampler.
    - A SoundFont that's missing keeps its lines and gives the status "Missing: <path>", like a missing sample.
  - **CLAP render extension** (`clap.render`):
    - In offline mode the synth waits for pending loads at each block (the test render's waiting).
    - A has_hard_realtime_requirement of false.
    - set(CLAP_RENDER_OFFLINE) turns waiting on; realtime turns it off.
  - **Status:**
    - "Ready: GM.sf2, 412 presets" when nothing is loading;
    - "Loading samples for 3 presets" while it is.
- **host.c:**
  - `play` takes an optional argument after the rate, `all`, which writes every output port into the wav (2 channels per port, port order). `render offline` is a new event, sent before the first block, that calls the render extension.

- [ ] **Step 1: Write failing tests** in tests/test_plugin.py:
  - `test_plugin_plays_soundfont`: a state with `plays soundfont` and a fixture font plays the bass for midi program 33 and a note on channel 2. It matches `smp_synth_render_test` on the same events within -90 dB.
  - `test_plugin_old_state_is_sampler`: a stage 1 state with no plays line plays the sample as before.
  - `test_plugin_outputs`: `sf_output 2 3` with `all` puts channel 2 on channels 4 and 5 of the wav only.
  - `test_plugin_offline_render_waits`: with `render offline`, a program change and a note at frame 0 sound from frame 0.
  - `test_plugin_font_missing`: a state naming a missing font gives the status "Missing: <path>" and saves its lines back unchanged.
  - `test_plugin_params`: parameters 31–34 are listed with their names and ranges, and set through the host. sf_volume -6 makes the output 6 dB quieter, ± 0.1.
- [ ] **Step 2: Run them**: they fail. **Step 3: Implement.**
- [ ] **Step 4: Run** `python -m pytest tests -q` (the whole suite: the sampler mustn't change). Expected: PASS.
- [ ] **Step 5: Commit.**

### Task 9: The window: the Sampler and SoundFont tabs

**Files:**
- Modify: `src/gui.c`, `tests/host.c` (if a key or check is missing), `tests/test_plugin.py`

**Interfaces:**
- Consumes: Task 8's smp_plug_* calls; gui.c's sliders and combo patterns.

**The window:**
- **The tab control.** The first control on both main pages is a tab control (WC_TABCONTROL), named "Player", with the tabs "Sampler" and "SoundFont".
  - Left and Right (the tab control's own keys) change the tab. The tab chosen is what the instance plays (smp_plug_set_plays), and its page shows.
  - Ctrl+Tab and Ctrl+Shift+Tab also change the tab from any control on either main page.
  - The window opens on the tab that plays.
  - The tab control must be named by MSAA (an annotation, like the sliders), and read its selected tab.
- **The SoundFont page, in Tab order:**
  1. the tab control;
  2. SoundFont (a read-only edit: smp_plug_font_text);
  3. Browse (for .sf2 and .sf3), Previous file and Next file (the .sf2 and .sf3 files in its folder, as the sampler's buttons do);
  4. Mode (a combo: "All channels" or "One preset").
  5. **One preset:**
     - Preset, a combo of every preset, written as "<name>, bank <b> program <p+1>";
     - Channel, a combo: "All channels", then "Channel 1" to "Channel 16".
  6. **All channels:** for each channel 1 to 16:
     - "Channel N", a combo. Its first entry is "From the song (now <name>)", updated by the 250 ms timer when it changes, but not while that combo's list is open. Then every preset.
     - "Channel N output", a combo of "Output 1" to "Output 16".
  7. SoundFont volume, SoundFont voices and SoundFont transpose (sliders), and Reverb and chorus (a combo).
  8. Status; Settings.
- The controls of the mode not chosen are hidden, and Tab skips them (tab_key skips hidden windows).
- The page's arrays (gui.c's all[] and order[]) grow to hold three pages and 64 Tab stops a page.
- **Drag and drop:** a dropped .sf2 or .sf3 loads as the SoundFont and switches to its tab. A dropped audio file loads into the sampler and switches to the Sampler tab.
- **Wording:**
  - "SoundFont" (the edit's label);
  - "Browse", "Previous file", "Next file";
  - "Mode", "Preset", "Channel";
  - "Channel 1" to "Channel 16", "Channel 1 output" to "Channel 16 output";
  - "SoundFont volume", "SoundFont voices", "SoundFont transpose", "Reverb and chorus";
  - "Status", "Settings".
- The window is tall enough for the SoundFont page's 16 rows: two columns of 8 channels, each row a channel combo and its output combo.

- [ ] **Step 1: Write failing tests** (window tests, so few runs: put several checks in each):
  - `test_gui_soundfont_tab`:
    - opens on the Sampler tab for a sampler state; Right on the tab control switches to SoundFont and saves `plays soundfont`;
    - Tab order on the SoundFont page in all mode matches the list above exactly (by class and name);
    - Mode set to One preset hides the 32 channel combos, and shows Preset and Channel;
    - choosing a preset with Down sets `sf_preset` in the saved state;
    - Ctrl+Tab goes back to Sampler.
  - `test_gui_soundfont_browse_and_drop`: the stubbed dialog picks a fixture .sf2, and the SoundFont edit reads "<name>, N presets". Next file steps to the next .sf2 in the folder.
  - `test_gui_channel_from_song`: after a program change 33 on channel 2 has played through the plugin (a host `play` before the `gui` action list, or a new host action `midi <status> <d1> <d2>` that processes one block), the Channel 2 combo reads "From the song (now <bass name>)".
- [ ] **Step 2: Run them**: they fail. **Step 3: Implement.** **Step 4: Run** `python -m pytest tests/test_plugin.py -q`. Expected: PASS.
- [ ] **Step 5: Commit.**

### Task 10: Numbers, documents and install

**Files:**
- Modify: `README.md`, `tests/measure.py`, `docs/superpowers/stage1-carryover.md` (a SoundFont section of parked items)

- [ ] **Step 1: Measure in measure.py**, then put the numbers in the README with their method. Measure on just_t4_fixed.sf2:
  - the time to open the font, and its memory;
  - the memory and load time of programs 0, 25 and 128:0;
  - the CPU for 64 voices held at 48 kHz in 256-frame blocks, with effects on and off (target under 5 % of real time);
  - the memory of a second instance on the same font (target: under 2 MB more).
- [ ] **Step 2: README.** Add:
  - the SoundFont tab, its controls in Tab order and its keys;
  - the modes, program fallback, outputs, reverb and chorus;
  - the state lines;
  - what's not in yet: keep length and tempo lock for SoundFont zones, editing SoundFont zones as a Samplosis instrument, sm24 24-bit samples, and NRPN or SysEx beyond the GM, GS and XG resets.
- [ ] **Step 3: Rebuild, run the full suite** (`python -m pytest tests -q`) and `install.bat`. Expected: all pass, "Installed to D:\clap\samplosis.clap" (or the message to close REAPER).
- [ ] **Step 4: Commit.**
