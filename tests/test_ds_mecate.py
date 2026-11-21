"""Decent Sampler, 2026-11-21, from Anode Audio's Mecate presets (Mecate Bass and the rest): the gain effect and its
LEVEL knob, loopDuringRelease, a group's own <effects> (filters on its regions, with knobs on them live), <button>s
whose states turn groups and modulators on and off, bindings by tag (level="tag" identifier=...), knobs on a
group's amplitude envelope, and <modulators> (an envelope or a CC on a group filter's frequency, an LFO on the
tuning) with knobs on their values. None of Mecate's own samples enter the tests."""
import ctypes

import numpy as np

from test_ds import write_ds
from test_ds_knobs import SmpKnobInfo, bind_knobs, knob_index, knobs_text, spectrum_db
from test_sf2 import regions
from test_sfz import OUT, RATE, Player, ignored, off, on, opened, sine, tone, write_wav
from test_sfz_flex_eg import cents, hz_at

N440 = 44100 * 4 // 440 * 440 // 100 * 100          # whole periods of 440 Hz: 4 s, looped


def sine_file(tmp_path, name="a.wav", fq=440.0):
    write_wav(tmp_path / name, sine(N440, fq), loop=(0, N440 - 1))


def noise_file(tmp_path, name="n.wav"):
    write_wav(tmp_path / name, np.random.default_rng(7).standard_normal(N440) * 0.1, loop=(0, N440 - 1))


def preset(groups, ui="", effects="", modulators=""):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<DecentSampler minVersion="1.36.1">\n<ui><tab>%s</tab></ui>\n'
            '<groups ampVelTrack="0">%s</groups>\n%s%s\n</DecentSampler>\n'
            % (ui, groups, effects and "<effects>%s</effects>" % effects, modulators and "<modulators>%s</modulators>" % modulators))


def group(path="a.wav", attrs="", inner=""):
    return ('<group %s attack="0.005" release="0.05">%s<sample path="%s" rootNote="69" loNote="69" hiNote="69" '
            'loopEnabled="true"/></group>' % (attrs, inner, path))


def held(p, secs=0.8, extra=()):
    return p.render(list(extra) + [on(69, 127, 0)], secs)


def no_ignored(lib, path):
    f = opened(lib, path)
    try:
        assert ignored(lib, f)[0] == 0, ignored(lib, f)
        return knobs_text(lib, f)
    finally:
        lib.smp_sf_close(f)


def test_ds_gain_effect_and_level_knob(lib, tmp_path):
    """<effect type="gain" level="2"> doubles the preset (linear); levelUnit="db" reads decibels; a knob's LEVEL
    binding sets it (Mecate's Master fader: 0..2.6 linear)."""
    bind_knobs(lib)
    sine_file(tmp_path)
    plain = write_ds(tmp_path / "plain.dspreset", preset(group()))
    twice = write_ds(tmp_path / "gain.dspreset", preset(group(), effects='<effect type="gain" levelUnit="linear" level="2"/>'))
    half = write_ds(tmp_path / "db.dspreset", preset(group(), effects='<effect type="gain" levelUnit="db" level="-6.0206"/>'))
    fader = write_ds(tmp_path / "fader.dspreset", preset(group(), effects='<effect type="gain" level="1.82"/>', ui=(
        '<control parameterName="Master" minValue="0" maxValue="1" value="0.25">'
        '<binding type="effect" level="instrument" effectIndex="0" parameter="LEVEL" translation="linear" '
        'translationOutputMin="0" translationOutputMax="2.6"/></control>')))
    for path in (twice, half, fader):
        no_ignored(lib, path)
    lv = {}
    for name, path in (("plain", plain), ("twice", twice), ("half", half), ("fader", fader)):
        p = Player(lib, tmp_path, path)
        try:
            lv[name] = tone(held(p)[int(0.2 * OUT):int(0.6 * OUT)], 440.0)
            if name == "fader":
                lib.smp_synth_knob_set(p.s, knob_index(lib, p.s, "Master"), 1.0)
                lv["fader_top"] = tone(held(p)[int(0.2 * OUT):int(0.6 * OUT)], 440.0)
        finally:
            p.free()
    assert abs(lv["twice"] / lv["plain"] - 2) < 0.02, lv
    assert abs(lv["half"] / lv["plain"] - 0.5) < 0.01, lv
    assert abs(lv["fader"] / lv["plain"] - 0.65) < 0.01, lv          # 0.25 of the way to 2.6
    assert abs(lv["fader_top"] / lv["plain"] - 2.6) < 0.03, lv


def test_ds_loop_during_release(lib, tmp_path):
    """loopDuringRelease="false" leaves the loop at the release (SFZ's loop_sustain, sampleModes 3);
    loopCrossfade is still counted."""
    sine_file(tmp_path)
    path = write_ds(tmp_path / "l.dspreset", preset(
        group(attrs='loopDuringRelease="false"') + group(attrs='loopDuringRelease="true"')))
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert [x.gen[54] for x in r] == [3, 1]
        assert ignored(lib, f)[0] == 0
    finally:
        lib.smp_sf_close(f)
    xf = write_ds(tmp_path / "x.dspreset", preset(group(attrs='loopCrossfade="1000" loopCrossfadeMode="equal_power"')))
    f = opened(lib, xf)
    try:
        assert ignored(lib, f) == (1, "loopCrossfade")
    finally:
        lib.smp_sf_close(f)


def test_ds_group_filters_and_their_knobs(lib, tmp_path):
    """A group's <effects> filter its own regions (the first as SFZ's fil, the second as fil2): Mecate's cello
    lowpass. A knob bound to a group's effect (level="group" groupIndex effectIndex FX_FILTER_FREQUENCY) moves
    it live, the first filter or the second."""
    bind_knobs(lib)
    noise_file(tmp_path)
    ui = ('<control parameterName="Contour" minValue="0" maxValue="1" value="0">'
          '<binding type="effect" level="group" groupIndex="0" effectIndex="%d" parameter="FX_FILTER_FREQUENCY" '
          'translation="table" translationTable="0,400;1,22000"/></control>')
    plain = write_ds(tmp_path / "plain.dspreset", preset(group("n.wav")))
    one = write_ds(tmp_path / "one.dspreset", preset(group("n.wav", inner=(
        '<effects><effect type="lowpass" frequency="400" resonance="0.7"/></effects>')), ui=ui % 0))
    two = write_ds(tmp_path / "two.dspreset", preset(group("n.wav", inner=(
        '<effects><effect type="lowpass" frequency="22000" resonance="0.7"/>'
        '<effect type="lowpass" frequency="400" resonance="0.7"/></effects>')), ui=ui % 1))
    for path in (one, two):
        no_ignored(lib, path)
    f = opened(lib, one)
    try:
        assert regions(lib, f, 0)[0].cond.fil_type == 0              # SMP_FIL_LP on the region
    finally:
        lib.smp_sf_close(f)

    def highs(p):
        return spectrum_db(held(p)[int(0.2 * OUT):int(0.6 * OUT)], 5000, 9000)
    q = Player(lib, tmp_path, plain)
    try:
        ref = highs(q)
    finally:
        q.free()
    for path in (one, two):
        p = Player(lib, tmp_path, path)
        try:
            assert ref - highs(p) > 25, path                          # 400 Hz: the highs gone
            lib.smp_synth_knob_set(p.s, knob_index(lib, p.s, "Contour"), 1.0)
            assert abs(ref - highs(p)) < 2, path                      # opened to 22 kHz by the knob
        finally:
            p.free()


def test_ds_button_turns_groups_on_and_off(lib, tmp_path):
    """A <button>'s states set groups ENABLED (fixed_value true/false): a group that starts disabled is a layer a
    state can turn on. The button is a knob of whole values, 0 .. its states less 1. Bindings by tag
    (level="tag" identifier) reach every group with the tag."""
    bind_knobs(lib)
    sine_file(tmp_path, "a.wav", 440.0)
    sine_file(tmp_path, "b.wav", 660.0)
    ui = ('<button value="0" tooltip="Swap" style="image">'
          '<state name="A"><binding type="general" level="group" groupIndex="0" parameter="ENABLED" translation="fixed_value" translationValue="true"/>'
          '<binding type="general" level="tag" identifier="bee" parameter="ENABLED" translation="fixed_value" translationValue="false"/></state>'
          '<state name="B"><binding type="general" level="group" groupIndex="0" parameter="ENABLED" translation="fixed_value" translationValue="false"/>'
          '<binding type="general" level="tag" identifier="bee" parameter="ENABLED" translation="fixed_value" translationValue="true"/></state>'
          '</button>'
          '<control parameterName="Bee level" minValue="0" maxValue="1" value="1">'
          '<binding type="amp" level="tag" identifier="bee" parameter="AMP_VOLUME" translation="linear"/></control>')
    path = write_ds(tmp_path / "b.dspreset", preset(group("a.wav") + group("b.wav", attrs='tags="bee" enabled="false"'), ui=ui))
    text = no_ignored(lib, path)
    assert 'knob 1 "Swap" int 0 1 0 int' in text and "bind group_en" in text and "state=1" in text, text
    p = Player(lib, tmp_path, path)
    try:
        x = held(p)[int(0.2 * OUT):int(0.6 * OUT)]
        a, b = tone(x, 440.0), tone(x, 660.0)
        assert a > 0.05 and b < a * 1e-3, (a, b)
        sw = knob_index(lib, p.s, "Swap")
        lib.smp_synth_knob_set(p.s, sw, 1.0)
        x = held(p)[int(0.2 * OUT):int(0.6 * OUT)]
        a, b = tone(x, 440.0), tone(x, 660.0)
        assert b > 0.05 and a < b * 1e-3, (a, b)
        lib.smp_synth_knob_set(p.s, knob_index(lib, p.s, "Bee level"), 0.5)
        b2 = tone(held(p)[int(0.2 * OUT):int(0.6 * OUT)], 660.0)
        assert abs(b2 / b - 0.5) < 0.01, (b, b2)                       # level="tag" AMP_VOLUME on the tagged group
    finally:
        p.free()


LFO = ('<lfo shape="square" frequency="1" modAmount="1" scope="global" enabled="%s">'
       '<binding type="amp" level="instrument" parameter="GLOBAL_TUNING" modBehavior="add" translation="linear" '
       'translationOutputMin="-1" translationOutputMax="1"/></lfo>')
LFO_UI = ('<button value="0" tooltip="Vibrato">'
          '<state name="Off"><binding type="modulator" level="instrument" modulatorIndex="0" parameter="ENABLED" translation="fixed_value" translationValue="false"/></state>'
          '<state name="On"><binding type="modulator" level="instrument" modulatorIndex="0" parameter="ENABLED" translation="fixed_value" translationValue="true"/></state>'
          '</button>'
          '<control parameterName="Depth" minValue="0" maxValue="1" value="1">'
          '<binding type="modulator" level="instrument" modulatorIndex="0" parameter="MOD_AMOUNT" translation="linear"/></control>'
          '<control parameterName="Speed" minValue="0" maxValue="1" value="0">'
          '<binding type="modulator" level="instrument" modulatorIndex="0" parameter="FREQUENCY" translation="linear" '
          'translationOutputMin="1" translationOutputMax="2"/></control>')


def test_ds_lfo_modulator_on_tuning(lib, tmp_path):
    """An <lfo> bound to GLOBAL_TUNING (add, -1..1 semitones) swings the pitch: a square at 1 Hz is a semitone up
    for the first half second. Mecate's vibrato: off until its button turns it on, MOD_AMOUNT and FREQUENCY on
    knobs, live."""
    bind_knobs(lib)
    sine_file(tmp_path)
    path = write_ds(tmp_path / "v.dspreset", preset(group(), ui=LFO_UI, modulators=LFO % "false"))
    text = no_ignored(lib, path)
    assert "mod 0 enabled=0 amount=1" in text and "bind mod 0 param=enabled" in text, text
    on_ = write_ds(tmp_path / "on.dspreset", preset(group(), modulators=LFO % "true"))   # no knobs: baked
    no_ignored(lib, on_)
    p = Player(lib, tmp_path, path)
    q = Player(lib, tmp_path, on_)
    try:
        assert abs(cents(hz_at(held(p, 1.0), 0.1, 0.4))) < 8            # off
        y = held(q, 1.0)
        assert abs(cents(hz_at(y, 0.1, 0.4)) - 100) < 8 and abs(cents(hz_at(y, 0.6, 0.9)) + 100) < 8
        lib.smp_synth_knob_set(p.s, knob_index(lib, p.s, "Vibrato"), 1.0)
        y = held(p, 1.0)
        assert abs(cents(hz_at(y, 0.1, 0.4)) - 100) < 8 and abs(cents(hz_at(y, 0.6, 0.9)) + 100) < 8
        lib.smp_synth_knob_set(p.s, knob_index(lib, p.s, "Depth"), 0.5)
        assert abs(cents(hz_at(held(p, 1.0), 0.1, 0.4)) - 50) < 8
        lib.smp_synth_knob_set(p.s, knob_index(lib, p.s, "Speed"), 1.0)  # 2 Hz: down from 0.25 s
        assert abs(cents(hz_at(held(p, 1.0), 0.3, 0.45)) + 50) < 8
    finally:
        p.free(); q.free()


ENV = ('<envelope attack="0.001" decay="%s" sustain="0" release="0.3" modAmount="1" scope="voice" enabled="true">'
       '<binding type="effect" level="group" groupIndex="0" effectIndex="0" parameter="FX_FILTER_FREQUENCY" '
       'modBehavior="multiply" translation="table" translationTable="0,1;0.5,8;1,64"/></envelope>')
CCMOD = ('<midiCC number="4" modAmount="1" scope="global" enabled="true">'
         '<binding type="effect" level="group" groupIndex="0" effectIndex="0" parameter="FX_FILTER_FREQUENCY" '
         'modBehavior="multiply" translation="table" translationTable="0,1;1,64"/></midiCC>')
LP200 = '<effects><effect type="lowpass" frequency="200" resonance="0.7"/></effects>'


def test_ds_envelope_modulator_on_a_group_filter(lib, tmp_path):
    """Mecate's brass filter envelope: an <envelope> multiplying a group lowpass's frequency by a table from 1 to
    64 (six octaves, 200 Hz to 12.8 kHz) opens it at the attack and closes it over the decay; an ENV_DECAY knob
    sets the decay (at note-on)."""
    bind_knobs(lib)
    noise_file(tmp_path)
    ui = ('<control parameterName="Decay" minValue="0" maxValue="1" value="0">'
          '<binding type="modulator" level="instrument" modulatorIndex="0" parameter="ENV_DECAY" translation="linear" '
          'translationOutputMin="0.2" translationOutputMax="5"/></control>')
    path = write_ds(tmp_path / "e.dspreset", preset(group("n.wav", inner=LP200), ui=ui, modulators=ENV % "0.2"))
    no_ignored(lib, path)
    p = Player(lib, tmp_path, path)
    try:
        y = held(p, 0.8)
        early, late = spectrum_db(y[int(0.005 * OUT):int(0.05 * OUT)], 3000, 6000), spectrum_db(y[int(0.5 * OUT):int(0.7 * OUT)], 3000, 6000)
        assert early - late > 25, (early, late)
        lib.smp_synth_knob_set(p.s, knob_index(lib, p.s, "Decay"), 1.0)   # 5 s: still open at 0.5 s
        y = held(p, 0.8)
        late2 = spectrum_db(y[int(0.5 * OUT):int(0.7 * OUT)], 3000, 6000)
        assert late2 - late > 20, (late, late2)
    finally:
        p.free()


def test_ds_midicc_modulator_on_a_group_filter(lib, tmp_path):
    """Mecate's pedal: a <midiCC number="4"> modulator multiplying the lowpass's frequency, 1 at CC 0 to 64 at 127."""
    noise_file(tmp_path)
    path = write_ds(tmp_path / "c.dspreset", preset(group("n.wav", inner=LP200), modulators=CCMOD))
    no_ignored(lib, path)
    p = Player(lib, tmp_path, path)
    try:
        dark = spectrum_db(held(p, 0.5, ["midi 176 4 0 0"])[int(0.2 * OUT):int(0.45 * OUT)], 3000, 6000)
        bright = spectrum_db(held(p, 0.5, ["midi 176 4 127 0"])[int(0.2 * OUT):int(0.45 * OUT)], 3000, 6000)
        assert bright - dark > 25, (dark, bright)
    finally:
        p.free()


def test_ds_knob_on_a_groups_release(lib, tmp_path):
    """Mecate's Sustain fader: type="amp" level="group" ENV_RELEASE sets the group's release, live."""
    bind_knobs(lib)
    sine_file(tmp_path)
    ui = ('<control parameterName="Sustain" minValue="0" maxValue="1" value="0">'
          '<binding type="amp" level="group" groupIndex="0" parameter="ENV_RELEASE" translation="table" '
          'translationTable="0,0.02;1,2"/></control>')
    path = write_ds(tmp_path / "r.dspreset", preset(group(), ui=ui))
    text = no_ignored(lib, path)
    assert "bind group_env param=release" in text, text
    p = Player(lib, tmp_path, path)
    try:
        def tail():
            y = p.render([on(69, 127, 0), off(69, 0.3)], 0.8)
            return tone(y[int(0.45 * OUT):int(0.6 * OUT)], 440.0), tone(y[int(0.1 * OUT):int(0.25 * OUT)], 440.0)
        short, ref = tail()
        assert short < ref * 1e-3, (short, ref)
        lib.smp_synth_knob_set(p.s, knob_index(lib, p.s, "Sustain"), 1.0)
        long_, ref = tail()
        assert long_ > ref * 0.2, (long_, ref)
    finally:
        p.free()


def test_ds_mecate_like_preset_reads_whole(lib, tmp_path):
    """Mecate's layout in small: a master gain fader, layer buttons, a tagged bus fader, a group's two filters with
    a contour knob, the brass envelope and pedal on them (one on, one off, swapped by a button), a vibrato LFO and a
    group release knob: nothing is left out."""
    noise_file(tmp_path)
    brass = group("n.wav", attrs='tags="brass" enabled="false" loopDuringRelease="false"', inner=(
        '<effects><effect type="lowpass" frequency="22000" resonance="0.7" tags="brass_vcf"/>'
        '<effect type="lowpass" frequency="22000" resonance="0.7" tags="brass_vcf"/></effects>'))
    bus = group("n.wav", attrs='tags="pianobus"')
    ui = ('<control parameterName="Master" value="0.7"><binding type="effect" level="instrument" effectIndex="0" parameter="LEVEL" '
          'translation="linear" translationOutputMin="0" translationOutputMax="2.6"/></control>'
          '<control parameterName="Piano" value="0.85"><binding type="amp" level="tag" identifier="pianobus" parameter="AMP_VOLUME" '
          'translation="table" translationTable="0,0;0.5,0.09;1,1"/></control>'
          '<control parameterName="Contour" value="1"><binding type="effect" level="group" groupIndex="0" effectIndex="0" '
          'parameter="FX_FILTER_FREQUENCY" translation="table" translationTable="0,31.89;1,22000"/>'
          '<binding type="effect" level="group" groupIndex="0" effectIndex="1" parameter="FX_FILTER_RESONANCE" '
          'translation="linear" translationOutputMin="0.7" translationOutputMax="3.7"/></control>'
          '<button value="0" tooltip="Brass"><state name="Off"><binding type="general" level="group" groupIndex="0" parameter="ENABLED" '
          'translation="fixed_value" translationValue="false"/></state><state name="On"><binding type="general" level="group" '
          'groupIndex="0" parameter="ENABLED" translation="fixed_value" translationValue="true"/></state></button>'
          '<button value="0" tooltip="Pedal"><state name="Off">'
          '<binding type="modulator" level="instrument" modulatorIndex="0" parameter="ENABLED" translation="fixed_value" translationValue="true"/>'
          '<binding type="modulator" level="instrument" modulatorIndex="1" parameter="ENABLED" translation="fixed_value" translationValue="false"/>'
          '</state><state name="On">'
          '<binding type="modulator" level="instrument" modulatorIndex="0" parameter="ENABLED" translation="fixed_value" translationValue="false"/>'
          '<binding type="modulator" level="instrument" modulatorIndex="1" parameter="ENABLED" translation="fixed_value" translationValue="true"/>'
          '</state></button>'
          '<control parameterName="Attack" value="0"><binding type="modulator" level="instrument" modulatorIndex="0" parameter="ENV_ATTACK" '
          'translation="table" translationTable="0,0.05;1,5"/></control>'
          '<control parameterName="Sustain" value="0.75"><binding type="amp" level="group" groupIndex="0" parameter="ENV_RELEASE" '
          'translation="table" translationTable="0,0.06;1,4"/></control>') + LFO_UI.replace('modulatorIndex="0"', 'modulatorIndex="2"')
    mods = (ENV % "19.4").replace("effectIndex=\"0\"", "effectIndex=\"1\"") + CCMOD.replace('enabled="true"', 'enabled="false"') + LFO % "false"
    path = write_ds(tmp_path / "m.dspreset", preset(brass + bus, ui=ui, effects='<effect type="gain" levelUnit="linear" level="1.82"/>',
                                                    modulators=mods))
    text = no_ignored(lib, path)
    for want in ("fx 0 gain", "level=1.82", "bind group_en", "bind group_fx fx=0 param=freq", "bind group_fx fx=1 param=q",
                 "bind mod 1 param=enabled", "bind mod 2 param=freq", "bind group_env param=release", "mod 1 enabled=0"):
        assert want in text, (want, text)
    f = opened(lib, path)
    try:
        r = regions(lib, f, 0)
        assert len(r) == 2 and r[0].cond.layer == 1 and r[1].cond.layer == 2 and r[0].gen[54] == 3
        assert r[0].cond.nflex == 3                               # the envelope, the pedal and the vibrato
    finally:
        lib.smp_sf_close(f)
