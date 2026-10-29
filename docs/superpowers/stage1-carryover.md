# Carried over from stage 1 (2026-10-28)

Rulings, plus findings that per-task and final reviews deferred. Each line comes from the stage 1 build ledger.

- - T1 and T5 (engine.c). T1 creates engine.c with smp_version; T5 says "Create engine.c". Conflict. Ruling: T5 modifies engine.c and keeps smp_version — trivial — none.
- - T2 and T5/T7/T9 (param ids). smp_value_text needs the param list, but the plan doesn't say where the ids live. Ruling: T2 adds `enum` SMP_P_* ids plus SMP_NPARAMS (29) and a static param table (name, lo, hi, default, unit kind) to samplosis.h/notes.c; later tasks use it — renaming later is cheap.
- - T5 and T6 (keep_length). T5 defines the param, but rendering arrives in T6. Ruling: in T5, keep_length is stored but ignored (always resample); T5 tests set it off explicitly — none.
- - T6 internal (speed with keep length). It says speed scales the time, not the pitch, but playing a copy slower lowers its pitch. Conflict. Ruling: keep-length copies are rendered by Rubber Band with time ratio 1/speed, and the cache key includes speed (`kl|src|shift|speed|r3`); changing speed re-queues renders, with resample fallback meanwhile — spec says keep length keeps length/pitch independent; cost if wrong: a re-render on every speed change.
- Task 1: minor (deferred): conftest temp settings named settings.ini, not settings.txt; build.bat no guard when vswhere finds no VS; README lacks GPL line; shared %TEMP% obj dir
- Task 2: Ruling: param ids enum + SMP_NPARAMS + smp_param_info table live in samplosis.h/notes.c — plan didn't say where — cheap to move
- Task 2: Ruling: loop_start, start, end are percent of the sample (0..100) in stage 1 — notes.c can't know sample length — a later display "0.000 s (0.0 %)" needs the length passed in
- Task 2: minor (deferred): smp_value_parse accepts nan/inf (NaN through clampd, UB cast) — fix before GUI typed values; "-0.0 dB" display; pitch RMS gate 1e-3 rejects quiet samples; "AB4" uppercase-B flat accepted; SMP_K_POSITION unused; short samples (<~0.5 s) give pitch -1
- Task 3: minor (deferred): trim from stale listing; realloc leak cache.c:185; non-hex .smpc names parse as key 0; (DWORD)(n*8) overflow cache.c:164; rate 0 header accepted; shared .probe name in smp_cache_ok; settings parse rejects BOM/tab, keeps trailing spaces; misleading test comment test_cache.py:541; test leaves settings at Q:\nope (order dependence)
- Task 3: minor (deferred): stale unique-named .part files from a crash are never cleaned (trim should remove old *.part)
- Task 4: minor (deferred): reversed copy and cold import hold whole-file buffers (~4n floats peak; cache write takes a whole buffer — a streaming write API would fix); zero-frame decode says "out of memory"; warm-cache-missing-sidecar re-detects every load; sidecar write not atomic; g_decodes not atomic; tests lack stereo L/R mapping check vs source and >2 channel case
- Task 5: Ruling: reviewer's Minor #3 (reverse built on streamer thread stalls rings) treated as Important — Task 8 sends CLAP param events on the audio thread, so it becomes dropouts — cost if wrong: one extra worker thread
- Task 5: minor (deferred): underrun cuts with no fade; streamer fills slot-by-slot to full (should fill all to low watermark first); hard cut when all 64 slots busy; s->rev/sample rely on MSVC volatile + x64 ordering, not formal acquire/release; pan law centre 0 dB (hard pan +3 dB) — check vs RS5K; old sample freed only after audio thread runs (smp_engine_collect for Task 8 timer)
- Task 5: minor (deferred, CARRY TO TASK 8/9): sh_publish sleeps when its 64-slot retire list is full and epoch doesn't advance (host not processing) → worker stalls, render_test hangs; collect reads h->s of a head the worker may free (UB compare); start moved while notes play: ~20 ms window loses first frames; live tests busy-wait (~15 s)
- Task 6: Ruling: reviewer's Minor #4 (Rubber Band C++ exceptions crossing the C API) treated as Important — a crash in REAPER is a spec anti-goal — cost if wrong: one small .cpp shim
- Task 6: Ruling: keep-length copies only within ±48 semitones (R3 throws at +67); beyond resamples — tell Mike — cost if wrong: extreme keys change length
- Task 6: minor (deferred): constraint wording — audio thread now SetEvents rwork/work too (still only SetEvent; accept); smp_render_shift(reversed=1) calls smp_sample_reversed without rev_cs; short copies (<=1 s) held whole ~34 MB/instance — measure in Task 10; first real-time note after start moves past head resamples at each key (lazy start head)
- Task 6: minor (deferred): rb.cpp extern "C" built /EHsc — throw from inside may skip local unwinding (leak); test_tiny_samples only proves no crash; trim scans folder per copy (ms)
- Task 7: minor (deferred): paths with " or >1023 chars not round-trippable; apply always reloads/swaps sample even if unchanged (CARRY TO TASK 8: skip swap when path+key match); no sample line keeps current sample; roundtrip test text-only; missing tests (failed load keeps prior sample, duplicate lines, overlong line); names case-sensitive, inline # comment is an error
- Task 8: minor (deferred): all_off queues 128 events into 1024-slot queue (CC 120/123 burst can drop notes); two sample lines: without_sample strips all, sample_line reads first, apply uses last; collection only while processing (Task 9 GUI timer could collect); state load during playback non-atomic across params; >64 start changes while deactivated may underrun first note ~20 ms
- Task 9: Ruling: status says "Keep length: N of M copies ready" instead of plan's "keys ready" — the count is copies (pitch shifts), keys would be wrong — cost if wrong: one string
- Task 9: minor (deferred): subclasses not removed in WM_NCDESTROY; window class never unregistered; g_hide leaves focus on hidden control; no DPI scaling (fixed 760x586); WM_DROPFILES fallback untested; Ctrl+Up/Down on integer params steps 1; cache limit range 0.1..10000 GB (builder's call); Clear cache can't delete in-use files
- Task 9: minor (deferred): test hooks (smp_test_gui_dialog etc.) ship in release DLL; second drop before WM_DROPPED overwrites first
- Task 10: minor (deferred): rendering all 97 copies of a 60 s sample took 823 s and ~2.2 GB cache; peak working set 984 MB during rendering not broken down (RB offline memory?) — final review should look
- Ruling: keep-length prefetch at load limited to ±12 semitones of the root, other keys on first note-on — the spec said "render the zone's keys on load" (our own call, not Mike's), and that cost 14 min and 2.2 GB for a 60 s sample — cost if wrong: first note at a far key resamples once
- Ruling: deferred to stage 2 — streaming render/import buffers (whole-file temporaries, 512 MB cap), cold load on REAPER's main thread, streamer fill order, all_off single event, two sample lines, cache.c realloc leak — reviewer triaged as before-stage-2, not before Mike's first try — cost if wrong: a long mp3 load freezes REAPER for seconds
- Ruling: the remaining deferred minors stay parked (reviewer triaged "can wait")
- Final: minor (parked): copies 0.25-1 s now stream, so start past head resamples until start head built; failed initial ring commit unreported; fade look-ahead per block; flaky ready==before+1 assert

## SoundFonts (stage 4): parked items (2026-10-28)

Rulings made while building are in the README. These are the minor findings the reviews left parked.

Parsing and loading:
- The check on generator ids also covers records that no preset reaches, so a real font with junk there could be refused.
- A font with no ifil chunk, or a mono sample with a bad link, is accepted. ROM samples are not handled.
- Some cases have no tests: a sample end past the data, dropping sm24, duplicate bank and program pairs, and 65535 presets.
- A hostile file with 10,000 zones of 1,024 modulators each is still slow to open, though bounded. The per-zone modulator cap counts invalid modulators before filtering.
- Linked modulators are ignored, as in FluidSynth.
- sf2.h pulls in windows.h. The real-font 2 second limit in the load test assumes the operating system's file cache.

Voice and player:
- Not tested: RPN tuning, bend through the default modulator, smp_sfv_update, the kill fade, effect sends, key-scaled hold and decay, and the modulation LFO to volume and filter.
- At song start the pending notes loop costs up to 1,024 steps per pending note each block.
- There is no data increment or decrement (CC96, CC97).
- No test isolates the rule that protects a note's own voices when stealing. With SoundFont voices at 1, a note with two regions overshoots by one block.
- Switching a font mid-song kills the voices at once, which may click.

Effects:
- Turning Reverb and chorus off cuts the tail, which may click.
- The reverb is Freeverb, not FluidSynth's 2.x design, so the character differs.

Comparison with FluidSynth:
- The pad's spectral centroid is at plus 14.6 percent against the 15 percent allowance, because a fully open filter at Q 0 is bypassed while FluidSynth always low-passes near 19.9 kHz. A font with more top end could cross it. Keeping the filter running at the top, at unity gain, would fix it.
- A font that brings the FluidSynth form of the velocity-to-cutoff modulator now applies it.
- Levels sit about 1.5 dB under FluidSynth by the spec's filter mapping.
- A comment in sf2.c near line 51 has loose wording.

The plugin:
- An instance that is not active holds its sf_used and its channels' presets until it is activated.
- collect_now relies on the plugin's active flag following CLAP's rules.
- The path for host buffers with a NULL channel pointer has no test, because host.exe cannot produce one.
- The Sampler does not wait in an offline render, only the SoundFont player does.
- A reset from the host lets the sf_used presets go in smp_synth_reset; the plugin uses smp_synth_flush instead, which keeps them.

The window:
- Listen in REAPER: rewriting the "From the song" entry on a focused channel combo may make NVDA read it again.
- Listen in REAPER: whether NVDA speaks the new tab when Ctrl+Tab is pressed with the focus on the tab control is untested.
- The channel combos are 120 pixels wide closed (320 open), so names are truncated for a sighted helper.
- Arrowing through an open File list loads each file in turn, and big fonts load one after another.
- The File combo does not wrap at the ends.

Measured (Task 10):
- A second instance on the same font costs 2.3 MB, over the 2 MB target. It is the second player's own memory; it was not broken down yet.

## After stage 1: stretcher choice and parallel copies (2026-10-28)

Mike found keep length slow, and keys far from the root only kept length after he had played them once. He asked
for a better stretcher, and then for the stretchers to be selectable in Settings. What changed:
- Signalsmith Stretch (MIT, src\signalsmith) now runs alongside Rubber Band.
- There are settings for the stretcher and for keeping the tone.
- Every reachable key is prepared at load.
- Copies are made in parallel on a shared thread pool.

A fresh review found no critical issues. It found seven others, all fixed:
- The level bump at the end of Signalsmith copies.
- Engine free waiting on queued pool jobs.
- Memory and disk budgets that applied to each instance rather than the whole process.
- Reading the source couldn't be cancelled.
- Changing the cache limit restarted the work.
- The new combo boxes rescanned the cache folder.
- Trailing spaces in the settings file weren't ignored.

Parked:
- The thread pool is never closed, so one pool is leaked each time the DLL is unloaded.
- A note-on's copy can wait behind other instances' running jobs (queued ones are not reordered across instances).
- At speeds above 100 %, a Signalsmith copy of a tone that stops dead fades over its last few milliseconds.
