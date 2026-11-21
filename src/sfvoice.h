/* sfvoice.h: one SoundFont voice, all the SF2.04 sound generation for a single region of a note (section 9):
   pitch, 4-point Hermite interpolation of the sample (int16 or float; its head in memory, then its ring when it
   streams), loops and sample modes, the volume and modulation
   envelopes, the two LFOs, the resonant lowpass, attenuation, pan and the effect sends, and the modulators.

   Every smp_sfv_* call is made on the audio thread: none allocates, locks, waits or touches the disk. A voice
   holds its sample (smp_sf_pcm_acquire) from a successful start until it ends or is killed, and releases it
   exactly once then, so a synth whose voices have all ended holds nothing. A voice that plays past its sample's
   head reads the rest from a ring its player's streamer fills (sfstream.c); the streamer holds the sample for
   itself meanwhile. The struct is public so a synth can keep a fixed array of voices. */
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

/* envelope stages, in order: delay, attack, hold, decay, sustain, release, done */
enum { SMP_SFV_DELAY, SMP_SFV_ATTACK };
typedef struct { int stage; double t, val; } SmpSfEnv;           /* t: samples into the stage */
typedef struct { double t, delay, phase, inc, val; } SmpSfLfo;    /* t: samples since the start, up to delay */

typedef struct SmpSfVoice {
  int live;                              /* 1 from a successful start until the voice ends */
  SmpFont *font; const SmpSfRegion *reg; int sample;
  const void *data; int is_float;        /* the held head (int16 or float): the whole sample when it is under 1 s */
  int64_t hf;                            /* frames in data */
  const void *tail; int64_t t0, t1;      /* the post-loop head (the same type as data): sample frames [t0, t1) */
  int key, vel;                          /* as played (for the modulators' key and velocity sources) */
  int pkey;                              /* the key for pitch and the keynum scalings: keynum when set */
  double rate, transpose, srate;         /* output rate, transpose in semitones, the sample's rate */
  int root, correction;
  /* playback, in frames from data[0] */
  int64_t start, end, ls, le;
  int mode, looping, released, ending;
  int looped;                            /* the loop has wrapped at least once (the point before ls is then le-1) */
  double pos;
  /* generators with the modulators added, and the modulators' sums alone */
  double g[SMP_SF_NGEN], mod[SMP_SF_NGEN];
  double chan_tune;                      /* RPN fine and coarse tuning, in cents */
  /* what the generators work out to */
  double cents0, ratio_k, atten_gain, pan_l, pan_r, rev_send, cho_send, q_lin, q_gain;
  double cond_gain;                      /* the region conditions' fixed gain: crossfades, amp_veltrack and a release
                                            trigger's rt_decay, worked out at start (1 for the SF2 family) */
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
  /* SFZ's second filter and EQ bands (cond.xdsp): xn biquads after the first filter, in order. Their targets (xt)
     are worked out at every derive (the start and each controller change); the coefficients (xc) step toward them
     sample by sample (xdc) as the first filter's do; xs is each one's state (x1, x2, y1, y2). */
  int xn; double xt[4][5], xc[4][5], xdc[4][5], xs[4][4];
  /* SFZ's flex envelopes (cond.flex, nfx of them): each one's point it is heading for (pt; npts once past the last),
     held at its sustain point (held), samples into the segment (t), the value it set out from and its value now,
     each point's length in samples (its time and the controllers on it at note-on), and its depths now */
  int nfx, keydelta;                     /* keydelta: SFZ's CC140, set by the player after the start */
  struct { int pt, held; double t, from, val, len[SMP_FLEX_PTS], depth[4], depth2; } fx[SMP_FLEX_MAX];
  double level;                          /* the last envelope level times the attenuation, for stealing */
  /* kill: a linear fade */
  int killing; double fade, fade_step;
  /* streaming: the voice plays past its head. ss and ring are its slot in the player's streamer (NULL when it has
     none: then it plays its head and falls silent). Path frames count along the loop: frame i of the current lap is
     path frame i + laps * L. */
  int streaming, loopy;
  struct SfSlot *ss; float *ring; LONG sgen;
  int64_t L, laps, rel_w;                /* rel_w: the laps when a loop-until-release let go, else -1 */
  int64_t avail;                         /* path frames in the ring, as last read */
  int64_t roff;                          /* laps * L, for a stretch read from the ring */
  double ug;                             /* underrun gain: fades out before the ring runs dry and back in after */
  int underrun;                          /* the ring ran dry since the player last looked (it clears this): 1, or 2
                                            when the streamer hadn't delivered the voice's first read yet (an onset) */
  int64_t fed0; int fed;                 /* fed: the ring has had frames past path frame fed0 (the first read came) */
  int64_t wait;                          /* output frames before it starts (SFZ's delay and delay_random) */
  double rnd_cents;                      /* SFZ's pitch_random draw for this note */
  /* Decent Sampler's knobs: the region's layer gain (linear) and tuning (cents) as the player keeps them now, or
     NULL; set after the start (then smp_sfv_update), read at every derive */
  const float *lgain, *lcents;
  /* and its layer's live values (sfknob.h's sf_knobs_levels_ex lfx: its group filters' and amplitude envelope's)
     and the preset's modulators' values (kmod, SF_MP_N each), or NULL */
  const float *lx, *lmod;
  double q_db1;                          /* the first filter's resonance (dB) as last worked out */
  int x2live; double x2cents, x2reso;    /* the second filter moves at every update (a knob or an envelope on it):
                                            its cents and resonance from the controllers, as x_targets left them */
} SmpSfVoice;

/* starts the voice for region r of a note. Returns 0 (and holds nothing) when the sample isn't loaded. st and slot:
   the player's streamer and the voice's slot in it, for samples longer than their head (st may be NULL). held: for
   a release trigger, the seconds its note was held (its level falls rt_decay dB per second of that); negative for
   any other voice. */
struct SfStream;
int smp_sfv_start(SmpSfVoice *v, SmpFont *f, const SmpSfRegion *r, int key, int vel, const SmpSfChan *c,
                  double rate, double transpose, struct SfStream *st, int slot, double held);
int smp_sfv_xf_silent(const SmpSfRegion *r, int key, int vel);   /* 1 when the region's crossfades put this note at
                                                          0 (the player doesn't start such a voice) */
int smp_sfv_needs_ring_now(const SmpSfVoice *v);       /* the voice's first frames are past its head (an offline
                                                          render waits for the streamer then) */
int smp_sfv_ring_low(SmpSfVoice *v);                   /* the voice streams and its ring holds less than half a ring
                                                          ahead of it (an offline render tops the rings up then) */
void smp_sfv_release(SmpSfVoice *v);                   /* note-off: the envelopes' release stages (not for a
                                                          one-shot region, which plays its sample to the end) */
void smp_sfv_off(SmpSfVoice *v);                       /* the release stages even for a one-shot (an off_by stop) */
void smp_sfv_kill(SmpSfVoice *v, double fade_ms);      /* fades out over fade_ms; 0 or less ends it at once */
void smp_sfv_update(SmpSfVoice *v, const SmpSfChan *c);   /* a controller changed: the modulators again */
/* just after the start: the voice's Decent Sampler layer (its gain and tuning, its group filters' values, the
   preset's modulators' values; see above), its envelopes started again with them and its values worked out */
void smp_sfv_set_layer(SmpSfVoice *v, const float *lgain, const float *lcents, const float *lx, const float *lmod,
                       const SmpSfChan *c);
/* adds n frames into l and r (and the effect sends into rev and cho, either may be NULL). Returns 0 once the
   voice has ended, and on every call after. */
int smp_sfv_render(SmpSfVoice *v, float *l, float *r, float *rev, float *cho, int n);
double smp_sfv_level(const SmpSfVoice *v);              /* envelope level times attenuation, 0 when ended */

#endif
