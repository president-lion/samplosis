/* sfstream.c: the SoundFont streamer thread, one per player (and one for each smp_sf_test_note). It keeps each
   streaming voice's ring buffer ahead of the voice, reading the voice's path (straight on, round its loop, and on to
   the end when a loop-until-release lets go) from the sample's file or cache file. Frames inside the sample's head
   (and its post-loop head) are never read: the voice takes those from memory. The audio thread wakes it after
   every block in which a voice streams; while any voice streams it also polls every POLL_MS (20 ms) as a backstop,
   and a read finishing wakes it too.

   Handoffs with the audio thread, per voice slot (no locks):
   - At a streaming voice's start the audio thread gives the slot a new generation g: it sets rel to -1, publishes
     need = tag(g) | the first path frame and its speed, then live = g, takes a second hold of the sample (for the
     streamer), and queues a start message with the voice's layout and that hold. When the queue is full the voice
     keeps the hold count it took back, sets live to 0 and plays without a ring (its head, then silence).
   - The streamer drains the queue at the start of each pass and keeps one record per slot. A record is alive while
     live == g. When it isn't (the voice ended, or a newer voice has the slot), the streamer releases the record's
     hold, but never while a read for the record is in flight: then the release (and a newer voice's start in the
     same slot, which would share the ring) waits for the read to finish. So the sample can't be freed under a read
     and a ring is never written for two voices at once.
   - The streamer writes ring frames, then publishes filled = tag(g, released) | path frames written. The audio
     thread reads ring frames only below filled and only while the tag's generation is its own, so data from an
     older note in the same slot is never played. It never writes more than SF_RING - 1 frames past need.
   - A loop-until-release (sample mode 3) changes the path when the key goes up: the audio thread stores the laps
     done in rel. Ring frames from that lap's loop end on were read for the loop, so until the streamer has
     rewound to there (filled then carries the released bit) the audio thread reads only below that point.
   - Rings are reserved for every slot and committed by the streamer before it first writes one; a ring no voice
     has used for 10 s is decommitted. The audio thread never touches a ring whose filled tag isn't its own, and the
     streamer publishes a tag only after committing, so neither step needs a lock.
   A player waits for this thread (sfs_sync) before closing a font, so every hold on it is released first.

   Scheduling (the cold-disk rule: notes struck together must not wait behind each other's first reads; sfstream.h
   says which of the parts below are on by default, from measurements on a USB hard disk):
   - Each pass works out, for every record, the frames its voice already has ahead of it: the ring, plus the head
     and the post-loop head, which it plays from memory. Divided by the voice's speed (sample frames per second, from
     its pitch) that is its slack, the time until it runs dry.
   - Records are served in slack order. Round 1 gives each record with under URGENT_MS of slack its ring filled;
     round 2 tops up the rings under half full in the same order. A visit reads the whole stretch to the ring's
     limit as back-to-back overlapped reads (a seek costs far more on a hard disk than reading on: short reads
     "just enough to lift a voice over a target" were measured to double the seeks at 128 voices). By default one
     record's reads are in flight at a time (SFS_F_ASYNC lets up to NIO records' reads be in flight); a pass
     doesn't wait for them, and the next pass, woken by a read finishing or by the audio thread, sorts again.
   - Urgent first reads of the same file within one 25 ms slack band go in byte order (a single-file font's
     samples sit together on the disk).
   - The two halves of a stereo file (two voices of one region, started in the same note event, with the same
     layout over the same frames) are read once: the first half's record (the leader) reads both channels and
     writes both rings. The pair parts when the voices' needs or releases part, or one of them ends; the other
     then reads for itself from where the pair had got to.
   - Reads land in the ring itself and are converted in place when a frame (all channels) is at most 4 bytes; wider
     layouts go through a 256 KB heap buffer, allocated once per streamer the first time one is needed. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "sf2.h"
#include "sfstream.h"

#define QN 256                           /* queued starts (power of two) */
#define NIO 8                            /* overlapped reads in flight at most */
#define MAX_READ (SF_RING / 2)           /* frames per read at most */
#define SMALL_READ 16384                 /* bytes per read at most without SFS_F_BIGREAD (the old stack buffer) */
#define URGENT_MS 150.0                  /* slack under this gets a read in round 1 */
#define SORT_BAND_MS 25.0                /* urgent first reads in one band of slack go in byte order */
#define RETRY_MS 20                      /* a read that failed or came short is tried again after this */
#define POOL_BYTES (256 * 1024)          /* the heap buffer for wide layouts: NIO pieces */
#define DECOMMIT_MS 10000                /* a ring unused this long is given back */
#define POLL_MS 20                       /* a backstop: the audio thread wakes the streamer after every block in which a
                                            voice streams, and a finished read wakes it too */
#define CANCEL_WAIT_MS 5000              /* sfs_free waits this long for cancelled reads (a dead network share) */

static volatile LONG g_feat = SFS_F_DEFAULT;
int sfs_features(void) { return g_feat; }
SMP_API int smp_sfs_test_features(int mask) {
  LONG old = g_feat;
  InterlockedExchange(&g_feat, mask < 0 ? SFS_F_DEFAULT : (mask & SFS_F_ALL));
  return (int)old;
}

/* tests: no pass counts as settled (a busy streamer on a cold disk that never catches up) */
static volatile LONG g_unsettled;
SMP_API void smp_sfs_test_unsettled(int on) { InterlockedExchange(&g_unsettled, on ? 1 : 0); }

/* tests: read latencies of every streamer, in buckets (see smp_sfs_test_read_hist) */
static volatile LONG g_hist[10], g_nreads, g_kb;
static const double g_edges[9] = {1, 2, 5, 10, 20, 50, 100, 200, 500};
static void hist_add(double ms, long bytes) {
  int b = 0;
  while (b < 9 && ms >= g_edges[b]) b++;
  InterlockedIncrement(&g_hist[b]);
  InterlockedIncrement(&g_nreads);
  InterlockedExchangeAdd(&g_kb, bytes / 1024);
}
static double now_ms(void) {
  static LARGE_INTEGER fq;
  LARGE_INTEGER t;
  if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart * 1000.0 / (double)fq.QuadPart;
}
SMP_API void smp_sfs_test_read_hist(long *out, int n, int reset) {
  int i;
  for (i = 0; i < n && i < 12; i++) out[i] = i < 10 ? g_hist[i] : i == 10 ? g_nreads : g_kb;
  if (reset) {
    for (i = 0; i < 10; i++) InterlockedExchange(&g_hist[i], 0);
    InterlockedExchange(&g_nreads, 0); InterlockedExchange(&g_kb, 0);
  }
}

typedef struct {
  SfMsg m;
  int on;                                /* the record holds m's sample */
  int ai;                                /* its place in the active list while on */
  LONG rel;                              /* the laps the path stopped looping at, or -1 */
  int64_t fill;                          /* path frames written so far (or skipped: in memory) */
  int committed;                         /* the slot's ring is committed */
  ULONGLONG idle_since;                  /* when the ring was last used (for the decommit) */
  int nio;                               /* its reads in flight (or finished, waiting for an earlier one) */
  int64_t ifill, ilimit;                 /* while it has reads: where the next would start, and where they stop */
  int bad;                               /* a read of the stretch failed: the rest of it is thrown away */
  LONG fgen;                             /* the follower's generation when the reads started (a pair) */
  int dead;                              /* its voice is gone: dropped once its read finishes */
  LONG64 dead_at;                        /* while dead: the pass it was found dead in (see sfs_release_sync) */
  int pend;                              /* a newer voice in the slot, started once those reads finish: its message
                                            in the streamer's pend pool, or -1 */
  int lead, led;                         /* a stereo pair: the follower's slot (in the leader), the leader's slot (in
                                            the follower, which then reads nothing itself); -1 when none */
  int reads;                             /* reads done for it */
  ULONGLONG retry_at;                    /* after a failed or short read: no read before this */
  int64_t need, limit, total;            /* this pass: the voice's need, the furthest frame to write, the path end */
} Rec;

typedef struct {
  OVERLAPPED o;                          /* its hEvent is made once, with the streamer */
  int busy, inflight, failed, rec, fb;   /* busy: the slot is taken (in flight, or finished and not yet applied) */
  int64_t p; long n, got;                /* the read's first path frame, frames asked and frames that came */
  uint8_t *raw;                          /* where its bytes land */
  HANDLE h;
  double t0;                             /* when it was issued */
} Io;

typedef struct { double k1; uintptr_t k2; uint64_t k3; int i; } Cand;

struct SfStream {
  int nslots;
  SfSlot *slot;
  float *rings;                          /* nslots rings, reserved */
  Rec *rec;                              /* the streamer's own */
  int *act, nact;                        /* the slots whose records are on, so a pass touches only those */
  Cand *cand; int ncand;                 /* a pass's records in serving order (grown with the active voices) */
  SfMsg pendq[NIO]; int pend_on[NIO];    /* newer voices waiting for a slot's reads (only a record with reads in
                                            flight has one, so NIO of them at most) */
  Io io[NIO];
  int nbusy;
  uint8_t *pool;                         /* the heap buffer for wide layouts, made the first time one is read */
  ULONGLONG last_sweep;                  /* when idle rings were last looked at */
  int leak;                              /* a cancelled read never finished: the memory it writes is never freed */
  SfMsg q[QN];
  volatile LONG qw, qr;                  /* written by the audio thread and by the streamer */
  HANDLE thread, wake, idle;
  volatile LONG quit, topup, paused;
  volatile LONG64 pass;                  /* passes sfs_sync counts (settled ones while it tops up) */
  volatile LONG64 npass;                 /* every pass, settled or not */
  volatile LONG64 dead_floor;            /* at the end of the last pass: the earliest pass a record that still holds
                                            its sample was found dead in (an ended voice not yet dropped counts as
                                            that pass); INT64_MAX when none */
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

static void unpair(SfStream *st, Rec *r) {
  if (r->lead >= 0) { st->rec[r->lead].led = -1; r->lead = -1; }
  if (r->led >= 0) { st->rec[r->led].lead = -1; r->led = -1; }
}

static void start_rec(SfStream *st, Rec *r, const SfMsg *m);

/* the record's voice is gone, but its reads are in flight: dropped when they finish */
static void mark_dead(SfStream *st, Rec *r) {
  if (!r->dead) r->dead_at = st->npass + 1;        /* the pass going on now */
  r->dead = 1;
}

/* lets go of a record's hold, unless a read for it is in flight (then that read's end does it). A newer voice
   waiting for the slot starts then. */
static void drop(SfStream *st, Rec *r) {
  int last;
  if (!r->on) return;
  if (r->nio) { mark_dead(st, r); return; }
  unpair(st, r);
  smp_sf_pcm_release(r->m.f, r->m.sample);
  r->on = 0; r->dead = 0;
  r->idle_since = GetTickCount64();
  last = st->act[--st->nact];                      /* out of the active list: the last one takes its place */
  st->act[r->ai] = last;
  st->rec[last].ai = r->ai;
  if (r->pend >= 0) {
    SfMsg m = st->pendq[r->pend];
    st->pend_on[r->pend] = 0;
    r->pend = -1;
    start_rec(st, r, &m);
  }
}

static void start_rec(SfStream *st, Rec *r, const SfMsg *m) {
  r->m = *m; r->on = 1; r->rel = -1; r->fill = m->p0;
  r->nio = 0; r->bad = 0; r->dead = 0; r->lead = r->led = -1; r->reads = 0; r->retry_at = 0;
  r->ai = st->nact; st->act[st->nact++] = m->slot;
  if (st->slot[m->slot].live != m->gen) drop(st, r);   /* already over */
}

/* b can be read by a's reads: the other channel of the same stereo frames, over the same path */
static int pairable(const Rec *a, const Rec *b) {
  const SfPcm *pa = (const SfPcm *)a->m.stream, *pb = (const SfPcm *)b->m.stream;
  if (!(g_feat & SFS_F_PAIR) || !a->on || !b->on || a->lead >= 0 || a->led >= 0 || a->reads || a->nio) return 0;
  if (!pa || !pb || pa->h == INVALID_HANDLE_VALUE || pa->h != pb->h || pa->offset != pb->offset ||
      pa->enc != pb->enc || pa->channels != 2 || pb->channels != 2 || pa->channel == pb->channel || pa->frames != pb->frames)
    return 0;
  return a->m.p0 == b->m.p0 && a->m.end == b->m.end && a->m.ls == b->m.ls && a->m.le == b->m.le &&
         a->m.loopy == b->m.loopy && a->m.hf == b->m.hf && a->m.t0 == b->m.t0 && a->m.t1 == b->m.t1 && a->fill == b->fill;
}

static void drain(SfStream *st) {
  int prev = -1;
  while (st->qr != st->qw) {
    SfMsg m = st->q[st->qr & (QN - 1)];
    Rec *r = &st->rec[m.slot];
    InterlockedIncrement(&st->qr);
    if (r->on && r->nio) {                     /* a read for the older voice is in flight: this one waits */
      if (r->pend < 0) {                           /* a free place in the pool (there always is: see pendq) */
        int k;
        for (k = 0; k < NIO && st->pend_on[k]; k++) {}
        if (k == NIO) { smp_sf_pcm_release(m.f, m.sample); prev = -1; continue; }   /* (never): it plays its head */
        r->pend = k; st->pend_on[k] = 1;
      } else smp_sf_pcm_release(st->pendq[r->pend].f, st->pendq[r->pend].sample);   /* replaced: let go */
      st->pendq[r->pend] = m; mark_dead(st, r);
      prev = -1;
      continue;
    }
    drop(st, r);                                   /* a newer voice in the slot: the old one is gone */
    start_rec(st, r, &m);
    if (r->on && prev >= 0 && pairable(&st->rec[prev], r)) { st->rec[prev].lead = m.slot; r->led = prev; }
    prev = r->on ? m.slot : -1;
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

/* moves fill past the frames the voice has in memory (the head, the post-loop head), up to limit: what is left
   at fill is the first frame that must be read */
static void skip_mem(Rec *r, int64_t limit) {
  int k;
  for (k = 0; k < 8 && r->fill < limit; k++) {
    int64_t s, n = run_at(r, r->fill, &s);
    if (n <= 0) { r->fill = limit; return; }
    if (r->m.loopy && r->rel < 0 && r->m.le <= r->m.hf && r->fill >= r->m.le) { r->fill = limit; return; }   /* round a loop in the head */
    if (n > limit - r->fill) n = limit - r->fill;
    if (s < r->m.hf) { r->fill += s + n <= r->m.hf ? n : r->m.hf - s; continue; }
    if (s >= r->m.t0 && s < r->m.t1) { r->fill += s + n <= r->m.t1 ? n : r->m.t1 - s; continue; }
    return;
  }
}

static void publish(SfStream *st, Rec *r) {
  st->slot[r->m.slot].filled = SF_TAG(r->m.gen, r->rel >= 0) | r->fill;   /* volatile store: the ring data first */
}

static int commit(SfStream *st, Rec *r) {
  if (r->committed) return 1;
  if (!VirtualAlloc(sfs_ring(st, r->m.slot), SF_RING * sizeof(float), MEM_COMMIT, PAGE_READWRITE)) return 0;
  r->committed = 1;
  return 1;
}

/* this pass's state of record i: 1 when it may want a read (alive, not reading, frames left before its limit) */
static int prep(SfStream *st, int i) {
  Rec *r = &st->rec[i];
  SfSlot *sl = &st->slot[i];
  LONG g = r->m.gen, rl;
  LONG64 nd;
  int64_t L = (int64_t)r->m.le - r->m.ls;
  if (!r->on) return 0;
  if (r->dead || sl->live != g) { drop(st, r); return 0; }
  if (r->nio) return 0;
  rl = InterlockedCompareExchange(&sl->rel, 0, 0);
  if (sl->live != g) { drop(st, r); return 0; }   /* rel may belong to a newer voice: read it while g still lives */
  if (rl >= 0 && r->rel < 0 && r->m.loopy) {        /* let go: the path goes on to the end from that lap */
    int64_t b = r->m.le + (int64_t)rl * L;
    r->rel = rl;
    if (r->fill > b) r->fill = b;
    nd = sl->need;
    if (SF_TAGGEN(nd) == (g & 0x7FFFFF) && r->fill >= SF_VAL(nd)) publish(st, r);   /* what is left stays good */
  }
  nd = sl->need;
  if (SF_TAGGEN(nd) != (g & 0x7FFFFF)) return 0;
  r->need = SF_VAL(nd);
  if (r->fill < r->need) r->fill = r->need;         /* the voice ran ahead (an underrun): skip forward */
  r->total = !r->m.loopy ? (int64_t)r->m.end : r->rel >= 0 ? (int64_t)r->m.end + (int64_t)r->rel * L : INT64_MAX;
  if (r->fill >= r->total) { publish(st, r); return 0; }
  r->limit = r->need + SF_RING - 1;
  if (r->limit > r->total) r->limit = r->total;
  return 1;
}

static int by_key(const void *a, const void *b) {
  const Cand *x = (const Cand *)a, *y = (const Cand *)b;
  if (x->k1 != y->k1) return x->k1 < y->k1 ? -1 : 1;
  if (x->k2 != y->k2) return x->k2 < y->k2 ? -1 : 1;
  if (x->k3 != y->k3) return x->k3 < y->k3 ? -1 : 1;
  return x->i - y->i;
}

static void complete(SfStream *st, int k);

/* Starts the reads for record r from its fill up to upto: the whole stretch at once, as several reads back to back
   when the ring wraps or the path turns (they are contiguous on the disk, so the disk serves them in one go; a seek
   costs far more than reading on). Returns how many reads started. */
static int issue(SfStream *st, Rec *r, int64_t upto) {
  const SfPcm *p = (const SfPcm *)r->m.stream;
  Rec *fo = r->lead >= 0 ? &st->rec[r->lead] : NULL;
  int started = 0, fb, feat = g_feat;
  if (!p || p->h == INVALID_HANDLE_VALUE || r->nio) return 0;
  if (!(feat & SFS_F_ASYNC) && st->nbusy) return 0;
  if (upto > r->limit) upto = r->limit;
  fb = sf_pcm_bytes(p->enc) * p->channels;
  if (!commit(st, r) || (fo && !commit(st, fo))) return 0;   /* tried again next pass */
  r->ifill = r->fill; r->ilimit = upto; r->bad = 0;
  r->fgen = fo ? fo->m.gen : 0;
  {
  LONG gen = r->m.gen;
  while (r->ifill < upto) {
    Io *io = NULL;
    int k;
    int64_t s, n, wrap, save = r->fill;
    uint64_t off;
    r->fill = r->ifill; skip_mem(r, upto); r->ifill = r->fill; r->fill = save;   /* frames in memory: not read */
    if (r->ifill >= upto) break;
    for (k = 0; k < NIO; k++) if (!st->io[k].busy) { io = &st->io[k]; break; }
    if (!io) break;
    n = run_at(r, r->ifill, &s);
    if (n > upto - r->ifill) n = upto - r->ifill;
    if (n <= 0) break;
    if (s < r->m.t0 && s + n > r->m.t0) n = r->m.t0 - s;   /* the post-loop head's frames are in memory */
    if (n > MAX_READ) n = MAX_READ;
    if (!(feat & SFS_F_BIGREAD) && n * fb > SMALL_READ) n = SMALL_READ / fb;
    wrap = SF_RING - (r->ifill & SF_RMASK);
    if (n > wrap) n = wrap;
    if (fb <= 4) {
      /* in place: the bytes land at the end of the ring stretch they become, and are converted front to back */
      io->raw = (uint8_t *)(sfs_ring(st, r->m.slot) + (r->ifill & SF_RMASK)) + (size_t)n * (size_t)(4 - fb);
    } else {
      size_t piece = POOL_BYTES / NIO;
      if (!st->pool && !(st->pool = (uint8_t *)malloc(POOL_BYTES))) break;
      io->raw = st->pool + (size_t)k * piece;
      if ((size_t)n * (size_t)fb > piece) n = (int64_t)(piece / (size_t)fb);
    }
    off = p->offset + (uint64_t)s * (uint64_t)fb;
    {
      HANDLE ev = io->o.hEvent;
      memset(&io->o, 0, sizeof io->o);
      io->o.hEvent = ev;
    }
    io->o.Offset = (DWORD)off; io->o.OffsetHigh = (DWORD)(off >> 32);
    io->busy = 1; io->inflight = 1; io->failed = 0; io->got = 0;
    io->rec = r->m.slot; io->fb = fb; io->p = r->ifill; io->n = (long)n; io->h = p->h;
    io->t0 = now_ms();
    st->nbusy++; r->nio++; started++;
    r->ifill += n;
    if (ReadFile(p->h, io->raw, (DWORD)(n * fb), NULL, &io->o)) complete(st, k);   /* done at once (the file cache) */
    else if (GetLastError() != ERROR_IO_PENDING) { io->failed = 1; complete(st, k); }
    if (r->bad || !r->on || r->m.gen != gen) break;   /* (a read done at once may have ended the record) */
  }
  }
  return started;
}

/* frees read slot k */
static void io_free(SfStream *st, int k) {
  st->io[k].busy = 0; st->io[k].inflight = 0;
  st->nbusy--;
  st->rec[st->io[k].rec].nio--;
}

/* Record r's finished reads, applied in path order: each one's frames are published once every read before it is
   in (the frames in memory between them skipped). After a failed or short read the rest of the stretch is thrown
   away as it comes in, and tried again later. A record whose voice went is dropped once no read of its is left. */
static void apply(SfStream *st, Rec *r) {
  int k, progressed = 1;
  Rec *fo = r->lead >= 0 ? &st->rec[r->lead] : NULL;
  if (fo && (!fo->on || fo->dead || fo->m.gen != r->fgen || fo->fill != r->fill)) { unpair(st, r); fo = NULL; }
  while (progressed) {
    progressed = 0;
    for (k = 0; k < NIO; k++) {
      Io *io = &st->io[k];
      if (!io->busy || io->inflight || io->rec != r->m.slot) continue;
      if (r->bad || r->dead) { io_free(st, k); progressed = 1; continue; }
      skip_mem(r, r->ilimit);
      if (io->p != r->fill) continue;               /* an earlier read is still on its way */
      r->fill = io->p + io->got;
      r->reads++;
      r->idle_since = GetTickCount64();
      if (io->got < io->n) { r->bad = 1; r->retry_at = r->idle_since + RETRY_MS; }   /* the voice fades and counts it */
      publish(st, r);
      if (fo) {
        fo->fill = r->fill; fo->reads++; fo->idle_since = r->idle_since; fo->retry_at = r->retry_at;
        publish(st, fo);
      }
      io_free(st, k);
      progressed = 1;
    }
  }
  if (!r->nio && (r->dead || st->slot[r->m.slot].live != r->m.gen)) drop(st, r);
}

/* a read has finished: its frames converted into the ring (both rings for a pair), then applied in order */
static void complete(SfStream *st, int k) {
  Io *io = &st->io[k];
  Rec *r = &st->rec[io->rec], *fo;
  const SfPcm *p = (const SfPcm *)r->m.stream;
  DWORD got = 0;
  long frames;
  if (!io->failed && !GetOverlappedResult(io->h, &io->o, &got, FALSE)) got = 0;
  hist_add(now_ms() - io->t0, (long)got);
  io->inflight = 0;
  frames = (long)(got / (DWORD)io->fb);
  if (frames > io->n) frames = io->n;
  io->got = frames;
  if (!r->dead && !r->bad && st->slot[io->rec].live == r->m.gen && frames > 0) {
    fo = r->lead >= 0 ? &st->rec[r->lead] : NULL;
    /* the follower's channel first: in place, the leader's own conversion overwrites the bytes as it goes */
    if (fo && fo->on && !fo->dead && fo->m.gen == r->fgen)
      sf_pcm_convert(io->raw, p->enc, p->channels, ((const SfPcm *)fo->m.stream)->channel, (uint32_t)frames,
                     sfs_ring(st, fo->m.slot) + (io->p & SF_RMASK), NULL);
    sf_pcm_convert(io->raw, p->enc, p->channels, p->channel, (uint32_t)frames, sfs_ring(st, r->m.slot) + (io->p & SF_RMASK), NULL);
  }
  apply(st, r);
}

/* one pass: the records in slack order, round 1 and round 2. Returns 1 when it is settled: no read in flight and
   none started (every ring as full as it should be for now) */
static int pass_once(SfStream *st) {
  int k, n = 0, started = 0, feat = g_feat;
  ULONGLONG now = GetTickCount64();
  for (k = st->nact - 1; k >= 0; k--) if (k < st->nact) prep(st, st->act[k]);   /* (a prep may drop a record) */
  if (st->paused) return st->nbusy == 0;            /* tests: no reads, but ended voices still let go */
  if (st->ncand < st->nact) {                       /* room to sort every active record (this thread's own) */
    int nc = st->nact < 64 ? 64 : st->nact * 2;
    Cand *c = (Cand *)realloc(st->cand, (size_t)nc * sizeof *c);
    if (!c) return st->nbusy == 0;                  /* tried again next pass */
    st->cand = c; st->ncand = nc;
  }
  for (k = 0; k < st->nact; k++) {
    int i = st->act[k];
    Rec *r = &st->rec[i], *fo = NULL;
    SfSlot *sl = &st->slot[i];
    const SfPcm *p = (const SfPcm *)r->m.stream;
    double speed, slack;
    int64_t need;
    if (!r->on || r->dead || r->nio || r->led >= 0) continue;
    if (SF_TAGGEN(sl->need) != (r->m.gen & 0x7FFFFF) || r->fill >= r->total) continue;
    if (r->lead >= 0) {                            /* a pair keeps going while the two voices go together */
      fo = &st->rec[r->lead];
      if (!fo->on || fo->dead || fo->rel != r->rel || fo->fill != r->fill ||
          SF_TAGGEN(st->slot[r->lead].need) != (fo->m.gen & 0x7FFFFF) || llabs(fo->need - r->need) > SF_RING / 4) {
        unpair(st, r); fo = NULL;
      } else if (fo->limit < r->limit) r->limit = fo->limit;
    }
    skip_mem(r, r->limit);
    if (fo) fo->fill = r->fill;
    if (r->fill >= r->limit) { publish(st, r); if (fo) publish(st, fo); continue; }
    if (now < r->retry_at) continue;
    need = fo && fo->need < r->need ? fo->need : r->need;
    speed = sl->speed > 0 ? (double)sl->speed : r->m.speed > 0 ? (double)r->m.speed : 44100.0;
    slack = (double)(r->fill - need) * 1000.0 / speed;
    st->cand[n].i = i;
    if (!(feat & SFS_F_DEADLINE)) {                /* the first order: newest records first */
      st->cand[n].k1 = -(double)k; st->cand[n].k2 = 0; st->cand[n].k3 = 0;
    } else if ((feat & SFS_F_SORT) && !r->reads && slack < URGENT_MS && p) {
      st->cand[n].k1 = floor(slack / SORT_BAND_MS) * SORT_BAND_MS;
      st->cand[n].k2 = (uintptr_t)p->h;
      st->cand[n].k3 = p->offset + (uint64_t)r->m.p0;
    } else {
      st->cand[n].k1 = slack; st->cand[n].k2 = 0; st->cand[n].k3 = 0;
    }
    n++;
  }
  if (n > 1) qsort(st->cand, (size_t)n, sizeof *st->cand, by_key);
  /* Round 1: the urgent (under URGENT_MS of slack), in deadline order. Each gets its ring filled at once: on a hard
     disk a seek costs as much as reading tens of milliseconds of audio on, so a short read "just enough to lift it
     over the target" bought nothing and cost a seek more later (measured: twice the seeks at 128 voices). */
  for (k = 0; k < n; k++) {
    Rec *r = &st->rec[st->cand[k].i];
    SfSlot *sl = &st->slot[st->cand[k].i];
    double speed = sl->speed > 0 ? (double)sl->speed : r->m.speed > 0 ? (double)r->m.speed : 44100.0;
    if (r->nio || !r->on) continue;
    if (feat & SFS_F_DEADLINE) { if ((double)(r->fill - r->need) * 1000.0 / speed >= URGENT_MS) continue; }
    else if (r->fill - r->need >= SF_RING / 4) continue;
    if (issue(st, r, r->limit)) started++;
    else if (st->nbusy >= NIO || (!(feat & SFS_F_ASYNC) && st->nbusy)) break;
  }
  /* round 2: the rings under half full topped up, in the same order */
  for (k = 0; k < n; k++) {
    Rec *r = &st->rec[st->cand[k].i];
    if (r->nio || !r->on || r->fill >= r->limit) continue;
    if (!st->topup && r->fill - r->need >= SF_RING / 2) continue;   /* at least half full */
    if (issue(st, r, r->limit)) started++;
    else if (st->nbusy >= NIO || (!(feat & SFS_F_ASYNC) && st->nbusy)) break;
  }
  return st->nbusy == 0 && started == 0;
}

static DWORD WINAPI streamer(LPVOID arg) {
  SfStream *st = (SfStream *)arg;
  DWORD wait = INFINITE;
  int k;
  while (!st->quit) {
    HANDLE hs[1 + NIO];
    DWORD nh = 1;
    int i, committed = 0, settled;
    ULONGLONG now;
    hs[0] = st->wake;
    for (k = 0; k < NIO; k++) if (st->io[k].busy && st->io[k].inflight) hs[nh++] = st->io[k].o.hEvent;
    WaitForMultipleObjects(nh, hs, FALSE, wait);
    if (st->quit) break;
    for (k = 0; k < NIO; k++)
      if (st->io[k].busy && st->io[k].inflight && HasOverlappedIoCompleted(&st->io[k].o)) complete(st, k);
    drain(st);
    settled = pass_once(st);
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
    /* more to start at once; else wait for a read, a block or the poll; else sleep until woken (or an idle ring
       is due to go) */
    wait = !settled && !st->nbusy && !st->paused ? 0 : st->nact || st->nbusy ? POLL_MS : committed ? 1000 : INFINITE;
    if (!st->topup || (settled && !g_unsettled))  /* a sync counts only settled passes */
      InterlockedIncrement64(&st->pass);
    {                                               /* what sfs_release_sync waits on */
      LONG64 lo = INT64_MAX, cur = st->npass + 1;
      for (i = 0; i < st->nact; i++) {
        Rec *r = &st->rec[st->act[i]];
        LONG64 at = r->dead ? r->dead_at : st->slot[st->act[i]].live != r->m.gen ? cur : INT64_MAX;
        if (at < lo) lo = at;
      }
      InterlockedExchange64(&st->dead_floor, lo);
      InterlockedIncrement64(&st->npass);           /* after the floor: a waiter that sees the pass sees its floor */
    }
    SetEvent(st->idle);
  }
  /* stopping: reads in flight are cancelled and waited for (bounded: a dead network share may never answer) */
  for (k = 0; k < NIO; k++) if (st->io[k].busy && st->io[k].inflight) CancelIoEx(st->io[k].h, &st->io[k].o);
  {
    ULONGLONG end = GetTickCount64() + CANCEL_WAIT_MS;
    for (k = 0; k < NIO; k++) {
      ULONGLONG t = GetTickCount64();
      if (!st->io[k].busy) continue;
      if (st->io[k].inflight && WaitForSingleObject(st->io[k].o.hEvent, t < end ? (DWORD)(end - t) : 0) != WAIT_OBJECT_0 ||
          !HasOverlappedIoCompleted(&st->io[k].o)) st->leak = 1;
      st->io[k].busy = 0; st->io[k].inflight = 0;
      st->rec[st->io[k].rec].nio = 0;
    }
    st->nbusy = 0;
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
  for (i = 0; i < NIO; i++) st->io[i].o.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!st->slot || !st->rec || !st->act || !st->rings || !st->wake || !st->idle) { sfs_free(st); return NULL; }
  for (i = 0; i < NIO; i++) if (!st->io[i].o.hEvent) { sfs_free(st); return NULL; }
  for (i = 0; i < nslots; i++) { st->slot[i].rel = -1; st->rec[i].lead = st->rec[i].led = -1; st->rec[i].pend = -1; }
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
    WaitForSingleObject(st->thread, INFINITE);     /* bounded: the thread waits at most CANCEL_WAIT_MS for reads */
    CloseHandle(st->thread);
  }
  if (st->rec && st->act) {
    drain(st);                                     /* queued starts carry holds too */
    for (i = 0; i < st->nslots; i++) {
      Rec *r = &st->rec[i];
      if (r->pend >= 0) { smp_sf_pcm_release(st->pendq[r->pend].f, st->pendq[r->pend].sample); st->pend_on[r->pend] = 0; r->pend = -1; }
      drop(st, r);
    }
  }
  if (st->leak) return;                            /* a read may still write into the rings: they stay */
  if (st->rings) VirtualFree(st->rings, 0, MEM_RELEASE);
  if (st->wake) CloseHandle(st->wake);
  if (st->idle) CloseHandle(st->idle);
  for (i = 0; i < NIO; i++) if (st->io[i].o.hEvent) CloseHandle(st->io[i].o.hEvent);
  free(st->pool);
  free(st->slot); free(st->rec); free(st->act); free(st->cand);
  free(st);
}

int sfs_sync_for(SfStream *st, int timeout_ms) {
  LONG64 target;
  ULONGLONG t0 = GetTickCount64();
  int done = 1;
  if (!st) return 1;
  InterlockedIncrement(&st->topup);
  target = st->pass + 2;                           /* a whole settled pass that began after this call */
  while (st->pass < target) {
    if (timeout_ms >= 0 && GetTickCount64() - t0 >= (ULONGLONG)timeout_ms) { done = 0; break; }
    SetEvent(st->wake);
    WaitForSingleObject(st->idle, 50);
  }
  InterlockedDecrement(&st->topup);
  return done;
}

void sfs_sync(SfStream *st) { sfs_sync_for(st, -1); }

/* Waits until every voice that ended before the call has let go of its sample, without asking for a top-up (so a
   busy streamer on a slow disk isn't made to fill every ring first). Pass n0 + 1 may have begun before the call;
   pass n0 + 2 began after it, so by its end the voices ended before the call have each been dropped, or (reads in
   flight) found dead in a pass no later than n0 + 2. Done at the end of a pass when no record found dead by then
   still holds its sample. */
int sfs_release_sync(SfStream *st, int timeout_ms) {
  LONG64 n0, target;
  ULONGLONG t0 = GetTickCount64();
  if (!st) return 1;
  n0 = st->npass;
  target = n0 + 2;
  for (;;) {
    LONG64 n = InterlockedCompareExchange64(&st->npass, 0, 0);
    if (n >= target && InterlockedCompareExchange64(&st->dead_floor, 0, 0) > target) return 1;
    if (timeout_ms >= 0 && GetTickCount64() - t0 >= (ULONGLONG)timeout_ms) return 0;
    SetEvent(st->wake);
    WaitForSingleObject(st->idle, 50);
  }
}
