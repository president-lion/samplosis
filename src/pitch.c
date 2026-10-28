/* YIN pitch detection (threshold 0.15, 2048-sample windows, 30 Hz to 4 kHz). */
#include <math.h>
#include <stdlib.h>
#include "samplosis.h"

#define WIN 2048
#define THRESH 0.15

static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

/* frequency of one frame starting at x, or -1 */
static double yin_frame(const float *x, int rate, double *d, double *c, int tmax, int tmin) {
  double e = 0;
  for (int i = 0; i < WIN; i++) e += (double)x[i] * x[i];
  if (sqrt(e / WIN) < 1e-3) return -1;
  d[0] = 0;
  for (int t = 1; t <= tmax; t++) {
    double s = 0;
    for (int i = 0; i < WIN; i++) { double v = (double)x[i] - x[i + t]; s += v * v; }
    d[t] = s;
  }
  double run = 0;
  c[0] = 1;
  for (int t = 1; t <= tmax; t++) {
    run += d[t];
    c[t] = run > 1e-12 ? d[t] * t / run : 1;
  }
  for (int t = tmin; t < tmax; t++) {
    if (c[t] < THRESH) {
      while (t + 1 < tmax && c[t + 1] < c[t]) t++;
      double tau = t;
      if (t > 1 && t < tmax) {
        double a = c[t - 1], b = c[t], g = c[t + 1], den = a - 2 * b + g;
        if (den > 1e-12) tau = t + 0.5 * (a - g) / den;
      }
      return rate / tau;
    }
  }
  return -1;
}

SMP_API double smp_detect_pitch(const float *mono, long n, int rate) {
  if (!mono || n <= 0 || rate <= 0) return -1;
  long lim = n < 3L * rate ? n : 3L * rate;
  long span = rate / 2;
  if (span > lim) span = lim;
  int tmin = (int)(rate / 4000.0); if (tmin < 2) tmin = 2;
  int tmax = (int)(rate / 30.0);
  if (span < WIN + tmax + 1) return -1;
  /* loudest span: slide by 1/20 s, comparing sums of squares */
  long hop = rate / 20 > 0 ? rate / 20 : 1, best = 0;
  double bestE = -1;
  for (long p = 0;; p += hop) {
    double e = 0;
    for (long i = p; i < p + span; i++) e += (double)mono[i] * mono[i];
    if (e > bestE) { bestE = e; best = p; }
    if (p + hop + span > lim) break;
  }
  double *d = malloc(sizeof(double) * (tmax + 2)), *c = malloc(sizeof(double) * (tmax + 2));
  double *f = malloc(sizeof(double) * (span / (WIN / 2) + 1));
  int nf = 0;
  if (d && c && f)
    for (long p = best; p + WIN + tmax + 1 <= best + span; p += WIN / 2) {
      double hz = yin_frame(mono + p, rate, d, c, tmax, tmin);
      if (hz >= 30 && hz <= 4000) f[nf++] = hz;
    }
  double r = -1;
  if (nf) {
    qsort(f, nf, sizeof(double), cmp_d);
    double hz = nf & 1 ? f[nf / 2] : 0.5 * (f[nf / 2 - 1] + f[nf / 2]);
    r = 69 + 12 * log2(hz / 440.0);
  }
  free(d); free(c); free(f);
  return r;
}
