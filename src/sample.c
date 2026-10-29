/* sample.c: import a sound file into the disk cache, find its root note, make the reversed copy on demand. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include "samplosis.h"
#include "decode.h"

static volatile long g_decodes;
long smp_debug_decodes(void) { return g_decodes; }

static void w_path(const char *utf8, wchar_t *w, int cap) {
  if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, cap)) w[0] = 0;
}

/* the source's size and last-write time; 0 when it isn't there */
static int stat_src(const char *path, uint64_t *size, uint64_t *mtime) {
  wchar_t w[2048]; WIN32_FILE_ATTRIBUTE_DATA d;
  w_path(path, w, 2048);
  if (!w[0] || !GetFileAttributesExW(w, GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
  *size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
  *mtime = ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
  return 1;
}

/* a raw file's key holds how it is read, so changing the raw settings decodes it again */
static uint64_t src_key_of(const char *path, uint64_t size, uint64_t mtime) {
  char d[1300];
  int k = snprintf(d, sizeof d, "src|%s|%llu|%llu", path, (unsigned long long)size, (unsigned long long)mtime);
  if (smp_is_raw_name(path) && k > 0 && k < (int)sizeof d) {
    SmpSettings s;
    smp_settings_get(&s);
    snprintf(d + k, sizeof d - (size_t)k, "|raw %d %d %d %d", s.raw_rate, s.raw_channels, s.raw_format, s.raw_endian);
  }
  return smp_key(d);
}

static void sidecar_path(uint64_t key, wchar_t *out, int cap) {
  SmpSettings s; wchar_t dir[1100];
  smp_settings_get(&s);
  w_path(s.cache_dir, dir, 1100);
  swprintf(out, cap, L"%s\\%016llx.txt", dir, (unsigned long long)key);
}

/* the sidecar: "root r from f by b" (by, what decoded the file, came later: 0 when it isn't there) */
static int sidecar_read(uint64_t key, int *root, int *from, int *by) {
  wchar_t p[1200]; FILE *f; int ok;
  sidecar_path(key, p, 1200);
  f = _wfopen(p, L"rb");
  if (!f) return 0;
  *by = 0;
  ok = fscanf(f, "root %d from %d by %d", root, from, by) >= 2;
  fclose(f);
  if (*by < SMP_DEC_BUILTIN || *by > SMP_DEC_FFMPEG) *by = 0;
  return ok;
}

static void sidecar_write(uint64_t key, int root, int from, int by) {
  wchar_t p[1200]; FILE *f;
  sidecar_path(key, p, 1200);
  f = _wfopen(p, L"wb");
  if (f) { fprintf(f, "root %d from %d by %d\n", root, from, by); fclose(f); }
}

static int root_of(double m) {
  int r;
  if (m < 0) return 60;
  r = (int)floor(m + 0.5);
  return r < 0 ? 0 : r > 127 ? 127 : r;
}

static const char *base_of(const char *path) {
  const char *b = path, *p;
  for (p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
  return b;
}

static SmpSample *make(const char *path, uint64_t key, SmpEntry *e) {
  SmpSample *s = calloc(1, sizeof *s);
  if (!s) { smp_cache_close(e); return NULL; }
  snprintf(s->path, sizeof s->path, "%s", path);
  snprintf(s->name, sizeof s->name, "%s", base_of(path));
  s->src_key = key; s->fwd = e;
  s->rate = smp_entry_rate(e); s->frames = smp_entry_frames(e);
  return s;
}

/* detect from the first 3 seconds of a cached entry */
static double detect_entry(SmpEntry *e, int rate) {
  uint64_t n = smp_entry_frames(e), want = (uint64_t)rate * 3;
  float *lr, *m; double r = -1; long got, i;
  if (n > want) n = want;
  lr = malloc(sizeof(float) * 2 * (size_t)n); m = malloc(sizeof(float) * (size_t)n);
  if (lr && m) {
    got = smp_entry_read(e, 0, lr, (long)n);
    for (i = 0; i < got; i++) m[i] = 0.5f * (lr[2 * i] + lr[2 * i + 1]);
    r = smp_detect_pitch(m, got, rate);
  }
  free(lr); free(m);
  return r;
}

/* detected: fractional MIDI note from the decode, or -2 when not measured (then the sidecar, then the cache, is used) */
static void find_root(SmpSample *s, const char *path, int use_sidecar, double detected) {
  int n = smp_note_from_filename(path), r, f, by, side = use_sidecar && sidecar_read(s->src_key, &r, &f, &by);
  if (side) s->by = by;                         /* what decoded it, noted at the first load */
  if (n >= 0) { s->root = n; s->root_from = 0; return; }
  if (side) { s->root = r; s->root_from = f; return; }
  if (detected < -1.5) detected = detect_entry(s->fwd, s->rate);
  s->root = root_of(detected); s->root_from = detected < 0 ? 2 : 1;
}

SmpSample *smp_sample_load(const char *path, char *err, int cap) {
  return smp_sample_load_ex(path, NULL, NULL, NULL, err, cap);
}

SmpSample *smp_sample_load_ex(const char *path, int (*cancel)(void *), void (*stage)(void *), void *arg, char *err, int cap) {
  uint64_t size, mtime, key; SmpEntry *e; SmpSample *s;
  double detected = -2; SmpDecBy by = SMP_DEC_BUILTIN;
  if (!path || !stat_src(path, &size, &mtime)) {
    snprintf(err, (size_t)cap, "can't open %s: the file isn't there", path ? path : "(no file)");
    return NULL;
  }
  key = src_key_of(path, size, mtime);
  e = smp_cache_open(key);
  if (e) {
    s = make(path, key, e);
    if (!s) { snprintf(err, (size_t)cap, "out of memory"); return NULL; }
    find_root(s, path, 1, -2);
    return s;
  }
  {
    int ch = 0, rate = 0; long n = 0, i; float *x, *lr;
    g_decodes++;
    x = decode_audio_full(path, &ch, &n, &rate, &by, cancel, stage, arg, err, cap);
    if (!x) return NULL;
    lr = malloc(sizeof(float) * 2 * (size_t)n);
    if (!lr) { free(x); snprintf(err, (size_t)cap, "out of memory reading %s", path); return NULL; }
    {
      const float *L = x, *R = ch > 1 ? x + n : x;
      for (i = 0; i < n; i++) { lr[2 * i] = L[i]; lr[2 * i + 1] = R[i]; }
      if (smp_note_from_filename(path) < 0) {      /* pitch on the mono mix of the first 3 s, mixed in place in x */
        long m = (long)((int64_t)rate * 3 < n ? (int64_t)rate * 3 : n);
        for (i = 0; i < m; i++) x[i] = 0.5f * (lr[2 * i] + lr[2 * i + 1]);
        detected = smp_detect_pitch(x, m, rate);
      }
    }
    free(x);
    if (smp_cache_write(key, lr, (uint64_t)n, rate)) {
      free(lr); snprintf(err, (size_t)cap, "can't write %s to the cache folder", path); return NULL;
    }
    free(lr);
  }
  e = smp_cache_open(key);
  if (!e) { snprintf(err, (size_t)cap, "can't read %s back from the cache folder", path); return NULL; }
  s = make(path, key, e);
  if (!s) { snprintf(err, (size_t)cap, "out of memory"); return NULL; }
  s->by = (int)by;
  find_root(s, path, 0, detected);
  sidecar_write(key, s->root, s->root_from, s->by);
  return s;
}

SmpSample *smp_sample_load_known(const char *path, uint64_t src_key, int root, char *err, int cap) {
  SmpEntry *e = smp_cache_open(src_key); SmpSample *s; int r, f, by = 0;
  if (!e) { snprintf(err, (size_t)cap, "%s is gone and its copy isn't in the cache", path ? path : "(no file)"); return NULL; }
  s = make(path ? path : "", src_key, e);
  if (!s) { snprintf(err, (size_t)cap, "out of memory"); return NULL; }
  s->root = root < 0 ? 0 : root > 127 ? 127 : root;
  s->root_from = sidecar_read(src_key, &r, &f, &by) && r == s->root ? f : 2;
  s->by = by;
  return s;
}

SmpEntry *smp_sample_reversed(SmpSample *s) {
  char d[64]; float *buf, *chunk; uint64_t n, done = 0; const long C = 65536;
  if (!s || !s->fwd) return NULL;
  if (s->rev) return s->rev;
  snprintf(d, sizeof d, "rev|%016llx", (unsigned long long)s->src_key);
  s->rev_key = smp_key(d);
  if ((s->rev = smp_cache_open(s->rev_key))) return s->rev;
  n = s->frames;
  buf = malloc(sizeof(float) * 2 * (size_t)n); chunk = malloc(sizeof(float) * 2 * (size_t)C);
  if (buf && chunk) {
    while (done < n) {                              /* forward data from the end, a chunk at a time */
      uint64_t take = n - done < (uint64_t)C ? n - done : (uint64_t)C, from = n - done - take;
      long got = smp_entry_read(s->fwd, from, chunk, (long)take), i;
      if (got != (long)take) break;
      for (i = 0; i < got; i++) {
        uint64_t o = done + (uint64_t)(got - 1 - i);
        buf[2 * o] = chunk[2 * i]; buf[2 * o + 1] = chunk[2 * i + 1];
      }
      done += take;
    }
    if (done == n && smp_cache_write(s->rev_key, buf, n, s->rate) == 0) s->rev = smp_cache_open(s->rev_key);
  }
  free(buf); free(chunk);
  return s->rev;
}

void smp_sample_free(SmpSample *s) {
  if (!s) return;
  if (s->fwd) smp_cache_close(s->fwd);
  if (s->rev) smp_cache_close(s->rev);
  free(s);
}
