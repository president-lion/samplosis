# Test fixtures

Files the tests can't make at test time.

## tone.ape

Monkey's Audio, 0.5 s, 16-bit stereo at 44100 Hz (14,488 bytes). ffmpeg reads APE but can't write it, so this file was
made once on 2026-10-29:

1. Python wrote a 16-bit WAV of 22050 frames: left `0.5 * sin(2 pi 220 t)`, right `0.3 * sin(2 pi 331 t)`, with
   `t = n / 44100` in float64, each sample `round(x * 32767)`. `tone_ape_source()` in test_formats.py makes the same
   samples.
2. The console encoder of the Monkey's Audio SDK 13.43 (`Source\Console\Console.cpp` with all of MACLib and Shared,
   built with cl outside this repository) compressed it: `mac.exe tone.wav tone.ape -c2000` (normal).
3. Checked: `ffmpeg -i tone.ape -f s16le -` gives exactly the WAV's samples.
