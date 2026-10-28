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
    _fields_ = [("cache_dir", ctypes.c_char * 1024), ("cache_limit_gb", ctypes.c_double),
                ("stretcher", ctypes.c_int), ("keep_tone", ctypes.c_int)]


class SmpSfMod(ctypes.Structure):
    _fields_ = [("src", ctypes.c_uint16), ("dest", ctypes.c_uint16), ("amount", ctypes.c_int16),
                ("amt_src", ctypes.c_uint16), ("trans", ctypes.c_uint16)]


class SmpSfSample(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char * 21), ("start", ctypes.c_uint32), ("end", ctypes.c_uint32),
                ("loop_start", ctypes.c_uint32), ("loop_end", ctypes.c_uint32), ("rate", ctypes.c_uint32),
                ("pitch", ctypes.c_uint8), ("correction", ctypes.c_int8), ("link", ctypes.c_uint16),
                ("type", ctypes.c_uint16)]


class SmpSfRegion(ctypes.Structure):
    _fields_ = [("lokey", ctypes.c_uint8), ("hikey", ctypes.c_uint8), ("lovel", ctypes.c_uint8),
                ("hivel", ctypes.c_uint8), ("sample", ctypes.c_int), ("gen", ctypes.c_int16 * 60),
                ("imod", ctypes.POINTER(SmpSfMod)), ("nimod", ctypes.c_int),
                ("pmod", ctypes.POINTER(SmpSfMod)), ("npmod", ctypes.c_int)]


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
    vp = ctypes.c_void_p
    dll.smp_engine_new.restype = vp
    dll.smp_engine_new.argtypes = [c_dbl, c_int]
    dll.smp_engine_free.restype = None
    dll.smp_engine_free.argtypes = [vp]
    dll.smp_engine_set_sample.restype = None
    dll.smp_engine_set_sample.argtypes = [vp, sp]
    dll.smp_engine_set_param.restype = None
    dll.smp_engine_set_param.argtypes = [vp, c_int, c_dbl]
    dll.smp_engine_get_param.restype = c_dbl
    dll.smp_engine_get_param.argtypes = [vp, c_int]
    dll.smp_engine_note_on.restype = None
    dll.smp_engine_note_on.argtypes = [vp, c_int, c_int, c_int]
    dll.smp_engine_note_off.restype = None
    dll.smp_engine_note_off.argtypes = [vp, c_int, c_int]
    dll.smp_engine_bend.restype = None
    dll.smp_engine_bend.argtypes = [vp, c_dbl, c_int]
    dll.smp_engine_process.restype = None
    dll.smp_engine_process.argtypes = [vp, fp, fp, c_int]
    dll.smp_engine_active_voices.restype = c_int
    dll.smp_engine_active_voices.argtypes = [vp]
    dll.smp_engine_underruns.restype = c_long
    dll.smp_engine_underruns.argtypes = [vp]
    dll.smp_engine_seed.restype = None
    dll.smp_engine_seed.argtypes = [vp, ctypes.c_uint]
    dll.smp_engine_collect.restype = None
    dll.smp_engine_collect.argtypes = [vp]
    dll.smp_engine_render_test.restype = c_int
    dll.smp_engine_render_test.argtypes = [vp, c_char_p, c_long, c_int, c_char_p]
    ip = ctypes.POINTER(c_int)
    dll.smp_sf_open.restype = vp
    dll.smp_sf_open.argtypes = [c_char_p, c_char_p, c_int]
    dll.smp_sf_close.restype = None
    dll.smp_sf_close.argtypes = [vp]
    dll.smp_sf_preset_count.restype = c_int
    dll.smp_sf_preset_count.argtypes = [vp]
    dll.smp_sf_preset_info.restype = c_int
    dll.smp_sf_preset_info.argtypes = [vp, c_int, ip, ip, c_char_p, c_int]
    dll.smp_sf_find_preset.restype = c_int
    dll.smp_sf_find_preset.argtypes = [vp, c_int, c_int]
    dll.smp_sf_sample_count.restype = c_int
    dll.smp_sf_sample_count.argtypes = [vp]
    dll.smp_sf_sample.restype = ctypes.POINTER(SmpSfSample)
    dll.smp_sf_sample.argtypes = [vp, c_int]
    dll.smp_sf_regions.restype = c_int
    dll.smp_sf_regions.argtypes = [vp, c_int, ctypes.POINTER(ctypes.POINTER(SmpSfRegion))]
    return dll
