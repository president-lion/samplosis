# Final fix wave report (stage3-formats, after fc639b4)

Status: DONE_WITH_CONCERNS (item 1's cold-disk target is improved but not fully met; see item 1 and Concerns).

Commits on stage3-formats (not pushed):
- 0cc63d0 one can't-load text, statuses agree during a load, FFmpeg searched once (items 9, 11)
- 012a59f GigaStudio compressed waves scanned lazily (item 5)
- 309bace streamer scheduling, decode worker, cache limit, offline waits (items 1, 2, 3, 4, 6, 7, 8, 10, 14, look-ahead gap)
- 6af6895 GigaStudio parse cache, prefetch before a compressed decode (item 5)
- 8ae7508 streamer defaults from the cold-disk measurements, smaller records (item 1)
- 9e13280 measure.py work folder, cleanup, before/after; README numbers (items 12, 13)

Machine: Core Ultra 7 255HX, 32 GB, Windows 11. E: is a Transcend StoreJet USB hard disk on the Bulk-Only driver
(`Get-PnpDevice`: USBSTOR, not UASPStor), so it takes one command at a time and can't reorder reads. C: and D: are
one NVMe SSD.

Measurement caveat found on the way: from 9:59 to about 10:55 Mike's 7-Zip (7zG.exe) was extracting an archive onto
E: (7 MB/s of writes). Every cold measurement taken in that window was thrown away; the numbers below were taken
after it finished (each run logs whether 7zG was running: "7zG: False").

## 1. Cold-disk 64-voice underruns (Important)

### Instrumentation first
- `smp_sfs_test_read_hist(out, n, reset)`: every streamer read's latency (issue to completion) in buckets <1, 1-2,
  2-5, 5-10, 10-20, 20-50, 50-100, 100-200, 200-500, >500 ms, plus reads and KB.
- `smp_synth_underruns_onset(s)`: underruns of voices whose first streamed read hadn't come yet; the rest are voices
  already fed (`fed`/`fed0` in the voice).
- `smp_sfs_test_features(mask)`: switches for each part below, so one build measures each step (measure.py
  MEASURE_STREAM_FEATURES); MEASURE_DLL measures the old build (a worktree of fc639b4 at
  E:\samplepacks\_build\samplosis_head, deleted afterwards).
- Finding from the split: every underrun, before and after, is "already playing". The first read after the head
  always comes in time (Windows' read-ahead from the head load). The dropouts are the second wave: every voice of
  a chord needs its first refill from a cold place at nearly the same moment, about 0.6 s in (head 0.25 s + one ring
  0.37 s), and 32 to 64 seeks of 10-20 ms don't fit in a ring.

### What was built (sfstream.c, sfvoice.c, sffont.c, cache.c)
a. Deadline scheduling: each pass works out per record the frames the voice has ahead (ring, head, post-loop head:
   `skip_mem`), divides by the voice's speed (published per block in `SfSlot.speed`; estimated from the pitch at the
   start), and sorts. Round 1 serves records under 150 ms of slack, round 2 tops up rings under half full, both in
   deadline order. The "one read just enough to lift it over the target" form was built and measured first: it
   doubled the slow (seek) reads at 128 voices (700-900 reads of 5-50 ms against about 380 before) and made 128
   voices far worse, so a visit now fills the ring to its limit.
b. Reads land in the ring and convert in place when a frame is at most 4 bytes (16-bit stereo, any mono): no copy.
   Wider layouts (24-bit stereo) go through a 256 KB heap buffer made once per streamer when first needed. Without
   SFS_F_BIGREAD reads are capped at 16 KB (the old stack buffer) for comparison.
c. Stereo pairs read once: the leader reads interleaved frames and writes both rings; the pair parts when needs,
   releases or lives part, and the follower reads for itself from there. A newer voice in a slot whose reads are in
   flight waits for them (pool of NIO messages), so a ring is never written for two voices.
d. Overlapped reads from a pool of 8: a record's stretch is issued as back-to-back reads (wrap, loop turns); with
   SFS_F_ASYNC several records' reads are in flight. A record keeps its hold until its reads complete; `sfs_free`
   cancels (`CancelIoEx`) and waits at most 5 s (else the streamer's memory is leaked on purpose, holds released).
   E: is BOT, not UAS: no reordering. Measured: several records in flight is worse at 64 and 128 voices (below).
e. Urgent first reads of one file within one 25 ms slack band are sorted by byte offset.
f. Warm-ahead: the loader reads 0.4 s past each streaming head into a throwaway buffer (file cache). Measured both
   ways (below): it moves the dropouts from 0.6 s to 1.3-1.4 s instead of removing them.
- Also: the look-ahead gap before the post-loop head (`ahead_short` now also checks frame t0 - 1 when the look-ahead
  lands in the post-loop head and the voice isn't looping), POLL_MS comment fixed (item 10).
- Extra experiment: FILE_FLAG_SEQUENTIAL_SCAN on streaming handles (SFS_F_SEQ): worse.

### Measurements (cold: purge per run, the measure.py method; 1.06 GB SFZ piano, 352 stereo WAVs on E:)
Feature by feature, clean disk (MEASURE_STREAM_FEATURES, one run each of 32/64/128 voices; underruns):

| mask | parts | 32 v | 64 v | 128 v |
|---|---|---|---|---|
| 0x0 | new code, all off | 135 | 873 | 9989 |
| 0x1 | deadline | 66 | 555 | 8036 |
| 0x3 | + whole reads | 225 | 596 | 3918 |
| 0x7 | + pair | 189 | 695 | 7863 |
| 0xf | + several in flight | 6 | 4132 | 30723 |
| 0x1f | + byte order | 2 | 16155 | 28363 |
| 0x3f | + warm-ahead | 0 | 5612 | 25534 |

Second series, two runs each (64, 128 voices): 0x3: 12, 108 / 3635, 11393; 0x43 (+sequential handles): 714, 1030
/ 7210, 14133; 0x47 (+pair): 1184, 1544 / 9080, 8806; 0x63 (+warm): 552, 958 / 6552, 8800 (dropouts at 1.3 s).

Default chosen: deadline + whole reads + byte order (SFS_F_DEFAULT = 0x13); the others are off (switches stay).

Final before/after, three rounds alternating the old build (fc639b4) and the new one, cold, 10 s real time,
`MEASURE_PARTS=stream MEASURE_STREAM_RUNS=1 MEASURE_STREAM_NOTES=16,32,64 python tests\measure.py` (MEASURE_DLL set
to the old build for "before"); underruns (voice-blocks) and 100 ms windows more than -20 dB off the exact offline
render:

| voices | before | after |
|---|---|---|
| 32 | 138 (2 bad), 180 (2), 2 (0) | 10 (1), 45 (1), 80 (2) |
| 64 | 1015 (5), 1528 (6), 222 (2) | 1640 (6), 94 (2), 0 (0) |
| 128 | 8284 (12), 10074 (12), 10547 (12) | 5594 (10), 1782 (7), 4291 (10) |

Warm (no purge, MEASURE_STREAM_PURGE=0): before 64 / 8 / 353, after 18 / 0 / 25 (32 / 64 / 128 voices).
Disk reads per run as needed (30 / 56-68 / 107-116 MB). CPU per 10 s: 0.33-1.16 s, about as before. Rings +4-8 MB.
The baseline taken before any change (with the instrumentation only) gave 3 / 6, 554 / 2812, 9412 / 7297.

Physics, honestly: a ring holds 0.37 s; one seek on this disk takes 10-20 ms; so about 20-35 voices can be refilled
from cold places within one ring, and a 64-voice chord on files never read since boot can still drop for a few 100 ms
around 0.6-1.3 s (one run in three at 64 voices, every run at 128). Curing it fully needs more audio ahead per voice
(longer heads, bigger rings: memory, Mike's choice) or a faster disk. Covered by the existing streaming tests
(`test_streaming_many_voices_no_underrun`, `test_mode3_release_realtime`, `test_stream_steal_reuse`,
`test_stream_read_failure`, `test_stream_font_switch_lifetime`, the SFZ/DLS/Gig stereo tests), all passing on the new
streamer. No new automated test drives a cold USB disk; the look-ahead-gap fix has no test that reproduces the gap
(it needs the ring to run short exactly before the post-loop head in real time).

## 2. Member decodes can be cancelled (Important)
- Change: `load_decoded` uses `decode_audio_ex(..., pack_cancel, f, ...)` for member files and `decode_memory_ex`
  for in-font byte ranges; a cancelled decode isn't "broken". decode.c sets the cancel hook around the built-in chain
  too, so the built-in decoders stop on a close, not only Windows and FFmpeg. With item 6 these run on the decode
  worker, whose stuck synchronous reads `smp_sf_close` also cancels.
- Test: `test_sfz.py::test_sfz_close_stops_member_decode` (fake FFmpeg that hangs, the full 60 s stall timer): the
  close returns in under 2 s and no FFmpeg process is left.

## 3. The decode cache keeps to its limit (Important)
- 16-bit sources stored as int16 (version 2 mono files; `exact16` check, lossless); heads of such samples are int16
  too; DECODED samples stream through their cache file's handle like PCM. The loader and worker trim the cache to
  `cache_limit_gb` after writing (every 256 MB in a long run, and after each job and pass); open files are never
  deleted (as before). The open-key table grows by doubling (no 4096 cap).
- Tests: `test_sfz.py::test_sfz_decode_cache_int16_and_trimmed`, `test_cache.py::test_open_files_pinned_past_4096`.

## 4. Offline renders don't start notes late (Important)
- `smp_sf_hint` puts a pending voice's sample first; `smp_sf_wait_loaded` waits for that sample (or idle) with no
  fixed cap while the font's progress counter moves (samples tried, decode pieces), giving up after a minute with
  none. `start_region` and `block_start` hint and wait per pending voice; the per-block 10 s waits are gone.
- Test: `test_sfsynth.py::test_offline_waits_past_ten_seconds` (loader held back 11 s; the render waits and the note
  starts on its frame at 440 Hz).

## 5. GigaStudio parse cost at project open (Important)
- Measured before (old build, cold, a 2.02 GB compressed .gig of 352 waves written by gigmake on E:): open 726 s
  (3 GB read), 93 s the second cold time, 1.9 s warm. Far over a second.
- Change 1: libgig patch `gig::Sample::LazyScan` + `EnsureScanned()` (gig.h/gig.cpp): compressed frames are scanned
  at the first read (SetPos, Read, LoadSampleData), not at open; until then SamplesTotal is the shim's existing upper
  bound. Result: open cold 3.97-4.0 s (2 MB read; what's left is libgig reading chunk headers all through the file).
- Change 2: a parse cache (gigshim.cpp, `<key>.gigp` in the cache folder, key "gigparse1|full path|size|mtime",
  extension files' stamps checked, struct sizes and counts validated, written via .part + move, trimmed with the
  cache): later opens 149 ms cold, 1 ms warm.
- Change 3: before libgig decodes a compressed wave, its bytes are read through in 4 MB pieces (libgig reads frame by
  frame on a random-access handle): first wave decode cold 58 s -> 0.37-0.41 s.
- Command: `MEASURE_PARTS=gig MEASURE_STREAM_KEEP=1 python tests\measure.py` (MEASURE_DLL for the old build).
- Test: `test_gig.py::test_gig_parse_cache` (second open served by the cache with the same model and sound; a damaged
  cache file and a changed file are parsed again); all 19 GigaStudio tests pass with the lazy scan.

## 6. One font's decodes don't starve others (Important)
- A decode worker thread beside the loader; the loader only loads heads (PCM, or a cached decoded copy); decodes and
  SF2Pack unpacking are queued to the worker, which leaves results for the loader to publish. Time slices were tried
  first and dropped (a single FFmpeg or Windows decode can't be split).
- Test: `test_sfz.py::test_sfz_long_decode_doesnt_hold_up_other_fonts` (font A stuck in FFmpeg; font B loads in under
  1 s).

## 7. Unbounded waits on a dead share (minor)
- `sfs_free`: CancelIoEx + at most 5 s; `smp_sf_close`: CancelSynchronousIo on the loader and worker every 50 ms
  while either works on the closing font, and while joining them; `smp_pread`: 30 s then CancelIoEx.
- No test makes a share go dead; reviewed by reading; every close path runs in the suite.

## 8. source_keys off the main thread (minor)
- `sf_parse_ex` computes the keys at the end of the parse (the window parses on its worker); `setup` only for fonts
  built otherwise. Covered by the DECODED-key tests (unchanged, passing).

## 9. Double full stop; statuses agree (minor)
- `smp_plug_cant_load` shared by both failure paths; `loading_text` first in both statuses. Covered by the existing
  window tests; none sends a why ending in a full stop through the immediate path (the code is now the shared one).

## 10. POLL_MS comment: fixed (20 ms, a backstop).

## 11. FFmpeg not searched on every Settings switch (minor)
- `find_ffmpeg(g, force)` caches per saved path. Test: `test_gui_settings_page` counts searches through a new host
  command (`ffsearches`): two more Settings visits don't search.

## 12. measure.py: work folder never %TEMP% (E:\samplepacks\_build, else the cache drive), close in finally, delete
the piano in finally; new options and the gig part. Killed runs during this work left three small work folders
(0-3 MB), deleted by hand.

## 13. README: run counts reproducible, 17 % stated as over the rule, AC-3 machine-dependent, streaming and
GigaStudio numbers, loader and worker, measure.py options.

## 14. The flaky test_font_switch_while_playing
- Causes (in the test): a 10-block analysis window holding a single block of sound (the switch silenced the rest)
  peaks at DC after the Hann window (or at bin 0, which raised); and under CPU load the below-normal loader is
  starved, so the fixed 0.4 s sleeps didn't guarantee the fonts were loaded.
- Fix: analyse only windows that sound in every block; wait for loads and count rendered blocks instead of sleeping.
- Proof: a temporary 50-run wrapper: idle "50 passed in 19.58s"; every core busy "50 passed in 216.85s"; on the final
  build "50 passed in 18.04s". (Before the fix, the same wrapper failed 10 of 50 idle.)

## Full suite
`python -m pytest tests -q -p no:cacheprovider`: "409 passed in 298.84s (0:04:58)" (final build, after deleting the
temporary flaky wrapper). An earlier full run at 309bace: 408 passed.

## Install
`install.bat`: "Installed to D:\clap\samplosis.clap" (2,082,304 bytes; hash equal to the built samplosis.clap);
REAPER didn't hold the file. Test data deleted: E:\samplepacks\_build\samplosis_stream_test, samplosis_gig_test,
the fc639b4 worktree samplosis_head (git worktree remove), and the measurement logs.

## Concerns
1. Review Focus 1 is improved, not met: see item 1's table and physics. The next lever is memory (longer heads or
   rings), which is Mike's call.
2. Several records' reads in flight, stereo-pair reads, warm-ahead and sequential handles are built but off by default
   (measured no better on this BOT disk); on a UAS disk or SSD they may behave differently. Re-measure with
   MEASURE_STREAM_FEATURES before turning any on.
3. The second instance with a note playing measured 2.8 MB against the old build's 2.7 MB in two alternating runs
   (bigger streamer records, about 0.1 MB); stage 2 was 2.3 MB.
4. A GigaStudio wave's length is an upper bound until its first decode (the decode gives the real one); nothing in
   the player sizes anything by it, and the GigaStudio tests pass, but the sample header's `end` reads larger before
   the decode.
5. Item 7 and the look-ahead-gap fix have no reproducing test.
6. The Tyros real-font timing test (`test_real_font_preset_memory`, dt < 2 s) failed once while 7-Zip was writing to
   E:; it passed on rerun and in the full suites.
