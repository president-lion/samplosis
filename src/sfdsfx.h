/* sfdsfx.h: a Decent Sampler preset's effects chain (its <effects>, sfknob.h's SfFxDesc), played on the preset's
   own stereo bus. See sfdsfx.c for the design. */
#ifndef SFDSFX_H
#define SFDSFX_H
#include "samplosis.h"
#include "sfknob.h"

typedef struct SfDsChain SfDsChain;

/* main thread: everything allocated here, sized for the rate (the delay holds 4 seconds); NULL when out of memory */
SfDsChain *sf_chain_new(const SfFxDesc *fx, int nfx, double rate, int max_block);
void sf_chain_free(SfDsChain *c);
/* audio thread (as the rest): effect fx's parameter SF_FXP_* to v (Hz, Q, 0..1, seconds, or the delay time as a
   musical-time index when the effect's delayTimeFormat is musical_time); it moves there without zipper noise */
void sf_chain_set(SfDsChain *c, int fx, int param, float v);
void sf_chain_clear(SfDsChain *c);           /* every tail to silence */
/* the chain over n frames of l and r, in place, at the host's tempo bpm. effects_on 0 bypasses the reverbs and
   delays (the filters stay: they are the sound's tone); turning it off clears their tails. Allocates nothing. */
void sf_chain_process(SfDsChain *c, float *l, float *r, int n, double bpm, int effects_on);
/* 1 when the chain has nothing that changes the sound now (every filter a wire, every reverb and delay silent or
   bypassed), so the caller may skip it */
int sf_chain_idle(const SfDsChain *c, int effects_on);

/* tests: desc is one effect a line ("lp freq=1000 q=0.7", "reverb room=0.9 damp=0.1 wet=0.5", "delay time=13
   musical=1 feedback=0.5 offset=0.01 wet=1"; an optional "fx N" first is skipped), sets is lines of "N param value"
   applied first and "effects 0" to bypass; in is mono, out stereo. 0 ok. */
SMP_API int smp_dsfx_test(const char *desc, const float *in, float *out_l, float *out_r, int n, double rate, double bpm,
                          const char *sets);

#endif
