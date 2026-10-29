/* sfstream.c: the SoundFont streamer thread, one per player (and one for each smp_sf_test_note). It keeps each
   streaming voice's ring buffer ahead of the voice, reading the voice's path (straight on, round its loop, and on to
   the end when a loop-until-release lets go) with smp_sf_stream_read. Frames inside the sample's head are never
   read: the voice takes those from the head in memory. The audio thread wakes it after every block in which a
   voice streams; while any voice streams it also polls every 5 ms.

   Handoffs with the audio thread, per voice slot (no locks):
   - At a streaming voice's start the audio thread gives the slot a new generation g: it sets rel to -1, publishes
     need = tag(g) | the first path frame, then live = g, takes a second hold of the sample (for the streamer), and
     queues a start message with the voice's layout and that hold. When the queue is full the voice keeps the
     hold count it took back, sets live to 0 and plays without a ring (its head, then silence).
   - The streamer drains the queue at the start of each pass and keeps one record per slot. A record is alive while
     live == g. When it isn't (the voice ended, or a newer voice has the slot), the streamer releases the record's
     hold. The streamer is the only reader through that hold and reads one thing at a time, so its release always
     comes after its last read, and the sample can't be freed under a read.
   - The streamer writes ring frames, then publishes filled = tag(g, released) | path frames written. The audio
     thread reads ring frames only below filled and only while the tag's generation is its own, so data from an
     older note in the same slot is never played. It never writes more than SF_RING - 1 frames past need.
   - A loop-until-release (sample mode 3) changes the path when the key goes up: the audio thread stores the laps
     done in rel. Ring frames from that lap's loop end on were read for the loop, so until the streamer has
     rewound to there (filled then carries the released bit) the audio thread reads only below that point.
   - Rings are reserved for every slot and committed by the streamer before it first writes one; a ring no voice
     has used for 10 s is decommitted. The audio thread never touches a ring whose filled tag isn't its own, and the
     streamer publishes a tag only after committing, so neither step needs a lock.
   A player waits for this thread (sfs_sync) before closing a font, so every hold on it is released first. */
#include <stdlib.h>
#include <string.h>
#include "sf2.h"
#include "sfstream.h"

#define QN 256                           /* queued starts (power of two) */
#define CHUNK 8192                       /* frames per read */
#define DECOMMIT_MS 10000                /* a ring unused this long is given back */
#define POLL_MS 20                       /* a backstop: the audio thread wakes it after every block in which a voice streams */

typedef struct {
  SfMsg m;
  int on;                                /* the record holds m's sample */
  int ai;                                /* its place in the active list while on */
  LONG rel;                              /* the laps the path stopped looping at, or -1 */
  int64_t fill;                          /* path frames written so far */
  int committed;                         /* the slot's ring is committed */
  ULONGLONG idle_since;                  /* when the ring was last used (for the decommit) */
} Rec;

struct SfStream {
  int nslots;
  SfSlot *slot;
  float *rings;                          /* nslots rings, reserved */
  Rec *rec;                              /* the streamer's own */
  int *act, nact;                        /* the slots whose records are on, so a pass touches only those */
  ULONGLONG last_sweep;                  /* when idle rings were last looked at */
  SfMsg q[QN];
  volatile LONG qw, qr;                  /* written by the audio thread and by the streamer */
  HANDLE thread, wake, idle;
  volatile LONG quit, topup, paused;
  volatile LONG64 pass;
};

SfSlot *sfs_slot(SfStream *st, int i) { return &st->slot[i]; }
float *sfs_ring(SfStream *st, int i) { return st->rings + (size_t)i * SF_RING; }
void sfs_wake(SfStream *st) { SetEvent(st->wake); }
void sfs_pause(SfStream *st, int on) { InterlockedExchange(&st->paused, on ? 1 : 0); }

int sfs_push(SfStream *st, const SfMsg *m) {
  LONG w = st->qw;
  if (w - st->qr >= QN) return 0;
  st->q[w & (QN - 1)] = *m;
  InterlockedExchange(&st->qw, w + 1);    /* a full barrier: the message is visible first */
  return 1;
}

static void drop(SfStream *st, Rec *r) {
  int last;
  if (!r->on) return;
  smp_sf_pcm_release(r->m.f, r->m.sample);
  r->on = 0;
  r->idle_since = GetTickCount64();
  last = st->act[--st->nact];                      /* out of the active list: the last one takes its place */
  st->act[r->ai] = last;
  st->rec[last].ai = r->ai;
}

static void drain(SfStream *st) {
  while (st->qr != st->qw) {
    const SfMsg *m = &st->q[st->qr & (QN - 1)];
    Rec *r = &st->rec[m->slot];
    drop(st, r);                                   /* a newer voice in the slot: the old one is gone */
    r->m = *m; r->on = 1; r->rel = -1; r->fill = m->p0;
    r->ai = st->nact; st->act[st->nact++] = m->slot;
    InterlockedIncrement(&st->qr);
    if (st->slot[r->m.slot].live != r->m.gen) drop(st, r);   /* already over */
  }
}

/* path frames from p that map to consecutive sample frames, and the first of them in *s; 0 past the path's end */
static int64_t run_at(const Rec *r, int64_t p, int64_t *s) {
  int64_t ls = r->m.ls, le = r->m.le, L = le - ls, end = r->m.end;
  if (!r->m.loopy || p < le) { *s = p; return (r->m.loopy ? le : end) - p; }
  if (r->rel >= 0 && p >= le + r->rel * L) { *s = p - r->rel * L; return *s < end ? end - *s : 0; }
  {
    int64_t k = (p - le) % L, n = L - k;
    *s = ls + k;
    if (r->rel >= 0 && p + n > le + r->rel * L) n = le + r->rel * L - p;
    return n;
  }
}

static void service(SfStream *st, int i) {
  Rec *r = &st->rec[i];
  SfSlot *sl = &st->slot[i];
  float *ring = sfs_ring(st, i);
  LONG g = r->m.gen, rl;
  LONG64 nd;
  int64_t need, total, limit, L = (int64_t)r->m.le - r->m.ls, hf = r->m.hf, t0 = r->m.t0, t1 = r->m.t1;
  long got;
  if (!r->on) return;
  if (sl->live != g) { drop(st, r); return; }
  rl = InterlockedCompareExchange(&sl->rel, 0, 0);
  if (sl->live != g) { drop(st, r); return; }          /* rel may belong to a newer voice: read it while g still lives */
  if (rl >= 0 && r->rel < 0 && r->m.loopy) {        /* let go: the path goes on to the end from that lap */
    int64_t b = r->m.le + (int64_t)rl * L;
    r->rel = rl;
    if (r->fill > b) r->fill = b;
    nd = sl->need;
    if (SF_TAGGEN(nd) == (g & 0x7FFFFF) && r->fill >= SF_VAL(nd)) sl->filled = SF_TAG(g, 1) | r->fill;   /* what is left stays good */
  }
  nd = sl->need;
  if (SF_TAGGEN(nd) != (g & 0x7FFFFF)) return;
  need = SF_VAL(nd);
  if (r->fill < need) r->fill = need;               /* the voice ran ahead (an underrun): skip forward */
  total = !r->m.loopy ? (int64_t)r->m.end : r->rel >= 0 ? (int64_t)r->m.end + (int64_t)r->rel * L : INT64_MAX;
  if (r->fill >= total) { sl->filled = SF_TAG(g, r->rel >= 0) | r->fill; return; }
  if (!st->topup && r->fill - need >= SF_RING / 2) return;   /* at least half full */
  if (!r->committed) {
    if (!VirtualAlloc(ring, SF_RING * sizeof(float), MEM_COMMIT, PAGE_READWRITE)) return;   /* tried again next pass */
    r->committed = 1;
  }
  limit = need + SF_RING - 1;
  if (limit > total) limit = total;
  while (r->fill < limit) {
    int64_t s, n = run_at(r, r->fill, &s), wrap;
    if (n <= 0) { r->fill = limit; break; }
    if (r->m.loopy && r->rel < 0 && r->m.le <= hf && r->fill >= r->m.le) { r->fill = limit; break; }   /* round a loop in the head */
    if (n > limit - r->fill) n = limit - r->fill;
    /* frames the voice has in memory (the head, the post-loop head) are skipped */
    if (s < hf) { r->fill += s + n <= hf ? n : hf - s; continue; }
    if (s >= t0 && s < t1) { r->fill += s + n <= t1 ? n : t1 - s; continue; }
    if (s < t0 && s + n > t0) n = t0 - s;
    if (n > CHUNK) n = CHUNK;
    wrap = SF_RING - (r->fill & SF_RMASK);
    if (n > wrap) n = wrap;
    got = smp_sf_stream_read(r->m.stream, (uint64_t)s, ring + (r->fill & SF_RMASK), (long)n);
    r->fill += got > 0 ? got : 0;
    if (sl->live != g) return;                      /* gone meanwhile: dropped on the next pass */
    if (got < n) {                                  /* a read failed: what came is good, the rest is tried again on a
                                                       later pass, and meanwhile the voice fades and counts it */
      sl->filled = SF_TAG(g, r->rel >= 0) | r->fill;
      return;
    }
    sl->filled = SF_TAG(g, r->rel >= 0) | r->fill;  /* volatile store: the ring data is visible first */
  }
  sl->filled = SF_TAG(g, r->rel >= 0) | r->fill;
  r->idle_since = GetTickCount64();
}

static DWORD WINAPI streamer(LPVOID arg) {
  SfStream *st = (SfStream *)arg;
  DWORD wait = INFINITE;
  while (!st->quit) {
    int i, k, committed = 0;
    ULONGLONG now;
    WaitForSingleObject(st->wake, wait);
    if (st->quit) break;
    drain(st);
    if (st->paused) {                              /* tests: no reads, but ended voices still let go */
      for (k = st->nact - 1; k >= 0; k--) {
        Rec *r = &st->rec[st->act[k]];
        if (st->slot[r->m.slot].live != r->m.gen) drop(st, r);
      }
    } else {
      /* the voices nearest to running dry first, then the rest (a service may drop a record: walk down) */
      for (k = st->nact - 1; k >= 0; k--) {
        i = st->act[k];
        if (st->rec[i].fill - SF_VAL(st->slot[i].need) < SF_RING / 4) service(st, i);
      }
      for (k = st->nact - 1; k >= 0; k--) if (k < st->nact) service(st, st->act[k]);
    }
    now = GetTickCount64();
    if (now - st->last_sweep >= 1000 || !st->nact) {   /* rings nobody has used for a while go back */
      st->last_sweep = now;
      for (i = 0; i < st->nslots; i++) {
        Rec *r = &st->rec[i];
        if (r->on || !r->committed) continue;
        if (now - r->idle_since >= DECOMMIT_MS) {
          VirtualFree(sfs_ring(st, i), SF_RING * sizeof(float), MEM_DECOMMIT);
          r->committed = 0;
        } else committed = 1;
      }
    } else committed = 1;
    /* poll while voices stream; else sleep until woken (or until an idle ring is due to go) */
    wait = st->nact ? POLL_MS : committed ? 1000 : INFINITE;
    InterlockedIncrement64(&st->pass);
    SetEvent(st->idle);
  }
  smp_pread_thread_done();
  return 0;
}

SfStream *sfs_new(int nslots) {
  SfStream *st = (SfStream *)calloc(1, sizeof *st);
  int i;
  if (!st) return NULL;
  st->nslots = nslots;
  st->slot = (SfSlot *)calloc((size_t)nslots, sizeof *st->slot);
  st->rec = (Rec *)calloc((size_t)nslots, sizeof *st->rec);
  st->act = (int *)calloc((size_t)nslots, sizeof *st->act);
  st->rings = (float *)VirtualAlloc(NULL, (SIZE_T)nslots * SF_RING * sizeof(float), MEM_RESERVE, PAGE_READWRITE);
  st->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
  st->idle = CreateEventW(NULL, FALSE, FALSE, NULL);
  if (!st->slot || !st->rec || !st->act || !st->rings || !st->wake || !st->idle) { sfs_free(st); return NULL; }
  for (i = 0; i < nslots; i++) st->slot[i].rel = -1;
  st->thread = CreateThread(NULL, 256 * 1024, streamer, st, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
  if (!st->thread) { sfs_free(st); return NULL; }
  SetThreadPriority(st->thread, THREAD_PRIORITY_ABOVE_NORMAL);
  return st;
}

void sfs_free(SfStream *st) {
  int i;
  if (!st) return;
  if (st->thread) {
    InterlockedExchange(&st->quit, 1);
    SetEvent(st->wake);
    WaitForSingleObject(st->thread, INFINITE);
    CloseHandle(st->thread);
  }
  if (st->rec && st->act) {
    drain(st);                                     /* queued starts carry holds too */
    for (i = 0; i < st->nslots; i++) drop(st, &st->rec[i]);
  }
  if (st->rings) VirtualFree(st->rings, 0, MEM_RELEASE);
  if (st->wake) CloseHandle(st->wake);
  if (st->idle) CloseHandle(st->idle);
  free(st->slot); free(st->rec); free(st->act);
  free(st);
}

int sfs_sync_for(SfStream *st, int timeout_ms) {
  LONG64 target;
  ULONGLONG t0 = GetTickCount64();
  int done = 1;
  if (!st) return 1;
  InterlockedIncrement(&st->topup);
  target = st->pass + 2;                           /* a whole pass that began after this call */
  while (st->pass < target) {
    if (timeout_ms >= 0 && GetTickCount64() - t0 >= (ULONGLONG)timeout_ms) { done = 0; break; }
    SetEvent(st->wake);
    WaitForSingleObject(st->idle, 50);
  }
  InterlockedDecrement(&st->topup);
  return done;
}

void sfs_sync(SfStream *st) { sfs_sync_for(st, -1); }
