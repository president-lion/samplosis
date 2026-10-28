/* sfvoice.h: one SoundFont voice, all the SF2.04 sound generation for a single region of a note (section 9):
   pitch, 4-point Hermite interpolation of the int16 sample, loops and sample modes, the volume and modulation
   envelopes, the two LFOs, the resonant lowpass, attenuation, pan and the effect sends, and the modulators.

   Every smp_sfv_* call is made on the audio thread: none allocates, locks, waits or touches the disk. A voice
   holds its sample (smp_sf_pcm_acquire) from a successful start until it ends or is killed, and releases it
   exactly once then, so a synth whose voices have all ended holds nothing. The struct is public so a synth can
   keep a fixed array of voices. */
#ifndef SFVOICE_H
#define SFVOICE_H
#include <stdint.h>
#include "samplosis.h"

/* a MIDI channel's state as the modulators see it */
typedef struct SmpSfChan {
  uint8_t cc[128];
  int bend /* 0..16383, 8192 is the centre */, bend_range_cents, pressure, poly[128];
  int fine_cents, coarse_semis;          /* RPN 1 and RPN 2 */
} SmpSfChan;

/* samples between control updates (envelopes, LFOs, modulators, filter coefficients) */
#define SMP_SFV_CTL 32

typedef struct { int stage; double t, val; } SmpSfEnv;           /* t: samples into the stage */
typedef struct { double t, delay, phase, inc, val; } SmpSfLfo;    /* t: samples since the start, up to delay */

typedef struct SmpSfVoice {
  int live;                              /* 1 from a successful start until the voice ends */
  SmpFont *font; const SmpSfRegion *reg; int sample;
  const int16_t *data;                   /* the held sample */
  int key, vel;                          /* as played (for the modulators' key and velocity sources) */
  int pkey;                              /* the key for pitch and the keynum scalings: keynum when set */
  double rate, transpose, srate;         /* output rate, transpose in semitones, the sample's rate */
  int root, correction;
  /* playback, in frames from data[0] */
  int64_t start, end, ls, le;
  int mode, looping, released, ending;
  double pos;
  /* generators with the modulators added, and the modulators' sums alone */
  double g[SMP_SF_NGEN], mod[SMP_SF_NGEN];
  double chan_tune;                      /* RPN fine and coarse tuning, in cents */
  /* what the generators work out to */
  double cents0, ratio_k, atten_gain, pan_l, pan_r, rev_send, cho_send, q_lin, q_gain;
  double v_delay, v_attack, v_hold, v_decay, v_sus_db, v_release;       /* samples, and dB of attenuation */
  double m_delay, m_attack, m_hold, m_decay, m_sus, m_release;          /* samples, and the 0..1 level */
  SmpSfEnv venv, menv;
  SmpSfLfo modlfo, viblfo;
  /* control rate: current values and their per-sample steps toward the next update */
  int ctl_left, primed;
  double ratio, d_ratio;
  double gl, gr, grev, gcho, d_gl, d_gr, d_grev, d_gcho;
  double b0, b1, b2, a1, a2, d_b0, d_b1, d_b2, d_a1, d_a2;
  int filt_on;                           /* 0 while the filter is a straight wire now and at the next update */
  double x1, x2, y1, y2;
  double level;                          /* the last envelope level times the attenuation, for stealing */
  /* kill: a linear fade */
  int killing; double fade, fade_step;
} SmpSfVoice;

/* starts the voice for region r of a note. Returns 0 (and holds nothing) when the sample isn't loaded. */
int smp_sfv_start(SmpSfVoice *v, SmpFont *f, const SmpSfRegion *r, int key, int vel, const SmpSfChan *c,
                  double rate, double transpose);
void smp_sfv_release(SmpSfVoice *v);                   /* note-off: the envelopes' release stages */
void smp_sfv_kill(SmpSfVoice *v, double fade_ms);      /* fades out over fade_ms; 0 or less ends it at once */
void smp_sfv_update(SmpSfVoice *v, const SmpSfChan *c);   /* a controller changed: the modulators again */
/* adds n frames into l and r (and the effect sends into rev and cho, either may be NULL). Returns 0 once the
   voice has ended, and on every call after. */
int smp_sfv_render(SmpSfVoice *v, float *l, float *r, float *rev, float *cho, int n);
double smp_sfv_level(const SmpSfVoice *v);              /* envelope level times attenuation, 0 when ended */

#endif
