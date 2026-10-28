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
    } else {                                              /* elsewhere: the streamer makes it; forward until then */
      InterlockedExchange(&e->want_rev, 1);
      if (e->wake) SetEvent(e->wake);
    }
  }
}

/* ---- life cycle ---- */
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
  /* committed now; pages are only touched as the streamer fills a voice's ring */
  e->rings = VirtualAlloc(NULL, (SIZE_T)SMP_MAXV * SMP_RING * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!e->rings) { DeleteCriticalSection(&e->rev_cs); free(e); return NULL; }
  for (i = 0; i < SMP_MAXV; i++) { e->v[i].ring = e->rings + (size_t)i * SMP_RING * 2; e->v[i].s_gen = -1; }
  if (!smp_stream_start(e)) { smp_engine_free(e); return NULL; }
  return e;
}

SMP_API void smp_engine_free(SmpEngine *e) {
  int i;
  if (!e) return;
  smp_stream_stop(e);
  for (i = 0; i < e->nret; i++) smp_sample_free(e->ret[i].s);
  free(e->ret);
  smp_sample_free(e->sample);
  if (e->rings) VirtualFree(e->rings, 0, MEM_RELEASE);
  DeleteCriticalSection(&e->rev_cs);
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
    if (r->phase == 0 && e->epoch >= r->a0 + 2 && !voice_uses(e, r->s)) { r->s0 = e->spass; r->phase = 1; }
    if (r->phase == 1 && e->spass >= r->s0 + 2) {
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
      r->s = old; r->a0 = e->epoch; r->s0 = 0; r->phase = 0;
    } /* else out of memory: leak the old sample rather than free it under a voice */
  }
  smp_engine_collect(e);
}

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

static void note_on(SmpEngine *e, const Snap *P, int key, int vel) {
  SmpSample *s = e->cur; SmpEntry *src, *rev; SmpVoice *v = NULL;
  int i, count = 0, voices = (int)P->p[SMP_P_VOICES];
  int64_t F, sf, ef, lsf, rs, re, rls; uint64_t hf; double vdb;
  if (!s || !s->fwd) return;
  if (key < P->p[SMP_P_NOTE_LO] || key > P->p[SMP_P_NOTE_HI]) return;
  if (vel < P->p[SMP_P_VEL_LO] || vel > P->p[SMP_P_VEL_HI]) return;
  if (P->p[SMP_P_PROBABILITY] < 100 && xorshift(e) / 4294967296.0 * 100.0 >= P->p[SMP_P_PROBABILITY]) return;
  rev = P->p[SMP_P_REVERSE] >= 0.5 ? *(SmpEntry *volatile *)&s->rev : NULL;
  src = rev ? rev : s->fwd;
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
  for (i = 0; i < SMP_MAXV; i++) if (!e->v[i].live) { v = &e->v[i]; break; }
  if (!v) {                                              /* every slot busy fading: cut the oldest */
    for (i = 0; i < SMP_MAXV; i++) if (!v || e->v[i].order < v->order) v = &e->v[i];
    kill(v);
  }
  /* the region, in src frames; reversed plays the mirrored region, loop start keeps its offset into it */
  sf = (int64_t)floor(P->p[SMP_P_START] / 100.0 * F);
  ef = (int64_t)floor(P->p[SMP_P_END] / 100.0 * F);
  if (sf >= F) sf = F - 1;
  if (ef > F) ef = F;
  if (ef <= sf) ef = sf + 1;
  lsf = (int64_t)floor(P->p[SMP_P_LOOP_START] / 100.0 * F);
  if (lsf < sf) lsf = sf;
  if (lsf >= ef) lsf = ef - 1;
  if (rev) { rs = F - ef; re = F - sf; rls = rs + (lsf - sf); }
  else { rs = sf; re = ef; rls = lsf; }
  v->src = src; v->rs = rs; v->re = re; v->rls = rls; v->loop = (int)P->p[SMP_P_LOOP];
  v->head = smp_entry_head(src, &hf); v->head_frames = hf;
  v->whole = smp_entry_whole(src);
  v->v0 = v->whole ? SMP_NOFILL : (rs < (int64_t)hf ? ((re < (int64_t)hf ? re : (int64_t)hf) - rs) : 0);
  v->path_len = re - rs;
  v->src_semis = 0;
  v->key = key; v->pos = 0; v->fading = 0; v->underrun = 0;
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
  /* A region starting past the head needs the ring at once. Real time can't wait (the first frames underrun);
     offline renders let the streamer fill it first so they come out exact. */
  if (e->offline && v->v0 == 0) smp_stream_wait_idle(e);
}

static void note_off(SmpEngine *e, const Snap *P, int key) {
  int i;
  if (P->p[SMP_P_OBEY_NOTE_OFF] < 0.5) return;
  for (i = 0; i < SMP_MAXV; i++) {
    SmpVoice *v = &e->v[i];
    if (v->live && v->key == key && v->stage < 3 && !v->fading) env_release(e, v, P);
  }
}

static double voice_semis(const SmpEngine *e, const SmpVoice *v, const Snap *P) {
  int mode = (int)P->p[SMP_P_MODE];
  double s = 0;
  if (mode == 1) s = v->key - P->p[SMP_P_ROOT];
  else if (mode == 2) {
    double nl = P->p[SMP_P_NOTE_LO], nh = P->p[SMP_P_NOTE_HI], pl = P->p[SMP_P_PITCH_LO], ph = P->p[SMP_P_PITCH_HI];
    s = nh > nl ? pl + (v->key - nl) * (ph - pl) / (nh - nl) : pl;
  }
  return s + P->p[SMP_P_TRANSPOSE] + P->p[SMP_P_FINE] / 100.0 + e->bend * P->p[SMP_P_BEND_RANGE] - v->src_semis;
}

/* one stereo frame at path index idx; 0 = silence, -1 = not streamed yet */
static __inline int fetch(const SmpVoice *v, int64_t idx, int64_t avail, float *L, float *R) {
  const float *f;
  if (idx < 0 || (!v->loop && idx >= v->path_len)) { *L = *R = 0; return 0; }
  if (v->whole) {
    int64_t at, run; int dir;
    if (!smp_path_run(v->rs, v->re, v->rls, v->loop, idx, &at, &dir, &run)) { *L = *R = 0; return 0; }
    f = v->head + 2 * at;
  } else if (idx < v->v0) f = v->head + 2 * (v->rs + idx);
  else if (idx < avail) f = v->ring + 2 * (idx & SMP_RMASK);
  else { *L = *R = 0; return -1; }
  *L = f[0]; *R = f[1];
  return 1;
}

static void render_voice(SmpEngine *e, SmpVoice *v, const Snap *P, float *l, float *r, int a, int b,
                         double gl, double gr) {
  double ratio = pow(2.0, voice_semis(e, v, P) * LOG2_12) * smp_entry_rate(v->src) / e->rate * P->p[SMP_P_SPEED] / 100.0;
  double sus = db2a(P->p[SMP_P_SUSTAIN]);
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
    if (dry) v->underrun = 1;                            /* the ring ran dry: silence, counted per block */
    else {
      float c1l = 0.5f * (x2l - x0l), c2l = x0l - 2.5f * x1l + 2.0f * x2l - 0.5f * x3l, c3l = 0.5f * (x3l - x0l) + 1.5f * (x1l - x2l);
      float c1r = 0.5f * (x2r - x0r), c2r = x0r - 2.5f * x1r + 2.0f * x2r - 0.5f * x3r, c3r = 0.5f * (x3r - x0r) + 1.5f * (x1r - x2r);
      float tf = (float)t;
      float yl = ((c3l * tf + c2l) * tf + c1l) * tf + x1l, yr = ((c3r * tf + c2r) * tf + c1r) * tf + x1r;
      g = v->env * v->vgain;
      if (v->fading) g *= v->fade;
      l[i] += (float)(yl * g * gl); r[i] += (float)(yr * g * gr);
    }
    v->pos += ratio;
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
  Snap P; int i, j, pos = 0;
  SmpSample *s = e->sample;
  if (n <= 0) { e->nev = 0; return; }
  memset(l, 0, sizeof(float) * (size_t)n); memset(r, 0, sizeof(float) * (size_t)n);
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

SMP_API int smp_engine_active_voices(const SmpEngine *e) {
  int i, n = 0;
  if (!e) return 0;
  for (i = 0; i < SMP_MAXV; i++) if (e->v[i].live && !e->v[i].fading) n++;
  return n;
}

SMP_API long smp_engine_underruns(const SmpEngine *e) { return e ? e->underruns : 0; }

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
  { /* every render starts from silence: no voices from an earlier render, no bend */
    int i;
    for (i = 0; i < SMP_MAXV; i++) e->v[i].live = 0;
    e->bend = 0;
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
