/* sffont.c: the process-wide font registry, preset use counts, the loader thread, sample heads and the reads that
   stream the rest of a sample.

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

   Heads and streaming. A loaded sample keeps only its head in memory: the first quarter second, or the whole
   sample when it is under 1 s. A voice starts from the head at once; the rest is read by the synth's streamer
   thread through smp_sf_stream_read, from the font file (PCM_FILE: one shared handle per file, opened by the loader
   with full sharing, read with positional reads so any number of streamers can share it) or from a mono float
   cache file (DECODED: SF3's and SF4's compressed samples, decoded once by the loader into the cache folder; the
   second open of the font decodes nothing). A stream handle is the loaded sample itself, so it is valid exactly
   as long as the sample is held.

   SF2Pack. The whole sample data is one compressed stream. The first time a preset of the font is used, the loader
   decodes it, in its own pass (so off the main thread; other fonts wait for that pass), into a 16-bit cache file,
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
#include "decode.h"

#define DEFAULT_GRACE_MS 2000
#define HELD_RECHECK_MS 20                 /* how often a retired sample that someone still holds is checked */
#define RETRY_MS 500                       /* how often a load that failed for now (no file, no memory) is retried */
#define MAX_DECODED_FRAMES (1u << 26)      /* a decoded sample's limit (25 minutes at 44.1 kHz), against lying headers */
#define MIN_HEAD 1024                      /* the shortest head, for samples with absurdly low rates */

static SRWLOCK g_lock = SRWLOCK_INIT;      /* guards the list, g_busy and g_thread */
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;   /* signalled when the loader lets go of a font */
static SmpFont **g_fonts; static int g_nfonts, g_capfonts;
static SmpFont *g_busy;                    /* the font the loader is working on, outside the lock */
static HANDLE g_thread;
static HANDLE g_wake;                      /* auto-reset; made once and never closed, so SetEvent is always safe */
static volatile LONG g_stop;
static volatile LONG g_grace = DEFAULT_GRACE_MS;
static volatile LONG g_whole;              /* tests: samples are held whole */
static volatile LONG g_decodes;            /* tests: compressed samples decoded */

void smp_sf_test_grace(int ms) { InterlockedExchange(&g_grace, ms > 0 ? ms : DEFAULT_GRACE_MS); }
void smp_sf_test_whole(int on) { InterlockedExchange(&g_whole, on ? 1 : 0); }
long smp_sf_test_decodes(void) { return g_decodes; }

static volatile LONG g_pause;              /* tests: the loader does nothing while set */
void smp_sf_test_pause(int on) {
  InterlockedExchange(&g_pause, on ? 1 : 0);
  if (!on && g_wake) SetEvent(g_wake);
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

/* The handle for path_id in this pass (loader thread only). Each pass opens the file by name, as stage 2 did, so a
   file that has gone away stops new samples loading (the ones loaded keep playing). The first time, a duplicate of
   then, a second handle opened by name becomes the font's streaming handle, kept until the font closes, so streams
   read the very file the headers came from even if it is renamed meanwhile. It is overlapped, so the streamers of
   every player on the font read it in parallel (smp_pread). NULL when the file can't be opened now. */
static HANDLE font_handle(SmpFont *f, int path_id) {
  HANDLE h;
  wchar_t w[2048];
  const wchar_t *name = f->wpath;
  if (path_id < 0 || path_id >= f->npaths) return NULL;
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
    HANDLE d = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (d == INVALID_HANDLE_VALUE) { CloseHandle(h); return NULL; }
    f->fh[path_id] = d;
  }
  f->ph[path_id] = h;
  return h;
}

/* A DECODED sample: its head from its cache file when there is one; else decoded now (once) and written there.
   When the cache can't be written, the whole decoded sample is held in memory instead. NULL with *broken set when
   the data can never decode; NULL without it when memory ran out or the file can't be opened for now. */
static SfPcm *load_decoded(SmpFont *f, const SmpSrc *c, uint32_t rate, int *broken) {
  SmpEntry *e = smp_cache_open_mono(c->key);
  SfPcm *p;
  uint32_t n, hf;
  if (!e) {
    float *x; int ch = 0, r = 0; long frames = 0; char err[256];
    const float *mono;
    if (c->length) {
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
      x = decode_memory(bytes, (size_t)c->length, &ch, &frames, &r, err, sizeof err);
      free(bytes);
    } else {
      x = decode_audio(f->paths[c->path_id], &ch, &frames, &r, err, sizeof err);
    }
    if (!x) { *broken = 1; return NULL; }
    InterlockedIncrement(&g_decodes);
    if (frames <= 0 || frames > (long)MAX_DECODED_FRAMES || ch <= 0) { free(x); *broken = 1; return NULL; }
    n = (uint32_t)frames;
    mono = x + (size_t)(c->channel < ch ? c->channel : 0) * (size_t)n;   /* channel after channel */
    /* every decoded sample goes to the cache, short ones too, so the next open decodes nothing */
    if (smp_cache_write_mono(c->key, mono, n, r > 0 ? r : (int)rate) == 0) e = smp_cache_open_mono(c->key);
    hf = e ? head_of(n, rate) : n;                                  /* no cache: held whole */
    p = new_pcm(n, hf, 1);
    if (!p) { free(x); if (e) smp_cache_close(e); return NULL; }
    memcpy(p->data, mono, (size_t)hf * sizeof(float));
    free(x);
    if (hf < n) p->entry = e;
    else if (e) smp_cache_close(e);
    return p;
  }
  if (smp_entry_frames(e) == 0 || smp_entry_frames(e) > MAX_DECODED_FRAMES) { smp_cache_close(e); *broken = 1; return NULL; }
  n = (uint32_t)smp_entry_frames(e);
  hf = head_of(n, rate);
  p = new_pcm(n, hf, 1);
  if (!p) { smp_cache_close(e); return NULL; }
  if (smp_entry_read_mono(e, 0, (float *)p->data, (long)hf) != (long)hf) { smp_cache_close(e); free(p); *broken = 1; return NULL; }
  if (hf < n) p->entry = e;
  else smp_cache_close(e);
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
    ok = smp_entry_read_mono(p->entry, t0, (float *)(b + SF_PCM_PAD * w), (long)n) == (long)n;
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
  if (c->kind == SMP_SRC_DECODED) {
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
  /* the loop inside the data; a backwards loop becomes an empty one at its end */
  if (ls > p->frames) ls = p->frames;
  if (le > p->frames) le = p->frames;
  if (ls > le) ls = le;
  p->loop_start = (uint32_t)ls; p->loop_end = (uint32_t)le;
  if (!load_tail(f, c, p, rate)) { free_pcm(p); return NULL; }   /* out of memory: tried again later */
  return p;
}

/* the font's problem, said once: written before has_problem is set, so a reader that sees the flag sees the text */
static void set_problem(SmpFont *f, const char *fmt, const char *a, const char *b) {
  if (InterlockedCompareExchange(&f->has_problem, 0, 0)) return;
  snprintf(f->problem, sizeof f->problem, fmt, a, b ? b : "");
  InterlockedExchange(&f->has_problem, 1);
}

/* the font's file name, for its problem */
static const char *font_name(const SmpFont *f) {
  const char *b = f->path, *q;
  for (q = f->path; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
  return b;
}

/* pack_prepare's cancel hook: the font is closing, or the loader is stopping */
static int pack_cancel(void *arg) {
  SmpFont *f = arg;
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
  int s, p, k, aborted = 0, retry = 0, cant_open = 0;

  memset(f->need, 0, (size_t)f->nshdr);
  for (p = 0; p < f->nphdr; p++) {
    const SmpSfRegion *r; int n;
    if (f->users[p] <= 0) continue;
    n = smp_sf_regions(f, p, &r);
    for (k = 0; k < n; k++) if (r[k].sample >= 0 && r[k].sample < f->nshdr) f->need[r[k].sample] = 1;
  }
  if (f->is_pack && f->pack_state == SF_PACK_NONE && !f->closing && !g_stop) {   /* the pack first, once needed */
    for (s = 0; s < f->nshdr && !f->need[s]; s++) {}
    if (s < f->nshdr) pack_prepare(f);
  }

  for (s = 0; s < f->nshdr; s++) {
    if (f->closing || g_stop) { aborted = 1; break; }
    if (f->need[s]) {
      f->idle_since[s] = 0;
      if (f->state[s] == SF_ST_RETIRED) {          /* needed again before it was freed: publish it again */
        publish(f, s, f->held[s]);
        f->state[s] = SF_ST_PUBLISHED;
      } else if (f->state[s] == SF_ST_NONE && f->is_pack && f->pack_state != SF_PACK_READY) {
        if (f->pack_state == SF_PACK_BROKEN) f->state[s] = SF_ST_BROKEN;   /* the pack never decodes */
        else retry = 1;                                                     /* tried again in RETRY_MS */
      } else if (f->state[s] == SF_ST_NONE && cant_open) {
        retry = 1;
      } else if (f->state[s] == SF_ST_NONE) {
        SfPcm *pc; int broken;
        if (!font_handle(f, f->src[s].path_id)) { cant_open = retry = 1; continue; }   /* tried again in RETRY_MS */
        pc = load_sample(f, s, &broken);
        if (pc) {
          f->held[s] = pc;
          InterlockedIncrement(&f->nloaded);
          InterlockedExchangeAdd64(&f->loaded_bytes, (LONG64)pc->bytes);
          publish(f, s, pc);
          f->state[s] = SF_ST_PUBLISHED;
        } else if (broken) {
          f->state[s] = SF_ST_BROKEN;                /* never tried again */
        } else {
          retry = 1;                                 /* no file or no memory: tried again in RETRY_MS */
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
  for (s = 0; s < f->npaths; s++) if (f->ph[s]) { CloseHandle(f->ph[s]); f->ph[s] = NULL; }
  /* A pass that left a needed sample unloaded for now (the file can't be opened, or memory ran out) isn't done: it
     runs again in RETRY_MS, and the status still counts the presets as loading. But it is as done as it can be
     for now, so wait_idle stops waiting (an offline render goes on, silent where samples are missing, rather than
     waiting 10 s a block); a later use() makes waiters wait for the next pass again. */
  if (retry && RETRY_MS < next) next = RETRY_MS;
  if (!aborted && !retry) InterlockedExchange(&f->done_gen, gen);
  if (!aborted && retry) InterlockedExchange(&f->stuck_gen, gen);
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

/* ---- the registry ---- */

static void free_font(SmpFont *f) {
  int s;
#ifdef _DEBUG
  /* the caller's guarantee (see the top): no voice still holds a sample */
  if (f->voices) for (s = 0; s < f->nshdr; s++)
    if (f->voices[s] > 0) { OutputDebugStringA("samplosis: a SoundFont was closed while a voice held a sample\n"); break; }
#endif
  if (f->held) for (s = 0; s < f->nshdr; s++) free_pcm(f->held[s]);
  if (f->fh) for (s = 0; s < f->npaths; s++) if (f->fh[s]) CloseHandle(f->fh[s]);
  if (f->pack_entry) smp_cache_close(f->pack_entry);
  free((void *)f->users); free((void *)f->voices); free((void *)f->pcm); free(f->fh); free(f->ph);
  free(f->held); free(f->state); free(f->need); free(f->idle_since); free(f->wpath);
  f->wpath = NULL;
  sf_free(f);
}

/* the cache keys of the font's DECODED samples: the file's full path, size and modified time, the byte range and
   the channel, so a changed file never finds a stale copy */
static void source_keys(SmpFont *f) {
  char path[2048 * 3], d[2048 * 3 + 160];
  int s;
  if (!WideCharToMultiByte(CP_UTF8, 0, f->wpath, -1, path, (int)sizeof path, NULL, NULL)) snprintf(path, sizeof path, "%s", f->path);
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
  f->stuck_gen = -1;
  if (!(f->users && f->voices && f->pcm && f->held && f->state && f->need && f->idle_since && f->fh && f->ph)) return 0;
  source_keys(f);
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
  if (GetFileAttributesExW(full, GetFileExInfoStandard, &a)) {
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
    if (found) { free(full); return found; }
  }
  f = sf_parse(path, err, cap);
  if (!f) { free(full); return NULL; }
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
    if (p->entry) {
      got = smp_entry_read_mono(p->entry, frame, out, want);
      if (got < 0) got = 0;
    } else if (p->h != INVALID_HANDLE_VALUE) {
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
  if (!f || !InterlockedCompareExchange((volatile LONG *)&f->has_problem, 0, 0)) return "";
  return f->problem;
}

uint64_t smp_sf_loaded_bytes(const SmpFont *f) {
  return f ? (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)&f->loaded_bytes, 0, 0) : 0;
}
