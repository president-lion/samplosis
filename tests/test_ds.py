"""Decent Sampler (Task 10): a .dspreset is one preset whose regions come from its <groups>, <group> and <sample>
elements. Every test writes a small .dspreset and its samples (WAV, some FLAC) into tmp_path, opens it through
smp_sf_open, and checks the regions' generators and conditions, and how notes sound (smp_sf_test_note, or the
player for the region conditions)."""
import ctypes
import math
import random

import numpy as np
import soundfile as sf

from test_sf2 import load_all, presets, regions, sf_open
from test_sfz import (OUT, RATE, Player, cents, db, ignored, note, off, on, opened, peak_hz, rms, sine, tone, walk,
                      write_wav)


def write_ds(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return str(path)


def loop_end(r):
    return r.gen[3] + 32768 * r.gen[50]


def loop_start(r):
    return r.gen[2] + 32768 * r.gen[45]


def test_ds_mapping(lib, tmp_path):
    """<sample> attributes, with <group> and <groups> ones inherited (volume, tuning and pan combined across the
    levels as Decent Sampler does), become SF2's ranges and generators and the region conditions; <ui>, <effects>,
    <modulators> and <midi>, and attributes this can't play, are counted; a disabled group is skipped."""
    write_wav(tmp_path / "a.wav", sine(RATE // 2, 440.0))
    write_wav(tmp_path / "lp.wav", sine(RATE // 2, 440.0), loop=(2000, 2999))
    sf.write(str(tmp_path / "st.flac"), np.stack([sine(RATE // 2, 440.0), sine(RATE // 2, 660.0)], 1), RATE)
    (tmp_path / "sub").mkdir()
    write_wav(tmp_path / "sub" / "a&b.wav", sine(RATE // 2, 440.0))
    path = write_ds(tmp_path / "Mapped Keys.dspreset", """<?xml version="1.0" encoding="UTF-8"?>
<DecentSampler minVersion="1.0.0">
  <ui width="812" height="375"><tab name="main"><labeled-knob x="10" parameterName="Tone">
    <binding type="effect" level="instrument" position="0" parameter="FX_FILTER_FREQUENCY"/></labeled-knob></tab></ui>
  <groups volume="-4dB" attack="0.01" decay="0.2" glideTime="0.1">
    <group name="one" volume="0.5" groupTuning="1" groupPan="10" sustain="0.5" ampVelTrack="0.5">
      <sample path="a.wav" rootNote="62" loNote="60" hiNote="64" loVel="10" hiVel="100" tuning="-0.3"
              volume="3dB" pan="-50" release="0.5" loopCrossfade="100"/>
      <sample path="a.wav" rootNote="70" loNote="70" hiNote="70" start="100" end="9999" loopEnabled="true"
              loopStart="1000" loopEnd="4999" pitchKeyTrack="0"/>
    </group>
    <group>
      <sample path="a.wav" rootNote="72" loNote="72" hiNote="72" loopEnabled="true"/>
      <sample path="lp.wav" rootNote="73" loNote="73" hiNote="73"/>
      <sample path="lp.wav" rootNote="74" loNote="74" hiNote="74" loopEnabled="false"/>
      <sample path="a.wav" rootNote="75" loNote="75" hiNote="75" ampEnvEnabled="false"/>
      <sample path="st.flac" rootNote="76" loNote="76" hiNote="76"/>
      <sample path="sub/a&amp;b.wav" rootNote="77" loNote="77" hiNote="77" ampEnvAttack="0.5" attack="2"/>
    </group>
    <group ampVelTrack="1"><sample path="a.wav" rootNote="78" loNote="78" hiNote="78"/></group>
    <group enabled="false"><sample path="a.wav" rootNote="80" loNote="80" hiNote="80"/></group>
  </groups>
  <effects><effect type="lowpass" frequency="22000"/></effects>
  <modulators><lfo shape="sine" frequency="2"/></modulators>
  <midi><cc number="1"><binding level="ui" type="control" position="0" parameter="VALUE"/></cc></midi>
</DecentSampler>
""")
    f = opened(lib, path)
    try:
        assert presets(lib, f) == [(0, 0, "Mapped Keys")]
        r = regions(lib, f, 0)
        assert [x.lokey for x in r] == [60, 70, 72, 73, 74, 75, 76, 76, 77, 78]
        a = r[0]
        assert (a.lokey, a.hikey, a.lovel, a.hivel, a.gen[58]) == (60, 64, 10, 100, 62)
        assert cents(a) == 70                                       # groupTuning 1 + tuning -0.3 semitones
        assert a.gen[48] == 176                                     # -4 dB + 0.5 (-6.02 dB) + 3 dB = -7.02 dB, x -25
        assert a.gen[17] == -200                                    # groupPan 10 + pan -50
        assert a.gen[34] == round(1200 * math.log2(0.01))           # attack from <groups>
        assert a.gen[36] == round(1200 * math.log2(0.2))            # decay from <groups>
        assert a.gen[37] == 60                                      # sustain 0.5 from <group>: 6 dB
        assert a.gen[38] == -1200                                   # release 0.5 s of its own
        assert abs(a.cond.amp_veltrack - 50) < 1e-6 and a.gen[54] == 0 and a.gen[56] == 100
        b = r[1]
        assert b.gen[0] == 100 and b.cond.end_frame == 10000 and b.gen[54] == 1
        assert loop_start(b) == 1000 and loop_end(b) == 5000 and b.gen[56] == 0
        assert b.gen[48] == 251                                     # -4 dB + 0.5: -10.02 dB
        assert r[2].gen[54] == 1 and loop_start(r[2]) == 0 and loop_end(r[2]) == RATE // 2   # the whole file
        assert r[2].gen[48] == 100 and r[2].gen[37] == 0            # <group> volume and sustain don't reach here
        assert r[3].gen[54] == 1                                    # the file's own loop
        assert r[4].gen[54] == 0                                    # loopEnabled=false
        assert r[5].gen[54] == 0 and r[5].cond.one_shot == 1        # ampEnvEnabled=false
        assert (r[6].gen[17], r[7].gen[17]) == (-500, 500) and r[7].cond.pair == 1   # a stereo pair
        assert r[8].gen[34] == round(1200 * math.log2(0.5))         # ampEnvAttack wins over attack
        walk(lib, f)
        n, first = ignored(lib, f)
        assert n == 2 and first == "glideTime", (n, first)          # glideTime loopCrossfade (the knob, its effect,
                                                                    # its CC and the modulators are read now)
        x, nv = note(lib, f, 62)
        assert nv == 1
        assert abs(peak_hz(x[2000:20000, 0], OUT) - 440 * 2 ** (70 / 1200)) < 2
        assert rms(x[2000:20000, 0]) > 1.5 * rms(x[2000:20000, 1])  # panned left
        y, nv = note(lib, f, 77)
        assert nv == 1 and rms(y) > 0.01                            # the &amp; in its path was decoded
        hi, _ = note(lib, f, 78, vel=127)
        lo, _ = note(lib, f, 78, vel=63)
        assert abs(db(rms(lo[2000:20000]), rms(hi[2000:20000])) - db(63, 127)) < 0.2   # gain linear in velocity
        x, nv = note(lib, f, 80)
        assert nv == 0                                              # the disabled group
    finally:
        lib.smp_sf_close(f)
    # told by its content whatever its name; and by its name
    other = write_ds(tmp_path / "preset.xml", '<DecentSampler><groups><group><sample path="a.wav" rootNote="60"/>'
                                              '</group></groups></DecentSampler>')
    f = opened(lib, other)
    try:
        assert len(regions(lib, f, 0)) == 1 and ignored(lib, f)[0] == 0
    finally:
        lib.smp_sf_close(f)


def test_ds_round_robin(lib, tmp_path):
    """seqMode round_robin with seqPosition (the length found from the positions when seqLength is left out)
    cycles through Task 8's player; seqMode on <groups> is inherited; random splits the draw by position; always
    plays every time."""
    freqs = [300.0, 500.0, 700.0]
    for fq in freqs:
        write_wav(tmp_path / ("s%d.wav" % fq), sine(RATE, fq))
    path = write_ds(tmp_path / "rr.dspreset", """<DecentSampler>
  <groups seqMode="round_robin">
    <group seqPosition="1"><sample path="s300.wav" rootNote="60" loNote="60" hiNote="60"/></group>
    <group seqPosition="2"><sample path="s500.wav" rootNote="60" loNote="60" hiNote="60"/></group>
    <group seqPosition="3"><sample path="s700.wav" rootNote="60" loNote="60" hiNote="60"/></group>
    <group seqMode="random" seqLength="2">
      <sample path="s300.wav" rootNote="62" loNote="62" hiNote="62" seqPosition="1"/>
      <sample path="s500.wav" rootNote="62" loNote="62" hiNote="62" seqPosition="2"/>
    </group>
    <group seqMode="true_random">
      <sample path="s300.wav" rootNote="64" loNote="64" hiNote="64" seqPosition="1"/>
      <sample path="s500.wav" rootNote="64" loNote="64" hiNote="64" seqPosition="2"/>
      <sample path="s700.wav" rootNote="64" loNote="64" hiNote="64" seqPosition="3"/>
    </group>
    <group seqMode="always"><sample path="s300.wav" rootNote="65" loNote="65" hiNote="65" seqPosition="2"/></group>
  </groups>
</DecentSampler>
""")
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert [(x.cond.seq_len, x.cond.seq_pos) for x in r[:3]] == [(3, 1), (3, 2), (3, 3)]
        assert [(round(x.cond.lorand, 4), round(x.cond.hirand, 4), x.cond.seq_len) for x in r[3:5]] == \
            [(0, 0.5, 0), (0.5, 1, 0)]
        assert [(round(x.cond.lorand, 4), round(x.cond.hirand, 4)) for x in r[5:8]] == \
            [(0, 0.3333), (0.3333, 0.6667), (0.6667, 1)]
        assert (r[8].cond.seq_len, r[8].cond.lorand, r[8].cond.hirand) == (0, 0, 1)
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
            got.append([fq for fq in freqs if tone(w, fq) > 0.02])
        assert got == [[300.0], [500.0], [700.0], [300.0]]
        ev = []
        for i in range(12):
            ev += [on(62, 100, 0.25 * i), off(62, 0.25 * i + 0.15)]
        x = p.render(ev, 3.2)
        seen = set()
        for i in range(12):
            w = x[int((0.25 * i + 0.03) * OUT):int((0.25 * i + 0.13) * OUT)]
            hits = [fq for fq in freqs if tone(w, fq) > 0.02]
            assert len(hits) == 1                                    # exactly one of the two each time
            seen.add(hits[0])
        assert seen == {300.0, 500.0}
    finally:
        p.free()


def test_ds_release_trigger(lib, tmp_path):
    """trigger=release plays at note-off and falls releaseTriggerDecay dB a second held; first and legato map to
    the conditions; tags with silencedByTags choke (silencingMode fast or normal)."""
    for fq in (300.0, 500.0, 900.0):
        write_wav(tmp_path / ("s%d.wav" % fq), sine(RATE, fq))
    path = write_ds(tmp_path / "rel.dspreset", """<DecentSampler>
  <groups>
    <group><sample path="s300.wav" rootNote="60" loNote="60" hiNote="60"/></group>
    <group trigger="release" releaseTriggerDecay="-6dB">
      <sample path="s900.wav" rootNote="60" loNote="60" hiNote="60"/>
    </group>
    <group><sample path="s300.wav" rootNote="61" loNote="61" hiNote="61" trigger="first"/>
      <sample path="s300.wav" rootNote="61" loNote="61" hiNote="61" trigger="legato"/></group>
    <group tags="open, hats" silencedByTags="closed">
      <sample path="s300.wav" rootNote="62" loNote="62" hiNote="62" silencingMode="normal"/>
    </group>
    <group tags="closed"><sample path="s500.wav" rootNote="64" loNote="64" hiNote="64"/></group>
    <group trigger="release" releaseTriggerDecay="0.5"><sample path="s900.wav" rootNote="66" loNote="66" hiNote="66"/>
    </group>
  </groups>
</DecentSampler>
""")
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert r[0].cond.trigger == 0 and r[1].cond.trigger == 1 and abs(r[1].cond.rt_decay - 6) < 1e-6
        assert (r[2].cond.trigger, r[3].cond.trigger) == (2, 3)
        assert r[4].cond.off_by != 0 and r[4].cond.off_by == r[5].cond.group and r[4].cond.off_mode == 1
        assert r[4].cond.group != r[5].cond.group and r[5].cond.off_by == 0
        assert abs(r[6].cond.rt_decay - 20 * math.log10(2)) < 1e-3      # linear 0.5: half the gain lost a second
    finally:
        lib.smp_sf_close(f)
    p = Player(lib, tmp_path, path)
    try:
        x = p.render([on(60, 100, 0), off(60, 0.2)], 0.5)
        assert tone(x[int(0.05 * OUT):int(0.18 * OUT)], 900.0) < 0.01
        assert tone(x[int(0.25 * OUT):int(0.45 * OUT)], 900.0) > 0.02    # the release sample after note-off
        a = p.render([on(60, 100, 0), off(60, 0.1)], 0.6)
        b = p.render([on(60, 100, 0), off(60, 1.1)], 1.6)
        la = tone(a[int(0.2 * OUT):int(0.4 * OUT)], 900.0)
        lb = tone(b[int(1.2 * OUT):int(1.4 * OUT)], 900.0)
        assert abs(db(la, lb) - 6.0) < 1.0                               # held a second longer: 6 dB quieter
        x = p.render([on(62, 100, 0), on(64, 100, 0.2), off(62, 0.6), off(64, 0.6)], 0.7)
        assert tone(x[int(0.05 * OUT):int(0.18 * OUT)], 300.0) > 0.05
        late = x[int(0.45 * OUT):int(0.58 * OUT)]
        assert tone(late, 300.0) < 0.01 and tone(late, 500.0) > 0.05     # the closed hat silenced the open one
    finally:
        p.free()


def test_ds_missing_sample(lib, tmp_path):
    """Missing samples skip their regions; the preset opens with the rest and its problem names the first missing
    file and the count. With every sample missing the file doesn't open, and the reason says so."""
    write_wav(tmp_path / "a.wav", sine(RATE // 4, 440.0))
    path = write_ds(tmp_path / "m.dspreset", """<DecentSampler><groups><group>
  <sample path="a.wav" rootNote="60" loNote="60" hiNote="60"/>
  <sample path="b.wav" rootNote="61" loNote="61" hiNote="61"/>
  <sample path="c.wav" rootNote="62" loNote="62" hiNote="62"/>
  <sample path="b.wav" rootNote="63" loNote="63" hiNote="63"/>
  <sample rootNote="64"/>
</group></groups></DecentSampler>
""")
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert [x.lokey for x in r] == [60]
        prob = lib.smp_sf_problem(f).decode()
        assert "Missing 2 samples (first: b.wav)" in prob
        x, nv = note(lib, f, 60)
        assert nv == 1 and rms(x) > 0.01
        load_all(lib, f)
    finally:
        lib.smp_sf_close(f)
    gone = write_ds(tmp_path / "gone.dspreset",
                    '<DecentSampler><groups><group><sample path="nothere.wav" rootNote="60"/></group></groups>'
                    '</DecentSampler>')
    f, err = sf_open(lib, gone)
    assert not f and "Missing 1 sample (first: nothere.wav)" in err and "gone.dspreset" in err
    empty = write_ds(tmp_path / "empty.dspreset", "<DecentSampler><groups/></DecentSampler>")
    f, err = sf_open(lib, empty)
    assert not f and "empty.dspreset" in err


def test_ds_corrupt(lib, tmp_path):
    """Every 97th truncation, seeded random corruption, random bytes, a huge attribute, deep nesting and a flood
    of elements: each opens as a font or gives NULL with a reason, never a crash; some that open load their
    samples."""
    write_wav(tmp_path / "a.wav", sine(RATE // 4, 440.0), loop=(100, 999))
    sf.write(str(tmp_path / "b.flac"), np.stack([sine(RATE // 4, 440.0), sine(RATE // 4, 660.0)], 1), RATE)
    good = """<?xml version="1.0" encoding="UTF-8"?>
<DecentSampler minVersion="1.0.0">
  <ui><tab><knob parameterName="Release"><binding type="amp" level="instrument" parameter="ENV_RELEASE"/></knob></tab></ui>
  <groups attack="0.001" release="0.2" volume="0.8" seqMode="round_robin">
    <group tags="a" silencedByTags="b" seqPosition="1" trigger="attack">
      <sample path="a.wav" rootNote="60" loNote="58" hiNote="62" start="10" end="9000" loopEnabled="true"/>
      <sample path="b.flac" rootNote="64" loNote="63" hiNote="66" pan="20" volume="-3dB" loCC64="0" hiCC64="63"/>
    </group>
    <group tags="b" seqPosition="2" trigger="release" releaseTriggerDecay="0.5">
      <sample path="a.wav" rootNote="60" loNote="58" hiNote="62" tuning="0.5" ampEnvEnabled="false"/>
    </group>
  </groups>
  <effects><effect type="reverb"/></effects>
</DecentSampler>
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
    variants.append(b'<DecentSampler><groups><group><sample path="' + b"x" * (1 << 20) + b'.wav" rootNote="60"/>'
                    b'</group></groups></DecentSampler>')
    variants.append(b'<DecentSampler><groups><group><sample path="a.wav" rootNote="' + b"9" * 100000 + b'"/>'
                    b'</group></groups></DecentSampler>')
    variants.append(b"<DecentSampler>" + b"<group>" * 100000 + b'<sample path="a.wav" rootNote="60"/>')
    variants.append(b'<DecentSampler><groups><group>' + b'<sample path="a.wav" rootNote="60"/>' * 20000 +
                    b'</group></groups></DecentSampler>')
    variants.append(b'<DecentSampler><groups><group ' + b" ".join(b'a%d="1"' % i for i in range(20000)) +
                    b'><sample path="a.wav" rootNote="60"/></group></groups></DecentSampler>')
    variants.append(b'<DecentSampler><groups><group><sample path="a.wav" rootNote="60" ' + b"x" * 100000 +
                    b'="1"/></group></groups></DecentSampler>')
    variants.append(b'<DecentSampler><groups><group><sample path="' + b"x" * 65535 + "é".encode() + b'a.wav"'
                    b' rootNote="60"/></group></groups></DecentSampler>')        # a character across the 64 KB cap
    variants.append(b"\xef\xbb\xbf<DecentSampler><groups><group><sample path=\"a.wav\" rootNote=\"60\" "
                    b"loNote=\"-5\" hiNote=\"300\" loVel=\"200\" start=\"-7\" end=\"99999999999\" seqPosition=\"-1\""
                    b" seqLength=\"70000\" volume=\"nan\" pan=\"1e300\" tuning=\"inf\" attack=\"-1\"/>"
                    b"</group></groups></DecentSampler>")
    path = tmp_path / "bad.dspreset"
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


def test_ds_xml_refusals_and_caps(lib, tmp_path):
    """A DOCTYPE with entities (one a billion-laughs chain) and a &#0; reference are refused quickly, never
    expanded; a preset past the element cap opens with what fits and says so in the ignored count."""
    import time
    write_wav(tmp_path / "a.wav", sine(RATE // 4, 440.0))
    laughs = b'<!DOCTYPE DecentSampler [<!ENTITY a "aaaaaaaaaa">' + b"".join(
        b'<!ENTITY %c "%s">' % (98 + i, (b"&%c;" % (97 + i)) * 10) for i in range(9)) + b"]>"
    bad = [
        laughs + b'<DecentSampler><groups><group><sample path="&j;.wav" rootNote="60"/></group></groups></DecentSampler>',
        b'<!DOCTYPE DecentSampler [<!ENTITY p "a.wav">]><DecentSampler><groups><group><sample path="&p;"'
        b' rootNote="60"/></group></groups></DecentSampler>',
        b'<DecentSampler><groups><group><sample path="a&#0;.wav" rootNote="60"/></group></groups></DecentSampler>',
    ]
    path = tmp_path / "x.dspreset"
    for v in bad:
        path.write_bytes(v)
        t = time.time()
        f, err = sf_open(lib, path)
        assert time.time() - t < 2
        assert not f and "x.dspreset" in err, err
    many = (b'<DecentSampler><groups><group><sample path="a.wav" rootNote="60" loNote="60" hiNote="60"/>' +
            b"<sample/>" * (1 << 18) + b"</group></groups></DecentSampler>")
    path.write_bytes(many)
    f = opened(lib, path)
    try:
        assert len(regions(lib, f, 0)) == 1
        n, first = ignored(lib, f)
        assert n >= 1 and first.startswith("too many elements"), (n, first)
    finally:
        lib.smp_sf_close(f)


def test_ds_attr_without_space(lib, tmp_path):
    """An attribute right after another's closing quote, with no space between them (as Finnish Forest Percussion
    has it), reads as Decent Sampler reads it; quotes inside a value of the other kind and comments are left alone."""
    write_wav(tmp_path / "a.wav", sine(RATE // 2, 440.0))
    path = write_ds(tmp_path / "Tight.dspreset", """<?xml version="1.0" encoding="UTF-8"?>
<DecentSampler>
  <!-- x="1"y -->
  <groups label='a"b'>
    <group><sample path="a.wav"rootNote="60" loNote="60"hiNote="60"/></group>
  </groups>
</DecentSampler>
""")
    f = opened(lib, path)
    rs = regions(lib, f, 0)
    assert len(rs) == 1 and (rs[0].lokey, rs[0].hikey) == (60, 60)
    lib.smp_sf_close(f)


def test_ds_main_output_not_counted(lib, tmp_path):
    """output1Target="MAIN_OUTPUT" is where every sound goes anyway (Finnish Forest Percussion names it on each
    group): not counted; another output is, since everything plays on the main output here."""
    write_wav(tmp_path / "a.wav", sine(RATE // 2, 440.0))
    for target, counted in (("MAIN_OUTPUT", 0), ("AUX_STEREO_OUTPUT_1", 1)):
        path = write_ds(tmp_path / ("o%d.dspreset" % counted), """<DecentSampler><groups>
  <group output1Target="%s"><sample path="a.wav" rootNote="60" loNote="60" hiNote="60"/></group>
</groups></DecentSampler>""" % target)
        f = opened(lib, path)
        n, first = ignored(lib, f)
        assert n == counted and (not counted or first.startswith("output1Target")), (target, n, first)
        lib.smp_sf_close(f)
