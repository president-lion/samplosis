/* stream.c: the streamer thread. It keeps each live voice's ring buffer ahead of the voice, reading the voice's
   play path (start, end, loop wraps, pingpong) from the cache entry with smp_entry_read. The audio thread wakes it
   with SetEvent after every block; it also polls every 5 ms.

   Handoff with the audio thread, per voice slot (no locks):
   - At note-on the audio thread writes src/rs/re/rls/loop/v0, sets live, bumps gen, then publishes need.
   - The streamer reads gen first, then the fields. A new gen restarts its fill at v0. It writes ring frames, then
     publishes filled = tag(gen) | frames. The audio thread only reads ring frames below filled when the tag
     matches its gen, so data from an older note in the same slot is never played.
   - The audio thread publishes need = tag(gen) | oldest path frame it still needs; the streamer never writes
     more than SMP_RING - 1 frames past it. */
#include <stdlib.h>
#include <string.h>
#include "engine_int.h"

/* Reversed copies asked for from a thread other than the main thread are made here (heavy I/O, once). Until it
   is ready, notes play forward. */
static void prepare_reverse(SmpEngine *e) {
  SmpSample *s;
  if (!InterlockedExchange(&e->want_rev, 0)) return;
  s = e->sample;
  if (!s || smp_eng_param(e, SMP_P_REVERSE) < 0.5) return;
  EnterCriticalSection(&e->rev_cs);
  smp_sample_reversed(s);
  LeaveCriticalSection(&e->rev_cs);
}

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
    prepare_reverse(e);
    for (i = 0; i < SMP_MAXV; i++) service(e, &e->v[i]);
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
