#pragma once
/* The Windows stage of the decode chain (mfdecode.c): a file read through Media Foundation on a helper thread, with
   a stall timer. Called from the main thread or worker threads, never from the audio thread. */
#include <stddef.h>
#include "samplosis.h"

enum {
  SMP_MF_OK = 1,          /* decoded: *x (interleaved, malloc'd), *floats, *ch and *sr are set */
  SMP_MF_NOT_MINE = 0,    /* Windows can't open it as media: err untouched */
  SMP_MF_FAILED = -1,     /* Windows opened it but couldn't decode it (no decoder, no sound, damaged): err says why */
  SMP_MF_STOPPED = -2,    /* cancelled, or no progress for too long: err says which */
  SMP_MF_TOO_LONG = -3,   /* more than max_floats: err untouched */
  SMP_MF_NO_MF = -4,      /* this Windows has no Media Foundation (an N edition without the Media Feature Pack) */
  SMP_MF_BUSY = -5        /* another helper's start of Media Foundation hasn't finished in 5 s */
};

/* An MPEG-4 file's edit list (Windows doesn't apply it): where its first sound track's audio really starts, in
   seconds (the coder's start delay, 1024 frames for AAC as ffmpeg and iTunes write it), and how long it plays (0 when
   it doesn't say). 1 when the file has one. smp_test_only_mp4_edit is the same call exported for the tests. */
int smp_mp4_edit(const unsigned char *b, size_t len, double *start, double *dur);
SMP_API int smp_test_only_mp4_edit(const unsigned char *b, size_t len, double *start, double *dur);

/* path is UTF-8; at most max_floats floats. cancel may be NULL; it is asked every 50 ms while Windows works. The
   wait ends when Windows has made no progress (no sample) for 60 s. *trimmed is 1 when Windows marked a
   start delay (a first sample before time zero) and it was dropped; the caller then doesn't apply an edit list
   too. */
int smp_mf_decode(const char *path, size_t max_floats, float **x, size_t *floats, unsigned *ch, unsigned *sr,
                  int *trimmed, int (*cancel)(void *), void *arg, char *err, int cap);

/* TESTS ONLY, from smp_test_only_decode_flags: the helper sleeps 3 s and the stall timer is 0.5 s; Media
   Foundation counts as missing; a sample's buffer can't be read at the first or the third sample; another helper
   seems to be starting Media Foundation (busy after 0.2 s) */
enum { SMP_MF_TEST_SLOW = 4, SMP_MF_TEST_NO_MF = 8, SMP_MF_TEST_BUFFER_FAILS_FIRST = 16, SMP_MF_TEST_BUFFER_FAILS_THIRD = 32,
       SMP_MF_TEST_BUSY = 64 };
void smp_mf_set_test_flags(long f);
/* TESTS ONLY: helper threads still running (an abandoned one finishes later) */
SMP_API long smp_test_only_mf_helpers(void);
