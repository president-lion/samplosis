# Samplosis: Many Formats Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Mike asked for "a stupid amount of supported formats" in both engines, and approved this list:
- The sampler reads every common audio format: Opus, WavPack, Monkey's Audio, CAF, raw PCM, AAC, M4A, MP4, Apple Lossless, WMA, AC-3 and AMR/3GP. Through FFmpeg it reads anything FFmpeg reads.
- The SoundFont tab plays SF2, SF3, SF4, SF2Pack, SoundFont 1 (.sbk), DLS, GigaStudio (.gig), SFZ and Decent Sampler (.dspreset).

**Architecture:**
- **One decode layer.** `decode.c` becomes a chain of decoders, each tried in order:
  1. built-in single-file decoders, chosen by magic bytes;
  2. vendored libraries (Opus, WavPack, Monkey's Audio);
  3. Windows Media Foundation;
  4. ffmpeg.exe last.
- **The cache.** Every decoded result goes to the existing disk cache, so a file is decoded only once.
- **Instrument formats are loaders.** Each one fills the same model the SF2 player already plays: presets, then regions with SF2 generators, then samples.
  - The model gains region conditions for SFZ, GigaStudio and Decent Sampler: round robin, random, keyswitch, release trigger, CC range and crossfade.
  - Samples gain a source descriptor (where the audio lives and how it's encoded).
  - Long samples stream from disk instead of being held whole, so multi-gigabyte libraries fit Mike's memory rule.

**Tech Stack:** As in stage 2 (C11 and C++ with MSVC cl, build.bat, pytest, ctypes, tests/host.c), plus these vendored sources:
- Opus 1.6.1, opusfile 0.12 and libogg 1.3.6 (BSD);
- WavPack 5.9.0 (BSD);
- Monkey's Audio SDK 13.43 (BSD-3-Clause);
- libgig 4.6.0 (GPL-2.0-or-later);
- yxml (MIT).

The research is in `.superpowers\formats-research.md`: build flags, file sets, format layouts and the SFZ opcode ranking. Every task's implementer reads the section for their task.

**Spec:** docs\superpowers\specs\2026-10-28-samplosis-design.md. The relevant sections are "SoundFonts", "Memory and speed", "Anti-goals" and "Non-goals" (SFZ was listed as "some day"; Mike has now asked for it).

## Global Constraints

The constraints of stage 1 and stage 2 still hold:
- The audio thread never allocates, locks, waits (except offline waiting in the SoundFont player), or touches the disk.
- Fonts close only after every voice has released them, and never from DllMain.
- The window rules: Tab leaves the plugin past either end; no focus dead ends; combo boxes for choices; Up and Down stop at the limits; every change applies at once; values are read in words; no screen-reader chatter.
- Tests send window keys by message, never with SendInput, and window test runs are kept few.
- The plan's numbered rules for SoundFont sound apply.

For this stage:
- **Branch.** Work on `stage3-formats`, cut from stage2-soundfont at 2dba8a6. Commit after each task, ending the message with "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>". Never push.
- **Licence.**
  - Samplosis stays GPL version 2. No Apache-2.0 code is vendored (so not Apple's ALAC decoder, and not SpessaSynth); Apache-licensed readers may be read for understanding but never copied.
  - Every vendored library keeps its licence file in its folder under `src\codecs\<name>\`.
  - README's licence line lists them.
- **Vendored sources.**
  - Download them once into `src\codecs\<name>\` (only the files the build needs, plus the licence and a VERSION.txt naming the version and URL).
  - Compile them into the plugin from build.bat, with no CMake.
  - C++ libraries (Monkey's Audio, libgig) get extern "C" shims compiled with /EHsc, the way rb.cpp is. No C++ exception may cross into C.
- **Read-only places:**
  - E:\samplepacks\soundfonts and any other sound library;
  - E:\pypy;
  - the FFmpeg install.

  Test fixtures are generated into pytest's tmp_path at test time, with ffmpeg or the vendored encoders where possible. A fixture that can't be generated (for example .ape, since ffmpeg can't encode it) may be committed as a tiny file under tests\fixtures, with a note on how it was made.
- **C: is nearly full.**
  - Decoded and cached audio goes to the cache folder in Settings, which defaults to D:.
  - Temporary files go to the cache folder or tmp_path, never to %TEMP% for anything big.
- **FFmpeg.**
  - It is found from the Settings path. Otherwise it's looked up on PATH; otherwise in the WinGet package folder `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg*\*\bin\ffmpeg.exe`.
  - It runs with CREATE_NO_WINDOW at below-normal priority and with a timeout.
  - It is never run on the audio thread.
  - When it isn't found, the reason is plain: "FFmpeg isn't installed; this format needs it".
- **Statuses** say what decoded a file when that wasn't built in, for example "Ready: x.m4a (decoded by Windows)" or "Ready: x.dsf (decoded by FFmpeg)". Nothing changes for formats that were already supported.
- **Memory.** The SoundFont engine holds in memory only:
  - a head per sample: the first 0.25 s, or the whole sample when it is shorter than 1 s (a 1 s cap);
  - the voices' ring buffers.

  The rest streams from disk. The stage 2 numbers for SF2 must not get worse by more than 10 %.

## Review Focus

1. **A sample library on E:, the USB hard disk.** Streaming must not drop out at 64 voices of a big piano (a GigaStudio or SFZ multi-sample piano). Each note must start at once from its head in memory, and ring sizes must be measured. Pinned by `test_streaming_many_voices_no_underrun` (Task 5) and the measurements in Task 12.
2. **An SFZ or Decent Sampler file whose samples are missing or in an odd format.** The instrument loads with the regions that are there, and the status names the first missing file. Nothing crashes and it doesn't load silently. Pinned by `test_sfz_missing_sample` (Task 9) and `test_ds_missing_sample` (Task 10).
3. **FFmpeg missing or hanging.**
   - Missing: a plain reason.
   - Hanging: a timeout (60 s, or 4 × the file's duration when ffprobe knows it), the process killed, and a plain reason.
   - Either way the window stays responsive: the decode runs in the background, and the status says "Decoding x.dsf with FFmpeg".
   
   Pinned by `test_ffmpeg_missing` and `test_ffmpeg_timeout` (Task 4).
4. **Damaged files of every new kind.** A file truncated at every 97th byte, and seeded random corruption, never crash. They give NULL with a reason, or a font. Pinned by a corrupt-file test in each loader task (Tasks 6, 7, 9, 10, 11).
5. **Round robins and release triggers.** These are what real SFZ and GigaStudio instruments are built on:
   - Round robin cycles per region group as the SFZ spec says.
   - A release trigger plays on note-off with the right velocity, and its level follows how long the note was held (`rt_decay`).
   
   Pinned by `test_round_robin_seq` and `test_release_trigger` (Task 8).

---

## Shared definitions

**The decode layer** (src\decode.h). It is called from main or worker threads, never from the audio thread.

```c
typedef enum { SMP_DEC_BUILTIN, SMP_DEC_LIBRARY, SMP_DEC_WINDOWS, SMP_DEC_FFMPEG } SmpDecBy;
/* float, channel after channel (as now); by = what decoded it; cancel may be NULL */
float *decode_audio_ex(const char *path, int *chans, long *frames, int *rate, SmpDecBy *by,
                       int (*cancel)(void *), void *arg, char *err, int cap);
float *decode_memory(const void *bytes, size_t len, int *chans, long *frames, int *rate, char *err, int cap);
/* built-in and library decoders only, for compressed SoundFont streams (FLAC, Ogg Vorbis, Opus, WavPack) */
int smp_audio_extensions(const wchar_t ***list);   /* the extensions the sampler lists in Browse and the File combo */
```

`decode_audio` stays the same call (it is `decode_audio_ex` with `by` and `cancel` NULL).

**Settings** (settings.txt and SmpSettings gain these fields, in this order after keep_tone):
- `ffmpeg_path` (empty means search for it);
- `raw_rate` (default 44100), `raw_channels` (1 or 2, default 2), `raw_format` (s16, s24, s32, f32 or u8; default s16), `raw_endian` (little or big; default little);
- these are used for files ending .raw or .pcm.

**The SoundFont model's sample source** (src\sf2.h). This replaces "an offset into smpl" for every format:

```c
typedef enum { SMP_SRC_PCM_FILE, SMP_SRC_DECODED } SmpSrcKind;
typedef struct {
  SmpSrcKind kind;
  int path_id;               /* index into the font's path table (the sf2 itself, or a sample file) */
  uint64_t offset;           /* PCM_FILE: byte offset of the first frame */
  uint8_t enc;               /* PCM_FILE: SMP_PCM_S16LE, S24LE, S32LE, F32LE, U8, S16BE, S24BE */
  uint8_t channels, channel; /* PCM_FILE: frames are interleaved over channels; this sample is one channel */
  uint64_t key;              /* DECODED: cache key of a decoded mono float copy in the disk cache */
} SmpSrc;
```

Every sample gets one. SF2 smpl data is PCM_FILE (S16LE, 1 channel). SF3, SF4 and SF2Pack data, and SFZ, Decent Sampler and DLS files in compressed formats, are DECODED: decoded once by the loader thread into the disk cache, then streamed like the sampler's cache files.

**Region conditions** (samplosis.h, in SmpSfRegion; all default to "always"):

```c
typedef struct {
  uint8_t trigger;              /* 0 attack, 1 release, 2 first, 3 legato */
  uint16_t seq_len, seq_pos;    /* round robin: plays when (counter % seq_len) + 1 == seq_pos; 0 = always */
  float lorand, hirand;         /* random 0..1 */
  int8_t sw_lokey, sw_hikey, sw_last, sw_down, sw_up, sw_previous;   /* keyswitches; -1 = unused */
  uint8_t locc_n; struct { uint8_t cc, lo, hi; } locc[4];           /* CC ranges */
  int8_t xfin_lokey, xfin_hikey, xfout_lokey, xfout_hikey;          /* crossfades; -1 = none */
  uint8_t xfin_lovel, xfin_hivel, xfout_lovel, xfout_hivel;
  uint8_t xf_curve;             /* 0 power, 1 gain */
  uint32_t group, off_by;       /* SFZ group / off_by (exclusive class is SF2's narrower form) */
  uint8_t off_mode;             /* 0 fast, 1 normal (release) */
  float rt_decay;               /* dB per second held, for release triggers */
  float amp_veltrack;           /* percent; NaN = use SF2 modulators */
} SmpSfCond;
```

SF2-family loaders leave every field at its default. The synth evaluates conditions at note-on, and at note-off for release triggers.

**Loader entry.** `smp_sf_open(path, …)` picks the loader by magic bytes, then by extension:
- RIFF form "sfbk" is SF2, SF3 or SF4;
- RIFF form "sfpk" is SF2Pack;
- RIFF "DLS " is DLS;
- a RIFF "DLS " file with Giga chunks, or the .gig extension, is GigaStudio;
- the .sbk extension and its structure is SF1;
- text with SFZ headers, or .sfz, is SFZ;
- XML with `<DecentSampler>`, or .dspreset, is Decent Sampler.

Every format keeps the same API: the presets, regions and samples of stage 2.

---

### Task 1: The decode chain, CAF, raw PCM and the Settings for raw and FFmpeg

**Files:**
- Modify: src\decode.c, src\decode.h, src\settings.c, src\samplosis.h, src\sample.c (it calls decode_audio_ex and keeps `by` for the status), src\gui.c (the Settings page and the sampler's extension list), src\plugin.c (statuses)
- Test: tests\test_formats.py (new) and tests\test_cache.py (settings)

**Interfaces:** Produces decode_audio_ex, decode_memory, smp_audio_extensions and the new settings fields (Shared definitions). In this task the chain is: built-in by magic bytes, then CAF, then raw by extension. Later tasks insert their stages.

**Window.** The Settings page gains these controls, after Cache size limit and before Cache in use. All are combo boxes, and "Raw sample rate" lists the common rates from 8000 to 192000:
- "Raw sample rate";
- "Raw channels";
- "Raw sample format";
- "Raw byte order";
- "FFmpeg" (a read-only edit showing the path found, or "Not found");
- "Browse for FFmpeg".

- [ ] **Step 1: Write failing tests.**
  - `test_caf_pcm_and_float`: ffmpeg writes CAF in s16, s24 and f32 into tmp_path. smp_sample_load reads each one, at the same rate and length as the source within 1 frame, and the same samples within 1e-4.
  - `test_raw_pcm_uses_settings`: a .raw file of s24 big-endian stereo at 48000 Hz loads correctly once the settings say so. With the default settings it loads differently but doesn't crash.
  - `test_settings_raw_and_ffmpeg_roundtrip`.
  - `test_audio_extensions_listed`: smp_audio_extensions includes .caf, .raw, .pcm, .opus, .wv, .ape, .m4a, .aac, .mp4, .wma, .ac3, .amr, .3gp, .alac, .w64, .rf64 and .aifc. Later tasks make these work; listing them here is fine.
  - The window test for the Settings page is extended with the new controls in Tab order.
- [ ] **Step 2: Run them; they fail.** **Step 3: Implement.** **Step 4: Run** `python -m pytest tests -q`; expect PASS. **Step 5: Commit.**

### Task 2: Opus, WavPack and Monkey's Audio

**Files:**
- Create: src\codecs\{opus,opusfile,ogg,wavpack,mac}\ (vendored), src\codecs\mac_shim.cpp
- Modify: build.bat, src\decode.c, README.md (licence line)
- Test: tests\test_formats.py; tests\fixtures\tone.ape (if one has to be committed)

**Interfaces:**
- Consumes Task 1's chain. Adds a "library" stage, chosen by magic bytes: `OggS` with an OpusHead packet; `wvpk`; `MAC `.
- decode_memory also uses these for compressed SoundFont streams (Tasks 6 and 7).

- [ ] **Step 1: Write failing tests.**
  - `test_opus`: an ffmpeg-made .opus, mono and stereo at 48 kHz. Rate 48000, length within the encoder's pre-skip, and correlation with the source above 0.99.
  - `test_wavpack`: ffmpeg-made .wv, 16-bit, 24-bit and float. Bit-exact for the lossless ones.
  - `test_ape`: the fixture is bit-exact.
  - `test_status_says_library`: none of these mention Windows or FFmpeg.
  - `test_build_has_no_new_warnings_from_our_code` is not needed. Vendored sources may warn; keep their warnings out of the log with /w for those files only.
- [ ] **Step 2: Run them; they fail.** **Step 3: Vendor** the libraries, using the file sets and flags in formats-research.md sections 1 to 3. **Implement.** **Step 4: Run** the full suite. **Step 5: Commit.**

### Task 3: Windows Media Foundation

**Files:**
- Create: src\mfdecode.c
- Modify: src\decode.c, build.bat (mfplat.lib, mfreadwrite.lib, mfuuid.lib)
- Test: tests\test_formats.py

**Interfaces:**
- Adds a "windows" stage after the library stage. It is tried for any file the earlier stages refused.
- IMFSourceReader asks for float PCM at the native rate and channels. MFStartup and COM are set up per calling thread.
- It never runs on the audio thread. Tasks 5 and 6 call it from the loader thread.

- [ ] **Step 1: Write failing tests.**
  - `test_m4a_aac`, `test_mp4_audio`, `test_wma`: ffmpeg makes each file. Each must load with by = WINDOWS, the right rate, length within 2048 frames, and correlation above 0.98.
  - `test_alac_m4a`: by = WINDOWS when Windows decodes it, otherwise it falls to FFmpeg in Task 4. Either is a pass, and the test records which.
  - `test_windows_refuses_cleanly`: a corrupt .m4a gives a plain error and doesn't hang.
- [ ] **Step 2: Run them; they fail.** **Step 3: Implement.** **Step 4: Run** the full suite. **Step 5: Commit.**

### Task 4: FFmpeg as the last decoder, in the background

**Files:**
- Create: src\ffdecode.c
- Modify: src\decode.c, src\sample.c, src\plugin.c, src\gui.c
- Test: tests\test_formats.py, tests\test_plugin.py

**Interfaces:**
- Adds the "ffmpeg" stage, which works as follows:
  - ffprobe gives the rate and channels (JSON); the decode itself is `-f f32le` to a pipe.
  - It reads with a timeout and kills the process on timeout or cancel.
  - CREATE_NO_WINDOW, below-normal priority.
  - The search order is in Global Constraints.
- **The sampler** loads every file in the background from now on; sample.c's load takes a cancel callback.
  - The window's load returns at once. The status says "Decoding x.dsf with FFmpeg" or "Loading x.wav", and the sample is swapped in when it's ready.
  - Loading a new file cancels a decode still running.
  - The plugin's state load (main thread) still loads synchronously, as now, so a project opens with its sample. It uses the cache when the file has been decoded before.
- Tests get a hook that sets the FFmpeg path and a fake-ffmpeg path (a tiny exe or .bat in tmp_path that hangs or prints garbage).

- [ ] **Step 1: Write failing tests.**
  - `test_ffmpeg_decodes_odd_format`: a .dsf, or .mka with flac, or .tta, made by ffmpeg, loads with by = FFMPEG.
  - `test_ffmpeg_missing`: the plain reason.
  - `test_ffmpeg_timeout`: a fake FFmpeg that never writes. The reason comes within the timeout, which the test hook shortens, and no process is left behind.
  - `test_ffmpeg_garbage`: a fake that writes random bytes and exits 1 gives a plain error.
  - `test_background_load_cancel`: two loads in a row leave the second sample loaded, and the first process is killed.
  - `test_gui_load_does_not_block`: through host.exe, the window's Browse of an FFmpeg-only file returns at once with the status "Decoding …", and a later `pump` shows "Ready: … (decoded by FFmpeg)".
- [ ] **Step 2: Run them; they fail.** **Step 3: Implement.** **Step 4: Run** the full suite. **Step 5: Commit.**

### Task 5: Streaming in the SoundFont engine

**Files:**
- Modify: src\sf2.h, src\sf2.c (SmpSrc for SF2 and SF3), src\sffont.c (heads, sources, decode to cache, a stream API), src\sfvoice.c (reads its head, then its ring), src\sfsynth.c (owns the rings and a streamer thread per synth)
- Test: tests\test_sffont.py, tests\test_sfsynth.py

**Interfaces:**
- **Each loaded sample** is held as one of two things:
  - a head: int16 or float mono, the first 0.25 s, or the whole sample when it's under 1 s;
  - the whole sample, when it is under 1 s.
- **Sample data storage** is float for DECODED sources and for PCM_FILE sources wider than 16 bits; int16 otherwise. The voice handles both.
- **Streaming.**
  - A voice past its head reads a per-voice ring (16384 frames for each sounding voice, allocated once per synth for sf_voices).
  - The synth's streamer thread fills the rings from the sample's source: a file read for PCM_FILE, or a cache file read for DECODED.
  - Loops stream with wraparound, as the sampler's streamer does.
  - An underrun fades, counts and never clicks (the sampler's SMP_UFADE pattern).
- **DECODED sources** are decoded by the loader thread with decode_memory, or decode_audio_ex for external files, into the cache folder as mono float. That is one cache file per sample (per channel for stereo source files). The key is built from the path, size, mtime, byte range and channel.
- **The use, acquire and release protocol stays.** Acquire now returns a view: a pointer to the head, the head frames, the total frames, the loop points and a stream handle.
- SF2 stays bit-identical in sound (test_sf_reference must still pass unchanged).

- [ ] **Step 1: Write failing tests.**
  - `test_long_sample_streams`: a 30 s SF2 sample keeps only about 0.25 s in memory, and a 30 s note matches the whole-in-memory reference within -90 dB.
  - `test_streamed_loop`: a long loop wraps correctly.
  - `test_streaming_many_voices_no_underrun`: 64 voices of distinct 20 s samples from a font file in tmp_path, rendered in real time for 5 s. The underrun count is 0 (log the CPU).
  - `test_sf3_decoded_once`: the second open of an SF3 font decodes nothing (the cache is hit).
  - `test_memory_follows_heads`: the loaded bytes of a preset with long samples are under 1.2 × the sum of its heads.
  - All existing SoundFont tests, test_sf_reference included, pass unchanged.
- [ ] **Step 2: Run them; they fail.** **Step 3: Implement.** **Step 4: Run** the full suite. **Step 5: Commit.**

### Task 6: SF4, SF2Pack and SoundFont 1

**Files:**
- Modify: src\sf2.c, src\sffont.c
- Test: tests\test_sf2.py, tests\sf2make.py

**Interfaces:**
- **SF4:** SF3 with FLAC samples. The codec is sniffed per sample (`OggS` for Vorbis or Opus, `fLaC` for FLAC). Loop conventions are those of SF3 (research section 6).
- **SF2Pack:** RIFF form `sfpk`, one compressed stream for the whole smpl chunk, with the codec sniffed by magic bytes. It is decoded once into the cache as int16 mono at smpl offsets, so it becomes one PCM_FILE over the cache file, and shdr offsets work unchanged.
- **SoundFont 1 (.sbk):** the layout and unit conversions are in research section 9. ROM samples, which point at Sound Blaster hardware memory, are skipped. If every sample is a ROM sample, the font is refused with the plain reason "this SoundFont uses Sound Blaster ROM samples, which aren't in the file".

- [ ] **Step 1: Write failing tests.** sf2make gains `sf4=True` (FLAC via soundfile) and `pack=codec`. The tests:
  - `test_sf4_matches_sf2`: bit-exact, since FLAC is lossless.
  - `test_sf2pack_flac_matches_sf2`: bit-exact.
  - `test_sf2pack_vorbis`: within -30 dB.
  - `test_sbk_minimal`: a hand-built SF1 file with one preset plays at the right pitch.
  - `test_sbk_rom_only_refused`.
  - The truncated and corrupt test covers the new kinds.
- [ ] **Step 2: Run them; they fail.** **Step 3: Implement.** **Step 4: Run** the full suite. **Step 5: Commit.**

### Task 7: DLS

**Files:**
- Create: src\sfdls.c
- Modify: src\sf2.c (dispatch), src\sf2.h
- Test: tests\test_dls.py, tests\dlsmake.py

**Interfaces:**
- Instruments become presets: bank from ulBank (bit 31 means drums, giving 128), program from ulInstrument.
- Each rgn becomes a region:
  - keyRange and velRange;
  - wsmp gives the root, fine tune, attenuation and loops;
  - wlnk points into wvpl. Its wave is PCM_FILE when it is PCM; otherwise (ADPCM and the like) it is DECODED through decode_memory, which uses dr_wav for MS-ADPCM.
- art1 and art2 connection blocks map to SF2 generators and modulators, with the units and table in research section 10. Anything that doesn't map is ignored and noted in a comment.
- Windows' gm.dls (C:\Windows\System32\drivers\gm.dls) is read-only and may be read by a skip-if-missing test.

- [ ] **Step 1: Write failing tests.** dlsmake.py writes DLS files. The tests:
  - `test_dls_presets_and_pitch`;
  - `test_dls_envelope_from_art1` (attack and release times within 5 %);
  - `test_dls_drums_bank`;
  - `test_dls_corrupt`;
  - `test_windows_gm_dls_opens` (skipped if missing): it opens and lists presets, and program 0 key 60 sounds.
- [ ] **Step 2: Run them; they fail.** **Step 3: Implement.** **Step 4: Run** the full suite. **Step 5: Commit.**

### Task 8: Region conditions in the player

**Files:**
- Modify: src\samplosis.h (SmpSfCond in SmpSfRegion), src\sf2.c (defaults), src\sfsynth.c, src\sfvoice.c (crossfade gain, amp_veltrack, rt_decay)
- Test: tests\test_sfsynth.py
- Note: a test-only export `smp_sf_test_font(regions…)` builds an in-memory font from Python, so conditions can be tested before any SFZ loader exists.

**Behaviour** (as the SFZ spec says, sfzformat.com):
- **Round robin:** the seq counter counts per region set (regions sharing the same key and velocity range, and conditions other than seq), and advances once per note-on.
- **Random:** the draw is one per note-on, shared by all regions.
- **Keyswitches:**
  - Keys inside the sw_lokey..sw_hikey range never sound.
  - sw_last is the last such key pressed (or sw_default).
  - sw_down and sw_up test whether a key is held now; sw_previous tests the previous note.
- **Release triggers:** they play at note-off, at the note-on's velocity, with gain = −rt_decay × seconds held in dB. Under the sustain pedal they play when the pedal comes up.
- **trigger=first or legato:** whether other notes on the channel are held.
- **CC ranges:** checked at note-on.
- **Crossfades:** a gain by key or velocity position, with power or gain curves.
- **group and off_by:** a note in group G stops the voices whose group is in its off_by, with off_mode fast (the 5 ms kill) or normal (release). SF2's exclusiveClass stays as is.
- **amp_veltrack:** when set, it replaces the SF2 velocity-to-attenuation modulator.

- [ ] **Step 1: Write failing tests.** Each one uses the in-memory font:
  - `test_round_robin_seq` (three samples in turn, then repeating);
  - `test_random_split`;
  - `test_keyswitch_last`, `test_keyswitch_down`;
  - `test_release_trigger` (it plays at note-off, its level falls with rt_decay, and it waits for the pedal);
  - `test_trigger_first_legato`;
  - `test_cc_range`;
  - `test_crossfade_velocity` (equal power at the midpoint);
  - `test_group_off_by`.
  - All SF2 tests, reference included, are unchanged.
- [ ] **Step 2: Run them; they fail.** **Step 3: Implement.** **Step 4: Run** the full suite. **Step 5: Commit.**

### Task 9: SFZ

**Files:**
- Create: src\sfsfz.c
- Modify: src\sf2.c (dispatch)
- Test: tests\test_sfz.py

**Interfaces:**
- A .sfz file is one preset (bank 0, program 0, named after the file).
- **The parser:**
  - `#define` and `#include` (relative to the file, depth-limited to 16);
  - the headers `<control>` (default_path, note_offset, octave_offset), `<global>`, `<master>`, `<group>`, `<region>` and `<curve>`; `<effect>` is ignored;
  - comments;
  - values with spaces (sample paths run until the next opcode);
  - inheritance from global to master to group to region.
- **Opcodes**, in the order of the research ranking (section 12):
  - pitch: key, lokey, hikey, lovel, hivel, pitch_keycenter, pitch_keytrack, tune, transpose;
  - level and pan: volume, amplitude, pan, amp_veltrack, amp_velcurve_N;
  - the envelopes ampeg_*, fileg_* and pitcheg_*, plus fil_type (the lowpass and highpass kinds the voice supports), cutoff, resonance and fil_veltrack;
  - LFOs as SF2's mod and vibrato LFO, where they map;
  - samples: offset, end, loop_mode, loop_start and loop_end, sample=*sine, *silence and *noise;
  - every condition in Task 8;
  - polyphony and note_polyphony (counted per region group);
  - opcodes it doesn't know are counted and ignored.
- Opcodes map onto SF2 generators, keeping SFZ's units, with the conversions in comments.
- **Sample files** are any format the decode layer reads: PCM WAV, FLAC and the like stream as PCM_FILE when uncompressed, otherwise DECODED. Missing files skip their regions. The status names the first missing file and the count.

- [ ] **Step 1: Write failing tests.** Small .sfz files are generated in tmp_path with WAV and FLAC samples. The tests:
  - `test_sfz_basic_mapping` (keys, pitch and volume);
  - `test_sfz_inheritance_and_define_include`;
  - `test_sfz_envelope_and_filter`;
  - `test_sfz_loop_modes`;
  - `test_sfz_round_robin_and_release` (through Task 8);
  - `test_sfz_missing_sample`;
  - `test_sfz_unknown_opcodes_counted`;
  - `test_sfz_corrupt` (truncation and random bytes);
  - `test_sfz_paths_with_spaces_and_backslashes`.
- [ ] **Step 2: Run them; they fail.** **Step 3: Implement.** **Step 4: Run** the full suite. **Step 5: Commit.**

### Task 10: Decent Sampler

**Files:**
- Create: src\sfds.c and src\codecs\yxml\ (vendored)
- Modify: src\sf2.c
- Test: tests\test_ds.py

**Interfaces:**
- A .dspreset file is one preset.
- `<groups>/<group>/<sample>` attributes map like SFZ (research section 13):
  - path, rootNote, loNote/hiNote, loVel/hiVel;
  - start, end, loopStart, loopEnd, loopEnabled;
  - attack, decay, sustain, release;
  - volume, pan, tuning;
  - seqMode, seqPosition, trigger;
  - group-level attributes inherit.
- UI, effects and modulators are ignored and counted.
- Missing samples are handled as in SFZ.

- [ ] **Step 1: Write failing tests:**
  - `test_ds_mapping`;
  - `test_ds_round_robin`;
  - `test_ds_release_trigger`;
  - `test_ds_missing_sample`;
  - `test_ds_corrupt`.
- [ ] **Step 2: Run them; they fail.** **Step 3: Implement.** **Step 4: Run** the full suite. **Step 5: Commit.**

### Task 11: GigaStudio

**Files:**
- Create: src\codecs\libgig\ (vendored), src\gigshim.cpp
- Modify: build.bat (the flags in research section 11, and rpcrt4.lib), src\sf2.c
- Test: tests\test_gig.py

**Interfaces:**
- libgig reads the file; the shim catches every exception.
- Instruments become presets (bank and program from the file).
- **Dimension regions** become regions plus conditions:
  - velocity splits become velocity ranges;
  - round robin becomes seq;
  - random becomes lorand and hirand;
  - keyswitch becomes sw_*;
  - release trigger becomes trigger=release;
  - samplechannel (stereo pairs) becomes two regions with pan;
  - layers become several regions.
- Envelopes, filter and LFOs map where they can (EG1 to the volume envelope, EG2 to the modulation envelope and filter, LFO1, 2 and 3).
- Samples: uncompressed sample data is PCM_FILE at its file offset, read through libgig's offset, extension files included. Compressed Giga samples are DECODED, decoded once by libgig into the cache.

- [ ] **Step 1: Write failing tests.** A small .gig is generated with libgig's own writer, through a test-only build of the shim or a tiny helper exe built by build.bat test. The tests:
  - `test_gig_presets_and_pitch`;
  - `test_gig_velocity_and_round_robin`;
  - `test_gig_release_trigger`;
  - `test_gig_corrupt` (exceptions are caught; the open gives NULL with a reason).
- [ ] **Step 2: Run them; they fail.** **Step 3: Implement.** **Step 4: Run** the full suite. **Step 5: Commit.**

### Task 12: The window, documents, measurements and install

**Files:**
- Modify: src\gui.c (the SoundFont page's Browse filter, File combo kinds and drop routing for every instrument extension; the sampler's extension list from smp_audio_extensions), README.md, tests\measure.py, docs\superpowers\stage1-carryover.md

**Window:**
- The SoundFont page's file label stays "SoundFont". The tab name stays "SoundFont" (Mike knows it by that name).
- The SoundFont edit shows the format, for example "Piano.sfz, SFZ, 1 preset" or "GM.dls, DLS, 235 presets".
- The status reads "Missing 3 samples (first: …)" when samples are missing.

**Documents and measurements:**
- **README:** every format, and which stage decodes it (built in, library, Windows or FFmpeg). Also:
  - the raw settings and FFmpeg settings;
  - streaming;
  - what of SFZ, Decent Sampler and GigaStudio isn't supported, with the opcode counts from the unknown-opcode counter.
- **Measurements:**
  - just_t4_fixed.sf2 against the stage 2 numbers, which may be at most 10 % worse;
  - a 1 GB SFZ or GigaStudio-sized instrument made from repeated test samples on E:, in a folder created under E:\samplepacks\_build\samplosis_stream_test (deleted afterwards): load time, memory, and 64 voices with no underruns.
- **Install.**

- [ ] **Step 1:** add the window test checks (put them inside existing window tests where possible): an .sfz and a .dls dropped switch to the SoundFont tab, and the File combo lists every instrument kind.
- [ ] **Step 2:** implement, document, measure, run the full suite and install.
- [ ] **Step 3: Commit.**
