import ctypes
import os
import tempfile

import pytest
import soundfile as sf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_SETTINGS = os.path.join(tempfile.mkdtemp(prefix="samplosis_"), "settings.ini")
os.environ["SAMPLOSIS_SETTINGS"] = _SETTINGS  # must be set before the DLL loads


class SmpParamInfo(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("lo", ctypes.c_double), ("hi", ctypes.c_double),
                ("def_", ctypes.c_double), ("kind", ctypes.c_int)]


def write_wav(path, data, rate):
    """Write a float32 wav (mono or frames x channels)."""
    sf.write(str(path), data, rate, subtype="FLOAT")


@pytest.fixture(scope="session")
def settings_file():
    return _SETTINGS


@pytest.fixture(scope="session")
def lib(settings_file):
    dll = ctypes.CDLL(os.path.join(ROOT, "samplosis.clap"))
    dll.smp_version.restype = ctypes.c_char_p
    dll.smp_note_parse.restype = ctypes.c_int
    dll.smp_note_parse.argtypes = [ctypes.c_char_p]
    dll.smp_note_name.restype = None
    dll.smp_note_name.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
    dll.smp_note_from_filename.restype = ctypes.c_int
    dll.smp_note_from_filename.argtypes = [ctypes.c_char_p]
    dll.smp_detect_pitch.restype = ctypes.c_double
    dll.smp_detect_pitch.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.c_long, ctypes.c_int]
    dll.smp_value_text.restype = ctypes.c_int
    dll.smp_value_text.argtypes = [ctypes.c_int, ctypes.c_double, ctypes.c_char_p, ctypes.c_int]
    dll.smp_value_parse.restype = ctypes.c_int
    dll.smp_value_parse.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(ctypes.c_double)]
    dll.smp_param_info.restype = ctypes.POINTER(SmpParamInfo)
    dll.smp_param_info.argtypes = [ctypes.c_int]
    return dll
