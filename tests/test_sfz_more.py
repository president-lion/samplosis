"""SFZ, 2026-11-02: set_ccN, controller modulations (gain_ccN, ampeg_*ccN, pitchlfo_*ccN), one-pole filters,
the per-note randoms (delay, delay_random, amp_random, pitch_random) and amp_keytrack; and the loading work that
came with them: prefill in a background open, and a waiting note's sample decoded ahead of the rest."""
import ctypes
import math
import time

import numpy as np
import soundfile as sf

from conftest import SmpSfView
from test_sf2 import load_all, mods, peak_hz, regions
from test_sfz import OUT, RATE, Player, db, ignored, note, on, opened, rms, sine, write_sfz, write_wav


def test_sfz_set_cc_and_gain_cc(lib, tmp_path):
    """<control>'s set_ccN is the controller's value from the start: a region gated on CC24 (locc24=2 hicc24=15)
    plays with set_cc24=13 and not once CC24 is sent as 0; gain_ccN (dB at 127) is a CC-to-attenuation modulator,
    so volume=-20 gain_cc25=20 plays at about 0 dB with CC25 at 127 and at -20 dB with CC25 at 0. Reset All
    Controllers goes back to set_cc's values."""
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0))
    path = write_sfz(tmp_path / "cc.sfz", """
<control> set_cc24=13 set_cc25=127 label_cc24=Shape(CC24)
<region> sample=a.wav key=60 pitch_keycenter=60 locc24=2 hicc24=15 volume=-20 gain_cc25=20 amp_veltrack=0
""")
    f = opened(lib, path)
    try:
        assert ignored(lib, f)[0] == 0
        r = regions(lib, f, 0)
        assert (0x80 | 25, 48, -200, 0, 0) in mods(r[0].imod, r[0].nimod)
    finally:
        lib.smp_sf_close(f)
    p = Player(lib, tmp_path, path)
    try:
        full = p.render([on(60, 100, 0)], 0.3)
        gated = p.render(["midi 176 24 0 0", on(60, 100, 0.01)], 0.3)
        quiet = p.render(["midi 176 25 0 0", on(60, 100, 0.01)], 0.3)
        reset = p.render(["midi 176 24 0 0", "midi 176 121 0 0", on(60, 100, 0.01)], 0.3)
    finally:
        p.free()
    lvl = rms(full[4800:])
    assert lvl > 0.1
    assert rms(gated[4800:]) < 1e-6
    assert abs(db(rms(quiet[4800:]), lvl) + 20) < 1.0
    assert abs(db(rms(reset[4800:]), lvl)) < 0.5


def test_sfz_ampeg_and_vibrato_cc(lib, tmp_path):
    """ampeg_decayccN and ampeg_sustainccN become modulators exact at the controller's ends (timecents and cB);
    pitchlfo_depthccN is a CC-to-vibrato modulator and pitchlfo_freqccN sets the vibrato's rate at set_cc's value."""
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0))
    path = write_sfz(tmp_path / "e.sfz", """
<control> set_cc21=127
<region> sample=a.wav key=60 ampeg_decay=1 ampeg_decaycc22=30 ampeg_sustain=1 ampeg_sustaincc22=30
 pitchlfo_depthcc20=100 pitchlfo_freqcc21=6
""")
    f = opened(lib, path)
    try:
        assert ignored(lib, f)[0] == 0
        r = regions(lib, f, 0)
        m = mods(r[0].imod, r[0].nimod)
        assert (0x80 | 22, 36, round(1200 * math.log2(31)), 0, 0) in m
        assert (0x80 | 22, 37, round(-200 * math.log10(0.31) + 200 * math.log10(0.01)), 0, 0) in m
        assert (0x80 | 20, 6, 100, 0, 0) in m
        assert abs(r[0].gen[24] - round(1200 * math.log2(6 / 8.176))) <= 1
    finally:
        lib.smp_sf_close(f)


def test_sfz_one_pole_filters(lib, tmp_path):
    """lpf_1p and hpf_1p are one pole: a 1 kHz lowpass leaves a 4 kHz tone about 12.3 dB down (two poles would be
    about 24); the highpass leaves it nearly whole and 250 Hz about 12.3 dB down. A cutoff of 10 Hz with
    fil_veltrack=20000 opens fully at velocity 127, as sfizz reads it (not from SF2's 20 Hz floor)."""
    for fq in (250, 4000):
        write_wav(tmp_path / ("t%d.wav" % fq), sine(RATE, float(fq)))

    def level(sample, extra, vel=127):
        path = write_sfz(tmp_path / "f.sfz", "<region> sample=%s key=60 pitch_keycenter=60 %s\n" % (sample, extra))
        f = opened(lib, path)
        try:
            assert ignored(lib, f)[0] == 0
            y, _ = note(lib, f, 60, vel=vel, n=12000)
            return rms(y[4000:, 0])
        finally:
            lib.smp_sf_close(f)

    hi = level("t4000.wav", "")
    lo = level("t250.wav", "")
    assert abs(db(level("t4000.wav", "cutoff=1000 fil_type=lpf_1p"), hi) + 12.3) < 1.5
    assert abs(db(level("t4000.wav", "cutoff=1000 fil_type=hpf_1p"), hi)) < 1.0
    assert abs(db(level("t250.wav", "cutoff=1000 fil_type=hpf_1p"), lo) + 12.3) < 1.5
    assert abs(db(level("t4000.wav", "cutoff=10 fil_type=lpf_1p fil_veltrack=20000"), hi)) < 1.0


def onset(y, thr=1e-3):
    k = np.nonzero(np.abs(y[:, 0]) > thr)[0]
    return int(k[0]) if len(k) else -1


def test_sfz_randoms_delay_and_keytrack(lib, tmp_path):
    """delay starts the voice late; delay_random adds 0..x seconds, amp_random 0..x dB and pitch_random -x..+x
    cents, a new draw each note; amp_keytrack is dB per key from amp_keycenter."""
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0))

    def notes(extra, n=8, key=60, frames=12000):
        path = write_sfz(tmp_path / "r.sfz",
                         "<region> sample=a.wav lokey=48 hikey=72 pitch_keycenter=60 amp_veltrack=0 %s\n" % extra)
        f = opened(lib, path)
        try:
            assert ignored(lib, f)[0] == 0
            return [note(lib, f, key, n=frames)[0] for _ in range(n)]
        finally:
            lib.smp_sf_close(f)

    plain = notes("", 1)[0]
    o0 = onset(plain)                                                  # the attack's first millisecond
    assert 0 <= o0 < 60
    assert abs(onset(notes("delay=0.1", 1)[0]) - o0 - int(0.1 * OUT)) < 4
    starts = [onset(y) - o0 for y in notes("delay_random=0.05")]
    assert all(-4 <= s <= int(0.05 * OUT) + 4 for s in starts) and len(set(starts)) > 3, starts
    lv = [db(rms(y[2000:, 0]), rms(plain[2000:, 0])) for y in notes("amp_random=-6")]
    assert all(-6.2 < x < 0.2 for x in lv) and max(lv) - min(lv) > 1, lv
    hz = [peak_hz(y[1000:, 0], OUT) for y in notes("pitch_random=50", frames=24000)]
    assert all(440 * 2 ** (-55 / 1200) < h < 440 * 2 ** (55 / 1200) for h in hz) and max(hz) - min(hz) > 3, hz
    up = notes("amp_keytrack=1.5 amp_keycenter=60", 1, key=64)[0]
    base = notes("amp_keytrack=1.5 amp_keycenter=60", 1, key=60)[0]
    assert abs(db(rms(up[2000:, 0]), rms(base[2000:, 0])) - 6) < 0.5


def flac(path, x, rate=RATE):
    sf.write(str(path), x, rate, format="FLAC", subtype="PCM_16")
    return path


def test_sfz_prefill_in_background_open(lib, tmp_path):
    """The window's background open (smp_sf_parse_detached_ex with a cancel) counts the sample files it reads and
    decodes compressed ones into the cache as it reads them, so the loader decodes nothing afterwards and a stereo
    FLAC's channels are known for sure; a plain open (smp_sf_open) leaves the decodes to the loader."""
    def make(sub):
        d = tmp_path / sub
        d.mkdir()
        lines = []
        for i in range(12):
            x = sine(RATE // 2, 200.0 + 50 * i)
            if i == 0:
                x = np.stack([x, sine(RATE // 2, 900.0)], axis=1)          # stereo: left 200 Hz, right 900 Hz
            flac(d / ("s%02d.flac" % i), x)
            lines.append("<region> sample=s%02d.flac key=%d pitch_keycenter=%d" % (i, 60 + i, 60 + i))
        return write_sfz(d / "p.sfz", "\n".join(lines) + "\n")

    CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
    never = CB(lambda a: 0)
    prog = (ctypes.c_long * 2)()
    err = ctypes.create_string_buffer(512)
    path = make("bg")
    f = lib.smp_sf_parse_detached_ex(path.encode(), ctypes.cast(never, ctypes.c_void_p), None,
                                     ctypes.addressof(prog), err, 512)
    assert f, err.value
    assert (prog[0], prog[1]) == (12, 12)
    f = lib.smp_sf_adopt(f, err, 512)
    assert f, err.value
    try:
        assert lib.smp_sf_sample_count(f) == 13                            # the stereo file is two samples
        d0 = lib.smp_sf_test_decodes()
        load_all(lib, f)
        assert lib.smp_sf_test_decodes() == d0                             # all from the cache: prefilled
        y, _ = note(lib, f, 60, n=12000)
        assert abs(peak_hz(y[2000:, 0], OUT) - 200) < 3 and abs(peak_hz(y[2000:, 1], OUT) - 900) < 3
        total = ctypes.c_int()
        lib.smp_sf_use(f, 0, 1)
        lib.smp_sf_wait_idle(f, 10000)
        assert lib.smp_sf_load_progress(f, ctypes.byref(total)) == total.value == 13
        lib.smp_sf_use(f, 0, -1)
    finally:
        lib.smp_sf_close(f)
    plain = opened(lib, make("plain"))
    try:
        d0 = lib.smp_sf_test_decodes()
        load_all(lib, plain)
        assert lib.smp_sf_test_decodes() - d0 == 12                       # one decode a file (both channels at once)
    finally:
        lib.smp_sf_close(plain)


def test_sfz_hinted_sample_jumps_the_queue(lib, tmp_path):
    """A sample a waiting note wants (smp_sf_hint) is decoded ahead of the rest of a big instrument: it is ready
    while most of the others are still waiting."""
    n = 400
    lines = []
    for i in range(n):
        flac(tmp_path / ("h%03d.flac" % i), sine(RATE // 2, 300.0 + i))
        lines.append("<region> sample=h%03d.flac key=60 lorand=%.6f hirand=%.6f" % (i, i / n, (i + 1) / n))
    f = opened(lib, write_sfz(tmp_path / "big.sfz", "\n".join(lines) + "\n"))
    view = SmpSfView()
    try:
        last = lib.smp_sf_sample_count(f) - 1
        lib.smp_sf_test_pause(1)
        lib.smp_sf_use(f, 0, 1)
        lib.smp_sf_hint(f, last)
        lib.smp_sf_test_pause(0)
        t0 = time.time()
        while not lib.smp_sf_pcm_acquire(f, last, ctypes.byref(view)):
            assert time.time() - t0 < 30
            time.sleep(0.001)
        ready = lib.smp_sf_loaded_samples(f)
        lib.smp_sf_pcm_release(f, last)
        assert ready < n // 4, ready
        lib.smp_sf_wait_idle(f, 60000)
        lib.smp_sf_use(f, 0, -1)
    finally:
        lib.smp_sf_test_pause(0)
        lib.smp_sf_close(f)


def test_sfz_directives_anywhere_and_root_relative_includes(lib, tmp_path):
    """As sfizz reads them (DrumGizmo's and Salamander's SFZ conversions are laid out this way): #include and
    #define anywhere on a line and more than one to a line ("<group> #include "x.txt" lovel=1", "#define $a 1
    #define $b 2"), each part expanded with the defines made before it; an include's path is from the root .sfz's
    folder, and from the including file's when it isn't there."""
    kit = tmp_path / "kit"
    for d in ("Stereo", "Data/multi", "Data/inc", "Samples"):
        (kit / d).mkdir(parents=True)
    write_wav(kit / "Samples" / "a.wav", sine(RATE // 2, 440.0))
    (kit / "Data" / "macro.txt").write_text("#define $lo 1        #define $hi 64\n#define $k 62\n")
    (kit / "Data" / "region.txt").write_text("<region> sample=../Samples/a.wav key=$k\n")
    # from the root's folder (Stereo/..): Data/multi/../Data/region.txt wouldn't exist
    (kit / "Data" / "multi" / "group.txt").write_text('<group> #include "../Data/region.txt" lovel=$lo hivel=$hi\n')
    # from the including file's folder: Stereo/near.txt doesn't exist, Data/inc/near.txt does
    (kit / "Data" / "inc" / "far.txt").write_text('#include "near.txt"\n')
    (kit / "Data" / "inc" / "near.txt").write_text("<region> sample=../Samples/a.wav key=70\n")
    path = write_sfz(kit / "Stereo" / "kit.sfz",
                     '#include "../Data/macro.txt" #include "../Data/multi/group.txt"\n'
                     '<group> #include "../Data/inc/far.txt"\n')
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert [(x.lokey, x.hikey, x.lovel, x.hivel) for x in r] == [(62, 62, 1, 64), (70, 70, 0, 127)]
        assert lib.smp_sf_problem(f) == b""
    finally:
        lib.smp_sf_close(f)


def test_sfz_set_hdcc(lib, tmp_path):
    """set_hdccN is set_ccN as a fraction (0 to 1, of 127): 0.5 is 64, so a region gated on 60..70 plays; 0.2 is 25,
    and it doesn't."""
    write_wav(tmp_path / "a.wav", sine(RATE, 440.0))
    lvl = []
    for v in ("0.5", "0.2"):
        path = write_sfz(tmp_path / ("h%s.sfz" % v), """
<control> set_hdcc24=%s
<region> sample=a.wav key=60 pitch_keycenter=60 locc24=60 hicc24=70 amp_veltrack=0
""" % v)
        f = opened(lib, path)
        try:
            assert ignored(lib, f)[0] == 0
        finally:
            lib.smp_sf_close(f)
        p = Player(lib, tmp_path, path)
        try:
            lvl.append(rms(p.render([on(60, 100, 0)], 0.3)[4800:]))
        finally:
            p.free()
    assert lvl[0] > 0.1 and lvl[1] < 1e-6, lvl
