#pragma once
/* The FFmpeg stage of the decode chain (ffdecode.c): a file read by running the ffmpeg.exe the user installed, the
   last thing tried. Called from the main thread or worker threads, never from the audio thread. */
#include <stddef.h>
#include "samplosis.h"

enum {
  SMP_FF_OK = 1,          /* decoded: *x (interleaved, malloc'd), *floats, *ch and *sr are set */
  SMP_FF_NOT_MINE = 0,    /* FFmpeg can't open it, or finds no sound in it: err untouched */
  SMP_FF_MISSING = -1,    /* no ffmpeg.exe was found: err untouched */
  SMP_FF_FAILED = -2,     /* FFmpeg opened it but couldn't decode it: err holds FFmpeg's own reason, trimmed */
  SMP_FF_STOPPED = -3,    /* cancelled, or no output for too long: err says which */
  SMP_FF_TOO_LONG = -4    /* more than max_floats: err untouched */
};

/* path is UTF-8; at most max_floats floats. cancel may be NULL; it is asked every 50 ms while FFmpeg runs. stage,
   when it isn't NULL, is called with arg once FFmpeg has been found, just before it runs (the window says
   "Decoding x with FFmpeg"). FFmpeg is stopped (and waited for) when cancel says so, when it writes nothing for 60 s,
   or past max_floats. */
int smp_ff_decode(const char *path, size_t max_floats, float **x, size_t *floats, unsigned *ch, unsigned *sr,
                  int (*cancel)(void *), void (*stage)(void *), void *arg, char *err, int cap);

/* TESTS ONLY, from smp_test_only_decode_flags: the stall timer is 0.5 s; FFmpeg counts as not found; the stage waits
   1.5 s before it runs FFmpeg (asking cancel) */
enum { SMP_FF_TEST_STALL_SHORT = 128, SMP_FF_TEST_MISSING = 256, SMP_FF_TEST_SLOW_START = 512 };
void smp_ff_set_test_flags(long f);
