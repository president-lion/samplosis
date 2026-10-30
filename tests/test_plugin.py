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


def host(*args, timeout=120, env=None):
    r = subprocess.run([HOST, CLAP, *[str(a) for a in args]], capture_output=True, text=True, timeout=timeout,
                       env=dict(os.environ, **env) if env else None)
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

        def play(self, text, events, frames, rate=None, all_outputs=False, env=None):
            st = self.state_file(text)
            ev = tmp_path / "events.txt"
            ev.write_text(events + "\nlength %d\n" % frames)
            out = tmp_path / "plug.wav"
            args = ["play", st, ev, out] + ([rate or 48000, "all"] if all_outputs else [rate] if rate else [])
            code, so, se = host(*args, env=env)
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


def test_plugin_switch_fades(P, tmp_path):
    # switching players during playback: the old player's voices fade out (5 ms; its effect tails 10 ms) on
    # output 1 instead of stopping dead
    base = P.script(P.sample()) + 'soundfont "%s"\nsf_used 0:0\n' % P.font()
    sf_state = "plays soundfont\n" + base
    sampler_state = "plays sampler\n" + base
    P.state_file(sampler_state, "to_sampler.txt")
    P.state_file(sf_state, "to_sf.txt")
    at = 47 * 256
    for start, switch, note in ((sf_state, "to_sampler.txt", "midi 144 57 100 0"), (sampler_state, "to_sf.txt", "on 57 100 0")):
        y, d, r = P.play(start, "%s\nstate %s %d" % (note, tmp_path / switch, at), 24000)
        x = y[:, 0]
        assert np.max(np.abs(x[at - 256:at])) > 0.05, switch
        assert np.max(np.abs(x[at:at + 48])) > 0.02, switch               # still sounding, fading
        step = np.max(np.abs(np.diff(x[at - 8:at + 8])))
        assert step < 0.05, (switch, step)                                  # no click at the switch
        assert np.max(np.abs(y[at + 768:])) == 0, switch                  # gone after the fade


def test_plugin_note_off_any_channel(P):
    # a CLAP note-off with channel -1 (any) and a real key ends that key on every channel
    f = P.font()
    text = 'plays soundfont\nsoundfont "%s"\nsf_used 0:0 0:33 128:0\n' % f
    frames = 48000
    notes = "midi 145 57 100 0\nmidi 147 57 100 0\nmidi 144 60 100 0"     # channels 2 and 4 key 57, channel 1 key 60
    ref = P.sf_reference(text, "midi 193 33 0\n" + notes + "\nmidi 129 57 0 12000\nmidi 131 57 0 12000", frames)
    y, d, r = P.play(text, "midi 193 33 0 0\n" + notes + "\noff 57 12000 -1", frames)
    assert db(ref) > -40
    assert db(y - ref) - db(ref) < -90
    held = P.sf_reference(text, "midi 193 33 0\n" + notes, frames)   # without the note-off the two notes ring on
    assert db(held - ref) - db(ref) > -40


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


def test_plugin_offline_render_font_gone(P, tmp_path):
    # the font file goes away after the state loaded: an offline render with a program change still finishes
    # quickly, silent where the samples are missing, and the samples already loaded still play
    import time
    f = P.font()
    away = tmp_path / "away.sf2"
    text = 'plays soundfont\nsoundfont "%s"\n' % f            # the channels' power-on presets: piano and kit
    frames = 3 * 48000
    events = ("render offline 0\nrename %s %s 0\nmidi 193 33 0 0\nmidi 145 57 100 0\nmidi 144 57 100 48000\n"
              "midi 194 33 0 96000\nmidi 146 57 100 96000" % (f, away))
    try:
        t0 = time.time()
        y, d, r = P.play(text, events, frames)
        took = time.time() - t0
    finally:
        if away.exists():
            away.rename(f)
    assert took < 5, took
    assert d["status after"] == "Loading samples for 1 preset", d     # the bass: still trying
    assert np.max(np.abs(y[:48000])) == 0                     # the bass on channel 2: its sample can't load
    assert db(y[48000:96000]) > -40                           # the piano on channel 1 was loaded before
    assert abs(peak_hz(y[52800:72000, 0]) - 220) < 3


def test_plugin_font_missing(P, tmp_path):
    f = P.font("gone.sf2")
    text = ('plays soundfont\nsoundfont "%s"\nsf_channel_preset 3 0 25\nsf_output 2 3\nsf_used 0:0 0:33\n'
            'sf_volume -3\n' % f)
    code, out, err = host("state", P.state_file(text))
    assert code == 0, out + err
    st = lines(out)["status"]
    assert st == "Ready: gone.sf2, 6 presets" or st.startswith("Loading samples for ")
    saved = saved_state(out)
    assert saved.startswith('plays soundfont\n# Samplosis 0.1.0\n')
    assert ('soundfont "%s"\n' % f) in saved and "\nsf_used 0:0 0:33" in saved and "sf_volume -3.0 dB\n" in saved
    f.unlink()
    code, out, err = host("state", P.state_file(saved, "s2.txt"))
    assert code == 0, out + err
    d = lines(out)
    assert d["load"] == "ok"
    assert d["status"] == "Missing: %s" % f
    assert saved_state(out) == saved
    y, d, r = P.play(saved, "midi 144 57 100 0", 4800)
    assert np.max(np.abs(y)) == 0


def test_plugin_reset_keeps_used_presets(P):
    # REAPER resets on stop, seek and loop. A reset just before the song must not let go of the state's sf_used
    # presets: a program change and a note right after it still sound on time, in real time
    f = P.font()
    text = 'plays soundfont\nsoundfont "%s"\nsf_used 0:0 0:33 128:0\n' % f
    for at in (0, 256):
        ref = P.sf_reference(text, "midi 193 33 %d\nmidi 145 57 100 %d" % (at, at), 24000)
        # samples nothing holds are freed after 20 ms here, and 300 ms pass after the reset
        y, d, r = P.play(text, "reset 0\nsleep 300 0\nmidi 193 33 0 %d\nmidi 145 57 100 %d" % (at, at), 24000,
                         env={"SAMPLOSIS_TEST_GRACE_MS": "20"})
        assert db(ref) > -40
        assert db(y - ref) - db(ref) < -90, at


def test_plugin_reactivate_is_silent_soundfont(P):
    # a held note with reverb and chorus on: deactivate and activate again, and nothing sounds after, tails included
    f = P.font()
    text = 'plays soundfont\nsoundfont "%s"\nsf_used 0:0 0:16 128:0\n' % f
    at = 19 * 256
    y, d, r = P.play(text, "midi 192 16 0 0\nmidi 144 69 100 0\ncycle %d" % at, 24000)   # the looping organ
    assert np.max(np.abs(y[at - 2000:at])) > 0.01
    assert np.max(np.abs(y[at:])) == 0
    assert d["voices"] == "0"


def test_plugin_loading_status(P):
    # with the loader paused, the status counts the presets still loading
    f = P.font()
    # one mode on channel 1: only the bass is in use; all mode: the channels' piano and kit, and the bass
    for extra, want in (("sf_mode one\nsf_preset 0 33\nsf_channel 1\nsf_used 0:33", "Loading samples for 1 preset"),
                        ("sf_used 0:33", "Loading samples for 3 presets")):
        st = P.state_file('plays soundfont\nsoundfont "%s"\n%s\n' % (f, extra), "load.txt")
        code, out, err = host("state", st, env={"SAMPLOSIS_TEST_PAUSE_LOADER": "1"})
        assert code == 0, out + err
        assert lines(out)["status"] == want


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
    ("Tab", "Player"), ("Edit", "Sample"), ("Button", "Browse"), ("ComboBox", "File"),
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


def gui(P, tmp_path, text, actions, env=None):
    """Opens the window on a state and runs actions; returns the printed lines."""
    st = P.state_file(text, "gui_state.txt")
    sc = tmp_path / "gui_script.txt"
    sc.write_text("\n".join(actions) + "\n", encoding="utf-8")
    code, out, err = host("gui", st, sc, timeout=120, env=env)
    assert code == 0, out + err
    return out


def got(out, tag):
    """Each `tag: class | name | value` line, split."""
    return [ln[len(tag) + 2:].split(" | ") for ln in out.splitlines() if ln.startswith(tag + ": ")]


def test_gui_tab_order(P, tmp_path):
    text = P.script(P.sample())
    out = gui(P, tmp_path, text, ["say"] + ["key tab"] * len(MAIN_ORDER) +
              ["focus Player", "key tab shift", "focus Volume", "window", "say"])
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


def test_gui_file_combo(P, tmp_path):
    # the File combo box: the folder's audio files by name, the loaded one selected; choosing one loads it at once
    d = tmp_path / "files"
    d.mkdir()
    for n in ("a.wav", "B.wav", "c.wav"):
        P.sample(n)
        os.replace(tmp_path / n, d / n)
    (d / "notes.txt").write_text("not audio")
    (d / "bad.wav").write_bytes(b"not a wav at all")
    text = P.script(d / "B.wav")
    out = gui(P, tmp_path, text, ["read File", "list File", "focus File", "key down", "read Sample", "read Status",
                                  "counts", "key end", "read Sample", "key down", "key home", "read Sample",
                                  "key up", "state", "counts",
                                  # with its list open: the failed choice, then the list closes
                                  "key down", "dropdown open", "key down", "dropdown close", "say", "read Sample",
                                  "counts"])
    r = [x[1] for x in got(out, "read")]
    f = got(out, "focus")
    assert r[0] == "B.wav"
    assert got(out, "list")[0] == ["File", "a.wav", "B.wav", "bad.wav", "c.wav"]    # notes.txt isn't listed
    # bad.wav won't load: a beep, the reason in Status, and the combo box back on the loaded file
    assert f[0] == ["ComboBox", "File", "B.wav"] and r[1].startswith("B.wav, 0.80 s, root ")
    assert r[2].startswith("Can't load bad.wav: ") and got(out, "counts")[0][0] == "beeps 1"
    assert f[1] == ["ComboBox", "File", "c.wav"] and r[3].startswith("c.wav, ")
    assert f[2] == ["ComboBox", "File", "c.wav"]                     # Down at the last stays
    assert f[3] == ["ComboBox", "File", "a.wav"] and r[4].startswith("a.wav, ")
    assert f[4] == ["ComboBox", "File", "a.wav"]                     # Up at the first stays
    assert ('sample "%s"' % (d / "a.wav")) in out
    c = got(out, "counts")[1]
    assert c[0] == "beeps 1" and c[2] != "mark_dirty 0"
    # the list open: bad.wav chosen there fails; when the list closes the combo box is back on the loaded B.wav
    assert f[5] == ["ComboBox", "File", "B.wav"] and f[6] == ["ComboBox", "File", "bad.wav"]
    assert f[7] == ["ComboBox", "File", "B.wav"], out
    assert r[5].startswith("B.wav, ") and got(out, "counts")[2][0] == "beeps 2"


def test_gui_settings_page(P, tmp_path, settings_file):
    raw = tmp_path / "take.raw"                     # a raw sample: changing a raw setting says it applies next time
    raw.write_bytes((np.sin(np.arange(44100) / 20) * 8000).astype("<i2").repeat(2).tobytes())
    text = P.script(raw)
    with open(settings_file) as fh:
        before = fh.read()
    try:
        fake = tmp_path / "tools" / "ffmpeg.exe"
        fake.parent.mkdir()
        fake.write_bytes(b"MZ")
        out = gui(P, tmp_path, text, ["focus Settings", "key enter", "key down", "key down", "key tab", "key down",
                                      "key tab", "key tab", "key tab", "key enter", "type 5", "key enter",
                                      "key up", "key down",
                                      # the raw settings and FFmpeg, after Cache size limit
                                      "key tab", "key down", "key home", "key end", "key down", "key tab", "key up",
                                      "key tab", "key tab", "key down", "key tab", "key tab",
                                      "key tab", "key tab", "key tab",
                                      "key tab shift", "key tab shift", "key tab shift", "focus Back", "key tab",
                                      "focus Browse for FFmpeg", "dialog pick %s" % fake, "key enter", "read FFmpeg",
                                      "focus Back", "key enter", "read Status", "focus Back", "say",
                                      # the Settings page twice more: FFmpeg isn't searched for again
                                      "ffsearches", "focus Settings", "key enter", "focus Back", "key enter",
                                      "focus Settings", "key enter", "ffsearches"])
        f = got(out, "focus")
        assert f[0] == ["ComboBox", "Stretcher", "Signalsmith Stretch, fast"]
        assert f[1] == f[2] == ["ComboBox", "Stretcher", "Rubber Band, slower"]   # Down stops at the last
        assert f[3] == ["ComboBox", "Keep the tone", "Off"] and f[4] == ["ComboBox", "Keep the tone", "On"]
        assert f[5][:3] == ["Edit", "Cache folder", str(tmp_path / "cache")]
        assert f[6][:2] == ["Button", "Browse folder"]
        assert f[7] == ["Slider", "Cache size limit", "10 GB"]
        assert f[8] == ["Edit", "Cache size limit", "10 GB"]
        assert f[9][2] == "5 GB" and f[10][2] == "6 GB" and f[11][2] == "5 GB"
        assert f[12] == ["ComboBox", "Raw sample rate", "44100 Hz"] and f[13][2] == "48000 Hz"
        assert f[14][2] == "8000 Hz" and f[15][2] == f[16][2] == "192000 Hz"   # Home, End; Down stops at the last
        assert f[17] == ["ComboBox", "Raw channels", "Stereo"] and f[18][2] == "Mono"
        assert f[19] == ["ComboBox", "Raw sample format", "16-bit integer"]
        assert f[20] == ["ComboBox", "Raw byte order", "Little endian"] and f[21][2] == "Big endian"
        assert f[22][:2] == ["Edit", "FFmpeg"]                               # the path found, or Not found
        assert f[22][2] == "Not found" or f[22][2].lower().endswith("ffmpeg.exe"), f[22]
        assert f[23][:2] == ["Button", "Browse for FFmpeg"]
        assert f[24][:2] == ["Edit", "Cache in use"] and f[24][2].endswith(" of 5 GB")
        assert [x[:2] for x in f[25:27]] == [["Button", "Clear cache"], ["Button", "Back"]]
        assert [x[:2] for x in f[27:30]] == [["Button", "Clear cache"], ["Edit", "Cache in use"],
                                             ["Button", "Browse for FFmpeg"]]          # Shift+Tab back up
        assert f[30][:2] == ["Button", "after"]                              # past the end: out of the plugin
        assert f[31][:2] == ["Button", "Browse for FFmpeg"]                  # the dialog returns to its button
        assert got(out, "read")[0] == ["FFmpeg", str(fake)]
        assert got(out, "read")[1] == ["Status", "Raw settings apply the next time a raw file loads"]
        assert f[32][:2] == ["Button", "Settings"]           # Back: on the Settings button again
        assert f[33][:2] == ["Button", "Settings"]           # Back is gone with its page: nothing to focus by that name
        n = [int(x[0]) for x in got(out, "ffsearches")]
        assert len(n) == 2 and n[0] >= 1 and n[1] == n[0], n               # searched on the first visit and after Browse only
        with open(settings_file) as fh:
            saved = fh.read()
        assert "raw_rate 192000\n" in saved and "raw_channels 1\n" in saved and "raw_endian big\n" in saved
        assert ("ffmpeg_path %s\n" % fake) in saved
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


# ---- the window's SoundFont tab ----
SF_ORDER = ([("Tab", "Player"), ("Edit", "SoundFont"), ("Button", "Browse"), ("ComboBox", "File"),
             ("ComboBox", "Mode")] +
            [x for n in range(1, 17) for x in (("ComboBox", "Channel %d" % n), ("ComboBox", "Channel %d output" % n))] +
            [("Slider", "SoundFont volume"), ("Slider", "SoundFont voices"), ("Slider", "SoundFont transpose"),
             ("ComboBox", "Reverb and chorus"), ("Edit", "Status"), ("Button", "Settings")])


def test_gui_soundfont_tab(P, tmp_path):
    f = P.font()
    text = "plays sampler\n" + P.script(P.sample()) + 'soundfont "%s"\n' % f
    out = gui(P, tmp_path, text, ["say", "key right", "state plays"] + ["key tab"] * (len(SF_ORDER) - 1) +
              ["key tab", "dump",
               "focus Mode", "key down", "state sf_mode", "dump",
               "focus Preset", "key down", "state sf_preset", "key up", "key up",
               "focus Channel", "key down", "state sf_channel", "key tab",
               "key up", "param 31",
               "key tab ctrl", "state plays", "focus Volume", "key tab ctrl shift", "key left", "key left",
               "key right", "focus Mode", "key up", "say",
               "focus Settings", "key enter", "key tab ctrl", "focus Back", "key enter", "key tab shift",
               "key tab shift"])
    f = got(out, "focus")
    st = [x[0] for x in got(out, "state")]
    assert f[0] == ["Tab", "Player", "Sampler"], out                 # opens on the tab that plays
    assert f[1] == ["Tab", "Player", "SoundFont"], out               # Right: the SoundFont tab, and it plays
    assert st[0] == "plays soundfont"
    n = len(SF_ORDER)
    assert [x[:2] for x in f[1:1 + n]] == [list(x) for x in SF_ORDER], out
    assert f[1 + n][:2] == ["Button", "after"]
    d = {x[1]: x[2] for x in got(out, "control")[:n]}
    assert d["SoundFont"] == "gm.sf2, 6 presets"
    assert d["Mode"] == "All channels" and d["Channel 1 output"] == "Output 1"
    assert d["Reverb and chorus"] == "On" and d["SoundFont volume"] == "0.0 dB"
    assert "Preset" not in d and "Channel" not in d
    k = n + 2
    assert f[k] == ["ComboBox", "Mode", "One preset"] and st[1] == "sf_mode one"
    d2 = [x[:2] for x in got(out, "control")[n:]]
    assert ["ComboBox", "Preset"] in d2 and ["ComboBox", "Channel"] in d2
    assert not [x for x in d2 if x[1].startswith("Channel ")], d2   # the 32 channel combos are hidden
    assert f[k + 1] == ["ComboBox", "Preset", "Stereo, bank 0 program 2"] and st[2] == "sf_preset 0 1"
    assert f[k + 2] == ["ComboBox", "Preset", "Piano, bank 0 program 1"]
    assert f[k + 3] == ["ComboBox", "Preset", "Piano, bank 0 program 1"]    # Up stops at the first
    assert f[k + 4] == ["ComboBox", "Channel", "Channel 1"] and st[3] == "sf_channel 1"
    assert f[k + 5][:2] == ["Slider", "SoundFont volume"]                   # Tab skips the hidden channel combos
    assert f[k + 6] == ["Slider", "SoundFont volume", "1.0 dB"] and "param 31: 1" in out
    assert f[k + 7] == ["Tab", "Player", "Sampler"] and st[4] == "plays sampler"     # Ctrl+Tab from any control
    assert f[k + 8] == ["Tab", "Player", "SoundFont"]                        # Ctrl+Shift+Tab from the sampler page
    assert f[k + 9] == f[k + 10] == ["Tab", "Player", "Sampler"]             # Left stops at the first tab
    assert f[k + 11] == ["Tab", "Player", "SoundFont"]
    assert f[k + 12] == ["ComboBox", "Mode", "All channels"] and f[k + 13] == f[k + 12]
    # Settings from the SoundFont page (Ctrl+Tab does nothing there), and Back to the SoundFont page's Settings
    assert f[k + 14][:2] == ["ComboBox", "Stretcher"] and f[k + 15] == f[k + 14]
    assert f[k + 16][:2] == ["Button", "Settings"] and f[k + 17][:2] == ["Edit", "Status"]
    assert f[k + 18] == ["ComboBox", "Reverb and chorus", "On"]


def test_gui_soundfont_browse_and_drop(P, tmp_path):
    d = tmp_path / "fonts"
    d.mkdir()
    for n in ("a.sf2", "B.sf2", "c.sf3"):
        write_sf_font(d / n)
    (d / "notes.txt").write_text("not a font")
    wav = P.sample("drop A3.wav")
    # a folder of every instrument kind: a real .sfz (one sample missing, one setting it can't play) and .dls; the
    # other kinds only need their names for the File combo box
    from test_dls import make_dls
    k = tmp_path / "kinds"
    k.mkdir()
    P.sample("s.wav")
    os.replace(tmp_path / "s.wav", k / "s.wav")
    (k / "kit.sfz").write_text("<region> sample=s.wav key=60 eq1_freq=100\n<region> sample=gone.wav key=62\n")
    make_dls(k / "bank.dls")
    for n in ("g.gig", "h.dspreset", "i.sf4", "j.sf2pack", "k.exs", "l.sbk", "m.sf2", "n.sf3", "notes.txt", "o.wav"):
        (k / n).write_bytes(b"x")
    text = P.script(P.sample())
    out = gui(P, tmp_path, text, ["key right", "read SoundFont", "read File", "dialog pick %s" % (d / "a.sf2"),
                                  "focus Browse", "key enter", "read SoundFont", "list File", "focus File", "key down",
                                  "read SoundFont", "key down", "read SoundFont", "key down", "key up",
                                  "read SoundFont", "state soundfont",
                                  "drop %s" % wav, "say", "read Sample", "state plays",
                                  "drop %s" % (d / "B.sf2"), "say", "read SoundFont", "state plays", "counts",
                                  # each page's Browse loads its own kind, whatever the file
                                  "dialog pick %s" % wav, "focus Browse", "key enter", "read SoundFont",
                                  "read Status", "state sample", "counts",
                                  "key tab ctrl", "dialog pick %s" % (d / "a.sf2"), "focus Browse", "key enter",
                                  "read Sample", "read Status", "state plays", "counts",
                                  # a dropped .sfz and .dls go to the SoundFont tab; File lists every instrument kind
                                  "drop %s" % (k / "kit.sfz"), "say", "read SoundFont", "list File",
                                  "read Status", "drop %s" % wav, "say", "drop %s" % (k / "bank.dls"), "say",
                                  "read SoundFont", "counts"])
    r = [x[1] for x in got(out, "read")]
    f = got(out, "focus")
    st = [x[0] for x in got(out, "state")]
    c = [x[0] for x in got(out, "counts")]
    assert r[0] == "No SoundFont loaded" and r[1] == "No file loaded"
    assert f[1][:2] == ["Button", "Browse"] and r[2] == "a.sf2, 6 presets"
    assert got(out, "list")[0] == ["File", "a.sf2", "B.sf2", "c.sf3"]          # notes.txt isn't listed
    assert f[2] == ["ComboBox", "File", "B.sf2"] and r[3] == "B.sf2, 6 presets"
    assert f[3] == ["ComboBox", "File", "c.sf3"] and r[4] == "c.sf3, 6 presets"
    assert f[4] == ["ComboBox", "File", "c.sf3"]                     # Down at the last stays
    assert f[5] == ["ComboBox", "File", "B.sf2"] and r[5] == "B.sf2, 6 presets"
    assert st[0] == 'soundfont "%s"' % (d / "B.sf2")
    assert f[6] == ["Tab", "Player", "Sampler"]                      # a dropped sample: the Sampler tab
    assert r[6].startswith("drop A3.wav, ") and st[1] == "plays sampler"
    assert f[7] == ["Tab", "Player", "SoundFont"]                    # a dropped font: the SoundFont tab
    assert r[7] == "B.sf2, 6 presets" and st[2] == "plays soundfont"
    assert c[0] == "beeps 0"
    # a wav through the SoundFont page's Browse: the SoundFont player's error; the sample stays
    assert r[8] == "B.sf2, 6 presets" and r[9].startswith("Can't load drop A3.wav: ") and c[1] == "beeps 1"
    assert st[3].startswith('sample "%s"' % wav)
    # a .sf2 through the Sampler page's Browse: the sampler's error; the sampler still plays its sample
    assert r[10].startswith("drop A3.wav, ") and r[11].startswith("Can't load a.sf2: ")
    assert r[11].endswith(". Still playing drop A3.wav."), r[11]       # the failure says what still plays
    assert st[4] == "plays sampler" and c[2] == "beeps 2"
    assert f[-3] == ["Tab", "Player", "SoundFont"], out              # a dropped .sfz: the SoundFont tab
    assert r[12] == "kit.sfz, SFZ, 1 preset", out
    assert got(out, "list")[1] == ["File", "bank.dls", "g.gig", "h.dspreset", "i.sf4", "j.sf2pack", "k.exs",
                                   "kit.sfz", "l.sbk", "m.sf2", "n.sf3"], out    # every kind; notes.txt and o.wav aren't
    assert r[13].startswith("Ready: kit.sfz, SFZ, 1 preset. ") or r[13].startswith("Loading samples for 1 preset. ")
    assert r[13].endswith(". Missing 1 sample (first: gone.wav). "
                          "Ignored 1 SFZ setting it doesn't support (first: eq1_freq)"), out
    assert f[-2] == ["Tab", "Player", "Sampler"]
    assert f[-1] == ["Tab", "Player", "SoundFont"], out              # a dropped .dls: the SoundFont tab
    assert r[14] == "bank.dls, DLS, 5 presets", out
    assert c[3] == "beeps 2", out


def test_gui_sfz_loads_in_background(P, tmp_path):
    # A dropped .sfz loads on the window's worker (it probes every sample file's header): the drop returns at once
    # with "Loading big.sfz", and the instrument is swapped in on the main thread and plays
    import struct
    d = tmp_path / "big"
    d.mkdir()
    lines = []
    for i in range(2000):
        data = struct.pack("<4h", 0, 1000, 0, -1000)
        fmt = struct.pack("<HHIIHH", 1, 1, 44100, 88200, 2, 16)
        body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", len(data)) + data
        (d / ("s%04d.wav" % i)).write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)
        lines.append("<region> sample=big/s%04d.wav key=%d lovel=%d hivel=%d" % (i, 20 + i % 80, i // 80, i // 80))
    sfz = tmp_path / "big.sfz"
    sfz.write_text("\n".join(lines) + "\n")
    text = P.script(P.sample())
    out = gui(P, tmp_path, text, ["async on", "clock", "drop %s" % sfz, "clock", "read Status", "wait", "say",
                                  "read SoundFont", "read Status", "state plays", "state soundfont", "counts"])
    r = [x[1] for x in got(out, "read")]
    ms = [int(ln.split(": ")[1].split()[0]) for ln in out.splitlines() if ln.startswith("clock: ")]
    assert ms[1] < 500, out                                          # the drop returned at once
    assert r[0] == "Loading big.sfz", out
    assert r[1] == "big.sfz, SFZ, 1 preset", out
    assert r[2].startswith("Ready: big.sfz") or r[2].startswith("Loading samples for "), out
    assert "state: plays soundfont" in out and ('state: soundfont "%s"' % sfz) in out
    assert got(out, "counts")[0][0] == "beeps 0"


def test_gui_channel_from_song(P, tmp_path):
    font = P.font()
    other = P.font("other.sf2")
    text = 'plays soundfont\nsoundfont "%s"\nsf_channel_preset 3 0 99\n' % font
    one = P.state_file('plays soundfont\nsoundfont "%s"\nsf_mode one\nsf_preset 0 99\n' % font, "one.txt")
    out = gui(P, tmp_path, text, ["say", "read Channel 2", "read Channel 10", "read Channel 3", "midi 193 33 0",
                                  "pump 400",
                                  "read Channel 2", "focus Channel 2", "key down", "state sf_channel_preset 2",
                                  "key up", "state sf_channel_preset 2", "focus Channel 2 output", "key down",
                                  "state sf_output", "key end", "key down", "counts",
                                  # One preset chosen by a state while the focus is on a channel's combo box
                                  "focus Channel 2", "load %s" % one, "pump 400", "say", "read Preset",
                                  "focus SoundFont volume", "key tab shift",
                                  # the window closed while the SoundFont page's Browse dialog is open
                                  "dialog closegui %s" % other, "focus Browse", "key enter", "pump 600",
                                  "state soundfont", "alive"])
    r = [x[1] for x in got(out, "read")]
    f = got(out, "focus")
    st = [x[0] for x in got(out, "state")]
    assert f[0] == ["Tab", "Player", "SoundFont"]                    # opens on the tab that plays
    assert r[0] == "From the song (now Piano)" and r[1] == "From the song (now Kit)"
    assert r[2] == "Bank 0 program 100 (not in this font)"           # pinned to a preset the font lacks
    assert r[3] == "From the song (now Bass)"                        # program 33 on channel 2 played through
    assert f[1] == ["ComboBox", "Channel 2", "Piano, bank 0 program 1"] and st[0] == "sf_channel_preset 2 0 0"
    assert f[2] == ["ComboBox", "Channel 2", "From the song (now Bass)"] and st[1] == "(none)"
    assert f[3] == ["ComboBox", "Channel 2 output", "Output 2"] and st[2] == "sf_output 2 2"
    assert f[4] == f[5] == ["ComboBox", "Channel 2 output", "Output 16"]   # Down stops at the last
    assert got(out, "counts")[0][0] == "beeps 0"
    assert f[6] == ["ComboBox", "Mode", "One preset"], out           # the channel combo hid: the focus on Mode
    assert r[4] == "Bank 0 program 100 (not in this font)"
    assert f[7] == ["ComboBox", "Channel", "All channels"]           # Shift+Tab skips the hidden channel combos
    assert st[3] == 'soundfont "%s"' % font                          # not loaded: the window had gone
    assert "alive: plugin\n" in out and "closed" in out


# ---- the window loads in the background ----
FFMPEG = __import__("shutil").which("ffmpeg")


def fake_ff(tmp_path, mode):
    """tests\fakeff.exe as ffmpeg.exe and ffprobe.exe in a folder named mode (which says how it behaves)."""
    import shutil
    d = tmp_path / mode
    d.mkdir()
    for n in ("ffmpeg.exe", "ffprobe.exe"):
        shutil.copyfile(os.path.join(ROOT, "tests", "fakeff.exe"), d / n)
    return d


def set_ffmpeg(lib, path):
    from conftest import SmpSettings
    s = SmpSettings()
    lib.smp_settings_get(ctypes.byref(s))
    s.ffmpeg_path = str(path).encode()
    assert lib.smp_settings_set(ctypes.byref(s)) == 0


@pytest.mark.skipif(not FFMPEG, reason="ffmpeg isn't installed")
def test_gui_load_does_not_block(P, tmp_path):
    # Browse an FFmpeg-only file: the window's call returns at once with "Decoding ...", the old sample still plays,
    # and the new one is swapped in when FFmpeg is done (the test flag holds FFmpeg back 1.5 s)
    src = P.sample("src.wav")
    f = tmp_path / "tone A3.tta"
    subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-y", "-i", str(src), "-c:a", "tta", str(f)], check=True, timeout=60)
    junk = tmp_path / "junk.wav"
    junk.write_bytes(b" not a wav" * 200)
    text = P.script(P.sample())
    out = gui(P, tmp_path, text, ["async on", "dialog pick %s" % f, "focus Browse", "clock", "key enter", "clock",
                                  "pump 300", "read Status", "read Sample", "wait", "read Status", "read Sample",
                                  "state", "counts", "dialog pick %s" % junk, "focus Browse", "key enter", "wait",
                                  "status", "counts", "pump 6500", "status", "read Status"],
              env={"SAMPLOSIS_TEST_DECODE_FLAGS": "512"})
    r = [x[1] for x in got(out, "read")]
    ms = [int(ln.split(": ")[1].split()[0]) for ln in out.splitlines() if ln.startswith("clock: ")]
    assert ms[1] < 500, out                                         # the key returned at once
    assert r[0] == "Decoding tone A3.tta with FFmpeg", out
    assert r[1].startswith("tone A3.wav, "), out                    # the old sample until the new one is ready
    assert r[2] == "Ready: tone A3.tta (decoded by FFmpeg)", out
    assert r[3].startswith("tone A3.tta, "), out
    assert ('state: sample "%s"' % f) in out
    assert got(out, "counts")[0][0] == "beeps 0"
    # a load that fails: told in Status with a beep, and kept in the plugin's status for a window opened later
    assert ("status: Can't load junk.wav: it isn't a sound file Samplosis can read. Still playing tone A3.tta."
            in out), out
    assert got(out, "counts")[1][0] == "beeps 1"
    # and only for a while: then the status says how things are again (it doesn't hide them for good)
    assert "status: Ready: tone A3.tta (decoded by FFmpeg)" in out and r[4] == "Ready: tone A3.tta (decoded by FFmpeg)", out


@pytest.mark.skipif(not FFMPEG, reason="ffmpeg isn't installed")
def test_gui_load_of_other_kind_is_told(P, tmp_path):
    # a sample still decoding when a .sfz is dropped: the one worker stops the sample's load, and Status says so
    src = P.sample("src.wav")
    f = tmp_path / "tone A3.tta"
    subprocess.run([FFMPEG, "-nostdin", "-v", "error", "-y", "-i", str(src), "-c:a", "tta", str(f)], check=True, timeout=60)
    (tmp_path / "s.wav").write_bytes(open(src, "rb").read())
    sfz = tmp_path / "kit.sfz"
    sfz.write_text("<region> sample=s.wav key=60\n")
    text = P.script(P.sample())
    out = gui(P, tmp_path, text, ["async on", "drop %s" % f, "pump 200", "drop %s" % sfz, "pump 300", "read Status",
                                  "wait", "read SoundFont", "counts"],
              env={"SAMPLOSIS_TEST_DECODE_FLAGS": "512"})
    r = [x[1] for x in got(out, "read")]
    assert r[0] == "Stopped loading tone A3.tta to load kit.sfz", out
    assert r[1] == "kit.sfz, SFZ, 1 preset", out
    assert got(out, "counts")[0][0] == "beeps 0"


def test_background_load_cancel(lib, P, tmp_path):
    # a second load while FFmpeg still works on the first: the first FFmpeg is killed and the second sample loads;
    # then the plugin is destroyed while a third decode runs, and nothing is left running
    d = fake_ff(tmp_path, "hang")
    junk = tmp_path / "first.tta"
    junk.write_bytes(b"nothing we read" * 100)
    third = tmp_path / "third.tta"
    third.write_bytes(b"nothing we read" * 100)
    second = P.sample("second A3.wav")
    text = P.script(P.sample())
    pids = d / "pids.txt"
    set_ffmpeg(lib, d / "ffmpeg.exe")
    try:
        out = gui(P, tmp_path, text, ["async on", "dialog pick %s" % junk, "focus Browse", "key enter", "pump 1500",
                                      "read Status", "pids %s" % pids, "dialog pick %s" % second, "focus Browse",
                                      "key enter", "wait", "pids %s" % pids, "read Status", "read Sample", "pump 600",
                                      "read Sample", "state", "dialog pick %s" % third, "focus Browse", "key enter",
                                      "pump 1000", "pids %s" % pids, "alive"])
    finally:
        set_ffmpeg(lib, "")
    r = [x[1] for x in got(out, "read")]
    p = [ln for ln in out.splitlines() if ln.startswith("pids: ")]
    assert r[0] == "Decoding first.tta with FFmpeg", out
    assert p[0] == "pids: 1 of 2 running", out                     # ffprobe ended, ffmpeg hangs
    assert p[1] == "pids: 0 of 2 running", out                     # the newer load killed it
    assert r[1] == "Ready: second A3.wav" and r[2].startswith("second A3.wav, ") and r[3] == r[2], out
    assert ('state: sample "%s"' % second) in out
    assert p[2] == "pids: 1 of 4 running", out                     # the third decode, when the plugin goes
    assert "alive: plugin\n" in out and "closed" in out
    import time
    t = time.perf_counter()
    while time.perf_counter() - t < 5:
        n = sum(1 for x in pids.read_text().split() if _running(int(x)))
        if not n:
            break
        time.sleep(0.1)
    assert n == 0


def _running(pid):
    k32 = ctypes.WinDLL("kernel32")
    k32.OpenProcess.restype = ctypes.c_void_p
    k32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    k32.CloseHandle.argtypes = [ctypes.c_void_p]
    h = k32.OpenProcess(0x00100000, False, pid)
    if not h:
        return False
    try:
        return k32.WaitForSingleObject(h, 0) == 0x102
    finally:
        k32.CloseHandle(h)
