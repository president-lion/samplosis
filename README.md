# Samplosis

An accessible sampler for REAPER, written in C as a CLAP instrument for Windows. The window is made of plain Windows
controls that NVDA reads as they are. Licence: GPL version 2 (see COPYING).

## What stage 1 does

Stage 1 plays one sample across the keyboard.

- Load one audio file (wav, flac, ogg, mp3 and the other formats the bundled decoders read) with the Browse button,
  by dragging it onto the window, or from a saved REAPER project.
- The root note comes from a note name in the file name ("Guitar C3.wav", "Pad F#2.flac") or, if there is none,
  from pitch detection on the audio.
- Notes keep their length. With Keep length on, a note played above or below the root is made by Rubber Band (the R3
  engine) so it lasts as long as the recording, instead of playing faster or slower. A chord of a guitar note then
  rings out evenly. The pitch-shifted copies are rendered in the background, one per semitone, and kept in the cache
  folder. When a sample loads, the copies for one octave each way of the root are prepared, nearest first. Any other
  key gets its copy the first time it is played, ahead of the copies still being prepared. Until a copy is ready the
  note plays by changing speed, as an ordinary sampler would.
- With Keep length off, pitch changes by changing speed.
- Envelope, loop, start and end points, reverse, speed, velocity response, key and velocity ranges, probability,
  pitch bend and polyphony (up to 64 voices, 16 by default).
- Every control is also a REAPER parameter you can automate.
- The whole instrument can be read and written as plain text (see "The script format").

## The window, in Tab order

Main page:

1. Sample: the loaded file, its length and its root note and where it came from. Read only.
2. Browse button.
3. Previous file button, and 4. Next file button: step through the audio files in the same folder.
5. Keep length (on or off).
6. Mode.
7. Then the other parameters in the order of the list below, from Root note to Speed (the window lays them out in two columns, but Tab follows the list order),
   each one a labelled box (or a combo box for choices).
8. Status: for example "Keep length: 10 of 25 copies ready", or the reason a file did not load.
9. Settings button.

Settings page:

1. Cache folder (edit box), then Browse folder.
2. Cache size limit (in gigabytes).
3. Cache in use (read only).
4. Clear cache button.
5. Back button.

Keys: Tab and Shift+Tab move through the controls and, past the first or last, out to REAPER's FX window. In a value
box, type a value in words and press Enter, or press Up and Down to step it (Shift: ten times as far, Control: a
tenth). Numbers stop at their limits. Choices are combo boxes. Every change applies at once.

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

## The script format

The instrument is plain text, one thing per line. Blank lines and lines starting with # are ignored. After the optional
sample line, each parameter is `name value-in-words`. Names are the lower case ones used by the engine (root,
transpose, fine, volume, pan, attack, decay, sustain, release, velocity_sens, min_volume, mode, note_lo, note_hi,
pitch_lo, vel_lo, vel_hi, obey_note_off, loop, loop_start, start, end, bend_range, pitch_hi, reverse, keep_length,
voices, probability, speed). The sample line has its path in quotes (backslashes literal) and the 16 hex digits of the
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
- The environment variable SAMPLOSIS_SETTINGS names a different settings file (the tests use it).

## Build, install and test

- `build.bat` builds `samplosis.clap`. `build.bat test` also builds `tests\host.exe`, a small CLAP host for the tests.
  It needs Visual Studio's C++ tools and the CLAP headers (set CLAP_SDK to a checkout root to override the default).
- `install.bat` copies `samplosis.clap` to `D:\clap`. If the copy fails, close REAPER first.
- `python -m pytest tests -q` runs the tests (needs numpy, soundfile and psutil). Some of them open test windows.
- `python tests\measure.py` prints the CPU and memory numbers below.

## Measured numbers

Measured on 2026-10-28 with `python tests\measure.py`, through the engine's exported functions, with a temporary
cache folder.

- CPU: 16 voices held, 48 kHz, 256-frame blocks, through smp_engine_render_test, 10 seconds of audio, note range 48
  to 63 (C3 to D#4). Keep length off: 0.84 percent of real time. Keep length on, all copies ready: 1.09 percent. The
  target was under 2 percent. This includes the test helper's small waits, so it is an upper bound for the audio
  thread.
- Memory, per instance, as private bytes of the measuring process from just before smp_engine_new:
  - A new idle engine: 8.9 MB. Nearly all of it is the voice rings, 512 KB for each of the 16 voices the Voices
    parameter allows by default. Rings for more voices are committed only when Voices is raised.
  - With a 60 second stereo sample loaded and keep length on: 12.9 MB, with the 25 copies for one octave each way of
    the root rendered (that took 115 seconds). Each copy keeps only its first quarter second in memory (about 88 KB);
    the rest is read from the cache folder (about 21 MB per copy here) by a background thread as notes play.
  - An earlier version of this README gave a 984 MB working-set peak while rendering. That was the whole Python test
    process (its own sample buffers and file cache pages included), not Rubber Band, and it is no longer reported.

## Known limits

- Keep-length copies exist only within 48 semitones of the root. Keys further away are resampled (pitch by speed).
- The first note after moving Start, End or the loop start may be resampled once per key, until the new copies
  have been rendered.
- Rendering the copies of a long sample takes a long time and a lot of cache space: each copy is as big as the
  decoded sample. Only one octave each way is prepared at load; narrowing the key range (Lowest note, Highest note)
  renders less and frees the copies it no longer reaches.
- The first note on a key more than an octave from the root is resampled; its copy follows in the background.
- One sample per instance. The rest is still to come.

## What is still to come

Stages 2 to 6 of the design (docs\superpowers\specs): several samples and zones, filter and modulation, SoundFonts,
the command line tool and the tempo lock.
