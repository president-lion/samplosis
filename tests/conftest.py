import ctypes
import os
import tempfile

import pytest
import soundfile as sf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_SETTINGS = os.path.join(tempfile.mkdtemp(prefix="samplosis_"), "settings.ini")
os.environ["SAMPLOSIS_SETTINGS"] = _SETTINGS  # must be set before the DLL loads


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
    return dll
