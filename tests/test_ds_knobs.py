"""Decent Sampler knobs: a preset's <ui> controls with their bindings, its <effects> chain and its <midi> CC
bindings are read onto the font (Task 3), play through the player (Tasks 4 and 6) and are read and set by the
window and the state. The kit here is modelled on Finnish Forest Percussion (whose own samples never enter the
tests): three tagged layers on one key, and the same seven controls, effects and CC bindings."""
import ctypes

import numpy as np

from test_ds import write_ds
from test_sf2 import mods, regions
from test_sfz import RATE, ignored, opened, sine, write_wav

CONTROLS = """
  <ui width="812" height="375" layoutMode="relative" bgImage="Images/bg_ffp.jpg">
    <keyboard><color loNote="35" hiNote="40" color="387496"/></keyboard>
    <tab>
      <control x="117" y="78" style="custom_skin_vertical_drag" tags="tone" defaultValue="0.5"
               parameterName="Tone" label="Tone" type="float" minValue="0" maxValue="1" value="0.5">
        <binding type="effect" level="instrument" effectIndex="0" parameter="FX_FILTER_FREQUENCY"
                 translation="table" translationTable="0.0,10;0.5,10;0.80,300;1,850"/>
        <binding type="effect" level="instrument" effectIndex="1" parameter="FX_FILTER_FREQUENCY"
                 translation="table" translationTable="0,250;0.25,1600;0.5,22000;1.0,22000"/>
      </control>
      <control x="216" tags="pitch" defaultValue="0" parameterName="Pitch" label="Pitch" type="float"
               valueType="integer" minValue="-24" maxValue="24" value="0">
        <binding type="amp" level="instrument" parameter="GLOBAL_TUNING" translation="linear"/>
      </control>
      <control x="314" label="Texture" tags="texture" type="float" defaultValue="0" minValue="0.0" maxValue="1" value="0">
        <binding type="amp" level="group" tags="Texture" parameter="AMP_VOLUME"
                 translation="linear" translationOutputMin="0" translationOutputMax="1.0"/>
      </control>
      <control x="428" label="Force" tags="force" type="float" defaultValue="0" minValue="0.0" maxValue="1" value="0">
        <binding type="amp" level="group" tags="Natural" parameter="AMP_VOLUME"
                 translation="linear" translationOutputMin="1.0" translationOutputMax="0"/>
        <binding type="amp" level="group" tags="Force" parameter="AMP_VOLUME"
                 translation="linear" translationOutputMin="0" translationOutputMax="1.0"/>
      </control>
      <control x="557" label="Reverb" tags="reverb" type="float" defaultValue="0" minValue="0" maxValue="1" value="0">
        <binding type="effect" level="instrument" position="2" parameter="FX_REVERB_WET_LEVEL"/>
      </control>
      <control x="633" label="Delay Mix" tags="delayMix" type="float" defaultValue="0" minValue="0.0" maxValue="1" value="0">
        <binding type="effect" level="instrument" position="3" parameter="FX_WET_LEVEL"/>
      </control>
      <control x="715" label="Delay Time" tags="delayTime" valueType="musical_time" minValue="6" maxValue="16"
               value="13" defaultValue="13">
        <binding type="effect" level="instrument" position="3" parameter="FX_DELAY_TIME"/>
      </control>
    </tab>
  </ui>"""

EFFECTS = """
  <effects>
    <effect type="highpass" resonance="0.7" frequency="10"/>
    <effect type="lowpass" resonance="0.7" frequency="22000"/>
    <effect type="reverb" roomSize="0.9" damping="0.1" wetLevel="0"/>
    <effect type="delay" delayTimeFormat="musical_time" delayTime="0.5" stereoOffset="0.01" feedback="0.65" wetLevel="0.5"/>
  </effects>"""

MIDI = """
  <midi>
    <cc number="11"><binding level="ui" type="control" controlTags="force" parameter="VALUE" translation="linear"
        translationOutputMin="0" translationOutputMax="1"/></cc>
    <cc number="74"><binding level="ui" type="control" controlTags="tone" parameter="VALUE" translation="linear"
        translationOutputMin="0" translationOutputMax="1"/></cc>
    <cc number="1"><binding level="ui" type="control" controlTags="pitch" parameter="VALUE" translation="linear"
        translationOutputMin="-24"translationOutputMax="12"/></cc>
    <cc number="17"><binding level="ui" type="control" controlTags="texture" parameter="VALUE" translation="linear"
        translationOutputMin="0" translationOutputMax="1"/></cc>
    <cc number="91"><binding level="ui" type="control" controlTags="reverb" parameter="VALUE" translation="linear"
        translationOutputMin="0" translationOutputMax="1"/></cc>
    <cc number="12"><binding level="ui" type="control" controlTags="delayMix" parameter="VALUE" translation="linear"
        translationOutputMin="0" translationOutputMax="1"/></cc>
    <cc number="19"><binding level="ui" type="control" controlTags="delayTime" parameter="VALUE" translation="linear"
        translationOutputMin="6" translationOutputMax="16"/></cc>
  </midi>"""

LAYERS = (("Natural", 300.0), ("Force", 500.0), ("Texture", 700.0))


def kit_text(ui=CONTROLS, effects=EFFECTS, midi=MIDI, group_attrs=("", "", ""), key=60, samples=None):
    groups = ""
    for (tag, fq), extra in zip(LAYERS, group_attrs):
        name = (samples or {}).get(tag, "%s.wav" % tag.lower())
        groups += ('    <group name="%s" tags="%s" release="0.05" ampVelTrack="1" %s>\n'
                   '      <sample path="%s" rootNote="%d" loNote="%d" hiNote="%d"/>\n    </group>\n'
                   % (tag, tag, extra, name, key, key, key))
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<DecentSampler pluginVersion="1">%s\n  <groups>\n%s  </groups>%s%s\n'
            '</DecentSampler>\n' % (ui, groups, effects, midi))


def write_layers(tmp_path, wave=None):
    for tag, fq in LAYERS:
        write_wav(tmp_path / ("%s.wav" % tag.lower()), wave(fq) if wave else sine(RATE, fq))


def make_kit(tmp_path, name="Kit.dspreset", **kw):
    write_layers(tmp_path)
    return write_ds(tmp_path / name, kit_text(**kw))


def knobs_text(lib, f, preset=0):
    buf = ctypes.create_string_buffer(1 << 16)
    lib.smp_sf_test_knobs_text.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
    lib.smp_sf_test_knobs_text.restype = ctypes.c_int
    n = lib.smp_sf_test_knobs_text(f, preset, buf, 1 << 16)
    return buf.value.decode() if n > 0 else ""


def test_ds_knobs_parsed(lib, tmp_path):
    f = opened(lib, make_kit(tmp_path))
    t = knobs_text(lib, f).splitlines()
    knobs = [x for x in t if x.startswith("knob ")]
    assert knobs == ['knob 1 "Tone" float 0 1 0.5 percent', 'knob 2 "Pitch" int -24 24 0 semitones',
                     'knob 3 "Texture" float 0 1 0 percent', 'knob 4 "Force" float 0 1 0 percent',
                     'knob 5 "Reverb" float 0 1 0 percent', 'knob 6 "Delay Mix" float 0 1 0 percent',
                     'knob 7 "Delay Time" mtime 6 16 13 mtime'], t
    s = "\n".join(t)
    assert "  bind fx fx=0 param=freq layers= table 0:10,0.5:10,0.8:300,1:850" in s, s
    assert "  bind fx fx=1 param=freq layers= table 0:250,0.25:1600,0.5:22000,1:22000" in s
    assert "  bind global_tune fx=0 param=none layers= linear -24 24" in s
    assert "  bind group_vol fx=0 param=none layers=3 linear 0 1" in s
    assert "  bind group_vol fx=0 param=none layers=1 linear 1 0" in s
    assert "  bind group_vol fx=0 param=none layers=2 linear 0 1" in s
    assert "  bind fx fx=2 param=wet layers= linear 0 1" in s
    assert "  bind fx fx=3 param=wet layers= linear 0 1" in s
    assert "  bind fx fx=3 param=time layers= linear 6 16" in s
    fx = [x for x in t if x.startswith("fx ")]
    assert fx[0].startswith("fx 0 hp freq=10 q=0.7 ") and fx[1].startswith("fx 1 lp freq=22000 q=0.7 ")
    assert fx[2].startswith("fx 2 reverb ") and "room=0.9 damp=0.1 wet=0 " in fx[2] + " "
    assert fx[3].startswith("fx 3 delay ") and "wet=0.5 time=0.5 musical=1 feedback=0.65 offset=0.01" in fx[3]
    ccs = [x for x in t if x.startswith("cc ")]
    assert ccs == ["cc 11 knob=4 linear 0 1", "cc 74 knob=1 linear 0 1", "cc 1 knob=2 linear -24 12",
                   "cc 17 knob=3 linear 0 1", "cc 91 knob=5 linear 0 1", "cc 12 knob=6 linear 0 1",
                   "cc 19 knob=7 linear 6 16"], t
    assert "layers 3" in t
    n, first = ignored(lib, f)
    assert n == 0, first                       # <ui>, <effects> and <midi> are read, not counted
    lib.smp_sf_close(f)


def test_ds_knob_binding_targets(lib, tmp_path):
    ui = """
  <ui><tab>
    <control label="ByIndex" minValue="0" maxValue="1" value="1">
      <binding type="amp" level="group" groupIndex="1" parameter="AMP_VOLUME"/></control>
    <control label="ByPosition" minValue="0" maxValue="1" value="1">
      <binding type="amp" level="group" position="0" parameter="AMP_VOLUME"/></control>
    <control label="TwoTags" minValue="0" maxValue="1" value="1">
      <binding type="amp" level="group" tags="Natural,Texture" parameter="AMP_VOLUME"/></control>
    <labeled-knob label="Master" minValue="0" maxValue="1" value="1">
      <binding type="amp" level="instrument" parameter="AMP_VOLUME"/></labeled-knob>
    <control label="Tune" minValue="-12" maxValue="12" value="0">
      <binding type="amp" level="group" tags="Force" parameter="GROUP_TUNING"/></control>
  </tab></ui>"""
    f = opened(lib, make_kit(tmp_path, ui=ui, effects="", midi=""))
    s = knobs_text(lib, f)
    assert "  bind group_vol fx=0 param=none layers=2 linear 0 1" in s, s
    assert "  bind group_vol fx=0 param=none layers=1 linear 0 1" in s
    assert "  bind group_vol fx=0 param=none layers=1,3 linear 0 1" in s
    assert "  bind global_vol fx=0 param=none layers= linear 0 1" in s
    assert "  bind group_tune fx=0 param=none layers=2 linear -12 12" in s
    lib.smp_sf_close(f)


def test_ds_knob_regions(lib, tmp_path):
    write_layers(tmp_path)
    attrs = ('volume="0.5"', "", "")
    f = opened(lib, write_ds(tmp_path / "k.dspreset", kit_text(group_attrs=attrs)))
    rs = regions(lib, f, 0)
    assert [r.cond.layer for r in rs] == [1, 2, 3]
    assert rs[0].gen[48] == 0                  # the group's 0.5 plays through the layer, which a knob may replace
    lib.smp_sf_close(f)
    f = opened(lib, write_ds(tmp_path / "plain.dspreset", kit_text(ui="", effects="", midi="", group_attrs=attrs)))
    rs = regions(lib, f, 0)
    assert [r.cond.layer for r in rs] == [0, 0, 0]
    assert rs[0].gen[48] > 0                   # baked as today
    assert knobs_text(lib, f) == ""
    lib.smp_sf_close(f)


def test_ds_knob_unsupported_counted(lib, tmp_path):
    write_layers(tmp_path)
    cases = [
        ('<ui><tab><button x="1"><state name="a"/></button></tab></ui>', "<button>"),
        ('<ui><tab><menu x="1"><option name="a"/></menu></tab></ui>', "<menu>"),
        ('<ui><tab><control label="A"><binding type="amp" level="instrument" parameter="ENABLED"/></control></tab></ui>',
         "binding ENABLED"),
        ("<ui><tab>%s</tab></ui>" % "".join('<control label="K%d"/>' % i for i in range(33)), "too many knobs"),
    ]
    for i, (ui, name) in enumerate(cases):
        f = opened(lib, write_ds(tmp_path / ("u%d.dspreset" % i), kit_text(ui=ui, effects="", midi="")))
        n, first = ignored(lib, f)
        assert n >= 1 and first == name, (name, n, first)
        lib.smp_sf_close(f)
    f = opened(lib, write_ds(tmp_path / "fx.dspreset",
                             kit_text(ui="", effects='<effects><effect type="chorus"/></effects>', midi="")))
    assert ignored(lib, f)[1] == "effect chorus"
    lib.smp_sf_close(f)


def cc_sources(r):
    return {m[0] & 0x7F for m in mods(r.imod, r.nimod) if m[0] & 0x80}


def test_ds_knob_default_mods(lib, tmp_path):
    f = opened(lib, make_kit(tmp_path))
    src = cc_sources(regions(lib, f, 0)[0])
    assert 11 not in src and 1 not in src and 91 not in src, src   # bound (and a reverb effect: no shared send)
    assert {7, 10, 93} <= src, src
    lib.smp_sf_close(f)
    f = opened(lib, write_ds(tmp_path / "plain.dspreset", kit_text(ui="", effects="", midi="")))
    assert {7, 10, 11, 91, 93} <= cc_sources(regions(lib, f, 0)[0])
    lib.smp_sf_close(f)
