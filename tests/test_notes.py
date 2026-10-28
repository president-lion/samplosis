def test_version(lib):
    assert lib.smp_version() == b"0.1.0"


import ctypes
import pytest

K_NOTE, K_SEMI, K_CENT, K_DB, K_PAN, K_MS, K_PERCENT, K_CHOICE, K_BOOL, K_COUNT, K_POSITION = range(11)
NPARAMS = 29


@pytest.mark.parametrize("s,n", [("C-1", 0), ("C4", 60), ("a4", 69), ("Db4", 61), ("B#3", 60),
                                 ("61", 61), ("H2", -1), ("128", -1), ("C", -1), ("", -1), ("G9", 127)])
def test_note_parse(lib, s, n):
    assert lib.smp_note_parse(s.encode()) == n


def test_note_name(lib):
    def name(n):
        b = ctypes.create_string_buffer(16)
        lib.smp_note_name(n, b, 16)
        return b.value.decode()
    assert name(61) == "C#4" and name(0) == "C-1" and name(60) == "C4" and name(127) == "G9"


@pytest.mark.parametrize("p,n", [("Guitar C3.wav", 48), ("piano_f#2_soft.flac", 42), ("Bb4 pluck.wav", 70),
                                 ("Cello A 440.wav", 69), ("440.wav", 69), ("A440.wav", 69), ("Take2.wav", -1),
                                 ("Chord.wav", -1), ("E:/x/Kick 01.wav", -1), ("E:/c3 stuff/Kick.wav", -1),
                                 ("x/Pad (D2) long.wav", 38), ("C3 then E4.wav", 64)])
def test_note_from_filename(lib, p, n):
    assert lib.smp_note_from_filename(p.encode()) == n


def test_param_table(lib):
    names = [lib.smp_param_info(i).contents.name.decode() for i in range(NPARAMS)]
    assert names[0] == "root" and names[1] == "transpose" and names[25] == "keep_length" and names[28] == "speed"
    assert len(set(names)) == NPARAMS
    assert not lib.smp_param_info(NPARAMS) and not lib.smp_param_info(-1)
    pi = lib.smp_param_info(11).contents
    assert (pi.lo, pi.hi, pi.def_, pi.kind) == (0, 2, 1, K_CHOICE)


def txt(lib, p, v):
    b = ctypes.create_string_buffer(64)
    lib.smp_value_text(p, v, b, 64)
    return b.value.decode()


def parse(lib, p, s):
    v = ctypes.c_double()
    ok = lib.smp_value_parse(p, s.encode(), ctypes.byref(v))
    return v.value if ok else None


def test_value_words(lib):
    assert txt(lib, 0, 48) == "C3"
    assert txt(lib, 1, 7) == "+7 st"
    assert txt(lib, 2, -12) == "-12 cents"
    assert txt(lib, 3, -6) == "-6.0 dB"
    assert txt(lib, 3, -60) == "off"
    assert txt(lib, 4, 0) == "center" and txt(lib, 4, -30) == "30 left" and txt(lib, 4, 30) == "30 right"
    assert txt(lib, 5, 120) == "120 ms" and txt(lib, 5, 1200) == "1.20 s"
    assert txt(lib, 17, 1) == "on" and txt(lib, 24, 0) == "off"
    assert txt(lib, 11, 2) == "free" and txt(lib, 18, 2) == "pingpong"
    assert txt(lib, 20, 12.5) == "12.5 %"
    assert parse(lib, 3, "-6 dB") == -6 and parse(lib, 3, "-6") == -6 and parse(lib, 3, "off") == -60
    assert parse(lib, 0, "C3") == 48 and parse(lib, 17, "on") == 1
    assert parse(lib, 4, "30 left") == -30 and parse(lib, 4, "30 right") == 30 and parse(lib, 4, "center") == 0
    assert parse(lib, 5, "1.2 s") == 1200 and parse(lib, 5, "120") == 120
    assert parse(lib, 1, "+7 st") == 7 and parse(lib, 1, "nonsense") is None
    assert parse(lib, 1, "999") == 48  # clamped to range


def test_value_roundtrip(lib):
    prec = {K_NOTE: 1, K_SEMI: 1, K_CENT: 1, K_DB: 0.1, K_PAN: 1, K_MS: 10, K_PERCENT: 0.1,
            K_CHOICE: 1, K_BOOL: 1, K_COUNT: 1}
    for p in range(NPARAMS):
        pi = lib.smp_param_info(p).contents
        for i in range(5):
            v = pi.lo + (pi.hi - pi.lo) * i / 4
            if pi.kind in (K_CHOICE, K_BOOL, K_NOTE, K_COUNT, K_SEMI, K_CENT, K_PAN):
                v = round(v)
            s = txt(lib, p, v)
            back = parse(lib, p, s)
            assert back is not None, (pi.name, v, s)
            assert abs(back - v) <= prec[pi.kind] / 2 + 1e-9, (pi.name, v, s, back)
