# Samplosis: design

Developer: audioskit. Date: 2026-10-28. Owner: Mike.

"Mike said" marks his decisions. "Our call" marks choices made for him that he can still change.

## What it is and why

Samplosis is an accessible sampler and SoundFont player in one CLAP instrument for REAPER. Mike is blind and works in
REAPER 7.41 with NVDA and OSARA.

It replaces two tools:
- ReaSamplOmatic5000, which he uses now. Mike said: it must do everything ReaSamplOmatic does, and more.
- BassMIDI for SoundFonts. Mike said there's nothing wrong with BassMIDI. He wants an instrument of his own, one that
  could support other formats some day.

The more Mike asked for:

1. **Notes keep their length.** ReaSamplOmatic changes pitch by changing speed. Playing a C major chord with a guitar
   sample, the G dies first, then the E, then the C. Mike said every note must last as long as the original
   recording, whatever its pitch. Loops and phrases must also be able to lock to the project tempo. Mike said: both.
2. **Many samples in one instrument.** Today Mike opens one ReaSamplOmatic per sample. Mike said that "shouldn't
   happen here".
3. **Shaping like Ableton's Sampler, but accessible.** Mike said the list below is exactly what he needs.
4. **Control from the terminal**, so an AI agent can use it fully. Mike said this is a handy rule, not a hard one.

Hard rules from Mike: it must be "blazing fast" and "memory efficient as much as can be". This covers playing, loading
and the window.

## Goals

### Loading and mapping (Mike said)

- **One sample.** It plays across the keyboard from its root note.
- **Several samples at once** (files or a folder). They map across the keyboard automatically by pitch. Mike can then
  put any sample on any note or range by hand, all in one instance.
- **Layers on a key.** Several samples on one key can be velocity layers, round robins, or both. Mike said: both.

Our calls on mapping:
- Pitch comes from a note name in the file name ("C3", "Gb4", "F#2", "A 440") when there is one. Otherwise it's
  detected from the audio (YIN, as in the skit tools).
- The detected pitch is stored with the instrument, so it's measured only once.
- Each sample's range runs halfway to its neighbours.
- Files with the same pitch but different loudness become velocity layers, ordered by measured loudness.
- Files with the same pitch and similar loudness, or names ending in a take number, become round robins.
- Mike can correct any of this afterwards.

### Shaping, after Ableton's Sampler (Mike said "exactly what I need")

Each section is a group of controls, in Tab order:

1. **Zones.** The key and velocity map, with round robins. Neighbouring zones can crossfade across their key and
   velocity edges.
2. **Sample.**
   - Start and end points.
   - Loop off, forward, or back and forth, with a loop crossfade.
   - Reverse.
   - Keep length, on or off.
   - Tempo lock, for loops, with the loop's length in beats.
3. **Pitch.**
   - Transpose and fine tune.
   - Glide.
   - Pitch envelope.
   - Pitch bend range.
4. **Filter.**
   - Low pass, high pass, band pass and notch, in 12 and 24 dB slopes.
   - Cutoff, resonance and drive.
   - Its own envelope.
5. **Amp.**
   - Attack, decay, sustain and release.
   - Volume and pan.
   - How much velocity affects loudness.
6. **Modulation.**
   - At least three LFOs.
   - A list of routings. Each one is a source, a target and an amount.
   - Sources: velocity, mod wheel, aftertouch, key position, the LFOs, the envelopes and any MIDI CC.
   - Targets: pitch, cutoff, resonance, volume, pan, sample start and the LFO rates.
7. **Voices.**
   - Polyphony.
   - Mono or poly.
   - Legato.
   - Whether a repeated note cuts off the one before it (choke). Choke groups across zones, as in a hi-hat.

All settings work at the instrument level, and any zone can override them.

### SoundFonts

- SF2 files are a goal. SF3 (sf2 with Ogg samples) is our call, since the decoder already reads Ogg.
- Two modes, switchable in the window (Mike said):
  - **One instrument.** The instance plays one preset, chosen by bank and program in the window. It listens to all
    MIDI channels or one chosen channel, so there's one instrument or track per instance.
  - **All channels.** The instance plays a whole General MIDI song, as BassMIDI does.
    - Each of the 16 channels has its own preset, set by program change and bank select.
    - Channel 10 plays drums.
    - The window lists each channel's preset, and any of them can be overridden.
    - Each channel can go to its own output.
  - In all-channels mode, the per-zone editing works on one channel's preset at a time.
- Generators, modulators and their default values follow the SF2.04 spec, so a SoundFont sounds as its author meant.
- SoundFont zones are the same zones as everything else. Mike can open them, edit them, and save them as a Samplosis
  instrument.
- Keep length and tempo lock apply to SoundFont zones as well.

### Text and terminal (Mike asked for it; the shape is our call)

1. **An instrument is plain text**, a script in the style of the Cosmos FX script box.
   - It holds every zone, sample path, mapping and setting.
   - The window and the script stay in step both ways.
   - The plugin saves its state in the REAPER project as this text. Sample paths are absolute, plus a path relative
     to the instrument file when there is one.
2. **Live control.** A command-line tool, `samplosis`, talks to running instances.
   - Each instance has a name, which Mike can set. It starts as the track name if REAPER gives one, or as
     "Samplosis 1", "Samplosis 2" and so on.
   - Commands:
     - list instances
     - show the script
     - set any setting
     - load files or a folder
     - map a sample to a key or range
     - save or load an instrument
     - list SoundFont presets
     - choose a preset
   - Every command answers in plain text.
3. **Rendering without REAPER.** `samplosis render instrument.txt song.mid out.wav` plays a MIDI file through an
   instrument with the same engine code. Our call: this also lets the skit engine use Samplosis instruments.

### The window (Mike's standing rules for his plugins)

- Plain Windows controls that NVDA reads as they are, as in Cosmos FX (gui.c).
- Tab and Shift+Tab move through every control. At the last and first control they go out to REAPER's FX window.
- If a control disappears, or REAPER focuses the plugin's own window, focus goes on to a real control.
- Fixed choices are combo boxes. Numbers stop at their limits with Up and Down. Shift moves 10 times as far and
  Control a tenth.
- Every change applies at once, with no lag.
- Labels are clear and short, and each value is read in words ("C4", "-6.0 dB", "120 ms").
- The zone list reads each zone as one line, for example "Guitar C3.wav, C3, keys A2 to D3, velocity 1 to 127,
  round robin 1 of 3".
- Automatable REAPER parameters for the instrument-level controls that matter (cutoff, the envelopes, volume and so
  on). Our call on exactly which.

## Non-goals for now

These are left out of the first version, but the design must leave room for them:
- Other formats, such as SFZ and Kontakt folders. Mike said maybe "some day". The loader keeps formats separate from
  the zone model.
- Slicing a loop to keys (Ableton Simpler's slice mode).
- Built-in effects such as reverb or chorus. REAPER and Cosmos FX already do those.
- Mac and Linux. Windows only, but nothing should rule them out.

## Anti-goals

Any of these makes it a failure even if it works:
- Any lag, a slow load, or a window "like JUCE things".
- Memory that grows with the size of the sample library instead of what's playing.
- Focus dead ends, or anything Mike has to reach with NVDA's object navigation.
- Needing one instance per sample.
- A high note that dies before a low one when keep length is on.
- Clicks or dropouts in playback.

## How it gets made (our calls)

### Code and build

- C, as a CLAP plugin, on the same base as the Cosmos plugins in skittools/cosmos_clap. That means:
  - gui.c's accessible window patterns.
  - decode.c (dr_wav, dr_flac, dr_mp3, stb_vorbis) and its Kaiser sinc resampler.
  - Rubber Band, through rb.cpp.
- Built with Visual Studio's cl, like cosmos_clap/build.bat, and installed into D:\clap.
- The code is copied into its own project, so Samplosis doesn't depend on skittools.
- It lives in E:\samplepacks\claudemusic\samplosis, as its own git repo. It is pushed to GitHub only when Mike asks.
- Rubber Band makes it GPL. Mike already accepted that for the Cosmos plugins.

### Keep length

- Our call: render, don't stretch live. When keep length is on, each key a zone covers gets its own pitch-shifted
  copy at the original length, made by Rubber Band's offline, best-quality mode in a background thread. Notes start
  with no added latency, and the quality is better than a live stretch.
- Rendered copies go to a disk cache, keyed by sample, key, settings and file date. They stream like any other sample.
- A note played before its copy is ready plays by resampling, then uses the copy from the next note on.
- Tempo lock renders the same way at the project tempo, and renders again when the tempo changes.

Further calls on keep length:
- **Switching it.** Keep length and tempo lock can each be switched on or off (Mike said). Each is a single switch
  near the top of the window, an automatable REAPER parameter, and a terminal command. The switch applies to the
  whole instrument, and any zone can override it.
- **Defaults.** Keep length is set per instrument and can be overridden per zone. It's on by default for samples
  Mike loads, and off by default for SoundFonts, whose looping zones already sustain.
- **Rendering on demand.** Copies are made when a key is first played, and when an instrument loads, for the keys
  of zones that have keep length on. A General MIDI SoundFont with keep length on renders only what is played, never
  every zone.
- **The cache** goes to a default location, D:\samplosis\cache on the internal SSD. E: is an external USB hard disk,
  and C: is nearly full.
  - Mike can choose another folder in a Settings window (Mike said).
  - The Settings window also holds the cache's size limit, 10 GB by default, with the least recently used copies
    removed first. It also has a "clear cache" button.
  - Settings are shared by every instance. They're kept in a small text file in %APPDATA%\Samplosis and can be set
    from the terminal too.
  - The Settings window follows the same accessibility rules as the main window.
- **Loops.** For a looping zone, keep length applies to the part before the loop. The loop itself sustains as
  normal.
- **Pitch that moves during a note** (bend, glide, the pitch envelope, LFOs, fine tune) is resampled from the
  rendered copy of the nearest key. Within a few semitones, the length changes by only a few percent, which is
  accepted. There is no live stretch engine.

On tempo lock:
- **What it stretches.** A tempo-locked loop is stretched to its length in beats and starts when the note is
  played, as in Ableton's Sampler. Starting in step with the song's bar position is a later option, and the design
  leaves room for it.
- **Tempo changes.** While the tempo moves (automation), the nearest finished copy is resampled to the right
  length. A new copy is rendered only once the tempo has held for about half a second.

On streaming: sample libraries mostly sit on E:, a USB hard disk. The amount held in memory per sample should be
large enough that E: never causes a dropout: about a quarter second from the SSD, more from E:. The builder sets
the exact sizes by measuring.

### Everything ReaSamplOmatic does (Mike said "everything")

Every ReaSamplOmatic5000 feature has an equivalent:
- **Modes:** sample, note (semitone shifted), and freely configurable shifted.
- **Ranges:** note range start and end, and minimum and maximum velocity.
- **Pitch:** pitch offset, pitch bend range and speed (the playback-rate setting).
- **Playback:**
  - Obey note-offs.
  - Number of voices.
  - Loop and loop start.
  - Sample start and end offsets.
- **Volume:**
  - Minimum and maximum volume.
  - Gain, with velocity to volume.
  - Attack, decay, sustain and release.
- **Probability:** the probability of hitting.
- **Files:**
  - Dragging files in, from Explorer, REAPER's Media Explorer or the arrange view, through Windows drag and drop.
  - Previous and next file in the folder.
  - Browse.

### Outputs (our call)

One main stereo output, plus up to 15 more stereo outputs. Any zone or choke group can be sent to an output, so a
drum kit can go to separate tracks. Everything goes to the main output by default.

### Memory and speed

- Our call: only the first part of each sample (about a quarter second) is held in memory. The rest streams from disk
  on a background thread into small per-voice buffers, the way big samplers do.
- Short samples, under about a second, are simply held whole.
- A SoundFont's sample data is read from the .sf2 file in place, not copied.
- The audio thread never allocates, locks or touches the disk.
- Settings changes are handed to the audio thread without locks.
- Loading reads only the file headers and the first parts, so a large instrument opens at once. Pitch detection runs
  in the background and is stored.

## Left to the builder

- The exact script syntax, as long as it's readable, round-trips with the window, and is close to Cosmos FX's.
- How the window's sections are laid out (tabs, a section list, or pages), within Mike's rules above.
- The pipe or socket and the protocol between the tool and the instances.
- Filter designs, envelope curves and LFO shapes.
- The order the work is done in. Mike should be able to try something in REAPER early. A suggested first step is a
  single sample with keep length and the accessible window.
