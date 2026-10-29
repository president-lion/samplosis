/* sftest.c: smp_sf_test_font, for tests only. It builds a font in memory from a short description (see
   samplosis.h): generated 16-bit sine samples, presets, and regions with any generators and conditions. The
   samples go into a raw file at the given path, which the font reads like an SF2's smpl chunk (PCM_FILE, path 0),
   and the font is registered under that path, so a player loading the path shares it. Nothing here is used by
   the plugin itself; it lets the player's region conditions be tested before any loader makes them. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sf2.h"

#define PI 3.14159265358979323846
#define MAX_ITEMS 20000

typedef struct { char name[21]; uint32_t frames, rate, ls, le; int root; double hz, amp; } TSample;

static int fail(char *err, int cap, int line, const char *msg, const char *word) {
  if (err && cap > 0) snprintf(err, (size_t)cap, "line %d: %s%s%s", line, msg, word ? " " : "", word ? word : "");
  return 0;
}

static int trig_of(const char *v) {
  if (!strcmp(v, "attack")) return SMP_TRIG_ATTACK;
  if (!strcmp(v, "release")) return SMP_TRIG_RELEASE;
  if (!strcmp(v, "first")) return SMP_TRIG_FIRST;
  if (!strcmp(v, "legato")) return SMP_TRIG_LEGATO;
  return -1;
}

/* one name=value of a region line; 1 when it was understood */
static int region_opt(SmpSfRegion *r, const char *name, const char *v) {
  SmpSfCond *c = &r->cond; int n = atoi(v), k;
  if (!strcmp(name, "sample")) { r->sample = n; return 1; }
  if (!strcmp(name, "lokey")) { r->lokey = (uint8_t)n; return 1; }
  if (!strcmp(name, "hikey")) { r->hikey = (uint8_t)n; return 1; }
  if (!strcmp(name, "lovel")) { r->lovel = (uint8_t)n; return 1; }
  if (!strcmp(name, "hivel")) { r->hivel = (uint8_t)n; return 1; }
  if (name[0] == 'g' && sscanf(name + 1, "%d", &k) == 1 && k >= 0 && k < SMP_SF_NGEN) { r->gen[k] = (int16_t)n; return 1; }
  if (!strcmp(name, "trigger")) { int t = trig_of(v); if (t < 0) return 0; c->trigger = (uint8_t)t; return 1; }
  if (!strcmp(name, "seq_len")) { c->seq_len = (uint16_t)n; return 1; }
  if (!strcmp(name, "seq_pos")) { c->seq_pos = (uint16_t)n; return 1; }
  if (!strcmp(name, "lorand")) { c->lorand = (float)atof(v); return 1; }
  if (!strcmp(name, "hirand")) { c->hirand = (float)atof(v); return 1; }
  if (!strcmp(name, "sw_lokey")) { c->sw_lokey = (int8_t)n; return 1; }
  if (!strcmp(name, "sw_hikey")) { c->sw_hikey = (int8_t)n; return 1; }
  if (!strcmp(name, "sw_last")) { c->sw_last = (int8_t)n; return 1; }
  if (!strcmp(name, "sw_down")) { c->sw_down = (int8_t)n; return 1; }
  if (!strcmp(name, "sw_up")) { c->sw_up = (int8_t)n; return 1; }
  if (!strcmp(name, "sw_previous")) { c->sw_previous = (int8_t)n; return 1; }
  if (!strcmp(name, "sw_default")) { c->sw_default = (int8_t)n; return 1; }
  if ((!strncmp(name, "locc", 4) || !strncmp(name, "hicc", 4)) && sscanf(name + 4, "%d", &k) == 1 && k >= 0 && k < 128) {
    int i;
    for (i = 0; i < c->locc_n && c->locc[i].cc != k; i++) {}
    if (i == c->locc_n) {                       /* a new CC: the whole range until the other end is given */
      if (c->locc_n >= 4) return 0;
      c->locc[i].cc = (uint8_t)k; c->locc[i].lo = 0; c->locc[i].hi = 127; c->locc_n++;
    }
    if (name[0] == 'l') c->locc[i].lo = (uint8_t)n; else c->locc[i].hi = (uint8_t)n;
    return 1;
  }
  if (!strcmp(name, "xfin_lokey")) { c->xfin_lokey = (int8_t)n; return 1; }
  if (!strcmp(name, "xfin_hikey")) { c->xfin_hikey = (int8_t)n; return 1; }
  if (!strcmp(name, "xfout_lokey")) { c->xfout_lokey = (int8_t)n; return 1; }
  if (!strcmp(name, "xfout_hikey")) { c->xfout_hikey = (int8_t)n; return 1; }
  if (!strcmp(name, "xfin_lovel")) { c->xfin_lovel = (uint8_t)n; return 1; }
  if (!strcmp(name, "xfin_hivel")) { c->xfin_hivel = (uint8_t)n; return 1; }
  if (!strcmp(name, "xfout_lovel")) { c->xfout_lovel = (uint8_t)n; return 1; }
  if (!strcmp(name, "xfout_hivel")) { c->xfout_hivel = (uint8_t)n; return 1; }
  if (!strcmp(name, "xf_curve")) {
    if (!strcmp(v, "power")) c->xf_curve = 0; else if (!strcmp(v, "gain")) c->xf_curve = 1; else return 0;
    return 1;
  }
  if (!strcmp(name, "group")) { c->group = (uint32_t)strtoul(v, NULL, 10); return 1; }
  if (!strcmp(name, "off_by")) { c->off_by = (uint32_t)strtoul(v, NULL, 10); return 1; }
  if (!strcmp(name, "off_mode")) {
    if (!strcmp(v, "fast")) c->off_mode = 0; else if (!strcmp(v, "normal")) c->off_mode = 1; else return 0;
    return 1;
  }
  if (!strcmp(name, "rt_decay")) { c->rt_decay = (float)atof(v); return 1; }
  if (!strcmp(name, "amp_veltrack")) { c->amp_veltrack = (float)atof(v); return 1; }
  return 0;
}

SMP_API SmpFont *smp_sf_test_font(const char *desc, const char *path, char *err, int cap) {
  TSample *sm = NULL; SfPhdr *ph = NULL; SmpSfRegion *rg = NULL; int *rp = NULL;   /* rp: each region's preset */
  int ns = 0, np = 0, nr = 0, line = 0, ok = 0, i;
  const char *p = desc;
  SmpFont *f = NULL; FILE *out = NULL; wchar_t *full = NULL; wchar_t w[2048];
  WIN32_FILE_ATTRIBUTE_DATA a;
  uint64_t off = 0;
  if (err && cap > 0) err[0] = 0;
  if (!desc || !path) { fail(err, cap, 0, "no description or path", NULL); return NULL; }
  sm = calloc(MAX_ITEMS, sizeof *sm); ph = calloc(MAX_ITEMS + 1, sizeof *ph);
  rg = calloc(MAX_ITEMS, sizeof *rg); rp = calloc(MAX_ITEMS, sizeof *rp);
  if (!sm || !ph || !rg || !rp) { fail(err, cap, 0, "out of memory", NULL); goto done; }
  while (*p) {                                 /* the description, a line at a time */
    char buf[2048], word[64]; const char *nl = strchr(p, '\n'); size_t len = nl ? (size_t)(nl - p) : strlen(p);
    int used = 0;
    line++;
    if (len >= sizeof buf) { fail(err, cap, line, "too long", NULL); goto done; }
    memcpy(buf, p, len); buf[len] = 0;
    p = nl ? nl + 1 : p + len;
    if (sscanf(buf, "%63s%n", word, &used) != 1) continue;
    if (!strcmp(word, "sample")) {
      TSample *t = &sm[ns]; char kind[16]; int n2 = 0;
      if (ns >= MAX_ITEMS) { fail(err, cap, line, "too many samples", NULL); goto done; }
      if (sscanf(buf + used, "%20s %u %u %d %15s %lf %lf%n", t->name, &t->frames, &t->rate, &t->root, kind, &t->hz, &t->amp, &n2) != 7 ||
          strcmp(kind, "sine") || !t->frames || !t->rate) { fail(err, cap, line, "bad sample", NULL); goto done; }
      if (sscanf(buf + used + n2, " loop %u %u", &t->ls, &t->le) != 2) t->ls = t->le = 0;
      ns++;
    } else if (!strcmp(word, "preset")) {
      unsigned b, pr;
      if (np >= MAX_ITEMS || sscanf(buf + used, "%u %u %20s", &b, &pr, ph[np].name) != 3) { fail(err, cap, line, "bad preset", NULL); goto done; }
      ph[np].bank = (uint16_t)b; ph[np].program = (uint16_t)pr;
      np++;
    } else if (!strcmp(word, "region")) {
      SmpSfRegion *r = &rg[nr]; const char *q = buf + used; char tok[128];
      if (!np) { fail(err, cap, line, "a region before any preset", NULL); goto done; }
      if (nr >= MAX_ITEMS) { fail(err, cap, line, "too many regions", NULL); goto done; }
      sf_gen_defaults(r->gen);
      sf_cond_defaults(&r->cond);
      r->lokey = 0; r->hikey = 127; r->lovel = 0; r->hivel = 127;
      while (sscanf(q, "%127s%n", tok, &used) == 1) {
        char *eq = strchr(tok, '=');
        q += used;
        if (!eq) { fail(err, cap, line, "not name=value:", tok); goto done; }
        *eq = 0;
        if (!region_opt(r, tok, eq + 1)) { *eq = '='; fail(err, cap, line, "can't read", tok); goto done; }
      }
      if (r->sample < 0 || r->sample >= ns) { fail(err, cap, line, "no such sample", NULL); goto done; }
      r->gen[SF_GEN_KEYRANGE] = (int16_t)(r->lokey | r->hikey << 8);
      r->gen[SF_GEN_VELRANGE] = (int16_t)(r->lovel | r->hivel << 8);
      r->gen[SF_GEN_SAMPLEID] = (int16_t)r->sample;
      r->imod = sf_default_mods(); r->nimod = SF_NDEFMOD;
      rp[nr++] = np - 1;
    } else { fail(err, cap, line, "unknown item", word); goto done; }
  }
  if (!ns || !np) { fail(err, cap, line, "no samples or no presets", NULL); goto done; }

  /* the samples, one after another as 16-bit mono */
  if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048) || !(out = _wfopen(w, L"wb"))) {
    fail(err, cap, 0, "can't write", path); goto done;
  }
  f = calloc(1, sizeof *f);
  if (!f) { fail(err, cap, 0, "out of memory", NULL); goto done; }
  f->version = 2;
  f->shdr = calloc((size_t)ns + 1, sizeof *f->shdr);
  f->src = calloc((size_t)ns, sizeof *f->src);
  f->paths = calloc(1, sizeof *f->paths);
  f->phdr = calloc((size_t)np + 1, sizeof *f->phdr);
  f->order = calloc((size_t)np, sizeof *f->order);
  f->path = _strdup(path);
  if (!f->shdr || !f->src || !f->paths || !f->phdr || !f->order || !f->path || !(f->paths[0] = _strdup(path))) {
    fail(err, cap, 0, "out of memory", NULL); goto done;
  }
  f->npaths = 1;
  for (i = 0; i < ns; i++) {
    TSample *t = &sm[i]; SmpSfSample *h = &f->shdr[i]; uint32_t k;
    for (k = 0; k < t->frames; k++) {
      double y = t->amp * sin(2 * PI * t->hz * k / t->rate) * 32768.0;
      int16_t v = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : floor(y + 0.5));
      fwrite(&v, 2, 1, out);
    }
    snprintf(h->name, sizeof h->name, "%s", t->name);
    h->start = 0; h->end = t->frames; h->loop_start = t->ls; h->loop_end = t->le;
    h->rate = t->rate; h->pitch = (uint8_t)t->root; h->type = SF_MONO;
    f->src[i].kind = SMP_SRC_PCM_FILE; f->src[i].path_id = 0; f->src[i].offset = off;
    f->src[i].enc = SMP_PCM_S16LE; f->src[i].channels = 1; f->src[i].channel = 0;
    off += 2ull * t->frames;
    if (t->frames >= t->rate) f->has_long = 1;
  }
  if (fclose(out)) { out = NULL; fail(err, cap, 0, "can't write", path); goto done; }
  out = NULL;
  f->smpl_off = 0; f->smpl_size = off;
  f->nshdr = ns;

  /* presets in the given order, each owning its regions; the one block sf_free frees: regions, then rstart */
  memcpy(f->phdr, ph, (size_t)np * sizeof *ph);
  f->nphdr = np;
  {
    size_t sreg = (size_t)nr * sizeof(SmpSfRegion);
    char *block = malloc(sreg + ((size_t)np + 1) * sizeof(int));
    int k = 0;
    if (!block) { fail(err, cap, 0, "out of memory", NULL); goto done; }
    f->regions = (SmpSfRegion *)block;
    f->rstart = (int *)(block + sreg);
    for (i = 0; i < np; i++) {                 /* regions grouped by preset, keeping their order */
      int j;
      f->rstart[i] = k;
      for (j = 0; j < nr; j++) if (rp[j] == i) f->regions[k++] = rg[j];
    }
    f->rstart[np] = k;
    f->nregions = nr;
  }
  if (!sf_build_order(f)) { fail(err, cap, 0, "out of memory", NULL); goto done; }
  if (!GetFileAttributesExW(w, GetFileExInfoStandard, &a) || !(full = sf_full_path(path))) {
    fail(err, cap, 0, "can't read back", path); goto done;
  }
  f->size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
  f->mtime = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
  ok = 1;
done:
  if (out) fclose(out);
  free(sm); free(ph); free(rg); free(rp);
  if (!ok) { if (f) sf_free(f); free(full); return NULL; }
  return sf_register(f, full, err, cap);       /* frees the font on failure */
}
