# Decent Sampler bundles and knobs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `.dslibrary` bundles open, and a Decent Sampler preset's knobs, CC bindings and effects chain play as in Decent Sampler, with the knobs as sliders on the SoundFont page and saved in the project.

**Architecture:** The bundle is unpacked once into the cache folder (vendored miniz) and its presets are built into one font by sfsfz.c's builder, now able to make several presets. sfds.c parses `<ui>`, `<effects>` and `<midi>` into an immutable per-preset description (`SfKnobs`, new `src/sfknob.h`) hung on the font; regions carry a layer id. The synth holds the knob values (atomic doubles), turns them into per-layer gains and tunings and effect parameters each block, and renders the voices of a preset with effects into its own bus through a new chain (`src/sfdsfx.c`). The GUI shows non-parameter sliders for the knobs; the state gets `sf_knob` lines.

**Tech Stack:** C (MSVC, Win32), yxml, miniz 3.0.2 (new, MIT), Python pytest with ctypes and tests\host.exe.

**Spec:** `docs/superpowers/specs/2026-11-07-dslibrary-knobs-design.md`

## Global Constraints

- The audio thread never allocates, locks, waits or touches the disk (sfsynth.c's contract).
- Presets without knobs or `<effects>` render bit-identically to today; every existing test keeps passing.
- No sample from the real library enters the repo or the tests (its license forbids redistribution).
- Limits: 32 knobs a preset, 8 bindings a knob, 8 effects a chain, 8 chains (knobbed presets with values) a font, 64 CC bindings a preset, 16 knob sliders in the window; beyond them counted as ignored.
- Musical time list (index 0..20): 1/64 triplet, 1/64, 1/32 triplet, 1/64 dotted, 1/32, 1/16 triplet, 1/32 dotted, 1/16, 1/8 triplet, 1/16 dotted, 1/8, 1/4 triplet, 1/8 dotted, 1/4, 1/2 triplet, 1/4 dotted, 1/2, 1 triplet, 1/2 dotted, 1, 1/1 dotted. A quarter is one beat; triplet x2/3, dotted x3/2. 120 BPM when the host gives no tempo.
- Comments and README follow the codebase's plain prose style (full sentences, units in words, no marketing).
- Build: `build.bat test`; tests: `python -m pytest tests -q`.

## Review Focus

1. A preset whose knob binds a group by `groupIndex`/`position` instead of tags, or an instrument-level `AMP_VOLUME`: the right regions change (Task 3 test `test_ds_knob_binding_targets`).
2. A bound CC arriving before any note, and CC11/CC7 at 0 on a knobbed preset that binds neither: CC11 still works as expression there (only bound CCs lose their GM meaning) (Task 4 `test_ds_knob_unbound_cc_keeps_gm`).
3. Opening the same bundle twice, and a bundle whose file changed: reopen doesn't unpack again; a changed file unpacks again into a new folder (Task 1 `test_dslib_reopen_and_change`).
4. Switching fonts while a knob CC streams and a delay tail rings: no crash, the tail stops with the font (Task 6 `test_ds_chain_font_switch`).
5. A state with `sf_knob` lines for a font that is missing, then found: the lines survive the round trip (Task 8 `test_state_knobs_missing_font`).

---

### Task 1: Bundles open (unzip, several presets, dispatch)

**Files:**
- Create: `src/codecs/miniz/miniz.c`, `src/codecs/miniz/miniz.h`, `src/codecs/miniz/VERSION.txt`, `src/codecs/miniz/COPYING` (miniz 3.0.2 amalgamation, from github.com/richgel999/miniz releases)
- Create: `src/sfdslib.c`
- Modify: `build.bat` (compile miniz like yxml; add `/I "%CODECS%\miniz"`), `src/sf2.h` (declarations), `src/sf2.c:935-964` (dispatch, before the text sniffers), `src/sfsfz.c` (builder: several presets), `src/sfds.c` (`sf_parse_ds_many`), `src/cache.c` (unpacked folders in used bytes and trim), `src/gui.c:29-30,666-667,725` (extension lists), `src/plugin.c:886-899` (unpack progress words)
- Test: `tests/test_dslib.py` (new), `tests/test_plugin.py:839,883-884` (extension lists)

**Interfaces:**
- Produces:
  - `void sfz_preset(SfzBuild *b, const char *name);` in sfsfz.c: during a feed pass, starts a new preset (bank 0, programs 0, 1, 2 ... in call order, name cut to 20 characters). A feed that never calls it gets today's single preset named after the file.
  - `int sf_parse_ds_many(SmpFont *f, const char *font_path, const char *const *presets, int n, int (*cancel)(void *), void *carg, char *why, int wcap);` in sfds.c: one font of n presets, each preset's sample paths resolved against its own .dspreset's folder (made absolute before `sfz_opcode(b, "sample", ...)`), name = preset file name without extension. Same return values as `sf_parse_ds`.
  - `int sf_is_dslib(const char *path, const uint8_t *head, size_t n);` (extension `.dslibrary`, or `PK\3\4` at the start) and `int sf_parse_dslib(SmpFont *f, const char *path, int (*cancel)(void *), void *carg, char *why, int wcap);` in sfdslib.c. Sets `f->is_ds = f->is_sfz = 1`.
  - Unpack folder: `<cache_dir>\dslib\<016llx of smp_key("dslib1|<full path lower case>|<size>|<mtime>")>\`; written as `<that>.part\` then renamed; entries kept: `*.dspreset` and files with an audio extension the decoders know; skipped: `__MACOSX/`, names starting `._`, everything else. Entry paths with `..`, drive letters or absolute paths are refused ("it holds an unsafe path").
  - Progress: `prog[1]` = entries to unpack, `prog[0]` counts them, and `prog[3]` is new: 1 while unpacking (plugin.c shows "Unpacking x.dslibrary: n of m files"), 0 after. `Loader.prog` grows to 4.
  - Reasons: "it isn't a zip file", "its zip is damaged", "it uses zip features this can't read (Zip64 or encryption)", "it has no .dspreset in it", "the cache folder can't be written".

- [ ] **Step 1: Write the failing tests** in `tests/test_dslib.py`, building bundles with `zipfile` from generated WAVs (`write_wav`, `sine` from test_sfz.py) and a `.dspreset` text (`write_ds` from test_ds.py), cache pointed per test with `point_cache` (test_cache.py):
  - `test_dslib_opens_deflated_and_stored`: for `ZIP_DEFLATED` and `ZIP_STORED`, a bundle with `Kit.dspreset` at the root and `Samples/a.wav`; `sf_open` succeeds, `presets(lib, f) == [(0, 0, "Kit")]`, a note on its key has `peak_hz` within 2 Hz of the sine.
  - `test_dslib_skips_junk`: with `__MACOSX/._Kit.dspreset`, `Samples/._a.wav`, `Samples/a.wav.reapeaks`, `Images/bg.jpg`: opens, and the unpack folder holds exactly `Kit.dspreset` and `Samples\a.wav`.
  - `test_dslib_several_presets`: `B.dspreset` and `a.dspreset` (different samples) in `Presets/`, samples in `Samples/`: presets are `[(0, 0, "a"), (0, 1, "B")]` (sorted by entry name ignoring case) and each plays its own pitch.
  - `test_dslib_reopen_and_change`: open, close, reopen: the unpacked `Samples\a.wav` mtime is unchanged; rewrite the bundle (new mtime): it opens from a different folder.
  - `test_dslib_bad`: random bytes named `.dslibrary` → "isn't a zip file"; a truncated zip → "its zip is damaged"; an entry named `../evil.wav` → "it holds an unsafe path"; a zip with only a WAV → "it has no .dspreset in it". Error text checked with `in`.
  - `test_dslib_cache_counts_and_trims`: after opening, `smp_cache_used_bytes` includes the unpacked bytes; with the font closed and a tiny cache limit, a trim removes the unpack folder; with the font open it stays.
  - Update `tests/test_plugin.py:839,883-884` so the File list includes an `i.dslibrary` dummy in its sorted place.

- [ ] **Step 2: Run them to verify they fail**
  Run: `python -m pytest tests/test_dslib.py -q`
  Expected: FAIL (bundles don't open: "it isn't a SoundFont" style reason).

- [ ] **Step 3: Vendor miniz 3.0.2** into `src/codecs/miniz` (amalgamated `miniz.c`/`miniz.h`, `VERSION.txt` with the version and URL, `COPYING` with its MIT license) and compile it in `build.bat` like yxml (`/D MINIZ_NO_STDIO /D MINIZ_NO_TIME` is fine; the zip reader uses memory or a callback reader).

- [ ] **Step 4: Builder presets.** Add `sfz_preset` to sfsfz.c: the builder keeps a list of preset starts (region index and name) per pass; `sfz_build` allocates `nphdr` presets with `rstart` from it. Per-preset state that is font-wide today (`max_sets`, round-robin sets, keyswitch masks) is already per phdr preset through `sf_finish_regions`; `cc_init` stays font-wide (first preset's wins).

- [ ] **Step 5: `sf_parse_ds_many`** in sfds.c: parse every preset's XML first (each its own `D`), then one `sfz_build` whose feed calls `sfz_preset(name)` and walks that preset's tree, for both passes.

- [ ] **Step 6: `sf_parse_dslib`** in sfdslib.c: read the zip's central directory with miniz (`mz_zip_reader_init_file_v2` on the wide path converted, or `mz_zip_reader_init_cfile` over `_wfopen`), check safety and features, unpack into the `.part` folder with progress and cancel (check `cancel` between entries and every 4 MB), keep each entry's zip modification time as the file's mtime, rename, then call `sf_parse_ds_many` with the unpacked `.dspreset` paths sorted by entry name ignoring case. The font's `path` stays the bundle's path. Wire `sf_is_dslib` into `sf2.c` before line 943's text sniffers.

- [ ] **Step 7: Cache.** In cache.c, count `<cache>\dslib\*\` folders' bytes in `smp_cache_used_bytes`, and let `smp_cache_trim` remove whole unpack folders oldest-first (by the folder's last-write time, which `sf_parse_dslib` touches on every open), skipping a folder while a font from it is open (pin by key the way `.smpc` keys are pinned).

- [ ] **Step 8: Extensions and words.** Add `.dslibrary` to gui.c's `font_exts[]`, the Browse filter and the top comment; plugin.c's loading text says "Unpacking %s: %ld of %ld files" while `prog[3]` is 1.

- [ ] **Step 9: Build and run the tests**
  Run: `cmd /c ".\build.bat test"` then `python -m pytest tests/test_dslib.py tests/test_plugin.py tests/test_ds.py -q`
  Expected: PASS.

- [ ] **Step 10: Commit**
  `git add src/codecs/miniz src/sfdslib.c build.bat src/sf2.h src/sf2.c src/sfsfz.c src/sfds.c src/cache.c src/gui.c src/plugin.c tests/test_dslib.py tests/test_plugin.py`
  `git commit -m "Decent Sampler bundles (.dslibrary): unpacked once into the cache, several presets in one font"`

### Task 2: XML with a missing space between attributes

**Files:**
- Modify: `src/sfds.c` (before `parse_xml`)
- Test: `tests/test_ds.py`

**Interfaces:**
- Produces: `static size_t ds_fix_attr_spaces(const char *in, size_t n, char *out);` (out has room for 2n bytes; returns the new length): inside a tag, outside comments, CDATA and processing instructions, a closing quote of an attribute value followed directly by a name-start character gets one space inserted. Nothing else changes.

- [ ] **Step 1: Write the failing test** `test_ds_attr_without_space`: a preset whose `<sample path="a.wav"rootNote="60" loNote="60" hiNote="60"/>` opens, its one region has key range 60..60; a value containing `"` inside single quotes (`label='a"b'`) and a comment `<!-- x="1"y -->` are left alone (the preset still opens and has its region).
- [ ] **Step 2: Run it to verify it fails**: `python -m pytest tests/test_ds.py::test_ds_attr_without_space -q` → FAIL ("its XML is damaged near line").
- [ ] **Step 3: Implement** `ds_fix_attr_spaces` and run it on the text before `parse_xml` (bundles too, since they go through the same parse).
- [ ] **Step 4: Run** `python -m pytest tests/test_ds.py -q` → PASS.
- [ ] **Step 5: Commit** `git commit -am "Decent Sampler: attributes with no space between them read as Decent Sampler reads them"`

### Task 3: Knobs, bindings, effects and CC maps parsed onto the font

**Files:**
- Create: `src/sfknob.h`
- Modify: `src/sfds.c` (tree keeps `<ui>`, `<effects>`, `<midi>` subtrees to 6 levels; new parse), `src/sf2.h` (`SfKnobs **knobs` per phdr preset in `SmpFont`; freed in `sf_free`), `src/samplosis.h` (`uint16_t layer;` in `SmpSfCond`, 0 = none; test export), `src/sfsfz.c` (internal opcodes; default modulators for bound CCs left out; no CC91 send), `src/sf2.c` (`sf_cond_defaults`: layer 0)
- Test: `tests/test_ds_knobs.py` (new)

**Interfaces:**
- Produces (`src/sfknob.h`):
```c
#define SF_KNOB_MAX 32
#define SF_KBIND_MAX 8
#define SF_FX_MAX 8
#define SF_KCC_MAX 64
#define SF_TABLE_MAX 16
enum { SF_KV_FLOAT, SF_KV_INT, SF_KV_MTIME };                       /* valueType */
enum { SF_KW_NUMBER, SF_KW_PERCENT, SF_KW_SEMITONES, SF_KW_MTIME, SF_KW_INT };   /* how the window reads it */
enum { SF_TR_LINEAR, SF_TR_TABLE, SF_TR_FIXED };
typedef struct { uint8_t kind, npts; float lo, hi, fixed; float x[SF_TABLE_MAX], y[SF_TABLE_MAX]; } SfTrans;
enum { SF_KT_GROUP_VOL, SF_KT_GLOBAL_VOL, SF_KT_GROUP_TUNE, SF_KT_GLOBAL_TUNE, SF_KT_FX };
enum { SF_FXP_FREQ, SF_FXP_Q, SF_FXP_WET, SF_FXP_ROOM, SF_FXP_DAMP, SF_FXP_TIME, SF_FXP_FEEDBACK };
typedef struct { uint8_t target, fx, param; uint16_t first_layer, nlayers; SfTrans tr; } SfKBind;  /* layers: into layer_list */
typedef struct { char label[64]; uint8_t vtype, words, nb; float lo, hi, value; SfKBind b[SF_KBIND_MAX]; } SfKnob;
enum { SF_FX_LP, SF_FX_LP1, SF_FX_HP, SF_FX_BP, SF_FX_REVERB, SF_FX_DELAY };
typedef struct { uint8_t type, musical; float freq, q, room, damp, wet, time, feedback, offset, fb_cutoff; } SfFxDesc;
typedef struct { uint8_t cc, knob; SfTrans tr; } SfKcc;
typedef struct SfKnobs {
  int nknob; SfKnob knob[SF_KNOB_MAX];
  int nfx; SfFxDesc fx[SF_FX_MAX];
  int ncc; SfKcc cc[SF_KCC_MAX];
  uint8_t bound[16];                 /* bit per CC: bound to a knob (its GM meaning is off for this preset) */
  int nlayer;                        /* regions' cond.layer runs 1..nlayer (one per enabled <group>, in file order) */
  float *layer_vol, *layer_tune;     /* per layer (index 1..nlayer; [0] unused): the group's own volume (linear)
                                        and tuning (semitones), which a group binding replaces */
  float global_vol, global_tune;     /* <groups>' volume (linear) and globalTuning, which an instrument binding replaces */
  uint16_t *layer_list; int nlayer_list;
  int has_reverb;                    /* a reverb effect: regions send nothing to the shared reverb */
} SfKnobs;
float sf_trans(const SfTrans *t, float x);          /* linear: lo + x (hi - lo) with x already 0..1 normalised by the
                                                       caller from the knob's range; table: piecewise linear, flat
                                                       outside; fixed: fixed */
double sf_mtime_beats(int index);                   /* the list above; index clamped to 0..20 */
```
  - A knobbed preset (any `SfKnobs` with knobs or effects) bakes only the sample's own volume and tuning into its regions; group and global volume and tuning come from `layer_vol`/`layer_tune`/`global_*` at play time (Task 4). Presets without knobs or effects get no `SfKnobs` and are built exactly as today.
  - Internal builder opcodes (never in SFZ files; names begin `samplosis_`): `samplosis_layer=N` (region), `samplosis_unbind_cc=N` (global, repeatable), `samplosis_no_reverb_send=1` (global). `void sfz_set_knobs(SfzBuild *b, SfKnobs *k);` hands the current preset its description (the font owns it after the build).
  - `SMP_API int smp_sf_test_knobs_text(const SmpFont *f, int preset, char *out, int cap);` (tests): one line per knob `knob <n> "<label>" <float|int|mtime> <lo> <hi> <value> <words>`, then per binding `  bind <group_vol|global_vol|group_tune|global_tune|fx> fx=<i> param=<name> layers=<a,b,...> <linear lo hi|table x:y,...|fixed v>`, then `fx <i> <type> freq= q= room= damp= wet= time= musical= feedback= offset=`, then `cc <n> knob=<k> <translation>`, then `layers <n>`; numbers `%g`. Returns the length, 0 for a preset without knobs.
  - `smp_sf_regions(...)[i].cond.layer` gives a region's layer.

- [ ] **Step 1: Write the failing tests** in `tests/test_ds_knobs.py` (synthetic preset modelled on the example: three tagged groups Natural/Force/Texture, one sample each, the seven controls with the example's exact bindings, `<effects>` and `<midi>` copied from the spec's description, including the no-space attribute):
  - `test_ds_knobs_parsed`: `smp_sf_test_knobs_text` gives 7 knobs in file order; Tone `float 0 1 0.5 percent` with 2 fx bindings, tables `0:10,0.5:10,0.8:300,1:850` and `0:250,0.25:1600,0.5:22000,1:22000`; Pitch `int -24 24 0 semitones`, `global_tune linear -24 24`; Texture `group_vol layers=3 linear 0 1`; Force two bindings `layers=1 linear 1 0` and `layers=2 linear 0 1`; Delay Time `mtime 6 16 13 mtime`; 4 fx (`hp freq=10 q=0.7`, `lp freq=22000`, `reverb room=0.9 damp=0.1 wet=0`, `delay time=0.5 musical=1 feedback=0.65 offset=0.01 wet=0.5`); cc lines for 11, 74, 1 (`linear -24 12`), 17, 91, 12, 19 (`linear 6 16`); `layers 3`.
  - `test_ds_knob_binding_targets`: bindings by `groupIndex="1"`, by `position="0"` at group level, by tags naming two groups (`tags="a,b"` covers both), and an instrument-level `AMP_VOLUME` (target `global_vol`); `GROUP_TUNING` by tags.
  - `test_ds_knob_regions`: regions' `cond.layer` is 1, 2, 3 by group; a region of a group with `volume="0.5"` in a knobbed preset has initial attenuation 0 (the 0.5 is in `layer_vol`), while the same preset text without `<ui>` and `<effects>` bakes it (attenuation 60 cB) as today.
  - `test_ds_knob_unsupported_counted`: a `<button>`, a `<menu>`, a binding `parameter="ENABLED"`, an effect `type="chorus"` and a 33rd knob are counted by `ignored(lib, f)` with names `<button>`, `<menu>`, `binding ENABLED`, `effect chorus`, `too many knobs`; `<ui>`, `<effects>` and `<midi>` themselves are no longer counted.
  - `test_ds_knob_default_mods`: in the knobbed preset, regions' modulators (`walk`/region `imod`) have no CC11 or CC1 or CC91 source; CC7 and CC10 sources remain; with a reverb effect, no CC91 send modulator. A preset without knobs keeps all of them.
- [ ] **Step 2: Run them to verify they fail**: `python -m pytest tests/test_ds_knobs.py -q` → FAIL (no export).
- [ ] **Step 3: Implement**: keep `<ui>`, `<effects>`, `<midi>` subtrees in the tree (raise `DS_LEVELS` for them only); parse controls (`control`, `labeled-knob`; `label` else `parameterName` else "Knob N"; `valueType` or legacy `type`; `value` default `minValue`), their bindings (amp/instrument/group, effect by `effectIndex` or `position`; `tags` matched against each group's `tags`, `groupIndex`/`position` against enabled groups 0-based; translation defaults to linear over the control's own range), effects in order, and `<midi><cc number>` bindings to `level="ui"` `type="control"` (`controlTags`, `controlIndex`, `position`) `parameter="VALUE"`. `words`: GLOBAL_TUNING/GROUP_TUNING-bound → semitones; mtime → mtime; float with lo 0 and hi 1 → percent; int → int; else number. Feed the internal opcodes; the builder leaves out the default modulators of `bound` CCs and the CC91 send with `samplosis_no_reverb_send`.
- [ ] **Step 4: Run** `python -m pytest tests/test_ds_knobs.py tests/test_ds.py tests/test_sfz.py tests/test_sfz_more.py -q` → PASS.
- [ ] **Step 5: Commit** `git commit -am "Decent Sampler: knobs, bindings, effects and CC maps read onto the font"` (add `src/sfknob.h`, `tests/test_ds_knobs.py`).

### Task 4: Knob values play: layer gains, tuning, CCs

**Files:**
- Modify: `src/sfsynth.c`, `src/sfvoice.c`, `src/sfvoice.h`, `src/samplosis.h`
- Test: `tests/test_ds_knobs.py`

**Interfaces:**
- Consumes: `SfKnobs`, `cond.layer`, `sf_trans` (Task 3).
- Produces (samplosis.h):
```c
typedef struct { char label[96]; int preset, knob, vtype, words; double lo, hi, value; } SmpKnobInfo;  /* value: its default */
SMP_API int smp_synth_knob_count(SmpSynth *s);                 /* main thread: knobs of the presets shown (One preset: the
                                    chosen one; All channels: every knobbed preset, labelled "<preset>: <label>" when
                                    the font has more than one), at most 16 */
SMP_API int smp_synth_knob_info(SmpSynth *s, int i, SmpKnobInfo *out);   /* 1 ok */
SMP_API double smp_synth_knob_get(SmpSynth *s, int i);
SMP_API void smp_synth_knob_set(SmpSynth *s, int i, double v);   /* clamped to lo..hi; any thread */
SMP_API int smp_synth_knob_by_preset(SmpSynth *s, int preset, int knob, double *v, int set);  /* state lines:
                                    public preset index and knob number, 0-based; set 1 writes *v, 0 reads it */
SMP_API void smp_knob_text(const SmpKnobInfo *k, double v, char *out, int cap);   /* "40 percent", "plus 3
                                    semitones", "minus 12 semitones", "0 semitones", "1/4", "1/8 dotted", "1/4 triplet",
                                    "0.25", "7" */
SMP_API int smp_knob_parse(const SmpKnobInfo *k, const char *text, double *v);   /* the same words back, or a bare
                                    number in the knob's own units; 0 when it can't */
```
  - Storage: `volatile LONG64 kval[8][SF_KNOB_MAX]` in `SmpSynth` (the font's first 8 knobbed presets, in public order) plus a change counter; `smp_synth_load_font` resets them to the new font's `value`s before publishing the font. A CC on a channel whose preset binds it sets the knob (audio thread, `InterlockedExchange64`).
  - Per block, when the counter moved: for each knobbed preset, start from `layer_vol`/`layer_tune`/`global_*`, apply every knob's bindings in knob order then binding order (each sets its target; integer and mtime values rounded first; the value is normalised by the knob's range before `sf_trans` for linear), into `float lgain[8][layers+1]` and `float lcents[8][layers+1]` (gain = layer vol x global vol; cents = 100 x (layer tune + global tune)), sized at load on the main thread (an array the font switch publishes with the font, freed with the retired font), then `smp_sfv_update` the preset's live voices.
  - `SmpSfVoice` gets `const float *lgain, *lcents` (NULL for regions without a layer); `derive` multiplies `atten_gain` by `*lgain` and adds `*lcents` to `cents0`.
  - `start_event` skips a region whose `*lgain` is 0 at note-on (next to `smp_sfv_xf_silent`).

- [ ] **Step 1: Write the failing tests** (Player from test_sfz.py, render events `"midi 176 <cc> <v> <frame>"` for CCs):
  - `test_ds_knob_layers_default_and_force`: knobbed kit with three groups on one key, sines 300/500/700 Hz for Natural/Force/Texture: a note at defaults has `tone` at 300 Hz and < -60 dB relative at 500 and 700; after `smp_synth_knob_set(Force, 1)` only 500 Hz; at Force 0.5 both 300 and 500 at -6 dB (±0.2 dB) relative to full; Texture 1 adds 700 Hz.
  - `test_ds_knob_cc_moves_knob`: CC11=127 before the note → only Force (500 Hz) sounds and `smp_synth_knob_get(Force)` reads 1.0; CC11=0 → Natural again (no muting: CC11 isn't expression here).
  - `test_ds_knob_unbound_cc_keeps_gm`: a knobbed preset that binds only CC17: CC11=0 still silences it (< -60 dB).
  - `test_ds_knob_pitch_semitones`: Pitch knob 3 → `peak_hz` is the root's ×2^(3/12) (±1 Hz); CC1=85 → value rounds to 0 (-24 + 36·85/127 = 0.094) so the pitch is unchanged (±0.5 Hz); CC1=0 → -24 semitones.
  - `test_ds_knob_replaces_group_volume`: a group with `volume="0.5"` bound to a knob at value 1 plays at full level (+6 dB against the knob at 0.5).
  - `test_ds_knob_text`: `smp_knob_text` for Force 0.4 → "40 percent"; Pitch 3 → "plus 3 semitones", -12 → "minus 12 semitones"; Delay Time 13 → "1/4", 12 → "1/8 dotted", 11 → "1/4 triplet"; `smp_knob_parse` reads each back.
  - `test_ds_knob_plain_preset_unchanged`: the plain `.dspreset` from `test_ds_mapping` renders identically (array equality) with the knob code in place versus the same preset loaded through a font without knobs — compare against a render captured from a knob-free copy of the preset text in the same test.
- [ ] **Step 2: Run them to verify they fail**: `python -m pytest tests/test_ds_knobs.py -q` → FAIL.
- [ ] **Step 3: Implement** as in Interfaces.
- [ ] **Step 4: Run** `python -m pytest tests -q -k "ds or sfz or synth or sf2"` → PASS.
- [ ] **Step 5: Commit** `git commit -am "Decent Sampler knobs play: layer volumes, tuning and bound CCs"`

### Task 5: The effects chain (DSP)

**Files:**
- Create: `src/sfdsfx.c`, `src/sfdsfx.h`
- Test: `tests/test_dsfx.py` (new)

**Interfaces:**
- Consumes: `SfFxDesc`, `sf_mtime_beats` (Task 3).
- Produces (`src/sfdsfx.h`):
```c
typedef struct SfDsChain SfDsChain;
SfDsChain *sf_chain_new(const SfFxDesc *fx, int nfx, double rate, int max_block);  /* main thread; NULL when out of memory */
void sf_chain_free(SfDsChain *c);
void sf_chain_set(SfDsChain *c, int fx, int param, float v);    /* audio thread: SF_FXP_* value (Hz, Q, 0..1, seconds or
                                                                    a musical-time index for the delay's time) */
void sf_chain_clear(SfDsChain *c);                              /* tails to silence */
void sf_chain_process(SfDsChain *c, float *l, float *r, int n, double bpm, int effects_on);  /* in place; effects_on 0
                                                                    bypasses reverb and delay (filters stay) */
SMP_API int smp_dsfx_test(const char *desc, const float *in, float *out_l, float *out_r, int n, double rate, double bpm,
                          const char *sets);   /* tests: desc one effect a line as smp_sf_test_knobs_text prints them;
                                                  sets "fx param value" lines applied before the render; mono in */
```
  - Filters: RBJ biquads (lowpass, highpass, bandpass at `q`), `lowpass_1pl` one-pole; frequency clamped to 10 Hz..0.45 rate; coefficients interpolated per 32 frames on a change.
  - Reverb: Freeverb as JUCE's `juce::Reverb` (8 combs, 4 allpasses, its tunings scaled by rate/44100, room 0.28 + 0.7 x roomSize feedback, damping 0.4 x damping, wet gain 3 x wetLevel, dry 1, width 1).
  - Delay: per channel delay line of 4 s at the rate (longer times clamped); left time = time - offset/2 (seconds) as the guide says; feedback with a one-pole lowpass at `fb_cutoff` (22000 = off); output = dry + wet x delayed; a time change crossfades the read point over 20 ms. Musical time: seconds = `sf_mtime_beats(round(time)) * 60 / bpm`.

- [ ] **Step 1: Write the failing tests** in `tests/test_dsfx.py`:
  - `test_dsfx_filters`: white noise through `lp freq=1000 q=0.7`: energy above 4 kHz at least 24 dB below below 500 Hz; `hp freq=850`: the reverse; `lp freq=22000` passes 10 kHz within 0.5 dB.
  - `test_dsfx_reverb`: an impulse through `reverb room=0.9 damp=0.1 wet=0` equals the input (dry only); with `wet=0.5` there is energy 0.5 s later (> -60 dB) and its decay from 0.2 s to 1.0 s is slower than with `room=0.2`.
  - `test_dsfx_delay_musical`: an impulse at 120 BPM through `delay time=13 musical=1 feedback=0.5 offset=0 wet=1`: echoes at 0.5 s (±1 frame) then 1.0 s at half level (±0.5 dB); at 90 BPM the first echo at 0.6667 s; `time=10` at 120 BPM → 0.25 s.
  - `test_dsfx_delay_offset_and_bypass`: `offset=0.01` puts the left echo 5 ms before the right; `effects_on=0` returns the input unchanged.
- [ ] **Step 2: Run them to verify they fail**: `python -m pytest tests/test_dsfx.py -q` → FAIL.
- [ ] **Step 3: Implement** sfdsfx.c (the audio functions allocate nothing).
- [ ] **Step 4: Run** `python -m pytest tests/test_dsfx.py -q` → PASS.
- [ ] **Step 5: Commit** `git add src/sfdsfx.c src/sfdsfx.h tests/test_dsfx.py; git commit -m "Decent Sampler effects chain: filters, reverb and tempo-synced delay"`

### Task 6: The chain in the player, driven by knobs and the host's tempo

**Files:**
- Modify: `src/sfsynth.c` (buses, chain per knobbed preset with effects, knob → `sf_chain_set`, flush/clear, retire), `src/plugin.c:546-573` (tempo), `src/samplosis.h`
- Test: `tests/test_ds_knobs.py`

**Interfaces:**
- Consumes: Tasks 4 and 5.
- Produces: `SMP_API void smp_synth_set_tempo(SmpSynth *s, double bpm);` (any thread; 120 until set; clamped 20..999). plugin.c reads `clap_process_t.transport` (when `CLAP_TRANSPORT_HAS_TEMPO`) each process call and sets it.
  - Chains are made on the main thread in `smp_synth_load_font` for each of the first 8 knobbed presets with effects, published with the font and freed with the retired font; buses (`8 x 2 x max_block` floats) are allocated in `smp_synth_new`. Voices of such a preset routed to output 1 render into its bus (with their chorus send as before, no reverb send); after the voices, each bus goes through `sf_chain_process(..., bpm, effects param on)` and is added into outputs 1. Voices routed to other outputs stay dry, as today. `flush_now` and the font switch call `sf_chain_clear`.
  - Fx-target bindings call `sf_chain_set` during the per-block knob pass; at load every fx starts from its `SfFxDesc`, then the knobs' defaults apply.

- [ ] **Step 1: Write the failing tests**:
  - `test_ds_chain_tone_knob`: kit with noise samples: Tone 0.5 is flat (±1 dB at 200 Hz and 8 kHz against a knob-free copy); Tone 0 (lowpass 250 Hz) cuts 8 kHz by 30 dB or more; Tone 1 (highpass 850 Hz) cuts 100 Hz by 12 dB or more.
  - `test_ds_chain_delay_knob_and_tempo`: Delay Mix 1, Delay Time 13, `smp_synth_set_tempo(s, 120)`: a short click note echoes at 0.5 s; at tempo 60 at 1.0 s; with the effects parameter (33) at 0 no echo.
  - `test_ds_chain_reverb_cc`: CC91=127 before the note → tail energy 0.5 s after a short note's end (> -50 dB); CC91=0 → none (< -90 dB).
  - `test_ds_chain_font_switch`: while a delay tail rings, load another font in the same Player and render again: no exception, output after the switch has no echo of the old note.
- [ ] **Step 2: Run them to verify they fail**: `python -m pytest tests/test_ds_knobs.py -q -k chain` → FAIL.
- [ ] **Step 3: Implement** as in Interfaces (Player sets param 33 to 0; these tests set it to 1).
- [ ] **Step 4: Run** `python -m pytest tests -q` → PASS (full suite: the bit-identical rule).
- [ ] **Step 5: Commit** `git commit -am "Decent Sampler effects play in the player: knobs drive them, the delay follows the host's tempo"`

### Task 7: Knob sliders on the SoundFont page

**Files:**
- Modify: `src/gui.c` (sliders `what = SL_KNOB0 - i` with `SL_KNOB0 = -2`; `g->sl` grows by 16; Tab order insert after Transpose; rows move down; rebuild in `sf_lists` when the font or mode or chosen preset changes; refresh in `sf_show`), `src/plugin.c`/`src/plugin_int.h` (`smp_plug_knob_*` wrappers that mark the project dirty), `tests/host.c` only if a new action is needed
- Test: `tests/test_plugin.py`

**Interfaces:**
- Consumes: `smp_synth_knob_count/info/get/set`, `smp_knob_text/parse` (Task 4).
- Produces: `int smp_plug_knob_count(Plug *P); int smp_plug_knob_info(Plug *P, int i, SmpKnobInfo *k); double smp_plug_knob_get(Plug *P, int i); void smp_plug_knob_set(Plug *P, int i, double v);` (main thread; set marks the state dirty).
  - Slider range: integer and mtime knobs one step per whole number; floats 100 steps over their range; arrows/Shift/Control/Page/Home/End as other sliders; Enter or typing opens the edit box (parsed by `smp_knob_parse`; a bad entry beeps). Accessible name = label, value = `smp_knob_text`. Rows: two sliders a row after Transpose (Reverb and chorus moves to the row after the last knob row).

- [ ] **Step 1: Write the failing tests** (`gui(P, tmp_path, text, actions)` with `drop`, `wait`, `read <name>`, `tab`, `key`, `state` actions as existing GUI tests use):
  - `test_gui_knobs_shown`: drop the synthetic kit (a `.dslibrary` from Task 1's helper): `read Force` → "0 percent", `read Pitch` → "0 semitones", `read Delay Time` → "1/4"; Tab from SoundFont transpose reaches Tone first and, after the seven knobs, Reverb and chorus.
  - `test_gui_knob_keys_and_edit`: on Force, Right → "1 percent"; End → "100 percent"; typing `40` Enter → "40 percent"; on Pitch typing `-3` Enter → "minus 3 semitones"; `state` contains `sf_knob 1 4 0.4` and `sf_knob 1 2 -3`.
  - `test_gui_knobs_hidden_without`: dropping a plain `.sfz` afterwards: `read Force` fails (no such control) and Tab goes from SoundFont transpose to Reverb and chorus.
  - `test_gui_knob_follows_cc`: with the kit loaded, a MIDI CC11=127 sent through the host's process (`midi` action) → within the refresh, `read Force` → "100 percent".
- [ ] **Step 2: Run them to verify they fail**: `python -m pytest tests/test_plugin.py -q -k knob` → FAIL.
- [ ] **Step 3: Implement** as in Interfaces (clear annotations of destroyed knob sliders with `ClearHwndProps`; the knob sliders are in `g->all[PG_FONT]` so `show_page` hides them with the page).
- [ ] **Step 4: Run** `cmd /c ".\build.bat test"` then `python -m pytest tests/test_plugin.py -q` → PASS.
- [ ] **Step 5: Commit** `git commit -am "SoundFont page: a Decent Sampler preset's knobs as sliders"`

### Task 8: Knob positions in the project state

**Files:**
- Modify: `src/sfsynth.c:1269-1310` (write), `src/sfsynth.c:1337-1458` (read), `src/plugin.c:258-338`, `src/plugin_int.h:22-23` (kept lines for a missing font)
- Test: `tests/test_plugin.py`

**Interfaces:**
- Consumes: `smp_synth_knob_by_preset` (Task 4).
- Produces: state line `sf_knob <preset> <knob> <value>` (1-based preset in public order and knob in file order; `%.9g`), written after `sf_used` only for knobs not at their `value`. Read after the font loads; lines naming a preset or knob the font lacks are skipped silently; the scratch validation player accepts any well-formed line. With the font missing, the lines are kept in `P->sf_knobs` and written back.

- [ ] **Step 1: Write the failing tests**:
  - `test_state_knobs_round_trip`: load the kit, set Force 0.4 and Delay Time 10 through `host` state text, save (`state` action) → lines `sf_knob 1 4 0.4` and `sf_knob 1 7 10`; load that state into a new instance → `read Force` "40 percent", `read Delay Time` "1/8".
  - `test_state_knobs_missing_font`: a state naming a missing bundle with `sf_knob 1 4 0.4`: saved state still contains the line; after the bundle is put in place and the state reloaded, Force reads "40 percent".
  - `test_state_knob_bad_lines`: `sf_knob 9 1 1` and `sf_knob 1 99 1` are skipped (state loads, Status has no error); `sf_knob x` is refused like other malformed lines.
  - `test_state_new_file_resets_knobs`: Force at 0.4, then choosing another file and the kit again → "0 percent".
- [ ] **Step 2: Run them to verify they fail**: `python -m pytest tests/test_plugin.py -q -k state_knob` → FAIL.
- [ ] **Step 3: Implement** as in Interfaces.
- [ ] **Step 4: Run** `python -m pytest tests -q` → PASS.
- [ ] **Step 5: Commit** `git commit -am "Knob positions saved with the project"`

### Task 9: README, ignored-settings text, and the real library

**Files:**
- Modify: `README.md` (formats: `.dslibrary`; the Decent Sampler paragraph; "What ... settings aren't supported": replace the `<ui>`/`<effects>`/`<midi>` sentence with what is and isn't read; the SoundFont page list gains "Knobs"; state lines gain `sf_knob`; status texts gain "Unpacking"), `src/sfds.c:1-56` (top comment), `docs/` ledger if the repo keeps one for formats.
- Test: manual.

- [ ] **Step 1: Update** README and the sfds.c top comment to match Tasks 1-8 (limits, calls from the spec's "Calls made by Claude").
- [ ] **Step 2: Full run**: `cmd /c ".\build.bat test"`; `python -m pytest tests -q` → all pass (report the count).
- [ ] **Step 3: Manual check with the real library** (not committed): through `tests\host.exe gui` with a script that drops `E:\samplepacks\pianobook\Finnish Forest Percussion\Finnish Forest Percussion.dslibrary`, waits, reads SoundFont (expect "Finnish Forest Percussion.dslibrary, Decent Sampler, 1 preset"), Status (no ignored count for the seven controls or four effects), each knob's words, and renders notes 35..57 at defaults and with CC11=127, CC17=127, CC74=0/127, CC1=0, CC91=127, CC12=127; listen-free checks: each note sounds, Force swaps the layer, Tone filters, reverb/delay tails exist. Report the results.
- [ ] **Step 4: Commit** `git commit -am "README: Decent Sampler bundles and knobs"`
