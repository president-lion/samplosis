/* engine.c: parameters, voices, envelopes and the process loop.
   The audio thread (process) never allocates, locks, waits or touches files; its only kernel call is SetEvent.
   Samples are swapped by atomic pointer; a replaced sample is freed by the main thread (collect) only after the
   audio thread has finished a block that began after the swap, no voice uses it, and the streamer has finished
   two passes since. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine_int.h"

SMP_API const char *smp_version(void) { return "0.1.0"; }

#define LOG2_12 (1.0 / 12.0)
#define ENV_FLOOR 1e-4                    /* -80 dB: where release ends */

/* ---- parameters ---- */
static LONG64 bits_of(double v) { LONG64 b; memcpy(&b, &v, 8); return b; }
double smp_eng_param(const SmpEngine *e, int id) { LONG64 b = e->p[id]; double v; memcpy(&v, &b, 8); return v; }

SMP_API double smp_engine_get_param(const SmpEngine *e, int id) {
  if (!e || id < 0 || id >= SMP_NPARAMS) return 0;
  return smp_eng_param(e, id);
}

static void store_param(SmpEngine *e, int id, double v) {
  const SmpParamInfo *pi = smp_param_info(id);
  if (!pi || v != v) return;
  if (v < pi->lo) v = pi->lo;
  if (v > pi->hi) v = pi->hi;
  InterlockedExchange64(&e->p[id], bits_of(v));
}

SMP_API void smp_engine_set_param(SmpEngine *e, int id, double v) {
  if (!e || id < 0 || id >= SMP_NPARAMS) return;
  store_param(e, id, v);
  if (id == SMP_P_REVERSE && smp_eng_param(e, id) >= 0.5) {
    if (GetCurrentThreadId() == e->main_tid) {           /* make the reversed copy now, on the main thread */
      SmpSample *s = e->sample;
      if (s && !s->rev) { EnterCriticalSection(&e->rev_cs); smp_sample_reversed(s); LeaveCriticalSection(&e->rev_cs); }
    } else {                                              /* elsewhere: the worker makes it; forward until then */
      InterlockedExchange(&e->want_rev, 1);
    }
  }
  if ((id == SMP_P_REVERSE || id == SMP_P_START || id == SMP_P_END) && e->work) SetEvent(e->work);  /* start heads */
  if (id == SMP_P_VOICES && (int)smp_eng_param(e, id) > e->nring) {   /* more rings: never on the audio thread */
    if (GetCurrentThreadId() == e->main_tid) smp_rings_commit(e);
    else if (e->work) SetEvent(e->work);                  /* the worker commits them; fewer voices until then */
  }
  switch (id) {                                          /* what keep length renders depends on these */
    case SMP_P_KEEP_LENGTH: case SMP_P_SPEED: case SMP_P_REVERSE: case SMP_P_MODE: case SMP_P_ROOT:
    case SMP_P_TRANSPOSE: case SMP_P_FINE: case SMP_P_NOTE_LO: case SMP_P_NOTE_HI: case SMP_P_PITCH_LO:
    case SMP_P_PITCH_HI:
      if (e->rwork) SetEvent(e->rwork);
  }
}

/* ---- life cycle ---- */
void smp_rings_commit(SmpEngine *e) {
  int want = (int)smp_eng_param(e, SMP_P_VOICES);
  if (want > SMP_MAXV) want = SMP_MAXV;
  if (want <= e->nring) return;
  EnterCriticalSection(&e->ring_cs);
  if (want > e->nring &&
      VirtualAlloc(e->rings + (size_t)e->nring * SMP_RING * 2, (SIZE_T)(want - e->nring) * SMP_RING * 8, MEM_COMMIT, PAGE_READWRITE))
    InterlockedExchange(&e->nring, want);                 /* committed first, then the audio thread may use them */
  LeaveCriticalSection(&e->ring_cs);
}

SMP_API SmpEngine *smp_engine_new(double rate, int max_block) {
  SmpEngine *e; int i;
  if (rate <= 0) return NULL;
  e = calloc(1, sizeof *e);
  if (!e) return NULL;
  e->rate = rate; e->max_block = max_block;
  for (i = 0; i < SMP_NPARAMS; i++) e->p[i] = bits_of(smp_param_info(i)->def);
  e->rng = 1;
  e->main_tid = GetCurrentThreadId();
  InitializeCriticalSection(&e->rev_cs);
  InitializeCriticalSection(&e->r_cs);
  InitializeCriticalSection(&e->ring_cs);
  /* address space for every voice; rings are committed only up to the Voices parameter, as it grows */
  e->rings = VirtualAlloc(NULL, (SIZE_T)SMP_MAXV * SMP_RING * 8, MEM_RESERVE, PAGE_READWRITE);
  if (!e->rings) {
    DeleteCriticalSection(&e->rev_cs); DeleteCriticalSection(&e->r_cs); DeleteCriticalSection(&e->ring_cs);
    free(e); return NULL;
  }
  for (i = 0; i < SMP_MAXV; i++) { e->v[i].ring = e->rings + (size_t)i * SMP_RING * 2; e->v[i].s_gen = -1; }
  smp_rings_commit(e);
  if (!smp_stream_start(e) || !smp_worker_start(e) || !smp_render_start(e)) { smp_engine_free(e); return NULL; }
  return e;
}

SMP_API void smp_engine_free(SmpEngine *e) {
  int i;
  if (!e) return;
  smp_render_stop(e);                                   /* first: it may be reading the sample */
  smp_worker_stop(e);                                   /* frees the start heads */
  smp_stream_stop(e);
  smp_render_free_copies(e);
  for (i = 0; i < e->nret; i++) smp_sample_free(e->ret[i].s);
  free(e->ret);
  smp_sample_free(e->sample);
  if (e->rings) VirtualFree(e->rings, 0, MEM_RELEASE);
  DeleteCriticalSection(&e->rev_cs);
  DeleteCriticalSection(&e->r_cs);
  DeleteCriticalSection(&e->ring_cs);
  free(e);
}

static int voice_uses(const SmpEngine *e, const SmpSample *s) {
  int i;
  for (i = 0; i < SMP_MAXV; i++) if (e->v[i].live && e->v[i].smp == s) return 1;
  return 0;
}

SMP_API void smp_engine_collect(SmpEngine *e) {
  int i = 0;
  if (!e) return;
  while (i < e->nret) {
    SmpRetired *r = &e->ret[i];
    SmpStartHead *h0 = e->sh[0], *h1 = e->sh[1];
    if (r->phase == 0 && e->epoch >= r->a0 + 2 && !voice_uses(e, r->s)) { r->s0 = e->spass; r->w0 = e->wpass; r->phase = 1; }
    if (r->phase == 1 && e->spass >= r->s0 + 2 && e->wpass >= r->w0 + 2 &&
        !(h0 && h0->s == r->s) && !(h1 && h1->s == r->s) && e->r_using != r->s) {
      smp_sample_free(r->s);
      e->ret[i] = e->ret[--e->nret];
      continue;
    }
    i++;
  }
}

SMP_API void smp_engine_set_sample(SmpEngine *e, SmpSample *s) {
  SmpSample *old;
  if (!e) return;
  if (s) {
    store_param(e, SMP_P_ROOT, s->root);
    if (smp_eng_param(e, SMP_P_REVERSE) >= 0.5 && !s->rev) {
      EnterCriticalSection(&e->rev_cs); smp_sample_reversed(s); LeaveCriticalSection(&e->rev_cs);
    }
  }
  old = InterlockedExchangePointer((PVOID volatile *)&e->sample, s);
  if (old && old != s) {
    if (e->nret == e->cret) {
      int c = e->cret ? e->cret * 2 : 8;
      SmpRetired *n = realloc(e->ret, sizeof *n * (size_t)c);
      if (n) { e->ret = n; e->cret = c; }
    }
    if (e->nret < e->cret) {
      SmpRetired *r = &e->ret[e->nret++];
      r->s = old; r->a0 = e->epoch; r->s0 = r->w0 = 0; r->phase = 0;
    } /* else out of memory: leak the old sample rather than free it under a voice */
  }
  if (e->work) SetEvent(e->work);                       /* new start heads */
  if (e->rwork) SetEvent(e->rwork);                     /* new keep-length copies */
  smp_engine_collect(e);
}

SMP_API const SmpSample *smp_engine_sample(const SmpEngine *e) { return e ? e->sample : NULL; }

SMP_API void smp_engine_seed(SmpEngine *e, unsigned seed) { if (e) e->rng = seed ? seed : 1; }

/* ---- events (queued by the audio thread before process) ---- */
static void push(SmpEngine *e, int type, int key, int vel, int at, double x) {
  SmpEv *v;
  if (!e || e->nev >= SMP_MAXEV) return;
  v = &e->ev[e->nev++];
  v->type = type; v->key = key; v->vel = vel; v->at = at < 0 ? 0 : at; v->x = x;
}
SMP_API void smp_engine_note_on(SmpEngine *e, int key, int vel, int at) { push(e, 0, key, vel, at, 0); }
SMP_API void smp_engine_note_off(SmpEngine *e, int key, int at) { push(e, 1, key, 0, at, 0); }
SMP_API void smp_engine_bend(SmpEngine *e, double x, int at) { push(e, 2, 0, 0, at, x); }

/* ---- voices ---- */
typedef struct { double p[SMP_NPARAMS]; } Snap;

static unsigned xorshift(SmpEngine *e) {
  unsigned x = e->rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  e->rng = x;
  return x;
}

static double db2a(double db) { return pow(10.0, db / 20.0); }

static void env_decay(SmpEngine *e, SmpVoice *v, const Snap *P) {
  double sus = db2a(P->p[SMP_P_SUSTAIN]), d = P->p[SMP_P_DECAY] * e->rate / 1000.0;
  if (d < 1 || sus >= 1) { v->stage = 2; v->env = sus; return; }
  v->stage = 1; v->env = 1; v->left = (int64_t)d; v->k = pow(sus, 1.0 / (double)v->left);
}

static void env_release(SmpEngine *e, SmpVoice *v, const Snap *P) {
  double n = P->p[SMP_P_RELEASE] * e->rate / 1000.0;
  if (v->env <= ENV_FLOOR) { v->stage = 4; return; }
  if (n < 1) n = 1;
  v->stage = 3; v->left = (int64_t)ceil(n); v->k = pow(ENV_FLOOR / v->env, 1.0 / (double)v->left);
}

static void kill(SmpVoice *v) { v->live = 0; }

static void fade_out(SmpEngine *e, SmpVoice *v) {
  if (v->fading) return;
  v->fading = 1; v->fade = 1; v->fade_step = 1.0 / (0.005 * e->rate);
}

/* asks the render thread for a copy (single producer: the audio thread) */
static void kl_request(SmpEngine *e, int slot) {
  LONG w = e->rq_w;
  e->rq[w & (SMP_RQ - 1)] = slot;
  InterlockedExchange(&e->rq_w, w + 1);
  if (e->rwork) SetEvent(e->rwork);
}

/* With keep length on: the ready copy for the key's nearest integer shift, or NULL (then the note resamples).
   *self is set when shift 0 at 100 % plays the source itself. */
static SmpKl *kl_pick(SmpEngine *e, const Snap *P, SmpSample *s, int rev, int key, int *self) {
  int n; LONG sp; SmpKl *k; uint64_t base, khf; int64_t F, rs, re, rls;
  *self = 0;
  if (P->p[SMP_P_KEEP_LENGTH] < 0.5) return NULL;
  n = (int)floor(smp_note_semis(P->p, key) + 0.5);
  sp = smp_speedp(P->p[SMP_P_SPEED]);
  if (n == 0 && sp == 10000) { *self = 1; return NULL; }
  if (n < -SMP_KLRENDER || n > SMP_KLRENDER) return NULL;   /* never rendered: resample */
  base = rev ? s->rev_key : s->src_key;
  k = e->kl[n + SMP_KLMAX];
  if (!k || k->base != base || k->speedp != sp) { kl_request(e, n + SMP_KLMAX); return NULL; }
  /* a start past the copy's head needs the copy's start head (built lazily by the worker) */
  F = (int64_t)smp_entry_frames(k->e);
  if (F <= 0) return NULL;
  smp_region(P->p[SMP_P_START], P->p[SMP_P_END], P->p[SMP_P_LOOP_START], F, rev, &rs, &re, &rls);
  smp_entry_head(k->e, &khf);
  if (!smp_entry_whole(k->e) && rs >= (int64_t)khf) {
    const SmpStartHead *sh = k->sh;
    if (!sh || sh->at != rs) {
      InterlockedExchange(&k->want_sh, 1);
      SetEvent(e->work);
      if (e->offline) { smp_worker_wait_idle(e); sh = k->sh; }   /* offline renders are exact */
      if (!sh || sh->at != rs) return NULL;              /* resample until it's there */
    }
  }
  return k;
}

/* ---- glide ----
   A note starts at the pitch the last note is at (where its own glide has got to) and slides to its own over the
   Glide time, in semitones at an even rate. Glide mode legato glides only while another key is held; and with Voices
   at 1 a held note isn't started again: the sounding one carries on and slides to the new key, and back to the
   latest key still held when that one is let go. */
static int held_other(const SmpEngine *e, int key) {
  int k;
  for (k = 0; k < 128; k++) if (e->held[k] && k != key) return 1;
  return 0;
}
static int glide_from(const SmpEngine *e, const Snap *P, double *from) {
  const SmpVoice *l = e->lastv;
  if (!e->have_last) return 0;
  *from = l && l->live && l->gen == e->last_gen ? smp_note_semis(P->p, l->key) + l->gl : e->last_pitch;
  return 1;
}
static void glide_to(SmpEngine *e, const Snap *P, SmpVoice *v, int key, int glide, double from) {
  double to = smp_note_semis(P->p, key), t = P->p[SMP_P_GLIDE] * e->rate / 1000.0;
  v->key = key; v->gl = 0; v->gl_step = 0; v->gl_left = 0;
  if (glide && t >= 1 && fabs(from - to) > 1e-9) {
    v->gl = from - to; v->gl_left = (int64_t)ceil(t); v->gl_step = -v->gl / (double)v->gl_left;
  }
  e->lastv = v; e->last_gen = v->gen; e->last_pitch = to; e->have_last = 1;
}
static SmpVoice *carried(SmpEngine *e) {             /* Voices at 1, legato: the note still held and sounding */
  int i;
  for (i = 0; i < SMP_MAXV; i++) {
    SmpVoice *v = &e->v[i];
    if (v->live && !v->fading && v->stage < 3 && e->held[v->key]) return v;
  }
  return NULL;
}
static int mono_legato(const Snap *P) { return P->p[SMP_P_GLIDE_MODE] >= 0.5 && (int)P->p[SMP_P_VOICES] == 1; }

static void note_on(SmpEngine *e, const Snap *P, int key, int vel) {
  SmpSample *s = e->cur; SmpEntry *src, *rev; SmpVoice *v = NULL; SmpKl *k; int self;
  int i, count = 0, voices = (int)P->p[SMP_P_VOICES], nr = e->nring;   /* slots with a ring */
  int64_t F, rs, re, rls; uint64_t hf; double vdb, from = 0; int loop, glide;
  if (!s || !s->fwd || nr <= 0) return;
  if (voices > nr) voices = nr;                          /* more rings are being committed by the worker */
  if (key < P->p[SMP_P_NOTE_LO] || key > P->p[SMP_P_NOTE_HI]) return;
  if (vel < P->p[SMP_P_VEL_LO] || vel > P->p[SMP_P_VEL_HI]) return;
  if (P->p[SMP_P_PROBABILITY] < 100 && xorshift(e) / 4294967296.0 * 100.0 >= P->p[SMP_P_PROBABILITY]) return;
  glide = (P->p[SMP_P_GLIDE_MODE] < 0.5 || held_other(e, key)) && glide_from(e, P, &from);
  if (mono_legato(P) && held_other(e, key) && (v = carried(e)) != NULL) {
    e->held[key] = ++e->hseq;
    glide_to(e, P, v, key, glide, from);
    return;
  }
  rev = P->p[SMP_P_REVERSE] >= 0.5 ? *(SmpEntry *volatile *)&s->rev : NULL;
  src = rev ? rev : s->fwd;
  k = kl_pick(e, P, s, rev != NULL, key, &self);
  if (k) src = k->e;
  F = (int64_t)smp_entry_frames(src);
  if (F <= 0) return;
  /* steal the oldest sounding voices beyond the limit */
  for (i = 0; i < SMP_MAXV; i++) if (e->v[i].live && !e->v[i].fading) count++;
  while (count >= voices) {
    SmpVoice *old = NULL;
    for (i = 0; i < SMP_MAXV; i++) {
      SmpVoice *w = &e->v[i];
      if (w->live && !w->fading && (!old || w->order < old->order)) old = w;
    }
    if (!old) break;
    fade_out(e, old); count--;
  }
  for (i = 0; i < nr; i++) if (!e->v[i].live) { v = &e->v[i]; break; }
  if (!v) {                                              /* every slot busy fading: cut the oldest */
    for (i = 0; i < nr; i++) if (!v || e->v[i].order < v->order) v = &e->v[i];
    kill(v);
  }
  smp_region(P->p[SMP_P_START], P->p[SMP_P_END], P->p[SMP_P_LOOP_START], F, rev != NULL, &rs, &re, &rls);
  /* a note that ignores note-off plays to the end: no loop, or it would never stop */
  loop = P->p[SMP_P_OBEY_NOTE_OFF] >= 0.5 ? (int)P->p[SMP_P_LOOP] : 0;
  v->src = src; v->rs = rs; v->re = re; v->rls = rls; v->loop = loop;
  v->head = smp_entry_head(src, &hf); v->head_frames = hf;
  v->whole = smp_entry_whole(src);
  v->path_len = re - rs;
  v->sh = NULL; v->hp = v->head + 2 * rs; v->v0 = 0;
  if (v->whole) v->v0 = SMP_NOFILL;
  else {
    SmpStartHead *sh = k ? k->sh : e->sh[rev ? 1 : 0];
    if (sh && (k || sh->s == s) && sh->src == src && sh->at == rs) {   /* the start point's pre-read frames */
      v->sh = sh; v->hp = sh->lr; v->v0 = sh->n < v->path_len ? sh->n : v->path_len;
    } else if (rs < (int64_t)hf) v->v0 = (re < (int64_t)hf ? re : (int64_t)hf) - rs;
  }
  v->src_semis = k ? k->semis : 0;
  v->kl = k || self;
  v->key = key; v->pos = 0; v->fading = 0; v->underrun = 0; v->ug = 1;
  vdb = -P->p[SMP_P_VELOCITY_SENS] / 100.0 * 40.0 * (127 - (vel > 127 ? 127 : vel)) / 126.0;
  if (vdb < P->p[SMP_P_MIN_VOLUME]) vdb = P->p[SMP_P_MIN_VOLUME];
  v->vgain = db2a(vdb);
  if (P->p[SMP_P_ATTACK] * e->rate / 1000.0 >= 1) { v->stage = 0; v->env = 0; v->att_inc = 1.0 / (P->p[SMP_P_ATTACK] * e->rate / 1000.0); }
  else env_decay(e, v, P);
  v->order = ++e->order;
  v->smp = s;
  v->live = 1;
  {
    LONG g = InterlockedIncrement(&v->gen);
    v->need = SMP_TAG(g);
  }
  e->held[key] = ++e->hseq;
  glide_to(e, P, v, key, glide, from);
  /* No pre-read frames (the start head isn't ready yet, e.g. start just moved): the ring is needed at once.
     Real time can't wait (the first frames underrun); offline renders let the streamer fill it first. */
  if (e->offline && v->v0 == 0) smp_stream_wait_idle(e);
}

static void note_off(SmpEngine *e, const Snap *P, int key) {
  int i, k, back = -1;
  e->held[key] = 0;
  if (P->p[SMP_P_OBEY_NOTE_OFF] < 0.5) return;
  if (mono_legato(P)) {                                  /* back to the latest key still held, if any */
    for (k = 0; k < 128; k++) if (e->held[k] && (back < 0 || e->held[k] > e->held[back])) back = k;
    for (i = 0; back >= 0 && i < SMP_MAXV; i++) {
      SmpVoice *v = &e->v[i];
      if (v->live && v->key == key && v->stage < 3 && !v->fading) {
        double from = smp_note_semis(P->p, v->key) + v->gl;
        glide_to(e, P, v, back, 1, from);
        return;
      }
    }
  }
  for (i = 0; i < SMP_MAXV; i++) {
    SmpVoice *v = &e->v[i];
    if (v->live && v->key == key && v->stage < 3 && !v->fading) env_release(e, v, P);
  }
}

static double voice_semis(const SmpEngine *e, const SmpVoice *v, const Snap *P) {
  return smp_note_semis(P->p, v->key) + e->bend * P->p[SMP_P_BEND_RANGE] - v->src_semis;
}

/* one stereo frame at path index idx; 0 = silence, -1 = not streamed yet */
static __inline int fetch(const SmpVoice *v, int64_t idx, int64_t avail, float *L, float *R) {
  const float *f;
  if (idx < 0 || (!v->loop && idx >= v->path_len)) { *L = *R = 0; return 0; }
  if (v->whole) {
    int64_t at, run; int dir;
    if (!smp_path_run(v->rs, v->re, v->rls, v->loop, idx, &at, &dir, &run)) { *L = *R = 0; return 0; }
    f = v->head + 2 * at;
  } else if (idx < v->v0) f = v->hp + 2 * idx;
  else if (idx < avail) f = v->ring + 2 * (idx & SMP_RMASK);
  else { *L = *R = 0; return -1; }
  *L = f[0]; *R = f[1];
  return 1;
}

static void render_voice(SmpEngine *e, SmpVoice *v, const Snap *P, float *l, float *r, int a, int b,
                         double gl, double gr) {
  double base = pow(2.0, voice_semis(e, v, P) * LOG2_12) * smp_entry_rate(v->src) / e->rate *
                (v->kl ? 1.0 : P->p[SMP_P_SPEED] / 100.0);    /* a keep-length copy has speed in it already */
  double ratio = v->gl_left > 0 ? base * pow(2.0, v->gl * LOG2_12) : base,
         gmul = v->gl_left > 0 ? pow(2.0, v->gl_step * LOG2_12) : 1;   /* a glide: the ratio's change per sample */
  double sus = db2a(P->p[SMP_P_SUSTAIN]), la = SMP_UFADE * ratio;   /* la: path frames SMP_UFADE samples ahead */
  int64_t avail = 0;
  int i;
  if (!v->whole) {
    LONG64 f = v->filled;
    if (SMP_TAGOF(f) == (v->gen & 0xFFFFFF)) avail = SMP_VALOF(f);
  }
  if (v->stage == 2) v->env = sus;
  for (i = a; i < b; i++) {
    int64_t i0 = (int64_t)floor(v->pos);
    float x0l, x0r, x1l, x1r, x2l, x2r, x3l, x3r;
    double t = v->pos - (double)i0, g;
    int dry = (fetch(v, i0 - 1, avail, &x0l, &x0r) < 0) | (fetch(v, i0, avail, &x1l, &x1r) < 0) |
              (fetch(v, i0 + 1, avail, &x2l, &x2r) < 0) | (fetch(v, i0 + 2, avail, &x3l, &x3r) < 0);
    if (!v->whole) {                                     /* fade out before the ring runs dry and back in after */
      int64_t look = (int64_t)(v->pos + la) + 2;
      if (dry || (look >= v->v0 && look >= avail && (v->loop || look < v->path_len))) {
        if ((v->ug -= 1.0 / SMP_UFADE) < 0) v->ug = 0;
      } else if (v->ug < 1 && (v->ug += 1.0 / SMP_UFADE) > 1) v->ug = 1;
    }
    if (dry) v->underrun = 1;                            /* the ring ran dry: silence, counted per block */
    else {
      float c1l = 0.5f * (x2l - x0l), c2l = x0l - 2.5f * x1l + 2.0f * x2l - 0.5f * x3l, c3l = 0.5f * (x3l - x0l) + 1.5f * (x1l - x2l);
      float c1r = 0.5f * (x2r - x0r), c2r = x0r - 2.5f * x1r + 2.0f * x2r - 0.5f * x3r, c3r = 0.5f * (x3r - x0r) + 1.5f * (x1r - x2r);
      float tf = (float)t;
      float yl = ((c3l * tf + c2l) * tf + c1l) * tf + x1l, yr = ((c3r * tf + c2r) * tf + c1r) * tf + x1r;
      g = v->env * v->vgain * v->ug;
      if (v->fading) g *= v->fade;
      l[i] += (float)(yl * g * gl); r[i] += (float)(yr * g * gr);
    }
    v->pos += ratio;
    if (v->gl_left > 0) {
      if (--v->gl_left == 0) { v->gl = 0; ratio = base; }
      else { v->gl += v->gl_step; ratio *= gmul; }
    }
    switch (v->stage) {                                  /* envelope, one step */
      case 0: v->env += v->att_inc; if (v->env >= 1) env_decay(e, v, P); break;
      case 1: v->env *= v->k; if (--v->left <= 0) { v->stage = 2; v->env = sus; } break;
      case 3: v->env *= v->k; if (--v->left <= 0) v->stage = 4; break;
    }
    if (v->fading && (v->fade -= v->fade_step) <= 0) v->stage = 4;
    if (v->stage == 4 || (!v->loop && v->pos >= (double)v->path_len)) { kill(v); return; }
  }
}

static void render(SmpEngine *e, const Snap *P, float *l, float *r, int a, int b) {
  double vol = P->p[SMP_P_VOLUME] <= -60 ? 0 : db2a(P->p[SMP_P_VOLUME]);
  double th = (P->p[SMP_P_PAN] / 100.0 + 1) * 0.78539816339744831;   /* equal power, unity at center */
  double gl = cos(th) * 1.4142135623730951 * vol, gr = sin(th) * 1.4142135623730951 * vol;
  int i;
  if (b <= a) return;
  for (i = 0; i < SMP_MAXV; i++) if (e->v[i].live) render_voice(e, &e->v[i], P, l, r, a, b, gl, gr);
}

SMP_API void smp_engine_process(SmpEngine *e, float *l, float *r, int n) {
  if (n > 0) { memset(l, 0, sizeof(float) * (size_t)n); memset(r, 0, sizeof(float) * (size_t)n); }
  smp_engine_process_mix(e, l, r, n);
}

/* adds the block into l and r. An engine with no voice sounding, no event queued and no new sample only counts the
   block (its retirements go on), so many silent engines cost next to nothing (multi.c) */
void smp_engine_process_mix(SmpEngine *e, float *l, float *r, int n) {
  Snap P; int i, j, pos = 0;
  SmpSample *s = e->sample;
  if (n <= 0) { e->nev = 0; return; }
  if (!e->nev && s == e->cur) {
    for (i = 0; i < SMP_MAXV; i++) if (e->v[i].live || e->v[i].underrun) break;
    if (i == SMP_MAXV) { InterlockedIncrement64(&e->epoch); return; }
  }
  if (s != e->cur) {                                     /* a new sample: fade out the old one's voices */
    for (i = 0; i < SMP_MAXV; i++) if (e->v[i].live && e->v[i].smp != s) fade_out(e, &e->v[i]);
    e->cur = s;
  }
  for (i = 0; i < SMP_NPARAMS; i++) P.p[i] = smp_eng_param(e, i);
  for (i = 1; i < e->nev; i++) {                         /* stable sort by offset */
    SmpEv x = e->ev[i];
    for (j = i - 1; j >= 0 && e->ev[j].at > x.at; j--) e->ev[j + 1] = e->ev[j];
    e->ev[j + 1] = x;
  }
  for (i = 0; i < e->nev; i++) {
    SmpEv *ev = &e->ev[i];
    int at = ev->at >= n ? n - 1 : ev->at;
    if (at > pos) { render(e, &P, l, r, pos, at); pos = at; }
    if (ev->type == 0) note_on(e, &P, ev->key, ev->vel);
    else if (ev->type == 1) note_off(e, &P, ev->key);
    else e->bend = ev->x < -1 ? -1 : ev->x > 1 ? 1 : ev->x;
  }
  e->nev = 0;
  render(e, &P, l, r, pos, n);
  for (i = 0; i < SMP_MAXV; i++) {
    SmpVoice *v = &e->v[i];
    if (v->underrun) { InterlockedIncrement(&e->underruns); v->underrun = 0; }
    if (!v->live) continue;
    if (!v->whole) {
      int64_t need = (int64_t)floor(v->pos) - 1;
      v->need = SMP_TAG(v->gen) | (need < 0 ? 0 : need);
    }
  }
  InterlockedIncrement64(&e->epoch);
  SetEvent(e->wake);
}

SMP_API void smp_engine_reset(SmpEngine *e, int now) {
  int i;
  if (!e) return;
  for (i = 0; i < SMP_MAXV; i++) {
    SmpVoice *v = &e->v[i];
    if (!v->live) continue;
    if (now) kill(v); else fade_out(e, v);
  }
  e->bend = 0;
  e->nev = 0;
  memset(e->held, 0, sizeof e->held); e->have_last = 0;
}

SMP_API int smp_engine_active_voices(const SmpEngine *e) {
  int i, n = 0;
  if (!e) return 0;
  for (i = 0; i < SMP_MAXV; i++) if (e->v[i].live && !e->v[i].fading) n++;
  return n;
}

SMP_API long smp_engine_underruns(const SmpEngine *e) { return e ? e->underruns : 0; }
SMP_API void smp_debug_stream_pause(SmpEngine *e, int on) { if (e) InterlockedExchange(&e->spause, on); }

/* ---- offline test render ---- */
typedef struct { long frame; int type, key, vel; double x; } TEv;

static void wav_header(FILE *f, uint32_t frames, double rate) {
  uint32_t data = frames * 8, riff = 36 + data, sr = (uint32_t)(rate + 0.5), br = sr * 8, fmt = 16;
  uint16_t tag = 3, ch = 2, align = 8, bits = 32;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
  fwrite(&tag, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
  fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

SMP_API int smp_engine_render_test(SmpEngine *e, const char *events, long frames, int block, const char *wav_out) {
  TEv *ev = NULL; int nev = 0, cap = 0, k = 0; const char *p = events;
  float *l, *r, *lr; FILE *f; wchar_t w[2048]; long b;
  if (!e || !wav_out || frames < 0 || block <= 0) return 1;
  while (p && *p) {                                      /* parse "on key vel frame" / "off key frame" / "bend x frame" */
    TEv t; char word[16]; int used = 0; const char *nl = strchr(p, '\n');
    memset(&t, 0, sizeof t);
    if (sscanf(p, "%15s%n", word, &used) == 1) {
      int ok = 0;
      if (!strcmp(word, "on")) { t.type = 0; ok = sscanf(p + used, "%d %d %ld", &t.key, &t.vel, &t.frame) == 3; }
      else if (!strcmp(word, "off")) { t.type = 1; ok = sscanf(p + used, "%d %ld", &t.key, &t.frame) == 2; }
      else if (!strcmp(word, "bend")) { t.type = 2; ok = sscanf(p + used, "%lf %ld", &t.x, &t.frame) == 2; }
      if (!ok) { free(ev); return 2; }
      if (nev == cap) { TEv *n2; cap = cap ? cap * 2 : 64; n2 = realloc(ev, sizeof *ev * (size_t)cap); if (!n2) { free(ev); return 3; } ev = n2; }
      ev[nev++] = t;
    }
    p = nl ? nl + 1 : NULL;
  }
  { /* stable sort: insertion */
    int i, j;
    for (i = 1; i < nev; i++) { TEv x = ev[i]; for (j = i - 1; j >= 0 && ev[j].frame > x.frame; j--) ev[j + 1] = ev[j]; ev[j + 1] = x; }
  }
  if (!MultiByteToWideChar(CP_UTF8, 0, wav_out, -1, w, 2048) || !(f = _wfopen(w, L"wb"))) { free(ev); return 4; }
  l = malloc(sizeof(float) * (size_t)block); r = malloc(sizeof(float) * (size_t)block); lr = malloc(sizeof(float) * 2 * (size_t)block);
  if (!l || !r || !lr) { free(l); free(r); free(lr); free(ev); fclose(f); return 3; }
  wav_header(f, (uint32_t)frames, e->rate);
  smp_worker_wait_idle(e);                              /* start heads and reversed copies ready */
  { /* every render starts from silence: no voices from an earlier render, no bend */
    int i;
    for (i = 0; i < SMP_MAXV; i++) e->v[i].live = 0;
    e->bend = 0;
    memset(e->held, 0, sizeof e->held); e->have_last = 0;
  }
  e->offline = 1;
  for (b = 0; b < frames; b += block) {
    int n = (int)(frames - b < block ? frames - b : block), i;
    for (; k < nev && ev[k].frame < b + n; k++) {
      int at = (int)(ev[k].frame - b);
      if (at < 0) at = 0;
      if (ev[k].type == 0) smp_engine_note_on(e, ev[k].key, ev[k].vel, at);
      else if (ev[k].type == 1) smp_engine_note_off(e, ev[k].key, at);
      else smp_engine_bend(e, ev[k].x, at);
    }
    smp_engine_process(e, l, r, n);
    for (i = 0; i < n; i++) { lr[2 * i] = l[i]; lr[2 * i + 1] = r[i]; }
    fwrite(lr, sizeof(float) * 2, (size_t)n, f);
    smp_stream_wait_idle(e);                             /* offline only: let the streamer catch up */
  }
  e->offline = 0;
  free(l); free(r); free(lr); free(ev);
  return fclose(f) ? 5 : 0;
}
