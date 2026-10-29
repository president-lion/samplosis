/* sffx.h: the SoundFont player's reverb and chorus. See sffx.c for the design. */
#ifndef SFFX_H
#define SFFX_H
#include "samplosis.h"

typedef struct SmpSfFx SmpSfFx;

/* All memory is allocated here, sized for the rate (up to 192 kHz and beyond). NULL when out of memory. */
SMP_API SmpSfFx *smp_sffx_new(double rate);
SMP_API void smp_sffx_free(SmpSfFx *fx);
/* every delay line and filter back to silence (audio thread) */
SMP_API void smp_sffx_clear(SmpSfFx *fx);
/* the tails fade out over 10 ms in the next process calls, then everything is cleared (at once when nothing
   sounds). Audio thread; allocates nothing. For a host's reset, so stop, seek and loop don't click */
SMP_API void smp_sffx_fade_out(SmpSfFx *fx);
/* effects on or off. Turning them off clears the tails at once, so turning them on later replays nothing. */
SMP_API void smp_sffx_enable(SmpSfFx *fx, int on);
/* Adds the reverb and chorus of the two mono sends (rev_in, cho_in: n frames each, either may be NULL) into l and r.
   Allocates nothing and takes no locks. When the effects are off it returns at once and adds nothing. */
SMP_API void smp_sffx_process(SmpSfFx *fx, const float *rev_in, const float *cho_in, float *l, float *r, int n);

#endif
