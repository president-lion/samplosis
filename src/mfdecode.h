#pragma once
/* The Windows stage of the decode chain (mfdecode.c): a file read through Media Foundation. Called from the main
   thread or worker threads, never from the audio thread. */
#include <stddef.h>

/* takes floats (whole interleaved frames); 0 refuses them, which stops the decode */
typedef int (*SmpMfPut)(void *ctx, const float *x, size_t floats);

enum {
  SMP_MF_OK = 1,          /* decoded: *ch and *sr are set, and every float went to put */
  SMP_MF_NOT_MINE = 0,    /* Windows can't open it as media (or has no Media Foundation): err untouched */
  SMP_MF_FAILED = -1,     /* Windows opened it but couldn't decode it (no decoder, no sound, damaged): err says why */
  SMP_MF_STOPPED = -2     /* put refused, or cancel said stop ("cancelled" in err) */
};

/* An MPEG-4 file's edit list (Windows doesn't apply it): where its first sound track's audio really starts, in
   seconds (the coder's start delay, 1024 frames for AAC as ffmpeg and iTunes write it), and how long it plays (0 when
   it doesn't say). 1 when the file has one. */
int smp_mp4_edit(const unsigned char *b, size_t len, double *start, double *dur);

/* path is UTF-8. cancel may be NULL; it is asked between samples. *trimmed is 1 when Windows marked a start delay
   (a first sample before time zero) and it was dropped; the caller then doesn't apply an edit list too. */
int smp_mf_decode(const char *path, unsigned *ch, unsigned *sr, SmpMfPut put, void *ctx,
                  int (*cancel)(void *), void *arg, int *trimmed, char *err, int cap);
