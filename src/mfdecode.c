/* mfdecode.c: the Windows stage of the decode chain. A file that no built-in or library decoder reads is offered to
   Windows Media Foundation, which reads AAC and HE-AAC (.m4a, .mp4, .aac), WMA (.wma, .asf), and on most Windows 10
   and 11 machines ALAC, AMR-NB (.3gp, .amr) and others. What it has differs by machine: Windows 11 24H2 removed the
   AC-3 decoder, and the N editions have no Media Foundation unless the Media Feature Pack is installed.

   A Source Reader opens the file by its path (whatever its name ends with: Windows looks at the bytes too), reads the
   first sound stream only, and asks for 32-bit float at the stream's own rate and channels. Reads are synchronous;
   cancel is asked between samples, and the decoded floats go to the caller's put (decode.c's growing buffer, which
   holds the size limit). Lossy coders' start delay is dropped when Windows marks it (a first sample whose time is
   before zero). Windows doesn't apply an MPEG-4 file's edit list, so smp_mp4_edit reads it from the file's bytes
   and decode.c cuts the start delay (1024 frames for AAC) and the padding at the end by it.

   Media Foundation's DLLs are delay-loaded and looked for first, so the plugin still loads where they're missing.
   MFStartup runs once per process and is never undone (process exit cleans up; never from DllMain). COM is set up for
   the calling thread for the length of the call, and undone only when this call set it up.

   Called from the main thread or worker threads (the sampler's loader), never from the audio thread: it allocates,
   reads the disk and waits on Windows. Files only, never bytes in memory (decode_memory doesn't come here). */
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
#include "mfdecode.h"

/* Media Foundation, once per process: 1 when it started */
static INIT_ONCE mf_once = INIT_ONCE_STATIC_INIT;
static int mf_ok;
static BOOL CALLBACK mf_start(PINIT_ONCE once, void *p, void **ctx) {
  (void)once; (void)p; (void)ctx;
  /* the delay-loaded DLLs must be there before anything in them is called */
  if (!LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32) ||
      !LoadLibraryExW(L"mfreadwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)) return TRUE;
  mf_ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
  return TRUE;
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

#define EMPTY_READS_MAX 100000      /* reads in a row with no sound and no end: Windows has stalled, so stop */

static int mf_read(const wchar_t *wpath, unsigned *ch, unsigned *sr, SmpMfPut put, void *ctx,
                   int (*cancel)(void *), void *arg, int *trimmed, char *err, int cap) {
  IMFSourceReader *r = NULL; IMFMediaType *want = NULL, *native = NULL;
  GUID native_sub = {0};
  unsigned long long got = 0, drop = 0;            /* floats given to put; floats still to drop (the start delay) */
  int first = 1, empty = 0, res = SMP_MF_FAILED;
  HRESULT hr;
  if (FAILED(MFCreateSourceReaderFromURL(wpath, NULL, &r))) return SMP_MF_NOT_MINE;   /* not a media file to Windows */
  IMFSourceReader_SetStreamSelection(r, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
  if (FAILED(IMFSourceReader_SetStreamSelection(r, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE))) {
    snprintf(err, (size_t)cap, "it has no sound in it");
    goto done;
  }
  if (SUCCEEDED(IMFSourceReader_GetNativeMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native))) {
    IMFMediaType_GetGUID(native, &MF_MT_SUBTYPE, &native_sub);
    IMFMediaType_Release(native);
  }
  /* float at the stream's own rate and channels: only the major type and subtype are set, so nothing is resampled */
  if (FAILED(MFCreateMediaType(&want))) { snprintf(err, (size_t)cap, "out of memory"); goto done; }
  IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
  IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, &MFAudioFormat_Float);
  hr = IMFSourceReader_SetCurrentMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, want);
  IMFMediaType_Release(want);
  if (FAILED(hr) || !out_format(r, ch, sr)) {
    const char *name = coder_name(&native_sub);
    if (name) snprintf(err, (size_t)cap, "Windows has no %s decoder on this computer", name);
    else snprintf(err, (size_t)cap, "Windows has no decoder on this computer for the sound in it");
    goto done;
  }
  for (;;) {
    DWORD idx = 0, flags = 0; LONGLONG when = 0; IMFSample *s = NULL; IMFMediaBuffer *buf = NULL;
    BYTE *p = NULL; DWORD bytes = 0; int ok = 1;
    if (cancel && cancel(arg)) { snprintf(err, (size_t)cap, "cancelled"); res = SMP_MF_STOPPED; goto done; }
    hr = IMFSourceReader_ReadSample(r, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &idx, &flags, &when, &s);
    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
      if (s) IMFSample_Release(s);
      if (got) break;                              /* a damaged end: keep what came before it, as the others do */
      snprintf(err, (size_t)cap, "Windows couldn't decode it; it may be damaged");
      goto done;
    }
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
      unsigned c2 = 0, r2 = 0;
      /* a coder may settle its format at the first sample (HE-AAC finding its upper band); later, a change ends it */
      if (!out_format(r, &c2, &r2) || (got && (c2 != *ch || r2 != *sr))) { if (s) IMFSample_Release(s); break; }
      *ch = c2; *sr = r2;
    }
    if (!s) {
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
      if (++empty > EMPTY_READS_MAX) break;
      continue;
    }
    empty = 0;
    if (first) {                                    /* a start before zero is the coder's delay, marked to drop */
      if (when < 0) { drop = (unsigned long long)((-when * (long double)*sr + 5000000) / 10000000) * *ch; *trimmed = 1; }
      first = 0;
    }
    if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(s, &buf))) {
      if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &bytes))) {
        size_t k = bytes / sizeof(float);
        const float *x = (const float *)p;
        k -= k % *ch;                              /* whole frames only */
        if (drop) { size_t d = drop < k ? (size_t)drop : k; x += d; k -= d; drop -= d; }
        if (k && !put(ctx, x, k)) ok = 0;
        else got += k;
        IMFMediaBuffer_Unlock(buf);
      }
      IMFMediaBuffer_Release(buf);
    }
    IMFSample_Release(s);
    if (!ok) { res = SMP_MF_STOPPED; goto done; }   /* put refused: too long, or out of memory */
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
  }
  if (got) res = SMP_MF_OK;
  else snprintf(err, (size_t)cap, "Windows found no sound in it");
done:
  if (r) IMFSourceReader_Release(r);
  return res;
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
/* a full box's timescale (mvhd and mdhd share the layout): version 1 has 64-bit times before it */
static uint32_t timescale(const unsigned char *p, size_t n) {
  if (n >= 24 && p[0] == 1) return rd32(p + 20);
  return n >= 16 ? rd32(p + 12) : 0;
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

int smp_mf_decode(const char *path, unsigned *ch, unsigned *sr, SmpMfPut put, void *ctx,
                  int (*cancel)(void *), void *arg, int *trimmed, char *err, int cap) {
  wchar_t *w; int n, res; HRESULT co;
  *ch = 0; *sr = 0; *trimmed = 0;
  InitOnceExecuteOnce(&mf_once, mf_start, NULL, NULL);
  if (!mf_ok) return SMP_MF_NOT_MINE;              /* no Media Foundation here (a Windows N edition) */
  n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  if (n <= 0) return SMP_MF_NOT_MINE;
  if (!(w = malloc(sizeof(wchar_t) * (size_t)n))) { snprintf(err, (size_t)cap, "out of memory"); return SMP_MF_FAILED; }
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, n);
  /* the thread's COM: set up here unless it already is (S_FALSE also counts and is undone); a thread that chose the
     other apartment kind (RPC_E_CHANGED_MODE) keeps its own, and nothing is undone */
  co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  res = mf_read(w, ch, sr, put, ctx, cancel, arg, trimmed, err, cap);
  if (SUCCEEDED(co)) CoUninitialize();
  free(w);
  return res;
}
