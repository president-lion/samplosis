import ctypes
import random

import numpy as np
import pytest

from conftest import write_wav
from test_cache import point_cache

NP = 31


@pytest.fixture
def S(lib, tmp_path):
    point_cache(lib, tmp_path / "cache")
    lib.smp_script_write.restype = ctypes.c_int
    lib.smp_script_write.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    lib.smp_script_apply.restype = ctypes.c_int
    lib.smp_script_apply.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    lib.smp_value_parse.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(ctypes.c_double)]

    class H:
        engines = []

        def engine(self):
            e = lib.smp_engine_new(48000.0, 256)
            self.engines.append(e)
            return e

        def sample(self, name="tone A3.wav"):
            f = tmp_path / name
            write_wav(f, (0.3 * np.sin(np.arange(44100) * 0.05)).astype(np.float32), 44100)
            err = ctypes.create_string_buffer(512)
            s = lib.smp_sample_load(str(f).encode(), err, 512)
            assert s, err.value
            return f, s

        def write(self, e):
            n = lib.smp_script_write(e, None, 0)
            buf = ctypes.create_string_buffer(n + 1)
            assert lib.smp_script_write(e, buf, n + 1) == n
            return buf.value.decode()

        def apply(self, e, text):
            err = ctypes.create_string_buffer(512)
            r = lib.smp_script_apply(e, text.encode(), err, 512)
            return r, err.value.decode()

        def params(self, e):
            return [lib.smp_engine_get_param(e, i) for i in range(NP)]

    h = H()
    yield h
    for e in h.engines:
        lib.smp_engine_free(e)


def test_roundtrip(lib, S):
    f, s = S.sample()
    a = S.engine()
    lib.smp_engine_set_sample(a, s)
    rnd = random.Random(7)
    for i in range(NP):
        info = lib.smp_param_info(i).contents
        lib.smp_engine_set_param(a, i, rnd.uniform(info.lo, info.hi))
    t1 = S.write(a)
    assert t1.startswith("# Samplosis 0.1.0\nsample \"" + str(f) + "\" key=")
    assert len(t1.splitlines()) == NP + 2
    b = S.engine()
    r, err = S.apply(b, t1)
    assert r == 0, err
    assert S.write(b) == t1


def test_write_without_sample_and_small_cap(lib, S):
    e = S.engine()
    t = S.write(e)
    assert "sample" not in t.split("\n", 1)[1].split(" ")[0]
    assert len(t.splitlines()) == NP + 1
    small = ctypes.create_string_buffer(10)
    assert lib.smp_script_write(e, small, 10) == len(t)
    assert small.value.decode() == t[:9]


def test_partial_and_order(lib, S):
    e = S.engine()
    defaults = S.params(e)
    r, err = S.apply(e, "volume -3\nroot A3")
    assert r == 0, err
    p = S.params(e)
    assert p[3] == -3 and p[0] == 57
    for i in range(NP):
        if i not in (0, 3):
            assert p[i] == defaults[i], i
    # a second script resets what it leaves out
    assert S.apply(e, "pan 20 left")[0] == 0
    p = S.params(e)
    assert p[4] == -20 and p[3] == 0


def test_root_rules(lib, S):
    f, s = S.sample("tone A3.wav")
    e = S.engine()
    r, err = S.apply(e, 'sample "%s"\nvolume -2' % f)
    assert r == 0, err
    assert lib.smp_engine_get_param(e, 0) == 57          # the sample's root
    r, err = S.apply(e, 'sample "%s"\nroot C3' % f)
    assert r == 0, err
    assert lib.smp_engine_get_param(e, 0) == 48          # the explicit line wins
    lib.smp_sample_free(s)


def test_sample_gone_uses_cache_key(lib, S):
    f, s = S.sample()
    a = S.engine()
    lib.smp_engine_set_sample(a, s)
    t = S.write(a)
    f.unlink()
    b = S.engine()
    r, err = S.apply(b, t)
    assert r == 0, err
    assert S.write(b) == t
    c = S.engine()
    r, err = S.apply(c, 'sample "%s"' % f)
    assert r != 0 and "line 1" in err and "isn't there" in err
    assert S.params(c)[3] == 0


def test_errors(lib, S):
    e = S.engine()
    lib.smp_engine_set_param(e, 3, -9)
    before = S.write(e)
    cases = [("volme -3", "line 1", "volme"), ("root H9", "line 1", "root"),
             ("volume -3\n\nvolume loud", "line 3", "loud"),
             ("volume nan", "line 1", "nan"), ("volume inf", "line 1", "inf"),
             ("volume", "line 1", "volume"),
             ("# c\nsample E:/x.wav", "line 2", "sample"),
             ('sample "E:/x.wav" key=zz', "line 1", "key"),
             ('sample "E:/x.wav', "line 1", "quote")]
    for text, ln, word in cases:
        r, err = S.apply(e, text)
        assert r != 0, text
        assert ln in err and word in err, (text, err)
        assert S.write(e) == before, text
    r, err = S.apply(e, "volme -3")
    assert err == "line 1: unknown setting 'volme'"


def test_bad_line_after_good_changes_nothing(lib, S):
    e = S.engine()
    r, err = S.apply(e, "pan 10 left\nvolume -3\nbogus 1")
    assert r != 0 and "line 3" in err
    assert S.params(e)[4] == 0 and S.params(e)[3] == 0


def test_comments_and_blank_lines(lib, S):
    e = S.engine()
    text = "# Samplosis 0.1.0\r\n\r\n   # note\n  volume   -6.0 dB  \r\n\n\tmode free\n# end"
    r, err = S.apply(e, text)
    assert r == 0, err
    p = S.params(e)
    assert p[3] == -6 and p[11] == 2


def test_nan_rejected_by_value_parse(lib):
    v = ctypes.c_double()
    for p, s in [(3, b"nan"), (3, b"inf"), (3, b"-inf"), (1, b"nan"), (11, b"nan"), (17, b"inf")]:
        assert lib.smp_value_parse(p, s, ctypes.byref(v)) == 0, s


def test_same_sample_not_reloaded(lib, S):
    # REAPER re-applies state often: a script naming the sample already loaded (same path and key) keeps it
    f, s = S.sample()
    e = S.engine()
    lib.smp_engine_set_sample(e, s)
    lib.smp_engine_sample.restype = ctypes.c_void_p
    lib.smp_engine_sample.argtypes = [ctypes.c_void_p]
    before = lib.smp_engine_sample(e)
    t = S.write(e)
    r, err = S.apply(e, t.replace("volume 0.0 dB", "volume -4.0 dB"))
    assert r == 0, err
    assert lib.smp_engine_sample(e) == before
    assert lib.smp_engine_get_param(e, 3) == -4
    # another path (or key) does swap
    g = f.with_name("other A3.wav")
    g.write_bytes(f.read_bytes())
    r, err = S.apply(e, 'sample "%s"' % g)
    assert r == 0, err
    assert lib.smp_engine_sample(e) != before


def test_ignores_soundfont_lines(lib, S):
    """a combined state text: the SoundFont player's lines and plays are left to others"""
    f, s = S.sample()
    a = S.engine()
    lib.smp_engine_set_sample(a, s)
    t1 = S.write(a)
    mixed = ("plays soundfont\n" + t1 + 'soundfont "D:\\fonts\\GM.sf2"\nsf_mode all\nsf_used 0:0 128:0\n'
             "sf_volume -6.0 dB\nsf_whatever anything at all\n")
    b = S.engine()
    r, err = S.apply(b, mixed)
    assert r == 0, err
    assert S.write(b) == t1
    assert S.apply(b, "sfx 1\n") == (1, "line 1: unknown setting 'sfx'")


