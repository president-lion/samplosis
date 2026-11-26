# Samplosis

An accessible sampler for REAPER, written in C as a CLAP instrument for Windows. The window is made of plain Windows
controls that NVDA reads as they are.

Licence: GPL version 2 (see COPYING). It includes these libraries, each under its own licence:

- Rubber Band (Particular Programs, GPL), in src\rubberband, and Signalsmith Stretch (Geraint Luff, MIT), in
  src\signalsmith: the time stretchers.
- dr_wav, dr_flac and dr_mp3 (David Reid; public domain or MIT No Attribution) and stb_vorbis (Sean Barrett; public
  domain or MIT), in src\decode: the built-in decoders, with their licences at the end of each file.
- In src\codecs, each folder with its licence file and a VERSION.txt naming the version and where it came from:
  libopus 1.6.1, opusfile 0.12 and libogg 1.3.6 (Xiph.Org, 3-clause BSD); WavPack 5.9.0 (David Bryant, 3-clause
  BSD); the Monkey's Audio SDK 13.43 (Matthew T. Ashland, 3-clause BSD); yxml (Yoran Heling, MIT), which reads
  Decent Sampler presets; and libgig 4.6.0 (Christian Schoenebeck, GPL version 2 or later, with four small read
  fixes listed in its VERSION.txt), which reads GigaStudio files.

## What it does

Samplosis has two players in one plugin, chosen by a tab: the Sampler, which plays samples across the keyboard
(stage 1; several samples, each on its own keys and velocities), and the SoundFont player (stage 4, see "The SoundFont player"), which also plays SFZ, Decent Sampler,
Logic EXS, DLS and GigaStudio instruments. A new instance starts on the Sampler, and a project saved before the SoundFont
player existed loads as it was. Every file format, and what reads it, is in "File formats".

The Sampler:

- Load one audio file (wav, flac, ogg, mp3, opus, wv, ape and the other formats the bundled decoders read, and m4a,
  aac, wma and whatever else Windows itself decodes; with FFmpeg installed, also dsf, tta and anything else it
  decodes; see "File formats") with the Browse button, by dragging it onto the window, or from a saved REAPER project. Files from the
  window load in the background: the status says "Loading x.wav" or "Decoding x.dsf with FFmpeg" until it's ready.
  A damaged file of a built-in format (a .wav or .flac the built-in decoders refuse) may still load through FFmpeg;
  the status then says "(decoded by FFmpeg)". Known limit: a saved project loads its sample while REAPER waits, so
  a sample not yet in the cache that needs FFmpeg holds up opening the project while it decodes.
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
- Several samples in one instance (up to 128): choose several files at once in Browse (they replace the samples
  loaded) or Add samples (they join them), or drop several on the window. Each sample has its own Start key and End
  key, its own Velocity range (1 to 127 unless set, typed like "15,80"), and its own root note; a note plays every
  sample whose keys and velocities it falls in. See "The samples".
- Envelope, loop, start and end points, reverse, speed, velocity response, key and velocity ranges, probability,
  pitch bend and polyphony (up to 64 voices, 16 by default).
- Glide: a note slides in from the pitch of the note before it.
- Every control is also a REAPER parameter you can automate.
- The whole instrument can be read and written as plain text (see "The script format").

## The window, in Tab order

Sampler page:

0. Player: the tab control (Sampler or SoundFont), first on both main pages (see "The window: the Player tabs").
1. Sample: the selected sample's file, its length and its root note and where it came from. Read only.
2. Browse button: one or more files, which replace the samples loaded.
3. File: a combo box of the audio files in the selected sample's folder (see "The File combo box"); choosing one
   replaces the selected sample, keeping its keys and velocities, so Up and Down try each file in its place. Enter
   adds the file shown as a new sample instead, and the selected sample goes back to the file it had when you came
   to the box; try another and press Enter again to add more.
4. Samples: a list of the samples, one line each ("Guitar C3.wav, root C3, keys C-1 to G9, velocity 1 to 127").
   Several can be picked, as in any Windows list: Up and Down pick one; Shift with Up, Down, Home, End or a click
   picks a run; Ctrl+A picks them all; Ctrl+Up and Ctrl+Down move without picking, and Ctrl+Space picks or unpicks
   the one moved to; Ctrl+click picks or unpicks one. Delete removes the picked ones. The one moved to last is the
   selected sample: the Sample box, File and Root note are its. With none it says "No samples loaded".
5. Add samples button: one or more files, added to the list (they are picked, the first of them selected).
6. Remove sample button: removes the picked samples (Ctrl+A, then Remove sample, empties the list).
7. Start key and End key: sliders (notes, like the other note sliders) for the keys of every picked sample (they
   show the selected one's). Moving one
   past the other takes the other with it. Typing note names without an octave in either one ("C", "D F", "C, E, G",
   "F#", "Bb") makes the sample play those notes in every octave instead: both then read "every D and F". A plain
   letter is that note only (D isn't D#; type D# for it). An arrow key afterwards makes it a range again, from the
   lowest to the highest of those keys.
8. Velocity: an edit box with the selected sample's velocities, like "1,127"; what you type goes to every picked
   sample. Type two numbers from 1 to 127
   ("15,80", "15 80", "15-80" or "15 to 80", either way round; one number is both ends), then Enter, or Tab away. A
   value that doesn't read beeps and the box goes back to what it was; Escape puts it back too.
9. Keep length (on or off).
10. Mode.
11. Then the other parameters in the order of the list below, from Root note to Glide mode (the window lays them out
   in two columns, but Tab follows the list order), each one a slider (or a combo box for choices). Root note is the
   selected sample's.
12. Status: for example "Keep length: 10 of 25 copies ready", or the reason a file did not load.
13. Settings button.

Settings page:

1. Stretcher: Signalsmith Stretch, fast, or Rubber Band, slower.
2. Keep the tone: off or on. On keeps the body of the instrument where it was, so a note shifted up doesn't go
   thin and a note shifted down doesn't go boomy.
3. Cache folder (edit box), then Browse folder.
4. Cache size limit (in gigabytes), a slider.
5. Raw sample rate, Raw channels, Raw sample format and Raw byte order: combo boxes for files ending .raw or .pcm
   (see "Settings").
6. FFmpeg: the ffmpeg.exe in use, or "Not found" (read only), then Browse for FFmpeg.
7. Cache in use (read only).
8. Clear cache button.
9. Back button.

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
  Switching during playback fades the old player's notes out over 5 ms (its reverb and chorus over 10 ms), so it
  doesn't click. While the Sampler plays, the SoundFont player lets go of its presets' samples (the font stays open,
  so switching back is quick; the presets load again then).
- Drops load by file type: an instrument (.sf2, .sf3, .sf4, .sf2pack, .sbk, .dls, .gig, .sfz, .dspreset, .dslibrary or .exs) loads
  into the SoundFont player and switches to it; any other audio file loads into the Sampler and switches to it.
- Each page's Browse loads only its own kind. On the SoundFont page a .wav gives an error and a beep and the font
  stays; on the Sampler page a .sf2 does the same.
- Settings is one page for both. Back returns to the main page that plays.

### The samples

- Each sample plays only for the keys from its Start key to its End key (or, when note names were typed, those
  notes in every octave) and the velocities in its Velocity range.
  Samples can overlap: a note in two samples' ranges plays both. A new sample has every key and velocity.
- Each sample's root note is its own: from its file name, or detected, as for one sample. The Root note slider (and
  REAPER parameter) is the selected sample's, so selecting another sample moves it.
- Every other parameter is the instrument's and applies to every sample alike: the envelope, volume, loop, keep
  length and the rest. Lowest note, Highest note, Lowest velocity and Highest velocity still limit the whole
  instrument: a sample sounds only inside both its own ranges and those.
- Voices is per sample (16 means up to 16 notes of each sample at once), glide and legato follow notes within a
  sample, and Probability is drawn for each sample.
- Browse and a drop of several files replace the samples; Add samples adds; the File combo box replaces the selected
  one. While several load the status says "Loading 2 of 5 samples". Those that load are taken; the first that
  doesn't is told ("Can't load bad.wav: why (and 1 more didn't load)", with a beep). When none loads, nothing
  changes.
- With several samples the status says "Ready: 3 samples". A sample from a project whose file is gone stays in the
  list as "Missing: <path>", silent, and is saved back as it was.

### The File combo box

File replaces the old Previous file and Next file buttons, on both main pages. It comes right after Browse.

- It lists the files in the loaded file's folder of that page's kind (audio files on the Sampler page, every
  instrument kind on the SoundFont page: .sf2, .sf3, .sf4, .sf2pack, .sbk, .dls, .gig, .sfz, .dspreset, .dslibrary and .exs): names
  only, alphabetical, ignoring case. The loaded one is selected. Browse on the SoundFont page offers the same kinds
  ("SoundFonts and instruments"), and All files.
- Choosing a file loads it at once. Up and Down on the closed box step through the folder and load each file as you
  go; they stop at the ends and do not wrap. Home and End go to the first and last.
- With nothing loaded it holds one entry, "No file loaded", and choosing does nothing. A file that has gone shows as
  "<name> (missing)".
- A file that fails to load gives a beep, the reason in Status, and the box goes back to the loaded file. When
  something still plays, Status says so: "Can't load x.sfz: why. Still playing Piano.sf2." The message stays for
  6 seconds, or until the next file loads; then Status says how things are again (such as the keep-length
  progress).
- The list is rescanned when the box gets the focus, and rebuilt when the loaded file changes. Arrowing through a list
  of big fonts loads each one in turn.

## The SoundFont player

It plays a SoundFont 2 (.sf2) or SoundFont 3 (.sf3, Ogg compressed) file on 16 MIDI channels, as a General MIDI
player does, and also SF4, SF2Pack, SoundFont 1, DLS, GigaStudio, SFZ, Decent Sampler and Logic EXS instruments (see "File
formats"); the tab and the page keep the name SoundFont. Each instrument file is a font of presets: a DLS or
GigaStudio file has one preset per instrument, an SFZ, Decent Sampler or EXS file is one preset (bank 0, program 1).
Notes, program changes, controllers, pitch bend, pressure, sustain, RPN tuning and bend range, and the GM, GS and XG
reset messages all work. Samples load in the background for the presets in use, and a font shared by several
instances is kept in memory once.

Only the start of each sample is kept in memory: the first quarter second, or the whole sample when it is shorter
than 1 second. A note starts from that at once, and the rest streams from the disk while it plays. Uncompressed
samples stream straight from where they are: the font file for an .sf2, .sf2pack's unpacked copy, a .dls or .gig
file's own wave data, and an SFZ or Decent Sampler instrument's own WAV or AIFF sample files. Compressed
samples (an .sf3's Ogg, an .sf4's FLAC, a compressed GigaStudio file, and SFZ or Decent Sampler samples in FLAC,
Ogg, MP3 and the other compressed formats) are decoded once, the first time they are needed, into the cache folder
(Settings) as mono float files, and stream from there; the next time the instrument opens nothing is decoded. A
loop that lies wholly in the first quarter second plays from memory. Each player streams with a thread of its own,
into a 64 KB ring for each voice that plays past a sample's start (a stereo sample plays as two voices). If the
disk falls behind, the voice fades out and back in rather than clicking (an underrun), and nothing stalls. An
offline render waits for the disk, so it is exact. The piano of a General MIDI font that needed 24 MB in memory now
needs under 1 MB. See "Measured numbers" for a 1 GB piano streaming from a USB hard disk.

The SoundFont page, in Tab order:

1. Player (the tab control).
2. SoundFont: the loaded font, its format and its preset count, read only, like "gm.sf2, 142 presets" (no format
   word for SoundFont 2 and 3), "Piano.sfz, SFZ, 1 preset", "gm.dls, DLS, 235 presets" or "Strings.gig, GigaStudio,
   4 presets". The other format words are SF4, SF2Pack, SoundFont 1, Decent Sampler and EXS.
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
9. The knobs of a Decent Sampler preset that has them (see "A Decent Sampler preset's knobs"): one slider each, named
   by the preset. A font without knobs has none, and Tab goes straight on.
10. Reverb and chorus: On or Off.
11. Status.
12. Settings button.

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
one. Channels on outputs other than 1 are dry: reverb and chorus apply only to Output 1, and so do a Decent Sampler
preset's own effects. If the host provides fewer
outputs, those channels play on Output 1. In REAPER, turn on the extra outputs in the plugin's routing.

### Reverb and chorus

A built-in reverb and chorus, driven by the song's CC91 and CC93 sends as General MIDI defines. The Reverb and chorus
switch turns both off. The power-on reverb send is 40 (General MIDI level 2). The reverb is a Freeverb design (not
FluidSynth's, so its character differs) and the chorus depth is 8 ms. SoundFont volume applies to the effects too. REAPER's
stop, seek and loop fade the tails out over 10 ms and clear them.

### A Decent Sampler preset's knobs

A Decent Sampler preset's knobs (its <ui> controls) play as in Decent Sampler, from the window and by MIDI CC:

- Each knob is a slider on the SoundFont page after SoundFont transpose, named by its label, two to a row (Reverb and
  chorus, Status and Settings move down below them). The window is tall enough for 16; a preset with more shows its
  first 16. In One preset mode they are the chosen preset's; in All channels mode every preset's that has knobs
  (named "<preset>: <label>" when a font has more than one such preset).
- The keys are the other sliders' (arrows, Shift ten times as far, Control a tenth, Page Up and Down, Home and End,
  Enter or a character to type a value). A knob of whole numbers steps by 1; the others by a hundredth of their
  range. The words: a 0 to 1 knob in percent ("40 percent"), a knob that tunes in semitones ("plus 3 semitones",
  "minus 12 semitones"), a musical-time knob as a note length ("1/4", "1/8 dotted", "1/4 triplet"), others as
  numbers. Typing takes the same words, or a number in the knob's own units.
- The preset's <midi> CC bindings move them: a CC on a channel that plays the preset sets the knob, through the
  binding's translation, and the slider follows within a quarter second. A CC bound this way loses its General MIDI
  meaning for that preset (Finnish Forest Percussion's CC11 is Force, not expression; CC1 is Pitch, not vibrato);
  other CCs keep theirs. A knob has one value, shared by the window and its CCs: the last move wins.
- What a knob sets (its bindings): a group's or every group's volume (AMP_VOLUME, by tags or groupIndex), the
  tuning (GLOBAL_TUNING, GROUP_TUNING), and its effects' parameters (FX_FILTER_FREQUENCY, FX_FILTER_RESONANCE or
  FX_FILTER_Q, FX_REVERB_WET_LEVEL, FX_REVERB_ROOM_SIZE, FX_REVERB_DAMPING, FX_WET_LEVEL, FX_MIX, FX_DELAY_TIME,
  FX_FEEDBACK), with linear, table and fixed_value translations. A binding sets its parameter, as in Decent Sampler:
  a group's volume knob replaces that group's volume attribute. Integer and musical-time knobs give whole numbers.
  A layer whose volume is 0 when a note starts plays no voice for that note.
- The preset's <effects> play on its own bus, in order: lowpass, highpass and bandpass (resonant, 12 dB an octave;
  lowpass_1pl 6 dB), reverb (Freeverb, as JUCE's, which Decent Sampler uses, with the preset's room size and
  damping) and delay (stereo, with its feedback, stereo offset and feedback cutoff; a musical time follows the host's
  tempo, 120 BPM without one). Musical times are Decent Sampler's list of 21: 1/64 triplet, 1/64, 1/32 triplet, 1/64
  dotted, 1/32, 1/16 triplet, 1/32 dotted, 1/16, 1/8 triplet, 1/16 dotted, 1/8, 1/4 triplet, 1/8 dotted, 1/4, 1/2
  triplet, 1/4 dotted, 1/2, 1 triplet, 1/2 dotted, 1, 1 dotted. A preset with a reverb sends nothing to the shared
  reverb. The Reverb and chorus switch also turns off a preset's reverbs and delays (its filters stay: they are its
  tone). Stop, seek and loop clear their tails.
- The positions are saved with the project (sf_knob lines) and come back with it. Loading a file (another, or the
  same again) sets every knob to its preset's starting value.
- Limits: 32 knobs, 8 bindings a knob, 8 effects and 64 CC bindings a preset; values for the font's first 8 presets
  that have knobs or effects (the rest play at their starting values); a group's volume or tuning for its first 1024
  groups.

### Loading ahead

The state keeps an `sf_used` line: the presets in use when it was saved. They load as soon as the project loads, so the
first notes aren't late. They stay loaded until 10 seconds after the first note the player gets (however long the
project sits before Play is pressed); by then the song's own program changes have chosen what it needs, and the rest
are let go. REAPER's stop, seek and loop keep them; a new state, a new font, or switching to the Sampler lets them go
at once. A program change right before the first notes loads that preset at once: in real time the
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
- "Loading Piano.sfz": an instrument opening in the background from the window (an SFZ reads every sample file's
  header first). The window stays usable, and what played before plays until it is ready.
- "Unpacking Kit.dslibrary: 120 of 345 files": a Decent Sampler bundle unpacking into the cache folder, the first
  time it opens.
- "No SoundFont loaded"
- "Missing: <path>": the file in the project is gone. The state keeps its path and settings, so the project still
  saves and comes back right if the file returns.
- "Can't load x: <reason>": a damaged or unsupported file, with " Still playing y." when something still plays. It
  never crashes anything.
- After "Ready: ..." or "Loading samples ...", what the instrument couldn't do, in the same line:
  - its problem: "Missing 3 samples (first: Piano C4.wav)" (those regions are left out, the rest plays), "2 samples
    can't be read (first: ...)", "Can't unpack x.sf2pack (it may be damaged)", or the GigaStudio restart message
    (below);
  - the settings it ignored: "Ignored 12 SFZ settings it doesn't support (first: lfo01_freq)" (also "Decent
    Sampler settings" and "GigaStudio settings"). Each different setting name counts once.

  For example: "Ready: kit.sfz, SFZ, 1 preset. Missing 1 sample (first: gone.wav). Ignored 1 SFZ setting it doesn't
  support (first: eq1_freq)". The line changes only when the instrument does (a problem found while samples load
  appears once), so it doesn't chatter.
- "A GigaStudio file crashed the reader; save your project under a new name and restart REAPER": libgig crashed on
  a damaged .gig file and Samplosis caught it. Every GigaStudio load is refused until REAPER restarts, since the
  memory may be damaged. Other formats still load.

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
    sf_knob 1 4 0.4
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
- sf_knob P K V: a Decent Sampler preset's knob away from its starting value: preset P (from 1, in the font's order),
  knob K (from 1, in the preset's order), value V in the knob's own units. A preset or knob the font no longer has is
  passed over. With the font missing, the lines are kept and saved again.
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

## File formats

### Sound files

The Sampler loads these, and SFZ and Decent Sampler instruments use them as their samples. A file goes through four
stages in turn, and the first one that reads it wins. The built-in and library stages go by the file's first bytes,
not its name, so a misnamed file still loads.

1. Built in (decoders compiled into the plugin):
   - WAV in any of its containers: RIFF, RIFX, RF64 and Wave64 (.wav, .w64, .rf64), with 8 to 32-bit integer,
     32 and 64-bit float, A-law, mu-law and ADPCM data;
   - AIFF and AIFF-C (.aif, .aiff, .aifc);
   - CAF (.caf), uncompressed only: 8 to 32-bit integer and 32 or 64-bit float, either byte order (an ALAC or other
     compressed CAF says so plainly);
   - FLAC (.flac), Ogg Vorbis (.ogg, .oga) and MP3 (.mp3);
   - raw PCM: files ending .raw or .pcm, read as the raw settings say (see "Settings"), since they have no header.
2. Library (the codecs vendored in src\codecs): Opus (.opus), WavPack (.wv, including DSD WavPack and files from
   before version 4) and Monkey's Audio (.ape).
3. Windows (Media Foundation, whatever this Windows decodes; files only): on Windows 11 here that is AAC and ALAC
   in .m4a, .mp4 and .m4b, WMA, AC-3 and AMR (.amr, .3gp). Other machines may decode more or less (a Windows "N"
   edition needs the Media Feature Pack). AC-3 in particular depends on the machine: it needs Windows' Dolby
   decoder, which some Windows installations don't have; there FFmpeg decodes it, if it is installed. The status then says "Ready: x.m4a (decoded by Windows)".
4. FFmpeg, if it is installed (files only): anything else it decodes, such as DSF and DFF (DSD), TTA, Musepack
   (.mpc), Speex (.spx), E-AC-3, DTS, MLP and TrueHD, AU (.au, .snd), VOC, and the sound of Matroska, WebM and
   QuickTime files (.mka, .mkv, .webm, .mov). While it works the status says "Decoding x.dsf with FFmpeg", then
   "Ready: x.dsf (decoded by FFmpeg)". A damaged file of a built-in format that the built-in decoders refuse may
   still be read by FFmpeg. FFmpeg runs hidden at below-normal priority; if it sends nothing for 60 seconds it is
   stopped and the status says "FFmpeg took too long to decode it". Without FFmpeg, a file only it could read says
   "FFmpeg isn't installed; this format needs it".

Browse and the File combo box on the Sampler page list these extensions: .wav .flac .mp3 .ogg .aif .aiff .aifc .w64
.rf64 .caf .raw .pcm .opus .wv .ape .m4a .aac .mp4 .wma .ac3 .amr .3gp .alac .dsf .dff .tta .mka .webm .au .snd
.mpc .spx .oga .m4b .eac3 .dts .mlp .thd .voc .mov .mkv. A decoded file is at most 2 GiB of float samples (about
1.5 hours of stereo at 48 kHz); a longer one is refused with a plain reason.

### Instruments (the SoundFont page)

The file's first bytes choose the reader, then its name.

- SoundFont 2 (.sf2): read by Samplosis itself. Samples stream from the file.
- SoundFont 3 (.sf3, Ogg Vorbis or Opus samples) and SF4 (.sf4, FLAC samples, as Cognitone's sf2convert writes
  them): samples decoded once by the built-in and library decoders into the cache folder.
- SF2Pack (.sf2pack, the whole sample block compressed in one stream): unpacked once into the cache folder, in the
  background, the first time a preset is used.
- SoundFont 1 (.sbk): read as SoundFont 2 with the version 1 rules.
- DLS levels 1 and 2 (.dls), such as Windows' own C:\Windows\System32\drivers\gm.dls: 8 and 16-bit PCM waves stream
  from the file; ADPCM waves are decoded once into the cache. Instruments are presets (drum instruments in bank 128),
  articulations become envelopes, LFOs and modulators, and conditional chunks are evaluated.
- GigaStudio and Gigasampler (.gig, versions 2 to 4, and the .gx01 extension files next to it): read through
  libgig. Uncompressed waves stream from the file; compressed ones are decoded once into the cache. Velocity,
  layer, keyswitch, round robin, random, release trigger, stereo and controller dimensions are mapped, with EG1,
  EG2, the filter and the three LFOs.
- SFZ (.sfz, text): read by Samplosis, with #include and #define, default_path, note_offset and octave_offset. Each
  sample file is a sound file of any format above: WAV and AIFF stream from the file, others are decoded once into
  the cache. Round robins, random layers, keyswitches, CC ranges, crossfades, release triggers (with rt_decay),
  groups and off_by, polyphony, envelopes, filters, LFOs and bend ranges play.
- Decent Sampler (.dspreset, XML): read by Samplosis (yxml checks the XML) into the same player as SFZ, with
  inheritance from groups, round robins, tags and silencing, release triggers, CC ranges, and the preset's knobs,
  CC bindings and effects (see "A Decent Sampler preset's knobs"). Two attributes with no space between them
  (name="a"next="b") read as Decent Sampler reads them.
- Decent Sampler bundles (.dslibrary, a zip): unpacked once into the cache folder (Settings), in a folder of their
  own, then read as .dspreset files, one preset each (by their names in the zip). Only the presets and the audio
  files are unpacked (not the pictures, __MACOSX or peak files); the bundle's own folder is never written to.
  Opening it again unpacks nothing; a changed bundle unpacks again. The unpacked folders count toward the cache
  limit and are trimmed oldest first, never while open. A bundle that isn't a zip, is damaged, holds a path outside
  its folder, or uses encryption, doesn't open, with the reason.
- Logic EXS24 and Sampler (.exs, both the little-endian and the older big-endian layout): read by Samplosis into the
  same player as SFZ. Zones and groups (key and velocity ranges, volume, pan, tuning, start and end, loops, one-shot,
  pitch tracking off, mute), round robins, random select, keyswitch groups (enable by note), CC-range groups,
  release triggers, exclusive groups, the amplitude envelope, velocity sensitivity, bend range and transpose play.
  An .exs names its samples by a folder on the Mac it was made on, so each is looked for: beside the .exs, in its
  subfolders, then in each folder above it (three at most, never a drive's root, at most 20000 folders and 3
  seconds); where two files share a name, the one in a folder named like the Mac folder wins. Keep the library's
  Sampler Instruments and Samples folders together, as Logic does, and they are found.

Opening an SFZ, Decent Sampler or EXS instrument reads every sample file's header (never decoding a file to do so), so it
opens in the background from the window. The headers are remembered in the cache folder, so the next open is quick.
Samples that are missing or can't be read leave their regions out; the rest plays, and the status names the first
(see "Status texts"). When none is left, the instrument doesn't load and that is the reason.

### What SFZ, Decent Sampler, EXS and GigaStudio settings aren't supported

These are counted: the status says how many different ones a file used, and the first ("Ignored 12 SFZ settings it
doesn't support (first: lfo01_freq)"). The rest of the instrument plays without them.

- SFZ: CC modulation (every opcode ending _onccN, _ccN or _curveccN), offset_random, amp_random, pitch_random,
  delay, the eq opcodes, ampeg_vel2 and ampeg_start, LFO fades, lfo3 and higher, lfo2 to volume or cutoff, set_cc,
  note_selfmask, position, count, sustain_cc, amp_keytrack; the one-pole, peaking, 4-pole and 6-pole filter types
  (those regions play unfiltered); unknown loop_mode, trigger, off_mode and crossfade curve values; more than 4 CC
  ranges in a region; made samples other than *sine, *silence and *noise; and unknown headers (counted as
  <name>). Labels, hints, image, md5, output, off_time, and the contents of <curve>, <effect> and <midi> are read
  past without counting. Approximations: LFOs are triangles (SFZ's default is a sine); when a region has both a
  filter and a pitch envelope, the filter envelope's times serve both; release_key plays as a release trigger and
  off_mode=time as normal.
- Decent Sampler: glide, loop crossfades, loopDuringRelease, curves, playbackMode, slices, output routing other
  than MAIN_OUTPUT, legato, onLoCC and onHiCC, delay, silencingDecay and the other attributes not listed above
  (names are read as written: rootnote isn't rootNote, as in Decent Sampler); the <modulators>, <tags>,
  <keyswitches> and <noteSequences> elements (counted once each, with their contents read past); buttons, menus, XY
  pads and drawbars, multi_state knobs, bindings to other parameters (named, like "binding ENV_RELEASE"), effect
  types other than the filters, reverb and delay (named, like "effect chorus"), CC bindings that aren't to a knob's
  value; trigger=continuous (plays as attack); a second silencing tag. The pictures (background, knob skins, keyboard
  colours) are read past. Decent Sampler's own limiter headroom isn't copied (a preset may play louder than in
  Decent Sampler). The knobs are not host parameters: REAPER can't automate them (send their CCs instead).
- EXS: reversed zones, backward and alternating loops, loop crossfades and loop tuning, zone outputs, Flex, group
  polyphony, release trigger decay, velocity and key crossfades, groups enabled by pitch bend, MIDI channel,
  articulation or tempo (they play as if always on), the filter, mono legato, glide and volume key scaling. The
  modulation matrix and LFOs are read past. Apple never published the format; the layout read is the one
  ConvertWithMoss documents.
- GigaStudio: EG1 and EG2 pre-attack and their controllers, the filter cutoff and resonance controllers, velocity
  to cutoff, a filter LFO beside the amplitude LFO at another speed, EG3 (the pitch envelope), dimension bypass,
  sustain defeat, mid/side decoding, ping-pong and backward loops (played forward), a second loop, velocity
  attenuation without a crossfade, controller crossfades, release triggers that don't come on note-off, instrument
  attenuation, MIDI rules, instrument scripts, channel aftertouch, smart MIDI and unknown dimensions (each plays the
  zone value 0 selects), and a fifth controller dimension. Approximations: EG1's second decay, EG2's cutoff
  scaling, the filter types (mapped to the player's filters with LinuxSampler's frequency map), LFO depths, and the
  amplitude and filter LFOs sharing one LFO. Round robins count per region, where GigaStudio counts per key.

Real files: the CC0 Karoryfer Emily Guitar SFZ library ignores 1 setting in emily_basic.sfz (amp_random), 1 in
emily_chords.sfz (delay) and none in emily_five_voice.sfz (see "Measured numbers").

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
  With Keep length on, a key away from the root plays a pitch-shifted copy whose loop no longer quite meets itself
  (the pitch moved, the loop points didn't), so a forward loop's last 85 ms (less for a short loop) crossfade into
  the sound just before the loop start: the wrap doesn't pop. A pure tone can dip by a few dB for those few
  milliseconds. Until that crossfade is ready (just after the loop moves), the note plays by changing speed instead.
  The root key, Keep length off and pingpong loops play the sample's own loop, as it is.
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

The instrument is plain text, one thing per line. Blank lines and lines starting with # are ignored. After the
sample lines (none, one, or one per sample), each parameter is `name value-in-words`. Names are the lower case ones used by the engine (root,
transpose, fine, volume, pan, attack, decay, sustain, release, velocity_sens, min_volume, mode, note_lo, note_hi,
pitch_lo, vel_lo, vel_hi, obey_note_off, loop, loop_start, start, end, bend_range, pitch_hi, reverse, keep_length,
voices, probability, speed, glide, glide_mode). The sample line has its path in quotes (backslashes literal) and the 16 hex digits of the
sample's cache key (the key below is only an example). With several samples, each sample line also has its root
(`root=C3`), and, when they aren't every key or every velocity, its keys (`keys=C2..C3`, or `keys=D,F` for every D and F) and velocities
(`velocity=15..80`); the root line is then the first sample's (the selected one when saved). With one sample its
root is the root line, as before. Example, shortened:

    # Samplosis 0.1.0
    sample "D:\guitar\Guitar C3.wav" key=0123456789abcdef
    root C3
    volume -6.0 dB
    release 250 ms
    mode note
    keep_length on
    voices 8

Two samples, a soft and a loud layer:

    sample "D:\piano\soft C4.wav" key=0123456789abcdef root=C4 velocity=1..80
    sample "D:\piano\loud C4.wav" key=fedcba9876543210 root=C4 velocity=81..127

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
- Raw sample rate (`raw_rate`, default 44100; 8000 to 192000 from a list), Raw channels (`raw_channels 1` or `2`,
  default 2), Raw sample format (`raw_format s16`, `s24`, `s32`, `f32` or `u8`, default s16) and Raw byte order
  (`raw_endian little` or `big`, default little): how a file ending .raw or .pcm is read, since it has no header to
  say. Changing one applies the next time a raw file loads, and the status says so.
- FFmpeg (`ffmpeg_path`, empty by default): the ffmpeg.exe to use (ffprobe.exe must be beside it). Empty means it is
  looked for on PATH, then in the WinGet package folder
  (`%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg*\*\bin\ffmpeg.exe`). The Settings page shows the one found,
  or "Not found". Samplosis never installs it.
- The environment variable SAMPLOSIS_SETTINGS names a different settings file (the tests use it).

## Build, install and test

- `build.bat` builds `samplosis.clap`. `build.bat test` also builds `tests\host.exe`, a small CLAP host for the tests.
  It needs Visual Studio's C++ tools and the CLAP headers (set CLAP_SDK to a checkout root to override the default).
- `install.bat` copies `samplosis.clap` to `D:\clap`. If the copy fails, close REAPER first.
- `python -m pytest tests -q` runs the tests (needs numpy, soundfile and psutil). Some of them open test windows.
- `python tests\measure.py` prints the CPU and memory numbers below, the SoundFont ones too when
  E:\samplepacks\soundfonts\just_t4_fixed.sf2 exists. Its work folder is MEASURE_DIR, else E:\samplepacks\_build, else
  the drive of the cache folder (never the system temp folder on C:); it needs a few GB for a while and deletes its
  work folder at the end. MEASURE_PARTS picks parts (sampler, sf2, stream, gig, dls, sfz; all by default). The
  stream part writes a 1 GB instrument to MEASURE_STREAM_DIR (default E:\samplepacks\_build\samplosis_stream_test)
  and deletes it (MEASURE_STREAM_NOTES, _RUNS, _PURGE, _REF, _KEEP and _FEATURES change what it plays; the top of
  measure.py says how); the gig part writes a 2 GB compressed .gig and deletes it; MEASURE_SFZ names a real .sfz to
  try; MEASURE_DLL measures another build (for before-and-after numbers).

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

- Since streaming (stage 3, measured 2026-10-29 the same way): the three presets below load 0.82, 0.58 and 5.27 MB of
  sample data (6.7 MB for all three instead of 52.4); the piano loads in 1 ms. A new idle player is 1.93 MB (1.80
  before), and a second instance on the same font 1.95 MB idle and 2.10 MB with a piano note playing (1.81 before:
  each voice that streams has a 64 KB ring). The time spent in smp_synth_process at real-time pace is the same as
  before; the streamer thread adds about 1 to 1.5 percent of a core for 66 streaming voices. The numbers below are
  the stage 2 ones; "Formats (stage 3)" below has the same measurements with every format in.
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
  under 2 MB: **missed** by about 0.3 MB. The samples are shared; the 2.3 MB is the second player's own memory (this has not been broken down yet).

### Formats (stage 3)

Measured on 2026-10-29 on the same machine, with `python tests\measure.py` (MEASURE_DIR on D:), the SoundFont parts at
below-normal priority. E: is a USB hard disk.

just_t4_fixed.sf2 against the stage 2 numbers above (the rule: at most 10 percent worse), four runs, each one
`MEASURE_PARTS=sf2 python tests\measure.py` (the 40-open median came from a loop of 40 smp_sf_open calls on the
same file, run by hand):

- Opening the font: 3 to 4 ms in a single open, 2.18 ms median over 40 opens (1.83 to 2.58), against stage 2's 2 ms;
  1.6 to 3.2 MB, against 1 to 3 MB.
- Loading the three presets: 2, 1 and 6 ms, with 0.82, 1.40 (in all) and 6.68 MB (in all) of samples in memory,
  against 11, 8 and 7 ms and 52.4 MB in all. Much better: only the heads are loaded.
- CPU for 66 organ voices: 2.63 to 3.05 percent with reverb and chorus off (stage 2: 2.7), 2.88 to 3.18 percent with
  them on (stage 2: 2.9). The medians, 2.67 and 3.08, are within the 10 percent; single runs vary by about 0.4.
- A second instance with a note playing: 2.1 to 2.7 MB, against 2.3 MB. The worst run is 17 percent more, which is
  over the 10 percent rule: it is the 64 KB ring of each streaming voice, accepted when streaming came in (idle,
  the second instance is within the rule: 6.8 to 8.2 percent more).
- The Sampler, for reference: 0.79 and 0.85 percent CPU for 16 voices (keep length off and on), 8.9 MB idle, 23.2 MB
  with a 60 second sample and its 97 copies.

A GigaStudio-sized instrument streaming from E:. measure.py writes an SFZ piano to
E:\samplepacks\_build\samplosis_stream_test: 88 keys, 4 velocity layers, 352 stereo 16-bit 44.1 kHz WAV files of
17 seconds each, 1.06 GB in all, made from 8 repeated test tones, and deletes it afterwards. E: is a Transcend
StoreJet USB hard disk on Windows' Bulk-Only driver (USBSTOR, not UAS: one command at a time, so it can't reorder
reads).

- What "cold" means here: Windows keeps files it has read in memory, and the cache can't be emptied without
  administrator rights. So after writing, each file is opened once unbuffered while nothing else has it open, which
  makes Windows drop its cached pages. Proof that it worked: a test file then read at 22 to 28 MB per second (the
  USB disk), and at 2 to 3.2 GB per second the second time (from memory). Before each run everything is closed and
  purged this way, and the frames that stream were never read before, so they come from the disk: the disk counters
  show 30, 56 and 107 MB read during the 32-, 64- and 128-voice runs, which is what the notes need.
- Opening it cold (every sample file's header read): 4.7 to 5.1 seconds. Opening it again: 8 to 12 ms (the headers
  are remembered in the cache folder). Loading its heads cold: 6.2 to 9.2 seconds including the open, 15.6 MB of
  heads for 1.06 GB of samples.
- The final streaming fixes (deadline order: the voices with the least sound left go first, counting their head in
  memory; each visit reads a voice's ring full in one go) against the build before them, measured on 2026-10-29
  with `MEASURE_PARTS=stream MEASURE_STREAM_RUNS=1 python tests\measure.py` (32, 64 and 128 voices: 16, 32 and 64
  stereo notes struck within 85 ms, each a different file, 10 seconds in real time), three rounds alternating the
  old build (MEASURE_DLL) and the new one. Underruns, counted per voice and per 256-frame block (out of about
  60,000, 120,000 and 240,000):

  | voices | before (three runs) | after (three runs) |
  |---|---|---|
  | 32 | 138, 180, 2 | 10, 45, 80 |
  | 64 | 1015, 1528, 222 | 1640, 94, 0 |
  | 128 | 8284, 10074, 10547 | 5594, 1782, 4291 |

  Earlier clean runs of the same setting gave 12, 108 and 596 at 64 voices and 3635, 11393 and 3918 at 128.
- Compared with the same notes rendered offline (which waits for the disk), the dropouts are a few 100 ms windows
  around 0.6 to 1.3 seconds: every voice's first refill after its head and the first ring comes from the disk at
  about the same moment, and 32 to 64 cold seeks on this disk take longer than the 0.37 seconds a ring holds. After
  that the voices drift apart and Windows' read-ahead keeps up (a stray window or two later in some runs).
- From the file cache (warm: not purged), 128 voices: 25 underruns (before: 353); 64 voices: 0 (before: 8).
- What was tried and measured no better on this disk, and is off (switches for measuring, smp_sfs_test_features):
  several voices' reads in flight at once (worse at 64 and 128 voices: the disk can't reorder them, and they break
  up each file's sequential reads), one read for both halves of a stereo file (Windows serves the second half from
  its cache anyway), short urgent reads "just enough" to lift a voice over a target (twice the seeks), reading
  0.4 seconds past each head into the file cache at load (it only moves the dropouts from 0.6 to 1.4 seconds), and
  handles opened for sequential reads.
- So streaming keeps up with 64 voices from a USB hard disk in most cold runs and with 128 voices from the file
  cache, but a big chord struck all at once, on samples never read since the computer started, can still drop out
  for a few hundred milliseconds about 0.6 to 1.3 seconds in, more often at 128 voices. **The target of no
  underruns from a cold disk is not fully met**; the physics: a ring holds 0.37 s, and one seek on this disk takes
  10 to 20 ms, so about 20 to 35 voices can be refilled from cold places within one ring. Longer heads or rings
  (memory) would be the next step; see "Known limits".

A compressed GigaStudio file at project open (measure.py's "gig" part: a 2.02 GB .gig of 352 compressed waves,
written by tests\gigmake.exe on E: and deleted afterwards). A project's state load parses on the main thread.

- Before: opening it cold took 726 seconds (3 GB read: libgig scanned every compressed frame), 93 seconds the
  second time, 1.9 seconds warm.
- Now libgig scans a wave's frames only when it is first decoded, and the parse is kept in the cache folder: the
  first open cold takes 4.0 seconds (2 MB read: the chunk headers), every later open of the unchanged file 0.15
  seconds cold and 1 ms warm. Decoding the first wave cold: 0.4 seconds (its bytes are read through first, in big
  pieces; it took 58 seconds when libgig read it frame by frame).

gm.dls (C:\Windows\System32\drivers\gm.dls, read only): opens in 12 ms with 235 presets and 495 samples (1.5 to
1.9 MB). The piano loads in 4 ms (0.5 MB of samples). A C major chord and the C above, offline and in real time:
peak minus 13.5 dBFS, all finite, no underruns, no ignored settings.

A real SFZ library: the Karoryfer Emily Guitar (CC0, from github.com/sfzinstruments/karoryfer.emilyguitar, 121 MB,
323 WAV samples; downloaded for the test and deleted afterwards). Five notes from C3 to G4 at velocities 60 to 127,
offline and in real time:

- emily_basic.sfz: opens in 7 ms, loads in 21 ms (13.6 MB of heads), peak minus 12 dBFS, 10 voices, no underruns;
  ignores 1 setting (amp_random).
- emily_five_voice.sfz: 216 samples, loads in 15 ms, peak minus 2.3 to minus 2.9 dBFS (five layered voices a
  note), 25 voices, no underruns; ignores nothing.
- emily_chords.sfz: loads in 18 ms, peak minus 10 to minus 12 dBFS, no underruns; ignores 1 setting (delay).
- Every render was finite. The offline and real-time renders agree within 0.5 dB in level (RMS) and 1.6 dB in
  peak.

## Known limits

- Keep-length copies exist only within 48 semitones of the root. Keys further away are resampled (pitch by speed).
- The first note after moving Start, End or the loop start may be resampled once per key, until the new copies
  have been rendered.
- The copies of a long sample take a lot of cache space: each copy is as big as the decoded sample. Copies past a
  quarter of the cache size limit (beyond an octave each way) are made only when their key is first played, and
  that first note is resampled. Narrowing the key range (Lowest note, Highest note) renders less and frees the
  copies it no longer reaches.
- At most 128 samples in one instance. Each has its own engine (three threads and its voices' buffers, 8 MB at 16
  voices), so a big set uses more memory than one sample. Removing a sample stops its notes at once (no fade).
- Streaming from a slow disk: a big chord struck all at once, on samples not read since the computer started, can
  drop out for a few hundred milliseconds about 0.6 to 1.3 seconds in on a USB hard disk (rarely at 64 voices, more
  often at 128) before the disk catches up (see "Formats (stage 3)" in the measured numbers). Notes already playing,
  and samples read before, keep up. Each sample keeps only a quarter second in memory.
- A GigaStudio file's first open reads its chunk headers all through the file (4 seconds for a 2 GB file on the USB
  disk, cold); later opens of the unchanged file read the parse from the cache folder.
- A saved project loads its sample while REAPER waits. A sample not yet in the cache that needs FFmpeg can hold up
  opening the project while it decodes (up to 60 seconds without output before FFmpeg is given up).
- Windows decoding: when Windows marks an encoder delay itself, the padding at the end isn't trimmed; files that
  give their delay only in an iTunes tag keep it (a few milliseconds of silence at the start); a bare AC-3 stream
  keeps its 288-frame delay. If Windows' decoder ever hangs, every later file that needs Windows fails after 5
  seconds until REAPER restarts.
- A decoded file is at most 2 GiB of float samples (about 1.5 hours of stereo at 48 kHz). Decoding one near that
  size briefly needs about twice as much memory.
- Not yet tried with real files: Opus with more than 2 channels, some WavPack kinds (mono, 8-bit, multichannel,
  .wvc correction files, DSD), 24 and 8-bit Monkey's Audio, a real SoundFont 1 bank (.sbk; only a hand-made one),
  and GigaStudio extension files (.gx01).

### Not in the SoundFont player yet

- Keep length and the tempo lock don't apply to SoundFonts (they are Sampler features).
- A SoundFont's zones can't be edited as Samplosis zones yet; the player plays the font as it is.
- sm24, the extra byte of 24-bit samples, is ignored: 24-bit SoundFonts play at 16 bits.
- NRPN and SysEx beyond the GM, GS and XG resets are ignored. There are no data increment and decrement (CC96, CC97).
- Reverb and chorus are not on the extra outputs (those channels are dry).
- Linked modulators are ignored (as in FluidSynth).
- One loader thread and one decode worker serve every instance. Heads load at once whatever is decoding, but
  decodes take turns: while an SF2Pack unpacks (up to about 10 seconds) or a big compressed sample decodes, another
  instance's samples that need decoding wait for it. If the cache folder doesn't exist the first time an SF2Pack is
  used, that pack stays unplayable until it is opened again.
- DLS: a bank with both bank select bytes set uses only the MSB; when a region has its own articulation it replaces
  the instrument's whole articulation (the usual reading of the DLS text); the sustain level is read as 1000 minus
  the level in centibels (Wine and SpessaSynth agree), which can differ by up to 4 dB from other readings.
- SFZ and Decent Sampler: keyswitch keys (sw_lokey to sw_hikey, and sw_last keys) never sound themselves; a release
  region uses the same random draw as its note-on (so it pairs with the attack's round robin); polyphony limits
  count per channel and group; a follower region whose leader was stolen isn't counted by polyphony. Decent
  Sampler's random round robins can repeat the same sample. Formats other than WAV, AIFF, FLAC and Ogg are opened
  once to read their length at the first open (later opens use the cache folder).
- GigaStudio: if libgig crashes on a damaged file, every GigaStudio load is refused until REAPER restarts (other
  formats are fine). Round robins count per region, not per key. File names with characters outside Windows'
  ANSI code page open through 8.3 short names, which the drive must have. Whether ReleaseTriggerDecay 0 means the
  fastest decay or none in GigaStudio itself isn't confirmed (Samplosis follows LinuxSampler).
- The status shows the instrument's first problem (and at most one more), and only the first ignored setting.

## What is still to come

Done so far: stage 1 (samples, keep length; several samples with their own keys and velocities), the stretcher choice, stage 4 (SoundFonts, with 16 stereo outputs
for them), and stage 3, many formats (the sound files and instruments in "File formats", streaming, and loading in
the background). Still to come, from the design (docs\superpowers\specs): round robins, crossfades and per-sample settings in the Sampler, filter and
modulation, the command line tool, the tempo lock, and 16 outputs for the Sampler's zones.
