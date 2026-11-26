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
    for name, res, args in [
        ("smp_multi_new", vp, [c_dbl, c_int]), ("smp_multi_free", None, [vp]), ("smp_multi_count", c_int, [vp]),
        ("smp_multi_add", c_int, [vp, vp]), ("smp_multi_replace", c_int, [vp, c_int, vp]),
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
    ]:
        f = getattr(lib, name)
        f.restype, f.argtypes = res, args
    lib.smp_sample_load.restype = vp

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
    lib.smp_multi_set_param(m, 11, 0)                          # mode sample
    y = M.render(m, "on 60 100 0\noff 60 20000")[:, 0][2000:14000]
    s = np.abs(np.fft.rfft(y * np.hanning(len(y))))
    f = np.fft.rfftfreq(len(y), 1 / RATE)
    at = lambda hz: s[np.argmin(np.abs(f - hz))]
    assert at(220) > 0.2 * s.max() and at(880) > 0.2 * s.max()


def test_instrument_range_still_limits(M, lib):
    m, _, _ = two(M, lib, "note_hi B3\n")                     # the instrument's Highest note: B3 for every sample
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
    assert M.line(m, 0) == "low.wav, root A3, keys F#1 to E2, velocity 15 to 80"


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
    assert samples[0].startswith('sample "%s" key=' % lo) and samples[0].endswith(" root=A3 keys=C-1..B3")
    assert samples[1].endswith(" root=A4 keys=C4..C5 velocity=20..100")
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


def test_one_sample_text_is_as_before(M, lib):
    w = M.wav("tone C3.wav", 220)
    old = engine_text(lib, 'sample "%s"\nkeep_length off\nrelease 50 ms\n' % w)
    m = M.new()
    r, miss, err = M.apply(m, old)
    assert r == 0 and miss == 0, err
    assert M.text(m) == old                                    # no root=, keys= or velocity= with one sample
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
    assert M.line(m, 1) == "Missing: %s, root C4, keys C4 to C5, velocity 1 to 127" % gone
    assert 'sample "%s" key=0123456789abcdef root=C4 keys=C4..C5' % gone in M.text(m)
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
