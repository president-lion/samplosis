"""The Sampler with several samples (multi.c): each sample's key and velocity range, root, and the script."""
import ctypes

import numpy as np
import pytest
import soundfile as sf

from conftest import write_wav
from test_cache import point_cache

vp, c_int, c_dbl, c_char_p = ctypes.c_void_p, ctypes.c_int, ctypes.c_double, ctypes.c_char_p
RATE = 48000


@pytest.fixture
def M(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    SP = lib.smp_sample_load.restype              # conftest's sample pointer (shared: left as it is)
    for name, res, args in [
        ("smp_multi_new", vp, [c_dbl, c_int]), ("smp_multi_free", None, [vp]), ("smp_multi_count", c_int, [vp]),
        ("smp_multi_add", c_int, [vp, SP]), ("smp_multi_replace", c_int, [vp, c_int, SP]),
        ("smp_multi_remove", None, [vp, c_int]), ("smp_multi_clear", None, [vp]),
        ("smp_multi_sample", vp, [vp, c_int]), ("smp_multi_path", c_int, [vp, c_int, c_char_p, c_int]),
        ("smp_multi_range", None, [vp, c_int] + [ctypes.POINTER(c_int)] * 4),
        ("smp_multi_set_range", c_int, [vp, c_int, c_int, c_int, c_int, c_int]),
        ("smp_multi_root", c_int, [vp, c_int]), ("smp_multi_set_root", None, [vp, c_int, c_int]),
        ("smp_multi_selected", c_int, [vp]), ("smp_multi_select", None, [vp, c_int]),
        ("smp_multi_line", None, [vp, c_int, c_char_p, c_int]),
        ("smp_multi_set_param", None, [vp, c_int, c_dbl]), ("smp_multi_get_param", c_dbl, [vp, c_int]),
        ("smp_multi_collect", None, [vp, c_int]), ("smp_multi_wait_renders", c_int, [vp, c_int]),
        ("smp_multi_render_test", c_int, [vp, c_char_p, ctypes.c_long, c_int, c_char_p]),
        ("smp_multi_script_write", c_int, [vp, c_char_p, c_int]),
        ("smp_multi_script_apply", c_int, [vp, c_char_p, ctypes.POINTER(c_int), c_char_p, c_int]),
        ("smp_multi_active_voices", c_int, [vp]),
        ("smp_multi_mode", c_int, [vp, c_int]), ("smp_multi_set_mode", None, [vp, c_int, c_int]),
        ("smp_multi_pitch", None, [vp, c_int, ctypes.POINTER(c_dbl), ctypes.POINTER(c_dbl)]),
        ("smp_multi_set_pitch", None, [vp, c_int, c_dbl, c_dbl]), ("smp_multi_pick", None, [vp, c_int, c_int]),
    ]:
        f = getattr(lib, name)
        f.restype, f.argtypes = res, args

    class H:
        def __init__(self):
            self.made = []

        def new(self):
            m = lib.smp_multi_new(float(RATE), 256)
            assert m
            self.made.append(m)
            return m

        def wav(self, name, freq, secs=0.5, rate=44100):
            f = tmp_path / name
            t = np.arange(int(secs * rate)) / rate
            write_wav(f, (0.5 * np.sin(2 * np.pi * freq * t)).astype(np.float32), rate)
            return f

        def load(self, path):
            err = ctypes.create_string_buffer(512)
            s = lib.smp_sample_load(str(path).encode(), err, 512)
            assert s, err.value
            return s

        def apply(self, m, text):
            err = ctypes.create_string_buffer(700)
            miss = c_int(-1)
            r = lib.smp_multi_script_apply(m, text.encode(), ctypes.byref(miss), err, 700)
            return r, miss.value, err.value.decode()

        def text(self, m):
            n = lib.smp_multi_script_write(m, None, 0)
            b = ctypes.create_string_buffer(n + 1)
            lib.smp_multi_script_write(m, b, n + 1)
            return b.value.decode()

        def render(self, m, events, frames=24000):
            out = tmp_path / "multi.wav"
            assert lib.smp_multi_wait_renders(m, 60000) == 0
            assert lib.smp_multi_render_test(m, events.encode(), frames, 256, str(out).encode()) == 0
            y, _ = sf.read(str(out), dtype="float32")
            return y

        def range(self, m, i):
            v = [c_int() for _ in range(4)]
            lib.smp_multi_range(m, i, *[ctypes.byref(x) for x in v])
            return [x.value for x in v]

        def line(self, m, i):
            b = ctypes.create_string_buffer(1500)
            lib.smp_multi_line(m, i, b, 1500)
            return b.value.decode()

    h = H()
    yield h
    for m in h.made:
        lib.smp_multi_free(m)


def peak_hz(y):
    x = y[:, 0] if y.ndim > 1 else y
    x = x[2000:14000]
    s = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    return np.fft.rfftfreq(len(x), 1 / RATE)[int(np.argmax(s))]


BASE = "keep_length off\nmode sample\nrelease 20 ms\n"     # every key at the recorded pitch: the sample shows


def two(M, lib, extra=""):
    lo, hi = M.wav("low.wav", 220), M.wav("high.wav", 880)
    m = M.new()
    r, miss, err = M.apply(m, 'sample "%s" keys=C-1..B3\nsample "%s" keys=C4..G9\n%s%s' % (lo, hi, BASE, extra))
    assert r == 0 and miss == 0, err
    return m, lo, hi


def test_keys_pick_the_sample(M, lib):
    m, _, _ = two(M, lib)
    assert lib.smp_multi_count(m) == 2
    assert M.range(m, 0) == [0, 59, 1, 127] and M.range(m, 1) == [60, 127, 1, 127]
    assert abs(peak_hz(M.render(m, "on 50 100 0\noff 50 20000"))) == pytest.approx(220, abs=5)
    assert abs(peak_hz(M.render(m, "on 70 100 0\noff 70 20000"))) == pytest.approx(880, abs=5)


def test_velocity_picks_the_sample(M, lib):
    lo, hi = M.wav("soft.wav", 220), M.wav("loud.wav", 880)
    m = M.new()
    r, miss, err = M.apply(m, 'sample "%s" velocity=1..64\nsample "%s" velocity=65..127\n%s' % (lo, hi, BASE))
    assert r == 0, err
    assert M.range(m, 0) == [0, 127, 1, 64] and M.range(m, 1) == [0, 127, 65, 127]
    assert peak_hz(M.render(m, "on 60 40 0\noff 60 20000")) == pytest.approx(220, abs=5)
    assert peak_hz(M.render(m, "on 60 100 0\noff 60 20000")) == pytest.approx(880, abs=5)


def test_overlapping_samples_both_play(M, lib):
    m = M.new()
    a, b = M.wav("a.wav", 220), M.wav("b.wav", 880)
    assert lib.smp_multi_add(m, M.load(a)) == 0 and lib.smp_multi_add(m, M.load(b)) == 1
    assert M.range(m, 1) == [0, 127, 1, 127]                  # by default every key and velocity
    lib.smp_multi_set_param(m, 25, 0)                          # keep length off
    lib.smp_multi_set_mode(m, 0, 0); lib.smp_multi_set_mode(m, 1, 0)   # each in sample mode
    y = M.render(m, "on 60 100 0\noff 60 20000")[:, 0][2000:14000]
    s = np.abs(np.fft.rfft(y * np.hanning(len(y))))
    f = np.fft.rfftfreq(len(y), 1 / RATE)
    at = lambda hz: s[np.argmin(np.abs(f - hz))]
    assert at(220) > 0.2 * s.max() and at(880) > 0.2 * s.max()


def test_old_instrument_range_still_limits(M, lib):
    m, _, _ = two(M, lib, "note_hi B3\n")                     # an older project's Highest note: B3 for every sample
    assert np.abs(M.render(m, "on 70 100 0\noff 70 20000")).max() < 1e-6
    assert np.abs(M.render(m, "on 50 100 0\noff 50 20000")).max() > 0.1


def test_set_range_checks(M, lib):
    m, _, _ = two(M, lib)
    assert lib.smp_multi_set_range(m, 0, 40, 30, 1, 127) == 0       # low above high
    assert lib.smp_multi_set_range(m, 0, 0, 128, 1, 127) == 0
    assert lib.smp_multi_set_range(m, 0, 0, 127, 0, 127) == 0       # velocity starts at 1
    assert lib.smp_multi_set_range(m, 5, 0, 127, 1, 127) == 0
    assert M.range(m, 0) == [0, 59, 1, 127]
    assert lib.smp_multi_set_range(m, 0, 30, 40, 15, 80) == 1
    assert M.range(m, 0) == [30, 40, 15, 80]
    assert M.line(m, 0) == "low.wav, root A3, keys F#1 to E2, velocity 15 to 80, sample mode"


def test_root_is_each_samples_and_follows_selection(M, lib):
    a, b = M.wav("pad C3.wav", 220), M.wav("pad G4.wav", 880)
    m = M.new()
    r, _, err = M.apply(m, 'sample "%s"\nsample "%s"\n%s' % (a, b, BASE))
    assert r == 0, err
    assert [lib.smp_multi_root(m, i) for i in (0, 1)] == [48, 67]
    assert lib.smp_multi_selected(m) == 0 and lib.smp_multi_get_param(m, 0) == 48
    lib.smp_multi_select(m, 1)
    assert lib.smp_multi_get_param(m, 0) == 67
    lib.smp_multi_set_param(m, 0, 70)                          # Root note: the selected sample's
    assert [lib.smp_multi_root(m, i) for i in (0, 1)] == [48, 70]


def test_script_round_trip(M, lib):
    m, lo, hi = two(M, lib, "volume -6.0 dB\n")
    lib.smp_multi_set_range(m, 1, 60, 72, 20, 100)
    lib.smp_multi_set_root(m, 1, 69)
    t = M.text(m)
    lines = t.splitlines()
    samples = [x for x in lines if x.startswith("sample ")]
    assert len(samples) == 2
    assert samples[0].startswith('sample "%s" key=' % lo) and samples[0].endswith(" root=A3 keys=C-1..B3 mode=sample")
    assert samples[1].endswith(" root=A4 keys=C4..C5 velocity=20..100 mode=sample")
    assert not [x for x in lines if x.split(" ")[0] in PER_SAMPLE]   # not instrument lines any more
    assert "volume -6.0 dB" in lines
    m2 = M.new()
    r, miss, err = M.apply(m2, t)
    assert r == 0 and miss == 0, err
    assert M.text(m2) == t
    assert M.range(m2, 1) == [60, 72, 20, 100] and lib.smp_multi_root(m2, 1) == 69


def engine_text(lib, text):
    """The single-sample engine's own text for a script (script.c)."""
    lib.smp_engine_new.restype = vp
    lib.smp_script_write.restype, lib.smp_script_write.argtypes = c_int, [vp, c_char_p, c_int]
    lib.smp_script_apply.restype, lib.smp_script_apply.argtypes = c_int, [vp, c_char_p, c_char_p, c_int]
    e = lib.smp_engine_new(float(RATE), 256)
    err = ctypes.create_string_buffer(512)
    assert lib.smp_script_apply(e, text.encode(), err, 512) == 0, err.value
    n = lib.smp_script_write(e, None, 0)
    b = ctypes.create_string_buffer(n + 1)
    lib.smp_script_write(e, b, n + 1)
    lib.smp_engine_free(e)
    return b.value.decode()


PER_SAMPLE = ("mode", "note_lo", "note_hi", "pitch_lo", "pitch_hi", "vel_lo", "vel_hi")


def test_one_sample_text_is_as_before(M, lib):
    w = M.wav("tone C3.wav", 220)
    old = engine_text(lib, 'sample "%s"\nkeep_length off\nrelease 50 ms\n' % w)
    m = M.new()
    r, miss, err = M.apply(m, old)
    assert r == 0 and miss == 0, err
    # no root=, keys= or velocity= with one sample; each sample's own settings (at their defaults) aren't lines
    assert M.text(m) == "".join(ln for ln in old.splitlines(True) if ln.split(" ")[0] not in PER_SAMPLE)
    r, _, err = M.apply(m, old.replace("root C3", "root D3"))   # and its root line is the sample's root
    assert r == 0 and lib.smp_multi_root(m, 0) == 50


def test_missing_sample_kept(M, lib, tmp_path):
    a = M.wav("a.wav", 220)
    gone = tmp_path / "gone.wav"
    m = M.new()
    text = 'sample "%s"\nsample "%s" key=0123456789abcdef keys=C4..C5\n%s' % (a, gone, BASE)
    r, miss, err = M.apply(m, text)
    assert r == 0 and miss == 1, err
    assert lib.smp_multi_count(m) == 2 and lib.smp_multi_sample(m, 1) is None
    b = ctypes.create_string_buffer(1100)
    assert lib.smp_multi_path(m, 1, b, 1100) == 2 and b.value.decode() == str(gone)
    assert M.line(m, 1) == "Missing: %s, root C4, keys C4 to C5, velocity 1 to 127, sample mode" % gone
    assert 'sample "%s" key=0123456789abcdef root=C4 keys=C4..C5 mode=sample' % gone in M.text(m)
    assert peak_hz(M.render(m, "on 50 100 0\noff 50 20000")) == pytest.approx(220, abs=5)


def test_bad_lines_change_nothing(M, lib):
    m, _, _ = two(M, lib)
    before = M.text(m)
    for bad, word in [('sample "x.wav" keys=C4', "keys"), ('sample "x.wav" velocity=0..10', "velocity"),
                      ('sample "x.wav" keys=D4..C4', "keys"), ('sample "x.wav" root=H2', "root"),
                      ('sample "x.wav" colour=red', "colour")]:
        r, _, err = M.apply(m, bad + "\n")
        assert r != 0 and err.startswith("line 1: ") and word in err, (bad, err)
    assert M.text(m) == before


def test_reapply_keeps_loaded_samples(M, lib):
    m, _, _ = two(M, lib)
    s0, s1 = lib.smp_multi_sample(m, 0), lib.smp_multi_sample(m, 1)
    r, _, err = M.apply(m, M.text(m).replace("release 20 ms", "release 30 ms"))
    assert r == 0, err
    assert lib.smp_multi_sample(m, 0) == s0 and lib.smp_multi_sample(m, 1) == s1


def test_add_replace_remove(M, lib):
    m = M.new()
    assert lib.smp_multi_selected(m) == -1 and M.text(m).count("sample ") == 0
    a, b, c = M.wav("a C3.wav", 220), M.wav("b D3.wav", 330), M.wav("c E3.wav", 440)
    lib.smp_multi_add(m, M.load(a)); lib.smp_multi_add(m, M.load(b))
    lib.smp_multi_set_range(m, 1, 10, 20, 30, 40)
    assert lib.smp_multi_replace(m, 1, M.load(c)) == 1
    assert M.range(m, 1) == [10, 20, 30, 40] and lib.smp_multi_root(m, 1) == 52   # ranges kept, root the new one's
    lib.smp_multi_select(m, 1)
    lib.smp_multi_remove(m, 1)
    assert lib.smp_multi_count(m) == 1 and lib.smp_multi_selected(m) == 0
    lib.smp_multi_remove(m, 0)
    assert lib.smp_multi_count(m) == 0 and lib.smp_multi_selected(m) == -1
    lib.smp_multi_collect(m, 1)


def test_every_octave_of_a_note(M, lib):
    lib.smp_multi_key_class.restype, lib.smp_multi_key_class.argtypes = c_int, [vp, c_int]
    lib.smp_multi_set_key_class.restype, lib.smp_multi_set_key_class.argtypes = c_int, [vp, c_int, c_int]
    a = M.wav("a.wav", 220)
    m = M.new()
    r, _, err = M.apply(m, 'sample "%s" keys=C\n%s' % (a, BASE))      # keys=C: every C, whatever its octave
    assert r == 0, err
    assert lib.smp_multi_key_class(m, 0) == 1
    assert M.line(m, 0).endswith("keys every C, velocity 1 to 127, sample mode")
    for k in (36, 48, 60, 120):
        assert np.abs(M.render(m, "on %d 100 0\noff %d 20000" % (k, k))).max() > 0.1, k
    for k in (49, 59, 61):
        assert np.abs(M.render(m, "on %d 100 0\noff %d 20000" % (k, k))).max() < 1e-6, k
    assert ' keys=C mode=sample\n' in M.text(m)
    assert lib.smp_multi_set_key_class(m, 0, 1 << 6) == 1 and ' keys=F# mode=sample\n' in M.text(m)
    assert lib.smp_multi_set_key_class(m, 0, 0) == 0 and lib.smp_multi_set_key_class(m, 0, 4096) == 0
    lib.smp_multi_set_range(m, 0, 10, 20, 1, 127)                       # a range again
    assert lib.smp_multi_key_class(m, 0) == 0 and ' keys=A#-1..G#0 mode=sample\n' in M.text(m)
    r, _, err = M.apply(m, 'sample "%s" keys=Bb\n' % a)
    assert r == 0 and lib.smp_multi_key_class(m, 0) == 1 << 10, err
    r, _, err = M.apply(m, 'sample "%s" keys=H\n' % a)
    assert r != 0 and "keys" in err


def test_several_notes_in_every_octave(M, lib):
    lib.smp_multi_key_class.restype, lib.smp_multi_key_class.argtypes = c_int, [vp, c_int]
    lib.smp_note_class_parse.restype, lib.smp_note_class_parse.argtypes = c_int, [c_char_p]
    parse = lambda t: lib.smp_note_class_parse(t.encode())
    assert parse("D F") == (1 << 2) | (1 << 5)
    assert parse("d, f") == parse("D,F") == parse("D F")
    assert parse("D#") == 1 << 3 and parse("Eb") == 1 << 3
    assert parse("C E G") == 1 | 1 << 4 | 1 << 7
    for bad in ("", "D4", "H", "D F4", "C-1", "Dx"):
        assert parse(bad) == 0, bad
    a = M.wav("a.wav", 220)
    m = M.new()
    r, _, err = M.apply(m, 'sample "%s" keys=D,F\n%s' % (a, BASE))
    assert r == 0, err
    assert M.line(m, 0).endswith("keys every D and F, velocity 1 to 127, sample mode")
    assert ' keys=D,F mode=sample\n' in M.text(m)
    for k in (50, 53, 62, 65):
        assert np.abs(M.render(m, "on %d 100 0\noff %d 20000" % (k, k))).max() > 0.1, k
    for k in (51, 52, 54, 60):                                  # D# and the rest: not unless typed
        assert np.abs(M.render(m, "on %d 100 0\noff %d 20000" % (k, k))).max() < 1e-6, k
    r, _, err = M.apply(m, 'sample "%s" keys=C,E,G\n' % a)
    assert r == 0 and M.line(m, 0).endswith("keys every C, E and G, velocity 1 to 127"), err


def test_picked_samples(M, lib):
    lib.smp_multi_picked.restype, lib.smp_multi_picked.argtypes = c_int, [vp, c_int]
    lib.smp_multi_pick.restype, lib.smp_multi_pick.argtypes = None, [vp, c_int, c_int]
    m = M.new()
    a, b, c = M.wav("a.wav", 220), M.wav("b.wav", 330), M.wav("c.wav", 440)
    r, _, err = M.apply(m, 'sample "%s"\nsample "%s"\nsample "%s"\n' % (a, b, c))
    assert r == 0, err
    picked = lambda: [lib.smp_multi_picked(m, i) for i in range(lib.smp_multi_count(m))]
    assert picked() == [1, 0, 0]                       # the selected one, after a load
    lib.smp_multi_pick(m, 2, 1)
    assert picked() == [1, 0, 1] and lib.smp_multi_selected(m) == 0
    lib.smp_multi_remove(m, 0)
    assert picked() == [0, 1]                          # the others keep theirs
    lib.smp_multi_pick(m, 1, 0)
    lib.smp_multi_remove(m, 1)
    assert picked() == [1]                             # none left picked: the selected one is
    m2 = M.new()
    lib.smp_multi_add(m2, M.load(a)); lib.smp_multi_add(m2, M.load(b))
    assert [lib.smp_multi_picked(m2, i) for i in (0, 1)] == [1, 0]   # the first one added to an empty list


def pitch(lib, m, i):
    lo, hi = c_dbl(), c_dbl()
    lib.smp_multi_pitch(m, i, ctypes.byref(lo), ctypes.byref(hi))
    return lo.value, hi.value


def test_each_sample_has_its_own_mode(M, lib):
    """Two samples on the same keys: one in sample mode (every key at its own pitch), one in note mode (from its root).
    Played an octave above both roots, only the note-mode one moves."""
    a, b = M.wav("a A3.wav", 220), M.wav("b A4.wav", 440)       # roots A3 (57) and A4 (69)
    m = M.new()
    r, _, err = M.apply(m, 'sample "%s" keys=C-1..G9 mode=sample\nsample "%s" velocity=1..1\nkeep_length off\n' % (a, b))
    assert r == 0, err
    assert [lib.smp_multi_mode(m, i) for i in (0, 1)] == [0, 1]
    assert peak_hz(M.render(m, "on 69 100 0\noff 69 20000")) == pytest.approx(220, abs=5)   # sample mode: as recorded
    lib.smp_multi_set_range(m, 0, 0, 127, 1, 1)                 # now only the second sample sounds at velocity 100
    lib.smp_multi_set_range(m, 1, 0, 127, 1, 127)
    assert peak_hz(M.render(m, "on 81 100 0\noff 81 20000")) == pytest.approx(880, abs=8)   # note mode: an octave up
    lib.smp_multi_set_mode(m, 1, 0)                             # the second in sample mode too
    assert peak_hz(M.render(m, "on 81 100 0\noff 81 20000")) == pytest.approx(440, abs=5)
    lib.smp_multi_set_mode(m, 1, 7)                             # not a mode: nothing changes
    assert lib.smp_multi_mode(m, 1) == 0


def test_free_mode_pitch_is_each_samples(M, lib):
    """Free mode: the sample's Start key plays at its Pitch at start key, its End key at Pitch at end key."""
    a = M.wav("a A3.wav", 220)
    m = M.new()
    r, _, err = M.apply(m, 'sample "%s" keys=C4..C5 mode=free pitch=0..12\nkeep_length off\n' % a)
    assert r == 0, err
    assert lib.smp_multi_mode(m, 0) == 2 and pitch(lib, m, 0) == (0, 12)
    assert peak_hz(M.render(m, "on 60 100 0\noff 60 20000")) == pytest.approx(220, abs=5)
    assert peak_hz(M.render(m, "on 72 100 0\noff 72 20000")) == pytest.approx(440, abs=8)
    assert M.line(m, 0).endswith(", free mode, pitch 0 st to +12 st") or ", free mode, pitch " in M.line(m, 0)
    assert ' mode=free pitch=0..12\n' in M.text(m)
    lib.smp_multi_set_pitch(m, 0, -12, 0)
    assert peak_hz(M.render(m, "on 60 100 0\noff 60 20000")) == pytest.approx(110, abs=4)
    lib.smp_multi_set_pitch(m, 0, -99, 99)                      # clamped to -48 and 48
    assert pitch(lib, m, 0) == (-48, 48)


def test_mode_and_pitch_round_trip(M, lib):
    a, b, c = M.wav("a.wav", 220), M.wav("b.wav", 330), M.wav("c.wav", 440)
    m = M.new()
    r, _, err = M.apply(m, 'sample "%s" mode=sample\nsample "%s" mode=free pitch=-5..7\nsample "%s"\n' % (a, b, c))
    assert r == 0, err
    t = M.text(m)
    m2 = M.new()
    r, _, err = M.apply(m2, t)
    assert r == 0, err
    assert M.text(m2) == t
    assert [lib.smp_multi_mode(m2, i) for i in range(3)] == [0, 2, 1]
    assert pitch(lib, m2, 1) == (-5, 7) and pitch(lib, m2, 2) == (-12, 12)
    s = [x for x in t.splitlines() if x.startswith("sample ")]
    assert s[0].endswith(" mode=sample") and s[1].endswith(" mode=free pitch=-5..7") and "mode=" not in s[2]
    for bad, word in [('sample "%s" mode=loud' % a, "mode"), ('sample "%s" pitch=-60..0' % a, "pitch"),
                      ('sample "%s" pitch=3' % a, "pitch")]:
        r, _, err = M.apply(m, bad + "\n")
        assert r != 0 and word in err, (bad, err)


def test_old_project_mode_is_every_samples(M, lib):
    """An older project's instrument lines (mode, pitch_lo, pitch_hi) become each sample's, unless its line says."""
    a, b = M.wav("a.wav", 220), M.wav("b.wav", 330)
    m = M.new()
    r, _, err = M.apply(m, 'sample "%s"\nsample "%s" mode=note\nmode free\npitch_lo -3 st\npitch_hi 9 st\n' % (a, b))
    assert r == 0, err
    assert [lib.smp_multi_mode(m, i) for i in (0, 1)] == [2, 1]
    assert pitch(lib, m, 0) == (-3, 9) and pitch(lib, m, 1) == (-3, 9)
    assert "\nmode " not in M.text(m) and "\npitch_lo " not in M.text(m)


def test_window_mode_sets_the_picked_samples(M, lib):
    """Mode and the pitches through set_param (the window): every picked sample; get_param reads the selected one's."""
    a, b, c = M.wav("a.wav", 220), M.wav("b.wav", 330), M.wav("c.wav", 440)
    m = M.new()
    r, _, err = M.apply(m, 'sample "%s"\nsample "%s"\nsample "%s"\n' % (a, b, c))
    assert r == 0, err
    lib.smp_multi_pick(m, 0, 0); lib.smp_multi_pick(m, 1, 1); lib.smp_multi_pick(m, 2, 1)
    lib.smp_multi_select(m, 2)
    lib.smp_multi_set_param(m, 11, 2)                          # Mode: free
    lib.smp_multi_set_param(m, 14, -7)                         # Pitch at start key
    assert [lib.smp_multi_mode(m, i) for i in range(3)] == [1, 2, 2]
    assert [pitch(lib, m, i)[0] for i in range(3)] == [-12, -7, -7]
    assert lib.smp_multi_get_param(m, 11) == 2 and lib.smp_multi_get_param(m, 14) == -7
    lib.smp_multi_select(m, 0)
    assert lib.smp_multi_get_param(m, 11) == 1
    lib.smp_multi_set_param(m, 12, 60)                         # Lowest note isn't the instrument's any more
    assert M.range(m, 0) == [0, 127, 1, 127]
