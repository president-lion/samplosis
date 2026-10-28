/* sfvoice.c: one SoundFont voice (SF2.04 section 9). See sfvoice.h.

   Every SMP_SFV_CTL samples the voice works out its envelopes, LFOs, pitch, gain and filter coefficients for the
   end of the next stretch, and steps every value linearly toward them sample by sample, so nothing jumps.
   Modulators are worked out at start and when a controller changes (smp_sfv_update), and their sums are added
   to the region's generators. Nothing here allocates, locks or waits, except smp_sf_test_note (main thread). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "sfvoice.h"

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

/* envelope stages */
enum { E_DELAY, E_ATTACK, E_HOLD, E_DECAY, E_SUSTAIN, E_RELEASE, E_DONE };

/* the EMU factor on initialAttenuation that FluidSynth applies to the generator (not to modulators) */
#define ATTEN_FACTOR 0.4
#define FLOOR_DB (-100.0)

static double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

/* timecents to samples. -12000 and below is the shortest the spec allows, about 1 ms. */
static double tc_samples(double tc, double rate) { return rate * pow(2.0, clampd(tc, -12000, 8000) / 1200.0); }

/* absolute cents to Hz */
static double abs_hz(double cents) { return 8.176 * pow(2.0, cents / 1200.0); }

/* ---- modulators (SF2.04 section 8.2) ---- */

/* The concave curve, as SF2.04 section 8.2.4 draws it and FluidSynth tabulates it (fluid_conv.c,
   fluid_concave): concave(x) = -20/96 * log10((1 - x)^2), for x from 0 to 1, limited to 1. It is the curve
   whose 960 cB make an amplitude of x^2 (40 * log10(x) dB). Convex is its mirror: convex(x) = 1 - concave(1 - x). */
static double concave(double x) {
  double y;
  if (x <= 0) return 0;
  if (x >= 1) return 1;
  y = -20.0 / 96.0 * log10((1 - x) * (1 - x));
  return y > 1 ? 1 : y;
}
static double convex(double x) { return 1 - concave(1 - x); }

typedef struct { const SmpSfChan *c; int key, vel; } ModCtx;

/* a modulator source, mapped through its curve, polarity and direction (the four flags of section 8.2.1).
   7-bit sources count in 128ths (64 is the middle), the pitch wheel in 16384ths, and the pitch wheel
   sensitivity as cents / 12700 (so the default pitch wheel modulator's 12700 gives the bend range in cents).
   "No controller" is 1, as the spec says. */
static double source(unsigned s, const ModCtx *m) {
  unsigned idx = s & 127, type = s >> 10, bip = (s >> 9) & 1, neg = (s >> 8) & 1;
  double x;
  if (s & 0x80) x = m->c->cc[idx] / 128.0;
  else switch (idx) {
    case 0: return 1.0;
    case 2: x = m->vel / 128.0; break;
    case 3: x = m->key / 128.0; break;
    case 10: x = m->c->poly[m->key & 127] / 128.0; break;
    case 13: x = m->c->pressure / 128.0; break;
    case 14: x = m->c->bend / 16384.0; break;
    case 16: x = m->c->bend_range_cents / 12700.0; break;
    default: return 0;
  }
  if (neg) x = 1 - x;
  if (!bip) {
    switch (type) {
      case 1: return concave(x);
      case 2: return convex(x);
      case 3: return x >= 0.5 ? 1 : 0;
      default: return x;
    }
  }
  switch (type) {          /* bipolar: the curve runs both ways out from the middle */
    case 1: return x >= 0.5 ? concave(2 * x - 1) : -concave(1 - 2 * x);
    case 2: return x >= 0.5 ? convex(2 * x - 1) : -convex(1 - 2 * x);
    case 3: return x >= 0.5 ? 1 : -1;
    default: return 2 * x - 1;
  }
}

static void add_mods(double *sum, const SmpSfMod *list, int n, const ModCtx *m) {
  int i;
  for (i = 0; i < n; i++) {
    const SmpSfMod *d = &list[i];
    double y;
    if (d->dest >= SMP_SF_NGEN) continue;     /* the parser drops these; be safe */
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
  m.c = c; m.key = v->key; m.vel = v->vel;
  memset(v->mod, 0, sizeof v->mod);
  add_mods(v->mod, r->imod, r->nimod, &m);
  add_mods(v->mod, r->pmod, r->npmod, &m);
  for (i = 0; i < SMP_SF_NGEN; i++) g[i] = r->gen[i] + v->mod[i];

  /* Pitch, in cents from the sample as recorded. The channel's pitch bend is not added here: it comes through
     the default pitch wheel modulator (to G_PITCH, 12700 cents scaled by the pitch wheel sensitivity, which is
     the channel's bend range). RPN fine and coarse tuning are added directly. */
  v->chan_tune = c->fine_cents + 100.0 * c->coarse_semis;
  v->cents0 = (v->pkey - v->root) * g[G_SCALE] + 100.0 * g[G_COARSE] + g[G_FINE] + v->correction +
              g[G_PITCH] + v->chan_tune + 100.0 * v->transpose;
  v->ratio_k = v->srate / rate;

  /* gain: initialAttenuation times the EMU factor, plus the modulators' attenuation (velocity, CC7, CC11) */
  v->atten_gain = pow(10.0, -clampd(ATTEN_FACTOR * r->gen[G_ATTEN] + v->mod[G_ATTEN], 0, 1440) / 200.0);
  {
    double th = (clampd(g[G_PAN], -500, 500) + 500) / 1000.0 * (PI / 2);   /* constant power, -3 dB each at 0 */
    v->pan_l = cos(th); v->pan_r = sin(th);
    if (v->pan_r < 1e-12) v->pan_r = 0;
    if (v->pan_l < 1e-12) v->pan_l = 0;
  }
  v->rev_send = clampd(g[G_REVERB], 0, 1000) / 1000.0;
  v->cho_send = clampd(g[G_CHORUS], 0, 1000) / 1000.0;

  /* The filter's Q. The spec: initialFilterQ is the height in cB of the resonance above the DC gain, and 0 or
     less is not resonant. Q is chosen so the response's peak is exactly that high: for a 2-pole lowpass the
     peak is M = Q / sqrt(1 - 1/(4 Q^2)), so Q^2 = (M^2 + M sqrt(M^2 - 1)) / 2. At 0 cB that is 1/sqrt(2), a
     Butterworth with no peak, and it rises smoothly from there. As SF2.01 asks (and FluidSynth does), the gain
     drops by half the peak in dB, a factor 1/sqrt(M). */
  q_db = clampd(g[G_Q], 0, 960) / 10.0;
  peak = pow(10.0, q_db / 20.0);
  v->q_lin = sqrt((peak * peak + peak * sqrt(peak * peak - 1)) / 2);
  v->q_gain = 1.0 / sqrt(peak);

  /* LFOs */
  v->modlfo.delay = tc_samples(g[G_DELAY_MODLFO], rate);
  v->modlfo.inc = abs_hz(clampd(g[G_FREQ_MODLFO], -16000, 4500)) / rate;
  v->viblfo.delay = tc_samples(g[G_DELAY_VIBLFO], rate);
  v->viblfo.inc = abs_hz(clampd(g[G_FREQ_VIBLFO], -16000, 4500)) / rate;

  /* envelopes; hold and decay scale with the key, (60 - key) * value timecents */
  k = 60 - v->pkey;
  v->v_delay = tc_samples(g[G_DELAY_VOLENV], rate);
  v->v_attack = tc_samples(g[G_ATTACK_VOLENV], rate);
  v->v_hold = tc_samples(g[G_HOLD_VOLENV] + k * g[G_KEY_VOLENV_HOLD], rate);
  v->v_decay = tc_samples(g[G_DECAY_VOLENV] + k * g[G_KEY_VOLENV_DECAY], rate);
  v->v_sus_db = -clampd(g[G_SUSTAIN_VOLENV], 0, 1440) / 10.0;
  v->v_release = tc_samples(g[G_RELEASE_VOLENV], rate);
  v->m_delay = tc_samples(g[G_DELAY_MODENV], rate);
  v->m_attack = tc_samples(g[G_ATTACK_MODENV], rate);
  v->m_hold = tc_samples(g[G_HOLD_MODENV] + k * g[G_KEY_MODENV_HOLD], rate);
  v->m_decay = tc_samples(g[G_DECAY_MODENV] + k * g[G_KEY_MODENV_DECAY], rate);
  v->m_sus = 1 - clampd(g[G_SUSTAIN_MODENV], 0, 1000) / 1000.0;
  v->m_release = tc_samples(g[G_RELEASE_MODENV], rate);
}

/* ---- envelopes and LFOs, stepped n samples at a time ---- */

/* The volume envelope. Delay and attack keep val as an amplitude (the attack is linear in amplitude); from the
   hold on, val is in dB. Decay and release fall 100 dB per their time (linear in dB), the decay down to the
   sustain level, the release to -100 dB, where the envelope is done. */
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
        double step = 100.0 / v->v_decay, target = v->v_sus_db;
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
        e->val -= n * 100.0 / v->v_release;
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

/* the modulation envelope: the same shape on a 0..1 scale, linear throughout */
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

static void filter_target(const SmpSfVoice *v, double fc_cents, double *c) {
  double hz, w, cs, alpha, a0;
  if (fc_cents >= 13500 && v->g[G_Q] <= 0) {     /* bypassed, as FluidSynth does: a straight wire */
    c[0] = 1; c[1] = c[2] = c[3] = c[4] = 0;
    return;
  }
  hz = clampd(abs_hz(fc_cents), 20, 0.45 * v->rate);
  w = 2 * PI * hz / v->rate;
  cs = cos(w);
  alpha = sin(w) / (2 * v->q_lin);
  a0 = 1 + alpha;
  c[0] = (1 - cs) / 2 / a0 * v->q_gain;       /* RBJ's lowpass, the DC gain lowered by q_gain */
  c[1] = (1 - cs) / a0 * v->q_gain;
  c[2] = c[0];
  c[3] = -2 * cs / a0;
  c[4] = (1 - alpha) / a0;
}

static void control(SmpSfVoice *v) {
  const double inv = 1.0 / SMP_SFV_CTL, *g = v->g;
  double amp, cents, ratio, gain, c[5];
  int wire;
  venv_step(v, SMP_SFV_CTL);
  menv_step(v, SMP_SFV_CTL);
  lfo_step(&v->modlfo, SMP_SFV_CTL);
  lfo_step(&v->viblfo, SMP_SFV_CTL);
  if (v->venv.stage == E_DONE) v->ending = 1;    /* this stretch fades to 0, then the voice ends */

  cents = v->cents0 + v->menv.val * g[G_MODENV_PITCH] + v->modlfo.val * g[G_MODLFO_PITCH] +
          v->viblfo.val * g[G_VIBLFO_PITCH];
  ratio = pow(2.0, cents / 1200.0) * v->ratio_k;
  amp = venv_amp(&v->venv);
  gain = amp * v->atten_gain * pow(10.0, v->modlfo.val * g[G_MODLFO_VOL] / 200.0);
  v->level = amp * v->atten_gain;
  filter_target(v, g[G_FC] + v->menv.val * g[G_MODENV_FC] + v->modlfo.val * g[G_MODLFO_FC], c);
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

/* ---- the voice ---- */

static void end_voice(SmpSfVoice *v) {
  if (!v->live) return;
  v->live = 0;
  smp_sf_pcm_release(v->font, v->sample);     /* exactly once per successful start */
}

int smp_sfv_start(SmpSfVoice *v, SmpFont *f, const SmpSfRegion *r, int key, int vel, const SmpSfChan *c,
                  double rate, double transpose) {
  const SmpSfSample *sh;
  const int16_t *data;
  uint32_t frames = 0, ls = 0, le = 0;
  int64_t start, end, lps, lpe;
  const int16_t *gen = r->gen;
  memset(v, 0, sizeof *v);
  if (!f || !r || !c || !(rate > 0)) return 0;
  sh = smp_sf_sample(f, r->sample);
  if (!sh) return 0;
  data = smp_sf_pcm_acquire(f, r->sample, &frames, &ls, &le);
  if (!data) return 0;
  if (frames == 0) { smp_sf_pcm_release(f, r->sample); return 0; }

  v->font = f; v->reg = r; v->sample = r->sample; v->data = data;
  v->key = key & 127; v->vel = vel & 127;
  if (gen[G_VELOCITY] >= 0) v->vel = gen[G_VELOCITY] & 127;
  v->pkey = gen[G_KEYNUM] >= 0 ? (gen[G_KEYNUM] & 127) : v->key;
  v->rate = rate; v->transpose = transpose;
  v->srate = sh->rate > 0 ? sh->rate : 44100;
  v->root = gen[G_ROOTKEY] >= 0 ? gen[G_ROOTKEY] : (sh->pitch <= 127 ? sh->pitch : 60);
  v->correction = sh->correction;

  /* the address offsets, fine plus coarse (32768 frames), kept inside the sample */
  start = (int64_t)gen[G_START] + 32768 * (int64_t)gen[G_STARTCOARSE];
  end = (int64_t)frames + gen[G_END] + 32768 * (int64_t)gen[G_ENDCOARSE];
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

  v->live = 1;
  derive(v, c);
  v->venv.stage = E_DELAY; v->menv.stage = E_DELAY;
  v->fade = 1;
  v->ctl_left = 0;
  return 1;
}

void smp_sfv_release(SmpSfVoice *v) {
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
  if (v->mode == 3) v->looping = 0;          /* loop until release, then on to the end */
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

/* one frame at index i of the sample, as int16 scaled to -1..1: wrapped into the loop while looping, 0 past
   the end otherwise (the pad makes a frame or two before the start readable) */
static __inline float frame_at(const SmpSfVoice *v, int64_t i) {
  if (v->looping) {
    if (i >= v->le) i = v->ls + (i - v->le) % (v->le - v->ls);
  } else if (i >= v->end) return 0;
  return v->data[i] * (1.0f / 32768.0f);
}

/* m samples of the current stretch; 0 when the voice must end */
static int run(SmpSfVoice *v, float *l, float *r, float *rev, float *cho, int m) {
  int i;
  for (i = 0; i < m; i++) {
    int64_t i0 = (int64_t)v->pos;
    float x0 = frame_at(v, i0 - 1), x1 = frame_at(v, i0), x2 = frame_at(v, i0 + 1), x3 = frame_at(v, i0 + 2);
    float t = (float)(v->pos - (double)i0);
    float c1 = 0.5f * (x2 - x0), c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3, c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    double y = ((c3 * t + c2) * t + c1) * t + x1, f = v->fade;
    if (v->filt_on) {
      double o = v->b0 * y + v->b1 * v->x1 + v->b2 * v->x2 - v->a1 * v->y1 - v->a2 * v->y2;
      v->x2 = v->x1; v->x1 = y; v->y2 = v->y1; v->y1 = o;
      y = o;
      v->b0 += v->d_b0; v->b1 += v->d_b1; v->b2 += v->d_b2; v->a1 += v->d_a1; v->a2 += v->d_a2;
    }
    l[i] += (float)(y * v->gl * f);
    r[i] += (float)(y * v->gr * f);
    if (rev) rev[i] += (float)(y * v->grev * f);
    if (cho) cho[i] += (float)(y * v->gcho * f);
    v->gl += v->d_gl; v->gr += v->d_gr; v->grev += v->d_grev; v->gcho += v->d_gcho;
    v->pos += v->ratio;
    v->ratio += v->d_ratio;
    if (v->looping) {
      if (v->pos >= (double)v->le) v->pos -= (double)(v->le - v->ls) * floor((v->pos - v->ls) / (double)(v->le - v->ls));
    } else if (v->pos >= (double)v->end) return 0;
    if (v->killing && (v->fade -= v->fade_step) <= 0) return 0;
  }
  /* no denormals: tiny filter state is flushed to 0 */
  if (fabs(v->x1) < 1e-20) v->x1 = 0;
  if (fabs(v->x2) < 1e-20) v->x2 = 0;
  if (fabs(v->y1) < 1e-20) v->y1 = 0;
  if (fabs(v->y2) < 1e-20) v->y2 = 0;
  return 1;
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
  if (!vs || !l || !r || !rev || !cho) { nv = -1; goto done; }
  for (i = 0; i < nreg; i++) {
    const SmpSfRegion *g = &regs[i];
    if (key < g->lokey || key > g->hikey || vel < g->lovel || vel > g->hivel) continue;
    if (smp_sfv_start(&vs[nv], f, g, key, vel, &ch, rate, 0)) nv++;
  }
  for (at = 0; at < frames;) {
    long n = frames - at < TEST_BLOCK ? frames - at : TEST_BLOCK;
    if (release_at >= 0 && at < release_at && at + n > release_at) n = release_at - at;
    if (at == release_at) for (i = 0; i < nv; i++) smp_sfv_release(&vs[i]);
    for (i = 0; i < nv; i++) smp_sfv_render(&vs[i], l + at, r + at, rev, cho, (int)n);
    at += n;
  }
  for (i = 0; i < nv; i++) smp_sfv_kill(&vs[i], 0);
  for (at = 0; at < frames; at++) { out_lr[2 * at] = l[at]; out_lr[2 * at + 1] = r[at]; }
done:
  if (vs && nv < 0) for (i = 0; i < nreg; i++) smp_sfv_kill(&vs[i], 0);
  free(vs); free(l); free(r); free(rev); free(cho);
  smp_sf_use(f, preset, -1);
  return nv;
}
