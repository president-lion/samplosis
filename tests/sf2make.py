"""Writes small SoundFont files for the tests: SF2, SF3 (Ogg Vorbis samples), SF4 (FLAC samples), SF2Pack (the
whole smpl chunk as one compressed stream) and SoundFont 1 (.sbk).

write_sf2(path, presets, samples, sf3=False, sf4=False, pack=None)
  sf4: True writes each sample as its own FLAC stream flagged 0x20 with ifil 4 (Cognitone's sf2convert);
       0x10 writes FLAC flagged 0x10 with ifil 3 (FLAC inside SF3, as the KKQ fork of sf2convert writes it).
  pack: "flac", "vorbis", "opus" or "wavpack": the RIFF form becomes sfpk and smpl holds the whole SF2 sample
        data (16-bit mono) as one stream in that codec, as BASSMIDI's packer writes it. wavpack needs ffmpeg.
write_sbk(path, presets, samples, rom=()): see its own comment.

samples: a list of dicts
  name, data (int16 numpy array), rate, pitch (root key), correction (cents),
  loop (start, end) in frames from the sample's start, or None;
  optional type (default 1, mono) and link (default 0).
presets: a list of dicts
  name, bank, program, gens ({gen id: value}, preset level), pmods (list of modulators),
  instruments: a list of dicts
    name, gens and mods (the instrument's global zone), zones,
    optional pgens and pmods (the preset zone that points at this instrument),
    zones: a list of dicts
      keys (lo, hi), vels (lo, hi), sample (index into samples), gens, mods.
      keys or vels None leaves that range out of the zone.
A modulator is a tuple (src, dest, amount, amt_src, trans).
A generator value is an int, or a (lo, hi) tuple for a range.
The preset's own gens and pmods go in a global preset zone; it is only written when there are any.
junk_pgen and junk_igen: (oper, value) generator records written after the last zone's, in a bag no preset or
instrument owns, so nothing reaches them (real fonts sometimes carry junk there).
"""
import io
import struct

import numpy as np

GEN_KEYRANGE, GEN_VELRANGE, GEN_INSTRUMENT, GEN_SAMPLEID = 43, 44, 41, 53
PAD_FRAMES = 46  # the spec's zero frames after every sample


def _name(s):
    b = s.encode("latin-1")[:20]
    return b + b"\0" * (20 - len(b))


def _chunk(cid, data):
    out = cid + struct.pack("<I", len(data)) + data
    if len(data) & 1:
        out += b"\0"
    return out


def _list(kind, chunks):
    return _chunk(b"LIST", kind + b"".join(chunks))


def _gen(oper, value):
    if isinstance(value, (tuple, list)):
        return struct.pack("<HBB", oper, value[0], value[1])
    return struct.pack("<Hh", oper, value) if value < 0 else struct.pack("<HH", oper, value)


def _gens(gens, keys=None, vels=None, last=None):
    """A zone's generators in the order the spec asks: key range, velocity range, the rest, then the
    instrument or sample generator."""
    gens = dict(gens or {})
    out = []
    if keys is None:
        keys = gens.pop(GEN_KEYRANGE, None)
    else:
        gens.pop(GEN_KEYRANGE, None)
    if vels is None:
        vels = gens.pop(GEN_VELRANGE, None)
    else:
        gens.pop(GEN_VELRANGE, None)
    if keys is not None:
        out.append(_gen(GEN_KEYRANGE, keys))
    if vels is not None:
        out.append(_gen(GEN_VELRANGE, vels))
    for oper in sorted(gens):
        out.append(_gen(oper, gens[oper]))
    if last is not None:
        out.append(_gen(*last))
    return out


def _mods(mods):
    return [struct.pack("<HHhHH", *m) for m in (mods or [])]


def _ogg(data, rate):
    buf = io.BytesIO()
    import soundfile as sf
    sf.write(buf, np.asarray(data, dtype=np.int16), rate, format="OGG", subtype="VORBIS")
    return buf.getvalue()


def _flac(data, rate):
    buf = io.BytesIO()
    import soundfile as sf
    sf.write(buf, np.asarray(data, dtype=np.int16), rate, format="FLAC", subtype="PCM_16")
    return buf.getvalue()


def pack_stream(data, codec):
    """The whole sample data (int16 mono) as one stream in codec, the way BASSMIDI packs it (44100 Hz, or 48000 Hz
    for Opus)."""
    import soundfile as sf
    data = np.asarray(data, dtype=np.int16)
    buf = io.BytesIO()
    if codec == "flac":
        sf.write(buf, data, 44100, format="FLAC", subtype="PCM_16")
    elif codec == "vorbis":
        sf.write(buf, data, 44100, format="OGG", subtype="VORBIS")
    elif codec == "opus":
        sf.write(buf, data, 48000, format="OGG", subtype="OPUS")
    elif codec == "wavpack":
        import subprocess
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            raw, wv = d + "/in.raw", d + "/out.wv"
            data.tofile(raw)
            subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-f", "s16le", "-ar", "44100", "-ac", "1",
                            "-i", raw, "-c:a", "wavpack", wv], check=True)
            return open(wv, "rb").read()
    else:
        raise ValueError(codec)
    return buf.getvalue()


def write_sf2(path, presets, samples, sf3=False, junk_pgen=(), junk_igen=(), sf4=False, pack=None):
    # sample data and headers
    smpl = bytearray()
    shdr = []
    for s in samples:
        data = np.asarray(s["data"], dtype=np.int16)
        loop = s.get("loop")
        stype = s.get("type", 1)
        if sf3 or sf4:
            start = len(smpl)
            smpl += _flac(data, s["rate"]) if sf4 else _ogg(data, s["rate"])
            end = len(smpl)
            ls, le = (loop if loop else (0, 0))     # SF3: loop points count frames from the sample's start
            stype |= 0x20 if sf4 is True else 0x10
        else:
            start = len(smpl) // 2
            smpl += data.tobytes() + b"\0" * (2 * PAD_FRAMES)
            end = start + len(data)
            ls, le = ((start + loop[0], start + loop[1]) if loop else (start, start))
        shdr.append(_name(s["name"]) + struct.pack("<IIIIIBbHH", start, end, ls, le, s["rate"], s["pitch"],
                                                   s.get("correction", 0), s.get("link", 0), stype))
    shdr.append(_name("EOS") + b"\0" * 26)

    # instruments: one record per preset zone that points at one
    inst, ibag, imod, igen = [], [], [], []
    phdr, pbag, pmod, pgen = [], [], [], []
    for p in presets:
        phdr.append(_name(p["name"]) + struct.pack("<HHHIII", p["program"], p["bank"], len(pbag), 0, 0, 0))
        if p.get("gens") or p.get("pmods"):
            pbag.append(struct.pack("<HH", len(pgen), len(pmod)))
            pgen += _gens(p.get("gens"))
            pmod += _mods(p.get("pmods"))
        for ins in p.get("instruments", []):
            pbag.append(struct.pack("<HH", len(pgen), len(pmod)))
            pgen += _gens(ins.get("pgens"), last=(GEN_INSTRUMENT, len(inst)))
            pmod += _mods(ins.get("pmods"))
            inst.append(_name(ins["name"]) + struct.pack("<H", len(ibag)))
            if ins.get("gens") or ins.get("mods"):
                ibag.append(struct.pack("<HH", len(igen), len(imod)))
                igen += _gens(ins.get("gens"))
                imod += _mods(ins.get("mods"))
            for z in ins.get("zones", []):
                ibag.append(struct.pack("<HH", len(igen), len(imod)))
                keys, vels = z.get("keys", (0, 127)), z.get("vels", (0, 127))   # None: not written
                igen += _gens(z.get("gens"), keys=None if keys is None else tuple(keys),
                              vels=None if vels is None else tuple(vels),
                              last=(GEN_SAMPLEID, z["sample"]))
                imod += _mods(z.get("mods"))
    # the terminal records. Junk goes in a bag of its own after the last zone's: EOP and EOI point at that bag,
    # so no preset or instrument owns it, and the final bag still points at the terminal generator.
    phdr.append(_name("EOP") + struct.pack("<HHHIII", 0, 0, len(pbag), 0, 0, 0))
    if junk_pgen:
        pbag.append(struct.pack("<HH", len(pgen), len(pmod)))
        pgen += [_gen(*g) for g in junk_pgen]
    pbag.append(struct.pack("<HH", len(pgen), len(pmod)))
    pmod.append(b"\0" * 10)
    pgen.append(b"\0" * 4)
    inst.append(_name("EOI") + struct.pack("<H", len(ibag)))
    if junk_igen:
        ibag.append(struct.pack("<HH", len(igen), len(imod)))
        igen += [_gen(*g) for g in junk_igen]
    ibag.append(struct.pack("<HH", len(igen), len(imod)))
    imod.append(b"\0" * 10)
    igen.append(b"\0" * 4)

    if pack:
        smpl = pack_stream(np.frombuffer(bytes(smpl), dtype=np.int16), pack)
    info = _list(b"INFO", [_chunk(b"ifil", struct.pack("<HH", 4 if sf4 is True else 3 if (sf3 or sf4) else 2, 1)),
                           _chunk(b"isng", b"EMU8000\0"),
                           _chunk(b"INAM", b"Samplosis test font\0")])
    sdta = _list(b"sdta", [_chunk(b"smpl", bytes(smpl))])
    pdta = _list(b"pdta", [_chunk(cid, b"".join(recs)) for cid, recs in (
        (b"phdr", phdr), (b"pbag", pbag), (b"pmod", pmod), (b"pgen", pgen), (b"inst", inst),
        (b"ibag", ibag), (b"imod", imod), (b"igen", igen), (b"shdr", shdr))])
    body = (b"sfpk" if pack else b"sfbk") + info + sdta + pdta
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def write_sbk(path, presets, samples, rom=()):
    """A SoundFont 1 (.sbk) file, laid out as awesfx reads one: ifil 1, sample names in an snam chunk inside sdta,
    16-byte shdr records (start, end, loop start, loop end in frames from smpl's start; no rate, root or type), no
    modulators and no terminal sample record. The generators (presets as for write_sf2, without pmods and mods)
    are written as given, so they are in SBK units: times in milliseconds, pan 0-127, sustain 0-127 and so on; the
    root key and tuning are generator 55 (samplePitch, root x 100 minus the cents).
    rom: ROM samples, (name, start, end) in the sound card's memory; they come first, before the samples in the file
    (the first sample in the file starts at 0, which is how a reader tells where ROM ends). Instrument zones refer to
    samples by index in the shdr list, so the samples in the file are numbered after the ROM ones."""
    smpl = bytearray()
    snam, shdr = [], []
    for name, start, end in rom:
        snam.append(_name(name))
        shdr.append(struct.pack("<IIII", start, end, start, end))
    for s in samples:
        data = np.asarray(s["data"], dtype=np.int16)
        loop = s.get("loop")
        start = len(smpl) // 2
        smpl += data.tobytes() + b"\0" * (2 * PAD_FRAMES)
        end = start + len(data)
        ls, le = ((start + loop[0], start + loop[1]) if loop else (start, start))
        snam.append(_name(s["name"]))
        shdr.append(struct.pack("<IIII", start, end, ls, le))
    inst, ibag, igen = [], [], []
    phdr, pbag, pgen = [], [], []
    for p in presets:
        phdr.append(_name(p["name"]) + struct.pack("<HHHIII", p["program"], p["bank"], len(pbag), 0, 0, 0))
        if p.get("gens"):
            pbag.append(struct.pack("<HH", len(pgen), 0))
            pgen += _gens(p.get("gens"))
        for ins in p.get("instruments", []):
            pbag.append(struct.pack("<HH", len(pgen), 0))
            pgen += _gens(ins.get("pgens"), last=(GEN_INSTRUMENT, len(inst)))
            inst.append(_name(ins["name"]) + struct.pack("<H", len(ibag)))
            if ins.get("gens"):
                ibag.append(struct.pack("<HH", len(igen), 0))
                igen += _gens(ins.get("gens"))
            for z in ins.get("zones", []):
                ibag.append(struct.pack("<HH", len(igen), 0))
                igen += _gens(z.get("gens"), keys=tuple(z.get("keys", (0, 127))), vels=tuple(z.get("vels", (0, 127))),
                              last=(GEN_SAMPLEID, z["sample"]))
    phdr.append(_name("EOP") + struct.pack("<HHHIII", 0, 0, len(pbag), 0, 0, 0))
    pbag.append(struct.pack("<HH", len(pgen), 0))
    pgen.append(b"\0" * 4)
    inst.append(_name("EOI") + struct.pack("<H", len(ibag)))
    ibag.append(struct.pack("<HH", len(igen), 0))
    igen.append(b"\0" * 4)
    info = _list(b"INFO", [_chunk(b"ifil", struct.pack("<HH", 1, 0)), _chunk(b"isng", b"EMU8000\0"),
                           _chunk(b"irom", b"1MGM\0"), _chunk(b"INAM", b"Samplosis test bank\0")])
    sd = [_chunk(b"snam", b"".join(snam))]
    if smpl:
        sd.append(_chunk(b"smpl", bytes(smpl)))
    sdta = _list(b"sdta", sd)
    pdta = _list(b"pdta", [_chunk(cid, b"".join(recs)) for cid, recs in (
        (b"phdr", phdr), (b"pbag", pbag), (b"pgen", pgen), (b"inst", inst), (b"ibag", ibag), (b"igen", igen),
        (b"shdr", shdr))])
    body = b"sfbk" + info + sdta + pdta
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)
