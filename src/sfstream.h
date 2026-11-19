/* sfstream.h: the SoundFont streamer. One thread per player keeps a ring buffer ahead of each voice that plays
   past its sample's head, reading the rest of the sample from the font file or the cache (smp_sf_stream_read). See
   the top of sfstream.c for the handoffs. */
#ifndef SFSTREAM_H
#define SFSTREAM_H
#include "plat.h"
#include <stdint.h>
#include "samplosis.h"

#define SF_RING 16384                    /* frames per voice ring (power of two): 64 KB of mono float */
#define SF_RMASK (SF_RING - 1)
#define SF_UFADE 64                      /* samples of fade out and back in around an underrun */

/* need and filled: a voice generation and a "released" bit packed with a path frame count */
#define SF_TAG(g, rel) (((LONG64)((g) & 0x7FFFFF) << 41) | ((LONG64)((rel) ? 1 : 0) << 40))
#define SF_TAGGEN(x) ((LONG)(((x) >> 41) & 0x7FFFFF))
#define SF_TAGREL(x) ((int)(((x) >> 40) & 1))
#define SF_VAL(x) ((int64_t)((x) & ((1LL << 40) - 1)))

/* one voice slot, shared by the audio thread and the streamer */
typedef struct SfSlot {
  volatile LONG live;                    /* audio: the generation of the streaming voice sounding here, 0 when none */
  volatile LONG rel;                     /* audio: -1, or the laps done when a loop-until-release was released */
  volatile LONG64 need;                  /* audio: tag | the oldest path frame the voice still needs */
  volatile LONG64 filled;                /* streamer: tag | path frames written to the ring so far */
  volatile LONG speed;                   /* audio: the voice's sample frames per second (its pitch), for deadlines */
  LONG gen;                              /* audio only: the last generation given out in this slot */
} SfSlot;

/* a streaming voice's start, from the audio thread to the streamer. It carries one hold of the sample (taken by
   the voice for the streamer), which the streamer releases. Frames count from the sample's start. */
typedef struct SfMsg {
  SmpFont *f; const void *stream;
  int slot, sample; LONG gen;
  uint32_t hf;                           /* frames in the head: those are never read into the ring */
  uint32_t t0, t1;                       /* the post-loop head's frames [t0, t1) (none when equal): not read either */
  uint32_t end, ls, le;                  /* the voice's end and loop */
  int64_t p0;                            /* the first path frame the voice reads */
  int loopy;                             /* the voice loops (sample mode 1 or 3, with a loop) */
  LONG speed;                            /* sample frames per second, estimated at the start */
} SfMsg;

/* The sample frame at path frame p. A voice's path is its frames in the order it plays them: straight to the loop
   end, then round the loop (forever, or until the laps given by rel when a loop-until-release lets go, then on
   to the end). The voice and the streamer both map path frames with this, so they always agree. */
static __inline int64_t sf_path_frame(int64_t p, int64_t ls, int64_t le, int loopy, int64_t rel) {
  int64_t L = le - ls;
  if (!loopy || p < le) return p;
  if (rel >= 0 && p >= le + rel * L) return p - rel * L;
  return ls + (p - le) % L;
}

typedef struct SfStream SfStream;
SfStream *sfs_new(int nslots);           /* main thread: starts the thread; NULL when out of memory */
void sfs_free(SfStream *st);             /* main thread, while no audio thread runs: stops it, releases its holds */
SfSlot *sfs_slot(SfStream *st, int i);
float *sfs_ring(SfStream *st, int i);    /* the slot's ring (committed by the streamer before it writes to it) */
int sfs_push(SfStream *st, const SfMsg *m);   /* audio thread: 1 queued, 0 full (the caller keeps its hold) */
void sfs_wake(SfStream *st);             /* audio thread: a block was played */
void sfs_sync(SfStream *st);             /* offline and main thread: returns after a whole pass that tops every ring
                                            up (not just the half-empty ones) */
int sfs_sync_for(SfStream *st, int timeout_ms);   /* the same, giving up after timeout_ms: 1 done, 0 timed out */
int sfs_release_sync(SfStream *st, int timeout_ms);   /* main thread: returns once every voice ended before the call
                                            has let go of its sample (no top-up asked for); 1 done, 0 timed out */
void sfs_pause(SfStream *st, int on);    /* tests: the streamer reads nothing (forces underruns) */

/* The streamer's features. The default (SFS_F_DEFAULT) is what measured best on a USB hard disk (Bulk-Only
   Transport, one command at a time), cold, 64 and 128 voices: deadline order and whole-stretch reads, with first
   reads in byte order. The others measured no better or worse there and stay off: several records' reads in
   flight (the disk can't reorder them, and they break up each file's sequential reads), one read for both halves
   of a stereo pair (Windows serves the second half from its cache anyway), reading 0.4 s past each head at load
   (it only moves the chord's dropouts from 0.6 s to 1.4 s) and sequential-scan handles. tests\measure.py turns
   them on and off to measure them (smp_sfs_test_features). */
enum {
  SFS_F_DEADLINE = 1,                    /* records served in deadline order, urgent ones first with short reads */
  SFS_F_BIGREAD = 2,                     /* reads up to half a ring (else at most 16 KB, as stage 3 first did) */
  SFS_F_PAIR = 4,                        /* a stereo pair's two halves read with one read */
  SFS_F_ASYNC = 8,                       /* several records' reads in flight at once (else one record's at a time) */
  SFS_F_SORT = 16,                       /* urgent first reads of one file in byte order */
  SFS_F_WARM = 32,                       /* the loader reads 0.4 s past each head into the file cache */
  SFS_F_SEQ = 64,                        /* streaming handles opened for sequential reads (more Windows read-ahead) */
  SFS_F_ALL = 127,
  SFS_F_DEFAULT = SFS_F_DEADLINE | SFS_F_BIGREAD | SFS_F_SORT
};
int sfs_features(void);

#endif
