/* sffont.c: the process-wide font registry, preset use counts, the loader thread, sample heads and the reads that
   stream the rest of a sample.

   The registry. Every smp_sf_open of the same file (same full path, compared case-insensitively, same size and
   modified time) shares one SmpFont, so many instances of a big font hold one copy of its samples. Only the main
   thread opens and closes. The list is guarded by g_lock, which the loader also takes to walk it; the audio
   thread never touches the lock.

   Use counts. A synth calls smp_sf_use(f, preset, +1 or -1) from any thread, the audio thread included. That is an
   Interlocked add on users[preset] and a SetEvent on the loader, nothing more.

   The loader. One thread for the whole process, at below-normal priority, with a decode worker beside it. Both
   start with the first font and stop when the last one closes, so a process with no font open has no thread left
   to hang on unload. Each pass the loader walks every font: it loads each sample a preset in use needs (the
   samples pending voices asked for first, smp_sf_hint), publishes it with InterlockedExchangePointer, and retires
   samples no preset in use has needed for the grace time (2 s). It only does cheap work: heads read from a PCM
   file, or from a decoded copy in the cache. A sample that must be decoded first (SF3, a compressed member file, a
   GigaStudio wave) and an SF2Pack's unpacking go to the worker's queue; the worker decodes into the cache and
   leaves the loaded sample for the loader to publish, so one font's long decode never holds up another font's
   heads. Neither holds g_lock while it reads a file.

   Heads and streaming. A loaded sample keeps only its head in memory: the first quarter second, or the whole
   sample when it is under 1 s. A voice starts from the head at once; the rest is read by the synth's streamer
   thread through smp_sf_stream_read, from the font file (PCM_FILE: one shared handle per file, opened by the loader
   with full sharing, read with positional reads so any number of streamers can share it) or from a mono float
   cache file (DECODED: SF3's and SF4's compressed samples, decoded once by the loader into the cache folder; the
   second open of the font decodes nothing). A stream handle is the loaded sample itself, so it is valid exactly
   as long as the sample is held.

   SF2Pack. The whole sample data is one compressed stream. The first time a preset of the font is used, the decode
   worker decodes it (off the main thread and off the loader) into a 16-bit cache file,
   and from then on every sample is plain PCM in that file at its SF2 offset. The font opens at once from its
   headers; until the pack is decoded its samples aren't loaded, and smp_sf_wait_idle waits for that pass like any
   other. The cache file stays open (pinned) while the font is, and the next open finds it and decodes nothing.

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
   freed is published again rather than loaded anew, which also stops quick program changes from thrashing.
   A streamer reading for a voice holds the sample too: the voice adds a second count for it at its start (it
   already holds one, so no read of pcm[s] is needed), and the streamer releases that count itself once the voice
   is gone and it has stopped reading (sfstream.c).

   The caller's guarantee: every voice and streamer has released its samples before smp_sf_close drops the last
   reference (the synth stops its voices, then waits for its streamer). Close frees the counters themselves, so a
   release after it would write freed memory. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sf2.h"
#include "sfstream.h"
#include "decode.h"

#define DEFAULT_GRACE_MS 2000
#define HELD_RECHECK_MS 20                 /* how often a retired sample that someone still holds is checked */
#define RETRY_MS 500                       /* how often a load that failed for now (no file, no memory) is retried */
#define MAX_DECODED_FRAMES (1u << 26)      /* a decoded sample's limit (25 minutes at 44.1 kHz), against lying headers */
#define MIN_HEAD 1024                      /* the shortest head, for samples with absurdly low rates */
#define SF_BROKEN_DECODE 2                 /* a load's *broken: the decoder couldn't read the file at all */
#define SF_NEEDS_DECODE 3                  /* a load's *broken on the loader: not in the cache, the worker decodes it */
enum { SF_DQ_NONE, SF_DQ_QUEUED, SF_DQ_DONE };
#define WARM_DIV 5                         /* heads load with 2/WARM_DIV s (0.4 s) after them read into the file cache */
#define PREFETCH_CHUNK (4u << 20)          /* a GigaStudio wave's bytes are read into the file cache this much at a time */
#define TRIM_EVERY (256ull << 20)          /* a long pass trims the cache after this many decoded bytes written */
#define STALL_MS 60000                     /* an offline render stops waiting for a font whose loader shows no progress
                                              this long (FFmpeg's own stall timer is as long) */

static SRWLOCK g_lock = SRWLOCK_INIT;      /* guards the list, g_busy and g_thread */
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;   /* signalled when the loader lets go of a font */
static SmpFont **g_fonts; static int g_nfonts, g_capfonts;
static SmpFont *g_busy;                    /* the font the loader is working on, outside the lock */
static HANDLE g_thread;
static HANDLE g_dthread;                   /* the decode worker (started and stopped with the loader) */
static SmpFont *g_dbusy;                   /* the font the worker is decoding for, outside the lock */
typedef struct { SmpFont *f; int s; } Job;
static Job *g_jobs; static int g_njobs, g_capjobs;   /* the worker's queue (under g_lock) */
static CONDITION_VARIABLE g_jcv = CONDITION_VARIABLE_INIT;   /* signalled when a job comes, or to stop */
static __declspec(thread) int t_decoder;   /* this thread is the decode worker */
static HANDLE g_wake;                      /* auto-reset; made once and never closed, so SetEvent is always safe */
static volatile LONG g_stop;
static volatile LONG g_grace = DEFAULT_GRACE_MS;
static volatile LONG g_whole;              /* tests: samples are held whole */
static volatile LONG g_decodes;            /* tests: compressed samples decoded */

void smp_sf_test_grace(int ms) { InterlockedExchange(&g_grace, ms > 0 ? ms : DEFAULT_GRACE_MS); }
void smp_sf_test_whole(int on) { InterlockedExchange(&g_whole, on ? 1 : 0); }
long smp_sf_test_decodes(void) { return g_decodes; }

static volatile LONG g_pause;              /* tests: the loader does nothing while set */
static volatile LONG g_race;               /* tests: the worker takes no job, and a queued decode waits for a close */
static volatile LONG g_race_waits;         /* tests: decodes that waited so */
static SRWLOCK g_plock = SRWLOCK_INIT;     /* set_problem's (the loader and the worker can find one at once) */
void smp_sf_test_pause(int on) {
  InterlockedExchange(&g_pause, on ? 1 : 0);
  if (!on && g_wake) SetEvent(g_wake);
}

/* tests: close while a decode is being queued. While on, the decode worker takes no job, and a decode the loader
   queues first waits (up to 5 s) for its font's close to begin, the moment a job used to slip in after the close
   dropped the font's jobs */
void smp_sf_test_close_race(int on) {
  InterlockedExchange(&g_race, on ? 1 : 0);
  if (!on) WakeAllConditionVariable(&g_jcv);
}

/* tests: the worker's queued jobs, and (waits, may be NULL) how many decodes have waited for a close so far */
int smp_sf_test_jobs(int *waits) {
  int n;
  AcquireSRWLockShared(&g_lock);
  n = g_njobs;
  ReleaseSRWLockShared(&g_lock);
  if (waits) *waits = (int)g_race_waits;
  return n;
}

/* ---- loading one sample ---- */

/* the frames a sample keeps in memory: all of them under 1 s, else the first quarter second */
static uint32_t head_of(uint32_t frames, uint32_t rate) {
  uint32_t hf = rate / 4 > MIN_HEAD ? rate / 4 : MIN_HEAD;
  if (g_whole || frames < rate || hf >= frames) return frames;
  return hf;
}

static SfPcm *new_pcm(uint32_t frames, uint32_t head, int is_float) {
  size_t hdr = (sizeof(SfPcm) + 15) & ~(size_t)15, w = is_float ? sizeof(float) : sizeof(int16_t);
  size_t bytes = hdr + ((size_t)head + 2 * SF_PCM_PAD) * w;
  uint8_t *b = calloc(1, bytes);           /* zeroed: the pads before and after the data stay zero */
  SfPcm *p = (SfPcm *)b;
  if (!b) return NULL;
  p->frames = frames;
  p->head_frames = head;
  p->is_float = is_float;
  p->bytes = bytes;
  p->data = b + hdr + SF_PCM_PAD * w;
  p->h = INVALID_HANDLE_VALUE;
  return p;
}

/* overwrites a freed buffer's start first, so a read after free shows up in tests instead of finding old data */
static void free_pcm(SfPcm *p) {
  size_t w;
  uint64_t own;
  if (!p) return;
  w = p->is_float ? sizeof(float) : sizeof(int16_t);
  own = p->bytes;                          /* bytes counts the post-loop head too, which is its own allocation */
  if (p->entry) smp_cache_close(p->entry);
  if (p->tail) {
    own -= ((uint64_t)p->tail_frames + 2 * SF_PCM_PAD) * w;
    free((uint8_t *)p->tail - SF_PCM_PAD * w);
  }
  memset(p, 0xDD, own < 65536 ? (size_t)own : 65536);
  free(p);
}

/* a positional read, so several threads may read one handle at once; 1 when all n bytes came */
static int read_at(HANDLE h, uint64_t off, void *buf, uint64_t n) {
  while (n > 0) {
    OVERLAPPED o;
    DWORD want = n > (1u << 30) ? (1u << 30) : (DWORD)n, got = 0;
    memset(&o, 0, sizeof o);
    o.Offset = (DWORD)off; o.OffsetHigh = (DWORD)(off >> 32);
    if (!ReadFile(h, buf, want, &got, &o) || got != want) return 0;
    buf = (uint8_t *)buf + got; n -= got; off += got;
  }
  return 1;
}

static int pcm_bytes(int enc) {
  switch (enc) {
    case SMP_PCM_U8: return 1;
    case SMP_PCM_S16LE: case SMP_PCM_S16BE: return 2;
    case SMP_PCM_S24LE: case SMP_PCM_S24BE: return 3;
    default: return 4;
  }
}

/* n PCM frames (interleaved over ch channels) to channel c alone: as float (of), or as int16 (oi) for the 16- and
   8-bit kinds. 16-bit as float is exactly what the voice computes from int16 data, x * (1 / 32768). */
static void pcm_convert(const uint8_t *b, int enc, int ch, int c, uint32_t n, float *of, int16_t *oi) {
  int bps = pcm_bytes(enc), stride = bps * ch;
  uint32_t i;
  b += bps * c;
  for (i = 0; i < n; i++, b += stride) {
    int16_t x16; int32_t x; float y;
    switch (enc) {
      case SMP_PCM_S16LE: x16 = (int16_t)(b[0] | b[1] << 8); goto narrow;
      case SMP_PCM_S16BE: x16 = (int16_t)(b[1] | b[0] << 8); goto narrow;
      case SMP_PCM_U8: x16 = (int16_t)((b[0] - 128) * 256); goto narrow;
      case SMP_PCM_S24LE: x = (int32_t)((uint32_t)b[0] << 8 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 24) >> 8; y = x * (1.0f / 8388608.0f); break;
      case SMP_PCM_S24BE: x = (int32_t)((uint32_t)b[2] << 8 | (uint32_t)b[1] << 16 | (uint32_t)b[0] << 24) >> 8; y = x * (1.0f / 8388608.0f); break;
      case SMP_PCM_S32LE: x = (int32_t)((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24); y = (float)(x * (1.0 / 2147483648.0)); break;
      default: memcpy(&y, b, 4); if (!(y >= -1e6f && y <= 1e6f)) y = 0; break;   /* F32LE; NaN and junk become 0 */
    }
    if (of) of[i] = y; else oi[i] = (int16_t)(y >= 1 ? 32767 : y <= -1 ? -32768 : y * 32768.0f);
    continue;
  narrow:
    if (oi) oi[i] = x16; else of[i] = x16 * (1.0f / 32768.0f);
  }
}

int sf_pcm_bytes(int enc) { return pcm_bytes(enc); }
void sf_pcm_convert(const uint8_t *b, int enc, int ch, int c, uint32_t n, float *of, int16_t *oi) { pcm_convert(b, enc, ch, c, n, of, oi); }

/* The handle for path_id in this pass (loader thread only). Each pass opens the file by name, as stage 2 did, so a
   file that has gone away stops new samples loading (the ones loaded keep playing). The first time, a duplicate of
   then, a second handle opened by name becomes the font's streaming handle, kept until the font closes, so streams
   read the very file the headers came from even if it is renamed meanwhile. It is overlapped, so the streamers of
   every player on the font read it in parallel (smp_pread). NULL when the file can't be opened now. */
#define DH_MAX 8
static __declspec(thread) struct { SmpFont *f; int id; HANDLE h; } t_dh[DH_MAX];   /* the worker's own handles */
static __declspec(thread) int t_ndh;
static void decoder_handles_close(void) {
  while (t_ndh > 0) { t_ndh--; CloseHandle(t_dh[t_ndh].h); }
}

static HANDLE font_handle(SmpFont *f, int path_id) {
  HANDLE h;
  wchar_t w[2048];
  const wchar_t *name = f->wpath;
  int k;
  if (path_id < 0 || path_id >= f->npaths) return NULL;
  if (t_decoder) {
    /* the decode worker: handles of its own for this job (the per-pass ones and the streaming ones are the
       loader's), closed when the job ends */
    for (k = 0; k < t_ndh; k++) if (t_dh[k].f == f && t_dh[k].id == path_id) return t_dh[k].h;
    if (path_id > 0) {
      if (!MultiByteToWideChar(CP_UTF8, 0, f->paths[path_id], -1, w, 2048)) return NULL;
      name = w;
    }
    if (t_ndh == DH_MAX) decoder_handles_close();
    h = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    t_dh[t_ndh].f = f; t_dh[t_ndh].id = path_id; t_dh[t_ndh].h = h; t_ndh++;
    return h;
  }
  if (f->ph[path_id]) return f->ph[path_id];
  if (path_id > 0) {
    if (!MultiByteToWideChar(CP_UTF8, 0, f->paths[path_id], -1, w, 2048)) return NULL;
    name = w;
  }
  /* the same sharing as the parser's, so a file another program has open can still load */
  h = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return NULL;
  if (!f->fh[path_id]) {
    /* sequential: each stream reads its file front to back, and Windows reads ahead further for such a handle,
       so fewer reads wait for the disk */
    HANDLE d = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED |
                           ((sfs_features() & SFS_F_SEQ) ? FILE_FLAG_SEQUENTIAL_SCAN : 0), NULL);
    if (d == INVALID_HANDLE_VALUE) { CloseHandle(h); return NULL; }
    f->fh[path_id] = d;
  }
  f->ph[path_id] = h;
  return h;
}

static void set_problem(SmpFont *f, const char *fmt, const char *a, const char *b);
static const char *font_name(const SmpFont *f);
static int pack_cancel(void *arg);

/* A DECODED sample: its head from its cache file when there is one; else decoded now (once) and written there.
   When the cache can't be written, the whole decoded sample is held in memory instead. NULL with *broken set when
   the data can never decode; NULL without it when memory ran out or the file can't be opened for now. */
/* a DECODED sample streams from its cache file through the file's handle, like a PCM file: mono, float or int16 */
static void stream_entry(SfPcm *p, SmpEntry *e, uint32_t skip) {
  p->entry = e; p->skip = skip;
  p->h = smp_entry_handle(e);
  p->offset = smp_entry_data_offset(e, skip);
  p->enc = smp_entry_bps(e) == 2 ? SMP_PCM_S16LE : SMP_PCM_F32LE;
  p->channels = 1; p->channel = 0;
}

static SfPcm *load_decoded(SmpFont *f, const SmpSrc *c, uint32_t rate, int *broken) {
  SmpEntry *e = smp_cache_open_mono(c->key);
  SfPcm *p;
  uint32_t n, hf, skip = c->skip;
  int i16;
  if (!e && !t_decoder) { *broken = SF_NEEDS_DECODE; return NULL; }   /* the loader never decodes: the worker does */
  if (!e) {
    float *x; int ch = 0, r = 0; long frames = 0; char err[256];
    const float *mono;
    if (c->giga) {
      /* a compressed GigaStudio wave: libgig decodes it (gigshim.cpp), a piece at a time, stopping for a close.
         libgig reads it a frame (a few KB) at a time on a random-access handle, which on a cold hard disk is a
         wait per frame; the wave's bytes are read through first, in big sequential pieces, into the file cache. */
      HANDLE h = c->length > 1 ? font_handle(f, c->path_id) : NULL;
      if (h) {
        uint8_t *buf = malloc(PREFETCH_CHUNK);
        uint64_t at;
        for (at = 0; buf && at < c->length && !pack_cancel(f); at += PREFETCH_CHUNK) {
          uint64_t k = c->length - at < PREFETCH_CHUNK ? c->length - at : PREFETCH_CHUNK;
          if (!read_at(h, c->offset + at, buf, k)) break;
        }
        free(buf);
      }
      x = sf_gig_decode(f, c, &ch, &frames, &r, pack_cancel, f, err, sizeof err);
      if (!x && (f->closing || g_stop)) return NULL;                 /* cancelled: not broken */
      if (!x && sf_gig_crashed()) { set_problem(f, "%s%s", err, ""); *broken = 1; return NULL; }   /* said plainly */
    } else if (c->length) {
      HANDLE h = font_handle(f, c->path_id);
      uint8_t *bytes;
      if (!h) return NULL;                                          /* tried again in RETRY_MS */
      if (c->length > 0x7fffffffu) { *broken = 1; return NULL; }
      if (!(bytes = malloc((size_t)c->length))) return NULL;
      if (!read_at(h, c->offset, bytes, c->length)) { free(bytes); *broken = 1; return NULL; }
      if (c->wrap && c->length >= 12) {   /* a DLS wave's LIST chunk: the same bytes as a RIFF WAVE file but the ids */
        memcpy(bytes, "RIFF", 4);
        memcpy(bytes + 8, "WAVE", 4);
      }
      x = decode_memory_ex(bytes, (size_t)c->length, &ch, &frames, &r, pack_cancel, f, err, sizeof err);
      free(bytes);
      if (!x && (f->closing || g_stop)) return NULL;                 /* cancelled: not broken */
    } else {
      /* a member file (SFZ, Decent Sampler, DLS): Windows or FFmpeg may take a minute, so it stops for a close */
      x = decode_audio_ex(f->paths[c->path_id], &ch, &frames, &r, NULL, pack_cancel, f, err, sizeof err);
      if (!x && (f->closing || g_stop)) return NULL;                 /* cancelled: not broken */
    }
    if (!x) { *broken = SF_BROKEN_DECODE; return NULL; }
    InterlockedIncrement(&g_decodes);
    if (frames <= 0 || frames > (long)MAX_DECODED_FRAMES || ch <= 0) { free(x); *broken = SF_BROKEN_DECODE; return NULL; }
    n = (uint32_t)frames;
    if (c->guessed && ch == 1) {
      /* taken as stereo without knowing (an SFZ sample of a format with no header probe), and mono after all: both
         halves of the pair play this one channel, hard left and right, so each gets 1/sqrt(2), the level a centred
         mono sample has in each ear */
      long i;
      for (i = 0; i < frames; i++) x[i] *= 0.70710678f;
    }
    mono = x + (size_t)(c->channel < ch ? c->channel : 0) * (size_t)n;   /* channel after channel */
    /* every decoded sample goes to the cache, short ones too, so the next open decodes nothing */
    if (smp_cache_write_mono(c->key, mono, n, r > 0 ? r : (int)rate) == 0) {
      e = smp_cache_open_mono(c->key);
      InterlockedExchangeAdd64(&f->wrote_bytes, (LONG64)n * 4);    /* the cache is trimmed after the writes */
    }
    /* A whole sample file (SFZ, Decent Sampler) or a GigaStudio wave in stereo: the loader made its other channel
       the sample next to this one, so that one's cache file is written now too, from the same decode, rather than
       decoding the file a second time when it loads. */
    if ((!c->length || c->giga) && (ch > 1 || c->guessed)) {
      int s = (int)(c - f->src), k;
      for (k = s - 1; k <= s + 1; k += 2) {
        const SmpSrc *o;
        SmpEntry *oe;
        if (k < 0 || k >= f->nshdr) continue;
        o = &f->src[k];
        if (o->kind != SMP_SRC_DECODED || o->length != c->length || o->giga != c->giga || o->offset != c->offset ||
            o->path_id != c->path_id || o->channel == c->channel ||
            (o->channel >= ch && !(c->guessed && ch == 1)) || o->gen) continue;
        if ((oe = smp_cache_open_mono(o->key)) != NULL) { smp_cache_close(oe); continue; }
        smp_cache_write_mono(o->key, x + (size_t)(o->channel < ch ? o->channel : 0) * (size_t)n, n, r > 0 ? r : (int)rate);
        InterlockedExchangeAdd64(&f->wrote_bytes, (LONG64)n * 4);
      }
    }
    if (skip >= n) {                                                /* an offset past the decoded end */
      free(x); if (e) smp_cache_close(e);
      set_problem(f, "%s is shorter than a region's offset, so that region is silent%s", f->shdr[c - f->src].name, "");
      *broken = 1;
      return NULL;
    }
    if (!e && skip) {
      /* No cache: a base sample is held whole, but a start head (an SFZ offset into the same file) would hold a
         second copy of nearly all of it, one per offset. Refused instead, with the reason. */
      free(x);
      set_problem(f, "the cache folder can't hold %s's decoded samples, so regions with offsets are silent%s", font_name(f), "");
      *broken = 1;
      return NULL;
    }
    hf = e ? head_of(n - skip, rate) : n - skip;                    /* no cache: held whole */
    i16 = e && smp_entry_bps(e) == 2;                               /* a 16-bit source: its head is int16 too */
    p = new_pcm(n - skip, hf, !i16);
    if (!p) { free(x); if (e) smp_cache_close(e); return NULL; }
    p->rate = r > 0 ? (uint32_t)r : rate;
    if (i16) {
      uint32_t i;
      for (i = 0; i < hf; i++) ((int16_t *)p->data)[i] = (int16_t)(int)(mono[skip + i] * 32768.0f);
    } else memcpy(p->data, mono + skip, (size_t)hf * sizeof(float));
    free(x);
    if (hf < n - skip) stream_entry(p, e, skip);
    else if (e) smp_cache_close(e);
    return p;
  }
  if (smp_entry_frames(e) == 0 || smp_entry_frames(e) > MAX_DECODED_FRAMES) { smp_cache_close(e); *broken = 1; return NULL; }
  if (smp_entry_frames(e) <= skip) {
    smp_cache_close(e);
    set_problem(f, "%s is shorter than a region's offset, so that region is silent%s", f->shdr[c - f->src].name, "");
    *broken = 1;
    return NULL;
  }
  n = (uint32_t)smp_entry_frames(e) - skip;
  hf = head_of(n, rate);
  p = new_pcm(n, hf, smp_entry_bps(e) == 4);                        /* in the file's own format */
  if (!p) { smp_cache_close(e); return NULL; }
  if (smp_entry_read_raw(e, skip, p->data, (long)hf) != (long)hf) { smp_cache_close(e); free(p); *broken = 1; return NULL; }
  p->rate = smp_entry_rate(e) > 0 ? (uint32_t)smp_entry_rate(e) : rate;
  if (hf < n) stream_entry(p, e, skip);
  else smp_cache_close(e);
  return p;
}

/* a sample made in memory (SFZ's *sine, *silence and *noise), held whole: n frames of it */
static SfPcm *made_pcm(int gen, uint32_t n) {
  SfPcm *p = new_pcm(n, n, 1);
  float *d;
  uint32_t i, x = 0x12345678u;
  if (!p) return NULL;
  d = (float *)p->data;
  for (i = 0; i < n; i++) {
    if (gen == SF_GEN_SINE) d[i] = (float)sin(2 * 3.14159265358979323846 * i / n);   /* one cycle */
    else if (gen == SF_GEN_NOISE) {                                 /* xorshift32 white noise, -1..1 */
      x ^= x << 13; x ^= x >> 17; x ^= x << 5;
      d[i] = (float)((x >> 8) * (2.0 / 16777216.0) - 1.0);
    }
  }
  return p;
}

/* The post-loop head of a sample that streams and has frames after its loop: from the loop end (or the head's end,
   if later) to a quarter second past the loop end, or the sample's end. A loop-until-release then goes on past its
   loop end from memory while the streamer, woken after the block, catches up. 0 when memory ran out. */
static int load_tail(SmpFont *f, const SmpSrc *c, SfPcm *p, uint32_t rate) {
  uint32_t t0, t1, n, pl = rate / 4 > MIN_HEAD ? rate / 4 : MIN_HEAD;
  size_t w = p->is_float ? sizeof(float) : sizeof(int16_t);
  uint8_t *b;
  int ok = 0;
  if (p->head_frames >= p->frames || p->loop_end <= p->loop_start || p->loop_end >= p->frames) return 1;
  t0 = p->loop_end > p->head_frames ? p->loop_end : p->head_frames;
  t1 = p->frames - p->loop_end > pl ? p->loop_end + pl : p->frames;
  if (t1 <= t0) return 1;
  n = t1 - t0;
  if (!(b = calloc((size_t)n + 2 * SF_PCM_PAD, w))) return 0;
  if (p->entry) {
    ok = smp_entry_read_raw(p->entry, (uint64_t)t0 + p->skip, b + SF_PCM_PAD * w, (long)n) == (long)n;   /* its own format */
  } else if (p->h != INVALID_HANDLE_VALUE) {
    HANDLE h = font_handle(f, c->path_id);
    int bps = pcm_bytes(c->enc);
    uint64_t nb = (uint64_t)n * (uint64_t)bps * c->channels;
    if (h && c->enc == SMP_PCM_S16LE && c->channels == 1) {
      ok = read_at(h, c->offset + (uint64_t)t0 * 2, b + SF_PCM_PAD * w, nb);
    } else if (h) {
      uint8_t *raw = nb < 0x7fffffffu ? malloc((size_t)nb) : NULL;
      if (!raw) { free(b); return 0; }
      ok = read_at(h, c->offset + (uint64_t)t0 * (uint64_t)bps * c->channels, raw, nb);
      if (ok) pcm_convert(raw, c->enc, c->channels, c->channel, n, p->is_float ? (float *)(b + SF_PCM_PAD * w) : NULL,
                          p->is_float ? NULL : (int16_t *)(b + SF_PCM_PAD * w));
      free(raw);
    }
  }
  if (!ok) { free(b); return 1; }                                 /* unreadable: the voice streams those frames instead */
  p->tail = b + SF_PCM_PAD * w;
  p->tail_start = t0; p->tail_frames = n;
  p->bytes += ((size_t)n + 2 * SF_PCM_PAD) * w;
  return 1;
}

/* The 0.4 s after a streaming sample's head, read and thrown away: Windows keeps it in the file cache, so the
   streamer's first reads for a note come from memory even when the disk is cold (a chord's notes would otherwise
   all wait for the disk at once, just as their heads run out). The pages are the file cache's, not this process's. */
static void warm(SmpFont *f, const SmpSrc *c, const SfPcm *p, uint32_t rate) {
  uint32_t wf = rate * 2 / WARM_DIV, left = p->frames - p->head_frames;
  int fb = pcm_bytes(p->enc) * p->channels;
  uint64_t off, nb;
  uint8_t *b;
  if (wf > left) wf = left;
  if (!wf || p->h == INVALID_HANDLE_VALUE) return;
  nb = (uint64_t)wf * (uint64_t)fb;
  if (nb > (4u << 20)) nb = 4u << 20;
  off = p->offset + (uint64_t)p->head_frames * (uint64_t)fb;
  if (!(b = malloc((size_t)nb))) return;
  if (p->entry) { DWORD got; smp_pread(p->h, off, b, (DWORD)nb, &got); }
  else { HANDLE h = font_handle(f, c->path_id); if (h) read_at(h, off, b, nb); }
  free(b);
}

/* loads sample s's head (all of it under 1 s); NULL with *broken set when the sample can never load (empty,
   unreadable or bad Ogg), NULL without it when memory ran out or the file can't be opened (tried again on a later
   pass) */
static SfPcm *load_sample(SmpFont *f, int s, int *broken) {
  const SmpSfSample *sh = &f->shdr[s];
  const SmpSrc *c = &f->src[s];
  uint32_t rate = sh->rate ? sh->rate : 44100;
  uint32_t end = sh->end;
  SfPcm *p = NULL;
  uint64_t ls, le;
  *broken = 0;
  if (f->is_pack && end > f->pack_frames) end = (uint32_t)f->pack_frames;   /* a pack: inside the decoded data */
  if (end <= sh->start) { *broken = 1; return NULL; }       /* start and end were clamped into smpl at open */
  if (c->gen) {
    p = made_pcm(c->gen, end - sh->start);
    if (!p) return NULL;
  } else if (c->kind == SMP_SRC_DECODED) {
    p = load_decoded(f, c, rate, broken);
    if (!p) return NULL;
  } else {
    uint32_t frames = end - sh->start, hf = head_of(frames, rate);
    int bps = pcm_bytes(c->enc), wide = bps > 2;
    HANDLE h = font_handle(f, c->path_id);
    if (!h) return NULL;
    p = new_pcm(frames, hf, wide);
    if (!p) return NULL;
    if (c->enc == SMP_PCM_S16LE && c->channels == 1) {             /* SF2: straight into the head */
      if (!read_at(h, c->offset, p->data, (uint64_t)hf * 2)) { free(p); *broken = 1; return NULL; }
    } else {
      uint64_t nb = (uint64_t)hf * (uint64_t)bps * c->channels;
      uint8_t *b = nb < 0x7fffffffu ? malloc((size_t)nb) : NULL;
      if (!b) { free(p); return NULL; }
      if (!read_at(h, c->offset, b, nb)) { free(b); free(p); *broken = 1; return NULL; }
      pcm_convert(b, c->enc, c->channels, c->channel, hf, wide ? (float *)p->data : NULL, wide ? NULL : (int16_t *)p->data);
      free(b);
    }
    if (hf < frames) { p->h = f->fh[c->path_id]; p->offset = c->offset; p->enc = c->enc; p->channels = c->channels; p->channel = c->channel; }
  }
  if (sh->type & SF_COMPRESSED) {
    /* SF3 and SF4: loop points count decoded frames from the stream's start (as MuseScore, Polyphone and
       sf2convert write them) */
    ls = sh->loop_start; le = sh->loop_end;
  } else {
    ls = sh->loop_start > sh->start ? sh->loop_start - sh->start : 0;
    le = sh->loop_end > sh->start ? sh->loop_end - sh->start : 0;
  }
  if (!p->rate) p->rate = rate;
  /* the loop inside the data (a decode shorter than its header said clamps it here too); a backwards loop becomes
     an empty one at its end */
  if (ls > p->frames) ls = p->frames;
  if (le > p->frames) le = p->frames;
  if (ls > le) ls = le;
  p->loop_start = (uint32_t)ls; p->loop_end = (uint32_t)le;
  if (!load_tail(f, c, p, rate)) { free_pcm(p); return NULL; }   /* out of memory: tried again later */
  if ((sfs_features() & SFS_F_WARM) && p->head_frames < p->frames) warm(f, c, p, rate);
  return p;
}

/* the font's problem, said once: written before has_problem is set, so a reader that sees the flag sees the text.
   The loader and the decode worker can both find one, so the check and the write are one step under g_plock (which
   only this takes: readers go by the flags) */
static void set_problem(SmpFont *f, const char *fmt, const char *a, const char *b) {
  AcquireSRWLockExclusive(&g_plock);
  if (InterlockedCompareExchange(&f->has_problem2, 0, 0)) { ReleaseSRWLockExclusive(&g_plock); return; }
  if (InterlockedCompareExchange(&f->has_problem, 0, 0)) {
    /* the open already said one (an SFZ's missing samples): both, in the second buffer, once */
    size_t L;
    if (f->is_sfz) {
      snprintf(f->problem2, sizeof f->problem2, "%s; ", f->problem);
      L = strlen(f->problem2);
      snprintf(f->problem2 + L, sizeof f->problem2 - L, fmt, a, b ? b : "");
      InterlockedExchange(&f->has_problem2, 1);
    }
    ReleaseSRWLockExclusive(&g_plock);
    return;
  }
  snprintf(f->problem, sizeof f->problem, fmt, a, b ? b : "");
  InterlockedExchange(&f->has_problem, 1);
  ReleaseSRWLockExclusive(&g_plock);
}

/* the font's file name, for its problem */
static const char *font_name(const SmpFont *f) {
  const char *b = f->path, *q;
  for (q = f->path; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
  return b;
}

/* pack_prepare's and the GigaStudio decode's cancel hook: the font is closing, or the loader is stopping */
static int pack_cancel(void *arg) {
  SmpFont *f = arg;
  InterlockedIncrement(&f->progress);       /* asked once a piece: the decode is going on (offline renders wait) */
  return f->closing || g_stop;
}

#define PACK_READ_CHUNK (16u << 20)        /* a pack's payload is read this much at a time, checking for a close */

/* SF2Pack: the decoded pack, from the cache when it is there, else decoded now (once) and written there as 16-bit
   mono. The cache file's path (paths[1]) is taken from the cache folder in the Settings now, so a changed folder
   counts. Ends with pack_state:
   - READY: the samples can load;
   - BROKEN, with the font's problem said, when it can never be had here: no usable cache folder (found before the
     payload is read), the payload is empty, unreadable or over 2 GiB, the stream doesn't decode, or it is over the
     decode limit (the parser refuses most of those at open);
   - NONE, to be tried again in RETRY_MS: the font file can't be opened for now, or memory ran out; or the font is
     closing (the read, the decode and the cache write are each cancelled within a piece, so smp_sf_close doesn't
     wait for them).
   The decode runs on the loader thread. Its memory: the compressed payload, plus 4 bytes per frame of mono float
   (decode_memory keeps mono in its growing buffer; that grows by doubling only when the stream doesn't give its
   length), then converted to int16 in place. */
static void pack_prepare(SmpFont *f) {
  SmpEntry *e;
  char file[2048 * 3], why[256];
  /* the cache file for the cache folder of now */
  if (smp_cache_path(f->pack_key, file, (int)sizeof file)) {
    if (!f->paths[1] || strcmp(f->paths[1], file)) {
      char *d = _strdup(file);
      if (!d) return;
      free(f->paths[1]);
      f->paths[1] = d;
    }
  }
  e = f->paths[1] ? smp_cache_open_i16(f->pack_key) : NULL;
  if (!e) {
    HANDLE h;
    uint8_t *bytes;
    float *x;
    int16_t *o;
    int ch = 0, r = 0, k;
    long frames = 0, i;
    uint64_t got;
    char err[256] = "";
    if (!f->paths[1] || !smp_cache_ok(why, (int)sizeof why)) {
      set_problem(f, "the cache folder can't hold %s's unpacked samples%s", font_name(f), "");
      f->pack_state = SF_PACK_BROKEN;
      return;
    }
    if (!(h = font_handle(f, 0))) return;                       /* tried again in RETRY_MS */
    if (f->smpl_size == 0 || f->smpl_size > 0x7fffffffu) {
      set_problem(f, "can't unpack %s (it may be damaged)%s", font_name(f), "");
      f->pack_state = SF_PACK_BROKEN;
      return;
    }
    if (!(bytes = malloc((size_t)f->smpl_size))) return;
    for (got = 0; got < f->smpl_size; got += PACK_READ_CHUNK) {
      uint64_t k = f->smpl_size - got < PACK_READ_CHUNK ? f->smpl_size - got : PACK_READ_CHUNK;
      if (pack_cancel(f)) { free(bytes); return; }               /* closing: left NONE */
      if (!read_at(h, f->smpl_off + got, bytes + got, k)) {
        free(bytes);
        set_problem(f, "can't unpack %s (it may be damaged)%s", font_name(f), "");
        f->pack_state = SF_PACK_BROKEN;
        return;
      }
    }
    x = decode_memory_ex(bytes, (size_t)f->smpl_size, &ch, &frames, &r, pack_cancel, f, err, sizeof err);
    free(bytes);
    if (!x) {
      if (!strcmp(err, "cancelled")) return;                     /* closing: left NONE */
      if (!strcmp(err, "out of memory")) return;                 /* tried again in RETRY_MS */
      /* the decoder's own reason when it says something particular (too long, a library's message), else plain */
      if (!err[0] || strstr(err, "isn't a sound file")) set_problem(f, "can't unpack %s (it may be damaged)%s", font_name(f), "");
      else set_problem(f, "can't unpack %s (%s)", font_name(f), err);
      f->pack_state = SF_PACK_BROKEN;
      return;
    }
    InterlockedIncrement(&g_decodes);
    if (frames <= 0 || (unsigned long)frames > SF_PACK_MAX_FRAMES || ch <= 0) {
      free(x);
      set_problem(f, "can't unpack %s (it's too long to load)%s", font_name(f), "");
      f->pack_state = SF_PACK_BROKEN;
      return;
    }
    /* the first channel (channel after channel, so its frames come first) to int16, in place: frame i's int16 goes
       where float i/2 was, which has been read already. FLAC and WavPack give back exactly the packed values. */
    o = (int16_t *)x;
    for (i = 0; i < frames; i++) {
      float v = x[i] * 32768.0f;
      o[i] = (int16_t)(v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int)floorf(v + 0.5f));
    }
    /* writing a few hundred MB to a slow disk takes longer than decoding it: that is cancellable too */
    k = smp_cache_write_i16(f->pack_key, o, (uint64_t)frames, r > 0 ? r : 44100, pack_cancel, f);
    if (k == 4) { free(x); return; }                             /* closing: left NONE, nothing written */
    if (k == 0) InterlockedExchangeAdd64(&f->wrote_bytes, (LONG64)frames * 2);
    if (k != 0 || !(e = smp_cache_open_i16(f->pack_key))) {
      free(x);
      set_problem(f, "the cache folder can't hold %s's unpacked samples%s", font_name(f), "");
      f->pack_state = SF_PACK_BROKEN;
      return;
    }
    free(x);
  }
  f->pack_entry = e;
  f->pack_frames = smp_entry_frames(e);
  f->pack_state = SF_PACK_READY;
}

/* The decode cache keeps to its limit in the Settings: trimmed after the loader's writes (every TRIM_EVERY bytes in
   a long pass, and at the pass's end). Files held open (samples that stream from them) are never deleted. */
static void trim_after_writes(SmpFont *f, int force) {
  SmpSettings st;
  LONG64 w = InterlockedExchange64(&f->wrote_bytes, 0);   /* written by the worker, trimmed by either thread */
  if (!w || (!force && (uint64_t)w < TRIM_EVERY)) { if (w) InterlockedExchangeAdd64(&f->wrote_bytes, w); return; }
  smp_settings_get(&st);
  smp_cache_trim((uint64_t)(st.cache_limit_gb * 1024.0 * 1024.0 * 1024.0));
}

static void publish(SmpFont *f, int s, SfPcm *p) {
  InterlockedExchangePointer((PVOID volatile *)&f->pcm[s], p);
}

/* ---- the loader thread and the decode worker ---- */

/* a load's outcome, on the loader thread: published, broken (never tried again) or left for a retry */
static void take(SmpFont *f, int s, SfPcm *pc, int broken, int *retry) {
  if (pc) {
    f->held[s] = pc;
    InterlockedIncrement(&f->nloaded);
    InterlockedExchangeAdd64(&f->loaded_bytes, (LONG64)pc->bytes);
    publish(f, s, pc);
    f->state[s] = SF_ST_PUBLISHED;
  } else if (broken) {
    f->state[s] = SF_ST_BROKEN;                /* never tried again */
    /* an SFZ's sample files are only probed at open; one that turns out not to decode is said here */
    if ((f->is_sfz || f->is_gig) && broken == SF_BROKEN_DECODE)
      set_problem(f, "%s can't be decoded (it may be damaged or in a format this can't read)%s", f->shdr[s].name, "");
  } else {
    *retry = 1;                                /* no file or no memory: tried again in RETRY_MS */
  }
  trim_after_writes(f, 0);
}

/* a job for the decode worker: sample s's decode, or (s = -1) the font's SF2Pack. Loader thread. Never for a
   closing font: its close has dropped its jobs and may free it as soon as the loader lets go, so a job queued after
   that would reach the worker with a freed font (closing is set under g_lock, and checked here under it). */
static void queue_decode(SmpFont *f, int s) {
  if (s < 0 ? f->pack_q : f->dq[s]) return;
  if (g_race) {                                 /* tests: wait for the close to begin */
    ULONGLONG end = GetTickCount64() + 5000;
    InterlockedIncrement(&g_race_waits);
    while (!f->closing && GetTickCount64() < end) Sleep(1);
  }
  AcquireSRWLockExclusive(&g_lock);
  if (f->closing) { ReleaseSRWLockExclusive(&g_lock); return; }
  if (g_njobs == g_capjobs) {
    int nc = g_capjobs ? g_capjobs * 2 : 64;
    Job *nj = realloc(g_jobs, (size_t)nc * sizeof *nj);
    if (!nj) { ReleaseSRWLockExclusive(&g_lock); return; }   /* tried again on a later pass */
    g_jobs = nj; g_capjobs = nc;
  }
  g_jobs[g_njobs].f = f; g_jobs[g_njobs].s = s; g_njobs++;
  if (s < 0) InterlockedExchange(&f->pack_q, 1);
  else InterlockedExchange8((volatile char *)&f->dq[s], SF_DQ_QUEUED);
  ReleaseSRWLockExclusive(&g_lock);
  WakeConditionVariable(&g_jcv);
}


/* Sample s, needed and not loaded: loaded now (its head from a PCM file, or from its decoded copy in the cache),
   published, or marked broken (or left for a retry). A DECODED sample with no copy in the cache yet goes to the
   decode worker instead: the loader never decodes, so one font's long decode never holds up another's heads. */
static void load_one(SmpFont *f, int s, int *retry, int *cant_open, int *waiting) {
  SfPcm *pc; int broken;
  if (f->dq[s]) { *waiting = 1; return; }       /* the worker has it */
  if (!font_handle(f, f->src[s].path_id)) { *cant_open = *retry = 1; return; }   /* tried again in RETRY_MS */
  pc = load_sample(f, s, &broken);
  InterlockedIncrement(&f->progress);
  if (!pc && broken == SF_NEEDS_DECODE) { queue_decode(f, s); *waiting = 1; return; }
  take(f, s, pc, broken, retry);
}

/* the samples pending voices asked for first (smp_sf_hint): loaded before the walk */
static void serve_hints(SmpFont *f, int *retry, int *cant_open, int *waiting) {
  int k;
  for (k = 0; k < SF_HINTS; k++) {
    LONG h = InterlockedExchange(&f->hint[k], 0), s = h - 1;
    if (h <= 0 || s >= f->nshdr || f->closing || g_stop) continue;
    if (!f->need[s] || f->state[s] != SF_ST_NONE || *cant_open) continue;
    if (f->is_pack && f->pack_state != SF_PACK_READY) continue;
    load_one(f, s, retry, cant_open, waiting);
  }
}

/* one pass over font f; returns how long until it wants another pass (INFINITE when only a use() can change
   anything) */
static DWORD pass(SmpFont *f) {
  LONG gen = InterlockedCompareExchange(&f->use_gen, 0, 0);   /* read before the users, so a use() we miss shows */
  ULONGLONG now = GetTickCount64();
  ULONGLONG grace = (ULONGLONG)(DWORD)g_grace;
  DWORD next = INFINITE;
  int s, p, k, aborted = 0, retry = 0, cant_open = 0, waiting = 0;

  memset(f->need, 0, (size_t)f->nshdr);
  for (p = 0; p < f->nphdr; p++) {
    const SmpSfRegion *r; int n;
    if (f->users[p] <= 0) continue;
    n = smp_sf_regions(f, p, &r);
    for (k = 0; k < n; k++) if (r[k].sample >= 0 && r[k].sample < f->nshdr) f->need[r[k].sample] = 1;
  }
  /* what the decode worker finished: published like any load */
  for (s = 0; s < f->nshdr; s++) if (f->dq[s] == SF_DQ_DONE) {
    SfPcm *pc = f->dres[s]; int broken = f->dbroken[s];
    f->dres[s] = NULL; f->dbroken[s] = 0;
    InterlockedExchange8((volatile char *)&f->dq[s], SF_DQ_NONE);
    if (f->state[s] == SF_ST_NONE) take(f, s, pc, broken, &retry);
    else free_pcm(pc);
  }
  if (f->is_pack && f->pack_state == SF_PACK_NONE && !f->closing && !g_stop) {   /* the pack first, once needed */
    for (s = 0; s < f->nshdr && !f->need[s]; s++) {}
    if (s < f->nshdr) queue_decode(f, -1);                      /* the worker unpacks it */
  }
  serve_hints(f, &retry, &cant_open, &waiting);

  for (s = 0; s < f->nshdr; s++) {
    if (f->closing || g_stop) { aborted = 1; break; }
    if (f->need[s]) {
      f->idle_since[s] = 0;
      if (f->state[s] == SF_ST_RETIRED) {          /* needed again before it was freed: publish it again */
        publish(f, s, f->held[s]);
        f->state[s] = SF_ST_PUBLISHED;
      } else if (f->state[s] == SF_ST_NONE && f->is_pack && f->pack_state != SF_PACK_READY) {
        if (f->pack_state == SF_PACK_BROKEN) f->state[s] = SF_ST_BROKEN;   /* the pack never decodes */
        else if (f->pack_q) waiting = 1;                                    /* the worker is unpacking it */
        else retry = 1;                                                     /* tried again in RETRY_MS */
      } else if (f->state[s] == SF_ST_NONE && cant_open) {
        retry = 1;
      } else if (f->state[s] == SF_ST_NONE) {
        load_one(f, s, &retry, &cant_open, &waiting);
        serve_hints(f, &retry, &cant_open, &waiting);   /* a pending voice's sample doesn't wait out the walk */
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
  for (s = 0; s < f->npaths; s++) if (f->ph[s]) { CloseHandle(f->ph[s]); f->ph[s] = NULL; }
  trim_after_writes(f, 1);
  InterlockedIncrement(&f->progress);
  /* A pass that left a needed sample unloaded for now (the file can't be opened, or memory ran out) isn't done: it
     runs again in RETRY_MS, and the status still counts the presets as loading. But it is as done as it can be
     for now, so wait_idle stops waiting (an offline render goes on, silent where samples are missing, rather than
     waiting a block); a later use() makes waiters wait for the next pass again. While the decode worker still has
     samples of the font, the pass isn't done either (the worker wakes the loader when it finishes one). */
  if (retry && RETRY_MS < next) next = RETRY_MS;
  if (!aborted && !retry && !waiting) InterlockedExchange(&f->done_gen, gen);
  if (!aborted && retry && !waiting) InterlockedExchange(&f->stuck_gen, gen);
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
    if (g_pause) { wait = 10; continue; }
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
  smp_pread_thread_done();
  return 0;
}

/* The decode worker: one thread beside the loader, started and stopped with it, at the same below-normal
   priority. It takes jobs in order (a sample's decode into the cache, or an SF2Pack's unpacking), runs the same
   load_sample the loader would, with handles of its own, and leaves the result for the loader to publish. A
   closing font's jobs are dropped, and its running decode stops at its next piece (f->closing). */
static DWORD WINAPI decoder(void *arg) {
  (void)arg;
  t_decoder = 1;
  for (;;) {
    SmpFont *f = NULL; int s = 0;
    AcquireSRWLockExclusive(&g_lock);
    while (!g_stop && (!g_njobs || g_race)) SleepConditionVariableSRW(&g_jcv, &g_lock, g_race ? 10 : INFINITE, 0);
    if (g_stop) { ReleaseSRWLockExclusive(&g_lock); break; }
    f = g_jobs[0].f; s = g_jobs[0].s;
    memmove(g_jobs, g_jobs + 1, (size_t)(--g_njobs) * sizeof *g_jobs);
    g_dbusy = f;
    ReleaseSRWLockExclusive(&g_lock);
    if (!f->closing && !g_stop) {
      if (s < 0) {
        pack_prepare(f);
        InterlockedExchange(&f->pack_q, 0);
      } else {
        int broken = 0;
        SfPcm *pc = load_sample(f, s, &broken);
        InterlockedIncrement(&f->progress);
        f->dres[s] = pc; f->dbroken[s] = (uint8_t)broken;
        InterlockedExchange8((volatile char *)&f->dq[s], SF_DQ_DONE);   /* after the result: the loader takes it */
      }
    }
    decoder_handles_close();
    if (!f->closing) trim_after_writes(f, 1);       /* the cache keeps to its limit as the worker writes */
    AcquireSRWLockExclusive(&g_lock);
    g_dbusy = NULL;
    ReleaseSRWLockExclusive(&g_lock);
    WakeAllConditionVariable(&g_cv);
    if (g_wake) SetEvent(g_wake);                   /* the loader publishes what came */
  }
  smp_pread_thread_done();
  return 0;
}

/* ---- the registry ---- */

static void free_font(SmpFont *f) {
  int s;
#ifdef _DEBUG
  /* the caller's guarantee (see the top): no voice still holds a sample */
  if (f->voices) for (s = 0; s < f->nshdr; s++)
    if (f->voices[s] > 0) { OutputDebugStringA("samplosis: a SoundFont was closed while a voice held a sample\n"); break; }
#endif
  if (f->held) for (s = 0; s < f->nshdr; s++) free_pcm(f->held[s]);
  if (f->dres) for (s = 0; s < f->nshdr; s++) free_pcm(f->dres[s]);   /* decoded, not yet taken by the loader */
  free((void *)f->dq); free(f->dres); free(f->dbroken);
  if (f->fh) for (s = 0; s < f->npaths; s++) if (f->fh[s]) CloseHandle(f->fh[s]);
  if (f->pack_entry) smp_cache_close(f->pack_entry);
  free((void *)f->users); free((void *)f->voices); free((void *)f->pcm); free(f->fh); free(f->ph);
  free(f->held); free(f->state); free(f->need); free(f->idle_since); free(f->wpath);
  f->wpath = NULL;
  sf_free(f);
}

/* the cache keys of the font's DECODED samples: the file's full path, size and modified time, the byte range and
   the channel, so a changed file never finds a stale copy */
void sf_source_keys(SmpFont *f) {
  char path[2048 * 3], d[2048 * 3 + 160];
  wchar_t *full = f->wpath ? NULL : sf_full_path(f->path);
  const wchar_t *wp = f->wpath ? f->wpath : full;
  int s;
  f->keys_done = 1;
  if (!wp || !WideCharToMultiByte(CP_UTF8, 0, wp, -1, path, (int)sizeof path, NULL, NULL)) snprintf(path, sizeof path, "%s", f->path ? f->path : "");
  free(full);
  if (f->is_pack) {
    /* the decoded pack: keyed by the file (sfpack1: the version of this layout); pack_prepare names its file */
    snprintf(d, sizeof d, "sfpack1|%s|%llu|%llu", path, (unsigned long long)f->size, (unsigned long long)f->mtime);
    f->pack_key = smp_key(d);
  }
  for (s = 0; s < f->nshdr; s++) {
    SmpSrc *c = &f->src[s];
    uint64_t size = f->size, mtime = f->mtime;
    const char *file = path;
    if (c->kind != SMP_SRC_DECODED) continue;
    if (c->path_id > 0 && c->path_id < f->npaths) {    /* a sample file of its own: its own size and time */
      wchar_t w[2048]; WIN32_FILE_ATTRIBUTE_DATA a;
      file = f->paths[c->path_id];
      size = mtime = 0;
      if (MultiByteToWideChar(CP_UTF8, 0, file, -1, w, 2048) && GetFileAttributesExW(w, GetFileExInfoStandard, &a)) {
        size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
        mtime = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
      }
    }
    /* sfdec1: the decoder's version, so a changed decoder never finds an old copy */
    snprintf(d, sizeof d, "sfdec1|%s|%llu|%llu|%llu|%llu|%u", file, (unsigned long long)size,
             (unsigned long long)mtime, (unsigned long long)c->offset, (unsigned long long)c->length, (unsigned)c->channel);
    c->key = smp_key(d);
  }
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
  f->fh = calloc(f->npaths > 0 ? (size_t)f->npaths : 1, sizeof(HANDLE));
  f->ph = calloc(f->npaths > 0 ? (size_t)f->npaths : 1, sizeof(HANDLE));
  f->dq = calloc(ns, 1);
  f->dres = calloc(ns, sizeof(SfPcm *));
  f->dbroken = calloc(ns, 1);
  f->stuck_gen = -1;
  if (!(f->users && f->voices && f->pcm && f->held && f->state && f->need && f->idle_since && f->fh && f->ph &&
        f->dq && f->dres && f->dbroken)) return 0;
  if (!f->keys_done) sf_source_keys(f);         /* normally done by the parse (on its thread) */
  return 1;
}

/* the file's full path as a new wide string, or NULL */
wchar_t *sf_full_path(const char *path) {
  wchar_t w[2048], *full; DWORD n;
  if (!path || !MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048)) return NULL;
  n = GetFullPathNameW(w, 0, NULL, NULL);
  if (!n || !(full = malloc(n * sizeof(wchar_t)))) return NULL;
  if (GetFullPathNameW(w, n, full, NULL) >= n) { free(full); return NULL; }
  return full;
}

/* the font already open for this full path with this size and time, with one more reference; or NULL */
static SmpFont *find_open(const wchar_t *full, uint64_t size, uint64_t mt) {
  SmpFont *found = NULL;
  int i;
  AcquireSRWLockExclusive(&g_lock);
  for (i = 0; i < g_nfonts; i++) {
    SmpFont *g = g_fonts[i];
    if (g->wpath && g->size == size && g->mtime == mt &&
        CompareStringOrdinal(g->wpath, -1, full, -1, TRUE) == CSTR_EQUAL) { found = g; found->refs++; break; }
  }
  ReleaseSRWLockExclusive(&g_lock);
  return found;
}

SmpFont *smp_sf_open(const char *path, char *err, int cap) {
  wchar_t *full = sf_full_path(path);
  WIN32_FILE_ATTRIBUTE_DATA a;
  SmpFont *f;
  int i;
  if (err && cap > 0) err[0] = 0;
  if (!full) {                                      /* not a usable path: the loader could never find it */
    const char *b = path ? path : "(no file)", *q;
    for (q = b; *q; q++) if (*q == '\\' ||*q == '/' || *q == ':') b = q + 1;
    if (err && cap > 0) snprintf(err, cap, "can't open %s", b);
    return NULL;
  }
  (void)i;
  if (GetFileAttributesExW(full, GetFileExInfoStandard, &a)) {
    SmpFont *found = find_open(full, (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow,
                               (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime);
    if (found) { free(full); return found; }
  }
  f = sf_parse(path, err, cap);
  if (!f) { free(full); return NULL; }
  return sf_register(f, full, err, cap);
}

/* the window's background loads: the parse on a worker, the registration on the main thread */
SmpFont *smp_sf_parse_detached(const char *path, int (*cancel)(void *), void *arg, char *err, int cap) {
  if (err && cap > 0) err[0] = 0;
  return sf_parse_ex(path, cancel, arg, err, cap);
}

void smp_sf_discard(SmpFont *f) { if (f) sf_free(f); }

SmpFont *smp_sf_adopt(SmpFont *f, char *err, int cap) {
  wchar_t *full;
  SmpFont *found;
  if (err && cap > 0) err[0] = 0;
  if (!f) return NULL;
  if (!(full = sf_full_path(f->path))) { sf_free(f); if (err && cap > 0) snprintf(err, cap, "out of memory"); return NULL; }
  /* the same file opened meanwhile (by a project, or another instance): that one is shared */
  if ((found = find_open(full, f->size, f->mtime)) != NULL) { free(full); sf_free(f); return found; }
  return sf_register(f, full, err, cap);
}

SmpFont *sf_register(SmpFont *f, wchar_t *full, char *err, int cap) {
  const char *path = f->path ? f->path : "the font";
  f->wpath = full;
  if (!sf_finish_regions(f) || !setup(f)) goto no_memory;
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
  if (g_thread && !g_dthread) {
    g_dthread = CreateThread(NULL, 0, decoder, NULL, 0, NULL);
    if (g_dthread) SetThreadPriority(g_dthread, THREAD_PRIORITY_BELOW_NORMAL);
  }
  if (!g_thread || !g_dthread) {
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
  HANDLE t = NULL, dt = NULL;
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
  for (i = 0; i < g_njobs;) {                       /* the worker's jobs for f are dropped */
    if (g_jobs[i].f == f) { memmove(g_jobs + i, g_jobs + i + 1, (size_t)(g_njobs - i - 1) * sizeof *g_jobs); g_njobs--; }
    else i++;
  }
  /* A read the loader or the worker is stuck in for f (a dead network share, a disk that doesn't answer) is
     cancelled, so the close doesn't wait on it; decodes stop on f->closing themselves. */
  while (g_busy == f || g_dbusy == f) {
    if (g_busy == f && g_thread) CancelSynchronousIo(g_thread);
    if (g_dbusy == f && g_dthread) CancelSynchronousIo(g_dthread);
    SleepConditionVariableSRW(&g_cv, &g_lock, 50, 0);
  }
  if (g_nfonts == 0 && g_thread) {                  /* the last font: the loader and the worker stop */
    t = g_thread; g_thread = NULL;
    dt = g_dthread; g_dthread = NULL;
    InterlockedExchange(&g_stop, 1);
    g_njobs = 0;                                    /* nothing is left for the next worker (no font is open) */
  }
  ReleaseSRWLockExclusive(&g_lock);
  if (g_wake) SetEvent(g_wake);                     /* wakes it to stop, or to walk the shifted list again */
  if (t) {
    WakeAllConditionVariable(&g_jcv);
    while (WaitForSingleObject(t, 50) == WAIT_TIMEOUT) CancelSynchronousIo(t);   /* never stuck in a read either */
    CloseHandle(t);
    if (dt) {
      while (WaitForSingleObject(dt, 50) == WAIT_TIMEOUT) { WakeAllConditionVariable(&g_jcv); CancelSynchronousIo(dt); }
      CloseHandle(dt);
    }
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

int smp_sf_pcm_acquire(SmpFont *f, int sample, SmpSfView *view) {
  SfPcm *p;
  if (view) memset(view, 0, sizeof *view);
  if (!f || sample < 0 || sample >= f->nshdr) return 0;
  InterlockedIncrement(&f->voices[sample]);         /* first count ourselves, then read (see the top comment) */
  p = f->pcm[sample];
  if (!p) { InterlockedDecrement(&f->voices[sample]); return 0; }
  /* everything from the one pointer read: it stays valid while held, even after the sample is retired */
  if (view) {
    view->head = p->data; view->is_float = p->is_float;
    view->head_frames = p->head_frames; view->frames = p->frames;
    view->loop_start = p->loop_start; view->loop_end = p->loop_end;
    view->stream = p->head_frames < p->frames ? p : NULL;
    view->tail = p->tail; view->tail_start = p->tail_start; view->tail_frames = p->tail_frames;
    view->rate = p->rate;
  }
  return 1;
}

void sf_hold(SmpFont *f, int sample) { InterlockedIncrement(&f->voices[sample]); }

void smp_sf_pcm_release(SmpFont *f, int sample) {
  if (!f || sample < 0 || sample >= f->nshdr) return;
  InterlockedDecrement(&f->voices[sample]);
}

long smp_sf_stream_read(const void *stream, uint64_t frame, float *out, long n) {
  const SfPcm *p = (const SfPcm *)stream;
  long got = 0;
  if (n <= 0) return 0;
  if (p && frame < p->frames) {
    long want = (uint64_t)n < p->frames - frame ? n : (long)(p->frames - frame);
    if (p->h != INVALID_HANDLE_VALUE) {             /* a PCM file, or a DECODED sample's cache file */
      /* through a small buffer on the stack, a piece at a time: no allocation, and a failed read stops here */
      uint8_t buf[16384];
      int fb = pcm_bytes(p->enc) * p->channels;
      long per = (long)(sizeof buf / (size_t)fb);
      while (got < want) {
        long k = want - got < per ? want - got : per, r;
        DWORD rb = 0;
        if (!smp_pread(p->h, p->offset + (frame + (uint64_t)got) * (uint64_t)fb, buf, (DWORD)(k * fb), &rb)) break;
        r = (long)(rb / (DWORD)fb);
        if (r > 0) pcm_convert(buf, p->enc, p->channels, p->channel, (uint32_t)r, out + got, NULL);
        got += r;
        if (r < k) break;                                            /* short: the file ended early */
      }
    }
  }
  if (got < n) memset(out + got, 0, (size_t)(n - got) * sizeof(float));
  return got;
}

/* a pending voice's sample, loaded before the loader's walk gets to it (any thread, the audio thread too: an
   Interlocked store and a SetEvent) */
void smp_sf_hint(SmpFont *f, int sample) {
  LONG w;
  if (!f || sample < 0 || sample >= f->nshdr || f->pcm[sample]) return;
  w = InterlockedIncrement(&f->hint_w);
  InterlockedExchange(&f->hint[(w - 1) & (SF_HINTS - 1)], sample + 1);
  SetEvent(g_wake);
}

/* An offline render's wait: until sample (or, when it is -1, every sample the presets in use need) is loaded, or
   broken, or can't load for now; with no fixed cap while the loader keeps making progress on this font (samples
   loaded, decode pieces), and giving up after STALL_MS without any. 0 done, 1 gave up. */
int smp_sf_wait_loaded(SmpFont *f, int sample) {
  LONG seen;
  ULONGLONG since = GetTickCount64();
  if (!f) return 0;
  if (sample >= f->nshdr) sample = -1;
  seen = f->progress;
  for (;;) {
    LONG u = InterlockedCompareExchange(&f->use_gen, 0, 0), pr;
    if (sample >= 0 && (f->pcm[sample] || ((volatile uint8_t *)f->state)[sample] == SF_ST_BROKEN)) return 0;
    if (InterlockedCompareExchange(&f->done_gen, 0, 0) == u || InterlockedCompareExchange(&f->stuck_gen, 0, 0) == u)
      return 0;
    pr = f->progress;
    if (pr != seen) { seen = pr; since = GetTickCount64(); }
    else if (GetTickCount64() - since >= STALL_MS) return 1;
    Sleep(1);
  }
}

/* pending retires (unneeded samples waiting out the grace or a holder) don't count as busy, and neither do
   samples that can't load for now (see the end of pass) */
int smp_sf_wait_idle(SmpFont *f, int timeout_ms) {
  ULONGLONG end = GetTickCount64() + (ULONGLONG)(timeout_ms > 0 ? timeout_ms : 0);
  if (!f) return 0;
  for (;;) {
    LONG u = InterlockedCompareExchange(&f->use_gen, 0, 0);
    if (InterlockedCompareExchange(&f->done_gen, 0, 0) == u || InterlockedCompareExchange(&f->stuck_gen, 0, 0) == u)
      return 0;
    if (GetTickCount64() >= end) return 1;
    Sleep(1);
  }
}

int smp_sf_loaded_samples(const SmpFont *f) { return f ? (int)f->nloaded : 0; }

const char *smp_sf_problem(const SmpFont *f) {
  if (f && InterlockedCompareExchange((volatile LONG *)&f->has_problem2, 0, 0)) return f->problem2;
  if (!f || !InterlockedCompareExchange((volatile LONG *)&f->has_problem, 0, 0)) return "";
  return f->problem;
}

uint64_t smp_sf_loaded_bytes(const SmpFont *f) {
  return f ? (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)&f->loaded_bytes, 0, 0) : 0;
}
