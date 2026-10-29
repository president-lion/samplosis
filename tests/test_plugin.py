"""The CLAP plugin, driven through tests\\host.exe (a stand-in host that loads samplosis.clap by clap_entry)."""
import ctypes
import os
import random
import subprocess

import numpy as np
import pytest
import soundfile as sf

from conftest import ROOT, write_wav
from test_cache import point_cache
from test_sfsynth import write_font as write_sf_font

HOST = os.path.join(ROOT, "tests", "host.exe")
CLAP = os.path.join(ROOT, "samplosis.clap")
NP = 35         # the sampler's 31 and the SoundFont player's 4
VOLUME = 3


def host(*args, timeout=120):
    r = subprocess.run([HOST, CLAP, *[str(a) for a in args]], capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout, r.stderr


def lines(out):
    d = {}
    for ln in out.splitlines():
        if ": " in ln:
            k, v = ln.split(": ", 1)
            d.setdefault(k, v)
    return d


def saved_state(out):
    assert "--- state\n" in out, out
    return out.split("--- state\n", 1)[1]


@pytest.fixture
def P(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    lib.smp_script_write.restype = ctypes.c_int
    lib.smp_script_write.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    lib.smp_script_apply.restype = ctypes.c_int
    lib.smp_script_apply.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]

    class H:
        def sample(self, name="tone A3.wav", secs=0.8, rate=44100):
            f = tmp_path / name
            t = np.arange(int(secs * rate)) / rate
            write_wav(f, (0.4 * np.sin(2 * np.pi * 220 * t) + 0.1 * np.sin(2 * np.pi * 557 * t)).astype(np.float32), rate)
            return f

        def script(self, wav, extra=""):
            """A state text for wav, made by the library itself."""
            e = lib.smp_engine_new(48000.0, 256)
            r, err = self.apply(e, 'sample "%s"\nkeep_length off\nrelease 50 ms\n%s' % (wav, extra))
            assert r == 0, err
            n = lib.smp_script_write(e, None, 0)
            buf = ctypes.create_string_buffer(n + 1)
            lib.smp_script_write(e, buf, n + 1)
            lib.smp_engine_free(e)                      # closes its cache files
            return buf.value.decode()

        def apply(self, e, text):
            err = ctypes.create_string_buffer(512)
            return lib.smp_script_apply(e, text.encode(), err, 512), err.value.decode()

        def state_file(self, text, name="state.txt"):
            p = tmp_path / name
            p.write_bytes(text.encode())
            return p

        def reference(self, text, events, frames, rate=48000):
            """The same instrument and notes through smp_engine_render_test."""
            e = lib.smp_engine_new(float(rate), 256)
            r, err = self.apply(e, text)
            assert r == 0, err
            out = tmp_path / "ref.wav"
            lib.smp_engine_wait_renders.restype = ctypes.c_int
            lib.smp_engine_wait_renders.argtypes = [ctypes.c_void_p, ctypes.c_int]
            assert lib.smp_engine_wait_renders(e, 120000) == 0
            assert lib.smp_engine_render_test(e, events.encode(), frames, 256, str(out).encode()) == 0
            lib.smp_engine_free(e)
            y, _ = sf.read(str(out), dtype="float32")
            return y

        def play(self, text, events, frames, rate=None, all_outputs=False):
            st = self.state_file(text)
            ev = tmp_path / "events.txt"
            ev.write_text(events + "\nlength %d\n" % frames)
            out = tmp_path / "plug.wav"
            args = ["play", st, ev, out] + ([rate or 48000, "all"] if all_outputs else [rate] if rate else [])
            code, so, se = host(*args)
            assert code == 0, so + se
            y, r = sf.read(str(out), dtype="float32")
            assert y.shape == (frames, 32 if all_outputs else 2)
            return y, lines(so), r

        def sf_defaults(self):
            """The SoundFont player's lines with nothing loaded and nothing set, as a saved state ends."""
            s = lib.smp_synth_new(48000.0, 256)
            n = lib.smp_synth_script_write(s, None, 0)
            buf = ctypes.create_string_buffer(n + 1)
            lib.smp_synth_script_write(s, buf, n + 1)
            lib.smp_synth_free(s)
            return buf.value.decode()

        def font(self, name="gm.sf2"):
            f = tmp_path / name
            write_sf_font(f)
            return f

        def sf_reference(self, text, events, frames, all_outputs=0, rate=48000):
            """The same SoundFont state and MIDI through smp_synth_render_test (it waits for loads: exact)."""
            s = lib.smp_synth_new(float(rate), 256)
            err = ctypes.create_string_buffer(1024)
            assert lib.smp_synth_script_apply(s, text.encode(), err, 1024) == 0, err.value
            out = tmp_path / "sfref.wav"
            assert lib.smp_synth_render_test(s, events.encode(), frames, 256, str(out).encode(), all_outputs) == 0
            lib.smp_synth_free(s)
            y, _ = sf.read(str(out), dtype="float32", always_2d=True)
            return y

    return H()


def db(x):
    return 20 * np.log10(np.sqrt(np.mean(np.square(x, dtype=np.float64))) + 1e-12)


def test_descriptor(P):
    code, out, err = host("state", P.state_file("# Samplosis 0.1.0\n"))
    assert code == 0, out + err
    d = lines(out)
    assert d["name"] == "Samplosis"
    assert d["vendor"] == "audioskit"
    assert d["version"] == "0.1.0"
    assert d["id"] == "com.audioskit.samplosis"
    assert set(d["features"].split()) >= {"instrument", "sampler", "stereo"}
    assert d["params"] == str(NP)
    assert d["note ports"] == "1 in, clap midi, prefers midi"
    assert d["audio ports"] == "16 out, 2 channels"
    assert d["audio port names"] == ", ".join(["Output 1 (main)"] + ["Output %d" % i for i in range(2, 17)])
    assert d["latency"] == "0"


def test_param_info(P):
    code, out, err = host("state", P.state_file("# Samplosis 0.1.0\n"))
    assert code == 0, out + err
    params = [ln for ln in out.splitlines() if ln.startswith("param ")]
    assert len(params) == NP
    assert params[3].startswith("param 3 Volume -60 12 0 automatable") and "text 0.0 dB" in params[3]
    assert "stepped" in params[11] and "text note" in params[11]          # mode: a choice
    assert "stepped" in params[24]                                         # reverse: on or off
    assert "stepped" not in params[3]
    assert "parse -6 dB = -6" in out


@pytest.mark.parametrize("rate", [48000, 44100])
def test_play_through_clap(P, rate):
    text = P.script(P.sample())
    frames = rate
    events = "on 60 100 0\noff 60 %d" % (rate // 2)
    ref = P.reference(text, events, frames, rate)
    y, d, r = P.play(text, events, frames, rate)
    assert r == rate
    assert db(ref) > -40
    assert db(y - ref) - db(ref) < -90, d
    # the same note as MIDI bytes
    y2, d, r = P.play(text, "midi 144 60 100 0\nmidi 128 60 0 %d" % (rate // 2), frames, rate)
    assert db(y2 - ref) - db(ref) < -90
    # MIDI note-on with velocity 0 is a note-off
    y3, d, r = P.play(text, "midi 144 60 100 0\nmidi 144 60 0 %d" % (rate // 2), frames, rate)
    assert db(y3 - ref) - db(ref) < -90


def test_pitch_bend_midi(P):
    text = P.script(P.sample())
    events = "on 57 100 0\nbend 0.5 4000\noff 57 30000"
    ref = P.reference(text, events, 48000)
    y, d, r = P.play(text, "on 57 100 0\nmidi 224 0 96 4000\noff 57 30000", 48000)   # 96 << 7 = 12288: +0.5
    assert db(y - ref) - db(ref) < -90


def test_state_roundtrip(lib, P):
    f = P.sample()
    rnd = random.Random(3)
    extra = []
    for i in (1, 2, 3, 4, 5, 8, 11, 18, 24, 26):
        info = lib.smp_param_info(i).contents
        v = ctypes.create_string_buffer(64)
        lib.smp_value_text(i, rnd.uniform(info.lo, info.hi), v, 64)
        extra.append("%s %s" % (info.name.decode(), v.value.decode()))
    text = P.script(f, "\n".join(extra))
    code, out, err = host("state", P.state_file(text))
    assert code == 0, out + err
    assert saved_state(out) == "plays sampler\n" + text + P.sf_defaults()
    assert lines(out)["status"].startswith("Ready")


def test_bad_state_refused(P):
    code, out, err = host("state", P.state_file("# Samplosis 0.1.0\nvolme -3\n"))
    assert code == 0, out + err
    d = lines(out)
    assert d["load"] == "refused"
    assert "line 2" in d["status"]
    for bad in ('sample "E:/nowhere/x.wav', 'sample "E:/nowhere/x.wav" key=12'):   # a broken sample line isn't "missing"
        code, out, err = host("state", P.state_file("# Samplosis 0.1.0\n%s\nvolume -3\n" % bad))
        d = lines(out)
        assert d["load"] == "refused", bad
        assert "line 2" in d["status"], bad


def test_param_events(P):
    text = P.script(P.sample(secs=0.8))
    frames = 12000
    ref = P.reference(text, "on 57 100 0", frames)
    y, d, r = P.play(text, "on 57 100 0\nparam 3 -12 1000", frames)    # frame 1000 is inside the block at 768
    assert np.max(np.abs(y[:1000] - ref[:1000])) == 0
    g = 10 ** (-12 / 20)
    assert db(y[1000:] - ref[1000:] * g) - db(ref[1000:] * g) < -90
    assert np.max(np.abs(y[999] - ref[999])) == 0 and abs(y[1000, 0]) < abs(ref[1000, 0]) * 0.3


def test_state_with_missing_file(P, tmp_path):
    f = P.sample("gone A3.wav")
    text = P.script(f)
    events = "on 57 100 0\noff 57 20000"
    ref = P.reference(text, events, 48000)
    f.unlink()
    # the file is gone but its copy is in the cache: it plays
    y, d, r = P.play(text, events, 48000)
    assert d["status"].startswith("Ready")
    assert db(y - ref) - db(ref) < -90
    # both gone: silent, the status says so, and saving keeps the path
    for p in (tmp_path / "cache").iterdir():
        if p.suffix == ".smpc":
            p.unlink()
    y, d, r = P.play(text, events, 48000)
    assert d["status"] == "Missing: %s" % f
    assert np.max(np.abs(y)) == 0
    code, out, err = host("state", P.state_file(text, "s2.txt"))
    assert code == 0, out + err
    assert lines(out)["status"] == "Missing: %s" % f
    assert saved_state(out) == "plays sampler\n" + text + P.sf_defaults()


def test_streamed_keep_length_start(P):
    # a sample longer than the cache head, keep length on (one key, so one copy), started half way in
    text = P.script(P.sample("long A3.wav", secs=3.0), "keep_length on\nnote_lo C4\nnote_hi C4\nstart 50")
    assert "keep_length on" in text
    events = "on 60 100 0\noff 60 60000"
    ref = P.reference(text, events, 72000)
    y, d, r = P.play(text, events, 72000)
    assert db(ref) > -40
    assert db(y - ref) - db(ref) < -90


def test_state_without_sample_clears_it(P, tmp_path):
    # an empty preset (no sample line) after one with a sample: silent, "No sample loaded"
    text = P.script(P.sample())
    empty = "# Samplosis 0.1.0\nvolume -3.0 dB\n"
    st2 = P.state_file(empty, "empty.txt")
    y, d, r = P.play(text, "on 57 100 0\nstate %s 0" % st2, 24000)
    assert d["status after"] == "No sample loaded"
    assert np.max(np.abs(y)) == 0


def test_reset_kills_hanging_voices(P):
    # a looping note held with no note-off; reset() between blocks ends it within 10 ms
    text = P.script(P.sample(), "loop forward")
    at = 19 * 256
    y, d, r = P.play(text, "on 57 100 0\nreset %d" % at, 24000)
    assert np.max(np.abs(y[at - 2000:at])) > 0.05
    assert np.max(np.abs(y[at + 480:])) == 0
    assert d["voices"] == "0"
    # obey note off off: the note ignores note-off, a reset still ends it
    text = P.script(P.sample(secs=3.0, name="long A3.wav"), "obey_note_off off")
    y, d, r = P.play(text, "on 57 100 0\noff 57 100\nreset %d" % at, 24000)
    assert np.max(np.abs(y[at - 2000:at])) > 0.05
    assert np.max(np.abs(y[at + 480:])) == 0


def test_reactivate_is_silent(P):
    text = P.script(P.sample(), "loop forward")
    at = 19 * 256
    y, d, r = P.play(text, "on 57 100 0\ncycle %d" % at, 24000)
    assert np.max(np.abs(y[at - 2000:at])) > 0.05
    assert np.max(np.abs(y[at:])) == 0
    assert d["voices"] == "0"


def test_note_at_block_end_clamped(P):
    # a note whose time is past the block (a sloppy host) still plays, on the block's last frame
    text = P.script(P.sample())
    y, d, r = P.play(text, "rawon 57 100 0 256", 4000)
    nz = np.nonzero(np.abs(y[:, 0]) > 0)[0]
    assert len(nz) and nz[0] >= 255 and nz[0] < 300


# ---- the SoundFont player ----
def first_sound(y, th=1e-6):
    idx = np.nonzero(np.max(np.abs(y), axis=1) > th)[0]
    return int(idx[0]) if len(idx) else None


def peak_hz(x, rate=48000):
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    return np.argmax(spec) * rate / len(x)


def test_plugin_plays_soundfont(P):
    f = P.font()
    text = 'plays soundfont\nsoundfont "%s"\nsf_used 0:0 0:33 128:0\n' % f
    frames = 48000
    # program 33 (the bass) on channel 2, then a note there; the host's MIDI events carry 3 bytes, the reference 2
    ref = P.sf_reference(text, "midi 193 33 0\nmidi 145 57 100 0\nmidi 129 57 0 24000", frames)
    y, d, r = P.play(text, "midi 193 33 0 0\nmidi 145 57 100 0\nmidi 129 57 0 24000", frames)
    assert db(ref) > -40
    assert db(y - ref) - db(ref) < -90, d
    assert abs(peak_hz(y[4800:20000, 0]) - 110) < 3            # the bass an octave under the piano's 220 Hz
    assert d["status after"] == "Ready: gm.sf2, 6 presets"
    # a CLAP note goes to the player as MIDI on its channel (the host sends channel 0: MIDI channel 1)
    ref = P.sf_reference(text, "midi 144 57 100 0\nmidi 128 57 0 24000", frames)
    y, d, r = P.play(text, "on 57 100 0\noff 57 24000", frames)
    assert db(y - ref) - db(ref) < -90
    assert abs(peak_hz(y[4800:20000, 0]) - 220) < 3
    # at another rate, the player is made again with the same state
    ref = P.sf_reference(text, "midi 144 57 100 0\nmidi 128 57 0 22050", 44100, rate=44100)
    y, d, r = P.play(text, "on 57 100 0\noff 57 22050", 44100, rate=44100)
    assert r == 44100
    assert db(y - ref) - db(ref) < -90


def test_plugin_old_state_is_sampler(P):
    text = P.script(P.sample())                          # a stage 1 state: no plays line
    events = "on 57 100 0\noff 57 24000"
    ref = P.reference(text, events, 48000)
    y, d, r = P.play(text, events, 48000)
    assert db(ref) > -40
    assert db(y - ref) - db(ref) < -90
    assert d["status"] == "Ready: tone A3.wav"
    # with every output: the sampler writes output 1, the rest stay silent
    y, d, r = P.play(text, events, 48000, all_outputs=True)
    assert db(y[:, :2] - ref) - db(ref) < -90
    assert np.max(np.abs(y[:, 2:])) == 0
    code, out, err = host("state", P.state_file(text, "old.txt"))
    assert code == 0, out + err
    assert saved_state(out).startswith("plays sampler\n" + text)


def test_plugin_outputs(P):
    f = P.font()
    text = 'plays soundfont\nsoundfont "%s"\nsf_used 0:0 0:33 128:0\nsf_output 2 3\n' % f
    frames = 24000
    ref = P.sf_reference(text, "midi 145 57 100 0", frames, all_outputs=1)
    y, d, r = P.play(text, "midi 145 57 100 0", frames, all_outputs=True)
    assert db(y[:, 4:6]) > -40
    assert db(y - ref) - db(ref) < -90
    others = [c for c in range(32) if c not in (4, 5)]
    assert np.max(np.abs(y[:, others])) == 0
    # a host that gives only the main output: channel 2 falls back to it
    y, d, r = P.play(text, "midi 145 57 100 0", frames)
    assert db(y) > -40


def test_plugin_offline_render_waits(P):
    f = P.font()
    text = 'plays soundfont\nsoundfont "%s"\n' % f        # nothing loaded ahead
    frames = 12000
    events = "midi 193 33 0 0\nmidi 145 57 100 0"
    ref = P.sf_reference(text, "midi 193 33 0\nmidi 145 57 100 0", frames)
    y, d, r = P.play(text, "render offline 0\n" + events, frames)
    assert first_sound(ref) is not None and first_sound(ref) < 64     # from frame 0 (the attack's first frames are tiny)
    assert first_sound(y) == first_sound(ref)
    assert db(y - ref) - db(ref) < -90
    # in real time the samples load while the audio runs on: the note starts late, at a later block
    y, d, r = P.play(text, events, frames)
    assert first_sound(y) is None or first_sound(y) >= 256


def test_plugin_font_missing(P, tmp_path):
    f = P.font("gone.sf2")
    text = ('plays soundfont\nsoundfont "%s"\nsf_channel_preset 3 0 25\nsf_output 2 3\nsf_used 0:0 0:33\n'
            'sf_volume -3\n' % f)
    code, out, err = host("state", P.state_file(text))
    assert code == 0, out + err
    st = lines(out)["status"]
    assert st == "Ready: gone.sf2, 6 presets" or st in ("Loading samples for 1 preset", "Loading samples for 2 presets")
    saved = saved_state(out)
    assert saved.startswith('plays soundfont\n# Samplosis 0.1.0\n')
    assert ('soundfont "%s"\n' % f) in saved and "sf_used 0:0 0:33\n" in saved and "sf_volume -3.0 dB\n" in saved
    f.unlink()
    code, out, err = host("state", P.state_file(saved, "s2.txt"))
    assert code == 0, out + err
    d = lines(out)
    assert d["load"] == "ok"
    assert d["status"] == "Missing: %s" % f
    assert saved_state(out) == saved
    y, d, r = P.play(saved, "midi 144 57 100 0", 4800)
    assert np.max(np.abs(y)) == 0


SF_PARAMS = ["param 31 SoundFont volume -60 12 0 automatable text 0.0 dB",
             "param 32 SoundFont voices 16 1024 256 automatable stepped text 256",
             "param 33 Reverb and chorus 0 1 1 automatable stepped text on",
             "param 34 SoundFont transpose -24 24 0 automatable stepped text +0 st"]


def test_plugin_params(P):
    code, out, err = host("state", P.state_file("# Samplosis 0.1.0\n"))
    assert code == 0, out + err
    params = [ln for ln in out.splitlines() if ln.startswith("param ")]
    assert params[31:35] == SF_PARAMS, params[31:35]
    f = P.font()
    text = 'plays soundfont\nsoundfont "%s"\nsf_used 0:0 0:33 128:0\n' % f
    events = "midi 144 57 100 0"
    y0, d, r = P.play(text, events, 24000)
    y1, d, r = P.play(text, events + "\nparam 31 -6 0", 24000)
    assert abs((db(y1[256:]) - db(y0[256:])) - -6) < 0.1
    # transpose +12 through the host: an octave up
    y2, d, r = P.play(text, events + "\nparam 34 12 0", 24000)
    assert abs(peak_hz(y2[4800:20000, 0]) - 440) < 5


# ---- the window, driven by messages through host.exe's gui command ----
MAIN_ORDER = [
    ("Edit", "Sample"), ("Button", "Browse"), ("Button", "Previous file"), ("Button", "Next file"),
    ("ComboBox", "Keep length"), ("ComboBox", "Mode"),
    ("Slider", "Root note"), ("Slider", "Transpose"), ("Slider", "Fine tune"), ("Slider", "Volume"), ("Slider", "Pan"),
    ("Slider", "Attack"), ("Slider", "Decay"), ("Slider", "Sustain"), ("Slider", "Release"),
    ("Slider", "Velocity sensitivity"), ("Slider", "Minimum volume"), ("Slider", "Lowest note"),
    ("Slider", "Highest note"), ("Slider", "Pitch at lowest note"), ("Slider", "Lowest velocity"),
    ("Slider", "Highest velocity"), ("ComboBox", "Obey note off"), ("ComboBox", "Loop"), ("Slider", "Loop start"),
    ("Slider", "Start"), ("Slider", "End"), ("Slider", "Bend range"), ("Slider", "Pitch at highest note"),
    ("ComboBox", "Reverse"), ("Slider", "Voices"), ("Slider", "Probability"), ("Slider", "Speed"), ("Slider", "Glide"),
    ("ComboBox", "Glide mode"), ("Edit", "Status"), ("Button", "Settings"),
]


def gui(P, tmp_path, text, actions):
    """Opens the window on a state and runs actions; returns the printed lines."""
    st = P.state_file(text, "gui_state.txt")
    sc = tmp_path / "gui_script.txt"
    sc.write_text("\n".join(actions) + "\n")
    code, out, err = host("gui", st, sc, timeout=120)
    assert code == 0, out + err
    return out


def got(out, tag):
    """Each `tag: class | name | value` line, split."""
    return [ln[len(tag) + 2:].split(" | ") for ln in out.splitlines() if ln.startswith(tag + ": ")]


def test_gui_tab_order(P, tmp_path):
    text = P.script(P.sample())
    out = gui(P, tmp_path, text, ["say"] + ["key tab"] * len(MAIN_ORDER) +
              ["focus Sample", "key tab shift", "focus Volume", "window", "say"])
    f = got(out, "focus")
    assert [x[:2] for x in f[:len(MAIN_ORDER)]] == [list(x) for x in MAIN_ORDER], out   # opens on the first control
    assert f[len(MAIN_ORDER)][:2] == ["Button", "after"], out
    assert f[len(MAIN_ORDER) + 1][:2] == ["Button", "before"], out
    assert f[-1][:2] == ["Slider", "Volume"], out        # the window focused: on to the last control used


def test_gui_values_in_words(P, tmp_path):
    text = P.script(P.sample("tone C3.wav"))
    out = gui(P, tmp_path, text, ["dump"])
    d = {x[1]: x[2] for x in got(out, "control")}
    assert d["Root note"] == "C3"
    assert d["Volume"] == "0.0 dB"
    assert d["Pan"] == "center"
    assert d["Attack"] == "0 ms"
    assert d["Glide"] == "off" and d["Glide mode"] == "Always"
    assert d["Keep length"] == "Off" and d["Mode"] == "Note"      # P.script turns keep length off
    assert d["Sample"] == "tone C3.wav, 0.80 s, root C3 from the file name"
    assert d["Status"] == "Ready: tone C3.wav"


def test_gui_up_down_limits(P, tmp_path):
    text = P.script(P.sample(), "volume 10\nvoices 1")
    out = gui(P, tmp_path, text, ["focus Volume"] + ["key up"] * 13 + ["param 3", "focus Voices", "key down",
              "key up shift", "focus Attack", "key up", "key up shift", "key up ctrl", "key down shift",
              "key down shift"])
    v = [x[2] for x in got(out, "focus")]
    assert v[:3] == ["11.0 dB", "12.0 dB", "12.0 dB"]
    assert v[12] == "12.0 dB"
    assert "param 3: 12" in out
    assert v[13:] == ["1", "11", "10 ms", "110 ms", "111 ms", "11 ms", "0 ms"]


def test_gui_typed_value(P, tmp_path):
    # Enter on a slider: an edit box in its place, holding the value; Enter reads it and goes back to the slider
    text = P.script(P.sample())
    out = gui(P, tmp_path, text, ["focus Volume", "key enter", "type -6", "key enter", "param 3", "flush", "counts",
                                  "key enter", "type loud", "key enter", "param 3", "counts",
                                  "key enter", "type -3", "key escape", "param 3",
                                  "char 2", "key enter", "param 3",
                                  "key enter", "type -3", "key tab", "param 3", "key tab shift"])
    f = got(out, "focus")
    assert f[0] == ["Edit", "Volume", "0.0 dB"]                           # the box, named like its slider
    assert f[1] == ["Slider", "Volume", "-6.0 dB"]
    assert "param 3: -6" in out
    assert "out: gesture_begin 3" in out and "out: value 3 -6" in out and "out: gesture_end 3" in out
    c = got(out, "counts")
    assert c[0][0] == "beeps 0" and c[0][1] != "request_flush 0"
    assert f[3] == ["Slider", "Volume", "-6.0 dB"] and c[1][0] == "beeps 1"    # "loud" beeps; the value stays
    assert f[5] == ["Slider", "Volume", "-6.0 dB"] and out.count("param 3: -6") == 3   # Escape: as it was
    assert f[6] == ["Edit", "Volume", "2"] and f[7] == ["Slider", "Volume", "2.0 dB"]  # typing starts the box
    assert "param 3: 2" in out
    assert f[9][:2] == ["Slider", "Pan"] and "param 3: -3" in out        # Tab out of the box applies it too
    assert f[10] == ["Slider", "Volume", "-3.0 dB"]                       # and the slider is back in its place


def test_gui_slider_keys(P, tmp_path):
    text = P.script(P.sample(), "attack 100 ms")
    out = gui(P, tmp_path, text, ["focus Attack", "key right", "key left", "key left", "key pageup", "key pagedown",
                                  "key end", "key up", "key home", "key down",
                                  "focus Glide", "key up", "key up shift", "key enter", "type 1.5 s", "key enter",
                                  "key home", "param 29"])
    v = [x[2] for x in got(out, "focus")]
    assert v[:9] == ["110 ms", "100 ms", "90 ms", "190 ms", "90 ms", "10.00 s", "10.00 s", "0 ms", "0 ms"]
    assert v[9:] == ["10 ms", "110 ms", "110 ms", "1.50 s", "off"]
    assert "param 29: 0" in out


def test_gui_next_file(P, tmp_path):
    d = tmp_path / "files"
    d.mkdir()
    for n in ("a.wav", "B.wav", "c.wav"):
        P.sample(n)
        os.replace(tmp_path / n, d / n)
    (d / "notes.txt").write_text("not audio")
    text = P.script(d / "B.wav")
    out = gui(P, tmp_path, text, ["read Sample", "focus Next file", "key enter", "read Sample", "key enter",
                                  "read Sample", "focus Previous file", "key space", "read Sample", "state", "counts"])
    r = [x[1] for x in got(out, "read")]
    assert r[0].startswith("B.wav, 0.80 s, root ")
    assert r[1].startswith("c.wav, ") and r[2].startswith("a.wav, ") and r[3].startswith("c.wav, ")
    assert ('sample "%s"' % (d / "c.wav")) in out
    assert got(out, "counts")[0][2] != "mark_dirty 0"


def test_gui_settings_page(P, tmp_path, settings_file):
    text = P.script(P.sample())
    with open(settings_file) as fh:
        before = fh.read()
    try:
        out = gui(P, tmp_path, text, ["focus Settings", "key enter", "key down", "key down", "key tab", "key down",
                                      "key tab", "key tab", "key tab", "key enter", "type 5", "key enter",
                                      "key up", "key down", "key tab", "key tab", "key tab", "key tab",
                                      "focus Back", "key enter", "focus Back", "say"])
        f = got(out, "focus")
        assert f[0] == ["ComboBox", "Stretcher", "Signalsmith Stretch, fast"]
        assert f[1] == f[2] == ["ComboBox", "Stretcher", "Rubber Band, slower"]   # Down stops at the last
        assert f[3] == ["ComboBox", "Keep the tone", "Off"] and f[4] == ["ComboBox", "Keep the tone", "On"]
        assert f[5][:3] == ["Edit", "Cache folder", str(tmp_path / "cache")]
        assert f[6][:2] == ["Button", "Browse folder"]
        assert f[7] == ["Slider", "Cache size limit", "10 GB"]
        assert f[8] == ["Edit", "Cache size limit", "10 GB"]
        assert f[9][2] == "5 GB" and f[10][2] == "6 GB" and f[11][2] == "5 GB"
        assert f[12][:2] == ["Edit", "Cache in use"] and f[12][2].endswith(" of 5 GB")
        assert [x[:2] for x in f[13:16]] == [["Button", "Clear cache"], ["Button", "Back"], ["Button", "after"]]
        assert f[16][:2] == ["Button", "Settings"]           # Back: on the Settings button again
        assert f[17][:2] == ["Button", "Settings"]           # Back is gone with its page: nothing to focus by that name
        with open(settings_file) as fh:
            saved = fh.read()
        assert "cache_limit_gb 5\n" in saved and "stretcher rubberband\n" in saved and "keep_tone on\n" in saved
    finally:
        with open(settings_file, "w") as fh:
            fh.write(before)


def test_gui_keep_length_combo(P, tmp_path):
    text = P.script(P.sample(), "keep_length on")
    out = gui(P, tmp_path, text, ["focus Keep length", "say", "key up", "param 25", "flush",
                                  "focus Mode", "say", "key down", "key down", "param 11"])
    f = got(out, "focus")
    assert f[0] == ["ComboBox", "Keep length", "On"] and f[1] == ["ComboBox", "Keep length", "Off"]
    assert "param 25: 0" in out
    assert "out: value 25 0" in out
    assert [x[2] for x in f[2:]] == ["Note", "Free", "Free"]
    assert "param 11: 2" in out


def test_gui_closed_during_dialog(P, tmp_path, settings_file):
    # A file or folder dialog runs its own message loop: REAPER can close the window, or free the whole plugin,
    # before it returns. Nothing may load or touch freed memory then. (Dialogs stubbed: no real dialog opens.)
    d = tmp_path / "files"
    d.mkdir()
    for n in ("b.wav", "c.wav"):
        P.sample(n)
        os.replace(tmp_path / n, d / n)
    text = P.script(d / "b.wav")
    c = d / "c.wav"
    out = gui(P, tmp_path, text, ["dialog pick %s" % c, "focus Browse", "key enter", "read Sample", "say"])
    assert got(out, "read")[0][1].startswith("c.wav, ")
    assert got(out, "focus")[-1][:2] == ["Button", "Browse"]
    out = gui(P, tmp_path, text, ["dialog closegui %s" % c, "focus Browse", "key enter", "pump 600", "state", "alive"])
    assert ('state: sample "%s"' % (d / "b.wav")) in out          # not loaded: the window had gone
    assert "alive: plugin\n" in out and "closed" in out
    out = gui(P, tmp_path, text, ["dialog destroy %s" % c, "focus Browse", "key enter", "pump 600", "alive"])
    assert "alive: plugin destroyed" in out and "closed" in out
    # the Settings page's Browse folder
    new = tmp_path / "cache2"
    out = gui(P, tmp_path, text, ["focus Settings", "key enter", "dialog closegui %s" % new, "focus Browse folder",
                                  "key enter", "pump 600", "alive"])
    assert "alive: plugin\n" in out and "closed" in out
    with open(settings_file) as fh:
        assert str(new) not in fh.read()
    out = gui(P, tmp_path, text, ["focus Settings", "key enter", "dialog pick %s" % new, "focus Browse folder",
                                  "key enter", "say"])
    assert got(out, "focus")[-1][:2] == ["Button", "Browse folder"]
    with open(settings_file) as fh:
        assert ("cache_dir %s\n" % new) in fh.read()
