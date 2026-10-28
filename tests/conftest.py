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


class SmpSample(ctypes.Structure):
    _fields_ = [("path", ctypes.c_char * 1024), ("name", ctypes.c_char * 256),
                ("src_key", ctypes.c_uint64), ("rev_key", ctypes.c_uint64),
                ("rate", ctypes.c_int), ("frames", ctypes.c_uint64),
                ("root", ctypes.c_int), ("root_from", ctypes.c_int),
                ("fwd", ctypes.c_void_p), ("rev", ctypes.c_void_p)]


class SmpSettings(ctypes.Structure):
    _fields_ = [("cache_dir", ctypes.c_char * 1024), ("cache_limit_gb", ctypes.c_double)]


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
    c_char_p, c_int, c_long, c_u64, c_dbl = ctypes.c_char_p, ctypes.c_int, ctypes.c_long, ctypes.c_uint64, ctypes.c_double
    fp = ctypes.POINTER(ctypes.c_float)
    dll.smp_settings_get.restype = None
    dll.smp_settings_get.argtypes = [ctypes.POINTER(SmpSettings)]
    dll.smp_settings_set.restype = c_int
    dll.smp_settings_set.argtypes = [ctypes.POINTER(SmpSettings)]
    dll.smp_key.restype = c_u64
    dll.smp_key.argtypes = [c_char_p]
    dll.smp_cache_write.restype = c_int
    dll.smp_cache_write.argtypes = [c_u64, fp, c_u64, c_int]
    dll.smp_cache_open.restype = ctypes.c_void_p
    dll.smp_cache_open.argtypes = [c_u64]
    dll.smp_cache_close.restype = None
    dll.smp_cache_close.argtypes = [ctypes.c_void_p]
    dll.smp_entry_frames.restype = c_u64
    dll.smp_entry_frames.argtypes = [ctypes.c_void_p]
    dll.smp_entry_rate.restype = c_int
    dll.smp_entry_rate.argtypes = [ctypes.c_void_p]
    dll.smp_entry_whole.restype = c_int
    dll.smp_entry_whole.argtypes = [ctypes.c_void_p]
    dll.smp_entry_head.restype = fp
    dll.smp_entry_head.argtypes = [ctypes.c_void_p, ctypes.POINTER(c_u64)]
    dll.smp_entry_read.restype = c_long
    dll.smp_entry_read.argtypes = [ctypes.c_void_p, c_u64, fp, c_long]
    dll.smp_cache_trim.restype = c_u64
    dll.smp_cache_trim.argtypes = [c_u64]
    dll.smp_cache_ok.restype = c_int
    dll.smp_cache_ok.argtypes = [c_char_p, c_int]
    sp = ctypes.POINTER(SmpSample)
    dll.smp_sample_load.restype = sp
    dll.smp_sample_load.argtypes = [c_char_p, c_char_p, c_int]
    dll.smp_sample_load_known.restype = sp
    dll.smp_sample_load_known.argtypes = [c_char_p, c_u64, c_int, c_char_p, c_int]
    dll.smp_sample_reversed.restype = ctypes.c_void_p
    dll.smp_sample_reversed.argtypes = [sp]
    dll.smp_sample_free.restype = None
    dll.smp_sample_free.argtypes = [sp]
    dll.smp_debug_decodes.restype = c_long
    dll.smp_debug_decodes.argtypes = []
    return dll
