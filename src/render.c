/* render.c: keep length. Every pitched note lasts as long as the recording: for each integer shift the key range
   reaches, Rubber Band R3 (offline, finer engine, channels together) renders a copy of the sample at that pitch
   (and at 1/speed of its length), written to the cache under kl|<src or rev key>|<semis>|<speed %.2f>|r3.

   The render thread (below normal priority, separate from the worker so a long render never delays reversed
   copies or start heads):
   - Each pass it reads the current sample, keep length, reverse and speed, and works out the shifts note_lo to
     note_hi reach (with the same formula note-on uses). It wants the reachable shifts within SMP_KLPRE (12)
     semitones of the root, plus the shifts note-ons have asked for (a small ring the audio thread writes when a
     note finds no copy); other keys are never rendered until played. When the configuration changes it waits
     100 ms for it to settle (scrolling through files or dragging speed renders nothing) and forgets what note-ons
     asked for, then works on asked-for shifts first (in the order asked), then nearest the root.
   - A copy already in the cache is just opened; otherwise it's rendered, written, and opened. Renders check
     between chunks whether the configuration changed (or the engine is quitting) and give up if so. A render the
     thread chose itself (not asked for) also gives up when a note-on asks for another shift that isn't ready: that
     shift is rendered next and the abandoned one comes back in its turn.
   - Copies are opened with smp_cache_open_copy: only the 0.25 s head is held in memory (whole only when shorter);
     the rest is streamed like any long sample.
   - Ready copies are published into e->kl[shift + 127] by atomic pointer. A copy is identified by content (the
     key it was rendered from and the speed), so the audio thread just ignores one that doesn't match its note.
   - Stale copies (another sample, speed or direction, keep length off, or a shift the key range no longer
     reaches) are taken out of the table and retired: one is freed only after the audio thread has finished
     two blocks since (no new note can pick it), no live voice reads it, and then the streamer and the worker have
     each finished two passes (neither can still be reading it or its start head). The retire list grows as needed
     and the render thread never waits on it; if it can't grow, the copy is left in place (stale copies are
     harmless) or a new copy isn't published.
   - The sample it reads is protected from smp_engine_collect by e->r_using: it is set, then e->sample re-checked,
     so collect (which runs after the swap) sees it whenever the thread could still be reading the sample.

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

#define RB_CHUNK 4096
#define SETTLE_MS 100
#define DIR_RECHECK_MS 1000
#define SMP_KLMEM (512.0 * 1024 * 1024)   /* bytes: copies needing more temporary memory are refused */

static volatile LONG g_renders;
SMP_API long smp_debug_renders(void) { return g_renders; }

LONG smp_speedp(double pct) { return (LONG)floor(pct * 100.0 + 0.5); }

static uint64_t kl_key(uint64_t base, int semis, LONG speedp) {
  char d[96];
  snprintf(d, sizeof d, "kl|%016llx|%d|%.2f|r3", (unsigned long long)base, semis, speedp / 100.0);
  return smp_key(d);
}

typedef int (*CancelFn)(void *);

/* rbsafe.cpp: the Rubber Band calls, inside try/catch (no C++ exception may reach C or the host) */
int smp_rb_render(const float *L, const float *R, unsigned F, int rate, double time_ratio, double pitch_scale,
                  float *out, unsigned long long T, CancelFn cancel, void *arg, float *ob, char *why, int cap);
void smp_rb_set_debug_flags(long f);

/* tests: 1 = Rubber Band throws inside the guard, 2 = no 4-octave limit (to reach Rubber Band's own failures) */
static volatile LONG g_dflags;
SMP_API void smp_debug_render_flags(long f) { g_dflags = f; smp_rb_set_debug_flags(f & 1); }

/* 0 ok, 1 failed, 2 cancelled, 3 the cache write failed, 4 too big (temporary memory past SMP_KLMEM);
   why: a plain reason for 1, 3 and 4 */
static int render_entry(SmpEntry *src, uint64_t base, int semis, LONG speedp, CancelFn cancel, void *arg,
                        char *why, int cap) {
  uint64_t F = smp_entry_frames(src), T, i; int rate = smp_entry_rate(src), r = 1;
  float *L = NULL, *R = NULL, *lr = NULL, *out = NULL, *ob = NULL;
  snprintf(why, (size_t)cap, "a keep-length copy couldn't be made");
  if (!F || speedp <= 0 || rate <= 0) return 1;
  if (!(g_dflags & 2) && (semis < -SMP_KLRENDER || semis > SMP_KLRENDER)) {
    snprintf(why, (size_t)cap, "keep length only shifts up to 4 octaves");
    return 1;
  }
  T = speedp == 10000 ? F : (uint64_t)floor((double)F * 10000.0 / (double)speedp + 0.5);
  if (!T) T = 1;
  /* the source (both sides), the copy (interleaved), and about as much again inside Rubber Band */
  if (F > 0x7FFFFFFF || (double)(F + T) * 8.0 * 2.0 > SMP_KLMEM) {
    snprintf(why, (size_t)cap, "the sample is too long for keep-length copies at this speed");
    return 4;
  }
  L = malloc(sizeof(float) * (size_t)F); R = malloc(sizeof(float) * (size_t)F);
  lr = malloc(sizeof(float) * 2 * RB_CHUNK); ob = malloc(sizeof(float) * 2 * RB_CHUNK);
  out = calloc((size_t)T * 2, sizeof(float));
  if (!L || !R || !lr || !ob || !out) { snprintf(why, (size_t)cap, "out of memory for a keep-length copy"); goto done; }
  for (i = 0; i < F; i += RB_CHUNK) {                      /* the source, deinterleaved */
    long n = (long)(F - i < RB_CHUNK ? F - i : RB_CHUNK), k, g = smp_entry_read(src, i, lr, n);
    if (g < 0) g = 0;
    for (k = 0; k < n; k++) { L[i + k] = k < g ? lr[2 * k] : 0; R[i + k] = k < g ? lr[2 * k + 1] : 0; }
    if (cancel && cancel(arg)) { r = 2; goto done; }
  }
  r = smp_rb_render(L, R, (unsigned)F, rate, 10000.0 / (double)speedp, pow(2.0, semis / 12.0), out, T, cancel, arg,
                    ob, why, cap);
  if (r) goto done;
  InterlockedIncrement(&g_renders);
  if (smp_cache_write(kl_key(base, semis, speedp), out, T, rate)) {
    snprintf(why, (size_t)cap, "keep-length copies can't be written to the cache folder");
    r = 3;
  }
done:
  free(L); free(R); free(lr); free(ob); free(out);
  return r;
}

SMP_API int smp_render_shift(SmpSample *s, int reversed, int semis, double speed) {
  SmpEntry *src; LONG sp; char why[300];
  if (!s || !s->fwd || speed <= 0) return 1;
  src = reversed ? smp_sample_reversed(s) : s->fwd;
  if (!src) return 1;
  sp = smp_speedp(speed * 100.0);
  return render_entry(src, reversed ? s->rev_key : s->src_key, semis, sp, NULL, NULL, why, sizeof why);
}

/* ---- the render thread ---- */
typedef struct { SmpSample *s; SmpEntry *src; uint64_t base; LONG speedp; int on, rev; } Cfg;
typedef struct { SmpEngine *e; Cfg c; int slot, chosen; const char *failed; } CancelArg;

static void cfg_read(const SmpEngine *e, SmpSample *s, Cfg *c) {
  SmpEntry *rev = NULL;
  memset(c, 0, sizeof *c);
  c->s = s;
  if (!s || !s->fwd) return;
  c->on = smp_eng_param(e, SMP_P_KEEP_LENGTH) >= 0.5;
  if (smp_eng_param(e, SMP_P_REVERSE) >= 0.5) rev = *(SmpEntry *volatile *)&s->rev;
  c->rev = rev != NULL;
  c->src = rev ? rev : s->fwd;
  c->base = rev ? s->rev_key : s->src_key;
  c->speedp = smp_speedp(smp_eng_param(e, SMP_P_SPEED));
}

static int cfg_same(const Cfg *a, const Cfg *b) {
  return a->s == b->s && a->on == b->on && a->base == b->base && a->speedp == b->speedp && a->src == b->src;
}

static int is_ready(const SmpEngine *e, int slot, const Cfg *c);

static int renderable(int slot) { return slot >= SMP_KLMAX - SMP_KLRENDER && slot <= SMP_KLMAX + SMP_KLRENDER; }

static int cancelled(void *arg) {   /* reads only e->sample's value, never what it points to */
  CancelArg *a = arg; Cfg now; SmpEngine *e = a->e;
  if (e->rquit) return 1;
  if (e->sample != a->c.s) return 1;
  cfg_read(e, a->c.s, &now);                                 /* c.s is held by r_using: safe to read */
  if (!cfg_same(&now, &a->c)) return 1;
  if (a->chosen) {                                           /* a note-on waits for another shift: yield to it */
    LONG r, w = e->rq_w;
    for (r = e->rq_r; r != w; r++) {
      int slot = e->rq[r & (SMP_RQ - 1)];
      if (slot != a->slot && renderable(slot) && !a->failed[slot] && !is_ready(e, slot, &a->c)) return 1;
    }
  }
  return 0;
}

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
    if (!k || (c->on && reach[i] && k->base == c->base && k->speedp == c->speedp)) continue;
    if (!reserve(e)) return;                                  /* left in place: never used, freed later */
    InterlockedExchangePointer((PVOID volatile *)&e->kl[i], NULL);
    retire(e, k);
  }
}

static int publish(SmpEngine *e, int slot, SmpEntry *en, const Cfg *c) {
  SmpKl *k, *old;
  if (!reserve(e) || !(k = calloc(1, sizeof *k))) return 0;
  k->e = en; k->base = c->base; k->speedp = c->speedp; k->semis = slot - SMP_KLMAX; k->rev = c->rev;
  old = InterlockedExchangePointer((PVOID volatile *)&e->kl[slot], k);   /* k's fields are visible first */
  if (old) retire(e, old);
  return 1;
}

static int is_ready(const SmpEngine *e, int slot, const Cfg *c) {
  const SmpKl *k = e->kl[slot];
  if (slot == SMP_KLMAX && c->speedp == 10000) return 1;       /* shift 0 at 100 %: the source itself */
  return k && k->base == c->base && k->speedp == c->speedp;
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

static DWORD WINAPI render_thread(LPVOID arg) {
  SmpEngine *e = arg; Cfg last; ULONGLONG changed = 0, checked = 0; int dir_ok = 0, dir_known = 0, busy = 0;
  char failed[SMP_KLN], reach[SMP_KLN], need[SMP_KLN];
  LONG asked[SMP_KLN], seq = 0;                              /* asked: when a note-on asked for the shift, 0 never */
  memset(&last, 0, sizeof last); last.on = -1;
  memset(failed, 0, sizeof failed); memset(asked, 0, sizeof asked);
  while (!e->rquit) {
    SmpSample *s; Cfg c; ULONGLONG now; int i, total, ready = 0, pick = -1, chosen;
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
      InterlockedExchangePointer((PVOID volatile *)&e->r_using, NULL);
      idle(e);
      continue;
    }
    for (total = 0, i = 0; i < SMP_KLN; i++) {               /* near the root, and what note-ons asked for */
      need[i] = reach[i] && (asked[i] || (i >= SMP_KLMAX - SMP_KLPRE && i <= SMP_KLMAX + SMP_KLPRE));
      if (need[i]) { total++; if (is_ready(e, i, &c)) ready++; }
    }
    InterlockedExchange(&e->r_total, total); InterlockedExchange(&e->r_ready, ready);
    if (now - changed < SETTLE_MS) { InterlockedExchangePointer((PVOID volatile *)&e->r_using, NULL); continue; }
    if (!dir_known || (!dir_ok && now - checked >= DIR_RECHECK_MS)) {
      char why[600], msg[700];
      dir_ok = smp_cache_ok(why, sizeof why); dir_known = 1; checked = now;
      if (!dir_ok) { snprintf(msg, sizeof msg, "Keep length can't make copies: %s", why); set_why(e, msg); }
      else set_why(e, "");
    }
    if (!dir_ok) { InterlockedExchangePointer((PVOID volatile *)&e->r_using, NULL); idle(e); continue; }
    for (i = 0; i < SMP_KLN; i++)                             /* shifts note-ons asked for go first, oldest first */
      if (need[i] && asked[i] && !failed[i] && !is_ready(e, i, &c) && (pick < 0 || asked[i] < asked[pick])) pick = i;
    chosen = pick < 0;
    for (i = 0; pick < 0 && i <= SMP_KLRENDER; i++) {            /* then nearest the root */
      int a = SMP_KLMAX + i, b = SMP_KLMAX - i;
      if (need[a] && !failed[a] && !is_ready(e, a, &c)) pick = a;
      else if (i && need[b] && !failed[b] && !is_ready(e, b, &c)) pick = b;
    }
    if (pick < 0) { InterlockedExchangePointer((PVOID volatile *)&e->r_using, NULL); idle(e); continue; }
    {
      int n = pick - SMP_KLMAX; uint64_t key = kl_key(c.base, n, c.speedp);
      SmpEntry *en = smp_cache_open_copy(key);
      if (!en) {
        CancelArg ca; int r; char why[300];
        ca.e = e; ca.c = c; ca.slot = pick; ca.chosen = chosen; ca.failed = failed;
        r = render_entry(c.src, c.base, n, c.speedp, cancelled, &ca, why, sizeof why);
        if (r == 2) { InterlockedExchangePointer((PVOID volatile *)&e->r_using, NULL); busy = 1; continue; }
        if (r == 0) en = smp_cache_open_copy(key);
        if (!en) {
          failed[pick] = 1;
          char msg[400];
          if (r == 0) snprintf(why, sizeof why, "a keep-length copy couldn't be read back from the cache folder");
          snprintf(msg, sizeof msg, "Keep length: %s; that key resamples", why);
          set_why(e, msg);
        }
      }
      if (en && !publish(e, pick, en, &c)) { smp_cache_close(en); en = NULL; failed[pick] = 1; }
      if (en) {                                               /* open now, so trimming never deletes it */
        SmpSettings st;
        smp_settings_get(&st);
        smp_cache_trim((uint64_t)(st.cache_limit_gb * 1024.0 * 1024.0 * 1024.0));
      }
    }
    InterlockedExchangePointer((PVOID volatile *)&e->r_using, NULL);
    busy = 1;
  }
  InterlockedExchangePointer((PVOID volatile *)&e->r_using, NULL);
  return 0;
}

int smp_render_start(SmpEngine *e) {
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
