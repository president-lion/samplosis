/* sffx.c: the SoundFont player's reverb and chorus, tuned to sound close to FluidSynth's defaults.

   Reverb: Jezar's Freeverb (public domain). The mono send goes into 8 parallel lowpass-feedback comb filters,
   then through 4 allpass filters in series, once for the left side and once for the right. The right side's
   delay lengths are 23 samples (at 44.1 kHz) longer, which makes the tail stereo. Tunings are Jezar's, scaled by
   rate / 44100. Settings: room 0.2 (comb feedback 0.2 * 0.28 + 0.7), damp 0.0, width 0.5, level 0.9. As in
   FluidSynth the wet gain is level * 3, and the two sides are mixed with wet1 = gain * (width / 2 + 0.5) and
   wet2 = gain * (1 - width) / 2. The input gain is Freeverb's fixed 0.015.

   Chorus: the mono send goes into one delay line with 3 taps. Each tap's delay is 10 ms plus a sine of 8 ms
   depth (FluidSynth's default) at 0.3 Hz, the three sines 120 degrees apart, read with linear interpolation. The
   centre is 2 ms past the depth, so a tap never reads ahead of the write position. Tap 0 goes to the left,
   tap 1 to the right and tap 2 to the middle. Each tap has gain level / 3, level 2.0.

   The two effects are added to the left and right output in parallel. Everything is sized in smp_sffx_new, so
   process allocates nothing. Tiny values in the feedback paths are flushed to zero, so a long silence never
   turns into slow denormal arithmetic. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "sffx.h"

#define NCOMB 8
#define NALL 4
#define SPREAD 23                       /* the right side's extra delay, at 44.1 kHz */
#define FIXEDGAIN 0.015f
#define ROOM 0.2
#define DAMP 0.0
#define WIDTH 0.5
#define REV_LEVEL 0.9
#define SCALE_WET 3.0
#define ALL_FEEDBACK 0.5f
#define CHO_VOICES 3
#define CHO_DEPTH_MS 8.0
#define CHO_DELAY_MS 10.0
#define CHO_HZ 0.3
#define CHO_LEVEL 2.0
#define FLUSH 1e-18f
#define FADE_MS 10.0                    /* smp_sffx_fade_out: the tails fade out over this, then clear */

static const int comb_tune[NCOMB] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const int all_tune[NALL] = {556, 441, 341, 225};

typedef struct { float *buf; int n, i; float store; } Comb;
typedef struct { float *buf; int n, i; } Allpass;

struct SmpSfFx {
  double rate;
  int on;
  Comb comb[2][NCOMB];
  Allpass all[2][NALL];
  float feedback, damp1, damp2, wet1, wet2;
  float *cbuf; int cmask, cw;           /* the chorus delay line (a power of two long) and its write position */
  double cs, cc;                        /* the chorus LFO: sine and cosine of its phase */
  double crs, crc;                      /* the rotation per sample */
  float depth, base, gain;              /* in samples; the delay's centre; per-tap gain */
  float *mem;                           /* every buffer above lives in this one block */
  int fade_len, fade_left;              /* fading out: frames in all, frames still to go (0: not fading) */
  float level;                          /* the loudest wet sample of the last block (0: the tails are silent) */
};

static float flush(float x) { return fabsf(x) < FLUSH ? 0.0f : x; }

static int scaled(int n, double rate) {
  int v = (int)floor(n * rate / 44100.0 + 0.5);
  return v < 1 ? 1 : v;
}

SMP_API SmpSfFx *smp_sffx_new(double rate) {
  SmpSfFx *fx; size_t total = 0, off = 0; int side, k, cn;
  double w;
  if (!(rate >= 8000)) return NULL;
  fx = (SmpSfFx *)calloc(1, sizeof *fx);
  if (!fx) return NULL;
  fx->rate = rate; fx->on = 1;
  for (side = 0; side < 2; side++) {
    for (k = 0; k < NCOMB; k++) { fx->comb[side][k].n = scaled(comb_tune[k] + side * SPREAD, rate); total += (size_t)fx->comb[side][k].n; }
    for (k = 0; k < NALL; k++) { fx->all[side][k].n = scaled(all_tune[k] + side * SPREAD, rate); total += (size_t)fx->all[side][k].n; }
  }
  cn = 1;
  while (cn < (int)((CHO_DELAY_MS + CHO_DEPTH_MS + 2.0) * 0.001 * rate) + 4) cn <<= 1;
  fx->cmask = cn - 1;
  fx->mem = (float *)calloc(total + (size_t)cn, sizeof(float));
  if (!fx->mem) { free(fx); return NULL; }
  for (side = 0; side < 2; side++) {
    for (k = 0; k < NCOMB; k++) { fx->comb[side][k].buf = fx->mem + off; off += (size_t)fx->comb[side][k].n; }
    for (k = 0; k < NALL; k++) { fx->all[side][k].buf = fx->mem + off; off += (size_t)fx->all[side][k].n; }
  }
  fx->cbuf = fx->mem + off;
  fx->feedback = (float)(ROOM * 0.28 + 0.7);
  fx->damp1 = (float)DAMP; fx->damp2 = (float)(1.0 - DAMP);
  w = REV_LEVEL * SCALE_WET;
  fx->wet1 = (float)(w * (WIDTH / 2 + 0.5));
  fx->wet2 = (float)(w * ((1 - WIDTH) / 2));
  fx->depth = (float)(CHO_DEPTH_MS * 0.001 * rate);
  fx->base = (float)(CHO_DELAY_MS * 0.001 * rate);
  fx->gain = (float)(CHO_LEVEL / CHO_VOICES);
  fx->crs = sin(2 * 3.14159265358979323846 * CHO_HZ / rate);
  fx->crc = cos(2 * 3.14159265358979323846 * CHO_HZ / rate);
  fx->fade_len = (int)(FADE_MS * 0.001 * rate);
  if (fx->fade_len < 1) fx->fade_len = 1;
  smp_sffx_clear(fx);
  return fx;
}

SMP_API void smp_sffx_free(SmpSfFx *fx) {
  if (!fx) return;
  free(fx->mem); free(fx);
}

SMP_API void smp_sffx_clear(SmpSfFx *fx) {
  int side, k; size_t total = 0;
  if (!fx) return;
  for (side = 0; side < 2; side++) {
    for (k = 0; k < NCOMB; k++) { fx->comb[side][k].i = 0; fx->comb[side][k].store = 0; total += (size_t)fx->comb[side][k].n; }
    for (k = 0; k < NALL; k++) { fx->all[side][k].i = 0; total += (size_t)fx->all[side][k].n; }
  }
  memset(fx->mem, 0, sizeof(float) * (total + (size_t)fx->cmask + 1));
  fx->cw = 0; fx->cs = 0; fx->cc = 1;
  fx->fade_left = 0; fx->level = 0;
}

SMP_API void smp_sffx_fade_out(SmpSfFx *fx) {
  if (!fx) return;
  if (!fx->on || fx->level == 0) smp_sffx_clear(fx);   /* nothing sounds: no fade needed */
  else if (!fx->fade_left) fx->fade_left = fx->fade_len;
}

SMP_API void smp_sffx_enable(SmpSfFx *fx, int on) {
  if (!fx) return;
  on = on ? 1 : 0;
  if (fx->on && !on) smp_sffx_clear(fx);
  fx->on = on;
}

/* one side of the reverb for one input sample */
static float reverb_side(SmpSfFx *fx, int side, float in) {
  float sum = 0; int k;
  for (k = 0; k < NCOMB; k++) {
    Comb *c = &fx->comb[side][k];
    float out = c->buf[c->i];
    c->store = flush(out * fx->damp2 + c->store * fx->damp1);
    c->buf[c->i] = flush(in + c->store * fx->feedback);
    if (++c->i >= c->n) c->i = 0;
    sum += out;
  }
  for (k = 0; k < NALL; k++) {
    Allpass *a = &fx->all[side][k];
    float b = a->buf[a->i], out = b - sum;
    a->buf[a->i] = flush(sum + b * ALL_FEEDBACK);
    if (++a->i >= a->n) a->i = 0;
    sum = out;
  }
  return sum;
}

SMP_API void smp_sffx_process(SmpSfFx *fx, const float *rev_in, const float *cho_in, float *l, float *r, int n) {
  int i;
  const double k1 = -0.5, k2 = 0.86602540378443865;      /* cos and sin of 120 degrees */
  float peak = 0;
  if (!fx || !fx->on || !l || !r || n <= 0) return;
  for (i = 0; i < n; i++) {
    float rin = rev_in ? rev_in[i] * FIXEDGAIN : 0.0f, cin = cho_in ? cho_in[i] : 0.0f;
    float wl, wr, ol, orr, d, a, b, y0 = 0, y1 = 0, y2 = 0; double s0 = fx->cs, c0 = fx->cc, s1, s2;
    int j, p;
    /* reverb */
    wl = reverb_side(fx, 0, rin);
    wr = reverb_side(fx, 1, rin);
    ol = wl * fx->wet1 + wr * fx->wet2;
    orr = wr * fx->wet1 + wl * fx->wet2;
    /* chorus: the three sines 120 degrees apart come from one rotating phasor */
    fx->cbuf[fx->cw] = cin;
    s1 = s0 * k1 + c0 * k2;                              /* sin(t + 120 degrees) */
    s2 = s0 * k1 - c0 * k2;                              /* sin(t - 120 degrees) = sin(t + 240 degrees) */
    for (j = 0; j < CHO_VOICES; j++) {
      double s = j == 0 ? s0 : j == 1 ? s1 : s2;
      float pos = (float)fx->cw - (fx->base + fx->depth * (float)s);
      int ip = (int)floorf(pos); float fr = pos - (float)ip;
      p = ip & fx->cmask;
      a = fx->cbuf[p]; b = fx->cbuf[(p + 1) & fx->cmask];
      d = a + (b - a) * fr;
      if (j == 0) y0 = d; else if (j == 1) y1 = d; else y2 = d;
    }
    ol += (y0 + 0.70710678f * y2) * fx->gain;
    orr += (y1 + 0.70710678f * y2) * fx->gain;
    if (fx->fade_left) { float g = (float)fx->fade_left / (float)fx->fade_len; ol *= g; orr *= g; }
    l[i] += ol; r[i] += orr;
    if (fabsf(ol) > peak) peak = fabsf(ol);
    if (fabsf(orr) > peak) peak = fabsf(orr);
    fx->cw = (fx->cw + 1) & fx->cmask;
    fx->cs = s0 * fx->crc + c0 * fx->crs;
    fx->cc = c0 * fx->crc - s0 * fx->crs;
    if (fx->fade_left && --fx->fade_left == 0) smp_sffx_clear(fx);   /* faded out: the tails are gone */
  }
  fx->level = peak;
  {                                                       /* keep the phasor at length 1 */
    double m = sqrt(fx->cs * fx->cs + fx->cc * fx->cc);
    if (m > 0) { fx->cs /= m; fx->cc /= m; }
  }
}
