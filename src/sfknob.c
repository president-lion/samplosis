/* sfknob.c: the small pieces of a Decent Sampler preset's knob description (sfknob.h) the reader, the player and
   the window share: translations, musical time, freeing, and the tests' text dump. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sf2.h"

void sf_knobs_free(SfKnobs *k) {
  if (!k) return;
  free(k->layer_vol); free(k->layer_tune); free(k->layer_list);
  free(k);
}

float sf_trans(const SfTrans *t, float v, float lo, float hi) {
  int i;
  switch (t->kind) {
    case SF_TR_FIXED: return t->fixed;
    case SF_TR_TABLE:
      if (!t->npts) return v;
      if (v <= t->x[0]) return t->y[0];
      for (i = 1; i < t->npts; i++)
        if (v <= t->x[i]) {
          float dx = t->x[i] - t->x[i - 1];
          return dx > 0 ? t->y[i - 1] + (t->y[i] - t->y[i - 1]) * (v - t->x[i - 1]) / dx : t->y[i];
        }
      return t->y[t->npts - 1];
    default: {
      float x = hi > lo ? (v - lo) / (hi - lo) : 0;
      if (x < 0) x = 0;
      if (x > 1) x = 1;
      return t->lo + (t->hi - t->lo) * x;
    }
  }
}

/* Decent Sampler's musical_time list (decentsamples.com, "How to add tempo-synced delay", 2024): each a note value
   (1/n of a whole note, a whole note being 4 beats), plain, dotted (x 3/2) or triplet (x 2/3) */
static const struct { int n; char kind; } MT[21] = {
  {64, 't'}, {64, 0}, {32, 't'}, {64, 'd'}, {32, 0}, {16, 't'}, {32, 'd'}, {16, 0}, {8, 't'}, {16, 'd'}, {8, 0},
  {4, 't'}, {8, 'd'}, {4, 0}, {2, 't'}, {4, 'd'}, {2, 0}, {1, 't'}, {2, 'd'}, {1, 0}, {1, 'd'},
};

static int mt_clamp(int i) { return i < 0 ? 0 : i > 20 ? 20 : i; }

double sf_mtime_beats(int index) {
  int i = mt_clamp(index);
  double b = 4.0 / MT[i].n;
  return MT[i].kind == 'd' ? b * 1.5 : MT[i].kind == 't' ? b * 2.0 / 3.0 : b;
}

const char *sf_mtime_name(int index) {
  static const char *const names[21] = {
    "1/64 triplet", "1/64", "1/32 triplet", "1/64 dotted", "1/32", "1/16 triplet", "1/32 dotted", "1/16",
    "1/8 triplet", "1/16 dotted", "1/8", "1/4 triplet", "1/8 dotted", "1/4", "1/2 triplet", "1/4 dotted", "1/2",
    "1 triplet", "1/2 dotted", "1", "1 dotted",
  };
  return names[mt_clamp(index)];
}

/* ---- tests: the description as text ---- */

static size_t put(char *out, size_t cap, size_t at, const char *fmt, ...) {
  va_list ap;
  int n;
  if (at >= cap) return at;
  va_start(ap, fmt);
  n = vsnprintf(out + at, cap - at, fmt, ap);
  va_end(ap);
  return n < 0 ? at : at + (size_t)n;
}

static size_t put_trans(char *out, size_t cap, size_t at, const SfTrans *t) {
  int i;
  if (t->kind == SF_TR_FIXED) return put(out, cap, at, "fixed %g", t->fixed);
  if (t->kind == SF_TR_TABLE) {
    at = put(out, cap, at, "table ");
    for (i = 0; i < t->npts; i++) at = put(out, cap, at, "%s%g:%g", i ? "," : "", t->x[i], t->y[i]);
    return at;
  }
  return put(out, cap, at, "linear %g %g", t->lo, t->hi);
}

int smp_sf_test_knobs_text(const SmpFont *f, int preset, char *out, int cap) {
  static const char *const vt[] = {"float", "int", "mtime"};
  static const char *const wd[] = {"number", "percent", "semitones", "mtime", "int"};
  static const char *const tg[] = {"group_vol", "global_vol", "group_tune", "global_tune", "fx"};
  static const char *const pn[] = {"freq", "q", "wet", "room", "damp", "time", "feedback"};
  static const char *const ft[] = {"lp", "lp1", "hp", "bp", "reverb", "delay", "none"};
  const SfKnobs *k;
  size_t at = 0, c = cap > 0 ? (size_t)cap : 0;
  int i, j, l;
  if (!out || cap <= 0) return 0;
  out[0] = 0;
  if (!f || preset < 0 || preset >= f->nphdr || !f->knobs || !(k = f->knobs[f->order[preset]])) return 0;
  for (i = 0; i < k->nknob; i++) {
    const SfKnob *n = &k->knob[i];
    at = put(out, c, at, "knob %d \"%s\" %s %g %g %g %s\n", i + 1, n->label, vt[n->vtype], n->lo, n->hi, n->value, wd[n->words]);
    for (j = 0; j < n->nb; j++) {
      const SfKBind *b = &n->b[j];
      at = put(out, c, at, "  bind %s fx=%d param=%s layers=", tg[b->target], b->fx, b->param >= 0 ? pn[b->param] : "none");
      for (l = 0; l < b->nlayers; l++) at = put(out, c, at, "%s%d", l ? "," : "", k->layer_list[b->first_layer + l]);
      at = put(out, c, at, " ");
      at = put_trans(out, c, at, &b->tr);
      at = put(out, c, at, "\n");
    }
  }
  for (i = 0; i < k->nfx; i++) {
    const SfFxDesc *x = &k->fx[i];
    at = put(out, c, at, "fx %d %s freq=%g q=%g room=%g damp=%g wet=%g time=%g musical=%d feedback=%g offset=%g\n", i,
             ft[x->type], x->freq, x->q, x->room, x->damp, x->wet, x->time, x->musical, x->feedback, x->offset);
  }
  for (i = 0; i < k->ncc; i++) {
    at = put(out, c, at, "cc %d knob=%d ", k->cc[i].cc, k->cc[i].knob + 1);
    at = put_trans(out, c, at, &k->cc[i].tr);
    at = put(out, c, at, "\n");
  }
  at = put(out, c, at, "layers %d\n", k->nlayer);
  return (int)(at < c ? at : c - 1);
}

/* ---- a knob's value in words, and back ---- */

void smp_knob_text(const SmpKnobInfo *k, double v, char *out, int cap) {
  if (!out || cap <= 0) return;
  out[0] = 0;
  if (!k) return;
  switch (k->words) {
    case SF_KW_PERCENT: snprintf(out, (size_t)cap, "%.0f percent", v * 100 + 0.0); break;
    case SF_KW_SEMITONES: {
      double r = k->vtype == SF_KV_FLOAT ? v : floor(v + 0.5), a = fabs(r);
      char n[32];
      if (k->vtype == SF_KV_FLOAT) snprintf(n, sizeof n, "%.2f", a); else snprintf(n, sizeof n, "%.0f", a);
      if (k->vtype == SF_KV_FLOAT) { size_t L = strlen(n); while (L && n[L - 1] == '0') n[--L] = 0; if (L && n[L - 1] == '.') n[--L] = 0; }
      if (r == 0) snprintf(out, (size_t)cap, "0 semitones");
      else snprintf(out, (size_t)cap, "%s %s semitone%s", r > 0 ? "plus" : "minus", n, a == 1 ? "" : "s");
      break;
    }
    case SF_KW_MTIME: snprintf(out, (size_t)cap, "%s", sf_mtime_name((int)floor(v + 0.5))); break;
    case SF_KW_INT: snprintf(out, (size_t)cap, "%.0f", floor(v + 0.5)); break;
    default: {
      size_t L;
      snprintf(out, (size_t)cap, "%.2f", v);
      L = strlen(out);
      while (L && out[L - 1] == '0') out[--L] = 0;
      if (L && out[L - 1] == '.') out[--L] = 0;
      if (!strcmp(out, "-0")) snprintf(out, (size_t)cap, "0");
    }
  }
}

/* text without leading and trailing spaces, lower case, in t (cap bytes) */
static void tidy(const char *in, char *t, size_t cap) {
  size_t n = 0;
  while (*in == ' ' || *in == '\t') in++;
  while (*in && n + 1 < cap) { char c = *in++; t[n++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); }
  while (n && (t[n - 1] == ' ' || t[n - 1] == '\t')) n--;
  t[n] = 0;
}

int smp_knob_parse(const SmpKnobInfo *k, const char *text, double *v) {
  char t[96], *e;
  double x;
  int i, sign = 1;
  if (!k || !text || !v) return 0;
  tidy(text, t, sizeof t);
  if (!t[0]) return 0;
  if (k->words == SF_KW_MTIME) {
    for (i = 0; i <= 20; i++) if (!_stricmp(t, sf_mtime_name(i))) { *v = i; return 1; }
    if (!strcmp(t, "1/1")) { *v = 19; return 1; }
    if (!strcmp(t, "1/1 dotted")) { *v = 20; return 1; }
    if (!strcmp(t, "1/1 triplet")) { *v = 17; return 1; }
  }
  {
    char *p = t;
    if (!strncmp(p, "plus ", 5)) p += 5;
    else if (!strncmp(p, "minus ", 6)) { p += 6; sign = -1; }
    x = strtod(p, &e);
    if (e == p || !isfinite(x)) return 0;
    while (*e == ' ') e++;
    if (k->words == SF_KW_PERCENT) {
      if (*e == '%') e++;
      else if (!strncmp(e, "percent", 7)) e += 7;
      x /= 100;
    } else if (k->words == SF_KW_SEMITONES) {
      if (!strncmp(e, "semitones", 9)) e += 9;
      else if (!strncmp(e, "semitone", 8)) e += 8;
    }
    while (*e == ' ') e++;
    if (*e) return 0;
    *v = sign * x;
    return 1;
  }
}
