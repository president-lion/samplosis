/* On-disk sample cache: <16 hex>.smpc = 32 byte header + float32 stereo interleaved. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "samplosis.h"

#define HDR 32
#define MAX_OPEN 4096

struct SmpEntry {
  HANDLE h;
  uint64_t key, frames;
  int rate, whole;
  float *head;
  uint64_t head_frames;
};

static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION cs;
static uint64_t open_keys[MAX_OPEN];
static int open_n;

static BOOL CALLBACK init_cs(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&cs); return TRUE; }
static void lock(void) { InitOnceExecuteOnce(&once, init_cs, NULL, NULL); EnterCriticalSection(&cs); }

static int is_open_locked(uint64_t key) {
  for (int i = 0; i < open_n; i++) if (open_keys[i] == key) return 1;
  return 0;
}

uint64_t smp_key(const char *d) {
  uint64_t h = 1469598103934665603ULL;
  for (; *d; d++) { h ^= (unsigned char)*d; h *= 1099511628211ULL; }
  return h;
}

static int dir_w(wchar_t *out, int cap) {
  SmpSettings s;
  smp_settings_get(&s);
  int n = MultiByteToWideChar(CP_UTF8, 0, s.cache_dir, -1, out, cap - 40);
  if (n <= 0) return 0;
  n--;
  while (n > 3 && (out[n - 1] == L'\\' || out[n - 1] == L'/')) out[--n] = 0;
  return 1;
}

static int file_w(uint64_t key, const wchar_t *ext, wchar_t *out, int cap) {
  if (!dir_w(out, cap)) return 0;
  size_t n = wcslen(out);
  swprintf(out + n, cap - n, L"\\%016llx%s", (unsigned long long)key, ext);
  return 1;
}

static void make_dirs(wchar_t *p) { /* creates the folder p itself, and its parents */
  for (wchar_t *c = p + 1; *c; c++)
    if (*c == L'\\' || *c == L'/') { wchar_t t = *c; *c = 0; CreateDirectoryW(p, NULL); *c = t; }
  CreateDirectoryW(p, NULL);
}

static int write_all(HANDLE h, const void *buf, uint64_t n) {
  const char *p = buf;
  while (n) {
    DWORD chunk = n > (1u << 26) ? (1u << 26) : (DWORD)n, w = 0;
    if (!WriteFile(h, p, chunk, &w, NULL) || w != chunk) return 0;
    p += chunk; n -= chunk;
  }
  return 1;
}

int smp_cache_write(uint64_t key, const float *lr, uint64_t frames, int rate) {
  wchar_t dir[1100], part[1200], fin[1200];
  static volatile LONG counter;
  wchar_t ext[64];
  swprintf(ext, 64, L".%lu.%lu.%ld.part", GetCurrentProcessId(), GetCurrentThreadId(), InterlockedIncrement(&counter));
  if (!dir_w(dir, 1100) || !file_w(key, L".smpc", fin, 1200) || !file_w(key, ext, part, 1200)) return 1;
  make_dirs(dir);
  HANDLE h = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return 2;
  unsigned char hdr[HDR] = {0};
  uint32_t v = 1, ch = 2, r = (uint32_t)rate;
  memcpy(hdr, "SMPC", 4); memcpy(hdr + 4, &v, 4); memcpy(hdr + 8, &ch, 4); memcpy(hdr + 12, &r, 4); memcpy(hdr + 16, &frames, 8);
  int ok = write_all(h, hdr, HDR) && write_all(h, lr, frames * 8);
  CloseHandle(h);
  if (ok && MoveFileExW(part, fin, MOVEFILE_REPLACE_EXISTING)) return 0;
  DeleteFileW(part);
  if (ok) { /* keys are content-addressed: a complete file already there (maybe held open) is as good as ours */
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExW(fin, GetFileExInfoStandard, &d) &&
        (((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow) == HDR + frames * 8) return 0;
  }
  return 3;
}

static int read_at(HANDLE h, uint64_t off, void *buf, DWORD n, DWORD *got) {
  OVERLAPPED o = {0};
  o.Offset = (DWORD)off; o.OffsetHigh = (DWORD)(off >> 32);
  *got = 0;
  return ReadFile(h, buf, n, got, &o) != 0;
}

/* whole_div: the entry is held whole when it is at most rate / whole_div frames long */
static SmpEntry *open_entry(uint64_t key, int whole_div) {
  wchar_t fn[1200];
  if (!file_w(key, L".smpc", fn, 1200)) return NULL;
  HANDLE h = CreateFileW(fn, GENERIC_READ | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return NULL;
  unsigned char hdr[HDR]; DWORD got;
  uint32_t ver, ch, rate; uint64_t frames;
  if (!read_at(h, 0, hdr, HDR, &got) || got != HDR || memcmp(hdr, "SMPC", 4)) { CloseHandle(h); return NULL; }
  memcpy(&ver, hdr + 4, 4); memcpy(&ch, hdr + 8, 4); memcpy(&rate, hdr + 12, 4); memcpy(&frames, hdr + 16, 8);
  LARGE_INTEGER sz;
  if (ver != 1 || ch != 2 || !GetFileSizeEx(h, &sz) || (uint64_t)sz.QuadPart < HDR + frames * 8) { CloseHandle(h); return NULL; }
  SmpEntry *e = calloc(1, sizeof *e);
  if (!e) { CloseHandle(h); return NULL; }
  e->h = h; e->key = key; e->frames = frames; e->rate = (int)rate;
  uint64_t hf = (uint64_t)rate / 4;
  if (frames <= (uint64_t)rate / (uint64_t)whole_div) { hf = frames; e->whole = 1; }
  else if (hf > frames) hf = frames;
  e->head_frames = hf;
  e->head = malloc((size_t)(hf ? hf * 8 : 8));
  DWORD want = (DWORD)(hf * 8);
  if (!e->head || (want && (!read_at(h, HDR, e->head, want, &got) || got != want))) {
    free(e->head); free(e); CloseHandle(h); return NULL;
  }
  FILETIME ft; GetSystemTimeAsFileTime(&ft);
  SetFileTime(h, NULL, NULL, &ft);
  lock();
  if (open_n < MAX_OPEN) open_keys[open_n++] = key;
  LeaveCriticalSection(&cs);
  return e;
}

SmpEntry *smp_cache_open(uint64_t key) { return open_entry(key, 1); }       /* imported files: whole up to 1 s */
SmpEntry *smp_cache_open_copy(uint64_t key) { return open_entry(key, 4); }  /* keep-length copies: whole up to 0.25 s */

void smp_cache_close(SmpEntry *e) {
  if (!e) return;
  lock();
  for (int i = 0; i < open_n; i++) if (open_keys[i] == e->key) { open_keys[i] = open_keys[--open_n]; break; }
  LeaveCriticalSection(&cs);
  CloseHandle(e->h);
  free(e->head);
  free(e);
}

uint64_t smp_entry_frames(const SmpEntry *e) { return e->frames; }
int smp_entry_rate(const SmpEntry *e) { return e->rate; }
int smp_entry_whole(const SmpEntry *e) { return e->whole; }
const float *smp_entry_head(const SmpEntry *e, uint64_t *hf) { if (hf) *hf = e->head_frames; return e->head; }

long smp_entry_read(SmpEntry *e, uint64_t frame, float *lr, long frames) {
  if (frame >= e->frames || frames <= 0) return 0;
  uint64_t n = e->frames - frame;
  if (n > (uint64_t)frames) n = (uint64_t)frames;
  DWORD got;
  if (!read_at(e->h, HDR + frame * 8, lr, (DWORD)(n * 8), &got)) return 0;
  return (long)(got / 8);
}

typedef struct { uint64_t key, size, time; } Item;
static int by_time(const void *a, const void *b) {
  const Item *x = a, *y = b;
  return x->time < y->time ? -1 : x->time > y->time;
}

/* .part files left by a crash (a write in progress is never an hour old) */
static void trim_parts(const wchar_t *dir) {
  wchar_t pat[1200], fn[1200];
  WIN32_FIND_DATAW fd;
  FILETIME now; uint64_t t, hour = 3600ULL * 10000000ULL;
  HANDLE f;
  swprintf(pat, 1200, L"%s\\*.part", dir);
  f = FindFirstFileW(pat, &fd);
  if (f == INVALID_HANDLE_VALUE) return;
  GetSystemTimeAsFileTime(&now);
  t = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
  do {
    uint64_t w = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || w + hour > t) continue;
    swprintf(fn, 1200, L"%s\\%s", dir, fd.cFileName);
    DeleteFileW(fn);
  } while (FindNextFileW(f, &fd));
  FindClose(f);
}

uint64_t smp_cache_trim(uint64_t limit) {
  wchar_t dir[1100], pat[1200];
  if (!dir_w(dir, 1100)) return 0;
  trim_parts(dir);
  swprintf(pat, 1200, L"%s\\*.smpc", dir);
  Item *items = NULL; size_t n = 0, cap = 0;
  uint64_t total = 0, pinned = 0; /* open files are never deleted and do not count against the limit */
  WIN32_FIND_DATAW fd;
  HANDLE f = FindFirstFileW(pat, &fd);
  if (f != INVALID_HANDLE_VALUE) {
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      if (n == cap) { cap = cap ? cap * 2 : 256; items = realloc(items, cap * sizeof *items); if (!items) { FindClose(f); return 0; } }
      items[n].key = wcstoull(fd.cFileName, NULL, 16);
      items[n].size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
      items[n].time = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
      total += items[n].size; n++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
  }
  qsort(items, n, sizeof *items, by_time);
  lock();
  for (size_t i = 0; i < n; i++)
    if (is_open_locked(items[i].key)) { pinned += items[i].size; total -= items[i].size; items[i].size = 0; items[i].time = 0; }
  for (size_t i = 0; i < n && total > limit; i++) {
    if (!items[i].size) continue;
    wchar_t fn[1200];
    file_w(items[i].key, L".smpc", fn, 1200);
    if (DeleteFileW(fn)) total -= items[i].size;
  }
  LeaveCriticalSection(&cs);
  free(items);
  return total + pinned;
}

int smp_cache_ok(char *why, int cap) {
  wchar_t dir[1100], probe[1200];
  static volatile LONG counter;
  SmpSettings s;
  smp_settings_get(&s);
  if (dir_w(dir, 1100)) {
    make_dirs(dir);   /* a probe name of its own: instances checking at once never collide */
    swprintf(probe, 1200, L"%s\\.probe.%lu.%lu.%ld", dir, GetCurrentProcessId(), GetCurrentThreadId(), InterlockedIncrement(&counter));
    HANDLE h = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return 1; }
    DWORD a = GetFileAttributesW(dir);
    if (why && cap > 0) snprintf(why, cap, a == INVALID_FILE_ATTRIBUTES ? "the cache folder %s can't be created" : "the cache folder %s can't be written to", s.cache_dir);
  } else if (why && cap > 0) snprintf(why, cap, "the cache folder %s isn't a valid path", s.cache_dir);
  return 0;
}

uint64_t smp_cache_used_bytes(void) {
  wchar_t dir[1100], pat[1200];
  WIN32_FIND_DATAW fd;
  uint64_t total = 0;
  if (!dir_w(dir, 1100)) return 0;
  swprintf(pat, 1200, L"%s\\*.smpc", dir);
  HANDLE f = FindFirstFileW(pat, &fd);
  if (f == INVALID_HANDLE_VALUE) return 0;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
  } while (FindNextFileW(f, &fd));
  FindClose(f);
  return total;
}
