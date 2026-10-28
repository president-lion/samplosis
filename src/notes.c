/* Note names, the parameter table, and values in words. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "samplosis.h"

static const SmpParamInfo PARAMS[SMP_NPARAMS] = {
  {"root", 0, 127, 60, SMP_K_NOTE},
  {"transpose", -48, 48, 0, SMP_K_SEMI},
  {"fine", -100, 100, 0, SMP_K_CENT},
  {"volume", -60, 12, 0, SMP_K_DB},
  {"pan", -100, 100, 0, SMP_K_PAN},
  {"attack", 0, 10000, 0, SMP_K_MS},
  {"decay", 0, 10000, 0, SMP_K_MS},
  {"sustain", -60, 0, 0, SMP_K_DB},
  {"release", 0, 10000, 30, SMP_K_MS},
  {"velocity_sens", 0, 100, 100, SMP_K_PERCENT},
  {"min_volume", -60, 0, -60, SMP_K_DB},
  {"mode", 0, 2, 1, SMP_K_CHOICE},
  {"note_lo", 0, 127, 0, SMP_K_NOTE},
  {"note_hi", 0, 127, 127, SMP_K_NOTE},
  {"pitch_lo", -48, 48, -12, SMP_K_SEMI},
  {"vel_lo", 1, 127, 1, SMP_K_COUNT},
  {"vel_hi", 1, 127, 127, SMP_K_COUNT},
  {"obey_note_off", 0, 1, 1, SMP_K_BOOL},
  {"loop", 0, 2, 0, SMP_K_CHOICE},
  {"loop_start", 0, 100, 0, SMP_K_PERCENT},
  {"start", 0, 100, 0, SMP_K_PERCENT},
  {"end", 0, 100, 100, SMP_K_PERCENT},
  {"bend_range", 0, 48, 2, SMP_K_SEMI},
  {"pitch_hi", -48, 48, 12, SMP_K_SEMI},
  {"reverse", 0, 1, 0, SMP_K_BOOL},
  {"keep_length", 0, 1, 1, SMP_K_BOOL},
  {"voices", 1, 64, 16, SMP_K_COUNT},
  {"probability", 0, 100, 100, SMP_K_PERCENT},
  {"speed", 25, 400, 100, SMP_K_PERCENT},
};

SMP_API const SmpParamInfo *smp_param_info(int id) {
  return id >= 0 && id < SMP_NPARAMS ? &PARAMS[id] : NULL;
}

/* ---- note names ---- */
static const int PC[7] = {9, 11, 0, 2, 4, 5, 7}; /* A B C D E F G */
static const char *const NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/* parses a note with octave in [s, end); returns -1 on failure */
static int parse_note_span(const char *s, const char *end) {
  if (s >= end) return -1;
  int c = toupper((unsigned char)*s);
  if (c < 'A' || c > 'G') return -1;
  int pc = PC[c - 'A'];
  s++;
  if (s < end && *s == '#') { pc++; s++; }
  else if (s < end && (*s == 'b' || *s == 'B') && s + 1 < end) { pc--; s++; }
  int neg = 0;
  if (s < end && *s == '-') { neg = 1; s++; }
  if (s >= end) return -1;
  int oct = 0, digits = 0;
  for (; s < end; s++) {
    if (!isdigit((unsigned char)*s) || ++digits > 2) return -1;
    oct = oct * 10 + (*s - '0');
  }
  if (neg) oct = -oct;
  int n = (oct + 1) * 12 + pc;
  return n >= 0 && n <= 127 ? n : -1;
}

SMP_API int smp_note_parse(const char *s) {
  if (!s) return -1;
  while (isspace((unsigned char)*s)) s++;
  const char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) e--;
  if (e == s) return -1;
  if (isdigit((unsigned char)*s)) {
    int v = 0;
    for (const char *p = s; p < e; p++) {
      if (!isdigit((unsigned char)*p)) return -1;
      v = v * 10 + (*p - '0');
      if (v > 999) return -1;
    }
    return v <= 127 ? v : -1;
  }
  return parse_note_span(s, e);
}

SMP_API void smp_note_name(int n, char *out, int cap) {
  if (cap <= 0) return;
  if (n < 0) n = 0;
  if (n > 127) n = 127;
  snprintf(out, cap, "%s%d", NAMES[n % 12], n / 12 - 1);
}

static int is_sep(char c) {
  return c == ' ' || c == '_' || c == '-' || c == '.' || c == '(' || c == ')' || c == '[' || c == ']' ||
         c == '{' || c == '}';
}

static int token_note(const char *s, const char *e) {
  if (e - s == 3 && !strncmp(s, "440", 3)) return 69;
  if (e - s == 4 && toupper((unsigned char)s[0]) == 'A' && !strncmp(s + 1, "440", 3)) return 69;
  if (e - s < 2 || isdigit((unsigned char)s[0])) return -1;
  return parse_note_span(s, e); /* '-' is a separator, so filename octaves are never negative */
}

SMP_API int smp_note_from_filename(const char *path) {
  if (!path) return -1;
  const char *base = path;
  for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
  size_t len = strlen(base);
  const char *dot = NULL;
  for (const char *p = base; *p; p++) if (*p == '.') dot = p;
  if (dot && dot > base && strlen(dot) <= 6) len = (size_t)(dot - base);
  int found = -1;
  size_t i = 0;
  while (i < len) {
    while (i < len && is_sep(base[i])) i++;
    size_t j = i;
    while (j < len && !is_sep(base[j])) j++;
    if (j > i) {
      int n = token_note(base + i, base + j);
      if (n >= 0) found = n;
    }
    i = j;
  }
  return found;
}

/* ---- values in words ---- */
static const char *const MODE_NAMES[] = {"sample", "note", "free"};
static const char *const LOOP_NAMES[] = {"off", "forward", "pingpong"};
static const char *const BOOL_NAMES[] = {"off", "on"};

static const char *const *choice_names(int param, int *count) {
  if (param == SMP_P_MODE) { *count = 3; return MODE_NAMES; }
  if (param == SMP_P_LOOP) { *count = 3; return LOOP_NAMES; }
  *count = 2;
  return BOOL_NAMES;
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

SMP_API int smp_value_text(int param, double v, char *out, int cap) {
  const SmpParamInfo *pi = smp_param_info(param);
  if (!out || cap <= 0) return 0;
  if (!pi) { out[0] = 0; return 0; }
  v = clampd(v, pi->lo, pi->hi);
  int r;
  switch (pi->kind) {
  case SMP_K_NOTE: {
    char b[16]; smp_note_name((int)floor(v + 0.5), b, 16);
    r = snprintf(out, cap, "%s", b); break;
  }
  case SMP_K_SEMI: {
    int k = (int)floor(v + 0.5);
    r = pi->lo < 0 ? snprintf(out, cap, "%+d st", k) : snprintf(out, cap, "%d st", k); break;
  }
  case SMP_K_CENT: r = snprintf(out, cap, "%d cents", (int)floor(v + 0.5)); break;
  case SMP_K_DB:
    if (v <= pi->lo + 0.05 && pi->lo <= -60) r = snprintf(out, cap, "off");
    else r = snprintf(out, cap, "%.1f dB", v);
    break;
  case SMP_K_PAN: {
    int k = (int)floor(v + 0.5);
    r = k == 0 ? snprintf(out, cap, "center") : snprintf(out, cap, "%d %s", abs(k), k < 0 ? "left" : "right");
    break;
  }
  case SMP_K_MS: {
    double m = floor(v + 0.5);
    r = m < 1000 ? snprintf(out, cap, "%.0f ms", m) : snprintf(out, cap, "%.2f s", v / 1000.0);
    break;
  }
  case SMP_K_PERCENT:
  case SMP_K_POSITION: r = snprintf(out, cap, "%.1f %%", v); break;
  case SMP_K_CHOICE:
  case SMP_K_BOOL: {
    int n; const char *const *nm = choice_names(param, &n);
    int k = (int)floor(v + 0.5); if (k < 0) k = 0; if (k >= n) k = n - 1;
    r = snprintf(out, cap, "%s", nm[k]); break;
  }
  default: r = snprintf(out, cap, "%d", (int)floor(v + 0.5)); break;
  }
  if (r >= cap) r = cap - 1;
  return r < 0 ? 0 : r;
}

static int ieq(const char *a, const char *b) {
  for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
  return !*a && !*b;
}

SMP_API int smp_value_parse(int param, const char *s, double *v) {
  const SmpParamInfo *pi = smp_param_info(param);
  if (!pi || !s || !v) return 0;
  while (isspace((unsigned char)*s)) s++;
  char buf[64];
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) n--;
  if (!n || n >= sizeof buf) return 0;
  memcpy(buf, s, n); buf[n] = 0;
  double r;
  int kind = pi->kind;
  if (kind == SMP_K_NOTE) {
    int k = smp_note_parse(buf);
    if (k < 0) return 0;
    r = k;
  } else if (kind == SMP_K_CHOICE || kind == SMP_K_BOOL) {
    int cnt; const char *const *nm = choice_names(param, &cnt);
    int k = -1;
    for (int i = 0; i < cnt; i++) if (ieq(buf, nm[i])) k = i;
    if (k < 0 && kind == SMP_K_BOOL) {
      if (ieq(buf, "true") || ieq(buf, "yes")) k = 1;
      else if (ieq(buf, "false") || ieq(buf, "no")) k = 0;
    }
    if (k < 0) {
      char *e; double d = strtod(buf, &e);
      if (e == buf || *e) return 0;
      k = (int)floor(d + 0.5);
    }
    r = k;
  } else {
    if (kind == SMP_K_PAN && ieq(buf, "center")) { *v = 0; return 1; }
    if (kind == SMP_K_DB && ieq(buf, "off") && pi->lo <= -60) { *v = pi->lo; return 1; }
    char *e; double d = strtod(buf, &e);
    if (e == buf) return 0;
    while (isspace((unsigned char)*e)) e++;
    r = d;
    switch (kind) {
    case SMP_K_SEMI: if (*e && !ieq(e, "st") && !ieq(e, "semitones") && !ieq(e, "semitone")) return 0; break;
    case SMP_K_CENT: if (*e && !ieq(e, "cents") && !ieq(e, "cent") && !ieq(e, "ct")) return 0; break;
    case SMP_K_DB: if (*e && !ieq(e, "db")) return 0; break;
    case SMP_K_PAN:
      if (ieq(e, "left")) r = -fabs(d);
      else if (ieq(e, "right")) r = fabs(d);
      else if (*e) return 0;
      break;
    case SMP_K_MS:
      if (ieq(e, "s") || ieq(e, "sec")) r = d * 1000;
      else if (*e && !ieq(e, "ms")) return 0;
      break;
    case SMP_K_PERCENT: case SMP_K_POSITION: if (*e && strcmp(e, "%")) return 0; break;
    default: if (*e) return 0; break;
    }
  }
  *v = clampd(r, pi->lo, pi->hi);
  return 1;
}
