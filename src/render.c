/* render.c: keep length. Every pitched note lasts as long as the recording: for each integer shift the key range
   reaches, the stretcher chosen in Settings (Signalsmith Stretch, or Rubber Band R3 offline; either one keeping the
   tone or not) renders a copy of the sample at that pitch (and at 1/speed of its length), written to the cache under
   kl|<src or rev key>|<semis>|<speed %.2f>|<ss, sst, r3 or r3t>. Changing the stretcher makes new copies; the old ones
   stay in the cache, so changing back is instant.

   The render thread (one per engine) plans; the copies themselves are rendered on one process-wide thread pool, as
   many at once as the processors allow less two (at most SMP_KLJOBS), at below normal priority:
   - Each pass it reads the current sample, keep length, reverse, speed and the stretcher settings, and works out
     the shifts note_lo to note_hi reach (with the same formula note-on uses). It wants every reachable shift,
     nearest the root first, while their copies fit in a quarter of the cache limit (and always at least
     SMP_KLFLOOR, an octave each way), plus the shifts note-ons have asked for (a small ring the audio thread writes
     when a note finds no copy). When the configuration changes it waits 100 ms for it to settle (scrolling through
     files or dragging speed renders nothing) and forgets what note-ons asked for, then works on asked-for shifts
     first (in the order asked), then nearest the root.
   - Copies already in the cache are just opened. The rest go out as a batch, as many as the pool runs at once and
     their temporary memory allows (SMP_KLMEM), sharing one deinterleaved copy of the source; each copy is published
     as soon as it's done. While a batch runs the thread watches: a change of configuration (or the engine
     quitting) stops every job between chunks, and a note-on asking for a shift that isn't ready and isn't in the
     batch stops the jobs the thread chose itself: that shift goes in the next batch, the stopped ones come back in
     their turn.
   - Copies are opened with smp_cache_open_copy: only the 0.25 s head is held in memory (whole only when shorter);
     the rest is streamed like any long sample.
   - Ready copies are published into e->kl[shift + 127] by atomic pointer. A copy is identified by content (the
     key it was rendered from and the speed), so the audio thread just ignores one that doesn't match its note.
   - Stale copies (another sample, speed, direction or stretcher, keep length off, or a shift the key range no
     longer reaches) are taken out of the table and retired: one is freed only after the audio thread has finished
     two blocks since (no new note can pick it), no live voice reads it, and then the streamer and the worker have
     each finished two passes (neither can still be reading it or its start head). The retire list grows as needed
     and the render thread never waits on it; if it can't grow, the copy is left in place (stale copies are
     harmless) or a new copy isn't published.
   - The sample it reads is protected from smp_engine_collect by e->r_using: it is set, then e->sample re-checked,
     so collect (which runs after the swap) sees it whenever the thread could still be reading the sample. Pool jobs
     read only the batch's own copy of the source.

   A note-on with keep length on plays the ready copy for the nearest integer shift and resamples the rest (fine,
   bend, free mode) at 2^(rest/12); speed is already in the copy. Shift 0 at 100 % is the source itself. A note
   whose copy isn't ready (or whose copy needs a start head the worker hasn't built yet) resamples for the whole
   note, with speed scaling the rate as with keep length off. Shifts past 4 octaves (SMP_KLRENDER) are never rendered,
   because Rubber Band R3 breaks down there; they always resample. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine_int.h"

#define RD_CHUNK 4096
#define SETTLE_MS 100
#define DIR_RECHECK_MS 1000
#define WAIT_MS 10
#define SMP_KLMEM (512.0 * 1024 * 1024)   /* bytes: a batch's temporary memory; one copy needing more is refused */
#define SMP_KLJOBS 16                     /* copies rendered at once, at most (fewer on smaller machines) */
#define SMP_KLFLOOR 25                    /* copies always prepared (an octave each way), whatever their size */

static volatile LONG g_renders;
SMP_API long smp_debug_renders(void) { return g_renders; }

LONG smp_speedp(double pct) { return (LONG)floor(pct * 100.0 + 0.5); }

/* the stretcher and keep-tone settings as one number, 0 to 3, and the tag that names them in a copy's key */
static int eng_of(const SmpSettings *s) { return (s->stretcher == SMP_STRETCH_RUBBERBAND ? 2 : 0) + (s->keep_tone ? 1 : 0); }
static const char *const eng_tag[4] = {"ss", "sst", "r3", "r3t"};

static uint64_t kl_key(uint64_t base, int semis, LONG speedp, int eng) {
  char d[96];
  snprintf(d, sizeof d, "kl|%016llx|%d|%.2f|%s", (unsigned long long)base, semis, speedp / 100.0, eng_tag[eng & 3]);
  return smp_key(d);
}

typedef int (*CancelFn)(void *);

/* stretch.cpp: the stretchers, inside try/catch (no C++ exception may reach C or the host) */
int smp_stretch(int engine, int keep_tone, const float *L, const float *R, unsigned F, int rate, double time_ratio,
                double pitch_scale, float *out, unsigned long long T, CancelFn cancel, void *arg, char *why, int cap);
void smp_rb_set_debug_flags(long f);

/* tests: 1 = the stretcher throws inside the guard, 2 = no 4-octave limit (to reach the stretchers' own failures) */
static volatile LONG g_dflags;
SMP_API void smp_debug_render_flags(long f) { g_dflags = f; smp_rb_set_debug_flags(f & 1); }

static uint64_t out_frames(uint64_t F, LONG speedp) {
  uint64_t T = speedp == 10000 ? F : (uint64_t)floor((double)F * 10000.0 / (double)speedp + 0.5);
  return T ? T : 1;
}
/* a copy's temporary memory: the copy (interleaved), and inside the stretcher: Rubber Band about as much again plus
   its own copy of the source (it studies it whole), so with the shared source (F * 8) one copy needs (F + T) * 16;
   Signalsmith Stretch only its few blocks (a megabyte is plenty) */
static double job_bytes(int eng, uint64_t F, uint64_t T) {
  return eng >= 2 ? (double)(F + 2 * T) * 8.0 : (double)T * 8.0 + 1048576.0;
}

/* the source, deinterleaved (both sides): 1 ok, 0 out of memory, 2 cancelled */
static int load_src(SmpEntry *src, float **pL, float **pR, CancelFn cancel, void *arg) {
  uint64_t F = smp_entry_frames(src), i;
  float *L = malloc(sizeof(float) * (size_t)F), *R = malloc(sizeof(float) * (size_t)F), *lr = malloc(sizeof(float) * 2 * RD_CHUNK);
  *pL = *pR = NULL;
  if (!L || !R || !lr) { free(L); free(R); free(lr); return 0; }
  for (i = 0; i < F; i += RD_CHUNK) {
    long n = (long)(F - i < RD_CHUNK ? F - i : RD_CHUNK), k, g = smp_entry_read(src, i, lr, n);
    if (g < 0) g = 0;
    for (k = 0; k < n; k++) { L[i + k] = k < g ? lr[2 * k] : 0; R[i + k] = k < g ? lr[2 * k + 1] : 0; }
    if (cancel && cancel(arg)) { free(L); free(R); free(lr); return 2; }
  }
  free(lr);
  *pL = L; *pR = R;
  return 1;
}

/* 0 ok, 1 failed, 2 cancelled, 3 the cache write failed, 4 too big (temporary memory past SMP_KLMEM);
   why: a plain reason for 1, 3 and 4 */
static int render_copy(const float *L, const float *R, uint64_t F, int rate, uint64_t base, int semis, LONG speedp,
                       int eng, CancelFn cancel, void *arg, char *why, int cap) {
  uint64_t T; float *out; int r;
  snprintf(why, (size_t)cap, "a keep-length copy couldn't be made");
  if (!F || speedp <= 0 || rate <= 0) return 1;
  if (!(g_dflags & 2) && (semis < -SMP_KLRENDER || semis > SMP_KLRENDER)) {
    snprintf(why, (size_t)cap, "keep length only shifts up to 4 octaves");
    return 1;
  }
  T = out_frames(F, speedp);
  if (F > 0x7FFFFFFF || (double)F * 8.0 + job_bytes(eng, F, T) > SMP_KLMEM) {
    snprintf(why, (size_t)cap, "the sample is too long for keep-length copies at this speed");
    return 4;
  }
  out = calloc((size_t)T * 2, sizeof(float));
  if (!out) { snprintf(why, (size_t)cap, "out of memory for a keep-length copy"); return 1; }
  r = smp_stretch(eng >= 2, eng & 1, L, R, (unsigned)F, rate, 10000.0 / (double)speedp, pow(2.0, semis / 12.0), out, T,
                  cancel, arg, why, cap);
  if (!r) {
    InterlockedIncrement(&g_renders);
    if (smp_cache_write(kl_key(base, semis, speedp, eng), out, T, rate)) {
      snprintf(why, (size_t)cap, "keep-length copies can't be written to the cache folder");
      r = 3;
    }
  }
  free(out);
  return r;
}

SMP_API int smp_render_shift(SmpSample *s, int reversed, int semis, double speed) {
  SmpEntry *src; SmpSettings st; float *L, *R; char why[300]; int r;
  if (!s || !s->fwd || speed <= 0) return 1;
  src = reversed ? smp_sample_reversed(s) : s->fwd;
  if (!src) return 1;
  if (load_src(src, &L, &R, NULL, NULL) != 1) return 1;
  smp_settings_get(&st);
  r = render_copy(L, R, smp_entry_frames(src), smp_entry_rate(src), reversed ? s->rev_key : s->src_key, semis,
                  smp_speedp(speed * 100.0), eng_of(&st), NULL, NULL, why, sizeof why);
  free(L); free(R);
  return r;
}

/* ---- the shared pool: every engine's copies are rendered on one process-wide thread pool, at most SMP_KLJOBS
   (or the processor count less two) at once, at below normal priority. Each callback holds a reference on this
   module, so it can't be unloaded under a running render. ---- */
static INIT_ONCE pool_once = INIT_ONCE_STATIC_INIT;
static PTP_POOL g_pool; static TP_CALLBACK_ENVIRON g_env; static int g_par = 1;
static volatile LONG64 g_mem;      /* temporary bytes reserved by running batches, every engine's: SMP_KLMEM in all */
static volatile LONG g_engines;    /* engines alive: each prepares its share of a quarter of the cache limit */

/* Reserves a batch's temporary memory out of the process-wide SMP_KLMEM: the shared source (src) and up to want jobs
   of each bytes. Returns how many jobs it reserved for (0: none fit now; try again later); *got is what to give back
   with unreserve. A batch that is alone (nothing else reserved) always gets one job. */
static int reserve_mem(double src, double each, int want, LONG64 *got) {
  for (;;) {
    LONG64 used = g_mem, take; int n;
    double room = (SMP_KLMEM - (double)used - src) / each;
    n = room < 1 ? 0 : room < want ? (int)room : want;
    if (!n && !used) n = 1;
    if (!n) { *got = 0; return 0; }
    take = (LONG64)(src + each * n);
    if (InterlockedCompareExchange64(&g_mem, used + take, used) == used) { *got = take; return n; }
  }
}
static void unreserve_mem(LONG64 got) { if (got) InterlockedAdd64(&g_mem, -got); }

static BOOL CALLBACK pool_init(PINIT_ONCE o, PVOID p, PVOID *c) {
  SYSTEM_INFO si; HMODULE me = NULL; (void)o; (void)p; (void)c;
  GetSystemInfo(&si);
  g_par = (int)si.dwNumberOfProcessors - 2;
  if (g_par > SMP_KLJOBS) g_par = SMP_KLJOBS;
  if (g_par < 1) g_par = 1;
  InitializeThreadpoolEnvironment(&g_env);
  g_pool = CreateThreadpool(NULL);
  if (!g_pool) { g_par = 1; return TRUE; }                 /* jobs then run on the render thread itself */
  SetThreadpoolThreadMaximum(g_pool, (DWORD)g_par);
  SetThreadpoolCallbackPool(&g_env, g_pool);
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCWSTR)&pool_init, &me);
  if (me) SetThreadpoolCallbackLibrary(&g_env, me);
  return TRUE;
}

typedef struct Batch Batch;
typedef struct Job {
  Batch *b; int slot, asked, r, handled; volatile LONG stop, fin, started; char why[300];   /* fin: r and why set */
  PTP_WORK w;
} Job;
struct Batch {
  SmpEngine *e; const float *L, *R; uint64_t F; int rate, eng; uint64_t base; LONG speedp;
  Job job[SMP_KLJOBS]; int n; volatile LONG left; HANDLE done;
};

static int job_cancelled(void *arg) { Job *j = arg; return j->stop || j->b->e->rquit; }

static void job_run(Job *j) {
  Batch *b = j->b;
  if (job_cancelled(j)) { j->r = 2; InterlockedExchange(&j->fin, 1); return; }   /* stopped while it was queued */
  j->r = render_copy(b->L, b->R, b->F, b->rate, b->base, j->slot - SMP_KLMAX, b->speedp, b->eng, job_cancelled, j,
                     j->why, sizeof j->why);
  InterlockedExchange(&j->fin, 1);                       /* a full barrier: r and why are visible first */
}

static VOID CALLBACK job_cb(PTP_CALLBACK_INSTANCE inst, PVOID arg, PTP_WORK w) {
  Job *j = arg; Batch *b = j->b; (void)inst; (void)w;
  InterlockedExchange(&j->started, 1);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  job_run(j);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
  SetEvent(b->done);                                     /* wakes the render thread to publish it */
  InterlockedDecrement(&b->left);                        /* last: the batch may be gone after this */
}

/* ---- the render thread ---- */
typedef struct { SmpSample *s; SmpEntry *src; uint64_t base; LONG speedp; int on, rev, eng; double limit; } Cfg;

static void cfg_read(const SmpEngine *e, SmpSample *s, Cfg *c) {
  SmpEntry *rev = NULL; SmpSettings st;
  memset(c, 0, sizeof *c);
  c->s = s;
  smp_settings_get(&st);
  c->eng = eng_of(&st);
  c->limit = st.cache_limit_gb * 1024.0 * 1024.0 * 1024.0;
  if (!s || !s->fwd) return;
  c->on = smp_eng_param(e, SMP_P_KEEP_LENGTH) >= 0.5;
  if (smp_eng_param(e, SMP_P_REVERSE) >= 0.5) rev = *(SmpEntry *volatile *)&s->rev;
  c->rev = rev != NULL;
  c->src = rev ? rev : s->fwd;
  c->base = rev ? s->rev_key : s->src_key;
  c->speedp = smp_speedp(smp_eng_param(e, SMP_P_SPEED));
}

static int cfg_same(const Cfg *a, const Cfg *b) {
  return a->s == b->s && a->on == b->on && a->base == b->base && a->speedp == b->speedp && a->src == b->src &&
         a->eng == b->eng;   /* not the cache limit: it changes only how many keys are wanted */
}

static int renderable(int slot) { return slot >= SMP_KLMAX - SMP_KLRENDER && slot <= SMP_KLMAX + SMP_KLRENDER; }

static void sh_free1(SmpStartHead *h) { if (h) { free(h->lr); free(h); } }

void smp_kl_free(SmpKl *k) {
  if (!k) return;
  sh_free1(k->sh);
  smp_cache_close(k->e);
  free(k);
}

static int reserve(SmpEngine *e) {
  if (e->nklret < e->cklret) return 1;
  {
    int c = e->cklret ? e->cklret * 2 : 64;
    SmpKlRet *n = realloc(e->klret, sizeof *n * (size_t)c);
    if (!n) return 0;
    e->klret = n; e->cklret = c;
  }
  return 1;
}

static void retire(SmpEngine *e, SmpKl *k) {   /* capacity reserved by the caller */
  SmpKlRet *r = &e->klret[e->nklret++];
  r->k = k; r->a0 = e->epoch; r->s0 = r->w0 = 0; r->phase = 0;
}

static int voice_reads(const SmpEngine *e, const SmpEntry *en) {
  int i;
  for (i = 0; i < SMP_MAXV; i++) if (e->v[i].live && e->v[i].src == en) return 1;
  return 0;
}

static void sweep(SmpEngine *e) {
  int i = 0;
  while (i < e->nklret) {
    SmpKlRet *r = &e->klret[i];
    if (r->phase == 0 && e->epoch >= r->a0 + 2 && !voice_reads(e, r->k->e)) { r->s0 = e->spass; r->w0 = e->wpass; r->phase = 1; }
    if (r->phase == 1 && e->spass >= r->s0 + 2 && e->wpass >= r->w0 + 2) {
      smp_kl_free(r->k);
      e->klret[i] = e->klret[--e->nklret];
      continue;
    }
    i++;
  }
}

static void unpublish_stale(SmpEngine *e, const Cfg *c, const char *reach) {
  int i;
  for (i = 0; i < SMP_KLN; i++) {
    SmpKl *k = e->kl[i];
    if (!k || (c->on && reach[i] && k->base == c->base && k->speedp == c->speedp && k->eng == c->eng)) continue;
    if (!reserve(e)) return;                                  /* left in place: never used, freed later */
    InterlockedExchangePointer((PVOID volatile *)&e->kl[i], NULL);
    retire(e, k);
  }
}

static int publish(SmpEngine *e, int slot, SmpEntry *en, const Cfg *c) {
  SmpKl *k, *old;
  if (!reserve(e) || !(k = calloc(1, sizeof *k))) return 0;
  k->e = en; k->base = c->base; k->speedp = c->speedp; k->eng = c->eng; k->semis = slot - SMP_KLMAX; k->rev = c->rev;
  old = InterlockedExchangePointer((PVOID volatile *)&e->kl[slot], k);   /* k's fields are visible first */
  if (old) retire(e, old);
  return 1;
}

static int is_ready(const SmpEngine *e, int slot, const Cfg *c) {
  const SmpKl *k = e->kl[slot];
  if (slot == SMP_KLMAX && c->speedp == 10000) return 1;       /* shift 0 at 100 %: the source itself */
  return k && k->base == c->base && k->speedp == c->speedp && k->eng == c->eng;
}

/* the renderable shifts note_lo to note_hi reach, as note-on rounds them */
static void reachable(const SmpEngine *e, char reach[SMP_KLN]) {
  double p[SMP_NPARAMS]; int i, k, lo, hi;
  for (i = 0; i < SMP_NPARAMS; i++) p[i] = smp_eng_param(e, i);
  memset(reach, 0, SMP_KLN);
  lo = (int)ceil(p[SMP_P_NOTE_LO]); hi = (int)floor(p[SMP_P_NOTE_HI]);
  for (k = lo; k <= hi; k++) {
    int n = (int)floor(smp_note_semis(p, k) + 0.5);
    if (n >= -SMP_KLRENDER && n <= SMP_KLRENDER) reach[n + SMP_KLMAX] = 1;
  }
}

static void set_why(SmpEngine *e, const char *why) {
  EnterCriticalSection(&e->r_cs);
  snprintf(e->r_why, sizeof e->r_why, "%s", why ? why : "");
  LeaveCriticalSection(&e->r_cs);
}

static SmpSample *acquire(SmpEngine *e) {
  for (;;) {
    SmpSample *s = e->sample;
    InterlockedExchangePointer((PVOID volatile *)&e->r_using, s);   /* full barrier before the re-check */
    if (e->sample == s) return s;
  }
}

static void idle(SmpEngine *e) { InterlockedIncrement64(&e->ridle_n); SetEvent(e->ridle); }

static void release(SmpEngine *e) { InterlockedExchangePointer((PVOID volatile *)&e->r_using, NULL); }

/* A finished job: its copy opened from the cache and published, or the key marked failed with the reason shown.
   Returns 1 when published. */
static int finish(SmpEngine *e, Job *j, const Cfg *c, char *failed) {
  SmpEntry *en = NULL; char msg[400];
  j->handled = 1;
  if (j->r == 2) return 0;
  if (j->r == 0) en = smp_cache_open_copy(kl_key(c->base, j->slot - SMP_KLMAX, c->speedp, c->eng));
  if (en && publish(e, j->slot, en, c)) return 1;
  if (en) smp_cache_close(en);
  failed[j->slot] = 1;
  if (j->r == 0) snprintf(j->why, sizeof j->why, "a keep-length copy couldn't be read back from the cache folder");
  snprintf(msg, sizeof msg, "Keep length: %s; that key resamples", j->why);
  set_why(e, msg);
  return 0;
}

/* Runs a batch on the pool (or here, without one) and returns when every job has finished, publishing each copy as
   soon as it's done. Meanwhile it stops every job when the configuration changes or the engine quits (and publishes
   nothing more), and the ones the thread chose itself when a note-on asks for a shift that isn't ready and isn't in
   the batch: that shift goes first next pass. Returns how many copies it published. */
static int run_batch(SmpEngine *e, Batch *b, const Cfg *c, char *failed) {
  int i, published = 0, all = 0, cancelled = 0, left;
  b->left = b->n;
  for (i = 0; i < b->n; i++) {
    Job *j = &b->job[i];
    j->w = g_pool && b->done ? CreateThreadpoolWork(job_cb, j, &g_env) : NULL;
    if (j->w) SubmitThreadpoolWork(j->w);
    else { job_run(j); InterlockedDecrement(&b->left); }
  }
  do {
    int yield = 0;
    left = b->left;
    if (!all) all = e->rquit || e->sample != c->s;           /* reads only e->sample's value */
    if (!all) { Cfg now; cfg_read(e, c->s, &now); all = !cfg_same(&now, c); }   /* c->s is held by r_using */
    if (!all) {
      LONG r, w = e->rq_w;
      for (r = e->rq_r; r != w && !yield; r++) {
        int slot = e->rq[r & (SMP_RQ - 1)], in = 0, k;
        for (k = 0; k < b->n; k++) if (b->job[k].slot == slot) in = 1;
        if (!in && renderable(slot) && !failed[slot] && !is_ready(e, slot, c)) yield = 1;
      }
    }
    for (i = 0; i < b->n; i++) {
      Job *j = &b->job[i];
      if (all || (yield && !j->asked)) j->stop = 1;
      if (j->fin && !j->handled) { if (all) j->handled = 1; else published += finish(e, j, c, failed); }
    }
    if (all && !cancelled) {       /* the jobs still queued behind other engines' work never start: they count as done */
      cancelled = 1;
      for (i = 0; i < b->n; i++) {
        Job *j = &b->job[i];
        if (!j->w) continue;
        WaitForThreadpoolWorkCallbacks(j->w, TRUE);           /* the running one (stopping) ends; a queued one goes */
        if (!j->started) { j->r = 2; j->fin = j->handled = 1; InterlockedDecrement(&b->left); }
      }
      left = b->left;
    }
  } while (left && b->done && WaitForSingleObject(b->done, WAIT_MS) != WAIT_FAILED);
  for (i = 0; i < b->n; i++) if (b->job[i].w) { CloseThreadpoolWork(b->job[i].w); b->job[i].w = NULL; }
  return published;
}

typedef struct { SmpEngine *e; SmpSample *s; } LoadArg;
static int load_cancelled(void *arg) { LoadArg *a = arg; return a->e->rquit || a->e->sample != a->s; }

static int listed(const int *order, int n, int slot) {
  int k;
  for (k = 0; k < n; k++) if (order[k] == slot) return 1;
  return 0;
}

static DWORD WINAPI render_thread(LPVOID arg) {
  SmpEngine *e = arg; Cfg last; ULONGLONG changed = 0, checked = 0; int dir_ok = 0, dir_known = 0, busy = 0;
  char failed[SMP_KLN], reach[SMP_KLN], need[SMP_KLN];
  LONG asked[SMP_KLN], seq = 0;                              /* asked: when a note-on asked for the shift, 0 never */
  Batch *b = calloc(1, sizeof *b);
  if (!b) return 0;
  b->e = e;
  b->done = CreateEventW(NULL, FALSE, FALSE, NULL);
  memset(&last, 0, sizeof last); last.on = -1;
  memset(failed, 0, sizeof failed); memset(asked, 0, sizeof asked);
  while (!e->rquit) {
    SmpSample *s; Cfg c; ULONGLONG now; int i, total, ready = 0, order[SMP_KLN], norder = 0, jobs, published = 0;
    uint64_t F, T; LONG64 got = 0;
    WaitForSingleObject(e->rwork, busy ? 0 : 20);
    busy = 0;
    if (e->rquit) break;
    sweep(e);
    s = acquire(e);
    cfg_read(e, s, &c);
    now = GetTickCount64();
    if (!cfg_same(&c, &last)) {
      last = c; changed = now; dir_known = 0; set_why(e, "");
      memset(failed, 0, sizeof failed); memset(asked, 0, sizeof asked); seq = 0;
    }
    reachable(e, reach);
    unpublish_stale(e, &c, reach);
    while (e->rq_r != e->rq_w) {                              /* what note-ons asked for since the last pass */
      int slot = e->rq[e->rq_r & (SMP_RQ - 1)];
      if (e->rq_w - e->rq_r > SMP_RQ) e->rq_r = e->rq_w - SMP_RQ; else e->rq_r++;
      if (c.on && renderable(slot) && reach[slot] && !asked[slot]) asked[slot] = ++seq;
    }
    if (!c.on) {
      InterlockedExchange(&e->r_total, 0); InterlockedExchange(&e->r_ready, 0);
      release(e); idle(e);
      continue;
    }
    /* every reachable shift, nearest the root first, while the copies fit in this engine's share of a quarter of
       the cache limit (always at least SMP_KLFLOOR, an octave each way); and whatever note-ons asked for */
    F = smp_entry_frames(c.src); T = out_frames(F, c.speedp);
    {
      double cap = c.limit / 4.0 / (g_engines > 1 ? g_engines : 1) / ((double)T * 8.0 + 32.0); int n = 0;
      memset(need, 0, sizeof need);
      for (i = 0; i <= SMP_KLRENDER; i++) {
        int up = SMP_KLMAX + i, dn = SMP_KLMAX - i;
        if (reach[up] && (n < SMP_KLFLOOR || n < cap)) { need[up] = 1; n++; }
        if (i && reach[dn] && (n < SMP_KLFLOOR || n < cap)) { need[dn] = 1; n++; }
      }
    }
    for (total = 0, i = 0; i < SMP_KLN; i++) {
      need[i] = reach[i] && (need[i] || asked[i]);
      if (need[i]) { total++; if (is_ready(e, i, &c)) ready++; }
    }
    InterlockedExchange(&e->r_total, total); InterlockedExchange(&e->r_ready, ready);
    if (now - changed < SETTLE_MS) { release(e); continue; }
    if (!dir_known || (!dir_ok && now - checked >= DIR_RECHECK_MS)) {
      char why[600], msg[700];
      dir_ok = smp_cache_ok(why, sizeof why); dir_known = 1; checked = now;
      if (!dir_ok) { snprintf(msg, sizeof msg, "Keep length can't make copies: %s", why); set_why(e, msg); }
      else set_why(e, "");
    }
    if (!dir_ok) { release(e); idle(e); continue; }
    /* the order: shifts note-ons asked for, oldest first; then nearest the root */
    for (;;) {
      int pick = -1;
      for (i = 0; i < SMP_KLN; i++)
        if (need[i] && asked[i] && !failed[i] && !is_ready(e, i, &c) && !listed(order, norder, i) &&
            (pick < 0 || asked[i] < asked[pick])) pick = i;
      if (pick < 0) break;
      order[norder++] = pick;
    }
    for (i = 0; i <= SMP_KLRENDER; i++) {
      int two[2] = {SMP_KLMAX + i, SMP_KLMAX - i}, j;
      for (j = 0; j < (i ? 2 : 1); j++)
        if (need[two[j]] && !failed[two[j]] && !is_ready(e, two[j], &c) && !listed(order, norder, two[j]))
          order[norder++] = two[j];
    }
    if (!norder) { release(e); idle(e); continue; }
    /* copies already in the cache are just opened; the rest make up a batch, as many as the pool runs at once */
    jobs = g_par;
    b->n = 0;
    for (i = 0; i < norder && b->n < jobs; i++) {
      int sl = order[i];
      SmpEntry *en = smp_cache_open_copy(kl_key(c.base, sl - SMP_KLMAX, c.speedp, c.eng));
      if (en) {
        if (publish(e, sl, en, &c)) published++;
        else { smp_cache_close(en); failed[sl] = 1; }
        continue;
      }
      memset(&b->job[b->n], 0, sizeof b->job[b->n]);
      b->job[b->n].b = b; b->job[b->n].slot = sl; b->job[b->n].asked = asked[sl] != 0;
      b->n++;
    }
    if (b->n) {                  /* as many as fit in what's left of the temporary memory all engines share */
      int fit = reserve_mem((double)F * 8.0, job_bytes(c.eng, F, T), b->n, &got);
      if (!fit) { b->n = 0; if (published) smp_cache_trim((uint64_t)c.limit); release(e); continue; }  /* later */
      b->n = fit;
    }
    if (b->n) {
      float *L = NULL, *R = NULL; LoadArg la; int got_src;
      la.e = e; la.s = c.s;
      got_src = load_src(c.src, &L, &R, load_cancelled, &la);
      if (got_src == 0) {
        for (i = 0; i < b->n; i++) failed[b->job[i].slot] = 1;
        set_why(e, "Keep length: out of memory for the copies; those keys resample");
      } else if (got_src == 1) {
        b->L = L; b->R = R; b->F = F; b->rate = smp_entry_rate(c.src); b->eng = c.eng; b->base = c.base;
        b->speedp = c.speedp;
        published += run_batch(e, b, &c, failed);
      }
      free(L); free(R);
      b->L = b->R = NULL;
      unreserve_mem(got);
    }
    if (published) smp_cache_trim((uint64_t)c.limit);         /* opened first, so trimming never deletes them */
    release(e);
    busy = 1;
  }
  release(e);
  if (b->done) CloseHandle(b->done);
  free(b);
  return 0;
}

int smp_render_start(SmpEngine *e) {
  InitOnceExecuteOnce(&pool_once, pool_init, NULL, NULL);
  InterlockedIncrement(&g_engines);
  e->rcounted = 1;
  e->rwork = CreateEventW(NULL, FALSE, FALSE, NULL);
  e->ridle = CreateEventW(NULL, FALSE, FALSE, NULL);
  if (!e->rwork || !e->ridle) return 0;
  e->rthread = CreateThread(NULL, 0, render_thread, e, 0, NULL);
  if (!e->rthread) return 0;
  SetThreadPriority(e->rthread, THREAD_PRIORITY_BELOW_NORMAL);
  return 1;
}

void smp_render_stop(SmpEngine *e) {
  if (e->rthread) {
    InterlockedExchange(&e->rquit, 1);
    SetEvent(e->rwork);
    WaitForSingleObject(e->rthread, INFINITE);
    CloseHandle(e->rthread);
    e->rthread = NULL;
  }
  if (e->rcounted) { InterlockedDecrement(&g_engines); e->rcounted = 0; }
  if (e->rwork) CloseHandle(e->rwork);
  if (e->ridle) CloseHandle(e->ridle);
  e->rwork = e->ridle = NULL;
}

void smp_render_free_copies(SmpEngine *e) {   /* every thread stopped */
  int i;
  for (i = 0; i < SMP_KLN; i++) { smp_kl_free(e->kl[i]); e->kl[i] = NULL; }
  for (i = 0; i < e->nklret; i++) smp_kl_free(e->klret[i].k);
  free(e->klret); e->klret = NULL; e->nklret = e->cklret = 0;
}

SMP_API int smp_engine_render_status(SmpEngine *e, int *ready, int *total, char *why, int cap) {
  int r, t, ok;
  if (!e) return 0;
  r = e->r_ready; t = e->r_total;
  if (ready) *ready = r;
  if (total) *total = t;
  EnterCriticalSection(&e->r_cs);
  ok = r == t && !e->r_why[0];
  if (why && cap > 0) snprintf(why, (size_t)cap, "%s", e->r_why);
  LeaveCriticalSection(&e->r_cs);
  return ok;
}

/* tests, main thread, while the render thread is idle: the copy published for a shift, and how many copies are
   published and how many are retired but not yet freed */
SMP_API const SmpEntry *smp_debug_kl_entry(SmpEngine *e, int semis) {
  SmpKl *k;
  if (!e || semis < -SMP_KLMAX || semis > SMP_KLMAX) return NULL;
  k = e->kl[semis + SMP_KLMAX];
  return k ? k->e : NULL;
}
SMP_API void smp_debug_kl_counts(SmpEngine *e, int *published, int *retiring) {
  int i, n = 0;
  for (i = 0; i < SMP_KLN; i++) if (e->kl[i]) n++;
  *published = n; *retiring = e->nklret;
}

SMP_API int smp_engine_wait_renders(SmpEngine *e, int timeout_ms) {
  LONG64 target; ULONGLONG t0 = GetTickCount64();
  if (!e || !e->rthread) return 1;
  target = e->ridle_n + 2;                                    /* an idle pass that began after this call */
  while (e->ridle_n < target) {
    if (GetTickCount64() - t0 > (ULONGLONG)(timeout_ms < 0 ? 0 : timeout_ms)) return 1;
    SetEvent(e->rwork);
    WaitForSingleObject(e->ridle, 50);
  }
  return 0;
}
