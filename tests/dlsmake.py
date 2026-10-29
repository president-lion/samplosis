"""Writes small DLS (Downloadable Sounds level 1 and 2) files for the tests.

write_dls(path, instruments, waves, ptbl=True)

waves: a list of dicts
  data: a numpy array: int16 (16-bit PCM), uint8 (8-bit PCM, unsigned as DLS has it), or int16 with
        adpcm=True (written as Microsoft ADPCM through soundfile);
  rate; optional name; optional wsmp (see below) written inside the wave.
instruments: a list of dicts
  name, program, bank (ulBank's MSB, default 0), lsb (default 0), drum (bit 31, default False),
  optional art (connection blocks for the instrument), level (1 writes lart/art1, 2 writes lar2/art2; default 1),
  optional cdl (condition ops, see below) as the first chunk of the instrument,
  regions: a list of dicts
    keys (lo, hi), vels (lo, hi), wave (index into the wave pool table), optional keygroup, options (fusOptions),
    channel (wlnk ulChannel, default 1), wsmp (overrides the wave's), art and level (region articulation),
    cdl (condition ops, the region's first chunk).
wsmp: dict(unity=60, fine=0 (cents), gain=0 (centibels; negative is quieter), loop=None or (start, length, type))
A connection block is a tuple (source, control, destination, transform, scale), scale in the base unit (timecents,
cents, centibels, 0.1 %...): it is written as round(scale * 65536); ("raw", value) writes lScale as it is.
cdl ops: a list of tuples: ("const", value), ("query", guid_bytes), ("supported", guid_bytes), or (opcode,).
"""
import io
import struct
import uuid

import numpy as np

# DLS2 query GUIDs (dls2.h)
GUID_GM_IN_HARDWARE = uuid.UUID("178f2f24-c364-11d1-a760-0000f875ac12").bytes_le
GUID_SUPPORTS_DLS1 = uuid.UUID("178f2f27-c364-11d1-a760-0000f875ac12").bytes_le
GUID_SUPPORTS_DLS2 = uuid.UUID("f14599e5-4689-11d2-afa6-00aa0024d8b6").bytes_le
CDL_AND, CDL_OR, CDL_NOT, CDL_EQ = 0x1, 0x2, 0xF, 0xE

# connection sources and destinations
SRC_NONE, SRC_LFO, SRC_VELOCITY, SRC_KEY, SRC_EG2, SRC_CC1 = 0x0, 0x1, 0x2, 0x3, 0x5, 0x81
DST_GAIN, DST_PITCH, DST_PAN = 0x1, 0x3, 0x4
DST_LFO_FREQ, DST_LFO_DELAY = 0x104, 0x105
DST_EG1_ATTACK, DST_EG1_DECAY, DST_EG1_RELEASE, DST_EG1_SUSTAIN = 0x206, 0x207, 0x209, 0x20A
DST_EG1_DELAY, DST_EG1_HOLD = 0x20B, 0x20C
DST_EG2_ATTACK, DST_EG2_DECAY, DST_EG2_RELEASE, DST_EG2_SUSTAIN = 0x30A, 0x30B, 0x30D, 0x30E
DST_FILTER_CUTOFF, DST_FILTER_Q = 0x500, 0x501


def _chunk(cid, data):
    out = cid + struct.pack("<I", len(data)) + data
    if len(data) & 1:
        out += b"\0"
    return out


def _list(kind, chunks):
    return _chunk(b"LIST", kind + b"".join(chunks))


def _info(name):
    if not name:
        return []
    return [_list(b"INFO", [_chunk(b"INAM", name.encode("latin-1") + b"\0")])]


def _wsmp(w):
    loop = w.get("loop")
    gain = int(round(w.get("gain", 0) * 65536))
    out = struct.pack("<IHhiII", 20, w.get("unity", 60), w.get("fine", 0), gain, 0, 1 if loop else 0)
    if loop:
        out += struct.pack("<IIII", 16, loop[2] if len(loop) > 2 else 0, loop[0], loop[1])
    return _chunk(b"wsmp", out)


def _scale(v):
    if isinstance(v, tuple) and v[0] == "raw":
        return v[1]
    return int(round(v * 65536))


def _art(blocks, level):
    data = struct.pack("<II", 8, len(blocks))
    for src, ctl, dst, trn, scale in blocks:
        data += struct.pack("<HHHHi", src, ctl, dst, trn, _scale(scale))
    if level == 2:
        return _list(b"lar2", [_chunk(b"art2", data)])
    return _list(b"lart", [_chunk(b"art1", data)])


def _cdl(ops):
    data = b""
    for op in ops:
        if op[0] == "const":
            data += struct.pack("<HI", 0x10, op[1])
        elif op[0] == "query":
            data += struct.pack("<H", 0x11) + op[1]
        elif op[0] == "supported":
            data += struct.pack("<H", 0x12) + op[1]
        else:
            data += struct.pack("<H", op[0])
    return _chunk(b"cdl ", data)


def adpcm_chunks(data, rate):
    """Microsoft ADPCM through soundfile: the fmt, fact and data chunks of the WAV it writes."""
    import soundfile as sf
    buf = io.BytesIO()
    sf.write(buf, np.asarray(data, dtype=np.int16), rate, format="WAV", subtype="MS_ADPCM")
    b = buf.getvalue()
    pos, out = 12, []
    while pos + 8 <= len(b):
        cid, n = b[pos:pos + 4], struct.unpack("<I", b[pos + 4:pos + 8])[0]
        if cid in (b"fmt ", b"fact", b"data"):
            out.append(_chunk(cid, b[pos + 8:pos + 8 + n]))
        pos += 8 + n + (n & 1)
    return out


def _wave(w):
    data = w["data"]
    rate = w["rate"]
    if w.get("adpcm"):
        body = adpcm_chunks(data, rate)
    elif data.dtype == np.uint8:
        body = [_chunk(b"fmt ", struct.pack("<HHIIHH", 1, 1, rate, rate, 1, 8)), _chunk(b"data", data.tobytes())]
    else:
        d = np.asarray(data, dtype=np.int16)
        body = [_chunk(b"fmt ", struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)), _chunk(b"data", d.tobytes())]
    if w.get("wsmp"):
        body.append(_wsmp(w["wsmp"]))
    return _list(b"wave", body + _info(w.get("name")))


def _region(r):
    lo, hi = r.get("keys", (0, 127))
    vlo, vhi = r.get("vels", (0, 127))
    level = r.get("level", 1)
    chunks = []
    if r.get("cdl"):
        chunks.append(_cdl(r["cdl"]))
    chunks.append(_chunk(b"rgnh", struct.pack("<HHHHHH", lo, hi, vlo, vhi, r.get("options", 0), r.get("keygroup", 0))))
    if r.get("wsmp"):
        chunks.append(_wsmp(r["wsmp"]))
    chunks.append(_chunk(b"wlnk", struct.pack("<HHII", 0, 0, r.get("channel", 1), r["wave"])))
    if r.get("art"):
        chunks.append(_art(r["art"], level))
    return _list(b"rgn2" if level == 2 else b"rgn ", chunks)


def _instrument(ins):
    ul_bank = (ins.get("bank", 0) & 127) << 8 | (ins.get("lsb", 0) & 127)
    if ins.get("drum"):
        ul_bank |= 0x80000000
    chunks = []
    if ins.get("cdl"):
        chunks.append(_cdl(ins["cdl"]))
    chunks.append(_chunk(b"insh", struct.pack("<III", len(ins["regions"]), ul_bank, ins["program"])))
    chunks.append(_list(b"lrgn", [_region(r) for r in ins["regions"]]))
    if ins.get("art"):
        chunks.append(_art(ins["art"], ins.get("level", 1)))
    return _list(b"ins ", chunks + _info(ins.get("name")))


def write_dls(path, instruments, waves, ptbl=True):
    pool = [_wave(w) for w in waves]
    offsets, pos = [], 0
    for w in pool:
        offsets.append(pos)
        pos += len(w)
    body = [_chunk(b"colh", struct.pack("<I", len(instruments))),
            _chunk(b"vers", struct.pack("<II", 0x00010000, 0)),
            _list(b"lins", [_instrument(i) for i in instruments])]
    if ptbl:
        body.append(_chunk(b"ptbl", struct.pack("<II", 8, len(offsets)) + b"".join(struct.pack("<I", o) for o in offsets)))
    body.append(_list(b"wvpl", pool))
    body += _info("Test bank")
    data = b"DLS " + b"".join(body)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(data)) + data)
