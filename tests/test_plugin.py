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

HOST = os.path.join(ROOT, "tests", "host.exe")
CLAP = os.path.join(ROOT, "samplosis.clap")
NP = 29
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

        def play(self, text, events, frames, rate=None):
            st = self.state_file(text)
            ev = tmp_path / "events.txt"
            ev.write_text(events + "\nlength %d\n" % frames)
            out = tmp_path / "plug.wav"
            args = ["play", st, ev, out] + ([rate] if rate else [])
            code, so, se = host(*args)
            assert code == 0, so + se
            y, r = sf.read(str(out), dtype="float32")
            assert y.shape == (frames, 2)
            return y, lines(so), r

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
    assert d["note ports"] == "1 in, clap midi"
    assert d["audio ports"] == "1 out, 2 channels"
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
    assert saved_state(out) == text
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
    assert saved_state(out) == text


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


# ---- the window, driven by messages through host.exe's gui command ----
MAIN_ORDER = [
    ("Edit", "Sample"), ("Button", "Browse"), ("Button", "Previous file"), ("Button", "Next file"),
    ("ComboBox", "Keep length"), ("ComboBox", "Mode"),
    ("Edit", "Root note"), ("Edit", "Transpose"), ("Edit", "Fine tune"), ("Edit", "Volume"), ("Edit", "Pan"),
    ("Edit", "Attack"), ("Edit", "Decay"), ("Edit", "Sustain"), ("Edit", "Release"),
    ("Edit", "Velocity sensitivity"), ("Edit", "Minimum volume"), ("Edit", "Lowest note"), ("Edit", "Highest note"),
    ("Edit", "Pitch at lowest note"), ("Edit", "Lowest velocity"), ("Edit", "Highest velocity"),
    ("ComboBox", "Obey note off"), ("ComboBox", "Loop"), ("Edit", "Loop start"), ("Edit", "Start"), ("Edit", "End"),
    ("Edit", "Bend range"), ("Edit", "Pitch at highest note"), ("ComboBox", "Reverse"), ("Edit", "Voices"),
    ("Edit", "Probability"), ("Edit", "Speed"), ("Edit", "Status"), ("Button", "Settings"),
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
    assert f[-1][:2] == ["Edit", "Volume"], out          # the window focused: on to the last control used


def test_gui_values_in_words(P, tmp_path):
    text = P.script(P.sample("tone C3.wav"))
    out = gui(P, tmp_path, text, ["dump"])
    d = {x[1]: x[2] for x in got(out, "control")}
    assert d["Root note"] == "C3"
    assert d["Volume"] == "0.0 dB"
    assert d["Pan"] == "center"
    assert d["Attack"] == "0 ms"
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
    text = P.script(P.sample())
    out = gui(P, tmp_path, text, ["focus Volume", "type -6", "key enter", "param 3", "flush", "counts",
                                  "type loud", "key enter", "param 3", "counts",
                                  "type -3", "key tab", "param 3"])
    f = got(out, "focus")
    assert f[0][1:] == ["Volume", "-6.0 dB"]
    assert "param 3: -6" in out
    assert "out: gesture_begin 3" in out and "out: value 3 -6" in out and "out: gesture_end 3" in out
    c = got(out, "counts")
    assert c[0][0] == "beeps 0" and c[0][1] != "request_flush 0"
    assert f[1][1:] == ["Volume", "-6.0 dB"] and c[1][0] == "beeps 1"     # "loud" beeps and goes back
    assert out.count("param 3: -6") == 2
    assert f[2][1] == "Pan" and "param 3: -3" in out                     # leaving the box applies it too


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
    out = gui(P, tmp_path, text, ["focus Settings", "key enter", "key tab", "key tab", "type 5", "key enter",
                                  "key up", "key down", "key tab", "key tab", "key tab", "key tab",
                                  "focus Back", "key enter", "focus Back", "say"])
    f = got(out, "focus")
    assert f[0][:3] == ["Edit", "Cache folder", str(tmp_path / "cache")]
    assert f[1][:2] == ["Button", "Browse folder"]
    assert f[2] == ["Edit", "Cache size limit", "10 GB"]
    assert f[3][2] == "5 GB" and f[4][2] == "6 GB" and f[5][2] == "5 GB"
    assert f[6][:2] == ["Edit", "Cache in use"] and f[6][2].endswith(" of 5 GB")
    assert [x[:2] for x in f[7:10]] == [["Button", "Clear cache"], ["Button", "Back"], ["Button", "after"]]
    assert f[10][:2] == ["Button", "Settings"]           # Back: on the Settings button again
    assert f[11][:2] == ["Button", "Settings"]           # Back is gone with its page: nothing to focus by that name
    with open(settings_file) as fh:
        assert "cache_limit_gb 5\n" in fh.read()


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
