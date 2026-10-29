/* mfdecode.c: the Windows stage of the decode chain. A file that no built-in or library decoder reads is offered to
   Windows Media Foundation, which reads AAC and HE-AAC (.m4a, .mp4, .aac), WMA (.wma, .asf), and on most Windows 10
   and 11 machines ALAC, AMR-NB (.3gp, .amr) and others. What it has differs by machine: Windows 11 24H2 removed the
   AC-3 decoder, and the N editions have no Media Foundation unless the Media Feature Pack is installed.

   A Source Reader opens the file by its path (whatever its name ends with: Windows looks at the bytes too), reads the
   first sound stream only, and asks for 32-bit float at the stream's own rate and channels. Lossy coders' start
   delay is dropped when Windows marks it (a first sample whose time is before zero). Windows doesn't apply an MPEG-4
   file's edit list, so smp_mp4_edit reads it from the file's bytes and decode.c cuts the start delay (1024 frames for
   AAC) and the padding at the end by it.

   Never hangs. Media Foundation's calls are synchronous, and a wedged decoder or a dead USB or network drive would
   block them for good, so the work runs on a helper thread. The helper owns everything it makes (its COM, the
   reader, its own growing buffer). The caller waits for it with a stall timer (60 s with no progress: no sample
   decoded) and asks cancel now and then. On the stall timer or a cancel, the caller marks the job
   abandoned and returns at once with a plain reason; the helper sees the mark between samples, or whenever its
   stuck call finally returns, and frees the job itself. Who owns the job is settled by one interlocked exchange of
   its state (running to done by the helper, running to abandoned by the caller), so the helper never touches the
   caller's memory: it hands over its result only by winning that exchange. Each helper holds a reference on this
   DLL (taken before it starts, dropped by FreeLibraryAndExitThread as it ends), so a host that unloads the plugin
   while an abandoned helper is still stuck inside Windows doesn't pull the code out from under it.

   Media Foundation's DLLs are delay-loaded and looked for first, so the plugin still loads where they're missing.
   Starting it (CoIncrementMTAUsage once, then MFStartup) happens once per process when it works, and is tried again
   on the next load when it doesn't; it is never undone (process exit cleans up; never from DllMain). One helper at
   a time tries; another that comes meanwhile waits up to 5 s and then gives up ("busy or stuck"), so stuck starts
   can't pile up helpers.

   Called from the main thread or worker threads (the sampler's loader), never from the audio thread: it allocates,
   reads the disk and waits. Files only, never bytes in memory (decode_memory doesn't come here). */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "decode.h"
#include "mfdecode.h"

/* TESTS ONLY (smp_test_only_decode_flags passes them on): the bits of SMP_MF_TEST_* in mfdecode.h */
static volatile LONG test_flags;
void smp_mf_set_test_flags(long f) { InterlockedExchange(&test_flags, (LONG)f); }
static volatile LONG helpers;                      /* helper threads still running, for the tests */
SMP_API long smp_test_only_mf_helpers(void) { return InterlockedCompareExchange(&helpers, 0, 0); }

/* Media Foundation, started once per process when it can be; a failure is tried again next time. mf_state: 0 not
   started (or failed), 1 a helper is starting it, 2 started. */
enum { MF_IDLE, MF_STARTING, MF_STARTED };
static volatile LONG mf_state;
static int mta_held;                               /* only the helper that set MF_STARTING touches it */
static int mf_start(void) {
  HMODULE plat = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  HMODULE rw = plat ? LoadLibraryExW(L"mfreadwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32) : NULL;
  int ok = 0;
  if (plat && rw) {                                /* the delay-loaded DLLs are there before anything in them is called */
    CO_MTA_USAGE_COOKIE cookie;                    /* keeps the process's MTA alive between our helpers */
    if (!mta_held && SUCCEEDED(CoIncrementMTAUsage(&cookie))) mta_held = 1;
    ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
  }
  if (!ok) {                                       /* nothing kept: the next load tries again */
    if (rw) FreeLibrary(rw);
    if (plat) FreeLibrary(plat);
  }
  return ok;
}
/* SMP_MF_OK when Media Foundation is ready, SMP_MF_NO_MF, or SMP_MF_BUSY (another helper's start is stuck) */
static int mf_ready(long flags) {
  ULONGLONG began = GetTickCount64(), wait = flags & SMP_MF_TEST_BUSY ? 200 : 5000;
  if (flags & SMP_MF_TEST_NO_MF) return SMP_MF_NO_MF;
  for (;;) {
    LONG st = flags & SMP_MF_TEST_BUSY ? MF_STARTING : InterlockedCompareExchange(&mf_state, MF_STARTING, MF_IDLE);
    if (st == MF_STARTED) return SMP_MF_OK;
    if (st == MF_IDLE) {                           /* this helper starts it */
      int ok = mf_start();
      InterlockedExchange(&mf_state, ok ? MF_STARTED : MF_IDLE);
      return ok ? SMP_MF_OK : SMP_MF_NO_MF;
    }
    if (GetTickCount64() - began > wait) return SMP_MF_BUSY;
    Sleep(20);
  }
}

/* One decode. The helper writes the results; the caller reads them only after the helper set state to DONE. */
enum { JOB_RUNNING, JOB_DONE, JOB_ABANDONED };
typedef struct {
  volatile LONG state;
  volatile LONG progress;                          /* counts up as the helper gets on: the caller's stall timer */
  wchar_t *path;
  size_t max_floats;
  long flags;
  int res, trimmed;
  unsigned ch, sr;
  float *x; size_t n, cap;                         /* interleaved, floats */
  char err[300];
} Job;
static void job_free(Job *j) { free(j->x); free(j->path); free(j); }
static int abandoned(Job *j) { return InterlockedCompareExchange(&j->state, JOB_ABANDONED, JOB_ABANDONED) == JOB_ABANDONED; }

/* room for more floats in the job's buffer: 1, or 0 (too long: res says so; or out of memory) */
static int job_need(Job *j, size_t more) {
  if (more > j->max_floats || j->n + more > j->max_floats) { j->res = SMP_MF_TOO_LONG; return 0; }
  if (j->n + more > j->cap) {
    size_t c = j->cap ? j->cap : 65536;
    float *y;
    while (c < j->n + more) c *= 2;
    if (c > j->max_floats) c = j->max_floats;
    if (!(y = realloc(j->x, c * sizeof(float)))) { snprintf(j->err, sizeof j->err, "out of memory"); j->res = SMP_MF_FAILED; return 0; }
    j->x = y; j->cap = c;
  }
  return 1;
}

/* a name for a sound coder Windows may lack, by its wave format tag (the first part of its subtype), or NULL */
static const char *coder_name(const GUID *g) {
  switch (g->Data1) {
  case 0x2000: case 0xE06D802C: return "AC-3 (Dolby Digital)";
  case 0xA7FB87AF: case 0xE06D8033: return "E-AC-3 (Dolby Digital Plus)";
  case 0x6C61: return "ALAC (Apple Lossless)";
  case 0x7361: return "AMR-NB";
  case 0x7262: return "AMR-WB";
  case 0xF1AC: return "FLAC";
  case 0x704F: return "Opus";
  case 0x2001: return "DTS";
  default: return NULL;
  }
}

/* the reader's output now: channels and rate, 1 when they're usable */
static int out_format(IMFSourceReader *r, unsigned *ch, unsigned *sr) {
  IMFMediaType *t = NULL; UINT32 c = 0, s = 0; GUID sub = {0};
  if (FAILED(IMFSourceReader_GetCurrentMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &t))) return 0;
  IMFMediaType_GetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, &c);
  IMFMediaType_GetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &s);
  IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub);
  IMFMediaType_Release(t);
  if (!IsEqualGUID(&sub, &MFAudioFormat_Float) || c < 1 || c > 64 || s < 1 || s > 2000000) return 0;
  *ch = c; *sr = s;
  return 1;
}

/* one sample's floats into the job: 1, or 0 when its buffer couldn't be read (a read error) or the job refused them
   (res is then set) */
static int take_sample(Job *j, IMFSample *s, unsigned long long *drop, long index) {
  IMFMediaBuffer *buf = NULL; BYTE *p = NULL; DWORD bytes = 0; int ok = 0;
  long f = j->flags;
  if ((f & SMP_MF_TEST_BUFFER_FAILS_FIRST && index == 0) || (f & SMP_MF_TEST_BUFFER_FAILS_THIRD && index == 2)) return 0;
  if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &buf))) return 0;
  if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &bytes))) {
    size_t k = bytes / sizeof(float);
    const float *x = (const float *)p;
    k -= k % j->ch;                                /* whole frames only */
    if (*drop) { size_t d = *drop < k ? (size_t)*drop : k; x += d; k -= d; *drop -= d; }
    ok = 1;
    if (k) {
      if (job_need(j, k)) { memcpy(j->x + j->n, x, k * sizeof(float)); j->n += k; }
      else ok = 0;
    }
    IMFMediaBuffer_Unlock(buf);
  }
  IMFMediaBuffer_Release(buf);
  return ok;
}

#define EMPTY_READS_MAX 100000      /* reads in a row with no sound and no end: Windows has stalled, so stop */

/* the decode itself, on the helper thread: sets j->res, and j->err when it fails */
static void mf_read(Job *j) {
  IMFSourceReader *r = NULL; IMFMediaType *want = NULL, *native = NULL;
  GUID native_sub = {0};
  unsigned long long drop = 0;                     /* floats still to drop (the start delay) */
  int first = 1, empty = 0; long index = 0;
  HRESULT hr;
  j->res = SMP_MF_FAILED;
  if (FAILED(MFCreateSourceReaderFromURL(j->path, NULL, &r))) { j->res = SMP_MF_NOT_MINE; return; }   /* not media */
  InterlockedIncrement(&j->progress);
  IMFSourceReader_SetStreamSelection(r, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
  if (FAILED(IMFSourceReader_SetStreamSelection(r, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE))) {
    snprintf(j->err, sizeof j->err, "it has no sound in it");
    goto done;
  }
  if (SUCCEEDED(IMFSourceReader_GetNativeMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native))) {
    IMFMediaType_GetGUID(native, &MF_MT_SUBTYPE, &native_sub);
    IMFMediaType_Release(native);
  }
  /* float at the stream's own rate and channels: only the major type and subtype are set, so nothing is resampled */
  if (FAILED(MFCreateMediaType(&want))) { snprintf(j->err, sizeof j->err, "out of memory"); goto done; }
  IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
  IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, &MFAudioFormat_Float);
  hr = IMFSourceReader_SetCurrentMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, want);
  IMFMediaType_Release(want);
  if (FAILED(hr) || !out_format(r, &j->ch, &j->sr)) {
    const char *name = coder_name(&native_sub);
    if (name) snprintf(j->err, sizeof j->err, "Windows has no %s decoder on this computer", name);
    else snprintf(j->err, sizeof j->err, "Windows has no decoder on this computer for the sound in it");
    goto done;
  }
  for (;;) {
    DWORD idx = 0, flags = 0; LONGLONG when = 0; IMFSample *s = NULL; int ok;
    if (abandoned(j)) goto done;                   /* the caller has gone: stop, and free everything */
    hr = IMFSourceReader_ReadSample(r, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &idx, &flags, &when, &s);
    InterlockedIncrement(&j->progress);
    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
      if (s) IMFSample_Release(s);
      break;                                       /* a read error: what came before it is kept, if any */
    }
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
      unsigned c2 = 0, r2 = 0;
      /* a coder may settle its format at the first sample (HE-AAC finding its upper band); later, a change ends it */
      if (!out_format(r, &c2, &r2) || (j->n && (c2 != j->ch || r2 != j->sr))) { if (s) IMFSample_Release(s); break; }
      j->ch = c2; j->sr = r2;
    }
    if (!s) {
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
      if (++empty > EMPTY_READS_MAX) break;
      continue;
    }
    empty = 0;
    if (first) {                                   /* a start before zero is the coder's delay, marked to drop */
      if (when < 0) { drop = (unsigned long long)((-when * (long double)j->sr + 5000000) / 10000000) * j->ch; j->trimmed = 1; }
      first = 0;
    }
    ok = take_sample(j, s, &drop, index++);
    IMFSample_Release(s);
    if (!ok) {
      if (j->res == SMP_MF_TOO_LONG || j->err[0]) goto done;   /* refused: too long, or out of memory */
      break;                                       /* its buffer couldn't be read: a read error, as above */
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
  }
  if (j->n) j->res = SMP_MF_OK;
  else snprintf(j->err, sizeof j->err, "Windows couldn't decode it; it may be damaged");
done:
  if (r) IMFSourceReader_Release(r);
}

typedef struct { Job *job; HMODULE self; } Start;
static DWORD WINAPI helper(void *arg) {
  Start st = *(Start *)arg;
  Job *j = st.job;
  HRESULT co;
  int ready;
  free(arg);
  if (j->flags & SMP_MF_TEST_SLOW) Sleep(3000);    /* TESTS ONLY: a decoder that is stuck for 3 s */
  co = CoInitializeEx(NULL, COINIT_MULTITHREADED); /* its own thread: always its own COM */
  ready = abandoned(j) ? SMP_MF_FAILED : mf_ready(j->flags);
  if (ready != SMP_MF_OK) j->res = ready;
  else if (!abandoned(j)) mf_read(j);
  if (SUCCEEDED(co)) CoUninitialize();
  /* hand over, or (the caller has gone) clean up: whoever changes state from RUNNING owns the job */
  if (InterlockedCompareExchange(&j->state, JOB_DONE, JOB_RUNNING) != JOB_RUNNING) job_free(j);
  InterlockedDecrement(&helpers);
  FreeLibraryAndExitThread(st.self, 0);            /* drops this helper's hold on the DLL; never returns */
}

/* MPEG-4 boxes, for the edit list: big-endian sizes, a box is size (4), type (4), then its payload; size 1 means a
   64-bit size follows, size 0 means to the end */
static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t rd64(const unsigned char *p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }
/* the first box of type t in [b, b + len): its payload and payload length, or NULL */
static const unsigned char *box(const unsigned char *b, size_t len, const char *t, size_t *plen) {
  size_t at = 0;
  while (at + 8 <= len) {
    uint64_t size = rd32(b + at); size_t head = 8;
    if (size == 1) { if (at + 16 > len) return NULL; size = rd64(b + at + 8); head = 16; }
    else if (size == 0) size = len - at;
    if (size < head || size > len - at) return NULL;
    if (!memcmp(b + at + 4, t, 4)) { *plen = (size_t)size - head; return b + at + head; }
    at += (size_t)size;
  }
  return NULL;
}
/* a full box's timescale (mvhd and mdhd share the layout: version and flags, creation and change times, the
   timescale, the duration; version 1 has 64-bit times). 0, meaning no edit list, for an unknown version or a box too
   short for its version. */
static uint32_t timescale(const unsigned char *p, size_t n) {
  if (n >= 1 && p[0] == 1) return n >= 32 ? rd32(p + 20) : 0;
  if (n >= 1 && p[0] == 0) return n >= 20 ? rd32(p + 12) : 0;
  return 0;
}

int smp_mp4_edit(const unsigned char *b, size_t len, double *start, double *dur) {
  size_t n, tn, mn, en, hn, dn, ln, mv; const unsigned char *moov, *trak, *mdia, *edts, *hdlr, *mdhd, *elst, *mvhd;
  uint32_t movie_ts, media_ts, count, i;
  *start = 0; *dur = 0;
  if (len < 12 || memcmp(b + 4, "ftyp", 4) || !(moov = box(b, len, "moov", &n))) return 0;
  if (!(mvhd = box(moov, n, "mvhd", &mv)) || !(movie_ts = timescale(mvhd, mv))) return 0;
  /* the first sound track (the one Windows reads): its handler is "soun" */
  for (trak = moov; (trak = box(trak, n - (size_t)(trak - moov), "trak", &tn)) != NULL; trak += tn) {
    if (!(mdia = box(trak, tn, "mdia", &mn)) || !(hdlr = box(mdia, mn, "hdlr", &hn)) || hn < 12 ||
        memcmp(hdlr + 8, "soun", 4)) continue;
    if (!(mdhd = box(mdia, mn, "mdhd", &dn)) || !(media_ts = timescale(mdhd, dn))) return 0;
    if (!(edts = box(trak, tn, "edts", &en)) || !(elst = box(edts, en, "elst", &ln)) || ln < 8) return 0;
    if (elst[0] > 1) return 0;                     /* an unknown version */
    count = rd32(elst + 4);
    for (i = 0; i < count; i++) {                  /* the first edit that plays media (-1 is an empty edit) */
      int v1 = elst[0] == 1; size_t at = 8 + (size_t)i * (v1 ? 20 : 12);
      uint64_t seg; int64_t mt;
      if (at + (v1 ? 20 : 12) > ln) return 0;
      seg = v1 ? rd64(elst + at) : rd32(elst + at);
      mt = v1 ? (int64_t)rd64(elst + at + 8) : (int64_t)(int32_t)rd32(elst + at + 4);
      if (mt < 0) continue;
      *start = (double)mt / media_ts;
      *dur = count == 1 || i + 1 == count ? (double)seg / movie_ts : 0;   /* several edits: only the start is used */
      return 1;
    }
    return 0;
  }
  return 0;
}
SMP_API int smp_test_only_mp4_edit(const unsigned char *b, size_t len, double *start, double *dur) {
  return smp_mp4_edit(b, len, start, dur);
}

int smp_mf_decode(const char *path, size_t max_floats, float **x, size_t *floats, unsigned *ch, unsigned *sr,
                  int *trimmed, int (*cancel)(void *), void *arg, char *err, int cap) {
  Job *j; Start *st = NULL; HANDLE t = NULL; HMODULE self = NULL; int n, res; LONG seen = 0;
  ULONGLONG moved = GetTickCount64();              /* when the helper last got on */
  *x = NULL; *floats = 0; *ch = 0; *sr = 0; *trimmed = 0;
  n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  if (n <= 0) return SMP_MF_NOT_MINE;
  if (!(j = calloc(1, sizeof *j)) || !(j->path = malloc(sizeof(wchar_t) * (size_t)n))) {
    free(j);
    snprintf(err, (size_t)cap, "out of memory");
    return SMP_MF_FAILED;
  }
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, j->path, n);
  j->max_floats = max_floats;
  j->flags = InterlockedCompareExchange(&test_flags, 0, 0);
  j->state = JOB_RUNNING;
  InterlockedIncrement(&helpers);
  /* the helper's hold on this DLL (a real reference, not GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT), dropped as
     it exits; the helper frees st */
  if ((st = malloc(sizeof *st)) &&
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)(void *)helper, &self)) {
    st->job = j; st->self = self;
    t = CreateThread(NULL, 0, helper, st, 0, NULL);
  }
  if (!t) {
    InterlockedDecrement(&helpers);
    if (self) FreeLibrary(self);
    free(st);
    job_free(j);
    snprintf(err, (size_t)cap, "Windows couldn't start decoding it");
    return SMP_MF_FAILED;
  }
  for (;;) {
    const char *why = NULL;
    ULONGLONG limit, now;
    LONG p;
    if (WaitForSingleObject(t, 50) == WAIT_OBJECT_0) break;
    /* the stall timer: 60 s with no progress (tests: half a second) */
    limit = j->flags & SMP_MF_TEST_SLOW ? 500 : 60000;
    now = GetTickCount64();
    p = InterlockedCompareExchange(&j->progress, 0, 0);
    if (p != seen) { seen = p; moved = now; }
    if (cancel && cancel(arg)) why = "cancelled";
    else if (now - moved > limit) why = "Windows took too long to decode it";
    if (!why) continue;
    if (InterlockedCompareExchange(&j->state, JOB_ABANDONED, JOB_RUNNING) == JOB_RUNNING) {
      CloseHandle(t);                              /* the helper frees the job when it returns */
      snprintf(err, (size_t)cap, "%s", why);
      return SMP_MF_STOPPED;
    }
    break;                                         /* it finished just now: take its result */
  }
  CloseHandle(t);
  /* the helper set DONE, so the job is the caller's now */
  res = j->res;
  if (res == SMP_MF_OK) {
    *x = j->x; j->x = NULL; *floats = j->n;
    *ch = j->ch; *sr = j->sr; *trimmed = j->trimmed;
  } else if (res == SMP_MF_FAILED) snprintf(err, (size_t)cap, "%s", j->err[0] ? j->err : "Windows couldn't decode it");
  job_free(j);
  return res;
}
