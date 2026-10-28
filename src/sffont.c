/* sffont.c: the process-wide font registry, preset use counts, the loader thread and sample decoding.

   The registry. Every smp_sf_open of the same file (same full path, compared case-insensitively, same size and
   modified time) shares one SmpFont, so many instances of a big font hold one copy of its samples. Only the main
   thread opens and closes. The list is guarded by g_lock, which the loader also takes to walk it; the audio
   thread never touches the lock.

   Use counts. A synth calls smp_sf_use(f, preset, +1 or -1) from any thread, the audio thread included. That is an
   Interlocked add on users[preset] and a SetEvent on the loader, nothing more.

   The loader. One thread for the whole process, at below-normal priority. It starts with the first font and
   stops when the last one closes, so a process with no font open has no thread left to hang on unload. Each pass
   it walks every font: it loads each sample a preset in use needs, publishes it with InterlockedExchangePointer,
   and retires samples no preset in use has needed for the grace time (2 s). It never holds g_lock while it reads
   a file.

   Freeing (the protocol the audio thread relies on). A reader does:
       InterlockedIncrement(&voices[s]);  p = pcm[s];  if (!p) InterlockedDecrement(&voices[s]);
   and InterlockedDecrement(&voices[s]) when it is done with p. The loader does:
       old = InterlockedExchangePointer(&pcm[s], NULL);       (unpublish, into the retire state)
       ... later, on this or a later pass ...
       if (voices[s] == 0) free(old);
   Why this is safe: Interlocked operations are full barriers, so each side's two steps happen in order. A reader
   that got old read pcm[s] before the exchange (after it, pcm[s] is NULL), so its increment came before the
   exchange too, and the loader's read of voices[s], which comes after the exchange, sees it until the reader
   releases. A reader whose increment comes after the exchange reads NULL (or old again, if the loader has
   published it again, and then it is counted the same way). So when the loader sees 0 after the exchange, nobody
   holds old and nobody can get it any more. There is never more than one buffer per sample, published or retired,
   so voices[s] only ever counts holders of that one buffer. A retired sample that is needed again before it is
   freed is published again rather than loaded anew, which also stops quick program changes from thrashing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sf2.h"
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "decode/stb_vorbis.c"   /* declarations only; decode.c compiles it */

#define DEFAULT_GRACE_MS 2000
#define HELD_RECHECK_MS 20                 /* how often a retired sample that someone still holds is checked */
#define MAX_VORBIS_FRAMES (1u << 26)       /* a decoded SF3 sample's limit (128 MB), against lying headers */

static SRWLOCK g_lock = SRWLOCK_INIT;      /* guards the list, g_busy and g_thread */
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;   /* signalled when the loader lets go of a font */
static SmpFont **g_fonts; static int g_nfonts, g_capfonts;
static SmpFont *g_busy;                    /* the font the loader is working on, outside the lock */
static HANDLE g_thread;
static HANDLE g_wake;                      /* auto-reset; made once and never closed, so SetEvent is always safe */
static volatile LONG g_stop;
static volatile LONG g_grace = DEFAULT_GRACE_MS;

void smp_sf_test_grace(int ms) { InterlockedExchange(&g_grace, ms > 0 ? ms : DEFAULT_GRACE_MS); }

/* ---- loading one sample ---- */

static SfPcm *new_pcm(uint32_t frames) {
  size_t hdr = (sizeof(SfPcm) + 15) & ~(size_t)15;
  size_t bytes = hdr + ((size_t)frames + 2 * SF_PCM_PAD) * sizeof(int16_t);
  uint8_t *b = calloc(1, bytes);           /* zeroed: the pads before and after the data stay zero */
  SfPcm *p = (SfPcm *)b;
  if (!b) return NULL;
  p->frames = frames;
  p->bytes = bytes;
  p->data = (int16_t *)(b + hdr) + SF_PCM_PAD;
  return p;
}

/* overwrites a freed buffer's start first, so a read after free shows up in tests instead of finding old data */
static void free_pcm(SfPcm *p) {
  if (!p) return;
  memset(p, 0xDD, p->bytes < 65536 ? (size_t)p->bytes : 65536);
  free(p);
}

static int read_at(HANDLE h, uint64_t off, void *buf, uint64_t n) {
  LARGE_INTEGER li;
  li.QuadPart = (LONGLONG)off;
  if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return 0;
  while (n > 0) {
    DWORD want = n > (1u << 30) ? (1u << 30) : (DWORD)n, got = 0;
    if (!ReadFile(h, buf, want, &got, NULL) || got != want) return 0;
    buf = (uint8_t *)buf + got; n -= got;
  }
  return 1;
}

/* loads sample s; NULL with *broken set when the sample can never load (empty, unreadable or bad Ogg), NULL
   without it when memory ran out (tried again on a later pass) */
static SfPcm *load_sample(SmpFont *f, HANDLE h, int s, int *broken) {
  const SmpSfSample *sh = &f->shdr[s];
  SfPcm *p = NULL;
  uint64_t ls, le;
  *broken = 0;
  if (sh->end <= sh->start) { *broken = 1; return NULL; }   /* start and end were clamped into smpl at open */
  if (sh->type & SF_VORBIS) {
    /* SF3: start and end are byte offsets of an Ogg Vorbis stream in smpl; loop points count decoded frames from
       the stream's start (as MuseScore and Polyphone write them) */
    uint32_t len = sh->end - sh->start; int e = 0, got; unsigned total;
    uint8_t *ogg;
    stb_vorbis *v;
    if (len > 0x7fffffffu) { *broken = 1; return NULL; }
    if (!(ogg = malloc(len))) return NULL;
    if (!read_at(h, f->smpl_off + sh->start, ogg, len)) { free(ogg); *broken = 1; return NULL; }
    v = stb_vorbis_open_memory(ogg, (int)len, &e, NULL);
    if (!v) { free(ogg); *broken = 1; return NULL; }
    total = stb_vorbis_stream_length_in_samples(v);
    if (total == 0 || total > MAX_VORBIS_FRAMES) { stb_vorbis_close(v); free(ogg); *broken = 1; return NULL; }
    p = new_pcm(total);
    if (!p) { stb_vorbis_close(v); free(ogg); return NULL; }
    got = stb_vorbis_get_samples_short_interleaved(v, 1, p->data, (int)total);   /* the first channel, or a mix */
    stb_vorbis_close(v); free(ogg);
    if (got <= 0) { free(p); *broken = 1; return NULL; }
    p->frames = (uint32_t)got;
    ls = sh->loop_start; le = sh->loop_end;
  } else {
    /* SF2: 16-bit little-endian frames in smpl. sm24 is ignored (its lowest 8 bits are inaudible here). */
    uint32_t frames = sh->end - sh->start;
    p = new_pcm(frames);
    if (!p) return NULL;
    if (!read_at(h, f->smpl_off + (uint64_t)sh->start * 2, p->data, (uint64_t)frames * 2)) {
      free(p); *broken = 1; return NULL;
    }
    ls = sh->loop_start > sh->start ? sh->loop_start - sh->start : 0;
    le = sh->loop_end > sh->start ? sh->loop_end - sh->start : 0;
  }
  /* the loop inside the data; a backwards loop becomes an empty one at its end */
  if (ls > p->frames) ls = p->frames;
  if (le > p->frames) le = p->frames;
  if (ls > le) ls = le;
  p->loop_start = (uint32_t)ls; p->loop_end = (uint32_t)le;
  return p;
}

static void publish(SmpFont *f, int s, SfPcm *p) {
  InterlockedExchangePointer((PVOID volatile *)&f->pcm[s], p);
}

/* ---- the loader thread ---- */

/* one pass over font f; returns how long until it wants another pass (INFINITE when only a use() can change
   anything) */
static DWORD pass(SmpFont *f) {
  LONG gen = InterlockedCompareExchange(&f->use_gen, 0, 0);   /* read before the users, so a use() we miss shows */
  ULONGLONG now = GetTickCount64();
  ULONGLONG grace = (ULONGLONG)(DWORD)g_grace;
  DWORD next = INFINITE;
  HANDLE h = INVALID_HANDLE_VALUE;
  int s, p, k, cant_open = 0, aborted = 0;

  memset(f->need, 0, (size_t)f->nshdr);
  for (p = 0; p < f->nphdr; p++) {
    const SmpSfRegion *r; int n;
    if (f->users[p] <= 0) continue;
    n = smp_sf_regions(f, p, &r);
    for (k = 0; k < n; k++) if (r[k].sample >= 0 && r[k].sample < f->nshdr) f->need[r[k].sample] = 1;
  }

  for (s = 0; s < f->nshdr; s++) {
    if (f->closing || g_stop) { aborted = 1; break; }
    if (f->need[s]) {
      f->idle_since[s] = 0;
      if (f->state[s] == SF_ST_RETIRED) {          /* needed again before it was freed: publish it again */
        publish(f, s, f->held[s]);
        f->state[s] = SF_ST_PUBLISHED;
      } else if (f->state[s] == SF_ST_NONE && !cant_open) {
        SfPcm *pc; int broken;
        if (h == INVALID_HANDLE_VALUE) {
          h = f->wpath ? CreateFileW(f->wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL)
                       : INVALID_HANDLE_VALUE;
          if (h == INVALID_HANDLE_VALUE) { cant_open = 1; continue; }   /* tried again on the next use() */
        }
        pc = load_sample(f, h, s, &broken);
        if (pc) {
          f->held[s] = pc;
          InterlockedIncrement(&f->nloaded);
          InterlockedExchangeAdd64(&f->loaded_bytes, (LONG64)pc->bytes);
          publish(f, s, pc);
          f->state[s] = SF_ST_PUBLISHED;
        } else if (broken) {
          f->state[s] = SF_ST_BROKEN;                /* never tried again */
        }
      }
      continue;
    }
    if (f->state[s] == SF_ST_PUBLISHED) {
      ULONGLONG idle;
      if (!f->idle_since[s]) f->idle_since[s] = now;
      idle = now - f->idle_since[s];
      if (idle >= grace) {                           /* unpublish; freed below once nobody holds it */
        publish(f, s, NULL);
        f->state[s] = SF_ST_RETIRED;
        f->idle_since[s] = 0;
      } else if (grace - idle < next) {
        next = (DWORD)(grace - idle);
      }
    }
    if (f->state[s] == SF_ST_RETIRED) {              /* still unneeded (checked above): free it if nobody holds it */
      if (InterlockedCompareExchange(&f->voices[s], 0, 0) == 0) {
        SfPcm *pc = f->held[s];
        f->held[s] = NULL;
        f->state[s] = SF_ST_NONE;
        InterlockedDecrement(&f->nloaded);
        InterlockedExchangeAdd64(&f->loaded_bytes, -(LONG64)pc->bytes);
        free_pcm(pc);
      } else if (HELD_RECHECK_MS < next) {
        next = HELD_RECHECK_MS;
      }
    }
  }
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  if (!aborted) InterlockedExchange(&f->done_gen, gen);
  return next ? next : 1;
}

static DWORD WINAPI loader(void *arg) {
  DWORD wait = INFINITE;
  (void)arg;
  for (;;) {
    DWORD next = INFINITE;
    int i;
    WaitForSingleObject(g_wake, wait);
    if (g_stop) break;
    /* a font closed mid-walk shifts the list; its close sets g_wake, so a skipped font gets the next pass */
    for (i = 0;; i++) {
      SmpFont *f; DWORD t;
      AcquireSRWLockExclusive(&g_lock);
      if (g_stop || i >= g_nfonts) { ReleaseSRWLockExclusive(&g_lock); break; }
      f = g_fonts[i];
      g_busy = f;
      ReleaseSRWLockExclusive(&g_lock);
      t = pass(f);                                   /* without the lock: this reads the file */
      if (t < next) next = t;
      AcquireSRWLockExclusive(&g_lock);
      g_busy = NULL;
      ReleaseSRWLockExclusive(&g_lock);
      WakeAllConditionVariable(&g_cv);
    }
    wait = next;
  }
  return 0;
}

/* ---- the registry ---- */

static void free_font(SmpFont *f) {
  int s;
  if (f->held) for (s = 0; s < f->nshdr; s++) free_pcm(f->held[s]);
  free((void *)f->users); free((void *)f->voices); free((void *)f->pcm);
  free(f->held); free(f->state); free(f->need); free(f->idle_since); free(f->wpath);
  f->wpath = NULL;
  sf_free(f);
}

static int setup(SmpFont *f) {
  size_t np = f->nphdr > 0 ? (size_t)f->nphdr : 1, ns = f->nshdr > 0 ? (size_t)f->nshdr : 1;
  f->users = calloc(np, sizeof(LONG));
  f->voices = calloc(ns, sizeof(LONG));
  f->pcm = calloc(ns, sizeof(SfPcm *));
  f->held = calloc(ns, sizeof(SfPcm *));
  f->state = calloc(ns, 1);
  f->need = calloc(ns, 1);
  f->idle_since = calloc(ns, sizeof(ULONGLONG));
  return f->users && f->voices && f->pcm && f->held && f->state && f->need && f->idle_since;
}

/* the file's full path as a new wide string, or NULL */
static wchar_t *full_path(const char *path) {
  wchar_t w[2048], *full; DWORD n;
  if (!path || !MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048)) return NULL;
  n = GetFullPathNameW(w, 0, NULL, NULL);
  if (!n || !(full = malloc(n * sizeof(wchar_t)))) return NULL;
  if (GetFullPathNameW(w, n, full, NULL) >= n) { free(full); return NULL; }
  return full;
}

SmpFont *smp_sf_open(const char *path, char *err, int cap) {
  wchar_t *full = full_path(path);
  WIN32_FILE_ATTRIBUTE_DATA a;
  SmpFont *f;
  int i;
  if (full && GetFileAttributesExW(full, GetFileExInfoStandard, &a)) {
    uint64_t size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    uint64_t mt = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
    SmpFont *found = NULL;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < g_nfonts; i++) {
      SmpFont *g = g_fonts[i];
      if (g->wpath && g->size == size && g->mtime == mt &&
          CompareStringOrdinal(g->wpath, -1, full, -1, TRUE) == CSTR_EQUAL) { found = g; found->refs++; break; }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (found) { free(full); if (err && cap > 0) err[0] = 0; return found; }
  }
  f = sf_parse(path, err, cap);
  if (!f) { free(full); return NULL; }
  f->wpath = full;
  if (!setup(f)) goto no_memory;
  f->refs = 1;
  AcquireSRWLockExclusive(&g_lock);
  if (g_nfonts == g_capfonts) {
    int nc = g_capfonts ? g_capfonts * 2 : 8;
    SmpFont **nl = realloc(g_fonts, (size_t)nc * sizeof *nl);
    if (!nl) { ReleaseSRWLockExclusive(&g_lock); goto no_memory; }
    g_fonts = nl; g_capfonts = nc;
  }
  if (!g_wake) g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
  if (g_wake && !g_thread) {
    g_thread = CreateThread(NULL, 0, loader, NULL, 0, NULL);
    if (g_thread) SetThreadPriority(g_thread, THREAD_PRIORITY_BELOW_NORMAL);
  }
  if (!g_thread) {
    ReleaseSRWLockExclusive(&g_lock);
    free_font(f);
    if (err && cap > 0) snprintf(err, cap, "can't start loading %s", path);
    return NULL;
  }
  g_fonts[g_nfonts++] = f;
  ReleaseSRWLockExclusive(&g_lock);
  return f;
no_memory:
  free_font(f);
  if (err && cap > 0) snprintf(err, cap, "out of memory opening %s", path);
  return NULL;
}

void smp_sf_close(SmpFont *f) {
  HANDLE t = NULL;
  int i;
  if (!f) return;
  AcquireSRWLockExclusive(&g_lock);
  if (--f->refs > 0) { ReleaseSRWLockExclusive(&g_lock); return; }
  for (i = 0; i < g_nfonts; i++) if (g_fonts[i] == f) {
    memmove(g_fonts + i, g_fonts + i + 1, (size_t)(g_nfonts - i - 1) * sizeof *g_fonts);
    g_nfonts--;
    break;
  }
  InterlockedExchange(&f->closing, 1);              /* a pass on f stops at its next sample */
  while (g_busy == f) SleepConditionVariableSRW(&g_cv, &g_lock, INFINITE, 0);
  if (g_nfonts == 0 && g_thread) {                  /* the last font: the loader stops */
    t = g_thread; g_thread = NULL;
    InterlockedExchange(&g_stop, 1);
  }
  ReleaseSRWLockExclusive(&g_lock);
  if (g_wake) SetEvent(g_wake);                     /* wakes it to stop, or to walk the shifted list again */
  if (t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    InterlockedExchange(&g_stop, 0);
  }
  /* the loader is done with f and it is out of the list; the caller guarantees no synth holds its samples */
  free_font(f);
}

/* ---- preset use and sample data (any thread) ---- */

void smp_sf_use(SmpFont *f, int preset, int delta) {
  if (!f || preset < 0 || preset >= f->nphdr || !delta) return;
  InterlockedExchangeAdd(&f->users[preset], delta);
  InterlockedIncrement(&f->use_gen);
  SetEvent(g_wake);                                 /* made before the first font, never closed */
}

const int16_t *smp_sf_pcm_acquire(SmpFont *f, int sample, uint32_t *frames) {
  SfPcm *p;
  if (frames) *frames = 0;
  if (!f || sample < 0 || sample >= f->nshdr) return NULL;
  InterlockedIncrement(&f->voices[sample]);         /* first count ourselves, then read (see the top comment) */
  p = f->pcm[sample];
  if (!p) { InterlockedDecrement(&f->voices[sample]); return NULL; }
  if (frames) *frames = p->frames;
  return p->data;
}

void smp_sf_pcm_release(SmpFont *f, int sample) {
  if (!f || sample < 0 || sample >= f->nshdr) return;
  InterlockedDecrement(&f->voices[sample]);
}

int smp_sf_pcm_loop(SmpFont *f, int sample, uint32_t *loop_start, uint32_t *loop_end) {
  SfPcm *p;
  if (!f || sample < 0 || sample >= f->nshdr || !(p = f->pcm[sample])) return 0;
  if (loop_start) *loop_start = p->loop_start;
  if (loop_end) *loop_end = p->loop_end;
  return 1;
}

int smp_sf_wait_idle(SmpFont *f, int timeout_ms) {
  ULONGLONG end = GetTickCount64() + (ULONGLONG)(timeout_ms > 0 ? timeout_ms : 0);
  if (!f) return 0;
  for (;;) {
    if (InterlockedCompareExchange(&f->done_gen, 0, 0) == InterlockedCompareExchange(&f->use_gen, 0, 0)) return 0;
    if (GetTickCount64() >= end) return 1;
    Sleep(1);
  }
}

int smp_sf_loaded_samples(const SmpFont *f) { return f ? (int)f->nloaded : 0; }

uint64_t smp_sf_loaded_bytes(const SmpFont *f) { return f ? (uint64_t)f->loaded_bytes : 0; }
