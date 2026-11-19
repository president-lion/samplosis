/* A command-line check of the engine on the Mac (built by CMakeLists.txt as smp_smoke): a generated sine sample
   through the sampler (root from its file name, notes at the right pitch, a keep-length copy), the same sample as an
   SFZ instrument through the SoundFont player, and the instrument text round trip. Uses a folder of its own for the
   settings and the cache, and prints one line per check; exits 1 if any fails. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "samplosis.h"

static int failures;
static void check(int ok, const char *what, const char *detail) {
  printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
  if (!ok) failures++;
}

static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }
static int write_sine(const char *path, double hz, int rate, int frames) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  fwrite("RIFF", 1, 4, f); put32(f, 36 + frames * 2); fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16); put16(f, 1); put16(f, 1); put32(f, (unsigned)rate); put32(f, (unsigned)rate * 2); put16(f, 2); put16(f, 16);
  fwrite("data", 1, 4, f); put32(f, (unsigned)frames * 2);
  for (int i = 0; i < frames; i++) {
    double env = i < 441 ? i / 441.0 : 1.0;
    short s = (short)lrint(12000 * env * sin(2 * M_PI * hz * i / rate));
    put16(f, (unsigned short)s);
  }
  fclose(f);
  return 1;
}

/* pitch of a stretch of mono samples by counting rising zero crossings */
static double freq_of(const float *x, long n, int stride, double rate) {
  long first = -1, last = -1, count = 0;
  for (long i = 1; i < n; i++) {
    float a = x[(i - 1) * stride], b = x[i * stride];
    if (a < 0 && b >= 0) { if (first < 0) first = i; last = i; count++; }
  }
  return count > 1 ? (count - 1) * rate / (double)(last - first) : 0;
}

/* a float32 stereo wav written by smp_engine_render_test: the left channel's frames */
static float *read_wav_f32(const char *path, long *frames) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  unsigned char *b = malloc((size_t)n);
  if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
  fclose(f);
  long at = 12;
  while (at + 8 <= n) {
    long sz = b[at + 4] | b[at + 5] << 8 | b[at + 6] << 16 | (long)b[at + 7] << 24;
    if (!memcmp(b + at, "data", 4)) {
      long fr = sz / 8;
      float *x = malloc(sizeof(float) * (size_t)fr * 2);
      memcpy(x, b + at + 8, (size_t)fr * 8);
      free(b);
      *frames = fr;
      return x;
    }
    at += 8 + sz + (sz & 1);
  }
  free(b);
  return NULL;
}

int main(void) {
  char dir[512], p[1024], err[512] = "", det[256];
  snprintf(dir, sizeof dir, "%s/samplosis_smoke_%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
  mkdir(dir, 0755);
  snprintf(p, sizeof p, "%s/settings.txt", dir);
  setenv("SAMPLOSIS_SETTINGS", p, 1);
  SmpSettings st;
  smp_settings_get(&st);
  snprintf(st.cache_dir, sizeof st.cache_dir, "%s/cache", dir);
  check(smp_settings_set(&st) == 0, "settings saved", p);
  check(smp_cache_ok(err, sizeof err), "cache folder works", err);

  snprintf(p, sizeof p, "%s/Sine A3.wav", dir);
  check(write_sine(p, 220.0, 44100, 44100), "wrote Sine A3.wav", NULL);
  SmpSample *s = smp_sample_load(p, err, sizeof err);
  check(s != NULL, "sample loads", err);
  if (!s) return 1;
  snprintf(det, sizeof det, "root %d from %s", s->root, s->root_from == 0 ? "the name" : "detection");
  check(s->root == 57 && s->root_from == 0, "root note from the file name", det);

  SmpEngine *e = smp_engine_new(48000, 512);
  check(e != NULL, "engine starts", NULL);
  smp_engine_set_sample(e, s);
  int keys[2] = {57, 69};
  double want[2] = {220, 440};
  for (int k = 0; k < 2; k++) {
    char ev[128], out[1024]; long fr = 0;
    snprintf(ev, sizeof ev, "on %d 100 0\noff %d 36000\n", keys[k], keys[k]);
    snprintf(out, sizeof out, "%s/note%d.wav", dir, keys[k]);
    int r = smp_engine_render_test(e, ev, 48000, 512, out);
    float *x = r == 0 ? read_wav_f32(out, &fr) : NULL;
    double hz = x ? freq_of(x + 2 * 2400, 24000, 2, 48000) : 0;
    snprintf(det, sizeof det, "key %d plays %.1f Hz (want %.0f)", keys[k], hz, want[k]);
    check(x && fabs(hz - want[k]) < want[k] * 0.01, "sampler pitch", det);
    free(x);
  }
  check(smp_render_shift(s, 0, 12, 1.0) == 0, "keep-length copy renders (12 semitones up)", NULL);

  int n = smp_script_write(e, NULL, 0);
  char *text = malloc((size_t)n + 1);
  smp_script_write(e, text, n + 1);
  check(n > 0 && strstr(text, "Sine A3.wav") != NULL, "instrument text names the sample", NULL);
  check(smp_script_apply(e, text, err, sizeof err) == 0, "instrument text reads back", err);
  free(text);
  smp_engine_free(e);

  /* the same sample as an SFZ instrument, through the SoundFont player */
  snprintf(p, sizeof p, "%s/sine.sfz", dir);
  FILE *f = fopen(p, "w");
  fprintf(f, "<region> sample=Sine A3.wav lokey=0 hikey=127 pitch_keycenter=57\n");
  fclose(f);
  SmpFont *font = smp_sf_open(p, err, sizeof err);
  check(font != NULL, "SFZ opens", err);
  if (font) {
    snprintf(det, sizeof det, "%d preset(s)", smp_sf_preset_count(font));
    check(smp_sf_preset_count(font) >= 1, "SFZ has a preset", det);
    float *o = calloc(24000 * 2, sizeof(float));
    int v = smp_sf_test_note(font, 0, 69, 100, 48000, 24000, -1, o);
    double hz = freq_of(o + 2 * 2400, 19000, 2, 48000);
    snprintf(det, sizeof det, "%d voice(s), %.1f Hz (want 440)", v, hz);
    check(v >= 1 && fabs(hz - 440) < 5, "SFZ note pitch", det);
    free(o);
    smp_sf_close(font);
  }
  printf("%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
  return failures ? 1 : 0;
}
