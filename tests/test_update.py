"""The Update Samplosis button: a stand-in server plays GitHub's latest-release answer and the .clap download."""
import ctypes
import http.server
import json
import os
import shutil
import threading

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLAP = os.path.join(ROOT, "samplosis.clap")


@pytest.fixture
def up(lib):
    lib.smp_version_newer.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    lib.smp_version_newer.restype = ctypes.c_int
    lib.smp_update_run.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    lib.smp_update_run.restype = ctypes.c_int
    lib.smp_update_cleanup.argtypes = [ctypes.c_char_p]
    lib.smp_update_cleanup.restype = None

    def run(url, target):
        m = ctypes.create_string_buffer(600)
        r = lib.smp_update_run(url.encode() if url else None, str(target).encode() if target else None, m, 600)
        return r, m.value.decode()
    lib.run = run
    return lib


@pytest.fixture
def server():
    """routes: path -> (status, bytes). Yields (base url, routes)."""
    routes = {}

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            st, body = routes.get(self.path, (404, b"not here"))
            if st in (301, 302):
                self.send_response(st)
                self.send_header("Location", body.decode())
                self.end_headers()
                return
            self.send_response(st)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    s = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    t = threading.Thread(target=s.serve_forever, daemon=True)
    t.start()
    base = "http://127.0.0.1:%d" % s.server_address[1]
    yield base, routes
    s.shutdown()
    s.server_close()


def release(base, tag, asset="/dl/samplosis.clap"):
    return json.dumps({"tag_name": tag, "name": "Samplosis", "assets": [
        {"name": "notes.txt", "browser_download_url": base + "/dl/notes.txt"},
        {"name": "samplosis.clap", "browser_download_url": base + asset}]}, indent=2).encode()


def new_clap():
    return b"MZ" + b"\x90" * (200 * 1024) + b"new"


def test_version_newer(up):
    v = up.smp_version_newer
    assert v(b"v0.2.1", b"0.2.0") == 1 and v(b"0.3", b"0.2.9") == 1 and v(b"v1.0.0", b"0.9.9") == 1
    assert v(b"v0.2.0", b"0.2.0") == 0 and v(b"v0.1.9", b"0.2.0") == 0 and v(b"0.10.0", b"0.9.0") == 1
    assert v(b"latest", b"0.2.0") == -1 and v(b"", b"0.2.0") == -1


def test_update_installs_a_newer_release(up, server, tmp_path):
    base, routes = server
    routes["/latest"] = (200, release(base, "v99.0.0"))
    routes["/dl/samplosis.clap"] = (302, (base + "/cdn/x.clap").encode())    # GitHub redirects downloads
    routes["/cdn/x.clap"] = (200, new_clap())
    target = tmp_path / "samplosis.clap"
    target.write_bytes(b"MZ old")
    r, m = up.run(base + "/latest", target)
    assert (r, m) == (1, "Installed Samplosis 99.0.0. Restart REAPER to use it")
    assert target.read_bytes() == new_clap()
    assert (tmp_path / "samplosis.clap.old").read_bytes() == b"MZ old"      # moved aside, not overwritten
    assert not (tmp_path / "samplosis.clap.new").exists()
    up.smp_update_cleanup(str(target).encode())                              # the next load deletes it
    assert sorted(os.listdir(tmp_path)) == ["samplosis.clap"]


def test_update_replaces_a_loaded_plugin(up, server, tmp_path):
    """the host has the .clap loaded: it is renamed aside, and a second update moves the next one aside too"""
    base, routes = server
    routes["/latest"] = (200, release(base, "v99.0.0"))
    target = tmp_path / "samplosis.clap"
    shutil.copy(CLAP, target)
    loaded = ctypes.WinDLL(str(target))
    try:
        routes["/dl/samplosis.clap"] = (200, new_clap())
        assert up.run(base + "/latest", target)[0] == 1
        assert target.read_bytes() == new_clap()
        shutil.copy(CLAP, target)                                            # loaded again, before a restart
        loaded2 = ctypes.WinDLL(str(target))
        try:
            assert up.run(base + "/latest", target)[0] == 1
            assert target.read_bytes() == new_clap()
            assert sorted(os.listdir(tmp_path)) == ["samplosis.clap", "samplosis.clap.old", "samplosis.clap.old1"]
            # Windows hands back the module it already has for that path, so only .old is loaded: it stays
            up.smp_update_cleanup(str(target).encode())
            assert sorted(os.listdir(tmp_path)) == ["samplosis.clap", "samplosis.clap.old"]
        finally:
            ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.FreeLibrary(loaded2._handle)
    finally:
        ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
        ctypes.windll.kernel32.FreeLibrary(loaded._handle)
    up.smp_update_cleanup(str(target).encode())
    assert sorted(os.listdir(tmp_path)) == ["samplosis.clap"]


def test_update_already_newest(up, server, tmp_path):
    base, routes = server
    mine = up.smp_version().decode()
    routes["/latest"] = (200, release(base, "v" + mine))
    target = tmp_path / "samplosis.clap"
    target.write_bytes(b"MZ old")
    assert up.run(base + "/latest", target) == (0, "Samplosis %s is the newest" % mine)
    assert os.listdir(tmp_path) == ["samplosis.clap"] and target.read_bytes() == b"MZ old"


@pytest.mark.parametrize("routes_set, said", [
    ({}, "Update failed: 127.0.0.1 answered 404"),
    ({"/latest": (200, b'{"name": "x"}')}, "Update failed: the latest release has no version"),
    ({"/latest": "nodl"}, "Update failed: release v99.0.0 has no samplosis.clap"),
    ({"/latest": "rel", "/dl/samplosis.clap": (200, b"<html>not a plugin</html>")},
     "Update failed: the download isn't a plugin"),
    ({"/latest": "rel", "/dl/samplosis.clap": (500, b"")}, "Update failed: 127.0.0.1 answered 500"),
])
def test_update_failures_leave_the_plugin(up, server, tmp_path, routes_set, said):
    base, routes = server
    for k, v in routes_set.items():
        if v == "rel":
            v = (200, release(base, "v99.0.0"))
        elif v == "nodl":
            v = (200, json.dumps({"tag_name": "v99.0.0", "assets": []}).encode())
        routes[k] = v
    target = tmp_path / "samplosis.clap"
    target.write_bytes(b"MZ old")
    assert up.run(base + "/latest", target) == (-1, said)
    assert os.listdir(tmp_path) == ["samplosis.clap"] and target.read_bytes() == b"MZ old"


def test_update_no_server(up, tmp_path):
    target = tmp_path / "samplosis.clap"
    target.write_bytes(b"MZ old")
    r, m = up.run("http://127.0.0.1:1/latest", target)
    assert r == -1 and m.startswith("Update failed: couldn't reach 127.0.0.1"), m
    assert target.read_bytes() == b"MZ old"
