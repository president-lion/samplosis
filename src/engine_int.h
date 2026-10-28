/* Shared by engine.c and stream.c: the engine and voice structs and the play path. Not part of the API. */
#ifndef SMP_ENGINE_INT_H
#define SMP_ENGINE_INT_H
#include <windows.h>
#include <stdint.h>
#include "samplosis.h"

#define SMP_MAXV 64
#define SMP_RING 65536                   /* frames per voice ring (power of two) */
#define SMP_RMASK (SMP_RING - 1)
#define SMP_CHUNK 16384                  /* frames per streamer read */
#define SMP_MAXEV 1024
#define SMP_NOFILL INT64_MAX             /* v0 of a voice that needs no ring (whole entry) */

/* a counter packed with a voice generation: (gen & 0xFFFFFF) << 40 | frames */
#define SMP_TAG(g) (((LONG64)((g) & 0xFFFFFF)) << 40)
#define SMP_TAGOF(x) ((LONG)(((x) >> 40) & 0xFFFFFF))
#define SMP_VALOF(x) ((x) & ((1LL << 40) - 1))

/* a pre-read copy of the first frames of the region from the current start point (start past the entry's head),
   made by the worker thread and published by atomic pointer */
typedef struct SmpStartHead { SmpSample *s; SmpEntry *src; int64_t at, n; float *lr; } SmpStartHead;
#define SMP_MAXSHRET 64

/* A keep-length copy, published in the engine's table by atomic pointer (slot = shift + 127). It is identified by
   content (the key of what it was rendered from and the speed), so a stale one is simply never used; the render
   thread retires stale ones. Its start head (start past the copy's head) is built lazily by the worker. */
#define SMP_KLN 256
#define SMP_KLMAX 127
#define SMP_KLRENDER 48                  /* Rubber Band R3 fails past about 4 octaves (+67 throws): wider shifts resample */
#define SMP_KLPRE 12                     /* shifts prepared as soon as a sample loads; others on their first note-on */
typedef struct SmpKl {
  SmpEntry *e;
  uint64_t base;                          /* src_key or rev_key it was rendered from */
  LONG speedp;                            /* speed in hundredths of a percent (10000 = 100 %) */
  int semis, rev;                         /* rev: rendered from the reversed copy */
  SmpStartHead *volatile sh;             /* written by the worker, read by the audio thread */
  volatile LONG want_sh;                 /* the audio thread asks the worker for a start head */
} SmpKl;
typedef struct { SmpKl *k; LONG64 a0, s0, w0; int phase; } SmpKlRet;
#define SMP_RQ 16

typedef struct SmpVoice {
  /* Written by the audio thread at note-on, before gen is bumped; read by the streamer after reading gen. */
  SmpEntry *src;                         /* what the voice reads: forward, reversed (Task 6: a rendered copy) */
  int64_t rs, re, rls;                   /* region start, end, loop start, in src frames */
  int loop;                              /* 0 off, 1 forward, 2 pingpong */
  int64_t v0;                            /* first path frame read from the ring (earlier ones come from the head) */
  volatile LONG gen;                     /* bumped at every note-on in this slot */
  volatile LONG live;                    /* 1 while the voice sounds */
  SmpSample *volatile smp;               /* the sample the voice belongs to (retirement checks) */
  volatile LONG64 need;                  /* audio: tag | oldest path frame still needed */
  volatile LONG64 filled;                /* streamer: tag | path frames written to the ring so far */
  float *ring;                           /* SMP_RING stereo frames */

  /* audio thread only */
  int key, whole, fading, stage, underrun;
  const float *head; uint64_t head_frames;
  const float *hp;                       /* path frames below v0 come from here: the head or a start head */
  const SmpStartHead *volatile sh;       /* the start head in use, if any (its retirement check reads this) */
  double src_semis;                      /* semitones already in src (0 when resampling) */
  int kl;                                /* src is a keep-length copy (or the source standing in for shift 0):
                                            speed is already in it, so it doesn't scale the rate */
  double pos, vgain, env, k, att_inc, fade, fade_step;
  int64_t left, path_len;
  uint64_t order;

  /* streamer only */
  LONG s_gen; int64_t s_fill;
} SmpVoice;

typedef struct { int type, key, vel, at; double x; } SmpEv;   /* type 0 on, 1 off, 2 bend */
typedef struct { SmpSample *s; LONG64 a0, s0, w0; int phase; } SmpRetired;

struct SmpEngine {
  double rate; int max_block;
  volatile LONG64 p[SMP_NPARAMS];        /* doubles as bits, written atomically */
  SmpSample *volatile sample;            /* published by the main thread */
  SmpSample *cur;                        /* the audio thread's view */
  SmpVoice v[SMP_MAXV];
  float *rings;
  SmpEv ev[SMP_MAXEV]; int nev;
  double bend;
  volatile unsigned rng;
  uint64_t order;
  volatile LONG64 epoch;                 /* blocks the audio thread has finished */
  volatile LONG64 spass;                 /* passes the streamer has finished */
  volatile LONG underruns;
  volatile LONG quit, want_rev;
  HANDLE wake, idle, thread;
  HANDLE work, widle, worker;           /* low-priority worker: reversed copies and start heads */
  volatile LONG64 wpass;                 /* passes the worker has finished */
  SmpStartHead *volatile sh[2];          /* forward, reversed */
  struct { SmpStartHead *h; LONG64 a0; } shret[SMP_MAXSHRET]; int nshret;   /* worker only */
  int offline;                           /* set only inside smp_engine_render_test: note-ons may wait for the streamer */
  CRITICAL_SECTION rev_cs;               /* main and streamer, around smp_sample_reversed */
  DWORD main_tid;
  SmpRetired *ret; int nret, cret;       /* main thread only */
  float *tmp;                            /* streamer scratch */

  /* keep length (render.c): the render thread renders and publishes copies; see render.c for the handoffs */
  SmpKl *volatile kl[SMP_KLN];
  HANDLE rwork, ridle, rthread;
  volatile LONG rquit;
  volatile LONG64 ridle_n;               /* render passes that found nothing to do */
  SmpSample *volatile r_using;           /* the sample the render thread is reading (collect won't free it) */
  volatile LONG r_ready, r_total, r_done;
  CRITICAL_SECTION r_cs; char r_why[512];   /* render thread and main thread only */
  volatile LONG rq[SMP_RQ]; volatile LONG rq_w; LONG rq_r;   /* note-on requests: audio writes, render reads */
  SmpKlRet *klret; int nklret, cklret;   /* render thread only */
};

double smp_eng_param(const SmpEngine *e, int id);
int smp_stream_start(SmpEngine *e);
void smp_stream_stop(SmpEngine *e);
void smp_stream_wait_idle(SmpEngine *e);  /* offline helper: returns after a full streamer pass has run */
int smp_worker_start(SmpEngine *e);
void smp_worker_stop(SmpEngine *e);
void smp_worker_wait_idle(SmpEngine *e);  /* offline helper: returns after a full worker pass has run */
int smp_render_start(SmpEngine *e);
void smp_render_stop(SmpEngine *e);       /* stops the render thread */
void smp_render_free_copies(SmpEngine *e);   /* frees every published and retired copy: every thread stopped */
void smp_kl_free(SmpKl *k);
LONG smp_speedp(double speed_percent);

/* A key's shift in semitones before bend: mode, root, transpose and fine (p: the parameters by id). Note-on and the
   render thread both round it to the nearest integer to pick a keep-length copy. */
static __inline double smp_note_semis(const double *p, int key) {
  int mode = (int)p[SMP_P_MODE];
  double s = 0;
  if (mode == 1) s = key - p[SMP_P_ROOT];
  else if (mode == 2) {
    double nl = p[SMP_P_NOTE_LO], nh = p[SMP_P_NOTE_HI], pl = p[SMP_P_PITCH_LO], ph = p[SMP_P_PITCH_HI];
    s = nh > nl ? pl + (key - nl) * (ph - pl) / (nh - nl) : pl;
  }
  return s + p[SMP_P_TRANSPOSE] + p[SMP_P_FINE] / 100.0;
}

/* The region in src frames for F frames. Reversed plays the mirrored region; loop start keeps its offset into it. */
static __inline void smp_region(double start, double end, double loop_start, int64_t F, int rev,
                                int64_t *rs, int64_t *re, int64_t *rls) {
  int64_t sf = (int64_t)(start / 100.0 * (double)F), ef = (int64_t)(end / 100.0 * (double)F),
          lsf = (int64_t)(loop_start / 100.0 * (double)F);
  if (sf >= F) sf = F - 1;
  if (ef > F) ef = F;
  if (ef <= sf) ef = sf + 1;
  if (lsf < sf) lsf = sf;
  if (lsf >= ef) lsf = ef - 1;
  if (rev) { *rs = F - ef; *re = F - sf; *rls = *rs + (lsf - sf); }
  else { *rs = sf; *re = ef; *rls = lsf; }
}

/* Path frame v of a voice -> src frame, direction and frames left in that straight run. 0 past the end. */
static __inline int smp_path_run(int64_t rs, int64_t re, int64_t rls, int loop, int64_t v,
                                 int64_t *at, int *dir, int64_t *run) {
  int64_t first = re - rs, L = re - rls, u, w, P;
  if (v < first) { *at = rs + v; *dir = 1; *run = first - v; return 1; }
  if (!loop || L <= 0) return 0;
  u = v - first;
  if (loop == 1 || L < 2) { w = u % L; *at = rls + w; *dir = 1; *run = L - w; return 1; }
  P = 2 * (L - 1); w = u % P;            /* pingpong: down from re-2 to rls, then up to re-1 */
  if (w < L - 1) { *at = re - 2 - w; *dir = -1; *run = (L - 1) - w; }
  else { *at = rls + 1 + (w - (L - 1)); *dir = 1; *run = P - w; }
  return 1;
}
#endif
