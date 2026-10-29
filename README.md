# Samplosis

An accessible sampler for REAPER, written in C as a CLAP instrument for Windows. The window is made of plain Windows
controls that NVDA reads as they are. Licence: GPL version 2 (see COPYING). It includes Rubber Band (GPL) and
Signalsmith Stretch (MIT, see src\signalsmith).

## What it does

Samplosis has two players in one plugin, chosen by a tab: the Sampler, which plays one sample across the keyboard
(stage 1), and the SoundFont player (stage 4, see "The SoundFont player"). A new instance starts on the Sampler, and
a project saved before the SoundFont player existed loads as it was.

The Sampler:

- Load one audio file (wav, flac, ogg, mp3 and the other formats the bundled decoders read) with the Browse button,
  by dragging it onto the window, or from a saved REAPER project.
- The root note comes from a note name in the file name ("Guitar C3.wav", "Pad F#2.flac") or, if there is none,
  from pitch detection on the audio.
- Notes keep their length. With Keep length on, a note played above or below the root is made by a time stretcher
  so it lasts as long as the recording, instead of playing faster or slower. A chord of a guitar note then rings out
  evenly. The stretcher is chosen on the Settings page: Signalsmith Stretch (the default, fast) or Rubber Band (the
  R3 engine, slower), either one keeping the tone of the instrument or not.
- The pitch-shifted copies are rendered in the background, one per semitone, several at once (as many as the
  processors allow, less two, up to 16), and kept in the cache folder. When a sample loads, every key in the key
  range gets its copy, nearest the root first, as long as the copies fit in a quarter of the cache size limit
  (shared out between the instances; an octave each way always fits). A key played before its copy is ready goes to the front of the queue. Until a copy
  is ready the note plays by changing speed, as an ordinary sampler would.
- With Keep length off, pitch changes by changing speed.
- Envelope, loop, start and end points, reverse, speed, velocity response, key and velocity ranges, probability,
  pitch bend and polyphony (up to 64 voices, 16 by default).
- Glide: a note slides in from the pitch of the note before it.
- Every control is also a REAPER parameter you can automate.
- The whole instrument can be read and written as plain text (see "The script format").

## The window, in Tab order

Sampler page:

0. Player: the tab control (Sampler or SoundFont), first on both main pages (see "The window: the Player tabs").
1. Sample: the loaded file, its length and its root note and where it came from. Read only.
2. Browse button.
3. File: a combo box of the audio files in the loaded sample's folder (see "The File combo box").
4. Keep length (on or off).
5. Mode.
6. Then the other parameters in the order of the list below, from Root note to Glide mode (the window lays them out
   in two columns, but Tab follows the list order), each one a slider (or a combo box for choices).
7. Status: for example "Keep length: 10 of 25 copies ready", or the reason a file did not load.
8. Settings button.

Settings page:

1. Stretcher: Signalsmith Stretch, fast, or Rubber Band, slower.
2. Keep the tone: off or on. On keeps the body of the instrument where it was, so a note shifted up doesn't go
   thin and a note shifted down doesn't go boomy.
3. Cache folder (edit box), then Browse folder.
4. Cache size limit (in gigabytes), a slider.
5. Cache in use (read only).
6. Clear cache button.
7. Back button.

Keys: Tab and Shift+Tab move through the controls and, past the first or last, out to REAPER's FX window.

Every value is a slider, and the screen reader reads its value in words ("-6.0 dB", "120 ms", "C3"). On a slider:

- Up or Right steps it up, Down or Left steps it down: a semitone, a cent, a dB, 10 ms, 1 percent, 1 GB. Shift
  steps ten times as far, Control a tenth.
- Page Up and Page Down step ten times as far. Home and End go to the lowest and highest values.
- It stops at its limits.
- Enter turns the slider into an edit box in the same place, holding the value, to type a value in words. Typing a
  character on the slider does the same, starting with that character. In the box, Enter sets the value and
  Escape leaves it as it was; either one turns the box back into the slider. A value that doesn't read beeps, and
  the old value stays. Tab, or leaving the box another way, sets the value too.

Choices are combo boxes. Every change applies at once.

## The window: the Player tabs

The first control on both main pages is a tab control named Player, with two tabs: Sampler and SoundFont.

- Left and Right switch tabs. They stop at the ends.
- Ctrl+Tab and Ctrl+Shift+Tab switch from any control on a main page, including a slider's edit box. They do nothing
  on the Settings page.
- The tab you choose is what plays. It is saved with the project (`plays sampler` or `plays soundfont`). The window
  opens on the tab that plays, and follows it when it changes some other way, such as a project loading.
- Drops load by file type: a .sf2 or .sf3 loads into the SoundFont player and switches to it; any other audio file
  loads into the Sampler and switches to it.
- Each page's Browse loads only its own kind. On the SoundFont page a .wav gives an error and a beep and the font
  stays; on the Sampler page a .sf2 does the same.
- Settings is one page for both. Back returns to the main page that plays.

### The File combo box

File replaces the old Previous file and Next file buttons, on both main pages. It comes right after Browse.

- It lists the files in the loaded file's folder of that page's kind (audio files on the Sampler page, .sf2 and .sf3
  on the SoundFont page): names only, alphabetical, ignoring case. The loaded one is selected.
- Choosing a file loads it at once. Up and Down on the closed box step through the folder and load each file as you
  go; they stop at the ends and do not wrap. Home and End go to the first and last.
- With nothing loaded it holds one entry, "No file loaded", and choosing does nothing. A file that has gone shows as
  "<name> (missing)".
- A file that fails to load gives a beep, the reason in Status, and the box goes back to the loaded file.
- The list is rescanned when the box gets the focus, and rebuilt when the loaded file changes. Arrowing through a list
  of big fonts loads each one in turn.

## The SoundFont player

It plays a SoundFont 2 (.sf2) or SoundFont 3 (.sf3, Ogg compressed) file on 16 MIDI channels, as a General MIDI
player does. Notes, program changes, controllers, pitch bend, pressure, sustain, RPN tuning and bend range, and the
GM, GS and XG reset messages all work. Samples load in the background for the presets in use, and a font shared by
several instances is kept in memory once.

The SoundFont page, in Tab order:

1. Player (the tab control).
2. SoundFont: the loaded font and its preset count, read only, like "gm.sf2, 142 presets".
3. Browse button.
4. File: the font files in that folder.
5. Mode: All channels or One preset.
6. In One preset mode: Preset (every preset of the font, like "Piano, bank 0 program 1"; programs are shown 1 to 128),
   then Channel (All channels, or 1 to 16).
7. In All channels mode: for each of the 16 channels, a Channel N combo and then a Channel N output combo.
   - The channel combo's first entry is "From the song (now <preset>)": it follows the program changes the song sends,
     and the words in brackets change as it does. The other entries pin the channel to one preset, which then ignores
     the song's program changes.
   - The output combo is Output 1 to Output 16.
8. SoundFont volume (minus 60 to plus 12 dB), SoundFont voices (16 to 1024, default 256) and SoundFont transpose
   (minus 24 to 24 semitones): sliders, with the same keys as the Sampler's.
9. Reverb and chorus: On or Off.
10. Status.
11. Settings button.

The controls of the mode you are not in are hidden, and Tab skips them. Every choice applies at once.

### Modes

- All channels: a song or a keyboard on any of the 16 MIDI channels plays through that channel's preset. Each channel
  follows the song's bank and program changes, unless you pinned it. Channel 10 plays drum kits (bank 128).
- One preset: every note, on every channel (or only the channel you choose), plays the one preset you picked. This is
  for playing one sound from a keyboard.

### Program fallback

A bank or program the font doesn't have still sounds:

- A melodic preset falls back to the same program in bank 0, and if that is missing too, to the font's first preset.
- A drum kit (channel 10, or bank 127 or higher) falls back to the same kit number in bank 128 (as XG numbers its kits),
  then to bank 128 program 0, then to the font's first preset.
- GS and XG bank numbers therefore play the font's nearest sound instead of staying silent.

A pinned or chosen preset the font lacks shows in the lists as "Bank b program p (not in this font)".

### Outputs

The plugin has 16 stereo audio outputs. Each channel goes to the output you gave it; the default is Output 1, the main
one. Channels on outputs other than 1 are dry: reverb and chorus apply only to Output 1. If the host provides fewer
outputs, those channels play on Output 1. In REAPER, turn on the extra outputs in the plugin's routing.

### Reverb and chorus

A built-in reverb and chorus, driven by the song's CC91 and CC93 sends as General MIDI defines. The Reverb and chorus
switch turns both off. The power-on reverb send is 40 (General MIDI level 2). The reverb is a Freeverb design (not
FluidSynth's, so its character differs) and the chorus depth is 8 ms. SoundFont volume applies to the effects too.

### Loading ahead

The state keeps an `sf_used` line: the presets in use when it was saved. They load as soon as the project loads, so the
first notes aren't late. A program change right before the first notes loads that preset at once: in real time the
first notes can be late by the load time (a note that waits more than a second of real time is dropped). A preset's
samples are freed 2 seconds after no channel needs them.

Renders to file. An exact render relies on REAPER telling the plugin it renders offline: a call to the CLAP render
extension's set with the offline mode (clap.render) before the render starts. Then the player waits for
its samples at every block and every note, so each note starts on its frame. A host that renders faster than real
time without saying so gets a real-time render played fast: the first notes after a program change may be late (by
the load time), but they are never lost unless loading takes more than a second of real (wall-clock) time. If the
font file disappears while a project is open, an offline render doesn't wait for it: the samples already loaded
play, and the ones it can't read are silent (the player keeps trying to load them every half second).

### Status texts

- "Ready: gm.sf2, 142 presets"
- "Loading samples for 3 presets" ("1 preset" when it is one)
- "No SoundFont loaded"
- "Missing: <path>": the file in the project is gone. The state keeps its path and settings, so the project still
  saves and comes back right if the file returns.
- "Can't load the SoundFont: <reason>": a damaged or unsupported file. It never crashes anything.

### The SoundFont state lines

REAPER saves these in the project, after the Sampler's lines, one per line:

    plays soundfont
    soundfont "D:\fonts\GM.sf2"
    sf_mode all
    sf_preset 0 0
    sf_channel all
    sf_channel_preset 3 0 25
    sf_output 3 2
    sf_used 0:0 0:25 128:0
    sf_volume 0.0 dB
    sf_voices 256
    sf_effects on
    sf_transpose 0 st

- plays: sampler (the default) or soundfont.
- soundfont: the path in quotes.
- sf_mode: all or one. sf_preset: bank and program (0 to 127) for One preset mode. sf_channel: all, or 1 to 16.
- sf_channel_preset N bank program: pins channel N. No line means "from the song". Bank 128 is the drum bank.
- sf_output N M: channel N goes to output M. No line means output 1.
- sf_used: the presets in use when saved, as bank:program.
- The last four are the four SoundFont parameters, which REAPER can automate.

A line that doesn't read refuses the whole state ("line N: ...") and nothing changes. Program numbers in the state are
0 to 127; the window shows 1 to 128.

### Deliberate departures from SF2.04

Samplosis follows the SoundFont 2.04 specification, and matches FluidSynth 2.6.1 only where the spec is unclear. These
are the differences from the spec's text:

- The volume release is at least 16 ms (minus 7200 timecents), so a note-off never clicks. This is FluidSynth's limit.
- Decay and release fall 96 dB over their time, and a voice ends 96 dB down. The spec's "100 percent change" doesn't say
  over what range.
- A modulator source at 127/128 stays 127/128 instead of being remapped, in the bipolar case too (FluidSynth's reading).
  CC10 at 127 pans to 496 of 500.
- The default modulator from velocity to filter cutoff is present but its amount is 0. The 2.04 value is widely treated
  as an erratum (it changed between versions), and FluidSynth disables it.
- The default modulator from CC10 to pan has an amount of 500, not 1000. The spec's 1000 contradicts its own statement
  that 0 to 127 spans left to right.
- CC91 (reverb send) powers on at 40, the General MIDI level 2 value. FluidSynth's is 0.
- Pitch bend is exactly 200 cents at the default two-semitone range. FluidSynth gives 198.4.

### How it compares with FluidSynth 2.6.1

tests\test_sf_reference.py renders 11 presets, a velocity and controller sweep, a pan sweep and a 20 second song on
E:\samplepacks\soundfonts\just_t4_fixed.sf2 with both players (effects off) and compares them. FluidSynth starts
notes on 64-sample boundaries, so each note is aligned by cross-correlation first.

- Level: Samplosis is 1.5 to 1.6 dB quieter than FluidSynth on every preset (the closed hat 1.2 dB), and 1.5 to 1.8 dB
  in the song's one-second windows. That is the 1.5 dB FluidSynth adds with its always-on filter, which the spec's
  filter doesn't have. The test allows 2 dB.
- Velocity and controllers agree within 0.1 dB (velocity 30: minus 20.92 against minus 20.90, velocity 127: plus 4.15
  against plus 4.10, CC7 and CC11 likewise). Pan agrees (CC10 at 127: plus 44.24 against plus 44.04 dB).
- Pitch is within 0.05 cents on every preset. Release times agree within one step (the pad's tail to minus 60 dB:
  3.209 s against 3.212 s).
- Spectral centroid is within 3 percent on most presets. The pad (plus 14.6 percent, against a 15 percent allowance)
  and the closed hat (plus 7.2 percent) are brighter in Samplosis, because a fully open filter is a wire here and
  FluidSynth low-passes at about 19.9 kHz. A font with more energy above 15 kHz could cross the allowance.
- The reverb and chorus are not compared.

## The parameters

Values are read and typed in words. Ranges and defaults in brackets. These are the Sampler's 31 parameters; the four
SoundFont ones (SoundFont volume, SoundFont voices, Reverb and chorus, SoundFont transpose) are REAPER parameters 32
to 35 and are described with the SoundFont page.

- Root note: the note the sample sounds at, like C3 (default C4).
- Transpose: whole semitones, "+3 st" (minus 48 to plus 48, default 0).
- Fine tune: "20 cents" (minus 100 to 100, 0).
- Volume: "-6.0 dB" (minus 60 to plus 12; "off" at the bottom, 0 dB).
- Pan: "center", "30 left", "50 right" (100 left to 100 right).
- Attack, Decay, Release: "120 ms" (0 to 10000, release 30 by default).
- Sustain: dB under full level (minus 60 to 0, 0).
- Velocity sensitivity: percent (0 to 100, default 100).
- Minimum volume: the quietest a soft note can get (minus 60 to 0 dB, off by default).
- Mode: sample, note or free. Note (the default) pitches each key from the root note. Sample plays every key at the
  recorded pitch. Free spreads a pitch range over a key range (see the next four).
- Lowest note and Highest note: the keys that sound (default all, C-1 to G9). In free mode they are also the ends of
  the pitch range.
- Pitch at lowest note and Pitch at highest note: semitones (minus 48 to plus 48, default minus 12 and plus 12) used
  in free mode.
- Lowest velocity and Highest velocity: 1 to 127.
- Obey note off: on by default. Off lets the whole sample play out when the key is released, as a one-shot.
- Loop: off, forward or pingpong. Loop start: percent into the selected region.
- Start and End: percent of the sample played (0 and 100).
- Bend range: semitones of pitch bend (0 to 48, default 2).
- Reverse: off or on.
- Keep length: off or on (on by default).
- Voices: 1 to 64, default 16. Playing more steals the oldest.
- Probability: percent chance a note plays (default 100).
- Speed: 25 to 400 percent (default 100). With Keep length on this changes speed without changing pitch.
- Glide: how long a note takes to slide from the pitch of the note before it to its own, "off" or "120 ms" (0 to
  10000, off by default). The slide is even in semitones. It starts from wherever the last note is, even part way
  through its own glide.
- Glide mode: always (the default) or legato.
  - Always: every note glides from the one before.
  - Legato: a note glides only when another key is still held, so playing detached notes doesn't glide. With Voices
    at 1, a key pressed while another is held doesn't start the note again: the note that is sounding carries on
    (no new attack) and slides to the new key, and letting go of that key slides it back to the key still held,
    as on a mono synth. This works with Glide off too: the pitch then jumps.
  - With Keep length on, a gliding note plays the copy made for its own key, and the glide changes its speed a little
    while it slides. A note carried on in legato keeps the copy of the key it started on.

## The script format

The instrument is plain text, one thing per line. Blank lines and lines starting with # are ignored. After the optional
sample line, each parameter is `name value-in-words`. Names are the lower case ones used by the engine (root,
transpose, fine, volume, pan, attack, decay, sustain, release, velocity_sens, min_volume, mode, note_lo, note_hi,
pitch_lo, vel_lo, vel_hi, obey_note_off, loop, loop_start, start, end, bend_range, pitch_hi, reverse, keep_length,
voices, probability, speed, glide, glide_mode). The sample line has its path in quotes (backslashes literal) and the 16 hex digits of the
sample's cache key (the key below is only an example). Example, shortened:

    # Samplosis 0.1.0
    sample "D:\guitar\Guitar C3.wav" key=0123456789abcdef
    root C3
    volume -6.0 dB
    release 250 ms
    mode note
    keep_length on
    voices 8

A project saved with this plugin starts with `plays sampler` or `plays soundfont`, and has the SoundFont lines after
these (see "The SoundFont state lines").

A script with an error changes nothing, and the error names the line ("line 4: ..."). REAPER saves this text in the
project, so a project reloads the same instrument.

## Settings

Settings live in `%APPDATA%\Samplosis\settings.txt`, one `name value` per line, and are edited on the Settings page.

- Cache folder: default `D:\samplosis\cache`. Holds the decoded copy of each loaded file, the reversed copy and the
  keep-length copies. Everything in it can be rebuilt.
- Cache size limit: default 10 GB. When it is passed, the oldest unused files are deleted.
- Stretcher: `stretcher signalsmith` (default) or `stretcher rubberband`. Keep the tone: `keep_tone off` (default) or
  `keep_tone on`. They apply to every Samplosis in every project. Changing either makes new keep-length copies; the
  old ones stay in the cache, so changing back is instant.
- The environment variable SAMPLOSIS_SETTINGS names a different settings file (the tests use it).

## Build, install and test

- `build.bat` builds `samplosis.clap`. `build.bat test` also builds `tests\host.exe`, a small CLAP host for the tests.
  It needs Visual Studio's C++ tools and the CLAP headers (set CLAP_SDK to a checkout root to override the default).
- `install.bat` copies `samplosis.clap` to `D:\clap`. If the copy fails, close REAPER first.
- `python -m pytest tests -q` runs the tests (needs numpy, soundfile and psutil). Some of them open test windows.
- `python tests\measure.py` prints the CPU and memory numbers below, the SoundFont ones too when
  E:\samplepacks\soundfonts\just_t4_fixed.sf2 exists. Set MEASURE_DIR to a folder on a roomy drive; it needs a few GB for a
  while and deletes its work folder at the end.

## Measured numbers

Measured on 2026-10-28 on a 20-thread Intel Core Ultra 7 255HX, with `python tests\measure.py` (through the engine's
exported functions, with a temporary cache folder) unless it says otherwise.

- CPU: 16 voices held, 48 kHz, 256-frame blocks, through smp_engine_render_test, 10 seconds of audio, note range 48
  to 63 (C3 to D#4). Keep length off: 1.00 percent of real time. Keep length on, all copies ready: 1.09 percent. The
  target was under 2 percent. This includes the test helper's small waits, so it is an upper bound for the audio
  thread.
- Getting every key ready (97 copies, 4 octaves each way) for a 5.2 second guitar note (KSHMR Vol 5, Acoustic
  Chime): Signalsmith Stretch 1.7 seconds (2.9 keeping the tone), Rubber Band 11.4 seconds (12.7 keeping the tone).
  Before copies were made in parallel, Rubber Band took about 0.55 seconds a copy, one at a time.
- Memory, per instance, as private bytes of the measuring process from just before smp_engine_new:
  - A new idle engine: 8.9 MB. Nearly all of it is the voice rings, 512 KB for each of the 16 voices the Voices
    parameter allows by default. Rings for more voices are committed only when Voices is raised.
  - With a 60 second stereo sample loaded and keep length on: 19.9 MB, with all 97 copies rendered (that took 33
    seconds with Signalsmith Stretch). Each copy keeps only its first quarter second in memory (about 88 KB); the
    rest is read from the cache folder (about 21 MB per copy here, 2.1 GB in all) by a background thread as notes
    play. While copies are being made, they use at most 512 MB of temporary memory, shared by every instance.

### SoundFont numbers

Measured on 2026-10-28 on the same machine with `python tests\measure.py`, at below-normal priority, on
E:\samplepacks\soundfonts\just_t4_fixed.sf2 (526 MB, 142 presets), through the exported functions. Memory is the
growth of this process's private bytes. The file was in the operating system's file cache, so the load times are
warm-cache times; a first read from a cold disk is slower.

- Opening the font (headers and preset lists only): 2 ms and 1 to 3 MB.
- Loading a preset's samples, time and memory:
  - program 0, the acoustic piano: 11 to 12 ms, 24 MB (23.8 MB of sample data);
  - program 25, the steel guitar: 8 to 9 ms, 17.6 MB;
  - drum kit 128:0: 7 ms in most runs (0.35 s in one of three), 9 MB.
  - These add up: after all three, 52.4 MB of samples are loaded.
- CPU for 66 voices held (33 notes of the drawbar organ, program 17, which loops and layers two regions a note), 48 kHz,
  256-frame blocks, 10 seconds of audio, through smp_synth_process:
  - reverb and chorus off: 2.7 percent of real time;
  - reverb and chorus on: 2.9 percent.
  - The target was under 5 percent: met. A piano on 64 notes decays to fewer than 30 sounding voices and costs 3.5 percent
    for the whole run, so it is not a held test. The first try with 122 voices (61 organ notes) measured 5.0 to 5.3 percent,
    which is the same cost per voice (about 4.2 percent per 100 voices); 64 notes in a layered preset can pass 5 percent.
- A second instance on the same font, with the same preset playing: 2.3 MB more (2.2 to 2.3 across runs). The target was
  under 2 MB: **missed** by about 0.3 MB. The samples are shared; the 2.3 MB is the second player's own memory (I did not break it down).

## Known limits

- Keep-length copies exist only within 48 semitones of the root. Keys further away are resampled (pitch by speed).
- The first note after moving Start, End or the loop start may be resampled once per key, until the new copies
  have been rendered.
- The copies of a long sample take a lot of cache space: each copy is as big as the decoded sample. Copies past a
  quarter of the cache size limit (beyond an octave each way) are made only when their key is first played, and
  that first note is resampled. Narrowing the key range (Lowest note, Highest note) renders less and frees the
  copies it no longer reaches.
- One sample per instance in the Sampler.

### Not in the SoundFont player yet

- Keep length and the tempo lock don't apply to SoundFonts (they are Sampler features).
- A SoundFont's zones can't be edited as Samplosis zones yet; the player plays the font as it is.
- sm24, the extra byte of 24-bit samples, is ignored: 24-bit SoundFonts play at 16 bits.
- NRPN and SysEx beyond the GM, GS and XG resets are ignored. There are no data increment and decrement (CC96, CC97).
- Reverb and chorus are not on the extra outputs (those channels are dry).
- Linked modulators are ignored (as in FluidSynth).

## What is still to come

Done so far: stage 1 (one sample, keep length), the stretcher choice, and stage 4 (SoundFonts, with 16 stereo outputs
for them). Still to come, from the design (docs\superpowers\specs): several samples and zones in the Sampler, filter and
modulation, the command line tool, the tempo lock, and 16 outputs for the Sampler's zones.
