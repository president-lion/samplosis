"""Run by test_gig.py in a child process, so its memory and its process-wide state are its own:

python -I gigchild.py open PATH                 opens PATH, prints the result and the growth of peak committed memory
python -I gigchild.py fault parse|decode GOOD   makes libgig crash inside its guard, then checks every later open

Prints one line of JSON. SAMPLOSIS_SETTINGS must be set by the caller (before the plugin loads)."""
import ctypes
import ctypes.wintypes as w
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Counters(ctypes.Structure):
    _fields_ = [("cb", w.DWORD), ("PageFaultCount", w.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t)]


def peak_commit():
    c = Counters()
    c.cb = ctypes.sizeof(c)
    k = ctypes.windll.kernel32
    k.GetCurrentProcess.restype = w.HANDLE
    k.K32GetProcessMemoryInfo.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD]
    k.K32GetProcessMemoryInfo(k.GetCurrentProcess(), ctypes.byref(c), c.cb)
    return c.PeakPagefileUsage


def load():
    dll = ctypes.CDLL(os.path.join(ROOT, "samplosis.clap"))
    dll.smp_sf_open.restype = ctypes.c_void_p
    dll.smp_sf_open.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    for name in ("smp_sf_close", "smp_sf_preset_count", "smp_sf_sample_count"):
        getattr(dll, name).argtypes = [ctypes.c_void_p]
    dll.smp_sf_use.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    dll.smp_sf_wait_idle.argtypes = [ctypes.c_void_p, ctypes.c_int]
    dll.smp_sf_problem.restype = ctypes.c_char_p
    dll.smp_sf_problem.argtypes = [ctypes.c_void_p]
    dll.smp_sf_test_gig_guards.restype = ctypes.c_long
    return dll


def sf_open(dll, path):
    err = ctypes.create_string_buffer(700)
    f = dll.smp_sf_open(path.encode("utf-8"), err, 700)
    return f, err.value.decode("utf-8", "replace")


def use_all(dll, f):
    n = dll.smp_sf_preset_count(f)
    for i in range(n):
        dll.smp_sf_use(f, i, 1)
    dll.smp_sf_wait_idle(f, 20000)
    for i in range(n):
        dll.smp_sf_use(f, i, -1)


def main():
    dll = load()
    out = {}
    if sys.argv[1] == "open":
        before = peak_commit()
        f, err = sf_open(dll, sys.argv[2])
        out["opened"] = bool(f)
        out["err"] = err
        if f:
            use_all(dll, f)
            out["problem"] = (dll.smp_sf_problem(f) or b"").decode("utf-8", "replace")
            dll.smp_sf_close(f)
        out["grow_mb"] = (peak_commit() - before) / 1048576.0
    else:
        where = 1 if sys.argv[2] == "parse" else 2
        dll.smp_sf_test_gig_fault.argtypes = [ctypes.c_int]
        dll.smp_sf_test_gig_fault(where)
        f, err = sf_open(dll, sys.argv[3])
        out["first_opened"] = bool(f)
        out["first_err"] = err
        if f:                                   # the decode fault: it trips when the compressed wave loads
            use_all(dll, f)
            out["problem"] = (dll.smp_sf_problem(f) or b"").decode("utf-8", "replace")
            dll.smp_sf_close(f)
        f, err = sf_open(dll, sys.argv[3])
        out["second_opened"] = bool(f)
        out["second_err"] = err
        if f:
            dll.smp_sf_close(f)
        out["guards"] = dll.smp_sf_test_gig_guards()
    print(json.dumps(out))


main()
