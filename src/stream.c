/* stream.c: the streamer thread. It keeps each live voice's ring buffer ahead of the voice, reading the voice's
   play path (start, end, loop wraps, pingpong) from the cache entry with smp_entry_read. The audio thread wakes it
   with SetEvent after every block; it also polls every 5 ms.

   Handoff with the audio thread, per voice slot (no locks):
   - At note-on the audio thread writes src/rs/re/rls/loop/v0, sets live, bumps gen, then publishes need.
   - The streamer reads gen first, then the fields. A new gen restarts its fill at v0. It writes ring frames, then
     publishes filled = tag(gen) | frames. The audio thread only reads ring frames below filled when the tag
     matches its gen, so data from an older note in the same slot is never played.
   - The audio thread publishes need = tag(gen) | oldest path frame it still needs; the streamer never writes
     more than SMP_RING - 1 frames past it.

   The worker thread (below normal priority) does the slow one-off jobs so the streamer never stalls: reversed
   copies asked for from threads other than the main one, and start heads (the first quarter second of the
   region from the current start point, forward and reversed) so notes starting past the entry's head sound at
   once. Start heads are published by atomic pointer; a replaced one is freed once the audio thread has finished
   a block that began after the swap and no voice uses it. */
#include <stdlib.h>
#include <string.h>
#include "engine_int.h"

static void put(float *ring, int64_t at, const float *src, int64_t n) {
  memcpy(ring + 2 * (at & SMP_RMASK), src, (size_t)n * 8);
}

static void service(SmpEngine *e, SmpVoice *v) {
  LONG g; LONG64 nd; SmpEntry *src; int64_t rs, re, rls, need, total, limit; int loop;
  if (!v->live) return;
  g = v->gen;
  src = v->src; rs = v->rs; re = v->re; rls = v->rls; loop = v->loop;
  if (v->v0 == SMP_NOFILL || !src) return;
  if (g != v->s_gen) { v->s_gen = g; v->s_fill = v->v0; }
  nd = v->need;
  if (SMP_TAGOF(nd) != (g & 0xFFFFFF)) return;          /* the audio thread hasn't published this note yet */
  need = SMP_VALOF(nd);
  if (v->s_fill < need) v->s_fill = need;                  /* the voice ran ahead (underrun): skip forward */
  total = loop ? INT64_MAX : re - rs;
  if (v->s_fill >= total || v->s_fill - need >= SMP_RING / 2) return;   /* done, or at least half full */
  limit = need + SMP_RING - 1;
  if (limit > total) limit = total;
  while (v->s_fill < limit) {
    int64_t at, run, n, wrap; int dir; long got;
    if (!smp_path_run(rs, re, rls, loop, v->s_fill, &at, &dir, &run)) break;
    n = limit - v->s_fill;
    if (n > run) n = run;
    if (n > SMP_CHUNK) n = SMP_CHUNK;
    wrap = SMP_RING - (v->s_fill & SMP_RMASK);
    if (n > wrap) n = wrap;
    if (dir > 0) {
      got = smp_entry_read(src, (uint64_t)at, e->tmp, (long)n);
      if (got < 0) got = 0;
      if (got < n) memset(e->tmp + 2 * got, 0, (size_t)(n - got) * 8);   /* a failed read plays as silence */
      put(v->ring, v->s_fill, e->tmp, n);
    } else {                                              /* frames at, at-1, ..., at-n+1 */
      int64_t i; float *dst = v->ring + 2 * (v->s_fill & SMP_RMASK);
      got = smp_entry_read(src, (uint64_t)(at - n + 1), e->tmp, (long)n);
      if (got < 0) got = 0;
      if (got < n) memset(e->tmp + 2 * got, 0, (size_t)(n - got) * 8);
      for (i = 0; i < n; i++) { dst[2 * i] = e->tmp[2 * (n - 1 - i)]; dst[2 * i + 1] = e->tmp[2 * (n - 1 - i) + 1]; }
    }
    v->s_fill += n;
    if (v->gen != g) return;                              /* retriggered meanwhile: the next pass starts over */
    v->filled = SMP_TAG(g) | v->s_fill;                    /* volatile store: the ring data is visible first */
  }
}

static DWORD WINAPI streamer(LPVOID arg) {
  SmpEngine *e = arg;
  while (!e->quit) {
    int i;
    WaitForSingleObject(e->wake, 5);
    if (e->quit) break;
    if (!e->spause) for (i = 0; i < SMP_MAXV; i++) service(e, &e->v[i]);
    InterlockedIncrement64(&e->spass);
    SetEvent(e->idle);
  }
  return 0;
}

int smp_stream_start(SmpEngine *e) {
  e->tmp = malloc(sizeof(float) * 2 * SMP_CHUNK);
  e->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
  e->idle = CreateEventW(NULL, FALSE, FALSE, NULL);
  if (!e->tmp || !e->wake || !e->idle) return 0;
  e->thread = CreateThread(NULL, 0, streamer, e, 0, NULL);
  if (!e->thread) return 0;
  SetThreadPriority(e->thread, THREAD_PRIORITY_ABOVE_NORMAL);
  return 1;
}

void smp_stream_stop(SmpEngine *e) {
  if (e->thread) {
    InterlockedExchange(&e->quit, 1);
    SetEvent(e->wake);
    WaitForSingleObject(e->thread, INFINITE);
    CloseHandle(e->thread);
    e->thread = NULL;
  }
  if (e->wake) CloseHandle(e->wake);
  if (e->idle) CloseHandle(e->idle);
  e->wake = e->idle = NULL;
  free(e->tmp); e->tmp = NULL;
}

void smp_stream_wait_idle(SmpEngine *e) {
  LONG64 target = e->spass + 2;                           /* a whole pass that began after this call */
  while (e->spass < target && e->thread) {
    SetEvent(e->wake);
    WaitForSingleObject(e->idle, 50);
  }
}

/* ---- the worker ---- */
static int sh_in_use(const SmpEngine *e, const SmpStartHead *h) {
  int i;
  for (i = 0; i < SMP_MAXV; i++) if (e->v[i].live && (e->v[i].sh == h || e->v[i].lt == h)) return 1;
  return 0;
}

static void sh_free(SmpStartHead *h) { if (h) { free(h->lr); free(h); } }

static void sh_sweep(SmpEngine *e) {
  int i = 0;
  while (i < e->nshret) {
    if (e->epoch >= e->shret[i].a0 + 2 && !sh_in_use(e, e->shret[i].h)) {
      sh_free(e->shret[i].h);
      e->shret[i] = e->shret[--e->nshret];
    } else i++;
  }
}

/* Room on the retire list for one more replaced start head. It drains only as the audio thread finishes blocks, so
   while nothing is processing (a deactivated plugin) it stays full: the caller then keeps the current head and tries
   again on a later pass, never waiting here. */
static int sh_room(SmpEngine *e) {
  if (e->nshret >= SMP_MAXSHRET) sh_sweep(e);
  return e->nshret < SMP_MAXSHRET;
}

static void sh_publish(SmpEngine *e, int d, SmpStartHead *h) {   /* the caller made room (sh_room) */
  SmpStartHead *old = InterlockedExchangePointer((PVOID volatile *)&e->sh[d], h);
  if (!old) return;
  e->shret[e->nshret].h = old; e->shret[e->nshret].a0 = e->epoch; e->nshret++;
}

static void sh_build(SmpEngine *e, int d) {
  SmpSample *s = e->sample; SmpEntry *src = NULL; SmpStartHead *cur = e->sh[d], *h;
  int64_t F, rs = 0, re, rls, n; uint64_t hf; long got;
  if (s) src = d ? *(SmpEntry *volatile *)&s->rev : s->fwd;
  if (src && !smp_entry_whole(src) && (F = (int64_t)smp_entry_frames(src)) > 0)
    smp_region(smp_eng_param(e, SMP_P_START), smp_eng_param(e, SMP_P_END), smp_eng_param(e, SMP_P_LOOP_START), F, d, &rs, &re, &rls);
  else src = NULL;
  if (!src || rs == 0) { if (cur && sh_room(e)) sh_publish(e, d, NULL); return; }   /* the entry's own head serves */
  if (cur && cur->s == s && cur->src == src && cur->at == rs) return;
  if (cur && !sh_room(e)) return;                         /* retire list full: build it on a later pass */
  smp_entry_head(src, &hf);
  n = (int64_t)hf;
  if (n > F - rs) n = F - rs;
  h = calloc(1, sizeof *h);
  if (!h || !(h->lr = malloc((size_t)n * 8))) { free(h); return; }
  got = smp_entry_read(src, (uint64_t)rs, h->lr, (long)n);
  if (got < 0) got = 0;
  if (got < n) memset(h->lr + 2 * got, 0, (size_t)(n - got) * 8);
  h->s = s; h->src = src; h->at = rs; h->n = n;
  sh_publish(e, d, h);                                    /* volatile/interlocked: the frames are visible first */
}

/* Start heads for keep-length copies, built when a note-on asks (the copy's want_sh) and the start is past the
   copy's own head. The copy itself can't be freed under this: the render thread waits two worker passes first.
   A replaced start head goes on the retire list; if that is full the old one stays and the ask is kept for later. */
static void kl_sh_build(SmpEngine *e) {
  int i;
  for (i = 0; i < SMP_KLN; i++) {
    SmpKl *k = e->kl[i]; SmpStartHead *cur, *h, *old; int64_t F, rs, re, rls, n; uint64_t hf; long got;
    if (!k || !k->want_sh) continue;
    InterlockedExchange(&k->want_sh, 0);
    F = (int64_t)smp_entry_frames(k->e);
    if (F <= 0 || smp_entry_whole(k->e)) continue;
    smp_region(smp_eng_param(e, SMP_P_START), smp_eng_param(e, SMP_P_END), smp_eng_param(e, SMP_P_LOOP_START), F, k->rev, &rs, &re, &rls);
    smp_entry_head(k->e, &hf);
    if (rs < (int64_t)hf) continue;
    cur = k->sh;
    if (cur && cur->at == rs) continue;
    if (cur && !sh_room(e)) { InterlockedExchange(&k->want_sh, 1); continue; }
    n = (int64_t)hf;
    if (n > F - rs) n = F - rs;
    h = calloc(1, sizeof *h);
    if (!h || !(h->lr = malloc((size_t)n * 8))) { free(h); continue; }
    got = smp_entry_read(k->e, (uint64_t)rs, h->lr, (long)n);
    if (got < 0) got = 0;
    if (got < n) memset(h->lr + 2 * got, 0, (size_t)(n - got) * 8);
    h->s = NULL; h->src = k->e; h->at = rs; h->n = n;
    old = InterlockedExchangePointer((PVOID volatile *)&k->sh, h);
    if (old) { e->shret[e->nshret].h = old; e->shret[e->nshret].a0 = e->epoch; e->nshret++; }
  }
}

/* A loop tail: the loop's last n frames crossfaded (equal power) into the n frames before the loop start, n at most
   SMP_LOOP_XFADE, the part before the loop and half the loop. With detect (declick), first looks whether the loop's
   end meets its start: the frame after the end, carried on from the last two, against the loop's first frame. If it
   does, n 0 (nothing to do); if it doesn't and the crossfade would be shorter than a declick fade, n 0 and dip 1.
   NULL when it can't be read. */
static SmpStartHead *loop_tail(SmpEntry *src, int64_t rs, int64_t re, int64_t rls, int detect) {
  SmpStartHead *h = calloc(1, sizeof *h); int64_t n, j; float *b;
  int rate = smp_entry_rate(src);
  if (!h) return NULL;
  h->src = src; h->at = rls; h->end = re;
  if (detect) {
    float a[4], c[4]; int ch, bad = 0;
    if (re - rls < 4 || smp_entry_read(src, (uint64_t)(re - 2), a, 2) != 2 || smp_entry_read(src, (uint64_t)rls, c, 2) != 2) {
      if (re - rls >= 4) { free(h); return NULL; }
      return h;                                           /* a loop this short: left as it is */
    }
    for (ch = 0; ch < 2; ch++) {
      double x0 = a[ch], x1 = a[2 + ch], y0 = c[ch], y1 = c[2 + ch];
      double err = fabs(y0 - (2 * x1 - x0)), step = fabs(x1 - x0) > fabs(y1 - y0) ? fabs(x1 - x0) : fabs(y1 - y0);
      if (err > SMP_DECLICK_FLOOR && err > 1.5 * step) bad = 1;
    }
    if (!bad) return h;
  }
  n = (int64_t)(SMP_LOOP_XFADE * rate);
  if (n > rls - rs) n = rls - rs;
  if (n > (re - rls) / 2) n = (re - rls) / 2;
  if (detect && n < (int64_t)(SMP_DECLICK * rate)) { h->dip = 1; return h; }
  if (n < 8) return h;                                    /* too short to crossfade: as it is */
  if (!(h->lr = malloc((size_t)n * 8)) || !(b = malloc((size_t)n * 8))) { free(h->lr); free(h); return NULL; }
  if (smp_entry_read(src, (uint64_t)(re - n), h->lr, (long)n) != n || smp_entry_read(src, (uint64_t)(rls - n), b, (long)n) != n) {
    free(b); free(h->lr); free(h); return NULL;
  }
  for (j = 0; j < n; j++) {
    double t = ((double)j + 0.5) / (double)n * 1.5707963267948966, go = cos(t), gi = sin(t);
    h->lr[2 * j] = (float)(h->lr[2 * j] * go + b[2 * j] * gi);
    h->lr[2 * j + 1] = (float)(h->lr[2 * j + 1] * go + b[2 * j + 1] * gi);
  }
  free(b);
  h->n = n;
  return h;
}

static void lt_publish(SmpEngine *e, int d, SmpStartHead *h) {   /* the caller made room (sh_room) */
  SmpStartHead *old = InterlockedExchangePointer((PVOID volatile *)&e->lt[d], h);
  if (!old) return;
  e->shret[e->nshret].h = old; e->shret[e->nshret].a0 = e->epoch; e->nshret++;
}

/* Declick's loop fix for the source, built when a note-on asks (want_lt). One for another sample is dropped. */
static void lt_build(SmpEngine *e, int d) {
  SmpSample *s = e->sample; SmpEntry *src; SmpStartHead *cur = e->lt[d], *h; int64_t F, rs, re, rls;
  if (cur && cur->s != s && sh_room(e)) { lt_publish(e, d, NULL); cur = NULL; }
  if (!e->want_lt[d]) return;
  InterlockedExchange(&e->want_lt[d], 0);
  if (!s) return;
  src = d ? *(SmpEntry *volatile *)&s->rev : s->fwd;
  if (!src || (F = (int64_t)smp_entry_frames(src)) <= 0) return;
  smp_region(smp_eng_param(e, SMP_P_START), smp_eng_param(e, SMP_P_END), smp_eng_param(e, SMP_P_LOOP_START), F, d, &rs, &re, &rls);
  if (cur && cur->s == s && cur->src == src && cur->at == rls && cur->end == re) return;
  if (cur && !sh_room(e)) { InterlockedExchange(&e->want_lt[d], 1); return; }
  if (!(h = loop_tail(src, rs, re, rls, 1))) return;
  h->s = s;
  lt_publish(e, d, h);
}

/* Loop tails for keep-length copies, built when a note-on asks (want_lt) for the loop as it is now: the loop's last
   n frames crossfade (equal power) into the n frames before the loop start, so the end flows into the start. n is
   SMP_LOOP_XFADE at most, and no more than the part before the loop or half the loop. Replaced ones are retired as
   start heads are. */
static void kl_lt_build(SmpEngine *e) {
  int i;
  for (i = 0; i < SMP_KLN; i++) {
    SmpKl *k = e->kl[i]; SmpStartHead *cur, *h, *old; int64_t F, rs, re, rls;
    if (!k || !k->want_lt) continue;
    InterlockedExchange(&k->want_lt, 0);
    F = (int64_t)smp_entry_frames(k->e);
    if (F <= 0) continue;
    smp_region(smp_eng_param(e, SMP_P_START), smp_eng_param(e, SMP_P_END), smp_eng_param(e, SMP_P_LOOP_START), F, k->rev, &rs, &re, &rls);
    cur = k->lt;
    if (cur && cur->at == rls && cur->end == re) continue;
    if (cur && !sh_room(e)) { InterlockedExchange(&k->want_lt, 1); continue; }
    if (!(h = loop_tail(k->e, rs, re, rls, 0))) continue;   /* can't read it: the note resamples, as without a copy */
    old = InterlockedExchangePointer((PVOID volatile *)&k->lt, h);
    if (old) { e->shret[e->nshret].h = old; e->shret[e->nshret].a0 = e->epoch; e->nshret++; }
  }
}

static DWORD WINAPI worker(LPVOID arg) {
  SmpEngine *e = arg;
  while (!e->quit) {
    WaitForSingleObject(e->work, 20);
    if (e->quit) break;
    if (InterlockedExchange(&e->want_rev, 0)) {           /* reverse switched on from a non-main thread */
      SmpSample *s = e->sample;
      if (s && smp_eng_param(e, SMP_P_REVERSE) >= 0.5) {
        EnterCriticalSection(&e->rev_cs);
        smp_sample_reversed(s);
        LeaveCriticalSection(&e->rev_cs);
      }
    }
    smp_rings_commit(e);                                   /* Voices raised from another thread */
    sh_build(e, 0);
    sh_build(e, 1);
    kl_sh_build(e);
    kl_lt_build(e);
    lt_build(e, 0);
    lt_build(e, 1);
    sh_sweep(e);
    InterlockedIncrement64(&e->wpass);
    SetEvent(e->widle);
  }
  return 0;
}

int smp_worker_start(SmpEngine *e) {
  e->work = CreateEventW(NULL, FALSE, FALSE, NULL);
  e->widle = CreateEventW(NULL, FALSE, FALSE, NULL);
  if (!e->work || !e->widle) return 0;
  e->worker = CreateThread(NULL, 0, worker, e, 0, NULL);
  if (!e->worker) return 0;
  SetThreadPriority(e->worker, THREAD_PRIORITY_BELOW_NORMAL);
  return 1;
}

void smp_worker_stop(SmpEngine *e) {   /* also frees every start head: call only when no audio thread runs */
  int i;
  if (e->worker) {
    InterlockedExchange(&e->quit, 1);
    SetEvent(e->work);
    WaitForSingleObject(e->worker, INFINITE);
    CloseHandle(e->worker);
    e->worker = NULL;
  }
  if (e->work) CloseHandle(e->work);
  if (e->widle) CloseHandle(e->widle);
  e->work = e->widle = NULL;
  for (i = 0; i < e->nshret; i++) sh_free(e->shret[i].h);
  e->nshret = 0;
  sh_free(e->sh[0]); sh_free(e->sh[1]);
  e->sh[0] = e->sh[1] = NULL;
  sh_free(e->lt[0]); sh_free(e->lt[1]);
  e->lt[0] = e->lt[1] = NULL;
}

void smp_worker_wait_idle(SmpEngine *e) {
  LONG64 target = e->wpass + 2;
  while (e->wpass < target && e->worker) {
    SetEvent(e->work);
    WaitForSingleObject(e->widle, 50);
  }
}

/* tests: 0 once a whole worker pass has run, 1 if none finished within timeout_ms */
SMP_API int smp_test_worker_wait(SmpEngine *e, int timeout_ms) {
  LONG64 target; ULONGLONG t0 = GetTickCount64();
  if (!e || !e->worker) return 1;
  target = e->wpass + 2;
  while (e->wpass < target) {
    if (GetTickCount64() - t0 > (ULONGLONG)(timeout_ms < 0 ? 0 : timeout_ms)) return 1;
    SetEvent(e->work);
    WaitForSingleObject(e->widle, 50);
  }
  return 0;
}
