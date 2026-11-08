# Decent Sampler bundles and knobs: design

Date: 2026-11-07. Driving example: `E:\samplepacks\pianobook\Finnish Forest Percussion\Finnish Forest Percussion.dslibrary`
(freeware, no redistribution: its samples must never enter the repo or the tests).

## Why

The user wants that library, and Decent Sampler libraries like it, to play in Samplosis the way they play in Decent
Sampler: open the file they downloaded, hear the right sound at the knobs' starting positions, and move the knobs both
from the Samplosis window (screen reader, keyboard) and by MIDI CC from the DAW. Today the bundle doesn't open at all,
and its unzipped preset plays all three layers at once (every `<ui>`, `<effects>` and `<midi>` is read past).

## What the example needs (from its preset)

- A zip, every entry deflated; one `.dspreset` at the root, `Samples/{Natural,Force,Texture}/*.wav` (345), junk
  (`__MACOSX/`, `._*`, `*.reapeaks`, `.dsp_cache`) and `Images/` (knob skins, background).
- XML that isn't well formed: `translationOutputMin="-24"translationOutputMax="12"` (no space between attributes).
- 69 groups tagged `Natural`, `Force` or `Texture`, `seqMode="random"`, `ampVelTrack="1"`, release 50 ms.
- Seven `<control>`s (`custom_skin_vertical_drag`), each with `tags`, `label`, min/max/value/defaultValue, some
  `valueType`:
  - Tone (0..1, 0.5): two effect bindings, `FX_FILTER_FREQUENCY` of effect 0 (highpass) and 1 (lowpass), both
    `translation="table"`.
  - Pitch (integer, -24..24, 0): `amp` / `instrument` / `GLOBAL_TUNING`, linear.
  - Texture (0..1, 0): `amp` / `group` / `tags="Texture"` / `AMP_VOLUME`, linear 0..1.
  - Force (0..1, 0): `AMP_VOLUME` on `tags="Natural"` linear 1..0 and on `tags="Force"` linear 0..1.
  - Reverb (0..1, 0): effect `position="2"` `FX_REVERB_WET_LEVEL`.
  - Delay Mix (0..1, 0): effect position 3 `FX_WET_LEVEL`.
  - Delay Time (`musical_time`, 6..16, 13): effect position 3 `FX_DELAY_TIME`.
- `<effects>`: highpass (Q 0.7, 10 Hz), lowpass (Q 0.7, 22000 Hz), reverb (roomSize 0.9, damping 0.1, wet 0), delay
  (`delayTimeFormat="musical_time"`, feedback 0.65, stereoOffset 0.01, wet 0.5).
- `<midi>`: `<cc number="N">` with one `<binding level="ui" type="control" controlTags="..." parameter="VALUE"
  translation="linear" translationOutputMin/Max>` each: CC11 Force, CC74 Tone, CC1 Pitch (-24..12), CC17 Texture,
  CC91 Reverb, CC12 Delay Mix, CC19 Delay Time (6..16).

## Goals

1. `.dslibrary` opens wherever `.dspreset` does (Browse, File box, drop, project state), with the format word
   "Decent Sampler". A bundle with several presets is one font with one preset per `.dspreset` (sorted by entry
   name, ignoring case).
2. XML with a missing space between attributes opens (the only leniency added).
3. A preset's knobs (`<control>`, `<labeled-knob>`) are parsed with their bindings, start at their `value`, and drive
   the sound live:
   - `amp` `AMP_VOLUME` at `group` level (by `tags` or `groupIndex`/`position`) or `instrument` level: a linear gain
     factor on the regions it covers.
   - `amp` `GLOBAL_TUNING` (instrument): semitones on every voice; `GROUP_TUNING` likewise on the covered groups.
   - `effect` bindings (by `effectIndex` or `position`, 0-based into `<effects>`): `FX_FILTER_FREQUENCY`,
     `FX_FILTER_Q`, `FX_REVERB_WET_LEVEL`, `FX_REVERB_ROOM_SIZE`, `FX_REVERB_DAMPING`, `FX_WET_LEVEL` (delay or
     reverb wet), `FX_DELAY_TIME`, `FX_FEEDBACK`.
   - Translations: `linear` (output min/max default to the control's range, so "linear" alone passes the value
     through), `table` (piecewise linear between the given points, flat outside them), `fixed_value`.
   - `valueType="integer"`: the control's value is rounded to a whole number before it is translated.
     `musical_time`: an index 0..20 into Decent Sampler's list (below).
4. `<midi>` `<cc>` bindings to `level="ui"` / `type="control"` (`controlTags`, `controlIndex` or `position`) /
   `VALUE`: a CC on a channel playing the preset sets the knob: value = linear (or table) translation of
   cc/127, then the knob's own bindings apply. A CC bound this way no longer has its General MIDI meaning for this
   instrument's regions (the default SF2 modulator for that CC is left out: CC1 vibrato, CC7 volume, CC10 pan,
   CC11 expression, CC91 reverb send, CC93 chorus send).
5. The instrument's `<effects>` chain plays: `highpass`, `lowpass` (and `lowpass_4pl`, `lowpass_1pl`, `bandpass`)
   as 2-pole resonant filters (RBJ biquads; 1-pole for `lowpass_1pl`), `reverb` (Freeverb, as JUCE's Reverb
   which Decent Sampler uses: roomSize, damping, wetLevel, dry 1), `delay` (stereo, feedback, stereoOffset,
   feedbackCutoff, wetLevel added to the dry signal; delayTime in seconds or `musical_time` at the host's tempo,
   120 BPM when the host gives none). Effect types not listed stay counted as ignored; the chain skips them.
6. The knobs appear on the SoundFont page as sliders (see "Window").
7. Knob positions are saved with the project and restored.

## Non-goals

- Decent Sampler's picture (background, knob skins, keyboard colours). Images aren't unpacked.
- Knobs as host-automatable parameters: Samplosis's CLAP parameter list is fixed at 35 and changing it at run
  time is a bigger change. (Possible later.)
- `<button>`, `<menu>`, `multi_state`, `<xyPad>`, `<modulators>`, `<keyswitches>`, `<noteSequences>`, effect types
  beyond those above, `general` and `control` bindings other than knob VALUE, binding `ENABLED`: still counted as
  ignored, the rest of the preset plays.
- Decent Sampler's limiter headroom (still not copied).
- Per-channel knob values: one value per knob of each preset, per Samplosis instance.

## Anti-goals

- The kit sounding different from Decent Sampler at its default knob positions (all three layers, reverb on, an
  expression CC muting it).
- Any regression in SFZ, EXS, SF2, DLS, GigaStudio or plain `.dspreset` loading, speed or sound; presets without
  knobs or effects must render bit-identically to today.
- Allocations, locks or disk access on the audio thread.
- Unbounded disk use from unpacked bundles.

## Decided by the user

- Scope "everything": all seven controls, including a new tempo-synced delay.
- Knobs both in the window and by MIDI CC.
- Playback accepted: one value per knob shared by window and CC (last one wins, the slider follows a CC within a
  quarter second); bound CCs take over (CC11 is Force, not expression, for this kit); positions saved with the
  project, reset to the preset's values when another file loads; values read as the preset means them (Pitch in
  semitones, Delay Time as a note length, 0..1 knobs as percent); a `.dslibrary` opens directly, unpacked once into
  the cache folder with progress in Status.

## Calls made by Claude (the user may overrule)

- Knobs and effect chains belong to a preset. A bound CC on a channel moves the knobs of the preset that channel
  plays (in One preset mode, every channel plays the chosen preset, so any channel's CC moves its knobs). With one
  preset per file, the usual case, that is "any channel".
- Each preset with `<effects>` gets its own chain and stereo bus, allocated on the main thread when the font loads
  (at most 8 chains a font; presets past that play dry, counted as ignored). Voices of a preset with a chain render
  into its bus; the chain's output is added to Output 1.
- A binding sets its parameter, as Decent Sampler does: a group-level `AMP_VOLUME` replaces that group's volume
  (`<group volume>` / `groupVolume`), an instrument-level one the global volume, `GLOBAL_TUNING` replaces
  `globalTuning`, `GROUP_TUNING` the group's tuning; the sample's own volume and tuning still combine with them.
  The builder keeps the three levels apart for the regions a binding covers. Two bindings on the same target: the
  one applied last wins (at load, file order; then whichever knob moved last).
- Samplosis's Reverb and chorus switch also bypasses a preset's reverb and delay effects (its filters stay: they
  are the tone, not an added effect). A preset with a `reverb` effect doesn't send to Samplosis's shared reverb
  (its regions get no CC91 send), so it never plays through two reverbs. Presets without `<effects>` are unchanged.
- The reverb is a Freeverb inside the instrument's chain with the preset's own room size and damping (closer to
  Decent Sampler than feeding Samplosis's shared reverb, which the playback had suggested).
- The instrument's chain is on Output 1, like Samplosis's reverb and chorus: channels routed to other outputs play
  dry (README already states that rule for the effects).
- A region whose knob gain is 0 when the note starts doesn't start a voice (saves a third of the voices at the
  defaults); raising the knob during a note doesn't bring that layer into the note already sounding. Percussion
  makes this inaudible in practice.
- Unpacked bundles: `<cache>\dslib\<016llx key of "dslib1|fullpath|size|mtime">\`, written to a `.part` folder and
  renamed when complete; reopened without unpacking while it exists. They count toward the cache's used bytes and
  are trimmed oldest-first like other cache files, never while a font from them is open. Only `.dspreset` entries
  and the audio files they name are unpacked (skips `__MACOSX/`, `._*`, images, peaks).
- Unzip with vendored miniz (MIT), in `src\codecs\miniz` like the other codecs. Zip64, encryption and methods other
  than stored/deflate: the bundle doesn't open, with the reason.
- XML leniency: a pre-pass that inserts a space where a quoted attribute value is followed directly by a name
  character inside a tag; nothing else is relaxed.
- At most 32 knobs per preset (Decent Sampler presets rarely have more than a dozen), 8 bindings per knob, 8 effects
  per chain; beyond that counted as ignored. The window shows at most 16 knob sliders (the window doesn't resize).
- Control values move without zipper noise: gains and filter frequencies are ramped over the control period the
  voice already uses; the delay time change crossfades (no pitch sweep).

## Musical time

Decent Sampler's list (decentsamples.com, "How to add tempo-synced delay", 2024-02-08), index 0..20:
1/64 triplet, 1/64, 1/32 triplet, 1/64 dotted, 1/32, 1/16 triplet, 1/32 dotted, 1/16, 1/8 triplet, 1/16 dotted, 1/8,
1/4 triplet, 1/8 dotted, 1/4, 1/2 triplet, 1/4 dotted, 1/2, 1 triplet, 1/2 dotted, 1, 1/1 dotted.
(Checks: 4 = 1/32, 10 = 1/8, 13 = 1/4, 19 = 1, as the article's examples say.) A quarter note is one beat; triplet is
2/3, dotted 3/2. A non-integer index rounds to the nearest. Words in the window: "1/4", "1/8 dotted", "1/4 triplet".

## Window

- On the SoundFont page, after SoundFont transpose and before Reverb and chorus: one slider per knob of the current
  preset (One preset mode: the chosen preset; All channels mode: the knobs of every preset that has them, labelled
  "<preset>: <label>" when the font has several knobbed presets), named by `label`, else `parameterName`, else
  "Knob N". Reverb and chorus, Status and Settings move down.
- No knobs: none shown and Tab skips straight to Reverb and chorus (today's page, unchanged).
- Keys as the other sliders (arrows, Shift, Control, Page, Home/End, typing opens the edit box). Steps: integer and
  musical_time knobs step by 1; float knobs by 1/100 of their range.
- Words: a 0..1 float knob "40 percent"; a knob bound to GLOBAL_TUNING or GROUP_TUNING "plus 3 semitones";
  musical_time its note length; other floats the number with two decimals; integers the number.
- The 250 ms refresh shows CC changes; a slider being edited isn't touched.

## State

- One line per knob that isn't at the preset's `value`: `sf_knob <preset number> <knob number> <value>` (1-based,
  in file order). Written after `sf_used`. Unknown or out-of-range knobs in a loaded state are skipped silently (a
  library update may remove one). Kept and written back when the font is missing, like `soundfont` and `sf_used`.
- Loading another file (or the same file again) resets every knob to its preset's `value`. A GM/GS/XG reset, CC121
  and the host's stop/seek don't move knobs (Decent Sampler doesn't either).

## Status and ignored settings

- Opening a bundle: "Unpacking x.dslibrary: n of m files", then the usual "Loading x.dslibrary: n of m samples".
- `<ui>`, `<effects>` and `<midi>` are no longer counted wholesale; each unsupported element, attribute, binding or
  effect type inside them is counted by name. The README lists what is and isn't supported.

## Testing

- Synthetic fixtures only (Python `zipfile` writes a `.dslibrary` of generated WAVs; deflated and stored variants,
  one with several presets, one with junk entries, a damaged zip, Zip64/encrypted refusal).
- Render checks through the existing test player: layer gains at knob values and after CC; Pitch steps in
  semitones; Tone's table at a few points (filter response on noise); reverb wet 0 is dry; delay echoes at the
  musical time for a given tempo, with feedback; presets without knobs render bit-identically to before.
- GUI through host.exe: the knob sliders appear with their words, move with arrows and CC, vanish for a font
  without knobs, Tab order; state round-trip of `sf_knob`.
- The real library is used only for a manual check (open, defaults sound right, each CC moves its knob).

## Left to the builder

The data structures (where the parsed knobs, bindings and chain live: font-side immutable description, synth-side
values), how regions carry their covering gain/tuning bindings, the chain's DSP details within the above, the
unpack cache's trim integration, the multi-preset build (extending `sfz_build` versus merging builds), and the order
of the work.
