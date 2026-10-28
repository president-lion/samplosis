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
  for (i = 0; i < f->npgen; i++)
    if (f->pgen[i].oper == SF_GEN_INSTRUMENT && (uint16_t)f->pgen[i].amount >= f->ninst) {
      snprintf(why, wcap, "a preset uses instrument %u, but there are %d", (uint16_t)f->pgen[i].amount, f->ninst); return 0;
    }
  for (i = 0; i < f->nigen; i++)
    if (f->igen[i].oper == SF_GEN_SAMPLEID && (uint16_t)f->igen[i].amount >= f->nshdr) {
      snprintf(why, wcap, "an instrument uses sample %u, but there are %d", (uint16_t)f->igen[i].amount, f->nshdr); return 0;
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

SmpFont *smp_sf_open(const char *path, char *err, int cap) {
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
  if (!ok) { smp_sf_close(f); return NULL; }
  return f;
}

void smp_sf_close(SmpFont *f) {
  if (!f) return;
  free(f->path); free(f->phdr); free(f->pbag); free(f->pmod); free(f->pgen); free(f->inst);
  free(f->ibag); free(f->imod); free(f->igen); free(f->shdr); free(f->order);
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

int smp_sf_sample_count(const SmpFont *f) { return f ? f->nshdr : 0; }

const SmpSfSample *smp_sf_sample(const SmpFont *f, int i) {
  if (!f || i < 0 || i >= f->nshdr) return NULL;
  return &f->shdr[i];
}
