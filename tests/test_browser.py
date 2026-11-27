"""The file browser that Add samples opens: folders and sound files, Enter, Backspace, typing, picking several,
Preview while browsing, and the folder and preview box remembered."""
import ctypes
import os

from test_plugin import P, got, gui, tone  # noqa: F401 (P: a fixture)


def music(tmp_path):
    """Music: two folders, sounds that sort as Explorer sorts them, and files that aren't listed."""
    m = tmp_path / "Music"
    (m / "Voices").mkdir(parents=True)
    (m / "Kits").mkdir()
    tone(m, "boom.wav", 220)
    tone(m, "10 b.wav", 330)
    tone(m, "2 b.wav", 440)
    tone(m, "LOUD.WAV", 550)
    tone(m / "Voices", "yell.wav", 660)
    (m / "notes.txt").write_text("x")
    hidden = tone(m, "hidden.wav", 110)
    ctypes.windll.kernel32.SetFileAttributesW(str(hidden), 2)        # FILE_ATTRIBUTE_HIDDEN
    return m


def opened(m):
    return ["focus Add samples", "key enter", "focus Folder", "type %s" % m, "key enter"]


def test_browser_lists_folders_then_sounds(P, tmp_path):
    m = music(tmp_path)
    out = gui(P, tmp_path, "keep_length off\n", opened(m) + ["say", "list Music"])
    assert got(out, "focus")[-1][:3] == ["ListBox", "Music", "Up one folder"]   # named for the folder
    assert got(out, "list")[0] == ["Music", "Up one folder", "Kits, folder", "Voices, folder", "2 b.wav", "10 b.wav",
                                   "boom.wav", "LOUD.WAV"]                   # no notes.txt, no hidden file


def test_browser_enter_and_backspace(P, tmp_path):
    m = music(tmp_path)
    out = gui(P, tmp_path, "keep_length off\n", opened(m) + ["key down", "key down", "key enter", "key backspace",
                                                             "key enter", "key down", "key enter", "list Samples",
                                                             "state sample"])
    f = got(out, "focus")
    assert f[-5][:3] == ["ListBox", "Voices", "Up one folder"]              # in: on the way up
    assert f[-4][:3] == ["ListBox", "Music", "Voices, folder"]              # back up: on the folder it left
    assert f[-2][:3] == ["ListBox", "Voices", "yell.wav"]
    assert f[-1][:2] == ["ListBox", "Samples"]                                # a sound added: on the Samples list
    assert [x.split(",")[0] for x in got(out, "list")[0][1:]] == ["yell.wav"]
    assert got(out, "state")[0][0].startswith('sample "%s"' % (m / "Voices" / "yell.wav"))


def test_browser_up_from_a_drive_lists_the_drives(P, tmp_path):
    root = os.path.splitdrive(str(tmp_path))[0] + "\\"
    out = gui(P, tmp_path, "keep_length off\n", opened(root) + ["key backspace", "list Drives", "key backspace",
                                                                "counts"])
    f = got(out, "focus")
    assert f[-3][1] == root[:2] and f[-3][2] == "Up one folder"
    assert f[-2][1] == "Drives" and f[-2][2].startswith(root[:2] + " ")      # on the drive it left
    assert f[-1] == f[-2]
    assert all(x.endswith(", drive") for x in got(out, "list")[0][1:])
    assert got(out, "counts")[0][0] == "beeps 1"                            # nothing above the drives


def test_browser_picks_several(P, tmp_path):
    m = music(tmp_path)
    out = gui(P, tmp_path, "keep_length off\n", opened(m) + ["key end", "key up shift", "key up shift", "key enter",
                                                             "list Samples"] +
              opened(m) + ["key a ctrl", "key enter", "list Samples"])
    ls = got(out, "list")
    assert [x.split(",")[0] for x in ls[0][1:]] == ["10 b.wav", "boom.wav", "LOUD.WAV"]
    assert [x.split(",")[0] for x in ls[1][1:]] == ["10 b.wav", "boom.wav", "LOUD.WAV", "2 b.wav", "10 b.wav",
                                                    "boom.wav", "LOUD.WAV"]   # Ctrl+A: every sound, no folders


def test_browser_typing_jumps(P, tmp_path):
    m = music(tmp_path)
    out = gui(P, tmp_path, "keep_length off\n", opened(m) + ["char b", "char o", "pump 1100", "char 1", "pump 1100",
                                                             "char v", "pump 1100", "char k", "char z", "pump 1100",
                                                             "char l", "char l", "counts"])
    v = [x[2] for x in got(out, "focus")[-8:]]
    assert v == ["boom.wav", "boom.wav", "10 b.wav", "Voices, folder", "Kits, folder", "Kits, folder",
                 "LOUD.WAV", "LOUD.WAV"]                                    # "bo"; "kz" beeps; "ll": no other L
    assert got(out, "counts")[0][0] == "beeps 1"


def test_browser_folder_box_and_escape(P, tmp_path):
    m = music(tmp_path)
    out = gui(P, tmp_path, "keep_length off\n", opened(m / "Voices" / "yell.wav") + [
        "key escape", "list Samples", "focus Add samples", "key enter",
        "focus Folder", "type %s" % (tmp_path / "nowhere"), "key enter", "counts"])
    f = got(out, "focus")
    assert f[0][1] == "Drives"                                              # nothing remembered yet: the drives
    assert f[1][:3] == ["ListBox", "Voices", "yell.wav"]                    # a file's path: its folder, on it
    assert f[2][:2] == ["Button", "Add samples"]                            # Escape: back, nothing added
    assert got(out, "list")[0][1:] == ["No samples loaded"]
    assert f[3][:3] == ["ListBox", "Voices", "Up one folder"]               # opens where it was last
    assert got(out, "counts")[0][0] == "beeps 1"                            # a folder that isn't there


def test_browser_tab_order(P, tmp_path):
    m = music(tmp_path)
    out = gui(P, tmp_path, "keep_length off\n", opened(m) + ["key tab", "key tab", "key tab", "focus Cancel",
                                                             "key tab shift", "key tab shift", "key tab shift"])
    f = [x[:2] for x in got(out, "focus")]
    assert f[-6:] == [["Button", "Preview while browsing"], ["Button", "Cancel"], ["Button", "after"],   # out
                      ["Button", "Preview while browsing"], ["ListBox", "Music"], ["Edit", "Folder"]]


def test_browser_preview(P, tmp_path, settings_file):
    m = music(tmp_path)
    out = gui(P, tmp_path, "keep_length off\n", opened(m) + [
        "key end", "blocks 40",                                   # preview off (the default): silence
        "key tab", "key space", "key tab shift", "key up", "blocks 40",   # on: the sound under the cursor plays
        "key home", "blocks 40",                                  # on the way up: it stops
        "key end", "blocks 5", "key escape", "blocks 40"])        # leaving the browser stops it
    peaks = [float(x[0]) for x in got(out, "blocks")]
    assert peaks[0] == 0 and peaks[1] > 0.3 and peaks[2] == 0 and peaks[-1] == 0, peaks
    lines = open(settings_file, encoding="utf-8").read().splitlines()
    assert "browse_preview on" in lines and ("browse_folder %s" % m) in lines   # both remembered
