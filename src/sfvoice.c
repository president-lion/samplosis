/* sfvoice.c: one SoundFont voice (SF2.04 section 9). See sfvoice.h.

   Every SMP_SFV_CTL samples the voice works out its envelopes, LFOs, pitch, gain and filter coefficients for the
   end of the next stretch, and steps every value linearly toward them sample by sample, so nothing jumps.
   Modulators are worked out at start and when a controller changes (smp_sfv_update), and their sums are added
   to the region's generators. Nothing here allocates, locks or waits, except smp_sf_test_note (main thread). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "sf2.h"
#include "sfvoice.h"
#include "sfstream.h"

#define PI 3.14159265358979323846

/* generator ids (SF2.04 section 8.1.2) */
enum {
  G_START = 0, G_END = 1, G_LOOPSTART = 2, G_LOOPEND = 3, G_STARTCOARSE = 4, G_MODLFO_PITCH = 5,
  G_VIBLFO_PITCH = 6, G_MODENV_PITCH = 7, G_FC = 8, G_Q = 9, G_MODLFO_FC = 10, G_MODENV_FC = 11,
  G_ENDCOARSE = 12, G_MODLFO_VOL = 13, G_CHORUS = 15, G_REVERB = 16, G_PAN = 17,
  G_DELAY_MODLFO = 21, G_FREQ_MODLFO = 22, G_DELAY_VIBLFO = 23, G_FREQ_VIBLFO = 24,
  G_DELAY_MODENV = 25, G_ATTACK_MODENV = 26, G_HOLD_MODENV = 27, G_DECAY_MODENV = 28, G_SUSTAIN_MODENV = 29,
  G_RELEASE_MODENV = 30, G_KEY_MODENV_HOLD = 31, G_KEY_MODENV_DECAY = 32,
  G_DELAY_VOLENV = 33, G_ATTACK_VOLENV = 34, G_HOLD_VOLENV = 35, G_DECAY_VOLENV = 36, G_SUSTAIN_VOLENV = 37,
  G_RELEASE_VOLENV = 38, G_KEY_VOLENV_HOLD = 39, G_KEY_VOLENV_DECAY = 40,
  G_LOOPSTARTCOARSE = 45, G_KEYNUM = 46, G_VELOCITY = 47, G_ATTEN = 48, G_LOOPENDCOARSE = 50,
  G_COARSE = 51, G_FINE = 52, G_MODES = 54, G_SCALE = 56, G_ROOTKEY = 58,
  G_PITCH = 59      /* "initial pitch": no generator, only the default pitch wheel modulator's destination */
};

/* envelope stages (the first two are SMP_SFV_DELAY and SMP_SFV_ATTACK in sfvoice.h, which the player reads) */
enum { E_DELAY, E_ATTACK, E_HOLD, E_DECAY, E_SUSTAIN, E_RELEASE, E_DONE };

/* the EMU factor on initialAttenuation that FluidSynth applies to the generator (not to modulators) */
#define ATTEN_FACTOR 0.4
/* The volume envelope's rate, as FluidSynth has it (measured against it in Task 7; the spec's "time for a 100 %
   change" is unclear about the range): decay and release fall 96 dB in their time, and the envelope is done
   96 dB down. The sustain level is the spec's exact centibels. */
#define ENV_DB 96.0
#define FLOOR_DB (-ENV_DB)

static double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

/* timecents to samples, limited to lo..hi timecents: -12000 is the shortest the spec allows (about 1 ms);
   the upper limit is 5000 for delays and holds (after the key scaling) and 8000 for the rest, as the spec's
   ranges and FluidSynth have it */
static double tc_samples(double tc, double lo, double hi, double rate) { return rate * pow(2.0, clampd(tc, lo, hi) / 1200.0); }

/* absolute cents to Hz */
static double abs_hz(double cents) { return 8.176 * pow(2.0, cents / 1200.0); }

/* ---- modulators (SF2.04 section 8.2) ---- */

/* The concave and convex curves, as SF2.04 section 8.2.4 draws them, on 128 steps the way FluidSynth tabulates
   them (measured against it in Task 7): step i of 0..127 is concave(i) = -20/96 * log10(((127 - i) / 127)^2),
   the curve whose 960 cB make an amplitude of ((127 - i) / 127)^2, and convex(i) = 1 - concave(127 - i); step 0
   is 0 and step 127 is 1. Between steps the value is interpolated. So a 7-bit controller at v through the
   default concave, negative CC7 modulator gives exactly an amplitude of (v / 127)^2, full at 127. */
static double curve_step(int i, int cvx) {
  if (i <= 0) return 0;
  if (i >= 127) return 1;
  return cvx ? 1 + 5.0 / 12.0 * log10(i / 127.0) : -5.0 / 12.0 * log10((127 - i) / 127.0);
}
static double curve(double x, int cvx) {       /* x: a fractional step */
  int i;
  if (x < 0) return 0;
  i = (int)x;
  if (i >= 127) return 1;
  return curve_step(i, cvx) + (curve_step(i + 1, cvx) - curve_step(i, cvx)) * (x - i);
}

typedef struct { const SmpSfChan *c; int key, vel, poly_key; } ModCtx;

/* a modulator source, mapped through its curve, polarity and direction (the four flags of section 8.2.1).
   The mapping is FluidSynth's (measured in Task 7), which reads the spec's 127/128 ranges literally:
   - a source counts in 128ths (the pitch wheel in 16384ths), so 64 is the exact middle;
   - the negative direction is (127 - v) / 128, so it never reaches 1 either;
   - the curves take 128 times that as their step, and stop at 127/128;
   - bipolar is 2x - 1, except that the top value stays as it is.
   The pitch wheel sensitivity is the bend range in cents / 12700, so the default pitch wheel modulator's
   12700 gives the range exactly. "No controller" is 1, as the spec says and FluidSynth 2.6 does. */
static double source(unsigned s, const ModCtx *m) {
  unsigned idx = s & 127, type = s >> 10, bip = (s >> 9) & 1, neg = (s >> 8) & 1;
  double v, range = 128, top, x;
  if (s & 0x80) v = m->c->cc[idx];
  else switch (idx) {
    case 0: return 1.0;
    case 2: v = m->vel; break;
    case 3: v = m->key; break;
    case 10: v = m->c->poly[m->poly_key & 127]; break;
    case 13: v = m->c->pressure; break;
    case 14: v = m->c->bend; range = 16384; break;
    case 16: v = m->c->bend_range_cents * 128.0 / 12700.0; break;
    default: return 0;
  }
  top = (range - 1) / range;
  x = neg ? top - v / range : v / range;
  if (!bip) {
    switch (type) {
      case 1: return fmin(curve(128 * x, 0), top);
      case 2: return fmin(curve(128 * x, 1), top);
      case 3: return x >= 0.5 ? 1 : 0;
      default: return x;
    }
  }
  if (x != top) x = 2 * x - 1;     /* bipolar: the curve runs both ways out from the middle */
  switch (type) {
    case 1: return x >= 0 ? fmin(curve(128 * x, 0), top) : -curve(-128 * x, 0);
    case 2: return x >= 0 ? fmin(curve(128 * x, 1), top) : -curve(-128 * x, 1);
    case 3: return x >= 0 ? 1 : -1;
    default: return x;
  }
}

/* the default velocity-to-attenuation modulator (SF2.04 section 8.4.1), which a region's amp_veltrack replaces */
static int is_vel_atten(const SmpSfMod *d) { return d->src == 0x0502 && d->dest == G_ATTEN && d->amt_src == 0; }

static void add_mods(double *sum, const SmpSfMod *list, int n, const ModCtx *m, int no_vel) {
  int i;
  for (i = 0; i < n; i++) {
    const SmpSfMod *d = &list[i];
    double y;
    if (d->dest >= SMP_SF_NGEN) continue;     /* the parser drops these; be safe */
    if (no_vel && is_vel_atten(d)) continue;
    y = d->amount * source(d->src, m) * source(d->amt_src, m);
    if (d->trans == 2) y = fabs(y);
    sum[d->dest] += y;
  }
}

/* the modulators of both lists, then what the generators work out to */
static void derive(SmpSfVoice *v, const SmpSfChan *c) {
  const SmpSfRegion *r = v->reg;
  ModCtx m;
  double *g = v->g, rate = v->rate, q_db, peak;
  int i, k;
  m.c = c; m.key = v->pkey; m.vel = v->vel; m.poly_key = v->key;   /* the key source is keynum when set, as in FluidSynth */
  memset(v->mod, 0, sizeof v->mod);
  add_mods(v->mod, r->imod, r->nimod, &m, !isnan(r->cond.amp_veltrack));   /* set: the default is replaced */
  add_mods(v->mod, r->pmod, r->npmod, &m, !isnan(r->cond.amp_veltrack));
  for (i = 0; i < SMP_SF_NGEN; i++) g[i] = r->gen[i] + v->mod[i];

  /* Pitch, in cents from the sample as recorded. The channel's pitch bend is not added here: it comes through
     the default pitch wheel modulator (to G_PITCH, 12700 cents scaled by the pitch wheel sensitivity, which is
     the channel's bend range). RPN fine and coarse tuning are added directly. */
  v->chan_tune = c->fine_cents + 100.0 * c->coarse_semis;
  v->cents0 = (v->pkey - v->root) * g[G_SCALE] + 100.0 * g[G_COARSE] + g[G_FINE] + v->correction +
              g[G_PITCH] + v->chan_tune + 100.0 * v->transpose;
  v->ratio_k = v->srate / rate;

  /* gain: initialAttenuation times the EMU factor, plus the modulators' attenuation (velocity, CC7, CC11) */
  v->atten_gain = pow(10.0, -clampd(ATTEN_FACTOR * r->gen[G_ATTEN] + v->mod[G_ATTEN], 0, 1440) / 200.0) * v->cond_gain;
  {
    double th = (clampd(g[G_PAN], -500, 500) + 500) / 1000.0 * (PI / 2);   /* constant power, -3 dB each at 0 */
    v->pan_l = cos(th); v->pan_r = sin(th);
    if (v->pan_r < 1e-12) v->pan_r = 0;
    if (v->pan_l < 1e-12) v->pan_l = 0;
  }
  v->rev_send = clampd(g[G_REVERB], 0, 1000) / 1000.0;
  v->cho_send = clampd(g[G_CHORUS], 0, 1000) / 1000.0;

  /* The filter's Q, as SF2.04 defines initialFilterQ: the height in cB of the resonance above the DC gain, and 0
     or less is not resonant. Q is chosen so the response's peak is exactly that high: for a 2-pole lowpass the
     peak is M = Q / sqrt(1 - 1/(4 Q^2)), so Q^2 = (M^2 + M sqrt(M^2 - 1)) / 2. At 0 cB that is 1/sqrt(2), a
     Butterworth with no peak. As SF2.01 asks, the gain drops by half the peak in dB, a factor 1/sqrt(M): at Q 100
     the DC is -5 dB and the peak +5 dB. (FluidSynth reads it differently, Q cB less 3.01 dB as a linear Q with
     1/sqrt(Q) gain: 7.2 dB of resonance at Q 100 and +1.5 dB at Q 0. The spec's text is clear, so it is followed.) */
  q_db = clampd(g[G_Q], 0, 960) / 10.0;
  peak = pow(10.0, q_db / 20.0);
  v->q_lin = sqrt((peak * peak + peak * sqrt(peak * peak - 1)) / 2);
  v->q_gain = r->cond.fil_unity ? 1.0 : 1.0 / sqrt(peak);   /* SFZ: the passband stays at unity, the peak at +Q */

  /* LFOs */
  v->modlfo.delay = tc_samples(g[G_DELAY_MODLFO], -12000, 5000, rate);
  v->modlfo.inc = abs_hz(clampd(g[G_FREQ_MODLFO], -16000, 4500)) / rate;
  v->viblfo.delay = tc_samples(g[G_DELAY_VIBLFO], -12000, 5000, rate);
  v->viblfo.inc = abs_hz(clampd(g[G_FREQ_VIBLFO], -16000, 4500)) / rate;

  /* Envelopes; hold and decay scale with the key, (60 - key) * value timecents. The volume envelope's release
     is at least -7200 timecents (16 ms), as in FluidSynth, so a note-off never clicks. Its sustain is exact
     centibels, as the spec says (200 cB is 20 dB down); at or past ENV_DB the voice is done. */
  k = 60 - v->pkey;
  v->v_delay = tc_samples(g[G_DELAY_VOLENV], -12000, 5000, rate);
  v->v_attack = tc_samples(g[G_ATTACK_VOLENV], -12000, 8000, rate);
  v->v_hold = tc_samples(g[G_HOLD_VOLENV] + k * g[G_KEY_VOLENV_HOLD], -12000, 5000, rate);
  v->v_decay = tc_samples(g[G_DECAY_VOLENV] + k * g[G_KEY_VOLENV_DECAY], -12000, 8000, rate);
  v->v_sus_db = -clampd(g[G_SUSTAIN_VOLENV], 0, 1440) / 10.0;
  v->v_release = tc_samples(g[G_RELEASE_VOLENV], -7200, 8000, rate);
  v->m_delay = tc_samples(g[G_DELAY_MODENV], -12000, 5000, rate);
  v->m_attack = tc_samples(g[G_ATTACK_MODENV], -12000, 8000, rate);
  v->m_hold = tc_samples(g[G_HOLD_MODENV] + k * g[G_KEY_MODENV_HOLD], -12000, 5000, rate);
  v->m_decay = tc_samples(g[G_DECAY_MODENV] + k * g[G_KEY_MODENV_DECAY], -12000, 8000, rate);
  v->m_sus = 1 - clampd(g[G_SUSTAIN_MODENV], 0, 1000) / 1000.0;
  v->m_release = tc_samples(g[G_RELEASE_MODENV], -12000, 8000, rate);
}

/* ---- envelopes and LFOs, stepped n samples at a time ---- */

/* The volume envelope. Delay and attack keep val as an amplitude (the attack is linear in amplitude); from the
   hold on, val is in dB. Decay and release fall ENV_DB (96 dB) per their time (linear in dB), the decay down to
   the sustain level, the release to -96 dB, where the envelope is done. */
static void venv_step(SmpSfVoice *v, double n) {
  SmpSfEnv *e = &v->venv;
  while (n > 0 && e->stage != E_DONE) {
    double d;
    switch (e->stage) {
      case E_DELAY:
        d = v->v_delay - e->t;
        if (n < d) { e->t += n; n = 0; } else { n -= d > 0 ? d : 0; e->stage = E_ATTACK; e->t = 0; e->val = 0; }
        break;
      case E_ATTACK:
        d = (1 - e->val) * v->v_attack;
        if (n < d) { e->val += n / v->v_attack; n = 0; } else { n -= d > 0 ? d : 0; e->stage = E_HOLD; e->t = 0; e->val = 0; }
        break;
      case E_HOLD:
        d = v->v_hold - e->t;
        if (n < d) { e->t += n; n = 0; } else { n -= d > 0 ? d : 0; e->stage = E_DECAY; e->t = 0; e->val = 0; }
        break;
      case E_DECAY: {
        double step = ENV_DB / v->v_decay, target = v->v_sus_db;
        d = (e->val - target) / step;
        if (n < d) { e->val -= n * step; n = 0; } else { n -= d > 0 ? d : 0; e->val = target; e->stage = E_SUSTAIN; }
        if (e->val <= FLOOR_DB) e->stage = E_DONE;   /* a silent sustain: nothing more to hear */
        break;
      }
      case E_SUSTAIN:
        e->val = v->v_sus_db;
        if (e->val <= FLOOR_DB) e->stage = E_DONE;
        n = 0;
        break;
      case E_RELEASE:
        e->val -= n * ENV_DB / v->v_release;
        if (e->val <= FLOOR_DB) { e->val = FLOOR_DB; e->stage = E_DONE; }
        n = 0;
        break;
    }
  }
}

static double venv_amp(const SmpSfEnv *e) {
  switch (e->stage) {
    case E_DELAY: return 0;
    case E_ATTACK: return e->val;
    case E_HOLD: return 1;
    case E_DONE: return 0;
    default: return e->val <= FLOOR_DB ? 0 : pow(10.0, e->val / 20.0);
  }
}

/* the modulation envelope: the same stages on a 0..1 scale, linear throughout; its attack is heard through the
   convex curve (menv_out), as FluidSynth does */
static void menv_step(SmpSfVoice *v, double n) {
  SmpSfEnv *e = &v->menv;
  while (n > 0 && e->stage != E_DONE) {
    double d;
    switch (e->stage) {
      case E_DELAY:
        d = v->m_delay - e->t;
        if (n < d) { e->t += n; n = 0; } else { n -= d > 0 ? d : 0; e->stage = E_ATTACK; e->val = 0; }
        break;
      case E_ATTACK:
        d = (1 - e->val) * v->m_attack;
        if (n < d) { e->val += n / v->m_attack; n = 0; } else { n -= d > 0 ? d : 0; e->stage = E_HOLD; e->t = 0; e->val = 1; }
        break;
      case E_HOLD:
        d = v->m_hold - e->t;
        if (n < d) { e->t += n; n = 0; } else { n -= d > 0 ? d : 0; e->stage = E_DECAY; }
        break;
      case E_DECAY:
        d = (e->val - v->m_sus) * v->m_decay;
        if (n < d) { e->val -= n / v->m_decay; n = 0; } else { n -= d > 0 ? d : 0; e->val = v->m_sus; e->stage = E_SUSTAIN; }
        break;
      case E_SUSTAIN:
        e->val = v->m_sus; n = 0;
        break;
      case E_RELEASE:
        e->val -= n / v->m_release;
        if (e->val <= 0) { e->val = 0; e->stage = E_DONE; }
        n = 0;
        break;
    }
  }
}

/* a triangle from 0 up to 1, down to -1 and back, after its delay */
static void lfo_step(SmpSfLfo *o, double n) {
  double p;
  if (o->t < o->delay) {
    double d = o->delay - o->t;
    if (n <= d) { o->t += n; o->val = 0; return; }
    o->t = o->delay; n -= d;
  }
  o->phase += o->inc * n;
  o->phase -= floor(o->phase);
  p = o->phase;
  o->val = p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
}

/* ---- the control update ---- */

/* the modulation envelope's value as it reaches pitch and cutoff */
static double menv_out(const SmpSfEnv *e) { return e->stage == E_ATTACK ? curve(127 * e->val, 1) : e->val; }

/* The cutoff is kept within the spec's 1500 to 13500 cents and under 0.45 times the output rate. Fully open
   (13500 cents or more) with Q 0 the lowpass is a straight wire, so it is transparent and skipped. The other kinds
   (SFZ's fil_type: highpass, bandpass and notch, from RBJ's cookbook like the lowpass) are never a wire; a loader
   sets them only with a cutoff. The highpass is lowered by q_gain as the lowpass is; the bandpass peaks at 0 dB
   (RBJ's constant 0 dB peak form) and the notch passes 0 dB away from its cutoff, both with the same Q. */
static void filter_target(const SmpSfVoice *v, double fc_cents, double *c) {
  double hz, w, cs, alpha, a0;
  int kind = v->reg->cond.fil_type;
  if (kind == SMP_FIL_LP && fc_cents >= 13500 && v->g[G_Q] <= 0) {
    c[0] = 1; c[1] = c[2] = c[3] = c[4] = 0;
    return;
  }
  hz = clampd(abs_hz(clampd(fc_cents, 1500, 13500)), 5, 0.45 * v->rate);
  w = 2 * PI * hz / v->rate;
  cs = cos(w);
  alpha = sin(w) / (2 * v->q_lin);
  a0 = 1 + alpha;
  switch (kind) {
    case SMP_FIL_HP:
      c[0] = (1 + cs) / 2 / a0 * v->q_gain;
      c[1] = -(1 + cs) / a0 * v->q_gain;
      c[2] = c[0];
      break;
    case SMP_FIL_BP:
      c[0] = alpha / a0; c[1] = 0; c[2] = -alpha / a0;
      break;
    case SMP_FIL_BR:
      c[0] = 1 / a0; c[1] = -2 * cs / a0; c[2] = c[0];
      break;
    default:
      c[0] = (1 - cs) / 2 / a0 * v->q_gain;   /* RBJ's lowpass, the DC gain lowered by q_gain */
      c[1] = (1 - cs) / a0 * v->q_gain;
      c[2] = c[0];
      break;
  }
  c[3] = -2 * cs / a0;
  c[4] = (1 - alpha) / a0;
}

static void control(SmpSfVoice *v) {
  const double inv = 1.0 / SMP_SFV_CTL, *g = v->g;
  double amp, cents, ratio, gain, c[5], me;
  int wire;
  venv_step(v, SMP_SFV_CTL);
  menv_step(v, SMP_SFV_CTL);
  lfo_step(&v->modlfo, SMP_SFV_CTL);
  lfo_step(&v->viblfo, SMP_SFV_CTL);
  if (v->venv.stage == E_DONE) v->ending = 1;    /* this stretch fades to 0, then the voice ends */

  me = menv_out(&v->menv);
  cents = v->cents0 + me * g[G_MODENV_PITCH] + v->modlfo.val * g[G_MODLFO_PITCH] +
          v->viblfo.val * g[G_VIBLFO_PITCH];
  ratio = pow(2.0, cents / 1200.0) * v->ratio_k;
  amp = venv_amp(&v->venv);
  gain = amp * v->atten_gain * pow(10.0, v->modlfo.val * g[G_MODLFO_VOL] / 200.0);
  v->level = amp * v->atten_gain;
  filter_target(v, g[G_FC] + me * g[G_MODENV_FC] + v->modlfo.val * g[G_MODLFO_FC], c);
  wire = c[0] == 1 && c[1] == 0 && c[2] == 0 && c[3] == 0 && c[4] == 0;
  if (wire && fabs(v->b0 - 1) < 1e-9 && fabs(v->b1) < 1e-9 && fabs(v->b2) < 1e-9 && fabs(v->a1) < 1e-9 &&
      fabs(v->a2) < 1e-9) {       /* stepped back to a wire: snap to it exactly, so the filter can switch off */
    v->b0 = 1; v->b1 = v->b2 = v->a1 = v->a2 = 0;
  }

  if (!v->primed) {               /* the first update: pitch and filter start where they are, gain from 0 */
    v->primed = 1;
    v->ratio = ratio;
    v->b0 = c[0]; v->b1 = c[1]; v->b2 = c[2]; v->a1 = c[3]; v->a2 = c[4];
    v->gl = v->gr = v->grev = v->gcho = 0;
  }
  v->d_ratio = (ratio - v->ratio) * inv;
  v->d_gl = (gain * v->pan_l - v->gl) * inv;
  v->d_gr = (gain * v->pan_r - v->gr) * inv;
  v->d_grev = (gain * v->rev_send - v->grev) * inv;
  v->d_gcho = (gain * v->cho_send - v->gcho) * inv;
  v->d_b0 = (c[0] - v->b0) * inv; v->d_b1 = (c[1] - v->b1) * inv; v->d_b2 = (c[2] - v->b2) * inv;
  v->d_a1 = (c[3] - v->a1) * inv; v->d_a2 = (c[4] - v->a2) * inv;
  v->filt_on = !(wire && v->b0 == 1 && v->b1 == 0 && v->b2 == 0 && v->a1 == 0 && v->a2 == 0);
  if (!v->filt_on) v->x1 = v->x2 = v->y1 = v->y2 = 0;
}

/* ---- the region conditions' gain ---- */

/* A crossfade's position, 0..1, for value x: fading in over lo..hi it is 0 at or below lo and 1 at or above hi
   (hi is checked first, so lo == hi is a plain step); fading out, 1 at or below lo and 0 at or above hi. As sfizz
   and sfzformat.com's xfin_ and xfout_ opcodes have it, linear between. */
static double xf_in(int x, int lo, int hi) { return x >= hi ? 1 : x <= lo ? 0 : (double)(x - lo) / (hi - lo); }
static double xf_out(int x, int lo, int hi) { return x <= lo ? 1 : x >= hi ? 0 : (double)(hi - x) / (hi - lo); }

/* The fixed gain of a region's conditions, as an amplitude factor:
   - crossfades by key (when set) and velocity: the product of the fade-in and fade-out positions, through
     xf_curve: gain is the position itself, power its square root (so two regions fading across each other keep
     the same power: at the midpoint each is at sqrt(0.5), -3 dB);
   - amp_veltrack (when not NaN; then the default velocity modulator is skipped): the SFZ default velocity curve,
     amplitude c = (vel / 127)^2, tracked by t = amp_veltrack / 100. For t >= 0, gain = 1 - t (1 - c) (sfizz's
     reading): 100 % is c itself (40 log10(vel / 127) dB), 0 % is flat. For t < 0 the curve turns over, loudest
     at the softest velocity: gain = 1 + t c, so -100 % is 1 - c (silent at 127) and the gain is continuous through
     0 % (sfzformat.com only says negative values invert the tracking; this is the form that meets 0 % smoothly);
   - a release trigger's rt_decay: -rt_decay dB per second the note was held;
   - a release trigger's rt_linear (GigaStudio's): 1 - rt_linear x the seconds held, never below 0;
   - amp_velcurve (SFZ's amp_velcurve_N), when set, is the curve c instead of (vel / 127)^2;
   - gain: a plain factor (SFZ's volume above 0 dB, which initialAttenuation can't say). */
static double xf_pos(const SmpSfRegion *r, int key, int vel) {
  const SmpSfCond *c = &r->cond;
  double x = 1;
  if (c->xfin_lokey >= 0 && c->xfin_hikey >= 0) x *= xf_in(key, c->xfin_lokey, c->xfin_hikey);
  if (c->xfout_lokey >= 0 && c->xfout_hikey >= 0) x *= xf_out(key, c->xfout_lokey, c->xfout_hikey);
  return x * xf_in(vel, c->xfin_lovel, c->xfin_hivel) * xf_out(vel, c->xfout_lovel, c->xfout_hivel);
}

int smp_sfv_xf_silent(const SmpSfRegion *r, int key, int vel) {
  if (r->gen[G_VELOCITY] >= 0) vel = r->gen[G_VELOCITY] & 127;   /* as smp_sfv_start reads the velocity */
  return xf_pos(r, key & 127, vel & 127) <= 0;
}

static double cond_gain(const SmpSfRegion *r, int key, int vel, double held) {
  const SmpSfCond *c = &r->cond;
  double x = xf_pos(r, key, vel), g;
  g = c->xf_curve == 1 ? x : sqrt(x);
  if (!isnan(c->amp_veltrack)) {
    double t = clampd(c->amp_veltrack / 100.0, -1, 1), cv = (vel / 127.0) * (vel / 127.0);
    if (c->amp_velcurve) cv = clampd(c->amp_velcurve[vel & 127], 0, 1);
    g *= t >= 0 ? 1 - t * (1 - cv) : 1 + t * cv;
  }
  if (held >= 0 && c->rt_decay > 0) g *= pow(10.0, -c->rt_decay * held / 20.0);
  if (held >= 0 && c->rt_linear > 0) g *= clampd(1 - c->rt_linear * held, 0, 1);
  if (c->gain > 0 && c->gain != 1) g *= c->gain;
  return g;
}

/* ---- the voice ---- */

static void end_voice(SmpSfVoice *v) {
  if (!v->live) return;
  v->live = 0;
  if (v->ss) InterlockedExchange(&v->ss->live, 0);   /* the streamer lets go of its hold on its next pass */
  smp_sf_pcm_release(v->font, v->sample);     /* exactly once per successful start */
}

/* the voice's slot in the streamer: a new generation, the first frame it needs, and a start message carrying a
   second hold of the sample for the streamer. Without room in the queue the voice plays without a ring. */
static void stream_start(SmpSfVoice *v, SfStream *st, int slot, const void *stream) {
  SfSlot *ss = sfs_slot(st, slot);
  SfMsg m;
  LONG g = ++ss->gen;
  if (!(g & 0x7FFFFF)) g = ++ss->gen;          /* 0 means "none" in the tags */
  memset(&m, 0, sizeof m);
  m.f = v->font; m.stream = stream; m.slot = slot; m.sample = v->sample; m.gen = g;
  m.hf = (uint32_t)v->hf; m.t0 = (uint32_t)v->t0; m.t1 = (uint32_t)v->t1; m.end = (uint32_t)v->end; m.ls = (uint32_t)v->ls; m.le = (uint32_t)v->le;
  m.p0 = v->start > 0 ? v->start - 1 : 0;
  m.loopy = v->loopy;
  InterlockedExchange(&ss->rel, -1);
  InterlockedExchange64(&ss->need, SF_TAG(g, 0) | m.p0);
  InterlockedExchange(&ss->live, g);
  sf_hold(v->font, v->sample);
  if (!sfs_push(st, &m)) {
    InterlockedExchange(&ss->live, 0);
    smp_sf_pcm_release(v->font, v->sample);
    return;
  }
  v->ss = ss; v->ring = sfs_ring(st, slot); v->sgen = g;
}

int smp_sfv_start(SmpSfVoice *v, SmpFont *f, const SmpSfRegion *r, int key, int vel, const SmpSfChan *c,
                  double rate, double transpose, SfStream *st, int slot, double held) {
  const SmpSfSample *sh;
  SmpSfView vw;
  uint32_t frames, ls, le;
  int64_t start, end, lps, lpe;
  const int16_t *gen = r->gen;
  memset(v, 0, sizeof *v);
  if (!f || !r || !c || !(rate > 0)) return 0;
  sh = smp_sf_sample(f, r->sample);
  if (!sh) return 0;
  if (!smp_sf_pcm_acquire(f, r->sample, &vw)) return 0;
  frames = vw.frames; ls = vw.loop_start; le = vw.loop_end;
  if (frames == 0) { smp_sf_pcm_release(f, r->sample); return 0; }

  v->font = f; v->reg = r; v->sample = r->sample;
  v->data = vw.head; v->is_float = vw.is_float; v->hf = vw.head_frames;
  if (vw.tail) { v->tail = vw.tail; v->t0 = vw.tail_start; v->t1 = (int64_t)vw.tail_start + vw.tail_frames; }
  v->key = key & 127; v->vel = vel & 127;
  if (gen[G_VELOCITY] >= 0) v->vel = gen[G_VELOCITY] & 127;
  v->pkey = gen[G_KEYNUM] >= 0 ? (gen[G_KEYNUM] & 127) : v->key;
  v->rate = rate; v->transpose = transpose;
  v->srate = vw.rate > 0 ? vw.rate : sh->rate > 0 ? sh->rate : 44100;   /* a decoded file's own rate wins */
  v->root = gen[G_ROOTKEY] >= 0 ? gen[G_ROOTKEY] : (sh->pitch <= 127 ? sh->pitch : 60);
  v->correction = sh->correction;

  /* the address offsets, fine plus coarse (32768 frames), kept inside the sample */
  start = (int64_t)gen[G_START] + 32768 * (int64_t)gen[G_STARTCOARSE];
  end = (int64_t)frames + gen[G_END] + 32768 * (int64_t)gen[G_ENDCOARSE];
  if (r->cond.end_frame) end = r->cond.end_frame;                /* SFZ's end, as a frame of the sample */
  lps = (int64_t)ls + gen[G_LOOPSTART] + 32768 * (int64_t)gen[G_LOOPSTARTCOARSE];
  lpe = (int64_t)le + gen[G_LOOPEND] + 32768 * (int64_t)gen[G_LOOPENDCOARSE];
  if (start < 0) start = 0;
  if (start > (int64_t)frames - 1) start = (int64_t)frames - 1;
  if (end > (int64_t)frames) end = frames;
  if (end < start + 1) end = start + 1;
  if (lps < 0) lps = 0;
  if (lpe > end) lpe = end;
  if (lps > end) lps = end;
  v->start = start; v->end = end; v->ls = lps; v->le = lpe;
  v->mode = gen[G_MODES] & 3;
  v->looping = (v->mode == 1 || v->mode == 3) && lpe > lps;     /* 0 and 2: no loop; a bad loop plays once */
  v->pos = (double)start;

  /* Past the head? A loop wholly inside the head plays from it; a loop-until-release may go on to the end. */
  v->loopy = v->looping;
  v->L = v->looping ? lpe - lps : 0;
  v->rel_w = -1; v->ug = 1;
  v->streaming = vw.stream && ((v->looping && v->mode != 3) ? lpe > v->hf : end > v->hf);
  v->live = 1;
  if (v->streaming && st) stream_start(v, st, slot, vw.stream);
  v->cond_gain = cond_gain(r, v->key, v->vel, held);
  derive(v, c);
  v->venv.stage = E_DELAY; v->menv.stage = E_DELAY;
  v->fade = 1;
  v->ctl_left = 0;
  return 1;
}

void smp_sfv_release(SmpSfVoice *v) {
  if (!v->live || v->released || v->reg->cond.one_shot) return;   /* a one-shot plays on to its sample's end */
  smp_sfv_off(v);
}

void smp_sfv_off(SmpSfVoice *v) {
  double a;
  if (!v->live || v->released) return;
  v->released = 1;
  a = venv_amp(&v->venv);
  if (v->venv.stage != E_DONE) {
    v->venv.val = a > 1e-5 ? 20 * log10(a) : FLOOR_DB;
    v->venv.stage = v->venv.val <= FLOOR_DB ? E_DONE : E_RELEASE;
  }
  if (v->menv.stage != E_DONE) {
    if (v->menv.stage == E_DELAY) v->menv.val = 0;
    v->menv.stage = E_RELEASE;
  }
  if (v->mode == 3 && v->looping) {          /* loop until release, then on to the end */
    v->looping = 0;
    v->rel_w = v->laps;                       /* the path leaves the loop at this lap's end */
    if (v->ss) InterlockedExchange(&v->ss->rel, (LONG)v->laps);
  }
}

int smp_sfv_needs_ring_now(const SmpSfVoice *v) {
  return v->live && v->streaming && v->ss && v->start + 2 >= v->hf;
}

static void read_avail(SmpSfVoice *v);

int smp_sfv_ring_low(SmpSfVoice *v) {
  int64_t p, total;
  if (!v->live || !v->streaming || !v->ss) return 0;
  read_avail(v);
  total = !v->loopy ? v->end : v->rel_w >= 0 ? v->end + v->rel_w * v->L : INT64_MAX;
  if (v->avail >= total) return 0;
  p = (int64_t)v->pos - 1 + v->laps * v->L;
  return v->avail - p < SF_RING / 2;
}

void smp_sfv_kill(SmpSfVoice *v, double fade_ms) {
  double step;
  if (!v->live) return;
  if (!(fade_ms > 0)) { end_voice(v); return; }
  step = 1.0 / (fade_ms * 0.001 * v->rate);
  if (!v->killing || step > v->fade_step) v->fade_step = step;
  v->killing = 1;
}

void smp_sfv_update(SmpSfVoice *v, const SmpSfChan *c) {
  if (!v->live || !c) return;
  derive(v, c);
}

double smp_sfv_level(const SmpSfVoice *v) {
  if (!v->live) return 0;
  return v->level * (v->killing ? v->fade : 1);
}

/* one frame at index i of the sample, scaled to -1..1: wrapped into the loop while looping, 0 past the end
   otherwise (the pad makes a frame or two before the start readable). Once the loop has wrapped, the point before
   the loop start is the loop's last frame, so the interpolation is seamless across the joint (FluidSynth does the
   same). These read the head, which holds every frame the voice can reach when it doesn't stream. */
static __inline int64_t wrap_at(const SmpSfVoice *v, int64_t i) {
  if (v->looping) {
    if (i >= v->le) i = v->ls + (i - v->le) % (v->le - v->ls);
    else if (i < v->ls && v->looped) i = v->le - 1 - (v->ls - 1 - i) % (v->le - v->ls);
  } else if (i >= v->end) return -SF_PCM_PAD - 1;
  return i;
}
static __inline float frame_i16(const SmpSfVoice *v, int64_t i) {
  i = wrap_at(v, i);
  return i < -SF_PCM_PAD ? 0 : ((const int16_t *)v->data)[i] * (1.0f / 32768.0f);
}
static __inline float frame_f32(const SmpSfVoice *v, int64_t i) {
  i = wrap_at(v, i);
  return i < -SF_PCM_PAD ? 0 : ((const float *)v->data)[i];
}

/* A streaming voice's frame i (of the current lap): from the head or the post-loop head when the sample frame is in
   one of them, else from the ring at its path frame, if the streamer has got that far; else *dry is set and it is 0. */
static __inline float frame_stream(const SmpSfVoice *v, int64_t i, int *dry) {
  int64_t p = i + v->laps * v->L, s = sf_path_frame(p, v->ls, v->le, v->loopy, v->rel_w);
  if (s >= v->end) return 0;
  if (s < v->hf) return v->is_float ? ((const float *)v->data)[s] : ((const int16_t *)v->data)[s] * (1.0f / 32768.0f);
  if (s >= v->t0 && s < v->t1)
    return v->is_float ? ((const float *)v->tail)[s - v->t0] : ((const int16_t *)v->tail)[s - v->t0] * (1.0f / 32768.0f);
  if (p < v->avail) return v->ring[p & SF_RMASK];
  *dry = 1;
  return 0;
}

/* frame i will come from the ring, and the ring doesn't have it yet */
static __inline int ring_short(const SmpSfVoice *v, int64_t i) {
  int64_t p = i + v->laps * v->L, s = sf_path_frame(p, v->ls, v->le, v->loopy, v->rel_w);
  return s >= v->hf && s < v->end && !(s >= v->t0 && s < v->t1) && p >= v->avail;
}

/* the ring won't have every frame the next m samples and SF_UFADE more need (at most a quarter ring): fade out now. The ring fills in path
   order, so it is enough to look at the furthest frame, and at the loop's last frame when the loop end comes
   first (past it the path may be back in the head). */
static int ahead_short(const SmpSfVoice *v, int m) {
  /* at a high pitch that many samples would be more frames than a ring holds: then a quarter ring (a quicker fade) */
  double span = (SF_UFADE + m) * fabs(v->ratio);
  int64_t a = (int64_t)v->pos, b = (int64_t)(v->pos + (span < SF_RING / 4 ? span : SF_RING / 4)) + 3;
  if (ring_short(v, b)) return 1;
  return v->looping && a < v->le && b >= v->le && ring_short(v, v->le - 1);
}

/* the path frames in the ring, from the slot's filled count. Until the streamer has seen a loop-until-release let
   go, only frames before that lap's loop end are good. */
static void read_avail(SmpSfVoice *v) {
  LONG64 f;
  v->avail = 0;
  if (!v->ss) return;
  f = v->ss->filled;
  if (SF_TAGGEN(f) != (v->sgen & 0x7FFFFF)) return;
  v->avail = SF_VAL(f);
  if (v->rel_w >= 0 && !SF_TAGREL(f)) {
    int64_t b = v->le + v->rel_w * v->L;
    if (v->avail > b) v->avail = b;
  }
}

/* m samples of the current stretch; 0 when the voice must end. FRAME(v, i) gives frame i. The int16 one is the
   stage 2 code exactly, so a sample held whole sounds bit for bit as it did. */
#define RUN_BODY(FRAME)                                                                                          \
  int i;                                                                                                         \
  for (i = 0; i < m; i++) {                                                                                      \
    int64_t i0 = (int64_t)v->pos;                                                                                \
    float x0 = FRAME(v, i0 - 1), x1 = FRAME(v, i0), x2 = FRAME(v, i0 + 1), x3 = FRAME(v, i0 + 2);                \
    float t = (float)(v->pos - (double)i0);                                                                      \
    float c1 = 0.5f * (x2 - x0), c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3, c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2); \
    double y = ((c3 * t + c2) * t + c1) * t + x1, f = v->fade;                                                   \
    if (v->filt_on) {                                                                                            \
      double o = v->b0 * y + v->b1 * v->x1 + v->b2 * v->x2 - v->a1 * v->y1 - v->a2 * v->y2;                      \
      v->x2 = v->x1; v->x1 = y; v->y2 = v->y1; v->y1 = o;                                                        \
      y = o;                                                                                                     \
      v->b0 += v->d_b0; v->b1 += v->d_b1; v->b2 += v->d_b2; v->a1 += v->d_a1; v->a2 += v->d_a2;                  \
    }                                                                                                            \
    l[i] += (float)(y * v->gl * f);                                                                              \
    r[i] += (float)(y * v->gr * f);                                                                              \
    if (rev) rev[i] += (float)(y * v->grev * f);                                                                 \
    if (cho) cho[i] += (float)(y * v->gcho * f);                                                                 \
    v->gl += v->d_gl; v->gr += v->d_gr; v->grev += v->d_grev; v->gcho += v->d_gcho;                              \
    v->pos += v->ratio;                                                                                          \
    v->ratio += v->d_ratio;                                                                                      \
    if (v->looping) {                                                                                            \
      if (v->pos >= (double)v->le) {                                                                             \
        v->pos -= (double)(v->le - v->ls) * floor((v->pos - v->ls) / (double)(v->le - v->ls));                   \
        v->looped = 1;                                                                                           \
      }                                                                                                          \
    } else if (v->pos >= (double)v->end) return 0;                                                               \
    if (v->killing && (v->fade -= v->fade_step) <= 0) return 0;                                                  \
  }                                                                                                              \
  /* no denormals: tiny filter state is flushed to 0 */                                                          \
  if (fabs(v->x1) < 1e-20) v->x1 = 0;                                                                            \
  if (fabs(v->x2) < 1e-20) v->x2 = 0;                                                                            \
  if (fabs(v->y1) < 1e-20) v->y1 = 0;                                                                            \
  if (fabs(v->y2) < 1e-20) v->y2 = 0;                                                                            \
  return 1;

static int run_i16(SmpSfVoice *v, float *l, float *r, float *rev, float *cho, int m) { RUN_BODY(frame_i16) }
static int run_f32(SmpSfVoice *v, float *l, float *r, float *rev, float *cho, int m) { RUN_BODY(frame_f32) }
/* a streaming voice's stretch that the ring covers whole, with no loop joint in it (see run_stream) */
#define frame_ring(v, i) ((v)->ring[((i) + (v)->roff) & SF_RMASK])
static int run_ring(SmpSfVoice *v, float *l, float *r, float *rev, float *cho, int m) { RUN_BODY(frame_ring) }

/* the same for a streaming voice: frames from the head or the ring, the laps counted, and the underrun gain, which
   fades out over SF_UFADE samples before the ring runs dry (looking that far ahead) and back in once it has
   frames again. A dry frame plays as silence and the voice goes on, so it never stalls. With the gain at 1 the
   output is the same as run_i16's. */
static int run_stream(SmpSfVoice *v, float *l, float *r, float *rev, float *cho, int m) {
  int i, short_ahead;
  int64_t hend = v->hf < v->end ? v->hf : v->end;   /* head frames the voice can play */
  read_avail(v);
  short_ahead = ahead_short(v, m);
  if (!short_ahead && v->ug == 1) {
    /* Most stretches lie wholly in the head or wholly in the ring, with no loop joint in them (then no lap can end
       in the stretch either): they run the plain loops, which give the same output. */
    double rmax = v->ratio + (v->d_ratio > 0 ? v->d_ratio * m : 0);
    int64_t lo = (int64_t)v->pos - 1, hi = (int64_t)(v->pos + rmax * m) + 3;
    int lin = !v->loopy || ((v->laps == 0 || lo >= v->ls) && (!v->looping || hi < v->le));
    if (lin && hi < hend) return v->is_float ? run_f32(v, l, r, rev, cho, m) : run_i16(v, l, r, rev, cho, m);
    if (lin && lo >= v->hf && hi < v->end && hi + v->laps * v->L < v->avail && (hi < v->t0 || lo >= v->t1)) {
      v->roff = v->laps * v->L;
      return run_ring(v, l, r, rev, cho, m);
    }
  }
  for (i = 0; i < m; i++) {
    int64_t i0 = (int64_t)v->pos;
    int dry = 0;
    float x0, x1, x2, x3;
    /* the four frames straight on in the sample (no loop joint among them): from the head, or from the ring */
    int lin = !v->loopy || ((v->laps == 0 || i0 - 1 >= v->ls) && (!v->looping || i0 + 2 < v->le));
    if (lin && i0 + 2 < hend) {
      if (v->is_float) {
        const float *d = (const float *)v->data + i0;
        x0 = d[-1]; x1 = d[0]; x2 = d[1]; x3 = d[2];
      } else {
        const int16_t *d = (const int16_t *)v->data + i0;
        x0 = d[-1] * (1.0f / 32768.0f); x1 = d[0] * (1.0f / 32768.0f); x2 = d[1] * (1.0f / 32768.0f); x3 = d[2] * (1.0f / 32768.0f);
      }
    } else if (lin && i0 - 1 >= v->hf && i0 + 2 < v->end && i0 + 2 + v->laps * v->L < v->avail &&
               (i0 + 2 < v->t0 || i0 - 1 >= v->t1)) {   /* the streamer skips the post-loop head's frames */
      int64_t q = i0 - 1 + v->laps * v->L;
      x0 = v->ring[q & SF_RMASK]; x1 = v->ring[(q + 1) & SF_RMASK]; x2 = v->ring[(q + 2) & SF_RMASK]; x3 = v->ring[(q + 3) & SF_RMASK];
    } else {
      x0 = frame_stream(v, i0 - 1, &dry); x1 = frame_stream(v, i0, &dry); x2 = frame_stream(v, i0 + 1, &dry);
      x3 = frame_stream(v, i0 + 2, &dry);
    }
    {                                   /* the interpolation, the gains and the step, as in run_i16 */
      float t = (float)(v->pos - (double)i0);
      float c1 = 0.5f * (x2 - x0), c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3, c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
      double y = ((c3 * t + c2) * t + c1) * t + x1, f = v->fade;
      if (dry || short_ahead) {
        if ((v->ug -= 1.0 / SF_UFADE) < 0) v->ug = 0;
      } else if (v->ug < 1 && (v->ug += 1.0 / SF_UFADE) > 1) v->ug = 1;
      if (dry) { v->underrun = 1; y = 0; }
      if (v->filt_on) {
        double o = v->b0 * y + v->b1 * v->x1 + v->b2 * v->x2 - v->a1 * v->y1 - v->a2 * v->y2;
        v->x2 = v->x1; v->x1 = y; v->y2 = v->y1; v->y1 = o;
        y = o;
        v->b0 += v->d_b0; v->b1 += v->d_b1; v->b2 += v->d_b2; v->a1 += v->d_a1; v->a2 += v->d_a2;
      }
      y *= v->ug;
      l[i] += (float)(y * v->gl * f);
      r[i] += (float)(y * v->gr * f);
      if (rev) rev[i] += (float)(y * v->grev * f);
      if (cho) cho[i] += (float)(y * v->gcho * f);
      v->gl += v->d_gl; v->gr += v->d_gr; v->grev += v->d_grev; v->gcho += v->d_gcho;
      v->pos += v->ratio;
      v->ratio += v->d_ratio;
      if (v->looping) {
        if (v->pos >= (double)v->le) {
          double k = floor((v->pos - v->ls) / (double)(v->le - v->ls));
          v->pos -= (double)(v->le - v->ls) * k;
          v->laps += (int64_t)k;
          v->looped = 1;
        }
      } else if (v->pos >= (double)v->end) return 0;
      if (v->killing && (v->fade -= v->fade_step) <= 0) return 0;
    }
  }
  if (fabs(v->x1) < 1e-20) v->x1 = 0;
  if (fabs(v->x2) < 1e-20) v->x2 = 0;
  if (fabs(v->y1) < 1e-20) v->y1 = 0;
  if (fabs(v->y2) < 1e-20) v->y2 = 0;
  return 1;
}

static int run(SmpSfVoice *v, float *l, float *r, float *rev, float *cho, int m) {
  if (v->streaming) return run_stream(v, l, r, rev, cho, m);
  return v->is_float ? run_f32(v, l, r, rev, cho, m) : run_i16(v, l, r, rev, cho, m);
}

/* the oldest path frame the voice still needs, for the streamer */
static void publish_need(SmpSfVoice *v) {
  int64_t nd;
  if (!v->ss || !v->live) return;
  nd = (int64_t)v->pos - 1 + v->laps * v->L;
  v->ss->need = SF_TAG(v->sgen, 0) | (nd < 0 ? 0 : nd);
}

int smp_sfv_render(SmpSfVoice *v, float *l, float *r, float *rev, float *cho, int n) {
  int i = 0;
  if (!v->live) return 0;
  while (i < n) {
    int m;
    if (v->ctl_left <= 0) {
      if (v->ending) { end_voice(v); return 0; }   /* the stretch that faded to 0 is over */
      control(v);
      v->ctl_left = SMP_SFV_CTL;
    }
    m = n - i < v->ctl_left ? n - i : v->ctl_left;
    if (!run(v, l + i, r + i, rev ? rev + i : NULL, cho ? cho + i : NULL, m)) { end_voice(v); return 0; }
    i += m;
    v->ctl_left -= m;
  }
  if (v->ctl_left <= 0 && v->ending) { end_voice(v); return 0; }
  publish_need(v);
  return 1;
}

/* ---- the test export ---- */

#define TEST_BLOCK 256

SMP_API int smp_sf_test_note(SmpFont *f, int preset, int key, int vel, double rate, long frames, long release_at,
                             float *out_lr) {
  const SmpSfRegion *regs = NULL;
  SmpSfVoice *vs = NULL;
  SmpSfChan ch;
  float *l = NULL, *r = NULL, *rev = NULL, *cho = NULL;
  SfStream *st = NULL;
  int nreg, nv = 0, i;
  long at;
  if (!f || frames < 0 || !out_lr || !(rate > 0)) return -1;
  nreg = smp_sf_regions(f, preset, &regs);
  if (nreg <= 0) return 0;
  memset(&ch, 0, sizeof ch);
  ch.cc[7] = 100; ch.cc[10] = 64; ch.cc[11] = 127;      /* GM reset values */
  ch.bend = 8192; ch.bend_range_cents = 200;
  smp_sf_use(f, preset, 1);
  smp_sf_wait_idle(f, 10000);
  vs = (SmpSfVoice *)calloc((size_t)nreg, sizeof *vs);
  l = (float *)calloc((size_t)frames + 1, sizeof *l);
  r = (float *)calloc((size_t)frames + 1, sizeof *r);
  rev = (float *)calloc(TEST_BLOCK, sizeof *rev);
  cho = (float *)calloc(TEST_BLOCK, sizeof *cho);
  if (f->has_long) st = sfs_new(nreg);          /* a streamer of its own, waited for before every block */
  if (!vs || !l || !r || !rev || !cho || (f->has_long && !st)) { nv = -1; goto done; }
  for (i = 0; i < nreg; i++) {
    const SmpSfRegion *g = &regs[i];
    if (key < g->lokey || key > g->hikey || vel < g->lovel || vel > g->hivel) continue;
    if (smp_sfv_start(&vs[nv], f, g, key, vel, &ch, rate, 0, st, nv, -1)) nv++;
  }
  for (at = 0; at < frames;) {
    long n = frames - at < TEST_BLOCK ? frames - at : TEST_BLOCK;
    if (release_at >= 0 && at < release_at && at + n > release_at) n = release_at - at;
    if (at == release_at) for (i = 0; i < nv; i++) smp_sfv_release(&vs[i]);
    if (st) for (i = 0; i < nv; i++) if (smp_sfv_ring_low(&vs[i]) || smp_sfv_needs_ring_now(&vs[i])) { sfs_sync(st); break; }
    for (i = 0; i < nv; i++) smp_sfv_render(&vs[i], l + at, r + at, rev, cho, (int)n);
    at += n;
  }
  for (i = 0; i < nv; i++) smp_sfv_kill(&vs[i], 0);
  for (at = 0; at < frames; at++) { out_lr[2 * at] = l[at]; out_lr[2 * at + 1] = r[at]; }
done:
  if (vs && nv < 0) for (i = 0; i < nreg; i++) smp_sfv_kill(&vs[i], 0);
  sfs_free(st);                                 /* after every voice ended: releases the streamer's holds */
  free(vs); free(l); free(r); free(rev); free(cho);
  smp_sf_use(f, preset, -1);
  return nv;
}
