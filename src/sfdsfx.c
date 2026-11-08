/* sfdsfx.c: a Decent Sampler preset's effects chain, in the order its <effects> gives them, on the preset's stereo
   bus (sfsynth.c renders the preset's voices there, then this, then adds the bus to output 1).

   - lowpass (lowpass_4pl), highpass, bandpass: RBJ biquads at the effect's frequency and resonance (Q, 0.1..20),
     the same on both channels; lowpass_1pl a one-pole lowpass. The frequency is kept below 0.45 of the rate; a
     lowpass at that top is a plain wire, so a tone knob at its middle changes nothing. A frequency change glides
     there (a one-pole of about 10 ms on the frequency, the coefficients worked out every 32 frames meanwhile).
   - reverb: Freeverb as JUCE's juce::Reverb has it, which Decent Sampler uses: 8 combs and 4 allpasses per
     channel (tunings for 44.1 kHz, scaled to the rate, the right channel 23 samples longer), the combs' feedback
     0.7 + 0.28 room size and damping 0.4 x damping, input gain 0.015, wet 3 x wetLevel, width 1, and the dry
     signal at 1 (Decent Sampler's reverb adds to the sound). Wet, room and damping glide over 10 ms. While wet
     is 0 nothing runs and the tails are cleared, so turning it up starts from silence.
   - delay: a line of 4 seconds per channel; the right channel's time is the effect's, the left's that less half the
     stereo offset (as the guide says); a musical time is a note value at the host's tempo (sfknob.c's list). The
     delayed signal is fed back at feedback (through a one-pole lowpass at feedbackCutoff, 22000 Hz being off) and
     added to the dry at wetLevel. A change of time (or tempo) crossfades to the new read point over 20 ms rather
     than sweeping the pitch. While wet is 0 nothing runs and the line is cleared.
   - effects off (the player's Reverb and chorus switch) bypasses reverbs and delays and clears their tails.
   Every buffer is allocated by sf_chain_new; nothing on the audio side allocates or locks. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sfdsfx.h"

#define PI 3.14159265358979323846
#define CTL 32                                   /* frames between coefficient updates while a filter glides */
#define NCOMB 8
#define NAP 4
#define DELAY_S 4.0

static const int comb_tun[NCOMB] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const int ap_tun[NAP] = {556, 441, 341, 225};
#define SPREAD 23

typedef struct { float *b; int n, i; float last; } Comb;
typedef struct { float *b; int n, i; } Ap;

typedef struct {
  SfFxDesc d;                                     /* the preset's, then as set */
  /* filters */
  double f_cur, q_cur; int primed, left;          /* the frequency gliding toward d.freq; frames to the next update */
  double b0, b1, b2, a1, a2, z1[2], z2[2], y1[2]; int wire;
  /* reverb */
  Comb comb[2][NCOMB]; Ap ap[2][NAP];
  float wet_cur, room_cur, damp_cur; int running;
  /* delay */
  float *line[2]; int len, w;
  double dcur[2], dnew[2]; int xf_left, xf_len;    /* the read delays (frames), and the crossfade between them */
  float fb_lp[2];
  float dwet_cur;
} Fx;

struct SfDsChain {
  double rate; int nfx;
  Fx fx[SF_FX_MAX];
  int was_on;
};

static double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

/* ---- filters ---- */

static void filter_coeffs(Fx *x, double rate) {
  double f = clampd(x->f_cur, 10, 0.45 * rate), q = clampd(x->q_cur, 0.1, 20), w = 2 * PI * f / rate;
  double cw = cos(w), sw = sin(w), al = sw / (2 * q), a0;
  x->wire = x->d.type == SF_FX_LP && f >= 0.45 * rate - 1e-6;
  switch (x->d.type) {
    case SF_FX_LP1: x->b0 = 1 - exp(-w); return;
    case SF_FX_HP: x->b0 = (1 + cw) / 2; x->b1 = -(1 + cw); x->b2 = (1 + cw) / 2; break;
    case SF_FX_BP: x->b0 = al; x->b1 = 0; x->b2 = -al; break;
    default: x->b0 = (1 - cw) / 2; x->b1 = 1 - cw; x->b2 = (1 - cw) / 2; break;
  }
  a0 = 1 + al;
  x->b0 /= a0; x->b1 /= a0; x->b2 /= a0; x->a1 = -2 * cw / a0; x->a2 = (1 - al) / a0;
}

static void filter_run(Fx *x, float *l, float *r, int n, double rate) {
  int i = 0, ch;
  if (!x->primed) { x->f_cur = x->d.freq; x->q_cur = x->d.q; filter_coeffs(x, rate); x->primed = 1; }
  while (i < n) {
    int m = n - i, gliding = x->f_cur != x->d.freq || x->q_cur != x->d.q;
    if (gliding) {
      double k = 1 - exp(-(double)CTL / (0.01 * rate));          /* about 10 ms */
      if (m > CTL) m = CTL;
      x->f_cur += (x->d.freq - x->f_cur) * k; x->q_cur += (x->d.q - x->q_cur) * k;
      if (fabs(x->f_cur - x->d.freq) < 0.01) x->f_cur = x->d.freq;
      if (fabs(x->q_cur - x->d.q) < 1e-4) x->q_cur = x->d.q;
      filter_coeffs(x, rate);
    }
    if (!x->wire) {
      for (ch = 0; ch < 2; ch++) {
        float *s = (ch ? r : l) + i;
        int j;
        if (x->d.type == SF_FX_LP1) {
          double y = x->y1[ch], a = x->b0;
          for (j = 0; j < m; j++) { y += a * (s[j] - y); s[j] = (float)y; }
          x->y1[ch] = fabs(y) < 1e-20 ? 0 : y;
        } else {
          double z1 = x->z1[ch], z2 = x->z2[ch];
          for (j = 0; j < m; j++) {                                   /* transposed direct form II */
            double in = s[j], out = x->b0 * in + z1;
            z1 = x->b1 * in - x->a1 * out + z2;
            z2 = x->b2 * in - x->a2 * out;
            s[j] = (float)out;
          }
          x->z1[ch] = fabs(z1) < 1e-20 ? 0 : z1; x->z2[ch] = fabs(z2) < 1e-20 ? 0 : z2;
        }
      }
    } else { x->z1[0] = x->z1[1] = x->z2[0] = x->z2[1] = 0; }
    i += m;
  }
}

/* ---- reverb ---- */

static void reverb_clear(Fx *x) {
  int c, k;
  for (c = 0; c < 2; c++) {
    for (k = 0; k < NCOMB; k++) { if (x->comb[c][k].b) memset(x->comb[c][k].b, 0, sizeof(float) * (size_t)x->comb[c][k].n); x->comb[c][k].last = 0; x->comb[c][k].i = 0; }
    for (k = 0; k < NAP; k++) { if (x->ap[c][k].b) memset(x->ap[c][k].b, 0, sizeof(float) * (size_t)x->ap[c][k].n); x->ap[c][k].i = 0; }
  }
}

static void reverb_run(Fx *x, float *l, float *r, int n, double rate) {
  int j, k;
  float wt = (float)clampd(x->d.wet, 0, 1), rt = (float)clampd(x->d.room, 0, 1), dt = (float)clampd(x->d.damp, 0, 1);
  float step = (float)(1.0 / (0.01 * rate));
  (void)rate;
  if (!x->running) {
    if (wt <= 0) return;
    reverb_clear(x);
    x->running = 1; x->wet_cur = 0; x->room_cur = rt; x->damp_cur = dt;
  }
  for (j = 0; j < n; j++) {
    float in, ol = 0, orr = 0, fb, d1, d2, wet;
    x->wet_cur += (float)clampd(wt - x->wet_cur, -step, step);
    x->room_cur += (float)clampd(rt - x->room_cur, -step, step);
    x->damp_cur += (float)clampd(dt - x->damp_cur, -step, step);
    fb = x->room_cur * 0.28f + 0.7f; d1 = x->damp_cur * 0.4f; d2 = 1 - d1;
    in = (l[j] + r[j]) * 0.015f;
    for (k = 0; k < NCOMB; k++) {
      Comb *cl = &x->comb[0][k], *cr = &x->comb[1][k];
      float o = cl->b[cl->i];
      cl->last = o * d2 + cl->last * d1;
      cl->b[cl->i] = in + cl->last * fb;
      if (++cl->i >= cl->n) cl->i = 0;
      ol += o;
      o = cr->b[cr->i];
      cr->last = o * d2 + cr->last * d1;
      cr->b[cr->i] = in + cr->last * fb;
      if (++cr->i >= cr->n) cr->i = 0;
      orr += o;
    }
    for (k = 0; k < NAP; k++) {
      Ap *al = &x->ap[0][k], *ar = &x->ap[1][k];
      float b = al->b[al->i];
      al->b[al->i] = ol + b * 0.5f; if (++al->i >= al->n) al->i = 0;
      ol = b - ol;
      b = ar->b[ar->i];
      ar->b[ar->i] = orr + b * 0.5f; if (++ar->i >= ar->n) ar->i = 0;
      orr = b - orr;
    }
    wet = x->wet_cur * 3.0f;                     /* width 1: each side its own channel */
    l[j] += ol * wet; r[j] += orr * wet;
  }
  if (wt <= 0 && x->wet_cur <= 0) { x->running = 0; reverb_clear(x); }
}

/* ---- delay ---- */

static double delay_frames(const Fx *x, int ch, double rate, double bpm) {
  double t = x->d.musical ? sf_mtime_beats((int)floor(x->d.time + 0.5)) * 60.0 / (bpm > 0 ? bpm : 120) : x->d.time;
  if (ch == 0) t -= x->d.offset / 2;
  return clampd(t * rate, 1, x->len - 2);
}

static void delay_clear(Fx *x) {
  if (x->line[0]) memset(x->line[0], 0, sizeof(float) * (size_t)x->len);
  if (x->line[1]) memset(x->line[1], 0, sizeof(float) * (size_t)x->len);
  x->fb_lp[0] = x->fb_lp[1] = 0; x->w = 0;
}

static float tap(const Fx *x, int ch, double d) {
  double p = x->w - d;
  int i0;
  float fr;
  while (p < 0) p += x->len;
  i0 = (int)p; fr = (float)(p - i0);
  return x->line[ch][i0] + (x->line[ch][i0 + 1 < x->len ? i0 + 1 : 0] - x->line[ch][i0]) * fr;
}

static void delay_run(Fx *x, float *l, float *r, int n, double rate, double bpm) {
  int j, ch;
  float wt = (float)clampd(x->d.wet, 0, 1), fb = (float)clampd(x->d.feedback, 0, 1), step = (float)(1.0 / (0.01 * rate));
  double nd[2], lpk = x->d.fb_cutoff >= 22000 ? 1 : 1 - exp(-2 * PI * clampd(x->d.fb_cutoff, 20, 0.45 * rate) / rate);
  if (!x->running) {
    if (wt <= 0) return;
    delay_clear(x);
    x->running = 1; x->dwet_cur = 0;
    x->dcur[0] = x->dnew[0] = delay_frames(x, 0, rate, bpm); x->dcur[1] = x->dnew[1] = delay_frames(x, 1, rate, bpm);
    x->xf_left = 0;
  }
  nd[0] = delay_frames(x, 0, rate, bpm); nd[1] = delay_frames(x, 1, rate, bpm);
  if (!x->xf_left && (fabs(nd[0] - x->dcur[0]) > 0.5 || fabs(nd[1] - x->dcur[1]) > 0.5)) {   /* a new time: crossfade */
    x->dnew[0] = nd[0]; x->dnew[1] = nd[1];
    x->xf_len = x->xf_left = (int)(0.02 * rate) > 1 ? (int)(0.02 * rate) : 1;
  }
  for (j = 0; j < n; j++) {
    float s[2];
    x->dwet_cur += (float)clampd(wt - x->dwet_cur, -step, step);
    for (ch = 0; ch < 2; ch++) {
      float in = ch ? r[j] : l[j];
      if (x->xf_left) {
        float a = (float)(x->xf_len - x->xf_left) / (float)x->xf_len;
        s[ch] = tap(x, ch, x->dcur[ch]) * (1 - a) + tap(x, ch, x->dnew[ch]) * a;
      } else s[ch] = tap(x, ch, x->dcur[ch]);
      x->fb_lp[ch] += (float)lpk * (s[ch] - x->fb_lp[ch]);
      x->line[ch][x->w] = in + x->fb_lp[ch] * fb;
    }
    l[j] += s[0] * x->dwet_cur; r[j] += s[1] * x->dwet_cur;
    if (++x->w >= x->len) x->w = 0;
    if (x->xf_left && !--x->xf_left) { x->dcur[0] = x->dnew[0]; x->dcur[1] = x->dnew[1]; }
  }
  if (fabs(x->fb_lp[0]) < 1e-20f) x->fb_lp[0] = 0;
  if (fabs(x->fb_lp[1]) < 1e-20f) x->fb_lp[1] = 0;
  if (wt <= 0 && x->dwet_cur <= 0) { x->running = 0; delay_clear(x); }
}

/* ---- the chain ---- */

SfDsChain *sf_chain_new(const SfFxDesc *fx, int nfx, double rate, int max_block) {
  SfDsChain *c = calloc(1, sizeof *c);
  int i, ch, k;
  (void)max_block;
  if (!c) return NULL;
  c->rate = rate; c->nfx = nfx < SF_FX_MAX ? nfx : SF_FX_MAX; c->was_on = 1;
  for (i = 0; i < c->nfx; i++) {
    Fx *x = &c->fx[i];
    x->d = fx[i];
    if (x->d.type == SF_FX_REVERB) {
      double sc = rate / 44100.0;
      for (ch = 0; ch < 2; ch++) {
        for (k = 0; k < NCOMB; k++) {
          x->comb[ch][k].n = (int)((comb_tun[k] + (ch ? SPREAD : 0)) * sc); if (x->comb[ch][k].n < 1) x->comb[ch][k].n = 1;
          if (!(x->comb[ch][k].b = calloc((size_t)x->comb[ch][k].n, sizeof(float)))) goto fail;
        }
        for (k = 0; k < NAP; k++) {
          x->ap[ch][k].n = (int)((ap_tun[k] + (ch ? SPREAD : 0)) * sc); if (x->ap[ch][k].n < 1) x->ap[ch][k].n = 1;
          if (!(x->ap[ch][k].b = calloc((size_t)x->ap[ch][k].n, sizeof(float)))) goto fail;
        }
      }
    } else if (x->d.type == SF_FX_DELAY) {
      x->len = (int)(DELAY_S * rate) + 4;
      if (!(x->line[0] = calloc((size_t)x->len, sizeof(float))) || !(x->line[1] = calloc((size_t)x->len, sizeof(float)))) goto fail;
    }
  }
  return c;
fail:
  sf_chain_free(c);
  return NULL;
}

void sf_chain_free(SfDsChain *c) {
  int i, ch, k;
  if (!c) return;
  for (i = 0; i < c->nfx; i++) {
    Fx *x = &c->fx[i];
    for (ch = 0; ch < 2; ch++) {
      for (k = 0; k < NCOMB; k++) free(x->comb[ch][k].b);
      for (k = 0; k < NAP; k++) free(x->ap[ch][k].b);
      free(x->line[ch]);
    }
  }
  free(c);
}

void sf_chain_set(SfDsChain *c, int fx, int param, float v) {
  Fx *x;
  if (!c || fx < 0 || fx >= c->nfx || v != v) return;
  x = &c->fx[fx];
  switch (param) {
    case SF_FXP_FREQ: x->d.freq = v; break;
    case SF_FXP_Q: x->d.q = v; break;
    case SF_FXP_WET: x->d.wet = v; break;
    case SF_FXP_ROOM: x->d.room = v; break;
    case SF_FXP_DAMP: x->d.damp = v; break;
    case SF_FXP_TIME: x->d.time = v; break;
    case SF_FXP_FEEDBACK: x->d.feedback = v; break;
    default: break;
  }
}

void sf_chain_clear(SfDsChain *c) {
  int i;
  if (!c) return;
  for (i = 0; i < c->nfx; i++) {
    Fx *x = &c->fx[i];
    x->z1[0] = x->z1[1] = x->z2[0] = x->z2[1] = x->y1[0] = x->y1[1] = 0;
    if (x->d.type == SF_FX_REVERB) { reverb_clear(x); x->running = 0; }
    if (x->d.type == SF_FX_DELAY) { delay_clear(x); x->running = 0; }
  }
}

void sf_chain_process(SfDsChain *c, float *l, float *r, int n, double bpm, int effects_on) {
  int i;
  if (!c || n <= 0) return;
  if (!effects_on && c->was_on) {               /* off: the tails go */
    for (i = 0; i < c->nfx; i++) {
      Fx *x = &c->fx[i];
      if (x->d.type == SF_FX_REVERB) { reverb_clear(x); x->running = 0; }
      if (x->d.type == SF_FX_DELAY) { delay_clear(x); x->running = 0; }
    }
  }
  c->was_on = effects_on;
  for (i = 0; i < c->nfx; i++) {
    Fx *x = &c->fx[i];
    switch (x->d.type) {
      case SF_FX_LP: case SF_FX_LP1: case SF_FX_HP: case SF_FX_BP: filter_run(x, l, r, n, c->rate); break;
      case SF_FX_REVERB: if (effects_on) reverb_run(x, l, r, n, c->rate); break;
      case SF_FX_DELAY: if (effects_on) delay_run(x, l, r, n, c->rate, bpm); break;
      default: break;
    }
  }
}

int sf_chain_idle(const SfDsChain *c, int effects_on) {
  int i;
  if (!c) return 1;
  for (i = 0; i < c->nfx; i++) {
    const Fx *x = &c->fx[i];
    switch (x->d.type) {
      case SF_FX_LP: if (!x->primed || !x->wire || x->f_cur != x->d.freq || x->d.freq < 0.45 * c->rate) return 0; break;
      case SF_FX_LP1: case SF_FX_HP: case SF_FX_BP: return 0;
      case SF_FX_REVERB: case SF_FX_DELAY: if (effects_on && (x->running || x->d.wet > 0)) return 0; break;
      default: break;
    }
  }
  return 1;
}

/* ---- tests ---- */

static int parse_fx(const char *line, SfFxDesc *x) {
  char ty[16];
  const char *p = line;
  int k = 0;
  memset(x, 0, sizeof *x);
  x->freq = 22000; x->q = 0.7f; x->room = 0.7f; x->damp = 0.3f; x->wet = 0.5f; x->time = 0.7f; x->feedback = 0.2f;
  x->fb_cutoff = 22000;
  while (*p == ' ') p++;
  if (!strncmp(p, "fx ", 3)) { p += 3; while (*p && *p != ' ') p++; while (*p == ' ') p++; }
  while (*p && *p != ' ' && k < 15) ty[k++] = *p++;
  ty[k] = 0;
  if (!strcmp(ty, "lp")) x->type = SF_FX_LP; else if (!strcmp(ty, "lp1")) x->type = SF_FX_LP1;
  else if (!strcmp(ty, "hp")) x->type = SF_FX_HP; else if (!strcmp(ty, "bp")) x->type = SF_FX_BP;
  else if (!strcmp(ty, "reverb")) x->type = SF_FX_REVERB; else if (!strcmp(ty, "delay")) x->type = SF_FX_DELAY;
  else return 0;
  while (*p) {
    char key[16]; double v; int n = 0;
    while (*p == ' ') p++;
    while (*p && *p != '=' && *p != ' ' && n < 15) key[n++] = *p++;
    key[n] = 0;
    if (*p != '=') break;
    v = strtod(++p, (char **)&p);
    if (!strcmp(key, "freq")) x->freq = (float)v; else if (!strcmp(key, "q")) x->q = (float)v;
    else if (!strcmp(key, "room")) x->room = (float)v; else if (!strcmp(key, "damp")) x->damp = (float)v;
    else if (!strcmp(key, "wet")) x->wet = (float)v; else if (!strcmp(key, "time")) x->time = (float)v;
    else if (!strcmp(key, "musical")) x->musical = v != 0; else if (!strcmp(key, "feedback")) x->feedback = (float)v;
    else if (!strcmp(key, "offset")) x->offset = (float)v; else if (!strcmp(key, "fbcut")) x->fb_cutoff = (float)v;
  }
  return 1;
}

int smp_dsfx_test(const char *desc, const float *in, float *out_l, float *out_r, int n, double rate, double bpm,
                  const char *sets) {
  static const char *const pn[] = {"freq", "q", "wet", "room", "damp", "time", "feedback"};
  SfFxDesc fx[SF_FX_MAX];
  SfDsChain *c;
  char line[256];
  const char *p = desc;
  int nfx = 0, on = 1, i;
  if (!desc || !in || !out_l || !out_r || n < 0 || !(rate > 0)) return 1;
  while (*p && nfx < SF_FX_MAX) {
    int k = 0;
    while (*p && *p != '\n' && k < 255) line[k++] = *p++;
    line[k] = 0;
    if (*p == '\n') p++;
    if (k && parse_fx(line, &fx[nfx])) nfx++;
  }
  if (!(c = sf_chain_new(fx, nfx, rate, 256))) return 2;
  for (p = sets ? sets : ""; *p;) {
    int k = 0, fi; char pname[16]; float v;
    while (*p && *p != '\n' && k < 255) line[k++] = *p++;
    line[k] = 0;
    if (*p == '\n') p++;
    if (sscanf(line, "effects %d", &fi) == 1) { on = fi != 0; continue; }
    if (sscanf(line, "%d %15s %f", &fi, pname, &v) == 3)
      for (i = 0; i < 7; i++) if (!strcmp(pname, pn[i])) sf_chain_set(c, fi, i, v);
  }
  for (i = 0; i < n; i++) { out_l[i] = in[i]; out_r[i] = in[i]; }
  for (i = 0; i < n; i += 256) sf_chain_process(c, out_l + i, out_r + i, n - i < 256 ? n - i : 256, bpm, on);
  sf_chain_free(c);
  return 0;
}
