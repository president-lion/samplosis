# SDD ledger — plan: docs/superpowers/plans/2026-10-29-samplosis-formats.md

Branch stage3-formats from stage2-soundfont 2dba8a6; plan commit c0bf7aa. Research: .superpowers/formats-research.md.
Mike approved the format list ("that seems to do it").

## Preflight scan

| Pair / task | Produces vs consumes | Found |
|---|---|---|
| T1-T2/T3/T4 | decode chain stages inserted in order | consistent |
| T2-T6/T7 | decode_memory library codecs for compressed SF streams | consistent |
| T4 | sampler loads go background (sample.c, plugin.c, gui.c) | touches stage-1 sampler; state load stays sync — consistent |
| T5-T6/T7/T9/T10/T11 | SmpSrc + streaming used by every loader | T5 must land before loaders — order ok |
| T8-T9/T10/T11 | SmpSfCond evaluated by synth, filled by loaders | consistent |
| T12 | window/docs for all | consistent |

Ruling: Samplosis stays GPL-2.0; no Apache-2.0 code (ALAC via Windows or FFmpeg) — licence choice is Mike's — cost if wrong: ALAC needs Windows/FFmpeg
Ruling: plan executed without a separate plan-review stop; Mike approved the format list and chose subagent-driven earlier — cost if wrong: rework on a branch
Ruling: tab and label stay "SoundFont" though it now plays SFZ/DLS/gig — Mike knows it by that name — cost if wrong: a rename
Ruling: sampler loads move to the background (needed for FFmpeg decodes that take seconds); project state loads stay synchronous — cost if wrong: different load timing in the window

## Tasks
Task 1: dispatched (base c0bf7aa, implementer opus)
Task 1: review: Important x2 (empty CAF succeeds with 0 frames; NaN rate passes) + minors 3-7 sent in fix round 1 (FIX_BASE 58bdbba; note HEAD also has an untrack commit for a stray report)
Task 1: fix round 1/5 (7 addressed, 0 open; commits 58bdbba..811a864)
Task 1: minor (deferred -> Task 2): float sanitising only on CAF/raw paths; dr_wav/flac/mp3 floats not sanitised; float32 1e38 not capped
Task 1: complete (commits c0bf7aa..811a864, review clean)
Task 2: dispatched (base 811a864, implementer opus; carries float sanitising for all decoders)
Task 2: review: Important x2 (unbounded allocation from untrusted headers; APE ratio gate rejects silence) + minors sent in fix round 1 (FIX_BASE 06cd2c6)
Ruling: one decoded-size ceiling of 2 GiB float for every decoder — long recordings fit, damaged headers can't exhaust memory — cost if wrong: a >1.5 h stereo file refused
Task 2: minor (deferred): no tests for >2ch Opus, mono/8-bit/multichannel WavPack, 24/8-bit APE, .wvc hybrid, DSD; AVX512 path in Monkey's untested on older CPUs; opus USE_ALLOCA stack depth on worker threads
Task 2: fix round 1/5 (4 addressed, 0 open; commits 06cd2c6..e3f3399)
Task 2: minor (deferred): ~4 GB transient peak near the 2 GiB ceiling (planar copy; mono could skip); grow_hint may reserve up to 2 GiB commit for lying headers on 8 MB+ files; 0.25 s damage-test bound may flake; slurp adds compressed bytes to the peak; cancel only before read
Task 2: complete (commits 811a864..e3f3399, review clean)
Task 3: dispatched (base e3f3399, implementer opus)
Task 3: review: Important x2 (blocking MF read can hang the loader; mid-stream Lock failure drops audio silently) + minors sent in fix round 1 (FIX_BASE e4db029)
Ruling: MF decodes run on an abandonable helper thread with a deadline (60 s or 4x duration) — "never hangs" — cost if wrong: a leaked thread after a wedged decoder
Task 3: minor (deferred): end padding not trimmed when Windows marks the delay; format change mid-stream truncates silently; iTunSMPB-only files keep their delay
Task 3: fix round 1/5 (6 addressed; new Important: abandoned helper can outlive DLL; commits e4db029..1b780a5)
Task 3: fix round 2 sent (FIX_BASE 1b780a5): pin module per helper; stall-timer deadline; mf_lock pile-up
Task 3: note for Task 4: sample.c calls decode_audio_ex with no cancel on the main thread — Task 4's background loading must route through a cancellable worker
Task 3: fix round 2/5 (3 addressed, 0 open; commits 1b780a5..bec690f)
Task 3: minor (deferred): BUSY message not gated on Windows extensions; a stuck MF start makes every later Windows-stage load fail after 5 s for the process life; unload test timing margin 1.5 s
Task 3: complete (commits e3f3399..bec690f, review clean)
Task 4: dispatched (base bec690f, implementer opus)
Task 4: review: Important x1 (FFmpeg-not-installed message for every unreadable file) + minors sent in fix round 1 (FIX_BASE 037fba7)
Ruling: FFmpeg timeout is a 60 s stall timer (no output), not 4x duration — matches Task 3 — cost if wrong: a very slow FFmpeg decode that stalls 60 s fails
Ruling: project state loads stay synchronous even uncached (may wait on FFmpeg up to 60 s per file at project open) — plan keeps projects opening with their samples — cost if wrong: a slow project open with exotic uncached files
Task 4: minor (deferred): request_callback called under the Loader lock (fine for REAPER); destroy that gives up after 2 s untested
Task 4: fix round 1/5 (9 addressed, 0 open; commits 037fba7..3e9893e)
Task 4: minor (deferred -> Task 12): failed window load status should say "Can't load x: why. Still playing y."; status hides keep-length progress after a failed load; 200 ms post-exit drain could cut a tail
Task 4: complete (commits bec690f..3e9893e, review clean)
Task 5: dispatched (base 3e9893e, implementer opus)
Task 5: review: Important x1 (mode-3 release underruns in real time) + minors 2-7 sent in fix round 1 (FIX_BASE 213d8dc)
Ruling: second instance +16% with a note playing (one 64 KB ring) accepted under the 10% rule's intent (idle +6.8%/+8.2%) — cost if wrong: 64 KB per streaming voice
Task 5: minor (deferred): starts past the head fade in late (need start heads before SFZ offsets — Task 9); short loops re-read per lap; test_font_switch_while_playing flaky (stage 2); SF3 tolerance -30 dB; cold USB 64-voice measurement -> Task 12
Task 5: fix round 1/5 (7 addressed + bounded collect, 0 open; commits 213d8dc..904ef1a)
Task 5: minor (deferred, final triage): look-ahead can miss a gap just before the post-loop head (hard cut during an underrun); quarter-ring look-ahead cap clicks >43x real-time pitch; post-loop head kept for all streaming looped samples (up to 2x head memory on odd fonts); one event leaked per foreign thread; sampler's stream.c plays failed reads as silence
Task 5: complete (commits 3e9893e..904ef1a, review clean)
Task 6: dispatched (base 904ef1a, implementer opus)
Task 6: review: Important x3 (pack decode freezes main thread on close; pack decode memory ~2x reported; pack failure after open is silent) + minors sent in fix round 1 (FIX_BASE 72c67fb)
Ruling: pack decodes are made cancellable (cancel hook in grow_need, chunked payload read) rather than a non-waiting close — keeps the free protocol and thread lifetime — cost if wrong: none expected
Task 6: minor (deferred): one shared loader thread — a pack decode delays other instances' preset loads; SF1 only tested with a hand-made bank (ask Mike for a real .sbk)
Task 6: fix round 1/5 (8 addressed, 0 open; commits 72c67fb..34ef527)
Task 6: minor (deferred): no cache folder at first use makes the pack BROKEN for the session (reopen fixes); pack unpack holds the shared loader (other fonts wait up to ~10 s)
Task 6: complete (commits 904ef1a..34ef527, review clean)
Task 7: dispatched (base 34ef527, implementer opus)
Task 7: review: Important x2 (Giga files through DLS loader can play compressed data as noise; DLS connections stack on SF2 defaults unless exact match) + minors sent in fix round 1 (FIX_BASE 23a6286)
Ruling: region articulation replaces the instrument's whole (usual reading of the DLS text), not Wine's per-connection overlay — cost if wrong: a DLS bank relying on overlay loses instrument-level connections in regions with their own articulation
Ruling: DLS sustain read as 1000 - level cB (Wine and SpessaSynth agree) — cost if wrong: up to 4 dB at mid sustain levels
Task 7: minor (deferred): region modulator lists not deduplicated; MSB/LSB banks collapse to MSB
Task 7: fix round 1/5 (9 addressed, 0 open; commits 23a6286..d10d535)
Task 7: complete (commits 34ef527..d10d535, review clean)
Task 8: dispatched (base d10d535, implementer opus)
Task 8: review: Important x2 (release triggers under legato pedalling; note-off/pedal-up scans all regions) + minors sent in fix round 1 (FIX_BASE 1bd9dbc)
Ruling: keyswitch keys silent (brief's rule) limited to sw_lokey..sw_hikey plus sw_last keys — sfzformat doesn't say silent; sfizz may play them — cost if wrong: a region mapped on a keyswitch key is silent
Ruling: release regions reuse the note-on random draw (pairs release with attack variant) — cost if wrong: libraries with independent release random bands
Task 8: carry to Tasks 9-11: seq_position default 1 when seq_length set; fill half-given sw/xf ranges with SFZ defaults; SFZ regions shouldn't get SF2's vel->filter default; looping release regions released at once (revisit with loop_mode); sw_default field exists
Task 8: fix round 1/5 (8 addressed, 0 open; commits 1bd9dbc..86d9bd9)
Task 8: minor (deferred): lokey>127 forces a preset to unindexed scan; retired round-robin counter arrays kept until synth free (bounded)
Task 8: complete (commits d10d535..86d9bd9, review clean)
Task 9: dispatched (base 86d9bd9, implementer opus; carries Task 8 loader notes and start heads for offsets)
Task 9: review: Critical x1 (polyphony counts stereo halves; note_polyphony=1 plays right channel only) + Important x2 (probe decodes/blocks main thread; probe cost for big libraries) + minors sent in fix round 1 (FIX_BASE 8878f13)
Ruling: SoundFont-page loads move to the background worker like the sampler's (Task 4 pattern), plus a probe cache — big SFZ libraries on USB must not freeze the window — cost if wrong: more plumbing in plugin/gui
Ruling: SFZ resonant filters keep unity passband (sfizz/sforzando); SF2 keeps SF2.04 — cost if wrong: SFZ filter levels differ from other players
Task 9: fix round 1/5 (11 addressed + 2 checks; new Important: odd local error gives up the whole drive; commits 8878f13..aa5f666)
Task 9: fix round 2 sent (FIX_BASE aa5f666): root give-up only for network/not-ready on remote/removable; probe-cache last entry; minors 3-9
Task 9: minor (deferred): a follower whose leader was stolen alone isn't counted by polyphony
Task 9: fix round 2/5 (9 addressed, 0 open; commits aa5f666..b0dca64)
Task 9: minor (deferred): test hooks compiled into production (inert); probe-cache reads don't touch mtime (trimmed first); one decode problem fits in status; unreachable share up to 4 timeouts
Task 9: complete (commits 86d9bd9..b0dca64, review clean)
Task 10: dispatched (base b0dca64, implementer opus)
Task 10: review: Important x2 (envelope defaults — 1 ms release vs DS's likely 0.1 s; loopEnabled default) to verify against the DS guide + minors sent in fix round 1 (FIX_BASE 91b1714)
Task 10: minor (deferred): random/true_random can repeat; <ui> knob values not applied; .dslibrary zips not read; no DS limiter headroom
Ruling: DS envelope and loopEnabled defaults stay as implemented — the official guide gives no defaults; the voice's -7200 tc release floor (~15 ms) already prevents note-off clicks — cost if wrong: presets relying on DS's real defaults differ (few lines to change)
Task 10: fix round 1/5 (7 addressed, 0 open; commits 91b1714..9d6c7c3)
Task 10: minor (deferred): seqLength inference counts samples with explicit seqLength (drop of !len); only negative dB releaseTriggerDecay tested; tag/region cap messages untested
Task 10: complete (commits b0dca64..9d6c7c3, review clean)
Task 11: dispatched (base 9d6c7c3, implementer opus)
Task 11: review: Critical x3 (libgig Region heap overflows; file-sized allocations of GBs; SEH guard semantics) + Important x4 (NULL dimension lookups; uncancellable decode open; free under g_lock; 8.3 path fallback) + minors sent in fix round 1 (FIX_BASE e18add0)
Ruling: keep the SEH last-resort guard but refuse all later Giga loads after a trip and tell Mike to save and restart REAPER — cost if wrong: one damaged .gig disables GigaStudio until restart
Ruling: rt_linear field added to SmpSfCond for Giga's linear release-trigger decay — accepted — cost if wrong: one more struct field
Task 11: fix round 1/5 (11 addressed; new Important: ScanCompressedSample underflow now allocates 256-512 MB via the length rule; commits e18add0..7be06eb)
Task 11: fix round 2 sent (FIX_BASE 7be06eb)
Task 11: minor (deferred): SEH unwind may still run std container destructors in parse() on an untrusted heap; Script::Script name resize unbounded but unreachable
Task 11: fix round 2/5 (4 addressed, 0 open; commits 7be06eb..585139b)
Task 11: complete (commits 9d6c7c3..585139b, review clean)
Task 12: dispatched (base 585139b, implementer opus; includes a real free SFZ library check and gm.dls)
Task 12: complete (commits 585139b..fc639b4, review clean)
Task 12: minor (deferred -> final): double full stop before "Still playing"; two statuses disagree during a load; measure.py defaults to %TEMP% on C:; measure.py leaves 1 GB on E: if a run fails; README run counts not reproducible from one invocation; README wording on streaming/second-instance (17% over the 10% rule)/AC-3 machine-dependent; untested note expiry paths
Task 12: OPEN TARGET: cold-disk 64-voice streaming underruns (up to 1109 in first 0.8 s) — Review Focus 1 missed -> final fix wave
Final review: with fixes — Important: streamer order starves near-dry voices (root of cold-disk target); uncancellable member decodes freeze close; SF decode cache ignores the cache limit (and MAX_OPEN 4096); offline render can start notes late while a big font loads; gig parse cost at project open (full scan, main thread); one loader thread serialises heavy decodes. Minors: unbounded waits on dead shares; source_keys on main thread; double full stop; POLL_MS comment; ffmpeg search on page switch. Ledger fix-before-merge: Task 12 minors, flaky font-switch test, look-ahead gap before post-loop head.
Final fix wave: dispatched (FIX_BASE fc639b4, opus)
Final fix wave: complete 0cc63d0..9e13280 (409 tests; cold-disk target improved, not met; 4 streaming options built but off)
Final re-review: all 14 addressed; new Important x2 (decode job queued for closing font -> use after free, sffont.c:693; unvalidated .gigp parse cache can crash, gigshim.cpp:1168); Minor: collect sync may never settle under heavy streaming; out-of-scope: FIFO worker starves second font, set_problem race, gig upper-bound lengths.
Ruling: residual fix pass for both Importants, the collect-sync minor and the set_problem race - a use-after-free and a damaged-file crash break Mike's hard rules, and each fix is small and precise - cost if wrong: one more short agent run.
Ruling: worker queue fairness (round-robin per font, hinted jobs first) deferred to next stage - it causes delayed sound, not a crash or freeze - cost if wrong: a second instrument can stay silent for a while behind a big decode backlog.
Ruling: cold-disk 64-voice target recorded as missed; longer heads/rings offered to Mike as an opt-in setting - memory cost is his call per his hard rules - cost if wrong: brief dropouts on big chords from cold USB disks.
Residual fixes: complete 4a4c7fa (414 tests, installed). Final review closed.
