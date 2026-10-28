# Samplosis

Accessible CLAP sampler for Windows (REAPER), written in C.

- `build.bat` builds `samplosis.clap`; `build.bat test` also builds `tests\host.exe`.
- `install.bat` copies it to `D:\clap`.
- `python -m pytest tests -q` runs the tests. Tests use the `lib` fixture (the DLL via ctypes),
  `settings_file`, and `from conftest import write_wav` (pytest puts tests\ on sys.path).
- Set `CLAP_SDK` to a clap checkout root to override the default header location.
