# Task 10 report: numbers, documents and install

Status: DONE_WITH_CONCERNS (one target missed: second-instance memory)

## Measurements (tests/measure.py, SoundFont section; just_t4_fixed.sf2, below-normal priority, 3 runs)
- Open: 2 ms, 1 to 3 MB private bytes, 142 presets.
- Load, warm OS file cache: program 0 11-12 ms, 24 MB; program 25 8-9 ms, 17.6 MB; 128:0 7 ms (one run 0.35 s), 9 MB. 52.4 MB of samples after all three.
- CPU, 66 voices held (33 drawbar organ notes, program 17, loops, two regions a note), 48 kHz, 256-frame blocks, 10 s: effects off 2.7 %, on 2.9 % of real time. Target under 5 %: met. Piano is not a held test (decays to under 30 voices, 3.5 %). 122 voices (61 organ notes) measured 5.0 to 5.3 %, so about 4.2 % per 100 voices; 64 notes of a layered preset can pass 5 %.
- Second instance on the same font: 2.2 to 2.3 MB more. Target under 2 MB: MISSED by about 0.3 MB. Not broken down.
- measure.py also got: atexit cleanup of its work folder (leftover runs had filled C: with 2 GB each, which broke a run; I deleted the old samplosis_measure_* temp folders) and it honours MEASURE_DIR (I ran with a folder on E:).

## Tests and install
- build.bat test: ok (old dr_flac warning only). python -m pytest tests -q: 225 passed in 91.8 s.
- install.bat: "Installed to D:\clap\samplosis.clap" (REAPER did not hold the file).

## Files changed
README.md, tests/measure.py, docs/superpowers/stage1-carryover.md (new SoundFont parked-items section from the ledger).

## README
Player tabs, File combo, SoundFont page in Tab order, modes, program fallback (from sfsynth.c lookup), outputs, effects, loading ahead, status texts, state lines, departures from SF2.04 (Task 7 final list), FluidSynth comparison, SoundFont numbers, not-yet list, updated "What it does" and "What is still to come".

## Concerns
- Second-instance memory misses the target (README says so).
- README says "sm24 ignored" and "no NRPN/data increment" from the brief and ledger, not re-verified in the code.
- Load times are warm-cache; cold-disk times not measured.
