/* sfdls.c: Downloadable Sounds, level 1 and 2 (RIFF form "DLS "), read into the SoundFont model of sf2.c: each
   instrument becomes a preset, each region a region with final SF2 generator values and an instrument modulator
   list, and each channel of each wave in the wave pool a sample. Only the headers, articulations and wave formats
   are read at open; the wave data stays on disk.

   The layout (the MMA's DLS Level 1 and 2.2 specifications and Microsoft's dls1.h and dls2.h):
     colh, vers, ptbl (the wave pool table: an offset per cue, from the first byte after wvpl's "wvpl" id),
     LIST lins > LIST ins  > insh (regions, ulBank, ulInstrument), LIST lrgn, LIST lart or lar2, LIST INFO
                             LIST lrgn > LIST rgn or rgn2 > rgnh, wsmp, wlnk, LIST lart or lar2
     LIST wvpl > LIST wave > fmt, data, wsmp, LIST INFO
   Any list may hold cdl chunks (DLS2 conditions): a cdl that comes out false hides the chunks after it in its list,
   up to the next cdl, so a false cdl first in an instrument or region leaves it out.

   Samples. A PCM wave (8-bit, which DLS keeps unsigned, 16, 24 or 32-bit, or 32-bit float) is PCM_FILE at its data
   chunk's offset in the DLS file. Anything else (Microsoft or IMA ADPCM, a-law...) is DECODED: its LIST wave chunk
   is the same bytes as a RIFF WAVE file but the two ids, so the loader reads it, changes them and gives it to
   decode_memory (dr_wav). A wave's wsmp gives the sample's root and loop; a region's wsmp, when it has one,
   replaces the wave's for that region (its loop through the loop offset generators).

   Articulation (art1 in lart, art2 in lar2). A region's own articulation replaces the instrument's whole, as the
   specification says. Each connection block (source, control, destination, transform, scale) becomes:
   - with no source and no control: a generator value;
   - key number to pitch: scaleTuning; key number to an envelope's hold or decay: keynumTo...;
   - an LFO or the modulation envelope to pitch, gain or cutoff: modLfoToPitch, modLfoToVolume, modLfoToFilterFc,
     vibLfoToPitch, modEnvToPitch, modEnvToFilterFc (and with a controller, a modulator from that controller to
     the same generator);
   - a MIDI source (velocity, key, pitch wheel, pressure, a controller, the bend range) with or without a control:
     an SF2 modulator, merged into SF2's default list (identical ones replaced).

   The units. lScale is the base unit times 65536; it is rounded to the base unit (scale_of). Then:
   - times: DLS absolute timecents are SF2 timecents. 0x80000000 is "no time", -32768, held at SF2's -12000.
   - LFO frequencies and filter cutoff: DLS absolute pitch cents are SF2 absolute cents (both 8.176 Hz at 0).
     A cutoff of 0x7FFFFFFF ("no filter") is held at SF2's 13500.
   - gain: DLS gain in centibels (wsmp's 1/655360 dB is the same unit), negative quieter. SF2 attenuation is
     positive quieter, and this player multiplies the generator (not the modulators) by 0.4, as FluidSynth does, so
     the generator is -gain / 0.4, and a modulator's amount -gain.
   - pitch: cents, into coarseTune (100s) and fineTune (the rest), or "initial pitch" (59) for modulators, which is
     what SF2's default pitch wheel modulator uses, so DLS's pitch wheel connection replaces it.
   - sustain: DLS gives a level in 0.1 % (1000 is full). The volume envelope's is 1000 - level centibels, as
     SpessaSynth reads it (0 % is 100 dB down, past this player's 96 dB floor, so silent); the modulation
     envelope's is SF2's 0.1 % below full, 1000 - level, exactly.
   - pan and the effect sends: 0.1 % both, as SF2.
   - filter Q: centibels, as SF2.
   - key number to scaleTuning: DLS gives the cents across all 128 keys (12800 is normal), SF2 cents per key: / 128.
   - key number to hold or decay: DLS adds scale x key / 128 timecents, SF2 adds (60 - key) x value. So the SF2
     value is -scale / 128, and the hold or decay itself gains 60 x scale / 128 (only when the articulation sets it,
     as SpessaSynth and Viena read it).
   - DLS1 has one LFO, which both vibrates and tremolos. Its pitch depth (and the CC1 modulator to it) goes to SF2's
     vibrato LFO, which takes the LFO's frequency and delay too; its gain and cutoff depths stay on the modulation
     LFO. DLS2's LFO is SF2's modulation LFO and its vibrato LFO is SF2's.
   - DLS's defaults where they differ from SF2's: LFOs at 5 Hz (-851 cents) after 10 ms (-7973 timecents); the
     reverb and chorus controllers (CC91, CC93) at full send (1000), not SF2's 200.
   - DLS1 articulations give a curve only in the output transform: it is used as the source's curve when the
     source's own is linear. Velocity, CC7 and CC11 to gain always fall as the source rises (SF2's negative
     direction): DLS1 has no invert bit and its concave curve already falls, and some DLS2 files leave the bit out.

   Connection blocks that map to nothing here are ignored and counted in f->dls_ignored:
   - destinations: none, the reserved EG ones (0x208, 0x30C), the EG1 shutdown time (0x20D), key number (5), the
     channel outputs (left, right, centre, LFE, rear: 0x10 to 0x15), and any unknown one;
   - sources: the volume envelope (4), the fine and coarse tune RPNs (0x101, 0x102) and any unknown one; an LFO or
     envelope used as a control; an LFO or envelope as the source to anything but pitch, gain or cutoff;
   - transforms past switch (4 to 15) on a source or control.
   Also not used: rgnh's fusOptions (self non-exclusive is what SF2 does anyway) and usLayer, wsmp's fulOptions,
   wlnk's phase groups, and a wave's second and later loops.

   Damaged files never crash: every chunk size is checked against its list and the file, every index against what
   it points into, and the whole file has caps (waves, regions, modulators) so it can't ask for gigabytes. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sf2.h"

/* SF2 generator ids used here */
enum {
  G_LOOPSTART = 2, G_LOOPEND = 3, G_MODLFO_PITCH = 5, G_VIBLFO_PITCH = 6, G_MODENV_PITCH = 7, G_FC = 8, G_Q = 9,
  G_MODLFO_FC = 10, G_MODENV_FC = 11, G_MODLFO_VOL = 13, G_CHORUS = 15, G_REVERB = 16, G_PAN = 17,
  G_DELAY_MODLFO = 21, G_FREQ_MODLFO = 22, G_DELAY_VIBLFO = 23, G_FREQ_VIBLFO = 24,
  G_DELAY_MODENV = 25, G_ATTACK_MODENV = 26, G_HOLD_MODENV = 27, G_DECAY_MODENV = 28, G_SUSTAIN_MODENV = 29,
  G_RELEASE_MODENV = 30, G_KEY_MODENV_HOLD = 31, G_KEY_MODENV_DECAY = 32,
  G_DELAY_VOLENV = 33, G_ATTACK_VOLENV = 34, G_HOLD_VOLENV = 35, G_DECAY_VOLENV = 36, G_SUSTAIN_VOLENV = 37,
  G_RELEASE_VOLENV = 38, G_KEY_VOLENV_HOLD = 39, G_KEY_VOLENV_DECAY = 40,
  G_KEYRANGE = 43, G_VELRANGE = 44, G_LOOPSTARTCOARSE = 45, G_ATTEN = 48, G_LOOPENDCOARSE = 50,
  G_COARSE = 51, G_FINE = 52, G_MODES = 54, G_SCALE = 56, G_EXCLUSIVE = 57, G_ROOTKEY = 58, G_PITCH = 59
};

/* DLS connection sources and destinations (dls1.h, dls2.h) */
enum {
  S_NONE = 0, S_LFO = 1, S_VELOCITY = 2, S_KEY = 3, S_EG1 = 4, S_EG2 = 5, S_PITCHWHEEL = 6, S_POLYPRESSURE = 7,
  S_CHANPRESSURE = 8, S_VIBRATO = 9, S_CC7 = 0x87, S_CC11 = 0x8B, S_RPN0 = 0x100
};
enum {
  D_GAIN = 1, D_PITCH = 3, D_PAN = 4, D_CHORUS = 0x80, D_REVERB = 0x81,
  D_LFO_FREQ = 0x104, D_LFO_DELAY = 0x105, D_VIB_FREQ = 0x114, D_VIB_DELAY = 0x115,
  D_EG1_ATTACK = 0x206, D_EG1_DECAY = 0x207, D_EG1_RELEASE = 0x209, D_EG1_SUSTAIN = 0x20A, D_EG1_DELAY = 0x20B,
  D_EG1_HOLD = 0x20C,
  D_EG2_ATTACK = 0x30A, D_EG2_DECAY = 0x30B, D_EG2_RELEASE = 0x30D, D_EG2_SUSTAIN = 0x30E, D_EG2_DELAY = 0x30F,
  D_EG2_HOLD = 0x310,
  D_FILTER_CUTOFF = 0x500, D_FILTER_Q = 0x501
};

#define MAX_WAVES 65535              /* samples are counted in 16 bits by the callers' checks */
#define MAX_CHANNELS 8
#define MAX_LINS (256u << 20)        /* the instrument list is read whole; a bigger one is refused */
#define MAX_CUES (1u << 20)
#define MAX_ART 16                   /* art chunks kept per articulation (more are ignored) */
#define MAX_DECODED (1u << 26)       /* a decoded wave's length cap, as sffont.c's */
#define DLS_LFO_HZ_CENTS (-851)      /* 5 Hz: 1200 x log2(5 / 8.176) */
#define DLS_LFO_DELAY_TC (-7973)     /* 10 ms: 1200 x log2(0.01) */

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

typedef struct { HANDLE h; uint64_t size; } Rd;

static int rd(Rd *s, uint64_t off, void *buf, uint32_t n) {
  LARGE_INTEGER li; DWORD got = 0;
  if (off > s->size || n > s->size - off) return 0;
  li.QuadPart = (LONGLONG)off;
  if (!SetFilePointerEx(s->h, li, NULL, FILE_BEGIN)) return 0;
  return ReadFile(s->h, buf, n, &got, NULL) && got == n;
}

/* ---- chunks in memory ---- */

typedef struct { const uint8_t *p; uint32_t n, pos; } It;

/* the next chunk of a list held in memory: its id, data and size; 0 at the end, or at a chunk that runs past the
   list (a damaged list: what came before it still counts) */
static int it_next(It *it, const uint8_t **id, const uint8_t **data, uint32_t *n) {
  uint32_t m;
  if (it->pos > it->n || it->n - it->pos < 8) return 0;
  m = u32(it->p + it->pos + 4);
  if (m > it->n - it->pos - 8) return 0;
  *id = it->p + it->pos; *data = it->p + it->pos + 8; *n = m;
  it->pos += 8 + m;
  if ((m & 1) && it->pos < it->n) it->pos++;
  return 1;
}

static int is_list(const uint8_t *id, const uint8_t *data, uint32_t n, const char *type) {
  return !memcmp(id, "LIST", 4) && n >= 4 && !memcmp(data, type, 4);
}

/* an INFO list's INAM, into out (cap bytes, at least 1) */
static void info_name(const uint8_t *data, uint32_t n, char *out, int cap) {
  It it = {data, n, 0}; const uint8_t *id, *d; uint32_t m;
  while (it_next(&it, &id, &d, &m))
    if (!memcmp(id, "INAM", 4)) {
      int k = 0;
      while (k < cap - 1 && (uint32_t)k < m && d[k]) { out[k] = (char)d[k]; k++; }
      while (k > 0 && out[k - 1] == ' ') k--;
      out[k] = 0;
      return;
    }
}

/* ---- DLS2 conditions (cdl) ---- */

/* the queries this player answers (dls2.h's GUIDs, as stored: the first three fields little-endian). It is a
   software DLS 1 and 2 synthesizer with no General MIDI, GS or XG set of its own. */
typedef struct { uint8_t guid[16]; int32_t value; } Query;
static const Query QUERIES[] = {
  {{0x24,0x2f,0x8f,0x17, 0x64,0xc3, 0xd1,0x11, 0xa7,0x60,0x00,0x00,0xf8,0x75,0xac,0x12}, 0},          /* GMInHardware */
  {{0x25,0x2f,0x8f,0x17, 0x64,0xc3, 0xd1,0x11, 0xa7,0x60,0x00,0x00,0xf8,0x75,0xac,0x12}, 0},          /* GSInHardware */
  {{0x26,0x2f,0x8f,0x17, 0x64,0xc3, 0xd1,0x11, 0xa7,0x60,0x00,0x00,0xf8,0x75,0xac,0x12}, 0},          /* XGInHardware */
  {{0x27,0x2f,0x8f,0x17, 0x64,0xc3, 0xd1,0x11, 0xa7,0x60,0x00,0x00,0xf8,0x75,0xac,0x12}, 1},          /* SupportsDLS1 */
  {{0xe5,0x99,0x45,0xf1, 0x89,0x46, 0xd2,0x11, 0xaf,0xa6,0x00,0xaa,0x00,0x24,0xd8,0xb6}, 1},          /* SupportsDLS2 */
  {{0x28,0x2f,0x8f,0x17, 0x64,0xc3, 0xd1,0x11, 0xa7,0x60,0x00,0x00,0xf8,0x75,0xac,0x12}, 0x7fffffff}, /* SampleMemorySize */
  {{0x81,0x11,0x3e,0xb0, 0x95,0x80, 0xd2,0x11, 0xa1,0xef,0x00,0x60,0x08,0x33,0xdb,0xd8}, 0},          /* ManufacturersID */
  {{0x82,0x11,0x3e,0xb0, 0x95,0x80, 0xd2,0x11, 0xa1,0xef,0x00,0x60,0x08,0x33,0xdb,0xd8}, 0},          /* ProductID */
  {{0x13,0xf7,0x91,0x2a, 0xbf,0xa4, 0xd2,0x11, 0xbb,0xdf,0x00,0x60,0x08,0x33,0xdb,0xd8}, 48000},      /* SamplePlaybackRate */
};

/* a cdl chunk's expression, run on a stack: 1 when it is true. Ops are 16-bit: 1 AND, 2 OR, 3 XOR, 4 ADD,
   5 SUBTRACT, 6 MULTIPLY, 7 DIVIDE, 8 LOGICAL AND, 9 LOGICAL OR, 10 LT, 11 LE, 12 GT, 13 GE, 14 EQ, 15 NOT,
   16 CONST (a 32-bit value follows), 17 QUERY and 18 QUERYSUPPORTED (a GUID follows). An unknown query is 0 and
   unsupported. An expression that can't be worked out (an unknown op, too few values) counts as true, so a damaged
   condition never hides what it guards. */
static int cdl_true(const uint8_t *p, uint32_t n) {
  int32_t st[64]; int sp = 0; uint32_t pos = 0;
  while (pos + 2 <= n) {
    unsigned op = u16(p + pos); pos += 2;
    if (op == 16) {
      if (pos + 4 > n || sp == 64) return 1;
      st[sp++] = (int32_t)u32(p + pos); pos += 4;
    } else if (op == 17 || op == 18) {
      size_t q; int32_t v = 0, known = 0;
      if (pos + 16 > n || sp == 64) return 1;
      for (q = 0; q < sizeof QUERIES / sizeof QUERIES[0]; q++)
        if (!memcmp(QUERIES[q].guid, p + pos, 16)) { v = QUERIES[q].value; known = 1; break; }
      st[sp++] = op == 17 ? v : known;
      pos += 16;
    } else if (op == 15) {
      if (sp < 1) return 1;
      st[sp - 1] = !st[sp - 1];
    } else if (op >= 1 && op <= 14) {
      int32_t a, b, r;
      if (sp < 2) return 1;
      b = st[--sp]; a = st[sp - 1];
      switch (op) {
        case 1: r = a & b; break;
        case 2: r = a | b; break;
        case 3: r = a ^ b; break;
        case 4: r = (int32_t)((uint32_t)a + (uint32_t)b); break;
        case 5: r = (int32_t)((uint32_t)a - (uint32_t)b); break;
        case 6: r = (int32_t)((uint32_t)a * (uint32_t)b); break;
        case 7: r = b == 0 || (a == INT32_MIN && b == -1) ? 0 : a / b; break;
        case 8: r = a && b; break;
        case 9: r = a || b; break;
        case 10: r = a < b; break;
        case 11: r = a <= b; break;
        case 12: r = a > b; break;
        case 13: r = a >= b; break;
        default: r = a == b; break;
      }
      st[sp - 1] = r;
    } else {
      return 1;
    }
  }
  return sp > 0 ? st[sp - 1] != 0 : 1;
}

/* ---- wsmp ---- */

typedef struct { int set; int unity, fine; int32_t gain; int loops; uint32_t loop_type, loop_start, loop_len; } Wsmp;

static void read_wsmp(Wsmp *w, const uint8_t *p, uint32_t n) {
  uint32_t cb, off;
  memset(w, 0, sizeof *w);
  w->unity = 60;
  if (n < 20) return;
  w->set = 1;
  cb = u32(p);
  w->unity = u16(p + 4) & 127;
  w->fine = (int16_t)u16(p + 6);
  w->gain = (int32_t)u32(p + 8);
  off = cb >= 20 && cb <= n ? cb : 20;
  if (u32(p + 16) > 0 && off <= n && n - off >= 16) {   /* the first loop only */
    w->loops = 1;
    w->loop_type = u32(p + off + 4);
    w->loop_start = u32(p + off + 8);
    w->loop_len = u32(p + off + 12);
  }
}

/* ---- the wave pool ---- */

typedef struct {
  int ok;                    /* a wave this can play */
  uint64_t list_off, list_len;   /* the LIST wave chunk: header offset, and 8 + its size */
  uint64_t data_off, data_len;
  uint32_t rate, frames;
  int channels, enc, decoded;
  Wsmp wsmp;
  char name[21];
  int first;                 /* its first sample (one per channel), or -1 */
} Wave;

/* a wave's fmt, from the first 40 bytes or fewer: PCM (with its encoding) or decoded; 0 when it can't be used */
static int read_fmt(Wave *w, const uint8_t *p, uint32_t n, uint32_t fact, int has_fact) {
  unsigned tag, bits, align;
  if (n < 16) return 0;
  tag = u16(p); w->channels = u16(p + 2); w->rate = u32(p + 4); align = u16(p + 12); bits = u16(p + 14);
  if (tag == 0xFFFE && n >= 26) tag = u16(p + 24);       /* WAVE_FORMAT_EXTENSIBLE: its sub-format's tag */
  if (w->channels < 1 || w->channels > MAX_CHANNELS || w->rate < 1 || w->rate > 1000000) return 0;
  w->decoded = 1;
  if ((tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (tag == 3 && bits == 32)) {
    int bps = (int)bits / 8;
    if (align == (unsigned)(bps * w->channels)) {
      w->decoded = 0;
      w->enc = tag == 3 ? SMP_PCM_F32LE : bits == 8 ? SMP_PCM_U8 : bits == 16 ? SMP_PCM_S16LE
             : bits == 24 ? SMP_PCM_S24LE : SMP_PCM_S32LE;
      if (w->data_len / align > MAX_DECODED * 64ull) return 0;
      w->frames = (uint32_t)(w->data_len / align);
      return w->frames > 0;
    }
  }
  /* decoded: its length isn't known until then; the fact chunk, the ADPCM block layout or the bit depth gives a
     guess for the sample header (the loader uses the decoded length) */
  if (has_fact && fact) w->frames = fact;
  else if ((tag == 2 || tag == 0x11) && n >= 20 && align > 0 && u16(p + 18) > 0)
    w->frames = (uint32_t)((w->data_len / align) * u16(p + 18) > MAX_DECODED ? MAX_DECODED : (w->data_len / align) * u16(p + 18));
  else if (bits > 0)
    w->frames = (uint32_t)(w->data_len * 8 / ((uint64_t)bits * w->channels) > MAX_DECODED ? MAX_DECODED
                           : w->data_len * 8 / ((uint64_t)bits * w->channels));
  else w->frames = (uint32_t)(w->data_len > MAX_DECODED ? MAX_DECODED : w->data_len);
  if (w->frames > MAX_DECODED) w->frames = MAX_DECODED;
  if (w->frames == 0) w->frames = 1;
  return w->data_len > 0;
}

/* one LIST wave at off (its header), size n (its LIST size): fmt, data, wsmp and name, read with small reads */
static void read_wave(Rd *s, Wave *w, uint64_t off, uint32_t n) {
  uint64_t pos = off + 12, end = off + 8 + (uint64_t)n;
  uint8_t fmt[40]; uint32_t nfmt = 0, fact = 0; int has_fmt = 0, has_data = 0, has_fact = 0, live = 1;
  memset(w, 0, sizeof *w);
  w->first = -1;
  w->wsmp.unity = 60;
  w->list_off = off; w->list_len = 8 + (uint64_t)n;
  while (pos + 8 <= end) {
    uint8_t c[8]; uint32_t m;
    if (!rd(s, pos, c, 8)) return;
    m = u32(c + 4);
    if (m > end - pos - 8) break;                        /* runs past the wave: the rest is lost */
    if (!memcmp(c, "cdl ", 4)) {
      uint8_t b[256]; uint32_t k = m < sizeof b ? m : sizeof b;
      live = rd(s, pos + 8, b, k) ? cdl_true(b, k) : 1;
    } else if (!live) {
    } else if (!memcmp(c, "fmt ", 4) && !has_fmt) {
      nfmt = m < sizeof fmt ? m : sizeof fmt;
      has_fmt = rd(s, pos + 8, fmt, nfmt);
    } else if (!memcmp(c, "data", 4) && !has_data) {
      w->data_off = pos + 8; w->data_len = m; has_data = 1;
    } else if (!memcmp(c, "fact", 4) && m >= 4 && !has_fact) {
      uint8_t b[4];
      if (rd(s, pos + 8, b, 4)) { fact = u32(b); has_fact = 1; }
    } else if (!memcmp(c, "wsmp", 4) && !w->wsmp.set) {
      uint8_t b[20 + 16];
      uint32_t k = m < sizeof b ? m : sizeof b;
      if (rd(s, pos + 8, b, k)) {
        if (k >= 20 && u32(b) > 20 && u32(b) <= m && m - u32(b) >= 16) {   /* a longer header: its loop is further on */
          uint8_t b2[16];
          read_wsmp(&w->wsmp, b, 20);
          if (u32(b + 16) > 0 && rd(s, pos + 8 + u32(b), b2, 16)) {
            w->wsmp.loops = 1; w->wsmp.loop_type = u32(b2 + 4); w->wsmp.loop_start = u32(b2 + 8); w->wsmp.loop_len = u32(b2 + 12);
          }
        } else read_wsmp(&w->wsmp, b, k);
      }
    } else if (!memcmp(c, "LIST", 4) && m >= 4) {
      uint8_t b[512]; uint32_t k = m < sizeof b ? m : sizeof b;
      if (rd(s, pos + 8, b, k) && !memcmp(b, "INFO", 4)) info_name(b + 4, k - 4, w->name, sizeof w->name);
    }
    pos += 8 + (uint64_t)m + (m & 1);
  }
  if (has_fmt && has_data) w->ok = read_fmt(w, fmt, nfmt, fact, has_fact);
}

/* ---- articulation ---- */

typedef struct { const uint8_t *p; uint32_t n; int dls1; } ArtSeg;   /* n connection blocks of 12 bytes at p */
typedef struct { ArtSeg seg[MAX_ART]; int nseg; } Art;

/* adds a LIST lart or lar2's art1 and art2 chunks (either kind in either list, as some writers mix them) */
static void read_art(Art *a, const uint8_t *data, uint32_t n) {
  It it = {data, n, 0}; const uint8_t *id, *d; uint32_t m; int live = 1;
  while (it_next(&it, &id, &d, &m)) {
    uint32_t cb, cnt;
    if (!memcmp(id, "cdl ", 4)) { live = cdl_true(d, m); continue; }
    if (!live || (memcmp(id, "art1", 4) && memcmp(id, "art2", 4)) || m < 8) continue;
    cb = u32(d); cnt = u32(d + 4);
    if (cb < 8 || cb > m) continue;
    if (cnt > (m - cb) / 12) cnt = (m - cb) / 12;
    if (a->nseg < MAX_ART) {
      a->seg[a->nseg].p = d + cb; a->seg[a->nseg].n = cnt; a->seg[a->nseg].dls1 = !memcmp(id, "art1", 4);
      a->nseg++;
    }
  }
}

/* lScale to its base unit, rounded (0x80000000 is -32768) */
static int scale_of(int32_t s) { return (int)(((int64_t)s + 32768) >> 16); }

/* a DLS source as an SF2 modulator source word, or -1 when SF2 has nothing like it. trn is the source's transform
   (0 linear, 1 concave, 2 convex, 3 switch), bip and inv its flags. */
static int sf_source(unsigned src, unsigned trn, int bip, int inv) {
  int idx, cc = 0;
  if (trn > 3) return -1;
  switch (src) {
    case S_NONE: idx = 0; break;
    case S_VELOCITY: idx = 2; break;
    case S_KEY: idx = 3; break;
    case S_POLYPRESSURE: idx = 10; break;
    case S_CHANPRESSURE: idx = 13; break;
    case S_PITCHWHEEL: idx = 14; break;
    case S_RPN0: idx = 16; break;
    default:
      if (src >= 0x80 && src <= 0xFF) { idx = (int)(src & 127); cc = 1; break; }
      return -1;
  }
  return idx | cc << 7 | (inv ? 1 : 0) << 8 | (bip ? 1 : 0) << 9 | (int)trn << 10;
}

/* the SF2 generator a DLS destination sets as a value (with *kind: 0 plain, 1 sustain level, 2 gain, 3 pitch), or
   -1. LFO destinations name DLS2's LFO; DLS1 moves the vibrato part later. */
static int dest_gen(unsigned d, int *kind) {
  *kind = 0;
  switch (d) {
    case D_GAIN: *kind = 2; return G_ATTEN;
    case D_PITCH: *kind = 3; return G_PITCH;
    case D_PAN: return G_PAN;
    case D_CHORUS: return G_CHORUS;
    case D_REVERB: return G_REVERB;
    case D_LFO_FREQ: return G_FREQ_MODLFO;
    case D_LFO_DELAY: return G_DELAY_MODLFO;
    case D_VIB_FREQ: return G_FREQ_VIBLFO;
    case D_VIB_DELAY: return G_DELAY_VIBLFO;
    case D_EG1_ATTACK: return G_ATTACK_VOLENV;
    case D_EG1_DECAY: return G_DECAY_VOLENV;
    case D_EG1_RELEASE: return G_RELEASE_VOLENV;
    case D_EG1_SUSTAIN: *kind = 1; return G_SUSTAIN_VOLENV;
    case D_EG1_DELAY: return G_DELAY_VOLENV;
    case D_EG1_HOLD: return G_HOLD_VOLENV;
    case D_EG2_ATTACK: return G_ATTACK_MODENV;
    case D_EG2_DECAY: return G_DECAY_MODENV;
    case D_EG2_RELEASE: return G_RELEASE_MODENV;
    case D_EG2_SUSTAIN: *kind = 1; return G_SUSTAIN_MODENV;
    case D_EG2_DELAY: return G_DELAY_MODENV;
    case D_EG2_HOLD: return G_HOLD_MODENV;
    case D_FILTER_CUTOFF: return G_FC;
    case D_FILTER_Q: return G_Q;
    default: return -1;
  }
}

/* the depth generator for an LFO or envelope source to a destination (modLfoToPitch...), or -1 */
static int depth_gen(unsigned src, unsigned d, int dls1) {
  if (src == S_LFO && d == D_PITCH) return dls1 ? G_VIBLFO_PITCH : G_MODLFO_PITCH;
  if (src == S_LFO && d == D_GAIN) return G_MODLFO_VOL;
  if (src == S_LFO && d == D_FILTER_CUTOFF) return G_MODLFO_FC;
  if (src == S_VIBRATO && d == D_PITCH) return G_VIBLFO_PITCH;
  if (src == S_EG2 && d == D_PITCH) return G_MODENV_PITCH;
  if (src == S_EG2 && d == D_FILTER_CUTOFF) return G_MODENV_FC;
  return -1;
}

/* What an articulation works out to: generator values (int, before the ranges), which were set, and its
   modulators. tune is in cents and atten in SF2's generator units (centibels / 0.4); both are added to the
   region's wsmp. */
typedef struct {
  int v[SMP_SF_NGEN]; uint64_t set;
  int key_scale[4], has_key_scale[4];   /* key to vol hold, vol decay, mod hold, mod decay: the DLS scale */
  double tune, atten;
  int dls1;
  SmpSfMod mods[SF_MAX_ZONE_MODS]; int nmods;
} ArtOut;

static void art_apply(ArtOut *o, const Art *a, int *ignored) {
  int i, s;
  int16_t def[SMP_SF_NGEN];
  memset(o, 0, sizeof *o);
  sf_gen_defaults(def);
  for (i = 0; i < SMP_SF_NGEN; i++) o->v[i] = def[i];
  o->v[G_FREQ_MODLFO] = o->v[G_FREQ_VIBLFO] = DLS_LFO_HZ_CENTS;
  o->v[G_DELAY_MODLFO] = o->v[G_DELAY_VIBLFO] = DLS_LFO_DELAY_TC;
  if (!a) return;
  for (s = 0; s < a->nseg; s++) {
    const ArtSeg *g = &a->seg[s];
    uint32_t b;
    if (g->dls1) o->dls1 = 1;
    for (b = 0; b < g->n; b++) {
      const uint8_t *c = g->p + 12 * (size_t)b;
      unsigned src = u16(c), ctl = u16(c + 2), dst = u16(c + 4), trn = u16(c + 6);
      int v = scale_of((int32_t)u32(c + 8)), kind, gen;
      unsigned out_trn = trn & 15, ctl_trn = (trn >> 4) & 15, src_trn = (trn >> 10) & 15;
      int ctl_bip = (trn >> 8) & 1, ctl_inv = (trn >> 9) & 1, src_bip = (trn >> 14) & 1, src_inv = (trn >> 15) & 1;
      if (src == S_NONE && ctl == S_NONE) {                        /* a value */
        gen = dest_gen(dst, &kind);
        if (gen < 0) { (*ignored)++; continue; }
        if (kind == 2) o->atten += -v / 0.4;
        else if (kind == 3) o->tune += v;
        else { o->v[gen] = kind == 1 ? 1000 - v : v; o->set |= 1ull << gen; }
        continue;
      }
      if (src == S_KEY && ctl == S_NONE) {
        int k = dst == D_EG1_HOLD ? 0 : dst == D_EG1_DECAY ? 1 : dst == D_EG2_HOLD ? 2 : dst == D_EG2_DECAY ? 3 : -1;
        if (dst == D_PITCH) { o->v[G_SCALE] = (int)floor(v / 128.0 + 0.5); o->set |= 1ull << G_SCALE; continue; }
        if (k >= 0) { o->key_scale[k] = v; o->has_key_scale[k] = 1; continue; }
      }
      if (src == S_LFO || src == S_VIBRATO || src == S_EG2) {      /* a depth, maybe under a controller */
        gen = depth_gen(src, dst, g->dls1);
        if (gen < 0) { (*ignored)++; continue; }
        if (ctl == S_NONE) { o->v[gen] = v; o->set |= 1ull << gen; continue; }
        {
          int w = sf_source(ctl, ctl_trn, ctl_bip, ctl_inv);
          if (w < 0 || o->nmods >= SF_MAX_ZONE_MODS) { (*ignored)++; continue; }
          o->mods[o->nmods].src = (uint16_t)w; o->mods[o->nmods].dest = (uint16_t)gen;
          o->mods[o->nmods].amount = (int16_t)clampi(v, -32768, 32767);
          o->mods[o->nmods].amt_src = 0; o->mods[o->nmods].trans = 0;
          o->nmods++;
        }
        continue;
      }
      {                                                            /* a modulator from a MIDI source */
        int ws, wc, amount = v;
        if (g->dls1 && src_trn == 0 && out_trn <= 3) src_trn = out_trn;   /* DLS1's curve is the output's */
        gen = dest_gen(dst, &kind);
        if ((dst == D_GAIN) && (src == S_VELOCITY || src == S_CC7 || src == S_CC11)) src_inv = 1;
        ws = sf_source(src, src_trn, src_bip, src_inv);
        wc = sf_source(ctl, ctl_trn, ctl_bip, ctl_inv);
        if (gen < 0 || ws < 0 || wc < 0 || o->nmods >= SF_MAX_ZONE_MODS) { (*ignored)++; continue; }
        if (kind == 1 || kind == 2) amount = -v;                   /* level or gain to attenuation */
        if (kind == 2) amount = clampi(amount, -960, 960);
        o->mods[o->nmods].src = (uint16_t)ws; o->mods[o->nmods].dest = (uint16_t)gen;
        o->mods[o->nmods].amount = (int16_t)clampi(amount, -32768, 32767);
        o->mods[o->nmods].amt_src = (uint16_t)wc; o->mods[o->nmods].trans = 0;
        o->nmods++;
      }
    }
  }
  /* key number to hold and decay (see the top) */
  {
    static const int kg[4] = {G_KEY_VOLENV_HOLD, G_KEY_VOLENV_DECAY, G_KEY_MODENV_HOLD, G_KEY_MODENV_DECAY};
    static const int bg[4] = {G_HOLD_VOLENV, G_DECAY_VOLENV, G_HOLD_MODENV, G_DECAY_MODENV};
    for (i = 0; i < 4; i++) {
      if (!o->has_key_scale[i]) continue;
      o->v[kg[i]] = (int)floor(-o->key_scale[i] / 128.0 + 0.5);
      if (o->set >> bg[i] & 1) o->v[bg[i]] += (int)floor(60.0 * o->key_scale[i] / 128.0 + 0.5);
    }
  }
  /* DLS1: the one LFO's vibrato is SF2's vibrato LFO */
  if (o->dls1) {
    o->v[G_FREQ_VIBLFO] = o->v[G_FREQ_MODLFO];
    o->v[G_DELAY_VIBLFO] = o->v[G_DELAY_MODLFO];
  }
}

/* ---- instruments ---- */

typedef struct {
  const uint8_t *rgnh, *wsmp, *wlnk; uint32_t nrgnh, nwsmp, nwlnk;
  Art art;
} Rgn;

/* a LIST rgn or rgn2's chunks; 0 when it has no rgnh or wlnk left after its conditions */
static int read_rgn(Rgn *r, const uint8_t *data, uint32_t n) {
  It it = {data, n, 0}; const uint8_t *id, *d; uint32_t m; int live = 1;
  memset(r, 0, sizeof *r);
  while (it_next(&it, &id, &d, &m)) {
    if (!memcmp(id, "cdl ", 4)) { live = cdl_true(d, m); continue; }
    if (!live) continue;
    if (!memcmp(id, "rgnh", 4) && !r->rgnh && m >= 12) { r->rgnh = d; r->nrgnh = m; }
    else if (!memcmp(id, "wsmp", 4) && !r->wsmp) { r->wsmp = d; r->nwsmp = m; }
    else if (!memcmp(id, "wlnk", 4) && !r->wlnk && m >= 12) { r->wlnk = d; r->nwlnk = m; }
    else if (is_list(id, d, m, "lart") || is_list(id, d, m, "lar2")) read_art(&r->art, d + 4, m - 4);
  }
  return r->rgnh && r->wlnk;
}

typedef struct {
  const uint8_t *lrgn; uint32_t nlrgn;
  uint32_t bank, program;
  Art art;
  char name[21];
} Ins;

static int read_ins(Ins *in, const uint8_t *data, uint32_t n) {
  It it = {data, n, 0}; const uint8_t *id, *d; uint32_t m; int live = 1, has_insh = 0;
  memset(in, 0, sizeof *in);
  while (it_next(&it, &id, &d, &m)) {
    if (!memcmp(id, "cdl ", 4)) { live = cdl_true(d, m); continue; }
    if (!live) continue;
    if (!memcmp(id, "insh", 4) && !has_insh && m >= 12) { in->bank = u32(d + 4); in->program = u32(d + 8); has_insh = 1; }
    else if (is_list(id, d, m, "lrgn") && !in->lrgn) { in->lrgn = d + 4; in->nlrgn = m - 4; }
    else if (is_list(id, d, m, "lart") || is_list(id, d, m, "lar2")) read_art(&in->art, d + 4, m - 4);
    else if (is_list(id, d, m, "INFO")) info_name(d + 4, m - 4, in->name, sizeof in->name);
  }
  return has_insh;
}

/* ---- building the font ---- */

typedef struct {
  SmpSfRegion *reg; int nreg, capreg;
  int *moff;                         /* per region: its list's offset in mods */
  SmpSfMod *mods; int nmods, capmods;
} Out;

static int out_mods(Out *o, const SmpSfMod *m, int n) {
  int at = o->nmods;
  if (o->nmods + n > SF_MAX_POOL_MODS) return -2;
  if (o->nmods + n > o->capmods) {
    int cap = o->capmods * 2 > o->nmods + n ? o->capmods * 2 : o->nmods + n + 256;
    SmpSfMod *g = realloc(o->mods, (size_t)cap * sizeof *g);
    if (!g) return -1;
    o->mods = g; o->capmods = cap;
  }
  memcpy(o->mods + at, m, (size_t)n * sizeof *m);
  o->nmods += n;
  return at;
}

/* the instrument modulator list of an articulation: SF2's defaults with DLS's reverb and chorus sends, then the
   articulation's own merged in. Its offset in o->mods, or -1 (memory) or -2 (too many). */
static int mod_list(Out *o, const ArtOut *a, int *n_out) {
  SmpSfMod list[SF_NDEFMOD + SF_MAX_ZONE_MODS];
  int n, i;
  memcpy(list, sf_default_mods(), SF_NDEFMOD * sizeof *list);
  for (i = 0; i < SF_NDEFMOD; i++)
    if (list[i].src == 0x00DB || list[i].src == 0x00DD) list[i].amount = 1000;   /* CC91, CC93: full send */
  n = sf_merge_mods(list, SF_NDEFMOD, a->mods, a->nmods);
  *n_out = n;
  return out_mods(o, list, n);
}

static int add_region(Out *o, const SmpSfRegion *r, int moff) {
  if (o->nreg == o->capreg) {
    int cap = o->capreg ? o->capreg * 2 : 256; void *g;
    if (o->nreg >= SF_MAX_REGIONS) return -2;
    if (!(g = realloc(o->reg, (size_t)cap * sizeof *o->reg))) return -1;
    o->reg = g;
    if (!(g = realloc(o->moff, (size_t)cap * sizeof *o->moff))) return -1;
    o->moff = g;
    o->capreg = cap;
  }
  o->reg[o->nreg] = *r;
  o->moff[o->nreg] = moff;
  o->nreg++;
  return 0;
}

/* a region's generators from its articulation, its wsmp (the region's, else the wave's) and its sample */
static void region_gens(SmpSfRegion *r, const ArtOut *a, const Wsmp *ws, const SmpSfSample *sh, int keygroup, int pan) {
  int i, tune, mode;
  int v[SMP_SF_NGEN];
  memcpy(v, a->v, sizeof v);
  tune = (int)floor(ws->fine + a->tune + 0.5);
  v[G_COARSE] = clampi(tune / 100, -120, 120);
  v[G_FINE] = tune - 100 * (tune / 100);
  v[G_ATTEN] = (int)floor(-scale_of(ws->gain) / 0.4 + a->atten + 0.5);
  v[G_ATTEN] = clampi(v[G_ATTEN], -32768, 32767);
  v[G_ROOTKEY] = ws->unity;
  v[G_EXCLUSIVE] = keygroup;
  v[G_PAN] += pan;
  mode = 0;
  if (ws->loops) {
    uint64_t ls = ws->loop_start, le = (uint64_t)ws->loop_start + ws->loop_len;
    int64_t ds = (int64_t)ls - sh->loop_start, de = (int64_t)le - sh->loop_end;
    mode = ws->loop_type == 1 ? 3 : 1;   /* 1 is loop and release */
    /* a loop other than the sample's (a region's own wsmp): the loop offset generators, fine plus 32768s */
    if (ds < -0x7fffffffll || ds > 0x7fffffffll || de < -0x7fffffffll || de > 0x7fffffffll) mode = 0;
    else {
      v[G_LOOPSTART] = (int)(ds % 32768); v[G_LOOPSTARTCOARSE] = clampi((int)(ds / 32768), -32768, 32767);
      v[G_LOOPEND] = (int)(de % 32768); v[G_LOOPENDCOARSE] = clampi((int)(de / 32768), -32768, 32767);
    }
  }
  v[G_MODES] = mode;
  for (i = 0; i < SMP_SF_NGEN; i++) r->gen[i] = (int16_t)sf_gen_clamp(i, v[i]);
  r->gen[G_KEYRANGE] = (int16_t)(r->lokey | r->hikey << 8);
  r->gen[G_VELRANGE] = (int16_t)(r->lovel | r->hivel << 8);
}

/* the wave for a wlnk table index: through ptbl's cue offsets when there is a table, else in pool order */
static int wave_of(uint32_t idx, const uint32_t *cue, uint32_t ncue, const Wave *w, int nw, uint64_t pool0) {
  if (cue) {
    int lo = 0, hi = nw;
    uint64_t want;
    if (idx >= ncue) return -1;
    want = pool0 + cue[idx];
    while (lo < hi) { int mid = (lo + hi) / 2; if (w[mid].list_off < want) lo = mid + 1; else hi = mid; }
    if (lo < nw && w[lo].list_off == want) return lo;
    return idx < (uint32_t)nw ? (int)idx : -1;   /* an offset that names no wave: pool order, as some readers do */
  }
  return idx < (uint32_t)nw ? (int)idx : -1;
}

int sf_parse_dls(SmpFont *f, HANDLE h, uint64_t size, const char *path, char *why, int wcap) {
  Rd s; uint8_t hd[12];
  uint64_t pos, end, lins_off = 0, wvpl_off = 0; uint32_t lins_n = 0, wvpl_n = 0;
  int have_lins = 0, have_wvpl = 0, ok = 0, nw = 0, capw = 0, i, nins = 0, capins = 0, nsamp = 0;
  uint8_t *lins = NULL; uint32_t *cue = NULL, ncue = 0;
  Wave *waves = NULL; Ins *ins = NULL;
  ArtOut *ia = NULL, *ra = NULL; Rgn *rg = NULL;
  Out o; memset(&o, 0, sizeof o);
  s.h = h; s.size = size;
  if (!rd(&s, 0, hd, 12)) { snprintf(why, wcap, "it can't be read"); return 0; }
  end = 8 + (uint64_t)u32(hd + 4);
  if (end > size) end = size;
  for (pos = 12; pos + 8 <= end; ) {
    uint8_t c[12]; uint32_t n;
    if (!rd(&s, pos, c, 8)) { snprintf(why, wcap, "it can't be read"); goto fail; }
    n = u32(c + 4);
    if (n > end - pos - 8) { snprintf(why, wcap, "it is cut short"); goto fail; }
    if (!memcmp(c, "LIST", 4) && n >= 4) {
      if (!rd(&s, pos + 8, c + 8, 4)) { snprintf(why, wcap, "it can't be read"); goto fail; }
      if (!memcmp(c + 8, "lins", 4) && !have_lins) { lins_off = pos + 12; lins_n = n - 4; have_lins = 1; }
      else if (!memcmp(c + 8, "wvpl", 4) && !have_wvpl) { wvpl_off = pos + 12; wvpl_n = n - 4; have_wvpl = 1; }
    } else if (!memcmp(c, "ptbl", 4) && !cue && n >= 8) {
      uint8_t t[8]; uint32_t cb, k;
      if (!rd(&s, pos + 8, t, 8)) { snprintf(why, wcap, "it can't be read"); goto fail; }
      cb = u32(t); ncue = u32(t + 4);
      if (cb < 8 || cb > n) cb = 8;
      if (ncue > (n - cb) / 4) ncue = (n - cb) / 4;
      if (ncue > MAX_CUES) { snprintf(why, wcap, "its wave table is too long"); goto fail; }
      if (!(cue = malloc((size_t)(ncue ? ncue : 1) * 4))) { snprintf(why, wcap, "out of memory"); goto fail; }
      if (ncue && !rd(&s, pos + 8 + cb, cue, ncue * 4)) { snprintf(why, wcap, "it can't be read"); goto fail; }
      for (k = 0; k < ncue; k++) cue[k] = u32((const uint8_t *)&cue[k]);
    }
    pos += 8 + (uint64_t)n + (n & 1);
  }
  if (!have_lins) { snprintf(why, wcap, "it has no instrument list"); goto fail; }
  if (!have_wvpl) { snprintf(why, wcap, "it has no wave pool"); goto fail; }
  if (lins_n > MAX_LINS) { snprintf(why, wcap, "its instrument list is too big"); goto fail; }

  /* the wave pool, a wave at a time */
  for (pos = wvpl_off; pos + 8 <= wvpl_off + wvpl_n; ) {
    uint8_t c[12]; uint32_t n;
    if (!rd(&s, pos, c, 12)) break;
    n = u32(c + 4);
    if (n > wvpl_off + wvpl_n - pos - 8) break;          /* runs past the pool: the waves before it count */
    if (!memcmp(c, "LIST", 4) && n >= 4 && (!memcmp(c + 8, "wave", 4) || !memcmp(c + 8, "WAVE", 4))) {
      if (nw == MAX_WAVES) break;
      if (nw == capw) {
        int cap = capw ? capw * 2 : 64; Wave *g = realloc(waves, (size_t)cap * sizeof *g);
        if (!g) { snprintf(why, wcap, "out of memory"); goto fail; }
        waves = g; capw = cap;
      }
      read_wave(&s, &waves[nw], pos, n);
      if (!waves[nw].name[0]) snprintf(waves[nw].name, sizeof waves[nw].name, "Wave %d", nw + 1);
      nw++;
    }
    pos += 8 + (uint64_t)n + (n & 1);
  }

  /* samples: one per channel of each wave that can play, while there is room */
  for (i = 0; i < nw; i++)
    if (waves[i].ok && nsamp + waves[i].channels <= MAX_WAVES) { waves[i].first = nsamp; nsamp += waves[i].channels; }
  f->nshdr = nsamp;
  f->shdr = calloc(nsamp ? (size_t)nsamp : 1, sizeof *f->shdr);
  f->src = calloc(nsamp ? (size_t)nsamp : 1, sizeof *f->src);
  f->paths = calloc(1, sizeof *f->paths);
  if (!f->shdr || !f->src || !f->paths || !(f->paths[0] = _strdup(path))) { snprintf(why, wcap, "out of memory"); goto fail; }
  f->npaths = 1;
  for (i = 0; i < nw; i++) {
    const Wave *w = &waves[i]; int ch;
    if (w->first < 0) continue;
    for (ch = 0; ch < w->channels; ch++) {
      SmpSfSample *sh = &f->shdr[w->first + ch];
      SmpSrc *c = &f->src[w->first + ch];
      memcpy(sh->name, w->name, sizeof sh->name);
      sh->start = 0; sh->end = w->frames; sh->rate = w->rate;
      sh->pitch = (uint8_t)w->wsmp.unity; sh->correction = 0; sh->link = 0; sh->type = SF_MONO;
      if (w->wsmp.loops) {
        uint64_t ls = w->wsmp.loop_start, le = ls + w->wsmp.loop_len;
        sh->loop_start = (uint32_t)(ls > 0xffffffffu ? 0xffffffffu : ls);
        sh->loop_end = (uint32_t)(le > 0xffffffffu ? 0xffffffffu : le);
      }
      c->path_id = 0; c->channel = (uint8_t)ch;
      if (w->decoded) {
        c->kind = SMP_SRC_DECODED; c->offset = w->list_off; c->length = w->list_len; c->wrap = 1;
        c->channels = (uint8_t)w->channels;
        f->has_long = 1;
      } else {
        c->kind = SMP_SRC_PCM_FILE; c->offset = w->data_off; c->enc = (uint8_t)w->enc; c->channels = (uint8_t)w->channels;
        if (w->frames >= w->rate) f->has_long = 1;
      }
    }
  }

  /* the instruments */
  if (!(lins = malloc(lins_n ? lins_n : 1))) { snprintf(why, wcap, "out of memory"); goto fail; }
  if (!rd(&s, lins_off, lins, lins_n)) { snprintf(why, wcap, "it can't be read"); goto fail; }
  {
    It it = {lins, lins_n, 0}; const uint8_t *id, *d; uint32_t m;
    while (it_next(&it, &id, &d, &m)) {
      if (!is_list(id, d, m, "ins ")) continue;
      if (nins == 65535) break;
      if (nins == capins) {
        int cap = capins ? capins * 2 : 64; Ins *g = realloc(ins, (size_t)cap * sizeof *g);
        if (!g) { snprintf(why, wcap, "out of memory"); goto fail; }
        ins = g; capins = cap;
      }
      if (read_ins(&ins[nins], d + 4, m - 4)) nins++;
    }
  }
  if (nins < 1) { snprintf(why, wcap, "it has no instruments"); goto fail; }

  f->nphdr = nins;
  f->phdr = calloc((size_t)nins + 1, sizeof *f->phdr);
  f->order = calloc((size_t)nins + 1, sizeof *f->order);
  f->rstart = NULL;
  ia = malloc(sizeof *ia); ra = malloc(sizeof *ra); rg = malloc(sizeof *rg);
  {
    int *rstart = malloc(((size_t)nins + 1) * sizeof *rstart), p;
    if (!f->phdr || !f->order || !ia || !ra || !rg || !rstart) { free(rstart); snprintf(why, wcap, "out of memory"); goto fail; }
    for (p = 0; p < nins; p++) {
      const Ins *in = &ins[p];
      SfPhdr *ph = &f->phdr[p];
      int imoff = -1, inmods = 0;
      It it = {in->lrgn, in->nlrgn, 0}; const uint8_t *id, *d; uint32_t m;
      if (in->name[0]) memcpy(ph->name, in->name, sizeof ph->name);
      else snprintf(ph->name, sizeof ph->name, "Instrument %d", p + 1);
      /* bit 31: drums, bank 128. Otherwise the bank select MSB (bits 8 to 14), or its LSB (bits 0 to 6) when the MSB
         is 0, as the player reads CC0 and CC32 */
      if (in->bank & 0x80000000u) ph->bank = 128;
      else ph->bank = (uint16_t)(((in->bank >> 8) & 127) ? ((in->bank >> 8) & 127) : (in->bank & 127));
      ph->program = (uint16_t)(in->program & 127);
      rstart[p] = o.nreg;
      art_apply(ia, &in->art, &f->dls_ignored);
      while (in->lrgn && it_next(&it, &id, &d, &m)) {
        Wsmp ws; const ArtOut *a; int wi, moff, nmods, lo, hi, vlo, vhi, keygroup, chans[MAX_CHANNELS], nch = 0, k;
        uint32_t mask; const Wave *w;
        if (!(is_list(id, d, m, "rgn ") || is_list(id, d, m, "rgn2"))) continue;
        if (!read_rgn(rg, d + 4, m - 4)) continue;
        lo = u16(rg->rgnh); hi = u16(rg->rgnh + 2); vlo = u16(rg->rgnh + 4); vhi = u16(rg->rgnh + 6);
        keygroup = clampi(u16(rg->rgnh + 10), 0, 127);
        if (hi > 127) hi = 127;
        if (vhi > 127) vhi = 127;
        if (vlo == 0 && vhi == 0) vhi = 127;               /* DLS1 writers often leave the velocity range at 0 */
        if (lo > hi || vlo > vhi) continue;
        mask = u32(rg->wlnk + 4);
        wi = wave_of(u32(rg->wlnk + 8), cue, ncue, waves, nw, wvpl_off);
        if (wi < 0 || waves[wi].first < 0) continue;
        w = &waves[wi];
        if (rg->wsmp) read_wsmp(&ws, rg->wsmp, rg->nwsmp);
        else ws.set = 0;
        if (!ws.set) ws = w->wsmp;
        /* its articulation, else the instrument's, and that one's modulator list once per instrument */
        if (rg->art.nseg) {
          art_apply(ra, &rg->art, &f->dls_ignored);
          a = ra;
          moff = mod_list(&o, a, &nmods);
        } else {
          a = ia;
          if (imoff < 0) imoff = mod_list(&o, a, &inmods);
          moff = imoff; nmods = inmods;
        }
        if (moff == -2) { free(rstart); snprintf(why, wcap, "it has too many modulators"); goto fail; }
        if (moff < 0) { free(rstart); snprintf(why, wcap, "out of memory"); goto fail; }
        /* which channels: wlnk's channel bits (1 left, 2 right...), each that the wave has; else its first */
        for (k = 0; k < w->channels; k++) if (mask >> k & 1) chans[nch++] = k;
        if (!nch) chans[nch++] = 0;
        for (k = 0; k < nch; k++) {
          SmpSfRegion r; int e;
          memset(&r, 0, sizeof r);
          r.lokey = (uint8_t)lo; r.hikey = (uint8_t)hi; r.lovel = (uint8_t)vlo; r.hivel = (uint8_t)vhi;
          r.sample = w->first + chans[k];
          /* a stereo wave's left and right channels sit at the left and right */
          region_gens(&r, a, &ws, &f->shdr[r.sample], keygroup, w->channels == 2 ? (chans[k] ? 500 : -500) : 0);
          r.nimod = nmods; r.npmod = 0;
          e = add_region(&o, &r, moff);
          if (e == -2) { free(rstart); snprintf(why, wcap, "it has too many regions"); goto fail; }
          if (e < 0) { free(rstart); snprintf(why, wcap, "out of memory"); goto fail; }
        }
      }
    }
    rstart[nins] = o.nreg;

    /* the one block sf_free frees: regions, then the per-preset starts, then the modulator lists */
    {
      size_t sreg = (size_t)o.nreg * sizeof(SmpSfRegion), sstart = ((size_t)nins + 1) * sizeof(int);
      size_t total = sreg + sstart + (size_t)o.nmods * sizeof(SmpSfMod);
      char *block = malloc(total ? total : 1);
      if (!block) { free(rstart); snprintf(why, wcap, "out of memory"); goto fail; }
      f->regions = (SmpSfRegion *)block;
      f->rstart = (int *)(block + sreg);
      if (sreg) memcpy(f->regions, o.reg, sreg);
      memcpy(f->rstart, rstart, sstart);
      if (o.nmods) memcpy(block + sreg + sstart, o.mods, (size_t)o.nmods * sizeof(SmpSfMod));
      for (i = 0; i < o.nreg; i++) {
        f->regions[i].imod = (const SmpSfMod *)(block + sreg + sstart) + o.moff[i];
        f->regions[i].pmod = NULL;
      }
      f->nregions = o.nreg;
    }
    free(rstart);
  }
  if (!sf_build_order(f)) { snprintf(why, wcap, "out of memory"); goto fail; }
  ok = 1;
fail:
  free(lins); free(cue); free(waves); free(ins); free(ia); free(ra); free(rg);
  free(o.reg); free(o.moff); free(o.mods);
  return ok;
}
