"""SFZ (Task 9): an .sfz is one preset whose regions come from its opcodes. Every test writes a small .sfz and its
samples (16-bit or 24-bit WAV, some with a smpl loop, and FLAC) into tmp_path, opens it through smp_sf_open, and
checks the regions' generators and conditions, and how notes sound (smp_sf_test_note, or the player for the region
conditions)."""
import ctypes
import math
import os
import random
import struct

import numpy as np
import soundfile as sf

from test_sf2 import CLAMPS, load_all, mods, peak_hz, presets, regions, sf_open
from test_sffont import pcm

RATE = 44100
OUT = 48000


# ---- fixtures written here ----

def sine(n, freq, rate=RATE, amp=0.5):
    return amp * np.sin(2 * np.pi * freq * np.arange(n) / rate)


def write_wav(path, x, rate=RATE, bits=16, loop=None, unity=None):
    """A PCM WAV (mono, or frames x 2), optionally with a smpl chunk: loop = (first, last) frames (last inclusive)."""
    x = np.asarray(x, dtype=np.float64)
    ch = 1 if x.ndim == 1 else x.shape[1]
    if bits == 16:
        data = np.clip(np.round(x * 32767), -32768, 32767).astype("<i2").tobytes()
    else:
        v = np.clip(np.round(x * 8388607), -8388608, 8388607).astype("<i4").reshape(-1)
        data = b"".join(int(s).to_bytes(4, "little", signed=True)[:3] for s in v)
    block = ch * bits // 8
    fmt = struct.pack("<HHIIHH", 1, ch, rate, rate * block, block, bits)
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    if loop or unity is not None:
        smpl = struct.pack("<9I", 0, 0, 0, 60 if unity is None else unity, 0, 0, 0, 1 if loop else 0, 0)
        if loop:
            smpl += struct.pack("<6I", 0, 0, loop[0], loop[1], 0, 0)
        chunks += b"smpl" + struct.pack("<I", len(smpl)) + smpl
    chunks += b"data" + struct.pack("<I", len(data)) + data + (b"\0" if len(data) & 1 else b"")
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)
    return path


def write_sfz(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return str(path)


def opened(lib, path):
    f, err = sf_open(lib, path)
    assert f, err
    return f


def note(lib, f, key, vel=100, n=24000, release_at=-1):
    """the preset's note as stereo frames x 2 at OUT Hz"""
    out = (ctypes.c_float * (2 * n))()
    nv = lib.smp_sf_test_note(f, 0, key, vel, float(OUT), n, release_at, out)
    assert nv >= 0
    return np.frombuffer(out, dtype=np.float32).reshape(n, 2).copy(), nv


def rms(x):
    return float(np.sqrt(np.mean(np.square(x)) + 1e-30))


def db(a, b):
    return 20 * math.log10(a / b)


def ignored(lib, f):
    first = ctypes.create_string_buffer(128)
    n = lib.smp_sf_ignored(f, first, 128)
    return n, first.value.decode("utf-8", "replace")


def walk(lib, f):
    """everything the API offers about the font, every index in range (test_sf2.walk_font without its SF2-only
    modulator count)"""
    ns = lib.smp_sf_sample_count(f)
    for i in range(lib.smp_sf_preset_count(f)):
        assert lib.smp_sf_preset_info(f, i, None, None, None, 0)
        for r in regions(lib, f, i):
            assert r.lokey <= r.hikey <= 127 and r.lovel <= r.hivel <= 127
            assert 0 <= r.sample < ns
            assert len(mods(r.imod, r.nimod)) == r.nimod
            for g, (lo, hi) in CLAMPS.items():
                if g == 56:
                    lo = -1200                                               # SFZ's pitch_keytrack may be negative
                assert lo <= r.gen[g] <= hi, (g, r.gen[g])


def cents(r):
    return 100 * r.gen[51] + r.gen[52]


# ---- the tests ----

def test_sfz_basic_mapping(lib, tmp_path):
    """Keys (numbers and note names, c4 = 60), velocity, pitch_keycenter, tune and transpose, volume, amplitude
    and pan become SF2's ranges and generators; notes sound at the right pitch and level."""
    write_wav(tmp_path / "a.wav", sine(RATE // 2, 440.0))
    write_wav(tmp_path / "b24.wav", sine(RATE // 2, 440.0), bits=24)
    sf.write(str(tmp_path / "c.aif"), sine(RATE // 2, 440.0), RATE, format="AIFF", subtype="PCM_16")
    path = write_sfz(tmp_path / "Basic Piano.sfz", """
<region> sample=a.wav lokey=c4 hikey=e4 pitch_keycenter=d4 tune=-30 transpose=1 volume=-6 pan=-50 lovel=10 hivel=100
<region> sample=a.wav key=72 volume=3
<region> sample=b24.wav key=74 volume=-3
<region> sample=a.wav lokey=C#3 hikey=eb3 pitch_keycenter=76 amplitude=50
<region> sample=c.aif key=40
<region> sample=a.wav key=41
""")
    f = opened(lib, path)
    try:
        assert presets(lib, f) == [(0, 0, "Basic Piano")]
        r = regions(lib, f, 0)
        assert len(r) == 6
        assert (r[0].lokey, r[0].hikey, r[0].lovel, r[0].hivel) == (60, 64, 10, 100)
        assert r[0].gen[58] == 62 and cents(r[0]) == 70
        assert r[0].gen[48] == 150 and r[0].gen[17] == -250          # -6 dB x -25; pan x 5
        assert abs(r[0].cond.amp_veltrack - 100) < 1e-6 and abs(r[0].cond.gain - 1) < 1e-6
        assert (r[1].lokey, r[1].hikey, r[1].gen[58]) == (72, 72, 72)
        assert r[1].gen[48] == 0 and abs(r[1].cond.gain - 10 ** (3 / 20)) < 1e-4
        assert (r[3].lokey, r[3].hikey) == (49, 51) and r[3].gen[48] == 151   # 50 % is -6.02 dB
        walk(lib, f)
        x, nv = note(lib, f, 62)
        assert nv == 1
        assert abs(peak_hz(x[2000:20000, 0], OUT) - 440 * 2 ** (70 / 1200)) < 2
        assert rms(x[2000:20000, 0]) > 2 * rms(x[2000:20000, 1])     # panned left
        hi, _ = note(lib, f, 72)
        lo, _ = note(lib, f, 74)
        assert abs(peak_hz(hi[2000:20000, 0], OUT) - 440) < 2 and abs(peak_hz(lo[2000:20000, 0], OUT) - 440) < 2
        assert abs(db(rms(hi[2000:20000]), rms(lo[2000:20000])) - 6) < 0.3   # +3 dB against -3 dB (16 and 24 bit)
        aif, _ = note(lib, f, 40)
        wav, _ = note(lib, f, 41)
        assert abs(peak_hz(aif[2000:20000, 0], OUT) - 440) < 2
        assert abs(db(rms(aif[2000:20000]), rms(wav[2000:20000]))) < 0.05      # big-endian AIFF streams the same
    finally:
        lib.smp_sf_close(f)


def test_sfz_inheritance_and_define_include(lib, tmp_path):
    """global, master, group and region inherit in order; #define replaces text; #include reads a file relative to
    the one including it (a cycle back to the root is ignored); comments; control's default_path and
    octave_offset."""
    os.makedirs(tmp_path / "samples")
    os.makedirs(tmp_path / "inc")
    write_wav(tmp_path / "samples" / "a.wav", sine(RATE // 2, 440.0))
    write_sfz(tmp_path / "inc" / "more.sfz", '<region> sample=a.wav lokey=65 hikey=66 volume=0\n#include "../main.sfz"\n')
    path = write_sfz(tmp_path / "main.sfz", """// an instrument
#define $VOL -12
#define $KEY 64
#define $K2 $KEY
<control> default_path=samples/
<global> volume=$VOL ampeg_release=0.5
<master> lovel=1 hivel=64
<group> pitch_keycenter=60 tune=10
<region> sample=a.wav key=$K2
#include "inc/more.sfz"
<group> tune=20   // a comment <region> sample=bogus.wav
/* a block
   comment <region> sample=bogus.wav */
<region> sample=a.wav lokey=70 hikey=71
<master> hivel=127
<region> sample=a.wav key=80
<control> octave_offset=1
<region> sample=a.wav key=60
""")
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert [(x.lokey, x.hikey) for x in r] == [(64, 64), (65, 66), (70, 71), (80, 80), (72, 72)]
        assert [(x.lovel, x.hivel) for x in r] == [(1, 64), (1, 64), (1, 64), (0, 127), (0, 127)]
        assert [x.gen[48] for x in r] == [300, 0, 300, 300, 300]
        assert [cents(x) for x in r] == [10, 10, 20, 0, 0]
        assert [x.gen[58] for x in r] == [64, 60, 60, 80, 72]     # key= sets pitch_keycenter too
        assert all(x.gen[38] == -1200 for x in r)                      # 0.5 s release in timecents
        assert lib.smp_sf_problem(f) == b""
        assert ignored(lib, f)[0] == 0
    finally:
        lib.smp_sf_close(f)


def test_sfz_directives_mid_line(lib, tmp_path):
    """#define and #include may sit anywhere in a line, as sforzando and sfizz read them (the Headroom Piano and
    Maestro Concert Grand write '<region> #define $KEY 60 lokey=59 #include "x.txt"'): a define's value ends where an
    opcode's would (the next name=, header or directive), so it can still hold spaces, and what follows on the line
    sees it; a # that starts no directive stays in a value."""
    os.makedirs(tmp_path / "Data")
    write_wav(tmp_path / "s60.wav", sine(RATE // 2, 261.63))
    write_wav(tmp_path / "s64.wav", sine(RATE // 2, 329.63))
    write_wav(tmp_path / "two words.wav", sine(RATE // 2, 440.0))
    write_wav(tmp_path / "a #1.wav", sine(RATE // 2, 440.0))
    write_sfz(tmp_path / "Data" / "smp.txt", "sample=s$KEY.$EXT\npitch_keycenter=$KEY\n")
    path = write_sfz(tmp_path / "mid.sfz", """#define $EXT wav
<group> #define $LO 1 lovel=$LO hivel=127
<region> #define $KEY 60 lokey=59 hikey=61 #include "Data/smp.txt"
<region> #define $KEY 64 lokey=62 hikey=66 #include "Data/smp.txt" volume=-6
#define $FILE two words.wav
<region> sample=$FILE key=70
<region> sample=a #1.wav key=72
""")
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert [(x.lokey, x.hikey) for x in r] == [(59, 61), (62, 66), (70, 70), (72, 72)]
        assert [x.gen[58] for x in r] == [60, 64, 70, 72]
        assert all((x.lovel, x.hivel) == (1, 127) for x in r)
        assert r[1].gen[48] == 150                                      # the opcode after the #include still counts
        assert len({x.sample for x in r}) == 4
        lo, _ = note(lib, f, 60)
        hi, _ = note(lib, f, 64)
        assert abs(peak_hz(lo[2000:20000, 0], OUT) - 261.63) < 2
        assert abs(peak_hz(hi[2000:20000, 0], OUT) - 329.63) < 2
        assert lib.smp_sf_problem(f) == b""
    finally:
        lib.smp_sf_close(f)


def test_sfz_include_relative_to_root(lib, tmp_path):
    """An #include in an included file is looked for from the root .sfz's folder first, as sforzando and sfizz do
    (Data/group.txt including "Data/region.txt"), then from the including file's own folder."""
    os.makedirs(tmp_path / "Data")
    write_wav(tmp_path / "a.wav", sine(RATE // 2, 440.0))
    write_sfz(tmp_path / "Data" / "region.txt", "<region> key=60 sample=a.wav\n")
    write_sfz(tmp_path / "Data" / "local.txt", "<region> key=62 sample=a.wav\n")
    write_sfz(tmp_path / "Data" / "group.txt", '<group> lovel=1\n#include "Data/region.txt"\n#include "local.txt"\n')
    path = write_sfz(tmp_path / "root.sfz", '#include "Data/group.txt"\n')
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert [(x.lokey, x.lovel) for x in r] == [(60, 1), (62, 1)]
    finally:
        lib.smp_sf_close(f)


def envelope(x, win=480):
    a = np.abs(x)
    return np.array([a[i:i + win].max() for i in range(0, len(a) - win, win)])


def test_sfz_envelope_and_filter(lib, tmp_path):
    """ampeg_* is the volume envelope, fileg_* the modulation envelope to the cutoff; fil_type, cutoff, resonance
    and fil_veltrack; highpass and bandpass (the voice's RBJ extension); a filter kind the voice lacks is counted
    and plays unfiltered."""
    write_wav(tmp_path / "s100.wav", sine(RATE * 2, 100.0))
    write_wav(tmp_path / "s5k.wav", sine(RATE * 2, 5000.0))
    path = write_sfz(tmp_path / "e.sfz", """
<region> sample=s100.wav key=60 ampeg_delay=0.01 ampeg_attack=0.1 ampeg_hold=0.05 ampeg_decay=0.2 ampeg_sustain=50 ampeg_release=0.3
<region> sample=s5k.wav key=62 pitch_keycenter=62 fil_type=lpf_2p cutoff=1000 resonance=6 fil_veltrack=1200 fileg_attack=0.2 fileg_sustain=50 fileg_depth=1200
<region> sample=s100.wav key=64 pitch_keycenter=64 fil_type=hpf_2p cutoff=2000
<region> sample=s100.wav key=65 pitch_keycenter=65
<region> sample=s5k.wav key=66 pitch_keycenter=66 fil_type=bpf_2p cutoff=500
<region> sample=s5k.wav key=67 pitch_keycenter=67 fil_type=pkf_2p cutoff=500
""")
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        e = r[0].gen
        assert (e[33], e[34], e[35], e[36], e[37], e[38]) == (-7973, -3986, -5186, -2786, 60, -2084)
        g = r[1].gen
        assert g[8] == round(1200 * math.log2(1000 / 8.176)) and g[9] == 60 and r[1].cond.fil_type == 0
        assert g[26] == round(1200 * math.log2(0.2)) and g[29] == 500 and g[11] == 1200
        assert (0x0002, 8, 1200, 0, 0) in mods(r[1].imod, r[1].nimod)
        assert all(m[1] != 8 or m[0] == 0x0002 for m in mods(r[0].imod, r[0].nimod))   # no velocity-to-cutoff default
        assert r[2].cond.fil_type == 1 and r[4].cond.fil_type == 2
        assert r[5].gen[8] == 13500 and r[5].cond.fil_type == 0                # unsupported: no filter
        n, first = ignored(lib, f)
        assert n == 1 and first == "fil_type=pkf_2p"
        # the envelope: silent in its delay, rising, then at the 50 % sustain (-6 dB) after the decay
        x, _ = note(lib, f, 60, n=int(0.8 * OUT))
        env = envelope(x[:, 0])
        peak = env[int(0.12 * 100):int(0.15 * 100)].mean()          # in the hold
        sus = env[int(0.6 * 100):int(0.7 * 100)].mean()
        assert env[0] < 0.01 * peak
        assert abs(db(sus, peak) + 6.0) < 1.0
        # the highpass removes the 100 Hz sine; the same region without the filter keeps it
        hp, _ = note(lib, f, 64)
        plain, _ = note(lib, f, 65)
        assert db(rms(hp[4000:]), rms(plain[4000:])) < -40
        # the lowpass (1 kHz, opened by velocity and its envelope) and the bandpass (500 Hz) cut the 5 kHz sine
        bp, _ = note(lib, f, 66)
        un, _ = note(lib, f, 67)
        assert db(rms(bp[4000:]), rms(un[4000:])) < -15
        lp, _ = note(lib, f, 62, vel=1)
        assert db(rms(lp[12000:]), rms(un[12000:])) < -20
    finally:
        lib.smp_sf_close(f)


def test_sfz_loop_modes(lib, tmp_path):
    """loop_mode: the default follows the file's smpl loop (loop_continuous) or its absence (no_loop); one_shot
    ignores note-off; loop_sustain plays on past the loop after release; loop_start, loop_end, offset and end
    become SF2's address offsets."""
    write_wav(tmp_path / "loop.wav", sine(RATE // 2, 441.0), loop=(1000, 1999))
    write_wav(tmp_path / "plain.wav", sine(RATE // 2, 441.0))
    path = write_sfz(tmp_path / "l.sfz", """
<group> pitch_keycenter=60
<region> sample=loop.wav key=60
<region> sample=loop.wav key=61 pitch_keycenter=61 loop_mode=no_loop
<region> sample=plain.wav key=62 pitch_keycenter=62 loop_mode=one_shot
<region> sample=loop.wav key=63 pitch_keycenter=63 loop_mode=loop_sustain loop_start=500 loop_end=1499
<region> sample=plain.wav key=64 pitch_keycenter=64 offset=100 end=20099
<region> sample=plain.wav key=65 pitch_keycenter=65 loop_mode=loop_continuous loop_start=200 loop_end=1199
""")
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert [x.gen[54] for x in r] == [1, 0, 0, 3, 0, 1]
        assert r[2].cond.one_shot == 1 and r[0].cond.one_shot == 0
        assert (r[3].gen[2], r[3].gen[3]) == (-500, -500)                 # against the file's loop 1000..2000
        assert r[4].gen[0] == 100 and r[4].gen[1] == 0 and r[4].cond.end_frame == 20100   # end: the last frame played
        assert (r[5].gen[2], r[5].gen[3]) == (200, 1200)                   # no file loop: from frame 0
        n = int(1.2 * OUT)
        held = lambda k, rel=-1: note(lib, f, k, n=n, release_at=rel)[0][:, 0]
        late = slice(int(0.8 * OUT), int(0.9 * OUT))
        assert rms(held(60)[late]) > 0.05                                   # loops past the 0.5 s sample
        assert rms(held(61)[late]) < 1e-4                                   # ends with the sample
        one = held(62, rel=int(0.05 * OUT))
        assert rms(one[int(0.3 * OUT):int(0.4 * OUT)]) > 0.05               # plays on after its note-off
        assert rms(one[late]) < 1e-4
        sus = held(63, rel=int(0.8 * OUT))
        assert rms(sus[int(0.7 * OUT):int(0.79 * OUT)]) > 0.05              # held: loops
        assert rms(sus[int(1.0 * OUT):]) < 1e-3                             # released: plays out, then silence
    finally:
        lib.smp_sf_close(f)


def test_sfz_offset_start_head(lib, tmp_path):
    """A region whose offset lies past the head the loader keeps gets a start head: a sample of its own from that
    frame, so the note starts from memory at once; an offset inside the head doesn't."""
    x = sine(3 * RATE, 300.0)
    x[2 * RATE:] = sine(RATE, 600.0)
    write_wav(tmp_path / "long.wav", x)
    path = write_sfz(tmp_path / "o.sfz", """
<region> sample=long.wav key=60 offset=88200
<region> sample=long.wav key=61 pitch_keycenter=61 offset=1000
<region> sample=long.wav key=62 pitch_keycenter=62 offset=88200
""")
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert lib.smp_sf_sample_count(f) == 2                              # the file, and one start head
        assert r[0].sample == r[2].sample != r[1].sample
        assert r[0].gen[0] == 0 and r[0].gen[4] == 0 and r[1].gen[0] == 1000
        load_all(lib, f)
        lib.smp_sf_use(f, 0, 1)
        lib.smp_sf_wait_idle(f, 10000)
        head, _ = pcm(lib, f, r[0].sample)
        lib.smp_sf_use(f, 0, -1)
        want = np.round(sine(len(head), 600.0) * 32767)
        assert np.max(np.abs(head[:2000] - want[:2000])) <= 1                # the file's frames from 2 s on
        y, _ = note(lib, f, 60, n=12000)
        assert abs(peak_hz(y[1000:12000, 0], OUT) - 600) < 3
    finally:
        lib.smp_sf_close(f)


class Player:
    def __init__(self, lib, tmp_path, path):
        self.lib, self.tmp = lib, tmp_path
        err = ctypes.create_string_buffer(512)
        self.s = lib.smp_synth_new(float(OUT), 256)
        lib.smp_synth_set_param(self.s, 33, 0)                               # no effects
        assert lib.smp_synth_load(self.s, path.encode(), err, 512) == 1, err.value

    def render(self, events, secs):
        out = self.tmp / "r.wav"
        assert self.lib.smp_synth_render_test(self.s, "\n".join(events).encode(), int(secs * OUT), 256,
                                              str(out).encode(), 0) == 0
        x, _ = sf.read(str(out), dtype="float32", always_2d=True)
        return x[:, 0]

    def free(self):
        self.lib.smp_synth_free(self.s)


def on(k, v, t):
    return "midi %d %d %d %d" % (0x90, k, v, int(round(t * OUT)))


def off(k, t):
    return "midi %d %d 0 %d" % (0x80, k, int(round(t * OUT)))


def tone(x, f):
    w = np.hanning(len(x))
    t = np.arange(len(x)) / OUT
    return 2 * abs(np.sum(x * w * np.exp(-2j * np.pi * f * t))) / np.sum(w)


def test_sfz_round_robin_and_release(lib, tmp_path):
    """seq_length and seq_position (position 1 by default) cycle through Task 8's player; a release region plays
    at note-off and falls rt_decay dB a second held; half-given keyswitch ranges take SFZ's defaults and
    sw_default is set on every region; polyphony per group."""
    freqs = [300.0, 500.0, 700.0, 900.0]
    for fq in freqs:
        write_wav(tmp_path / ("s%d.wav" % fq), sine(RATE, fq))
    path = write_sfz(tmp_path / "rr.sfz", """
<group> seq_length=3 pitch_keycenter=60 key=60
<region> sample=s300.wav
<region> sample=s500.wav seq_position=2
<region> sample=s700.wav seq_position=3
<group> trigger=release rt_decay=6 pitch_keycenter=60 key=60
<region> sample=s900.wav
""")
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert [(x.cond.seq_len, x.cond.seq_pos) for x in r[:3]] == [(3, 1), (3, 2), (3, 3)]
        assert r[3].cond.trigger == 1 and abs(r[3].cond.rt_decay - 6) < 1e-6 and r[3].cond.seq_len == 0
    finally:
        lib.smp_sf_close(f)
    sw = write_sfz(tmp_path / "sw.sfz", """
<group> sw_lokey=36 sw_last=36 key=100
<region> sample=s300.wav
<group> sw_hikey=40 sw_last=38 sw_default=38 key=101 xfin_lokey=90
<region> sample=s300.wav
""")
    f = opened(lib, sw)
    try:
        r = regions(lib, f, 0)
        assert (r[0].cond.sw_lokey, r[0].cond.sw_hikey, r[0].cond.sw_last) == (36, 127, 36)
        assert (r[1].cond.sw_lokey, r[1].cond.sw_hikey, r[1].cond.sw_last) == (0, 40, 38)
        assert (r[1].cond.xfin_lokey, r[1].cond.xfin_hikey) == (90, 0)
        assert all(x.cond.sw_default == 38 for x in r)
    finally:
        lib.smp_sf_close(f)
    p = Player(lib, tmp_path, path)
    try:
        ev = []
        for i in range(4):
            ev += [on(60, 100, 0.5 * i), off(60, 0.5 * i + 0.2)]
        x = p.render(ev, 2.2)
        got = []
        for i in range(4):
            w = x[int((0.5 * i + 0.05) * OUT):int((0.5 * i + 0.18) * OUT)]
            got.append([fq for fq in freqs[:3] if tone(w, fq) > 0.02])
        assert got == [[300.0], [500.0], [700.0], [300.0]]
        rel = x[int(0.25 * OUT):int(0.45 * OUT)]
        assert tone(rel, 900.0) > 0.02                                    # the release region after note-off
        # rt_decay: held 1.1 s against 0.1 s is 6 dB a second quieter
        a = p.render([on(60, 100, 0), off(60, 0.1)], 0.6)
        b = p.render([on(60, 100, 0), off(60, 1.1)], 1.6)
        la = tone(a[int(0.2 * OUT):int(0.4 * OUT)], 900.0)
        lb = tone(b[int(1.2 * OUT):int(1.4 * OUT)], 900.0)
        assert abs(db(la, lb) - 6.0) < 1.0
    finally:
        p.free()


def test_sfz_polyphony_and_one_shot_choke(lib, tmp_path):
    """polyphony=1 on a group stops its earlier voice when another key of the group starts (other groups play on);
    off_by chokes a one-shot (a hi-hat), which a note-off doesn't stop."""
    write_wav(tmp_path / "s300.wav", sine(RATE, 300.0))
    write_wav(tmp_path / "s500.wav", sine(RATE, 500.0))
    write_wav(tmp_path / "s700.wav", sine(RATE, 700.0))
    path = write_sfz(tmp_path / "p.sfz", """
<group> group=5 polyphony=1
<region> sample=s300.wav key=60 pitch_keycenter=60
<region> sample=s500.wav key=61 pitch_keycenter=61
<group> group=6
<region> sample=s700.wav key=65 pitch_keycenter=65
<group>
<region> sample=s300.wav key=62 pitch_keycenter=62 loop_mode=one_shot group=1 off_by=2
<region> sample=s500.wav key=64 pitch_keycenter=64 loop_mode=one_shot group=2
""")
    p = Player(lib, tmp_path, path)
    try:
        x = p.render([on(65, 100, 0), on(60, 100, 0), on(61, 100, 0.2), off(60, 0.5), off(61, 0.5), off(65, 0.5)], 0.6)
        early, late = x[int(0.05 * OUT):int(0.18 * OUT)], x[int(0.3 * OUT):int(0.45 * OUT)]
        assert tone(early, 300.0) > 0.05 and tone(early, 700.0) > 0.05
        assert tone(late, 300.0) < 0.01                                     # stopped by key 61 in its group
        assert tone(late, 500.0) > 0.05 and tone(late, 700.0) > 0.05         # group 6 isn't limited
        z = p.render([on(62, 100, 0), off(62, 0.05), on(64, 100, 0.3)], 0.6)
        assert tone(z[int(0.1 * OUT):int(0.25 * OUT)], 300.0) > 0.05          # one-shot plays past note-off
        assert tone(z[int(0.4 * OUT):int(0.55 * OUT)], 300.0) < 0.01          # choked by group 2
        assert tone(z[int(0.4 * OUT):int(0.55 * OUT)], 500.0) > 0.05
    finally:
        p.free()


def test_sfz_missing_sample(lib, tmp_path):
    """Missing samples skip their regions; the font opens with the rest and its problem names the first missing
    file and the count; a path that can't be made counts as unreadable. A file of no format the header probe knows
    is left to the loader (never decoded at open), which says so when it can't decode it. With every sample missing
    the file doesn't open, and the reason says so."""
    write_wav(tmp_path / "a.wav", sine(RATE // 4, 440.0))
    (tmp_path / "junk.wav").write_bytes(b"RIFF\x10\0\0\0WAVEjunkjunkjunk")
    long_name = "y" * 1000 + ".wav"
    path = write_sfz(tmp_path / "m.sfz", """
<region> sample=a.wav key=60
<region> sample=b.wav key=61
<region> sample=c.wav key=62
<region> sample=b.wav key=63
<region> sample=junk.wav key=64
<region> sample=%s key=65
<region> sample=%s key=66
""" % (long_name, long_name))
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert [x.lokey for x in r] == [60, 64, 64]      # junk.wav: unknown, so taken as stereo until decoded
        prob = lib.smp_sf_problem(f).decode()
        assert "Missing 2 samples (first: b.wav)" in prob
        assert "1 sample can't be read (first: yyyy" in prob
        x, nv = note(lib, f, 60)
        assert nv == 1 and rms(x) > 0.01
        load_all(lib, f)
        prob = lib.smp_sf_problem(f).decode()
        assert prob.startswith("Missing 2 samples (first: b.wav)") and "junk.wav can't be decoded" in prob
        x, nv = note(lib, f, 64)
        assert nv == 0
    finally:
        lib.smp_sf_close(f)
    gone = write_sfz(tmp_path / "gone.sfz", "<region> sample=nothere.wav key=60\n")
    f, err = sf_open(lib, gone)
    assert not f and "Missing 1 sample (first: nothere.wav)" in err and "gone.sfz" in err


def test_sfz_unknown_opcodes_counted(lib, tmp_path):
    """Opcodes this can't play are counted once per name (the first kept); labels and hints aren't; unknown
    headers count as <name>."""
    write_wav(tmp_path / "a.wav", sine(RATE // 4, 440.0))
    path = write_sfz(tmp_path / "u.sfz", """
<control> hint_ram_based=1 label_cc7=Volume
<global> eq1_freq=100 eq1_gain=3 region_label=Hi
<group> foo_bar=1 bar_baz=2 eq1_freq=200 group_label=x
<region> sample=a.wav key=60 sw_label=y
<region> sample=a.wav key=61 bar_baz=3
<weird> x=1
""")
    f = opened(lib, path)
    try:
        assert len(regions(lib, f, 0)) == 2
        assert ignored(lib, f) == (5, "eq1_freq")      # eq1_freq, eq1_gain, foo_bar, bar_baz, <weird>
    finally:
        lib.smp_sf_close(f)


def test_sfz_corrupt(lib, tmp_path):
    """Every 97th truncation, seeded random corruption, random bytes, a huge line, deep and cyclic includes:
    each opens as a font or gives NULL with a reason, never a crash; some that open load their samples."""
    write_wav(tmp_path / "a.wav", sine(RATE // 4, 440.0), loop=(100, 999))
    sf.write(str(tmp_path / "b.flac"), np.stack([sine(RATE // 4, 440.0), sine(RATE // 4, 660.0)], 1), RATE)
    write_sfz(tmp_path / "self.sfz", '#include "self.sfz"\n<region> sample=a.wav key=70\n')
    good = """#define $K 60
<control> default_path=./
<global> ampeg_release=0.2 // comment
<group> seq_length=2 lovel=1 hivel=127 cutoff=2000 fil_type=hpf_2p
<region> sample=a.wav key=$K seq_position=1 offset=10 end=9000 loop_mode=loop_sustain
<region> sample=b.flac lokey=c4 hikey=g4 seq_position=2 pan=20 width=50 sw_lokey=24 sw_hikey=30 sw_last=24
<region> sample=*sine key=72 trigger=release rt_decay=3 locc64=64 hicc64=127 amp_velcurve_64=0.5
#include "self.sfz"
"""
    data = good.encode()
    variants = [data[:cut] for cut in range(0, len(data), 97)]
    rng = random.Random(97)
    for _ in range(300):
        b = bytearray(data)
        for _ in range(8):
            b[rng.randrange(len(b))] = rng.randrange(256)
        variants.append(bytes(b))
    variants.append(bytes(rng.randrange(256) for _ in range(5000)))
    variants.append(b"a" * (1 << 20) + b"\n<region> sample=a.wav key=60\n")
    variants.append(b"<region> sample=" + b"x" * 5000 + b".wav key=60\n" + b"$" * 100000)
    variants.append(b"#define $A $A$A$A$A\n" * 50 + b"<region> sample=a.wav key=$A\n")
    path = tmp_path / "bad.sfz"
    opened_n = 0
    for k, v in enumerate(variants):
        path.write_bytes(v)
        f, err = sf_open(lib, path)
        if f:
            opened_n += 1
            walk(lib, f)
            ignored(lib, f)
            lib.smp_sf_problem(f)
            if k % 7 == 0:
                load_all(lib, f)
            lib.smp_sf_close(f)
        else:
            assert err
    assert opened_n > 0


def test_sfz_paths_with_spaces_and_backslashes(lib, tmp_path):
    """Sample paths with spaces run to the next opcode; backslashes and slashes both work, with default_path and
    absolute paths. A stereo FLAC is decoded (once) and plays as two regions panned left and right."""
    d = tmp_path / "my samples"
    os.makedirs(d)
    write_wav(d / "Grand Piano C4.wav", sine(RATE // 2, 440.0))
    sf.write(str(d / "Grand Piano D4.flac"), np.stack([sine(RATE // 2, 440.0), sine(RATE // 2, 660.0)], 1), RATE)
    sf.write(str(d / "Grand Piano E4.ogg"), sine(RATE // 2, 440.0), RATE, format="OGG", subtype="VORBIS")
    absolute = str(d / "Grand Piano C4.wav").replace("/", "\\")
    path = write_sfz(tmp_path / "s.sfz", """
<region> sample=my samples\\Grand Piano C4.wav lokey=60 hikey=60 pitch_keycenter=60
<control> default_path=my samples/
<region> sample=Grand Piano D4.flac   lokey=62 hikey=62 pitch_keycenter=62
<region> sample=%s key=64
<region> sample=Grand Piano E4.ogg key=66
""" % absolute)
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert [x.lokey for x in r] == [60, 62, 62, 64, 66]
        assert r[0].sample == r[3].sample                                    # the same file, once
        assert (r[1].gen[17], r[2].gen[17]) == (-500, 500)                  # the stereo pair
        assert lib.smp_sf_problem(f) == b""
        before = lib.smp_sf_test_decodes()
        x, nv = note(lib, f, 60)
        assert nv == 1 and abs(peak_hz(x[2000:20000, 0], OUT) - 440) < 2
        y, nv = note(lib, f, 62)
        assert nv == 2
        assert abs(peak_hz(y[2000:20000, 0], OUT) - 440) < 2                # left channel on the left
        assert abs(peak_hz(y[2000:20000, 1], OUT) - 660) < 2                # right on the right
        assert lib.smp_sf_test_decodes() - before <= 2                       # the FLAC's channels from one decode
        o, nv = note(lib, f, 66)
        assert nv == 1 and abs(peak_hz(o[2000:20000, 0], OUT) - 440) < 2   # Ogg Vorbis, length from its last page
    finally:
        lib.smp_sf_close(f)


def test_sfz_stereo_pairs_count_once(lib, tmp_path):
    """A stereo sample's two regions count as one voice: note_polyphony=1 keeps both halves (centred, not hard
    right), and polyphony=1 on a group stops both halves of the older note together."""
    write_wav(tmp_path / "st300.wav", np.stack([sine(RATE, 300.0), sine(RATE, 300.0)], 1))
    write_wav(tmp_path / "st500.wav", np.stack([sine(RATE, 500.0), sine(RATE, 500.0)], 1))
    path = write_sfz(tmp_path / "sp.sfz", """
<region> sample=st300.wav key=60 pitch_keycenter=60 note_polyphony=1
<group> group=3 polyphony=1
<region> sample=st300.wav key=62 pitch_keycenter=62
<region> sample=st500.wav key=64 pitch_keycenter=64
""")
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert [x.cond.pair for x in r] == [0, 1, 0, 1, 0, 1]
    finally:
        lib.smp_sf_close(f)
    p = Player(lib, tmp_path, path)
    try:
        out = tmp_path / "st.wav"
        assert lib.smp_synth_render_test(p.s, "\n".join([on(60, 100, 0), off(60, 0.4)]).encode(), int(0.5 * OUT), 256,
                                         str(out).encode(), 0) == 0
        x, _ = sf.read(str(out), dtype="float32", always_2d=True)
        w = x[int(0.1 * OUT):int(0.3 * OUT)]
        assert rms(w[:, 0]) > 0.05 and abs(db(rms(w[:, 0]), rms(w[:, 1]))) < 0.5      # both halves: centred
        y = p.render([on(62, 100, 0), on(64, 100, 0.2), off(62, 0.5), off(64, 0.5)], 0.6)
        assert tone(y[int(0.05 * OUT):int(0.18 * OUT)], 300.0) > 0.05
        late = y[int(0.3 * OUT):int(0.45 * OUT)]
        assert tone(late, 300.0) < 0.005 and tone(late, 500.0) > 0.05          # both halves of 62 stopped
    finally:
        p.free()


def test_sfz_made_samples_and_velcurve(lib, tmp_path):
    """*sine plays a sine at the region's pitch (C4 at pitch_keycenter) at full level; *noise is broadband;
    *silence is silent. amp_velcurve_N sets the velocity curve: with 64 at 1, velocity 64 is as loud as 127."""
    write_wav(tmp_path / "a.wav", sine(RATE // 2, 440.0))
    path = write_sfz(tmp_path / "mk.sfz", """
<region> sample=*sine key=69 pitch_keycenter=60
<region> sample=*noise key=70
<region> sample=*silence key=71
<region> sample=a.wav key=72 pitch_keycenter=72 amp_velcurve_64=1
<region> sample=a.wav key=73 pitch_keycenter=73
""")
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert [x.gen[54] for x in r[:3]] == [1, 1, 1]                        # made samples loop
        x, _ = note(lib, f, 69, vel=127)
        assert abs(peak_hz(x[2000:20000, 0], OUT) - 261.6256 * 2 ** (9 / 12)) < 1
        cc7 = (100 / 127) ** 2                                               # the channel's CC7 at its reset value
        assert abs(np.abs(x[4000:20000, 0]).max() - cc7 * math.sqrt(0.5)) < 0.02
        n, _ = note(lib, f, 70, vel=127)
        sig = n[4000:20000, 0]
        assert rms(sig) > 0.1 and np.mean(np.abs(np.diff(np.sign(sig))) > 0) > 0.2   # many zero crossings
        z, _ = note(lib, f, 71, vel=127)
        assert rms(z) < 1e-7
        a64, _ = note(lib, f, 72, vel=64)
        a127, _ = note(lib, f, 72, vel=127)
        b64, _ = note(lib, f, 73, vel=64)
        b127, _ = note(lib, f, 73, vel=127)
        assert abs(db(rms(a64[2000:20000]), rms(a127[2000:20000]))) < 0.1
        assert abs(db(rms(b64[2000:20000]), rms(b127[2000:20000])) - 40 * math.log10(64 / 127)) < 0.2
    finally:
        lib.smp_sf_close(f)


def test_sfz_filter_unity_keytrack_and_xml(lib, tmp_path):
    """SFZ's resonant lowpass keeps its passband at unity (SF2.04 would lower it by half the peak); a negative
    pitch_keytrack plays lower going up; XML (a Decent Sampler preset with <group> tags) isn't taken as SFZ
    (Task 10 plays it as a Decent Sampler preset), nor is other XML."""
    write_wav(tmp_path / "s100.wav", sine(RATE, 100.0))
    write_wav(tmp_path / "a.wav", sine(RATE // 2, 440.0))
    path = write_sfz(tmp_path / "fu.sfz", """
<region> sample=s100.wav key=60 cutoff=5000 resonance=12
<region> sample=s100.wav key=61 pitch_keycenter=61
<region> sample=a.wav lokey=40 hikey=50 pitch_keycenter=45 pitch_keytrack=-100
""")
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert r[0].cond.fil_unity == 1 and r[0].gen[9] == 120 and r[2].gen[56] == -100
        a, _ = note(lib, f, 60)
        b, _ = note(lib, f, 61)
        assert abs(db(rms(a[4000:]), rms(b[4000:]))) < 0.5
        c, _ = note(lib, f, 47)
        assert abs(peak_hz(c[2000:20000, 0], OUT) - 440 * 2 ** (-2 / 12)) < 2
    finally:
        lib.smp_sf_close(f)
    xml = tmp_path / "p.dspreset"
    xml.write_text('<?xml version="1.0"?>\n<DecentSampler><groups><group><sample path="a.wav"/></group></groups>'
                   '</DecentSampler>\n')
    f = opened(lib, xml)                                                    # Task 10: Decent Sampler's own
    try:
        assert len(regions(lib, f, 0)) == 1
    finally:
        lib.smp_sf_close(f)
    other = tmp_path / "other.sfz"                                         # other XML: not SFZ, whatever its name
    other.write_text('<?xml version="1.0"?>\n<region><group/></region>\n')
    f, err = sf_open(lib, other)
    assert not f and "SFZ" not in err


def test_sfz_decoded_lengths(lib, tmp_path):
    """A FLAC whose header claims twice its real length: end and loops clamp to the decoded length and the note
    plays the real sound; an IMA ADPCM WAV (no cheap length) is decoded by the loader with its own length and
    rate."""
    flac = tmp_path / "liar.flac"
    sf.write(str(flac), sine(RATE // 2, 440.0), RATE)
    b = bytearray(flac.read_bytes())
    assert b[:4] == b"fLaC"
    total = int.from_bytes(b[22:26], "big") | (b[21] & 15) << 32
    total2 = total * 2
    b[21] = (b[21] & 0xF0) | ((total2 >> 32) & 15)
    b[22:26] = (total2 & 0xFFFFFFFF).to_bytes(4, "big")
    flac.write_bytes(bytes(b))
    sf.write(str(tmp_path / "ad.wav"), sine(11025, 440.0, rate=22050), 22050, subtype="IMA_ADPCM")
    path = write_sfz(tmp_path / "dl.sfz", """
<region> sample=liar.flac key=60 end=40000 loop_mode=loop_continuous loop_start=10000 loop_end=39999
<region> sample=ad.wav key=62 pitch_keycenter=62
""")
    f = opened(lib, path)
    try:
        x, nv = note(lib, f, 60, n=int(1.5 * OUT))
        assert nv == 1 and np.all(np.isfinite(x))
        assert abs(peak_hz(x[2000:20000, 0], OUT) - 440) < 2
        y, nv = note(lib, f, 62)
        assert nv == 1 and abs(peak_hz(y[2000:12000, 0], OUT) - 440) < 3      # its own 22050 Hz rate
        walk(lib, f)
    finally:
        lib.smp_sf_close(f)


def sfz_stats(lib):
    probed, cached, wrote = ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
    path = ctypes.create_string_buffer(2048)
    lib.smp_sf_test_sfz_stats(ctypes.byref(probed), ctypes.byref(cached), ctypes.byref(wrote), path, 2048)
    return probed.value, cached.value, wrote.value, path.value.decode()


def test_sfz_probe_cache(lib, tmp_path):
    """The second open reuses every probe cache entry and doesn't rewrite the file; a sample file that changed
    (size and time) is probed again; a damaged probe cache is ignored safely (everything is probed, and it is
    written again)."""
    lib.smp_sf_test_sfz_stats.restype = None
    for i in range(12):
        write_wav(tmp_path / ("s%d.wav" % i), sine(RATE // 10, 300.0 + 50 * i), loop=(100, 999) if i % 2 else None)
    path = write_sfz(tmp_path / "pc.sfz", "".join("<region> sample=s%d.wav key=%d\n" % (i, 40 + i) for i in range(12)))

    def opened_regions():
        f = opened(lib, path)
        try:
            return [(r.lokey, r.gen[54], lib.smp_sf_sample(f, r.sample).contents.loop_end) for r in regions(lib, f, 0)]
        finally:
            lib.smp_sf_close(f)

    first = opened_regions()
    probed, cached, wrote, pc = sfz_stats(lib)
    assert (probed, cached, wrote) == (12, 0, 1) and pc.endswith(".sfzp") and os.path.exists(pc)
    stamp = os.stat(pc).st_mtime_ns
    assert opened_regions() == first
    assert sfz_stats(lib)[:3] == (0, 12, 0) and os.stat(pc).st_mtime_ns == stamp      # every entry, no rewrite
    write_wav(tmp_path / "s3.wav", sine(RATE // 5, 450.0))                            # changed: longer, no loop
    again = opened_regions()
    assert sfz_stats(lib)[:3] == (1, 11, 1)
    assert again[3][1:] == (0, 0) and again[:3] == first[:3]
    for junk in (b"SFZP\x02\0\0\0\xff\xff\xff\x7f" + b"\x01" * 50, b"", os.urandom(3000),
                 open(pc, "rb").read()[:-7]):
        with open(pc, "wb") as f:
            f.write(junk)
        assert opened_regions() == again
        probed, cached, wrote, _ = sfz_stats(lib)
        assert probed + cached == 12 and wrote == (probed > 0)


def test_sfz_odd_error_keeps_the_drive(lib, tmp_path):
    """A local sample that fails with an odd error (or even a network or not-ready one: the disk is local) is
    counted alone; the other files on the same drive still load."""
    lib.smp_sf_test_probe_error.restype = None
    lib.smp_sf_test_probe_error.argtypes = [ctypes.c_char_p, ctypes.c_ulong]
    for n in ("odd.wav", "a.wav", "b.wav", "c.wav"):
        write_wav(tmp_path / n, sine(RATE // 10, 440.0))
    path = write_sfz(tmp_path / "od.sfz", "<region> sample=odd.wav key=60\n<region> sample=a.wav key=61\n"
                                          "<region> sample=b.wav key=62\n<region> sample=c.wav key=63\n")
    try:
        for code, says in ((1117, "1 sample can't be read (first: odd.wav)"),     # ERROR_IO_DEVICE
                           (21, "Missing 1 sample (first: odd.wav)"),             # ERROR_NOT_READY
                           (59, "Missing 1 sample (first: odd.wav)")):            # ERROR_UNEXP_NET_ERR
            lib.smp_sf_test_probe_error(b"odd.wav", code)
            os.utime(tmp_path / "a.wav")                                          # probed afresh, not from the cache
            f = opened(lib, path)
            try:
                assert [r.lokey for r in regions(lib, f, 0)] == [61, 62, 63], code
                assert lib.smp_sf_problem(f).decode() == says
            finally:
                lib.smp_sf_close(f)
    finally:
        lib.smp_sf_test_probe_error(None, 0)


def test_sfz_guessed_mono_is_centred(lib, tmp_path):
    """A format with no header probe (CAF) is taken as stereo; when it decodes as mono, both halves of the pair
    play it at 1/sqrt(2), the level of a centred mono sample, from one decode."""
    sf.write(str(tmp_path / "m.caf"), sine(RATE // 2, 440.0), RATE, format="CAF", subtype="PCM_16")
    write_wav(tmp_path / "m.wav", sine(RATE // 2, 440.0))
    path = write_sfz(tmp_path / "g.sfz", "<region> sample=m.caf key=60\n<region> sample=m.wav key=61 pitch_keycenter=61\n")
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert [x.lokey for x in r] == [60, 60, 61] and [x.cond.pair for x in r] == [0, 1, 0]
        before = lib.smp_sf_test_decodes()
        g, nv = note(lib, f, 60)
        assert nv == 2 and lib.smp_sf_test_decodes() - before == 1
        m, _ = note(lib, f, 61)
        for ch in (0, 1):
            assert abs(db(rms(g[2000:20000, ch]), rms(m[2000:20000, ch]))) < 0.2, ch
    finally:
        lib.smp_sf_close(f)


def test_sfz_close_stops_member_decode(lib, tmp_path):
    """A member file only FFmpeg reads, and an FFmpeg that hangs (the full 60 s stall timer): closing the font while
    the loader decodes it returns at once, and the FFmpeg it started is killed."""
    from test_cache import point_cache
    from test_formats import fake_ffmpeg, pids_running, settings
    import time
    point_cache(lib, tmp_path / "cache")
    d = fake_ffmpeg(tmp_path, "hang")
    (tmp_path / "x.tta").write_bytes(b"not anything" * 100)
    path = write_sfz(tmp_path / "h.sfz", "<region> sample=x.tta key=60\n")
    settings(lib, ffmpeg_path=str(d / "ffmpeg.exe"))
    try:
        f = opened(lib, path)
        lib.smp_sf_use(f, 0, 1)
        t = time.perf_counter()
        while not (d / "pids.txt").exists() and time.perf_counter() - t < 10:
            time.sleep(0.02)
        assert (d / "pids.txt").exists()                                 # the loader is decoding it
        time.sleep(0.3)
        t = time.perf_counter()
        lib.smp_sf_use(f, 0, -1)
        lib.smp_sf_close(f)
        secs = time.perf_counter() - t
    finally:
        settings(lib, ffmpeg_path="")
    assert secs < 2, secs
    t = time.perf_counter()
    while pids_running(d)[0] and time.perf_counter() - t < 5:
        time.sleep(0.05)
    assert pids_running(d)[0] == 0


def test_sfz_decode_cache_int16_and_trimmed(lib, tmp_path):
    """Decoded member files (FLAC, 16-bit) go to the cache as int16 (version 2: half the size of float), and the
    loader trims the cache to its limit after writing; the files of samples that stream (held open) stay."""
    from test_cache import point_cache
    import glob
    cache = tmp_path / "cache"
    point_cache(lib, cache, gb=0.0005)                         # about 0.5 MB
    lines = []
    for i in range(24):                                        # 24 short samples: 0.9 s each, about 79 KB as int16
        sf.write(str(tmp_path / ("s%d.flac" % i)), sine(int(RATE * 0.9), 200.0 + 10 * i), RATE, subtype="PCM_16")
        lines.append("<region> sample=s%d.flac key=%d" % (i, 40 + i))
    sf.write(str(tmp_path / "long.flac"), sine(RATE * 3, 300.0), RATE, subtype="PCM_16")   # streams: stays pinned
    lines.append("<region> sample=long.flac key=80 pitch_keycenter=80")
    path = write_sfz(tmp_path / "c.sfz", "\n".join(lines) + "\n")
    f = opened(lib, path)
    try:
        lib.smp_sf_use(f, 0, 1)
        assert lib.smp_sf_wait_idle(f, 60000) == 0
        files = glob.glob(str(cache / "*.smpc"))
        heads = [open(p, "rb").read(32) for p in files]
        assert heads and all(h[:4] == b"SMPC" and struct.unpack("<I", h[4:8])[0] == 2 for h in heads), heads[:2]
        sizes = {os.path.basename(p): os.path.getsize(p) for p in files}
        assert RATE * 3 * 2 + 32 in sizes.values()                       # the long one, int16, still there
        assert sum(sizes.values()) <= 0.0005 * 2 ** 30 + RATE * 3 * 2 + 32, sizes   # the rest trimmed to the limit
        g, nv = note(lib, f, 80, n=4 * RATE)                              # and it still plays from the cache
        assert nv == 1 and rms(g[RATE * 2:RATE * 3, 0]) > 0.03
    finally:
        lib.smp_sf_use(f, 0, -1)
        lib.smp_sf_close(f)
        point_cache(lib, cache)


def test_sfz_long_decode_doesnt_hold_up_other_fonts(lib, tmp_path):
    """One font's member decode hangs in FFmpeg; another font's samples still load at once (the loader leaves
    decodes to its worker)."""
    from test_cache import point_cache
    from test_formats import fake_ffmpeg, settings
    import time
    point_cache(lib, tmp_path / "cache")
    d = fake_ffmpeg(tmp_path, "hang")
    (tmp_path / "x.tta").write_bytes(b"not anything" * 100)
    slow = write_sfz(tmp_path / "slow.sfz", "<region> sample=x.tta key=60\n")
    write_wav(tmp_path / "b.wav", sine(RATE * 2, 440.0))
    quick = write_sfz(tmp_path / "quick.sfz", "<region> sample=b.wav key=60\n")
    settings(lib, ffmpeg_path=str(d / "ffmpeg.exe"))
    try:
        fa = opened(lib, slow)
        lib.smp_sf_use(fa, 0, 1)
        t = time.perf_counter()
        while not (d / "pids.txt").exists() and time.perf_counter() - t < 10:
            time.sleep(0.02)
        assert (d / "pids.txt").exists()                                 # the worker is stuck in FFmpeg
        fb = opened(lib, quick)
        try:
            t = time.perf_counter()
            lib.smp_sf_use(fb, 0, 1)
            assert lib.smp_sf_wait_idle(fb, 5000) == 0
            secs = time.perf_counter() - t
            assert lib.smp_sf_loaded_samples(fb) >= 1
            assert lib.smp_sf_wait_idle(fa, 100) == 1                    # the first font is still decoding
        finally:
            lib.smp_sf_use(fb, 0, -1)
            lib.smp_sf_close(fb)
        lib.smp_sf_use(fa, 0, -1)
        lib.smp_sf_close(fa)
    finally:
        settings(lib, ffmpeg_path="")
    assert secs < 1, secs


def test_sfz_global_reaches_regions_without_group(lib, tmp_path):
    """Opcodes under <global> or <master> reach a <region> that follows with no <group> between (they used to stop
    at the level they were set on)."""
    write_wav(tmp_path / "a.wav", sine(RATE // 2, 440.0))
    path = write_sfz(tmp_path / "g.sfz", "<global> ampeg_attack=0.5 volume=-6\n<region> sample=a.wav key=60\n"
                                         "<master> ampeg_release=2\n<region> sample=a.wav key=62\n"
                                         "<group> ampeg_decay=1\n<region> sample=a.wav key=64\n")
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert [(x.gen[34], x.gen[48]) for x in r] == [(-1200, 150)] * 3
        assert [x.gen[38] for x in r] == [-12000, 1200, 1200]
        assert [x.gen[36] for x in r] == [-12000, -12000, 0]
    finally:
        lib.smp_sf_close(f)
