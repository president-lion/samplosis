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
  double src_semis;                      /* semitones already in src (0 when resampling) */
  double pos, vgain, env, k, att_inc, fade, fade_step;
  int64_t left, path_len;
  uint64_t order;

  /* streamer only */
  LONG s_gen; int64_t s_fill;
} SmpVoice;

typedef struct { int type, key, vel, at; double x; } SmpEv;   /* type 0 on, 1 off, 2 bend */
typedef struct { SmpSample *s; LONG64 a0, s0; int phase; } SmpRetired;

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
  int offline;                           /* set only inside smp_engine_render_test: note-ons may wait for the streamer */
  CRITICAL_SECTION rev_cs;               /* main and streamer, around smp_sample_reversed */
  DWORD main_tid;
  SmpRetired *ret; int nret, cret;       /* main thread only */
  float *tmp;                            /* streamer scratch */
};

double smp_eng_param(const SmpEngine *e, int id);
int smp_stream_start(SmpEngine *e);
void smp_stream_stop(SmpEngine *e);
void smp_stream_wait_idle(SmpEngine *e);  /* offline helper: returns after a full streamer pass has run */

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
