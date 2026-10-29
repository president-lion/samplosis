/* sf2.c: read a SoundFont's (SF2 or SF3) RIFF chunks and its hydra (the preset, instrument and sample headers).
   Only the headers are read; the sample data stays on disk and is found again by smpl_off. Every count is
   checked against its chunk's size and every index against the array it points into, so a damaged file
   gives a plain reason instead of a crash. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sf2.h"

typedef struct { HANDLE h; uint64_t size; } Src;

static int rd(Src *s, uint64_t off, void *buf, uint32_t n) {
  LARGE_INTEGER li; DWORD got = 0;
  if (off > s->size || n > s->size - off) return 0;
  li.QuadPart = (LONGLONG)off;
  if (!SetFilePointerEx(s->h, li, NULL, FILE_BEGIN)) return 0;
  return ReadFile(s->h, buf, n, &got, NULL) && got == n;
}

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* a 20-byte name: up to the first NUL, trailing spaces dropped */
static void name20(char *out, const uint8_t *p) {
  int n = 0;
  while (n < 20 && p[n]) { out[n] = (char)p[n]; n++; }
  while (n > 0 && out[n - 1] == ' ') n--;
  out[n] = 0;
}

static const char *base_name(const char *path) {
  const char *b = path, *p;
  for (p = path; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
  return b;
}

/* the reason a file can't be read: "<name> isn't a SoundFont this can read (<why>)" */
static void why_bad(char *err, int cap, const char *path, const char *why) {
  if (err && cap > 0) snprintf(err, cap, "%s isn't a SoundFont this can read (%s)", base_name(path), why);
}

/* the pdta chunk's nine record lists */
typedef struct { const char *id; uint32_t rec; const uint8_t *p; uint32_t n; } Sub;

/* sizes of the records in phdr, pbag, pmod, pgen, inst, ibag, imod, igen and shdr */
static const Sub SUBS[9] = {
  {"phdr", 38}, {"pbag", 4}, {"pmod", 10}, {"pgen", 4}, {"inst", 22},
  {"ibag", 4}, {"imod", 10}, {"igen", 4}, {"shdr", 46}
};

static void read_bags(SfBag *b, const uint8_t *p, uint32_t n) {
  uint32_t i;
  for (i = 0; i < n; i++, p += 4) { b[i].gen = u16(p); b[i].mod = u16(p + 2); }
}
static void read_mods(SmpSfMod *m, const uint8_t *p, uint32_t n) {
  uint32_t i;
  for (i = 0; i < n; i++, p += 10) {
    m[i].src = u16(p); m[i].dest = u16(p + 2); m[i].amount = (int16_t)u16(p + 4);
    m[i].amt_src = u16(p + 6); m[i].trans = u16(p + 8);
  }
}
static void read_gens(SfGen *g, const uint8_t *p, uint32_t n) {
  uint32_t i;
  for (i = 0; i < n; i++, p += 4) { g[i].oper = u16(p); g[i].amount = (int16_t)u16(p + 2); }
}

/* zone lists: header i owns bags [bag(i), bag(i+1)), so the starts must not go backwards or past the bags.
   n counts the headers without the terminal one. */
static int bags_ok(const uint16_t *starts, int stride, int n, int nbag) {
  int i;
  for (i = 0; i < n; i++) {
    uint16_t a = *(const uint16_t *)((const char *)starts + (size_t)i * stride);
    uint16_t b = *(const uint16_t *)((const char *)starts + (size_t)(i + 1) * stride);
    if (a > b) return 0;
  }
  return *(const uint16_t *)((const char *)starts + (size_t)n * stride) <= nbag;
}

/* bag i owns generators [gen(i), gen(i+1)) and modulators [mod(i), mod(i+1)) */
static int gens_ok(const SfBag *b, int nbag, int ngen, int nmod) {
  int i;
  for (i = 0; i < nbag; i++) if (b[i].gen > b[i + 1].gen || b[i].mod > b[i + 1].mod) return 0;
  return b[nbag].gen <= ngen && b[nbag].mod <= nmod;
}

static int parse_pdta(SmpFont *f, const uint8_t *p, uint32_t size, char *why, int wcap) {
  Sub sub[9]; uint32_t pos = 0; int i, k;
  memcpy(sub, SUBS, sizeof sub);
  while (pos + 8 <= size) {
    uint32_t n = u32(p + pos + 4);
    if (n > size - pos - 8) { snprintf(why, wcap, "a preset list runs past its chunk"); return 0; }
    for (k = 0; k < 9; k++)
      if (!memcmp(p + pos, sub[k].id, 4) && !sub[k].p) {
        if (n % sub[k].rec) { snprintf(why, wcap, "the %s list's size isn't a whole number of records", sub[k].id); return 0; }
        sub[k].p = p + pos + 8; sub[k].n = n / sub[k].rec;
      }
    pos += 8 + n + (n & 1);
  }
  for (k = 0; k < 9; k++)
    if (!sub[k].p || sub[k].n < 1) { snprintf(why, wcap, "it has no %s list", sub[k].id); return 0; }
  /* every index in the file is 16 bits, so longer lists can't be reached; refuse them */
  for (k = 0; k < 9; k++)
    if (sub[k].n > 65536) { snprintf(why, wcap, "the %s list is too long", sub[k].id); return 0; }

  /* the counts leave out the terminal record (EOP, EOI, EOS, the last bag and the last generator and modulator) */
  f->nphdr = sub[0].n - 1; f->npbag = sub[1].n - 1; f->npmod = sub[2].n - 1; f->npgen = sub[3].n - 1;
  f->ninst = sub[4].n - 1; f->nibag = sub[5].n - 1; f->nimod = sub[6].n - 1; f->nigen = sub[7].n - 1;
  f->nshdr = sub[8].n - 1;
  if (f->nphdr < 1) { snprintf(why, wcap, "it has no presets"); return 0; }

  f->phdr = calloc(sub[0].n, sizeof *f->phdr); f->pbag = calloc(sub[1].n, sizeof *f->pbag);
  f->pmod = calloc(sub[2].n, sizeof *f->pmod); f->pgen = calloc(sub[3].n, sizeof *f->pgen);
  f->inst = calloc(sub[4].n, sizeof *f->inst); f->ibag = calloc(sub[5].n, sizeof *f->ibag);
  f->imod = calloc(sub[6].n, sizeof *f->imod); f->igen = calloc(sub[7].n, sizeof *f->igen);
  f->shdr = calloc(sub[8].n, sizeof *f->shdr); f->order = calloc(sub[0].n, sizeof *f->order);
  if (!f->phdr || !f->pbag || !f->pmod || !f->pgen || !f->inst || !f->ibag || !f->imod || !f->igen || !f->shdr || !f->order) {
    snprintf(why, wcap, "out of memory"); return 0;
  }
  for (i = 0; i < (int)sub[0].n; i++) {
    const uint8_t *r = sub[0].p + (size_t)i * 38;
    name20(f->phdr[i].name, r);
    f->phdr[i].program = u16(r + 20); f->phdr[i].bank = u16(r + 22); f->phdr[i].bag = u16(r + 24);
  }
  for (i = 0; i < (int)sub[4].n; i++) {
    const uint8_t *r = sub[4].p + (size_t)i * 22;
    name20(f->inst[i].name, r);
    f->inst[i].bag = u16(r + 20);
  }
  read_bags(f->pbag, sub[1].p, sub[1].n); read_mods(f->pmod, sub[2].p, sub[2].n); read_gens(f->pgen, sub[3].p, sub[3].n);
  read_bags(f->ibag, sub[5].p, sub[5].n); read_mods(f->imod, sub[6].p, sub[6].n); read_gens(f->igen, sub[7].p, sub[7].n);
  for (i = 0; i < (int)sub[8].n; i++) {
    const uint8_t *r = sub[8].p + (size_t)i * 46;
    SmpSfSample *s = &f->shdr[i];
    name20(s->name, r);
    s->start = u32(r + 20); s->end = u32(r + 24); s->loop_start = u32(r + 28); s->loop_end = u32(r + 32);
    s->rate = u32(r + 36); s->pitch = r[40]; s->correction = (int8_t)r[41]; s->link = u16(r + 42); s->type = u16(r + 44);
  }

  /* zone lists and the indices that cross from one list to the next */
  if (!bags_ok(&f->phdr[0].bag, sizeof(SfPhdr), f->nphdr, f->npbag)) { snprintf(why, wcap, "a preset's zones are out of range"); return 0; }
  if (!gens_ok(f->pbag, f->npbag, f->npgen, f->npmod)) { snprintf(why, wcap, "a preset zone's settings are out of range"); return 0; }
  if (!bags_ok(&f->inst[0].bag, sizeof(SfInst), f->ninst, f->nibag)) { snprintf(why, wcap, "an instrument's zones are out of range"); return 0; }
  if (!gens_ok(f->ibag, f->nibag, f->nigen, f->nimod)) { snprintf(why, wcap, "an instrument zone's settings are out of range"); return 0; }
  /* instrument and sample ids are checked only in zones a preset or instrument owns; real fonts sometimes
     carry junk in records no zone reaches, and those are never read */
  for (i = 0; i < f->nphdr; i++) {
    int b, g;
    for (b = f->phdr[i].bag; b < f->phdr[i + 1].bag; b++)
      for (g = f->pbag[b].gen; g < f->pbag[b + 1].gen; g++)
        if (f->pgen[g].oper == SF_GEN_INSTRUMENT && (uint16_t)f->pgen[g].amount >= f->ninst) {
          snprintf(why, wcap, "a preset uses instrument %u, but there are %d", (uint16_t)f->pgen[g].amount, f->ninst);
          return 0;
        }
  }
  for (i = 0; i < f->ninst; i++) {
    int b, g;
    for (b = f->inst[i].bag; b < f->inst[i + 1].bag; b++)
      for (g = f->ibag[b].gen; g < f->ibag[b + 1].gen; g++)
        if (f->igen[g].oper == SF_GEN_SAMPLEID && (uint16_t)f->igen[g].amount >= f->nshdr) {
          snprintf(why, wcap, "an instrument uses sample %u, but there are %d", (uint16_t)f->igen[g].amount, f->nshdr);
          return 0;
        }
  }
  for (i = 0; i < f->nshdr; i++) {
    SmpSfSample *s = &f->shdr[i];
    if ((s->type & (SF_RIGHT | SF_LEFT | SF_LINKED)) && s->link >= f->nshdr) {
      snprintf(why, wcap, "sample %s is linked to sample %u, but there are %d", s->name, s->link, f->nshdr); return 0;
    }
    if (s->type & SF_VORBIS) f->is_sf3 = 1;
  }
  return 1;
}

/* the sorted preset index: bank, then program, then file order (so the first of two duplicates wins) */
static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}
static int build_order(SmpFont *f) {
  uint64_t *k = malloc((size_t)f->nphdr * sizeof *k); int i;
  if (!k) return 0;
  for (i = 0; i < f->nphdr; i++) k[i] = (uint64_t)f->phdr[i].bank << 32 | (uint64_t)f->phdr[i].program << 16 | (uint64_t)i;
  qsort(k, f->nphdr, sizeof *k, cmp_u64);
  for (i = 0; i < f->nphdr; i++) f->order[i] = (int)(k[i] & 0xffff);
  free(k);
  return 1;
}

/* sample positions are kept inside the sample data; loop points are left as written (the voice clamps them) */
static void clamp_samples(SmpFont *f) {
  int i;
  for (i = 0; i < f->nshdr; i++) {
    SmpSfSample *s = &f->shdr[i];
    uint64_t l = (s->type & SF_VORBIS) ? f->smpl_size : f->smpl_size / 2;   /* SF3: bytes; SF2: frames */
    if (s->end > l) s->end = (uint32_t)l;
    if (s->start > s->end) s->start = s->end;
  }
}

/* ---- flattening presets into regions (SF2.04 sections 8 and 9) ---- */

/* each generator's default and range, from SF2.04 section 8.1.3 (the same numbers as FluidSynth's gen table).
   The range holds the final value, instrument plus preset. keynum, velocity and overridingRootKey go down to -1,
   which means "not set". The sample offsets and sampleModes have no range here: the voice keeps the offsets
   inside the sample. where says at which levels a generator counts; the ones with neither (unused, reserved, and
   the instrument, range and sample generators, which are handled apart) are never stored. */
enum { AT_INST = 1, AT_PRESET = 2, AT_BOTH = 3 };
typedef struct { int16_t def, lo, hi; uint8_t where; } GenInfo;
#define ANY -32768, 32767
static const GenInfo GENS[SMP_SF_NGEN] = {
  {0, ANY, AT_INST},                 /* 0 startAddrsOffset */
  {0, ANY, AT_INST},                 /* 1 endAddrsOffset */
  {0, ANY, AT_INST},                 /* 2 startloopAddrsOffset */
  {0, ANY, AT_INST},                 /* 3 endloopAddrsOffset */
  {0, ANY, AT_INST},                 /* 4 startAddrsCoarseOffset */
  {0, -12000, 12000, AT_BOTH},       /* 5 modLfoToPitch */
  {0, -12000, 12000, AT_BOTH},       /* 6 vibLfoToPitch */
  {0, -12000, 12000, AT_BOTH},       /* 7 modEnvToPitch */
  {13500, 1500, 13500, AT_BOTH},     /* 8 initialFilterFc */
  {0, 0, 960, AT_BOTH},              /* 9 initialFilterQ */
  {0, -12000, 12000, AT_BOTH},       /* 10 modLfoToFilterFc */
  {0, -12000, 12000, AT_BOTH},       /* 11 modEnvToFilterFc */
  {0, ANY, AT_INST},                 /* 12 endAddrsCoarseOffset */
  {0, -960, 960, AT_BOTH},           /* 13 modLfoToVolume */
  {0, 0, 0, 0},                      /* 14 unused */
  {0, 0, 1000, AT_BOTH},             /* 15 chorusEffectsSend */
  {0, 0, 1000, AT_BOTH},             /* 16 reverbEffectsSend */
  {0, -500, 500, AT_BOTH},           /* 17 pan */
  {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},   /* 18 to 20 unused */
  {-12000, -12000, 5000, AT_BOTH},   /* 21 delayModLFO */
  {0, -16000, 4500, AT_BOTH},        /* 22 freqModLFO */
  {-12000, -12000, 5000, AT_BOTH},   /* 23 delayVibLFO */
  {0, -16000, 4500, AT_BOTH},        /* 24 freqVibLFO */
  {-12000, -12000, 5000, AT_BOTH},   /* 25 delayModEnv */
  {-12000, -12000, 8000, AT_BOTH},   /* 26 attackModEnv */
  {-12000, -12000, 5000, AT_BOTH},   /* 27 holdModEnv */
  {-12000, -12000, 8000, AT_BOTH},   /* 28 decayModEnv */
  {0, 0, 1000, AT_BOTH},             /* 29 sustainModEnv */
  {-12000, -12000, 8000, AT_BOTH},   /* 30 releaseModEnv */
  {0, -1200, 1200, AT_BOTH},         /* 31 keynumToModEnvHold */
  {0, -1200, 1200, AT_BOTH},         /* 32 keynumToModEnvDecay */
  {-12000, -12000, 5000, AT_BOTH},   /* 33 delayVolEnv */
  {-12000, -12000, 8000, AT_BOTH},   /* 34 attackVolEnv */
  {-12000, -12000, 5000, AT_BOTH},   /* 35 holdVolEnv */
  {-12000, -12000, 8000, AT_BOTH},   /* 36 decayVolEnv */
  {0, 0, 1440, AT_BOTH},             /* 37 sustainVolEnv */
  {-12000, -12000, 8000, AT_BOTH},   /* 38 releaseVolEnv */
  {0, -1200, 1200, AT_BOTH},         /* 39 keynumToVolEnvHold */
  {0, -1200, 1200, AT_BOTH},         /* 40 keynumToVolEnvDecay */
  {0, 0, 0, 0},                      /* 41 instrument */
  {0, 0, 0, 0},                      /* 42 reserved */
  {0, 0, 0, 0},                      /* 43 keyRange */
  {0, 0, 0, 0},                      /* 44 velRange */
  {0, ANY, AT_INST},                 /* 45 startloopAddrsCoarseOffset */
  {-1, -1, 127, AT_INST},            /* 46 keynum */
  {-1, -1, 127, AT_INST},            /* 47 velocity */
  {0, ANY, AT_BOTH},                 /* 48 initialAttenuation: not limited here; the voice limits 0.4 x it plus the
                                        modulators to 0..1440, as FluidSynth does, so a negative value counts */
  {0, 0, 0, 0},                      /* 49 reserved */
  {0, ANY, AT_INST},                 /* 50 endloopAddrsCoarseOffset */
  {0, -120, 120, AT_BOTH},           /* 51 coarseTune */
  {0, -99, 99, AT_BOTH},             /* 52 fineTune */
  {0, 0, 0, 0},                      /* 53 sampleID */
  {0, ANY, AT_INST},                 /* 54 sampleModes */
  {0, 0, 0, 0},                      /* 55 reserved */
  {100, 0, 1200, AT_BOTH},           /* 56 scaleTuning */
  {0, 0, 127, AT_INST},              /* 57 exclusiveClass */
  {-1, -1, 127, AT_INST},            /* 58 overridingRootKey */
  {0, 0, 0, 0},                      /* 59 unused */
};
#undef ANY

/* SF2.04 section 8.4's default modulators. The velocity to filter cutoff one is present (so a font's identical
   modulator still replaces it) but its amount is 0: the 2.04 default (-2400 cents) is widely treated as an
   erratum (it changed between spec versions, and FluidSynth deliberately disables it); with it, a velocity-100
   hi-hat came out 1.4 dB quieter and 11 % duller than in FluidSynth (Task 7). The CC10 one's amount is 500,
   not the spec's 1000: with a bipolar source (-1..1) 1000 would reach a hard pan at CC10 32 and 96, against the
   spec's own intent that 0..127 spans left to right; FluidSynth uses 500 for the same reason (measured in Task 7).
   The pitch wheel one goes to 59, "initial pitch", which has no generator of its own (FluidSynth uses 59 the same
   way). */
static const SmpSfMod DEFMODS[SF_NDEFMOD] = {
  {0x0502, 48, 960, 0, 0},        /* note-on velocity to initial attenuation */
  {0x0102, 8, 0, 0, 0},           /* note-on velocity to filter cutoff: present, amount 0 (see above) */
  {0x000D, 6, 50, 0, 0},          /* channel pressure to vibrato LFO pitch depth */
  {0x0081, 6, 50, 0, 0},          /* CC1 (mod wheel) to vibrato LFO pitch depth */
  {0x0587, 48, 960, 0, 0},        /* CC7 (volume) to initial attenuation */
  {0x028A, 17, 500, 0, 0},        /* CC10 (pan) to pan: 500, see below */
  {0x058B, 48, 960, 0, 0},        /* CC11 (expression) to initial attenuation */
  {0x00DB, 16, 200, 0, 0},        /* CC91 to reverb send */
  {0x00DD, 15, 200, 0, 0},        /* CC93 to chorus send */
  {0x020E, 59, 12700, 0x0010, 0}, /* pitch wheel to initial pitch, scaled by the bend range */
};

/* a fair cap on how many regions one font may make, so a damaged file can't ask for gigabytes
   (a big General MIDI font makes a few thousand) */
#define MAX_REGIONS (1 << 17)

/* one zone's generators as read: the values it sets (a bit per generator in set), its ranges, and the instrument
   or sample id it ends with (link; -1 when there is none, which makes a first zone the global one) */
typedef struct {
  int16_t v[SMP_SF_NGEN]; uint64_t set;
  uint8_t lokey, hikey, lovel, hivel;
  int link;
} Zone;

static void read_zone(const SfGen *gen, int g0, int g1, unsigned link_oper, int where, Zone *z) {
  int g;
  memset(z->v, 0, sizeof z->v);
  z->set = 0; z->link = -1;
  z->lokey = z->lovel = 0; z->hikey = z->hivel = 127;
  for (g = g0; g < g1; g++) {
    unsigned op = gen[g].oper; int a = gen[g].amount;
    /* the instrument or sample generator ends the zone; anything after it is ignored, as the spec says */
    if (op == link_oper) { z->link = (uint16_t)a; break; }
    if (op == SF_GEN_KEYRANGE || op == SF_GEN_VELRANGE) {
      int lo = a & 255, hi = (a >> 8) & 255;
      /* a key range counts only first, and a velocity range only first or right after the key range */
      if (op == SF_GEN_KEYRANGE ? g != g0 : !(g == g0 || (g == g0 + 1 && gen[g0].oper == SF_GEN_KEYRANGE))) continue;
      if (hi > 127) hi = 127;
      if (op == SF_GEN_KEYRANGE) { z->lokey = (uint8_t)lo; z->hikey = (uint8_t)hi; }
      else { z->lovel = (uint8_t)lo; z->hivel = (uint8_t)hi; }
      z->set |= 1ull << op;
      continue;
    }
    if (op >= SMP_SF_NGEN || !(GENS[op].where & where)) continue;
    z->v[op] = (int16_t)a; z->set |= 1ull << op;   /* a generator given twice: the later one counts */
  }
}

/* a zone's value for generator i: its own, else the global zone's, else dflt */
static int zone_val(const Zone *local, const Zone *global, int i, int dflt) {
  if (local->set >> i & 1) return local->v[i];
  if (global && (global->set >> i & 1)) return global->v[i];
  return dflt;
}

/* identical modulators have the same source, destination, amount source and transform; only the amount differs */
static int same_mod(const SmpSfMod *a, const SmpSfMod *b) {
  return a->src == b->src && a->dest == b->dest && a->amt_src == b->amt_src && a->trans == b->trans;
}

/* a modulator source or amount source the spec knows (SF2.04 sections 8.2.1 and 8.2.2): a type of linear,
   concave, convex or switch, and either one of the general controllers or a MIDI controller that isn't one of
   the reserved ones (bank select, data entry, the RPN and NRPN numbers, the channel mode messages) */
static int source_ok(unsigned s) {
  unsigned idx = s & 127;
  if ((s >> 10) > 3) return 0;
  if (s & 0x80) return !(idx == 0 || idx == 6 || idx == 32 || idx == 38 || (idx >= 98 && idx <= 101) || idx >= 120);
  return idx == 0 || idx == 2 || idx == 3 || idx == 10 || idx == 13 || idx == 14 || idx == 16;
}

/* a modulator this player uses: known sources, a real generator as the destination, and a linear or absolute
   value transform. Others are ignored (section 8.2.1). Linked modulators (a destination with bit 15 set, or a
   source of 127, "link") are ignored too, as FluidSynth does. */
static int mod_ok(const SmpSfMod *m) {
  if (!source_ok(m->src) || !source_ok(m->amt_src)) return 0;
  /* 59 is "initial pitch", the default pitch wheel modulator's destination, so a font can replace that one */
  if (m->dest >= SMP_SF_NGEN || !(GENS[m->dest].where || m->dest == 59)) return 0;
  return m->trans == 0 || m->trans == 2;
}

/* at most this many modulators are read from one zone (the rest are ignored), so a damaged or hostile file
   can't make merging take hours. Real zones have a handful. */
#define MAX_ZONE_MODS 1024

/* adds a zone's modulators to list (n long): each replaces an identical one already there, or is added to the end.
   Invalid ones are skipped. Within one zone a later identical modulator replaces the earlier one, as SF2.04
   section 7.4 says. Returns the new length. */
static int merge_mods(SmpSfMod *list, int n, const SmpSfMod *zm, int nz) {
  int j, k;
  if (nz > MAX_ZONE_MODS) nz = MAX_ZONE_MODS;
  for (j = 0; j < nz; j++) {
    if (!mod_ok(&zm[j])) continue;
    for (k = 0; k < n; k++) if (same_mod(&list[k], &zm[j])) break;
    list[k] = zm[j];
    if (k == n) n++;
  }
  return n;
}

/* the modulator lists, stored once each: identical lists share one copy. The defaults are always at 0. */
typedef struct {
  SmpSfMod *m; int n, cap;
  int *slot_off, *slot_n; unsigned mask;   /* a hash table of the lists stored so far; slot_n 0 is empty */
} Pool;

static unsigned hash_mods(const SmpSfMod *m, int n) {
  const uint8_t *p = (const uint8_t *)m; size_t i, len = (size_t)n * sizeof *m; unsigned h = 2166136261u;
  for (i = 0; i < len; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

/* the most modulators all the lists together may hold (10 MB), so a hostile file can't fill memory */
#define MAX_POOL_MODS (1 << 20)

/* the offset of this list in the pool, adding it if it isn't there yet; -1 when out of memory, -2 when the
   pool would grow past MAX_POOL_MODS */
static int pool_add(Pool *p, const SmpSfMod *m, int n) {
  unsigned s = hash_mods(m, n) & p->mask;
  while (p->slot_n[s]) {
    if (p->slot_n[s] == n && !memcmp(p->m + p->slot_off[s], m, (size_t)n * sizeof *m)) return p->slot_off[s];
    s = (s + 1) & p->mask;
  }
  if (p->n + n > MAX_POOL_MODS) return -2;
  if (p->n + n > p->cap) {
    int cap = p->cap * 2 > p->n + n ? p->cap * 2 : p->n + n;
    SmpSfMod *g = realloc(p->m, (size_t)cap * sizeof *g);
    if (!g) return -1;
    p->m = g; p->cap = cap;
  }
  memcpy(p->m + p->n, m, (size_t)n * sizeof *m);
  p->slot_off[s] = p->n; p->slot_n[s] = n;
  p->n += n;
  return p->slot_off[s];
}

/* flattens every preset into regions: a preset zone and an instrument zone make a region where their key and
   velocity ranges meet. Instrument generators are absolute (the zone's, the global zone's, or the default);
   preset ones are added on top, and the sum is clamped to the generator's range. Everything goes into one
   allocation at the end, which nothing changes until the font is closed. */
static int build_regions(SmpFont *f, char *why, int wcap) {
  SmpSfRegion *reg = NULL; int nreg = 0, capreg = 0;
  int *ioff = NULL;                               /* each region's two lists, as pool offsets, until the end */
  int *icache = NULL, *pcache = NULL;             /* per instrument and preset zone: its list's offset, or -1 */
  int *icount = NULL, *pcount = NULL;
  int *ibase = NULL, *ibase_n = NULL;             /* per instrument: its defaults-plus-global list, or -1 */
  SmpSfMod *scratch = NULL, *pbase = NULL;        /* pbase: the current preset's global list */
  Pool pool = {0};
  int *rstart = NULL; int ok = 0, p, i, nslots = 1, got;
  size_t sreg, sstart, total; char *block;

  /* a list per instrument zone, preset zone and instrument, and the defaults: twice that many slots */
  while (nslots < 2 * (f->nibag + f->npbag + f->ninst + 2)) nslots *= 2;
  icache = malloc((size_t)(f->nibag + 1) * sizeof *icache); icount = malloc((size_t)(f->nibag + 1) * sizeof *icount);
  pcache = malloc((size_t)(f->npbag + 1) * sizeof *pcache); pcount = malloc((size_t)(f->npbag + 1) * sizeof *pcount);
  ibase = malloc((size_t)(f->ninst + 1) * sizeof *ibase); ibase_n = malloc((size_t)(f->ninst + 1) * sizeof *ibase_n);
  scratch = malloc((size_t)(SF_NDEFMOD + 2 * MAX_ZONE_MODS + 1) * sizeof *scratch);
  pbase = malloc((size_t)(MAX_ZONE_MODS + 1) * sizeof *pbase);
  rstart = malloc((size_t)(f->nphdr + 1) * sizeof *rstart);
  pool.slot_off = calloc(nslots, sizeof *pool.slot_off); pool.slot_n = calloc(nslots, sizeof *pool.slot_n);
  pool.mask = (unsigned)nslots - 1;
  if (!icache || !icount || !pcache || !pcount || !ibase || !ibase_n || !scratch || !pbase || !rstart ||
      !pool.slot_off || !pool.slot_n) goto oom;
  for (i = 0; i <= f->nibag; i++) icache[i] = -1;
  for (i = 0; i <= f->npbag; i++) pcache[i] = -1;
  for (i = 0; i <= f->ninst; i++) ibase[i] = -1;
  if (pool_add(&pool, DEFMODS, SF_NDEFMOD) != 0) goto oom;

  for (p = 0; p < f->nphdr; p++) {
    int b0 = f->phdr[p].bag, b1 = f->phdr[p + 1].bag, b, npbase = -1;
    Zone pg, pz; int have_pg = 0;
    rstart[p] = nreg;
    for (b = b0; b < b1; b++) {
      int ins, ib0, ib1, ib, have_ig = 0, padd[SMP_SF_NGEN];
      uint8_t pklo, pkhi, pvlo, pvhi;
      Zone ig, iz;
      read_zone(f->pgen, f->pbag[b].gen, f->pbag[b + 1].gen, SF_GEN_INSTRUMENT, AT_PRESET, &pz);
      if (pz.link < 0) {   /* a first zone with no instrument is the global one; any later one is ignored */
        if (b == b0) { pg = pz; have_pg = 1; }
        continue;
      }
      for (i = 0; i < SMP_SF_NGEN; i++) padd[i] = zone_val(&pz, have_pg ? &pg : NULL, i, 0);
      /* ranges: the zone's own, else the global zone's, else every key and velocity */
      if (!(pz.set >> SF_GEN_KEYRANGE & 1) && have_pg) { pz.lokey = pg.lokey; pz.hikey = pg.hikey; }
      if (!(pz.set >> SF_GEN_VELRANGE & 1) && have_pg) { pz.lovel = pg.lovel; pz.hivel = pg.hivel; }
      pklo = pz.lokey; pkhi = pz.hikey; pvlo = pz.lovel; pvhi = pz.hivel;
      if (pcache[b] < 0) {   /* the preset list: the global zone's modulators (merged once), then this zone's */
        int n;
        if (npbase < 0)
          npbase = have_pg ? merge_mods(pbase, 0, f->pmod + f->pbag[b0].mod, f->pbag[b0 + 1].mod - f->pbag[b0].mod) : 0;
        memcpy(scratch, pbase, (size_t)npbase * sizeof *scratch);
        n = merge_mods(scratch, npbase, f->pmod + f->pbag[b].mod, f->pbag[b + 1].mod - f->pbag[b].mod);
        pcount[b] = n;
        pcache[b] = got = n ? pool_add(&pool, scratch, n) : 0;
        if (got < 0) goto pool_bad;
      }
      ins = pz.link; ib0 = f->inst[ins].bag; ib1 = f->inst[ins + 1].bag;
      for (ib = ib0; ib < ib1; ib++) {
        SmpSfRegion *r; uint8_t klo, khi, vlo, vhi; int s;
        read_zone(f->igen, f->ibag[ib].gen, f->ibag[ib + 1].gen, SF_GEN_SAMPLEID, AT_INST, &iz);
        if (iz.link < 0) {
          if (ib == ib0) { ig = iz; have_ig = 1; }
          continue;
        }
        s = iz.link;
        if (f->shdr[s].type & SF_ROM) continue;   /* the sound card's own memory: nothing here to play */
        if (!(iz.set >> SF_GEN_KEYRANGE & 1) && have_ig) { iz.lokey = ig.lokey; iz.hikey = ig.hikey; }
        if (!(iz.set >> SF_GEN_VELRANGE & 1) && have_ig) { iz.lovel = ig.lovel; iz.hivel = ig.hivel; }
        klo = iz.lokey > pklo ? iz.lokey : pklo; khi = iz.hikey < pkhi ? iz.hikey : pkhi;
        vlo = iz.lovel > pvlo ? iz.lovel : pvlo; vhi = iz.hivel < pvhi ? iz.hivel : pvhi;
        if (klo > khi || vlo > vhi) continue;
        if (icache[ib] < 0) {   /* the instrument list: the defaults, the global zone's modulators, then this zone's */
          int n;
          if (ibase[ins] < 0) {   /* the defaults and the global zone, merged once per instrument */
            memcpy(scratch, DEFMODS, sizeof DEFMODS);
            n = have_ig ? merge_mods(scratch, SF_NDEFMOD, f->imod + f->ibag[ib0].mod, f->ibag[ib0 + 1].mod - f->ibag[ib0].mod)
                        : SF_NDEFMOD;
            ibase_n[ins] = n;
            ibase[ins] = got = pool_add(&pool, scratch, n);
            if (got < 0) goto pool_bad;
          }
          memcpy(scratch, pool.m + ibase[ins], (size_t)ibase_n[ins] * sizeof *scratch);
          n = merge_mods(scratch, ibase_n[ins], f->imod + f->ibag[ib].mod, f->ibag[ib + 1].mod - f->ibag[ib].mod);
          icount[ib] = n;
          icache[ib] = got = pool_add(&pool, scratch, n);
          if (got < 0) goto pool_bad;
        }
        if (nreg == capreg) {
          int cap = capreg ? capreg * 2 : 256; void *g;
          if (nreg >= MAX_REGIONS) { snprintf(why, wcap, "it has too many regions"); goto fail; }
          if (!(g = realloc(reg, (size_t)cap * sizeof *reg))) goto oom;
          reg = g;
          if (!(g = realloc(ioff, (size_t)cap * 2 * sizeof *ioff))) goto oom;
          ioff = g;
          capreg = cap;
        }
        r = &reg[nreg];
        memset(r, 0, sizeof *r);
        r->lokey = klo; r->hikey = khi; r->lovel = vlo; r->hivel = vhi; r->sample = s;
        for (i = 0; i < SMP_SF_NGEN; i++) {
          int v = zone_val(&iz, have_ig ? &ig : NULL, i, GENS[i].def);
          if (GENS[i].where & AT_PRESET) v += padd[i];
          r->gen[i] = (int16_t)(v < GENS[i].lo ? GENS[i].lo : v > GENS[i].hi ? GENS[i].hi : v);
        }
        r->gen[SF_GEN_KEYRANGE] = (int16_t)(klo | khi << 8);
        r->gen[SF_GEN_VELRANGE] = (int16_t)(vlo | vhi << 8);
        r->nimod = icount[ib]; r->npmod = pcount[b];
        ioff[2 * nreg] = icache[ib]; ioff[2 * nreg + 1] = pcache[b];
        nreg++;
      }
    }
  }
  rstart[f->nphdr] = nreg;

  /* the one block: regions, then the per-preset starts, then the modulator lists */
  sreg = (size_t)nreg * sizeof(SmpSfRegion);
  sstart = (size_t)(f->nphdr + 1) * sizeof(int);
  total = sreg + sstart + (size_t)pool.n * sizeof(SmpSfMod);
  block = malloc(total ? total : 1);
  if (!block) goto oom;
  f->regions = (SmpSfRegion *)block;
  f->rstart = (int *)(block + sreg);
  memcpy(f->regions, reg, sreg);
  memcpy(f->rstart, rstart, sstart);
  memcpy(block + sreg + sstart, pool.m, (size_t)pool.n * sizeof(SmpSfMod));
  for (i = 0; i < nreg; i++) {
    const SmpSfMod *m = (const SmpSfMod *)(block + sreg + sstart);
    f->regions[i].imod = m + ioff[2 * i];
    f->regions[i].pmod = f->regions[i].npmod ? m + ioff[2 * i + 1] : NULL;
  }
  f->nregions = nreg;
  ok = 1;
  goto fail;
pool_bad:
  if (got == -2) { snprintf(why, wcap, "it has too many modulators"); goto fail; }
oom:
  snprintf(why, wcap, "out of memory");
fail:
  free(reg); free(ioff); free(icache); free(pcache); free(icount); free(pcount); free(scratch);
  free(ibase); free(ibase_n); free(pbase);
  free(rstart); free(pool.m); free(pool.slot_off); free(pool.slot_n);
  return ok;
}

/* parses a font's headers into a new SmpFont (sffont.c's smp_sf_open shares it through the registry) */
SmpFont *sf_parse(const char *path, char *err, int cap) {
  wchar_t w[2048]; Src s; LARGE_INTEGER sz; FILETIME mt; uint8_t h[12];
  uint64_t pos, end, pdta_off = 0; uint32_t pdta_size = 0; int have_pdta = 0, have_smpl = 0, ok = 0;
  char why[200] = "";
  SmpFont *f;
  uint8_t *pd = NULL;
  if (err && cap > 0) err[0] = 0;
  if (!path || !MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048)) {
    if (err && cap > 0) snprintf(err, cap, "can't open %s", path ? base_name(path) : "(no file)");
    return NULL;
  }
  s.h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
  if (s.h == INVALID_HANDLE_VALUE) {
    if (err && cap > 0) snprintf(err, cap, "can't open %s", base_name(path));
    return NULL;
  }
  if (!GetFileSizeEx(s.h, &sz) || !GetFileTime(s.h, NULL, NULL, &mt)) {
    CloseHandle(s.h);
    if (err && cap > 0) snprintf(err, cap, "can't open %s", base_name(path));
    return NULL;
  }
  s.size = (uint64_t)sz.QuadPart;
  f = calloc(1, sizeof *f);
  if (!f) { CloseHandle(s.h); if (err && cap > 0) snprintf(err, cap, "out of memory opening %s", base_name(path)); return NULL; }
  f->size = s.size;
  f->mtime = (uint64_t)mt.dwHighDateTime << 32 | mt.dwLowDateTime;

  if (!rd(&s, 0, h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "sfbk", 4)) {
    if (err && cap > 0) snprintf(err, cap, "%s isn't a SoundFont", base_name(path));
    goto done;
  }
  /* some writers get the RIFF size slightly wrong; the file's own end is the hard limit */
  end = 8 + (uint64_t)u32(h + 4);
  if (end > s.size) end = s.size;
  for (pos = 12; pos + 8 <= end; ) {
    uint8_t c[12]; uint32_t n;
    if (!rd(&s, pos, c, 8)) { snprintf(why, sizeof why, "it can't be read"); goto bad; }
    n = u32(c + 4);
    if (n > end - pos - 8) { snprintf(why, sizeof why, "it is cut short"); goto bad; }
    if (!memcmp(c, "LIST", 4) && n >= 4) {
      uint64_t sp, send = pos + 8 + n;
      if (!rd(&s, pos + 8, c + 8, 4)) { snprintf(why, sizeof why, "it can't be read"); goto bad; }
      if (!memcmp(c + 8, "pdta", 4)) {
        if (!have_pdta) { pdta_off = pos + 12; pdta_size = n - 4; have_pdta = 1; }
      } else if (!memcmp(c + 8, "sdta", 4) || !memcmp(c + 8, "INFO", 4)) {
        for (sp = pos + 12; sp + 8 <= send; ) {
          uint8_t d[8]; uint32_t m;
          if (!rd(&s, sp, d, 8)) { snprintf(why, sizeof why, "it can't be read"); goto bad; }
          m = u32(d + 4);
          if (m > send - sp - 8) { snprintf(why, sizeof why, "the %.4s chunk runs past its list", d); goto bad; }
          if (!memcmp(d, "smpl", 4) && !have_smpl) { f->smpl_off = sp + 8; f->smpl_size = m; have_smpl = 1; }
          else if (!memcmp(d, "sm24", 4) && !f->sm24_size) { f->sm24_off = sp + 8; f->sm24_size = m; }
          else if (!memcmp(d, "ifil", 4) && m >= 4) {
            uint8_t v[4];
            if (!rd(&s, sp + 8, v, 4)) { snprintf(why, sizeof why, "it can't be read"); goto bad; }
            if (u16(v) < 2 || u16(v) > 3) { snprintf(why, sizeof why, "it is version %u, and only 2 and 3 can be read", u16(v)); goto bad; }
          }
          sp += 8 + (uint64_t)m + (m & 1);
        }
      }
    }
    pos += 8 + (uint64_t)n + (n & 1);
  }
  if (!have_pdta) { snprintf(why, sizeof why, "it has no preset list"); goto bad; }
  if (!have_smpl) { snprintf(why, sizeof why, "it has no sample data"); goto bad; }
  pd = malloc(pdta_size ? pdta_size : 1);
  if (!pd) { snprintf(why, sizeof why, "out of memory"); goto bad; }
  if (!rd(&s, pdta_off, pd, pdta_size)) { snprintf(why, sizeof why, "it can't be read"); goto bad; }
  if (!parse_pdta(f, pd, pdta_size, why, sizeof why)) goto bad;
  if (!build_order(f)) { snprintf(why, sizeof why, "out of memory"); goto bad; }
  if (!build_regions(f, why, sizeof why)) goto bad;
  /* 24-bit low bytes only count when there is one for every 16-bit sample */
  if (f->sm24_size && (f->is_sf3 || f->sm24_size < f->smpl_size / 2)) f->sm24_off = f->sm24_size = 0;
  clamp_samples(f);
  f->path = _strdup(path);
  if (!f->path) { snprintf(why, sizeof why, "out of memory"); goto bad; }
  ok = 1;
  goto done;
bad:
  why_bad(err, cap, path, why);
done:
  free(pd);
  CloseHandle(s.h);
  if (!ok) { sf_free(f); return NULL; }
  return f;
}

/* frees what sf_parse made (sffont.c frees the loader's arrays and the sample data first) */
void sf_free(SmpFont *f) {
  if (!f) return;
  free(f->path); free(f->phdr); free(f->pbag); free(f->pmod); free(f->pgen); free(f->inst);
  free(f->ibag); free(f->imod); free(f->igen); free(f->shdr); free(f->order);
  free(f->regions);   /* the one block that also holds rstart and the modulator lists */
  free(f);
}

int smp_sf_preset_count(const SmpFont *f) { return f ? f->nphdr : 0; }

int smp_sf_preset_info(const SmpFont *f, int i, int *bank, int *program, char *name, int cap) {
  const SfPhdr *p;
  if (!f || i < 0 || i >= f->nphdr) return 0;
  p = &f->phdr[f->order[i]];
  if (bank) *bank = p->bank;
  if (program) *program = p->program;
  if (name && cap > 0) snprintf(name, cap, "%s", p->name);
  return 1;
}

int smp_sf_find_preset(const SmpFont *f, int bank, int program) {
  int lo = 0, hi; uint32_t want;
  if (!f || bank < 0 || bank > 65535 || program < 0 || program > 65535) return -1;
  want = (uint32_t)bank << 16 | (uint32_t)program;
  hi = f->nphdr;
  while (lo < hi) {   /* the first preset at or after (bank, program) */
    int mid = (lo + hi) / 2; const SfPhdr *p = &f->phdr[f->order[mid]];
    if (((uint32_t)p->bank << 16 | p->program) < want) lo = mid + 1; else hi = mid;
  }
  if (lo < f->nphdr) {
    const SfPhdr *p = &f->phdr[f->order[lo]];
    if (p->bank == bank && p->program == program) return lo;
  }
  return -1;
}

int smp_sf_regions(const SmpFont *f, int preset, const SmpSfRegion **out) {
  int p, n;
  if (out) *out = NULL;
  if (!f || !f->regions || preset < 0 || preset >= f->nphdr) return 0;
  p = f->order[preset];
  n = f->rstart[p + 1] - f->rstart[p];
  if (out && n > 0) *out = f->regions + f->rstart[p];
  return n;
}

int smp_sf_sample_count(const SmpFont *f) { return f ? f->nshdr : 0; }

const SmpSfSample *smp_sf_sample(const SmpFont *f, int i) {
  if (!f || i < 0 || i >= f->nshdr) return NULL;
  return &f->shdr[i];
}
