# Samplosis

An accessible, open-source sampler and instrument player for REAPER. It’s written in C as a CLAP plugin for Windows, and its user interface uses standard native Windows controls so screen readers like NVDA can read them out of the box.

* **Download the latest plugin:** [samplosis.clap](https://github.com/president-lion/samplosis/releases/latest/download/samplosis.clap)
* **Installation:** Drop the file into your CLAP folder (usually `C:\Program Files\Common Files\CLAP`) and rescan plugins in REAPER. Once installed, you can use the "Update Samplosis" button in the plugin settings to fetch future updates automatically.
* **Licence:** GPL v2.

---

## What It Is

Samplosis essentially packs two different instruments into one plugin, toggled via a main tab:

1. **The Sampler:** Maps raw audio files across your keyboard. It handles multi-samples, velocity layers, loops, envelopes, and time-stretching (so notes don't get cartoonishly high or low when you play them away from the root pitch).
2. **The SoundFont Player:** A multi-format instrument player. Beyond standard SoundFonts (`.sf2`, `.sf3`), it also opens SFZ files, Decent Sampler presets (`.dspreset` and `.dslibrary` bundles), Logic EXS24 instruments, DLS banks, and GigaStudio files. 

Every single parameter in the plugin maps to a REAPER automation parameter, and the entire instrument state can also be saved, read, or edited as plain text scripts.

---

## Key Features

### The Sampler
* **Format Support:** Loads WAV, FLAC, Ogg, MP3, Opus, WavPack, Monkey's Audio, and anything Windows Media Foundation natively handles (AAC, WMA, etc.). If you point it to FFmpeg, it will also handle DSF/DFF (DSD), TTA, and video container audio.
* **Smart Root Notes:** Automatically detects root notes from file names (e.g., `Guitar C3.wav`) or analyses the audio pitch if there isn't one.
* **Keep Length:** Uses time-stretching (choose between Signalsmith Stretch or Rubber Band) so that notes played across different keys retain their natural length instead of speeding up or slowing down.
* **Background Rendering:** Pitch-shifted copies are generated asynchronously in the background and cached to disk so they are ready instantly when you play.
* **Multi-Sample Support:** Load up to 128 samples per instance. Give each its own key range, velocity split, and playback mode.

### The SoundFont / Instrument Player
* **Streaming:** Instead of hogging RAM, large instruments stream audio directly from disk. Only the first quarter-second of a sample is kept in memory to eliminate latency on note-on; the rest streams seamlessly.
* **Full MIDI Integration:** Operates across 16 MIDI channels, respecting program changes, controllers, pitch bends, pressure, and GM/GS/XG resets.
* **Decent Sampler Knobs:** Automatically maps a preset's custom on-screen UI knobs to accessible sliders and MIDI CC bindings.
* **Outputs:** Provides 16 stereo outputs. You can route specific MIDI channels to separate plugin outputs in REAPER for external mixing.

---

## File Formats Supported

* **Audio/Samples:** `.wav`, `.flac`, `.mp3`, `.ogg`, `.aif`, `.w64`, `.caf`, `.opus`, `.wv`, `.ape`, `.m4a`, `.aac`, `.wma`, `.dsf`, `.tta`, and more.
* **SoundFonts & Instruments:** 
  * SoundFont 2 & 3 (`.sf2`, `.sf3`)
  * SF4 / SF2Pack / SoundFont 1 (`.sbk`)
  * Downloadable Sounds (`.dls`)
  * GigaStudio (`.gig`)
  * SFZ (`.sfz`)
  * Decent Sampler (`.dspreset`, `.dslibrary`)
  * Logic EXS24 (`.exs`)

---

## Settings & Configuration

Settings are stored locally in `%APPDATA%\Samplosis\settings.txt`. 

* **Cache Folder:** Where decoded audio, reversed files, and stretched pitch-copies live. You can clear this safely at any time; it will just rebuild what it needs.
* **Stretcher Engine:** Choose between **Signalsmith Stretch** (fast, default) and **Rubber Band** (higher quality, slower).
* **Declick:** Automatically applies 2-ms micro-fades to sample starts, ends, and imperfect loop points to prevent digital audio pops.
* **FFmpeg Path:** Optional. If pointed to an FFmpeg installation, Samplosis unlocks extra esoteric audio codecs and damaged file recovery.

---

## Building and Testing

If you want to build it from source:

1. Run `build.bat` to compile `samplosis.clap` (requires Visual Studio C++ tools and the CLAP SDK).
2. Run `build.bat test` to also build the test host.
3. Run `install.bat` to copy the plugin to your default CLAP directory.
4. Run python tests with: `python -m pytest tests -q`