/* multi.c: the Sampler with several samples. See samplosis.h.

   Each sample (a zone) plays in an engine of its own (engine.c, unchanged), so everything one sample does (streaming,
   keep-length copies, reversed copies, glide) is as before. The zones' key and velocity ranges reach their engines as
   the engine's own Lowest and Highest note and velocity: the zone's range inside the instrument's. Root note is each
   zone's own; the other parameters are the instrument's, the same in every engine.

   Threads. The zone list is the main thread's. The audio thread sees a snapshot of it (MSnap: the engines and their
   ranges), published by atomic pointer whenever the list or a range changes; the audio thread pins the snapshot it
   finds at its first call of a block and lets go at the end of process, counting blocks (epoch). A replaced snapshot,
   and the engines of zones gone with it, are freed on the main thread once two blocks have ended since (or at once
   when nothing processes). A parameter set while a new snapshot was being made may miss its new engines, so the
   audio thread copies every parameter into a snapshot's engines the first time it sees that snapshot. */
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine_int.h"

/* A zone. The main thread writes it; the audio thread reads it through a snapshot (its engine pointer, set once
   complete, and its ranges) and may write its root (automation of Root note). Freed like the snapshots. */
typedef struct {
  SmpEngine *volatile e;                 /* NULL while missing */
  char path[1024]; uint64_t key; int has_key;
  char why[600];                         /* missing: why it didn't load */
  volatile LONG klo, khi, vlo, vhi, root;
  volatile LONG pc;                      /* 0: the keys klo to khi; else a bit per note (bit 0 C to bit 11 B): every key of
                                            those notes, whatever its octave, and klo and khi the lowest and highest */
} Zone;

/* one note name without its octave ("C", "f#", "Bb"): 0 to 11, or -1. A plain letter is that note only: D isn't D#. */
static int pc_one(const char *t, size_t n) {
  static const int base[7] = {9, 11, 0, 2, 4, 5, 7};   /* A to G */
  int c, k;
  if (n < 1 || n > 2) return -1;
  c = toupper((unsigned char)t[0]);
  if (c < 'A' || c > 'G') return -1;
  k = base[c - 'A'];
  if (n == 2) { if (t[1] == '#') k++; else if (t[1] == 'b') k += 11; else return -1; }
  return k % 12;
}
/* note names without octaves, apart by spaces or commas ("D F", "C,E,G", "D#"): their bits, or 0 when any isn't one */
static int pc_parse(const char *t) {
  int mask = 0;
  while (*t) {
    size_t n; int k;
    while (*t == ' ' || *t == ',') t++;
    if (!*t) break;
    for (n = 0; t[n] && t[n] != ' ' && t[n] != ','; n++) {}
    if ((k = pc_one(t, n)) < 0) return 0;
    mask |= 1 << k;
    t += n;
  }
  return mask;
}
/* sep between names: "D,F" (the script) or " and " (words: "D, F and A") */
static void pc_name(int mask, int words, char *out, int cap) {
  int k, n = 0, total = 0, at = 0;
  for (k = 0; k < 12; k++) if (mask >> k & 1) total++;
  if (cap > 0) out[0] = 0;
  for (k = 0; k < 12; k++) {
    char t[16]; int j;
    if (!(mask >> k & 1)) continue;
    smp_note_name(60 + k, t, sizeof t);
    for (j = 0; t[j] && t[j] != '-' && (t[j] < '0' || t[j] > '9'); j++) {}
    t[j] = 0;
    n++;
    at += snprintf(out + at, (size_t)(cap - at > 0 ? cap - at : 0), "%s%s",
                   n == 1 ? "" : !words ? "," : n == total ? " and " : ", ", t);
    if (at >= cap) break;
  }
}
static void pc_keys(int mask, LONG *lo, LONG *hi) {
  int k;
  *lo = 0; *hi = 127;
  for (k = 0; k < 128; k++) if (mask >> (k % 12) & 1) { *lo = k; break; }
  for (k = 127; k >= 0; k--) if (mask >> (k % 12) & 1) { *hi = k; break; }
}

typedef struct { LONG64 gen; int n; Zone *z[SMP_MULTI_MAX]; } MSnap;

typedef struct { MSnap *s; Zone **dead; int ndead; LONG64 a0; } MRet;

struct SmpMulti {
  double rate; int max_block;
  volatile LONG64 p[SMP_NPARAMS];        /* the instrument's parameters, doubles as bits (Root note unused) */
  Zone *z[SMP_MULTI_MAX]; int n, sel;    /* main thread */
  Zone *volatile selz;                   /* the selected zone, for the audio thread's Root note */
  Zone **dead; int ndead, cdead;         /* main thread: zones gone, freed with the next retirement */
  MSnap *volatile pub;
  MSnap *aud;                            /* audio thread: the snapshot of this block */
  LONG64 synced;                         /* audio thread: the snapshot generation its engines were last synced to */
  LONG64 gen;
  volatile LONG64 epoch;                 /* blocks the audio thread has finished */
  MRet *ret; int nret, cret;             /* main thread */
  DWORD main_tid;
  unsigned seed;
};

static LONG64 bits(double v) { LONG64 b; memcpy(&b, &v, 8); return b; }
static double pget(const SmpMulti *m, int id) { LONG64 b = m->p[id]; double v; memcpy(&v, &b, 8); return v; }
static int on_main(const SmpMulti *m) { return GetCurrentThreadId() == m->main_tid; }

/* what an engine's parameter id should be for a zone with these ranges */
static double eff(const SmpMulti *m, int id, int klo, int khi, int vlo, int vhi) {
  double v = pget(m, id);
  switch (id) {
    case SMP_P_NOTE_LO: return v > klo ? v : klo;
    case SMP_P_NOTE_HI: return v < khi ? v : khi;
    case SMP_P_VEL_LO: return v > vlo ? v : vlo;
    case SMP_P_VEL_HI: return v < vhi ? v : vhi;
  }
  return v;
}
static void set_if(SmpEngine *e, int id, double v) {
  if (smp_engine_get_param(e, id) != v) smp_engine_set_param(e, id, v);
}
static void sync_engine(const SmpMulti *m, SmpEngine *e, const Zone *z) {
  int id;
  for (id = 0; id < SMP_NPARAMS; id++) if (id != SMP_P_ROOT) set_if(e, id, eff(m, id, z->klo, z->khi, z->vlo, z->vhi));
  set_if(e, SMP_P_ROOT, z->root);
}
static void sync_zone(const SmpMulti *m, Zone *z) { if (z->e) sync_engine(m, z->e, z); }
static void zone_free(Zone *z) { if (z) { smp_engine_free(z->e); free(z); } }
static void set_sel(SmpMulti *m, int i) {
  m->sel = i;
  InterlockedExchangePointer((PVOID volatile *)&m->selz, i >= 0 && i < m->n ? m->z[i] : NULL);
}

/* ---- the snapshot ---- */
static void retire(SmpMulti *m, MSnap *old) {
  if (!old && !m->ndead) return;
  if (m->nret == m->cret) {
    int c = m->cret ? m->cret * 2 : 8;
    MRet *n = realloc(m->ret, sizeof *n * (size_t)c);
    if (!n) return;                      /* out of memory: leak rather than free under the audio thread */
    m->ret = n; m->cret = c;
  }
  m->ret[m->nret].s = old; m->ret[m->nret].dead = m->dead; m->ret[m->nret].ndead = m->ndead;
  m->ret[m->nret].a0 = m->epoch;
  m->nret++;
  m->dead = NULL; m->ndead = m->cdead = 0;
}
static void publish(SmpMulti *m) {
  MSnap *s = calloc(1, sizeof *s), *old;
  int i;
  if (!s) return;                        /* out of memory: the audio thread keeps the old list */
  s->gen = ++m->gen;
  for (i = 0; i < m->n; i++) s->z[s->n++] = m->z[i];
  set_sel(m, m->sel);
  old = InterlockedExchangePointer((PVOID volatile *)&m->pub, s);
  retire(m, old);
}
static void bury(SmpMulti *m, Zone *e) {     /* freed once the audio thread can't reach it */
  if (!e) return;
  if (m->ndead == m->cdead) {
    int c = m->cdead ? m->cdead * 2 : 8;
    Zone **n = realloc(m->dead, sizeof *n * (size_t)c);
    if (!n) return;                      /* leak it */
    m->dead = n; m->cdead = c;
  }
  m->dead[m->ndead++] = e;
}

SMP_API void smp_multi_collect(SmpMulti *m, int idle) {
  int i = 0, k;
  if (!m) return;
  /* nothing processes: a snapshot the audio thread pinned without a process after it (a parameter flush) is let go,
     so it can't be used once freed */
  if (idle) m->aud = NULL;
  for (k = 0; k < m->n; k++) if (m->z[k]->e) smp_engine_collect(m->z[k]->e);
  while (i < m->nret) {
    MRet *r = &m->ret[i];
    if (idle || m->epoch >= r->a0 + 2) {
      for (k = 0; k < r->ndead; k++) zone_free(r->dead[k]);
      free(r->dead); free(r->s);
      m->ret[i] = m->ret[--m->nret];
      continue;
    }
    i++;
  }
}

/* the audio thread's snapshot for this block (pinned at its first call) */
static MSnap *aud(SmpMulti *m) {
  if (!m->aud) m->aud = m->pub;
  return m->aud;
}
/* the snapshot to use from this thread: the main thread's zones are its own, so it uses the published one */
static MSnap *view(SmpMulti *m) { return on_main(m) ? m->pub : aud(m); }

/* ---- life cycle ---- */
SMP_API SmpMulti *smp_multi_new(double rate, int max_block) {
  SmpMulti *m; int i;
  if (rate <= 0) return NULL;
  if (!(m = calloc(1, sizeof *m))) return NULL;
  m->rate = rate; m->max_block = max_block; m->sel = -1; m->seed = 1;
  m->main_tid = GetCurrentThreadId();
  for (i = 0; i < SMP_NPARAMS; i++) m->p[i] = bits(smp_param_info(i)->def);
  publish(m);
  if (!m->pub) { free(m); return NULL; }
  return m;
}

SMP_API void smp_multi_free(SmpMulti *m) {
  int i;
  if (!m) return;
  for (i = 0; i < m->n; i++) zone_free(m->z[i]);
  m->n = 0;
  for (i = 0; i < m->ndead; i++) zone_free(m->dead[i]);
  free(m->dead);
  m->dead = NULL; m->ndead = 0;
  smp_multi_collect(m, 1);
  free(m->ret);
  free(m->pub);
  free(m);
}

/* an engine for zone z playing s (taken; freed on failure) */
static int zone_engine(SmpMulti *m, Zone *z, SmpSample *s) {
  SmpEngine *e = z->e;
  if (!e) {                              /* made and set up before the audio thread can see it */
    if (!(e = smp_engine_new(m->rate, m->max_block))) { smp_sample_free(s); return 0; }
    smp_engine_seed(e, m->seed + (unsigned)m->n * 7919u);
  }
  smp_engine_set_sample(e, s);           /* sets its root to the sample's; ours below */
  snprintf(z->path, sizeof z->path, "%s", s->path);
  z->key = s->src_key; z->has_key = 1; z->why[0] = 0;
  sync_engine(m, e, z);
  InterlockedExchangePointer((PVOID volatile *)&z->e, e);
  return 1;
}

static Zone *zone_new(int root) {
  Zone *z = calloc(1, sizeof *z);
  if (!z) return NULL;
  z->klo = 0; z->khi = 127; z->vlo = 1; z->vhi = 127; z->root = root < 0 ? 60 : root > 127 ? 127 : root; z->pc = 0;
  return z;
}

SMP_API int smp_multi_count(const SmpMulti *m) { return m ? m->n : 0; }

SMP_API int smp_multi_add(SmpMulti *m, SmpSample *s) {
  Zone *z;
  if (!m || !s) { smp_sample_free(s); return -1; }
  if (m->n >= SMP_MULTI_MAX || !(z = zone_new(s->root))) { smp_sample_free(s); return -1; }
  if (!zone_engine(m, z, s)) { free(z); return -1; }
  m->z[m->n++] = z;
  if (m->sel < 0) m->sel = 0;
  publish(m);
  return m->n - 1;
}

SMP_API int smp_multi_replace(SmpMulti *m, int i, SmpSample *s) {
  int had;
  if (!m || !s || i < 0 || i >= m->n) { smp_sample_free(s); return 0; }
  had = m->z[i]->e != NULL;
  InterlockedExchange(&m->z[i]->root, s->root);
  if (!zone_engine(m, m->z[i], s)) return 0;
  if (!had) publish(m);                  /* the audio thread syncs its new engine */
  return 1;
}

SMP_API void smp_multi_remove(SmpMulti *m, int i) {
  if (!m || i < 0 || i >= m->n) return;
  bury(m, m->z[i]);
  memmove(&m->z[i], &m->z[i + 1], sizeof m->z[0] * (size_t)(m->n - i - 1));
  m->n--;
  if (m->sel >= m->n) m->sel = m->n - 1;
  publish(m);
}

SMP_API void smp_multi_clear(SmpMulti *m) {
  int i;
  if (!m) return;
  for (i = 0; i < m->n; i++) bury(m, m->z[i]);
  m->n = 0; m->sel = -1;
  publish(m);
}

/* ---- each zone ---- */
SMP_API const SmpSample *smp_multi_sample(const SmpMulti *m, int i) {
  return m && i >= 0 && i < m->n && m->z[i]->e ? smp_engine_sample(m->z[i]->e) : NULL;
}
SMP_API int smp_multi_path(const SmpMulti *m, int i, char *out, int cap) {
  if (cap > 0) out[0] = 0;
  if (!m || i < 0 || i >= m->n) return 0;
  snprintf(out, (size_t)cap, "%s", m->z[i]->path);
  return smp_multi_sample(m, i) ? 1 : 2;
}
SMP_API const char *smp_multi_why(const SmpMulti *m, int i) { return m && i >= 0 && i < m->n ? m->z[i]->why : ""; }
SMP_API void smp_multi_range(const SmpMulti *m, int i, int *klo, int *khi, int *vlo, int *vhi) {
  const Zone *z = m && i >= 0 && i < m->n ? m->z[i] : NULL;
  *klo = z ? z->klo : 0; *khi = z ? z->khi : 127; *vlo = z ? z->vlo : 1; *vhi = z ? z->vhi : 127;
}
SMP_API int smp_multi_set_range(SmpMulti *m, int i, int klo, int khi, int vlo, int vhi) {
  Zone *z;
  if (!m || i < 0 || i >= m->n || klo < 0 || khi > 127 || klo > khi || vlo < 1 || vhi > 127 || vlo > vhi) return 0;
  z = m->z[i];
  z->klo = klo; z->khi = khi; z->vlo = vlo; z->vhi = vhi; z->pc = 0;
  sync_zone(m, z);
  return 1;
}
SMP_API int smp_multi_key_class(const SmpMulti *m, int i) { return m && i >= 0 && i < m->n ? m->z[i]->pc : 0; }
SMP_API int smp_multi_set_key_class(SmpMulti *m, int i, int pc) {
  Zone *z;
  if (!m || i < 0 || i >= m->n || pc <= 0 || pc > 4095) return 0;
  z = m->z[i];
  pc_keys(pc, (LONG *)&z->klo, (LONG *)&z->khi);
  z->pc = pc;
  sync_zone(m, z);
  return 1;
}
SMP_API int smp_multi_root(const SmpMulti *m, int i) { return m && i >= 0 && i < m->n ? m->z[i]->root : 60; }
SMP_API void smp_multi_set_root(SmpMulti *m, int i, int root) {
  if (!m || i < 0 || i >= m->n) return;
  InterlockedExchange(&m->z[i]->root, root < 0 ? 0 : root > 127 ? 127 : root);
  if (m->z[i]->e) set_if(m->z[i]->e, SMP_P_ROOT, m->z[i]->root);
}
SMP_API int smp_multi_selected(const SmpMulti *m) { return m ? m->sel : -1; }
SMP_API void smp_multi_select(SmpMulti *m, int i) { if (m && i >= 0 && i < m->n) set_sel(m, i); }

SMP_API void smp_multi_line(const SmpMulti *m, int i, char *out, int cap) {
  const SmpSample *s = smp_multi_sample(m, i);
  char root[16], lo[16], hi[16];
  if (cap <= 0) return;
  {
  if (!m || i < 0 || i >= m->n) { out[0] = 0; return; }
  smp_note_name(m->z[i]->root, root, sizeof root);
  char keys[96];
  smp_note_name(m->z[i]->klo, lo, sizeof lo); smp_note_name(m->z[i]->khi, hi, sizeof hi);
  if (m->z[i]->pc) { char w[80]; pc_name(m->z[i]->pc, 1, w, sizeof w); snprintf(keys, sizeof keys, "every %s", w); }
  else snprintf(keys, sizeof keys, "%s to %s", lo, hi);
  if (s) snprintf(out, (size_t)cap, "%s, root %s, keys %s, velocity %d to %d", s->name, root, keys,
                  m->z[i]->vlo, m->z[i]->vhi);
  else snprintf(out, (size_t)cap, "Missing: %s, root %s, keys %s, velocity %d to %d", m->z[i]->path, root, keys,
                m->z[i]->vlo, m->z[i]->vhi);
  }
}

/* ---- the instrument's parameters ---- */
SMP_API void smp_multi_set_param(SmpMulti *m, int id, double v) {
  const SmpParamInfo *pi = smp_param_info(id);
  int i;
  if (!m || id < 0 || id >= SMP_NPARAMS || v != v) return;
  if (v < pi->lo) v = pi->lo;
  if (v > pi->hi) v = pi->hi;
  if (id == SMP_P_ROOT) {                /* the selected sample's */
    Zone *z = on_main(m) ? (m->sel >= 0 ? m->z[m->sel] : NULL) : m->selz;
    if (z) { InterlockedExchange(&z->root, (LONG)(v + 0.5)); if (z->e) smp_engine_set_param(z->e, id, z->root); }
    return;
  }
  InterlockedExchange64(&m->p[id], bits(v));
  {
    MSnap *s = view(m);
    for (i = 0; s && i < s->n; i++) {
      Zone *z = s->z[i];
      if (z->e) smp_engine_set_param(z->e, id, eff(m, id, z->klo, z->khi, z->vlo, z->vhi));
    }
  }
}
SMP_API double smp_multi_get_param(const SmpMulti *m, int id) {
  if (!m || id < 0 || id >= SMP_NPARAMS) return 0;
  if (id == SMP_P_ROOT) return m->sel >= 0 ? m->z[m->sel]->root : smp_param_info(id)->def;
  return pget(m, id);
}

/* ---- the audio thread ---- */
SMP_API void smp_multi_note_on(SmpMulti *m, int key, int vel, int at) {
  MSnap *s = aud(m); int i;
  for (i = 0; s && i < s->n; i++) {
    Zone *z = s->z[i];
    if (z->e && (!z->pc || (z->pc >> (key % 12) & 1))) smp_engine_note_on(z->e, key, vel, at);
  }
}
SMP_API void smp_multi_note_off(SmpMulti *m, int key, int at) {
  MSnap *s = aud(m); int i;
  for (i = 0; s && i < s->n; i++) if (s->z[i]->e) smp_engine_note_off(s->z[i]->e, key, at);
}
SMP_API void smp_multi_bend(SmpMulti *m, double x, int at) {
  MSnap *s = aud(m); int i;
  for (i = 0; s && i < s->n; i++) if (s->z[i]->e) smp_engine_bend(s->z[i]->e, x, at);
}
SMP_API void smp_multi_process(SmpMulti *m, float *l, float *r, int n) {
  MSnap *s = aud(m); int i;
  if (n > 0) { memset(l, 0, sizeof(float) * (size_t)n); memset(r, 0, sizeof(float) * (size_t)n); }
  if (s && s->gen != m->synced) {        /* a new list: its engines get every parameter as it is now */
    for (i = 0; i < s->n; i++) if (s->z[i]->e) sync_engine(m, s->z[i]->e, s->z[i]);
    m->synced = s->gen;
  }
  for (i = 0; s && i < s->n; i++) if (s->z[i]->e) smp_engine_process_mix(s->z[i]->e, l, r, n);
  m->aud = NULL;
  InterlockedIncrement64(&m->epoch);
}
SMP_API void smp_multi_reset(SmpMulti *m, int now) {
  MSnap *s; int i;
  if (!m) return;
  s = view(m);
  for (i = 0; s && i < s->n; i++) if (s->z[i]->e) smp_engine_reset(s->z[i]->e, now);
}
SMP_API int smp_multi_active_voices(const SmpMulti *m) {
  int i, k = 0;
  if (!m) return 0;
  for (i = 0; i < m->n; i++) if (m->z[i]->e) k += smp_engine_active_voices(m->z[i]->e);
  return k;
}
SMP_API long smp_multi_underruns(const SmpMulti *m) {
  int i; long k = 0;
  if (!m) return 0;
  for (i = 0; i < m->n; i++) if (m->z[i]->e) k += smp_engine_underruns(m->z[i]->e);
  return k;
}
SMP_API int smp_multi_render_status(SmpMulti *m, int *ready, int *total, char *why, int cap) {
  int i, all = 1;
  *ready = *total = 0;
  if (cap > 0) why[0] = 0;
  for (i = 0; m && i < m->n; i++) {
    int r = 0, t = 0; char w[512] = "";
    if (!m->z[i]->e || !smp_engine_sample(m->z[i]->e)) continue;
    if (!smp_engine_render_status(m->z[i]->e, &r, &t, w, sizeof w)) all = 0;
    *ready += r; *total += t;
    if (w[0] && cap > 0 && !why[0]) snprintf(why, (size_t)cap, "%s", w);
  }
  return all;
}
SMP_API int smp_multi_wait_renders(SmpMulti *m, int timeout_ms) {
  int i, bad = 0;
  for (i = 0; m && i < m->n; i++) {
    if (!m->z[i]->e) continue;
    if (smp_engine_wait_renders(m->z[i]->e, timeout_ms)) bad = 1;
    smp_worker_wait_idle(m->z[i]->e);    /* start heads */
  }
  return bad;
}
SMP_API void smp_multi_test_offline(SmpMulti *m, int on) {
  int i;
  for (i = 0; m && i < m->n; i++) if (m->z[i]->e) m->z[i]->e->offline = on;
}
SMP_API void smp_multi_test_sync(SmpMulti *m) {
  int i;
  for (i = 0; m && i < m->n; i++) if (m->z[i]->e) smp_stream_wait_idle(m->z[i]->e);
}
SMP_API void smp_multi_seed(SmpMulti *m, unsigned seed) {
  int i;
  if (!m) return;
  m->seed = seed ? seed : 1;
  for (i = 0; i < m->n; i++) if (m->z[i]->e) smp_engine_seed(m->z[i]->e, m->seed + (unsigned)i * 7919u);
}

/* ---- tests: an offline render through every sample, as smp_engine_render_test ---- */
typedef struct { int type, key, vel; long frame; double x; } MEv;
static void wav_header(FILE *f, uint32_t frames, double rate) {
  uint32_t data = frames * 8, riff = 36 + data, sr = (uint32_t)(rate + 0.5), br = sr * 8, fmt = 16;
  uint16_t tag = 3, ch = 2, align = 8, bitsps = 32;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
  fwrite(&tag, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
  fwrite(&align, 2, 1, f); fwrite(&bitsps, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}
SMP_API int smp_multi_render_test(SmpMulti *m, const char *events, long frames, int block, const char *wav_out) {
  MEv *ev = NULL; int nev = 0, cap = 0, k = 0, i, j; const char *p = events;
  float *l, *r, *lr; FILE *f; wchar_t w[2048]; long b; MSnap *s;
  if (!m || !wav_out || frames < 0 || block <= 0) return 1;
  while (p && *p) {
    MEv t; char word[16]; int used = 0; const char *nl = strchr(p, '\n');
    memset(&t, 0, sizeof t);
    if (sscanf(p, "%15s%n", word, &used) == 1) {
      int ok = 0;
      if (!strcmp(word, "on")) { t.type = 0; ok = sscanf(p + used, "%d %d %ld", &t.key, &t.vel, &t.frame) == 3; }
      else if (!strcmp(word, "off")) { t.type = 1; ok = sscanf(p + used, "%d %ld", &t.key, &t.frame) == 2; }
      else if (!strcmp(word, "bend")) { t.type = 2; ok = sscanf(p + used, "%lf %ld", &t.x, &t.frame) == 2; }
      if (!ok) { free(ev); return 2; }
      if (nev == cap) { MEv *n2; cap = cap ? cap * 2 : 64; n2 = realloc(ev, sizeof *ev * (size_t)cap); if (!n2) { free(ev); return 3; } ev = n2; }
      ev[nev++] = t;
    }
    p = nl ? nl + 1 : NULL;
  }
  for (i = 1; i < nev; i++) { MEv x = ev[i]; for (j = i - 1; j >= 0 && ev[j].frame > x.frame; j--) ev[j + 1] = ev[j]; ev[j + 1] = x; }
  if (!MultiByteToWideChar(CP_UTF8, 0, wav_out, -1, w, 2048) || !(f = _wfopen(w, L"wb"))) { free(ev); return 4; }
  l = malloc(sizeof(float) * (size_t)block); r = malloc(sizeof(float) * (size_t)block); lr = malloc(sizeof(float) * 2 * (size_t)block);
  if (!l || !r || !lr) { free(l); free(r); free(lr); free(ev); fclose(f); return 3; }
  wav_header(f, (uint32_t)frames, m->rate);
  s = m->pub;
  for (i = 0; i < s->n; i++) {           /* every render starts from silence */
    SmpEngine *e = s->z[i]->e;
    if (!e) continue;
    smp_worker_wait_idle(e);
    for (j = 0; j < SMP_MAXV; j++) e->v[j].live = 0;
    e->bend = 0; memset(e->held, 0, sizeof e->held); e->have_last = 0;
    e->offline = 1;
  }
  for (b = 0; b < frames; b += block) {
    int n = (int)(frames - b < block ? frames - b : block);
    for (; k < nev && ev[k].frame < b + n; k++) {
      int at = (int)(ev[k].frame - b);
      if (at < 0) at = 0;
      if (ev[k].type == 0) smp_multi_note_on(m, ev[k].key, ev[k].vel, at);
      else if (ev[k].type == 1) smp_multi_note_off(m, ev[k].key, at);
      else smp_multi_bend(m, ev[k].x, at);
    }
    smp_multi_process(m, l, r, n);
    for (i = 0; i < n; i++) { lr[2 * i] = l[i]; lr[2 * i + 1] = r[i]; }
    fwrite(lr, sizeof(float) * 2, (size_t)n, f);
    for (i = 0; i < s->n; i++) if (s->z[i]->e) smp_stream_wait_idle(s->z[i]->e);
  }
  for (i = 0; i < s->n; i++) if (s->z[i]->e) s->z[i]->e->offline = 0;
  free(l); free(r); free(lr); free(ev);
  return fclose(f) ? 5 : 0;
}

/* ---- the instrument as text ---- */
typedef struct { char *out; int cap; int n; } Out;
static void put(Out *o, const char *s) {
  size_t len = strlen(s);
  if (o->out && o->cap > 0 && (size_t)o->n < (size_t)o->cap) {
    size_t room = (size_t)o->cap - 1 - (size_t)o->n, c = len < room ? len : room;
    memcpy(o->out + o->n, s, c);
    o->out[o->n + (int)c] = 0;
  }
  o->n += (int)len;
}

SMP_API int smp_multi_script_write(const SmpMulti *m, char *out, int cap) {
  Out o = {out, cap, 0};
  char line[1500], a[16], b[16];
  int i;
  if (out && cap > 0) out[0] = 0;
  put(&o, "# Samplosis 0.1.0\n");
  for (i = 0; m && i < m->n; i++) {
    const Zone *z = m->z[i];
    int at = snprintf(line, sizeof line, "sample \"%s\"", z->path);
    if (z->has_key) at += snprintf(line + at, sizeof line - (size_t)at, " key=%016llx", (unsigned long long)z->key);
    if (m->n > 1) { smp_note_name(z->root, a, sizeof a); at += snprintf(line + at, sizeof line - (size_t)at, " root=%s", a); }
    if (z->pc) { char w[80]; pc_name(z->pc, 0, w, sizeof w); at += snprintf(line + at, sizeof line - (size_t)at, " keys=%s", w); }
    else if (z->klo != 0 || z->khi != 127) {
      smp_note_name(z->klo, a, sizeof a); smp_note_name(z->khi, b, sizeof b);
      at += snprintf(line + at, sizeof line - (size_t)at, " keys=%s..%s", a, b);
    }
    if (z->vlo != 1 || z->vhi != 127) at += snprintf(line + at, sizeof line - (size_t)at, " velocity=%d..%d", z->vlo, z->vhi);
    snprintf(line + at, sizeof line - (size_t)at, "\n");
    put(&o, line);
  }
  for (i = 0; i < SMP_NPARAMS; i++) {
    char v[64];
    smp_value_text(i, m ? smp_multi_get_param(m, i) : smp_param_info(i)->def, v, sizeof v);
    snprintf(line, sizeof line, "%s %s\n", smp_param_info(i)->name, v);
    put(&o, line);
  }
  return o.n;
}

static void fail(char *err, int cap, int line, const char *fmt, const char *a) {
  char msg[700];
  int n = snprintf(msg, sizeof msg, "line %d: ", line);
  snprintf(msg + n, sizeof msg - (size_t)n, fmt, a);
  if (err && cap > 0) snprintf(err, (size_t)cap, "%s", msg);
}
static int is_hex16(const char *s) {
  int i;
  for (i = 0; i < 16; i++) if (!isxdigit((unsigned char)s[i])) return 0;
  return s[16] == 0 || isspace((unsigned char)s[16]);
}
/* "C2..C3" (notes) or "15..80" (numbers 1 to 127): 1 and lo <= hi, else 0 */
static int read_range(const char *v, int notes, int *lo, int *hi) {
  char a[32], b[32]; const char *dots = strstr(v, "..");
  if (!dots || dots - v >= 32 || strlen(dots + 2) >= 32) return 0;
  memcpy(a, v, (size_t)(dots - v)); a[dots - v] = 0;
  snprintf(b, sizeof b, "%s", dots + 2);
  if (notes) { *lo = smp_note_parse(a); *hi = smp_note_parse(b); if (*lo < 0 || *hi < 0) return 0; }
  else {
    char *e1, *e2; long x = strtol(a, &e1, 10), y = strtol(b, &e2, 10);
    if (!a[0] || !b[0] || *e1 || *e2 || x < 1 || y > 127) return 0;
    *lo = (int)x; *hi = (int)y;
  }
  return *lo <= *hi;
}

typedef struct { char path[1024]; uint64_t key; int has_key, root, klo, khi, vlo, vhi, pc, line; } SLine;

SMP_API int smp_multi_script_apply(SmpMulti *m, const char *text, int *missing, char *err, int cap) {
  double val[SMP_NPARAMS]; char have[SMP_NPARAMS] = {0};
  SLine *sl = NULL; int ns = 0, cs = 0, lineno = 0, i, j, nmiss = 0;
  const char *p = text;
  Zone *nz[SMP_MULTI_MAX]; int used[SMP_MULTI_MAX] = {0};
  if (err && cap > 0) err[0] = 0;
  if (missing) *missing = 0;
  if (!m || !text) { if (err && cap > 0) snprintf(err, (size_t)cap, "no instrument or no text"); return 1; }
  while (*p) {
    const char *eol = p; char buf[1400]; size_t len; char *s, *name, *rest;
    while (*eol && *eol != '\n') eol++;
    len = (size_t)(eol - p);
    lineno++;
    if (len >= sizeof buf) { fail(err, cap, lineno, "%s", "the line is too long"); free(sl); return 1; }
    memcpy(buf, p, len); buf[len] = 0;
    p = *eol ? eol + 1 : eol;
    s = buf;
    while (isspace((unsigned char)*s)) s++;
    len = strlen(s);
    while (len && isspace((unsigned char)s[len - 1])) s[--len] = 0;
    if (!*s || *s == '#') continue;
    name = rest = s;
    while (*rest && !isspace((unsigned char)*rest)) rest++;
    if (*rest) { *rest++ = 0; while (isspace((unsigned char)*rest)) rest++; }
    if (!strcmp(name, "plays") || !strcmp(name, "soundfont") || !strncmp(name, "sf_", 3)) continue;
    if (!strcmp(name, "sample")) {
      SLine L; char *q, *end;
      memset(&L, 0, sizeof L);
      L.root = -1; L.klo = 0; L.khi = 127; L.vlo = 1; L.vhi = 127; L.pc = 0; L.line = lineno;
      if (*rest != '"') { fail(err, cap, lineno, "sample needs the path in quotes, like %s", "sample \"E:\\samples\\a.wav\""); free(sl); return 1; }
      q = rest + 1;
      if (!(end = strchr(q, '"'))) { fail(err, cap, lineno, "sample: %s", "the closing quote is missing"); free(sl); return 1; }
      *end = 0;
      if (!*q) { fail(err, cap, lineno, "sample: %s", "the path is empty"); free(sl); return 1; }
      snprintf(L.path, sizeof L.path, "%s", q);
      rest = end + 1;
      for (;;) {                           /* key=, root=, keys=, velocity= */
        char word[300], *eq; size_t wl;
        while (isspace((unsigned char)*rest)) rest++;
        if (!*rest) break;
        wl = 0;
        while (rest[wl] && !isspace((unsigned char)rest[wl])) wl++;
        if (wl >= sizeof word) wl = sizeof word - 1;
        memcpy(word, rest, wl); word[wl] = 0;
        rest += wl;
        eq = strchr(word, '=');
        if (!eq) { fail(err, cap, lineno, "sample: unexpected '%s' after the path", word); free(sl); return 1; }
        *eq++ = 0;
        if (!strcmp(word, "key")) {
          if (!is_hex16(eq) || strlen(eq) != 16) { fail(err, cap, lineno, "sample: expected key= and 16 hex digits, got '%s'", eq); free(sl); return 1; }
          L.key = strtoull(eq, NULL, 16); L.has_key = 1;
        } else if (!strcmp(word, "root")) {
          if ((L.root = smp_note_parse(eq)) < 0) { fail(err, cap, lineno, "sample: root=%s isn't a note, like C3", eq); free(sl); return 1; }
        } else if (!strcmp(word, "keys")) {
          if ((L.pc = pc_parse(eq)) != 0) { LONG a, b; pc_keys(L.pc, &a, &b); L.klo = a; L.khi = b; }
          else if (!read_range(eq, 1, &L.klo, &L.khi)) { fail(err, cap, lineno, "sample: keys=%s should be two notes, low first, like keys=C2..C3, or notes in every octave, like keys=C or keys=D,F", eq); free(sl); return 1; }
        } else if (!strcmp(word, "velocity")) {
          if (!read_range(eq, 0, &L.vlo, &L.vhi)) { fail(err, cap, lineno, "sample: velocity=%s should be 1 to 127, low first, like velocity=15..80", eq); free(sl); return 1; }
        } else { fail(err, cap, lineno, "sample: unknown setting '%s'", word); free(sl); return 1; }
      }
      if (ns >= SMP_MULTI_MAX) { fail(err, cap, lineno, "%s", "more than 128 samples"); free(sl); return 1; }
      if (ns == cs) {
        SLine *n2 = realloc(sl, sizeof *sl * (size_t)(cs = cs ? cs * 2 : 8));
        if (!n2) { free(sl); if (err && cap > 0) snprintf(err, (size_t)cap, "out of memory"); return 1; }
        sl = n2;
      }
      sl[ns++] = L;
      continue;
    }
    {
      int id = -1;
      for (i = 0; i < SMP_NPARAMS; i++) if (!strcmp(name, smp_param_info(i)->name)) { id = i; break; }
      if (id < 0) { fail(err, cap, lineno, "unknown setting '%s'", name); free(sl); return 1; }
      if (!*rest) { fail(err, cap, lineno, "%s needs a value", name); free(sl); return 1; }
      if (!smp_value_parse(id, rest, &val[id])) {
        char msg[700]; snprintf(msg, sizeof msg, "%s: can't read '%s'", name, rest);
        fail(err, cap, lineno, "%s", msg); free(sl); return 1;
      }
      have[id] = 1;
    }
  }
  /* every line reads: the parameters, then the samples (those already here with the same path and key are kept) */
  for (i = 0; i < SMP_NPARAMS; i++)
    if (i != SMP_P_ROOT) InterlockedExchange64(&m->p[i], bits(have[i] ? val[i] : smp_param_info(i)->def));
  for (i = 0; i < ns; i++) {
    SLine *L = &sl[i]; Zone *z = NULL;
    nz[i] = NULL;
    int root = L->root >= 0 ? L->root : ns == 1 && have[SMP_P_ROOT] ? (int)(val[SMP_P_ROOT] + 0.5) : -1;
    for (j = 0; L->has_key && j < m->n; j++)
      if (!used[j] && m->z[j]->e && smp_engine_sample(m->z[j]->e) && m->z[j]->key == L->key && !strcmp(m->z[j]->path, L->path)) {
        used[j] = 1; z = m->z[j]; m->z[j] = NULL; break;
      }
    if (!z) {
      char msg[600] = "";
      SmpSample *s = smp_sample_load(L->path, msg, sizeof msg);
      if (!s && L->has_key) {
        char m2[600] = "";
        s = smp_sample_load_known(L->path, L->key, root >= 0 ? root : 60, m2, sizeof m2);
      }
      z = zone_new(root >= 0 ? root : s ? s->root : 60);
      if (!z) { smp_sample_free(s); nmiss++; continue; }
      if (s) { if (!zone_engine(m, z, s)) s = NULL; }
      if (!z->e) {                         /* missing: kept as written */
        snprintf(z->path, sizeof z->path, "%s", L->path);
        z->key = L->key; z->has_key = L->has_key;
        snprintf(z->why, sizeof z->why, "%s", msg[0] ? msg : "can't load it");
        nmiss++;
      }
    }
    if (root >= 0) z->root = root;         /* else kept, or the new sample's */
    z->klo = L->klo; z->khi = L->khi; z->vlo = L->vlo; z->vhi = L->vhi; z->pc = L->pc;
    nz[i] = z;
  }
  for (j = 0; j < m->n; j++) if (m->z[j]) bury(m, m->z[j]);
  m->n = 0;
  for (i = 0; i < ns; i++) if (nz[i]) m->z[m->n++] = nz[i];
  for (i = 0; i < m->n; i++) sync_zone(m, m->z[i]);
  m->sel = m->n ? 0 : -1;
  publish(m);
  free(sl);
  if (missing) *missing = nmiss;
  return 0;
}

SMP_API int smp_note_class_parse(const char *t) { return t ? pc_parse(t) : 0; }
SMP_API void smp_note_class_name(int mask, char *out, int cap) { if (cap > 0) { if (mask > 0 && mask < 4096) pc_name(mask, 1, out, cap); else out[0] = 0; } }
