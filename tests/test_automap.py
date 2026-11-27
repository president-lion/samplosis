"""Adding samples named for their notes and dynamics (Ab3P.wav, Ab3MF.wav) maps them: each note's samples cover the
keys halfway to the next note, and a note's loudness layers split the velocity range, softest lowest."""
from test_plugin import P, browse_add, got, gui, tone  # noqa: F401 (P: a fixture)


def kit(d):
    d.mkdir()
    for n, hz in (("Ab3P", 208), ("Ab3MF", 208), ("C4P", 262), ("C4MF", 262), ("E4", 330)):
        tone(d, n + ".wav", hz)
    return d


def rows(out, k=0):
    return [x.split(", ", 1)[1] for x in got(out, "list")[k][1:]]


def test_adding_named_samples_maps_them(P, tmp_path):
    out = gui(P, tmp_path, "keep_length off\n", browse_add(kit(tmp_path / "kit")) + ["list Samples", "status"])
    assert [x.split(",")[0] for x in got(out, "list")[0][1:]] == ["Ab3MF.wav", "Ab3P.wav", "C4MF.wav", "C4P.wav",
                                                                  "E4.wav"]
    assert rows(out) == ["root G#3, keys C-1 to A#3, velocity 64 to 127",
                         "root G#3, keys C-1 to A#3, velocity 1 to 63",
                         "root C4, keys B3 to D4, velocity 64 to 127",
                         "root C4, keys B3 to D4, velocity 1 to 63",
                         "root E4, keys D#4 to G9, velocity 1 to 127"]
    st = [ln for ln in out.splitlines() if ln.startswith("status: ")][-1]
    assert st == "status: Ready: 5 samples, mapped to 3 notes in up to 2 velocity layers", st


def test_an_unnamed_sample_leaves_the_map(P, tmp_path):
    k = kit(tmp_path / "kit")
    (tmp_path / "more").mkdir()
    tone(tmp_path / "more", "Kick.wav", 60)
    out = gui(P, tmp_path, "keep_length off\n", browse_add(k) + ["list Samples"] + browse_add(tmp_path / "more") +
              ["list Samples", "status"])
    assert rows(out, 1)[:5] == rows(out, 0)
    assert rows(out, 1)[5].endswith("keys C-1 to G9, velocity 1 to 127")
    st = [ln for ln in out.splitlines() if ln.startswith("status: ")][-1]
    assert st == "status: Ready: 6 samples", st


def test_adding_more_notes_remaps(P, tmp_path):
    """A folder added in pieces: the second add's notes and the first's make one map."""
    a, b = tmp_path / "a", tmp_path / "b"
    a.mkdir(); b.mkdir()
    tone(a, "C4.wav", 262)
    tone(b, "C5.wav", 523)
    out = gui(P, tmp_path, "keep_length off\n", browse_add(a) + browse_add(b) + ["list Samples", "status"])
    assert rows(out) == ["root C4, keys C-1 to F#4, velocity 1 to 127", "root C5, keys G4 to G9, velocity 1 to 127"]
    st = [ln for ln in out.splitlines() if ln.startswith("status: ")][-1]
    assert st == "status: Ready: 2 samples, mapped to 2 notes", st
