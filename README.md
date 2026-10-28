# Samplosis

An accessible sampler for REAPER, written in C as a CLAP instrument for Windows. The window is made of plain Windows
controls that NVDA reads as they are. Licence: GPL version 2 (see COPYING). It includes Rubber Band (GPL) and
Signalsmith Stretch (MIT, see src\signalsmith).

## What stage 1 does

Stage 1 plays one sample across the keyboard.

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

Main page:

1. Sample: the loaded file, its length and its root note and where it came from. Read only.
2. Browse button.
3. Previous file button, and 4. Next file button: step through the audio files in the same folder.
5. Keep length (on or off).
6. Mode.
7. Then the other parameters in the order of the list below, from Root note to Glide mode (the window lays them out
   in two columns, but Tab follows the list order), each one a slider (or a combo box for choices).
8. Status: for example "Keep length: 10 of 25 copies ready", or the reason a file did not load.
9. Settings button.

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

## The parameters

Values are read and typed in words. Ranges and defaults in brackets.

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
- `python tests\measure.py` prints the CPU and memory numbers below.

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

## Known limits

- Keep-length copies exist only within 48 semitones of the root. Keys further away are resampled (pitch by speed).
- The first note after moving Start, End or the loop start may be resampled once per key, until the new copies
  have been rendered.
- The copies of a long sample take a lot of cache space: each copy is as big as the decoded sample. Copies past a
  quarter of the cache size limit (beyond an octave each way) are made only when their key is first played, and
  that first note is resampled. Narrowing the key range (Lowest note, Highest note) renders less and frees the
  copies it no longer reaches.
- One sample per instance. The rest is still to come.

## What is still to come

Stages 2 to 6 of the design (docs\superpowers\specs): several samples and zones, filter and modulation, SoundFonts,
the command line tool and the tempo lock.
