/* decode.c: a sound file read whole into float, channel after channel. The decode chain:
   1. the built-in decoders, picked by the file's first bytes: WAV in any encoding (PCM 8 to 32 bits, float 32 or 64,
      A-law, mu-law, ADPCM) in RIFF, RIFX, RF64 and Wave64 containers, and AIFF and AIFF-C, all through dr_wav; FLAC
      through dr_flac; MP3 through dr_mp3; Ogg Vorbis through stb_vorbis. All four are public domain (or MIT-0),
      single files in decode/ (dr_libs 8159355, stb 003501f). CAF (Apple's Core Audio Format) with PCM or float
      audio is read here.
   2. the library decoders, picked by the first bytes too: Opus (an Ogg file whose first packet is OpusHead) through
      opusfile, libopus and libogg; WavPack ("wvpk"); Monkey's Audio ("MAC ") through the C++ SDK behind
      codecs/mac_shim.cpp. All are BSD licensed and vendored in codecs/ (each folder's VERSION.txt names the
      release).
   3. raw PCM, for files ending .raw or .pcm: headerless samples read as the Settings say (rate, channels, format,
      byte order).
   4. when the first bytes say nothing, the built-in decoders are tried one after another (MP3 last: it finds frames
      in nearly anything). First bytes that name a container only Windows reads (MPEG-4 and 3GP "ftyp", ASF, AMR,
      ADTS AAC) skip this, so MP3 can't find false frames in them.
   5. a file (never bytes in memory) that nothing above read, and that isn't a raw name, goes to Windows Media
      Foundation (mfdecode.c, on a helper thread with a stall timer): AAC, WMA, and whatever else this machine's
      Windows decodes. by says Windows.
   6. what Windows doesn't read either goes to FFmpeg, when it's installed (ffdecode.c, run as a child process with a
      stall timer): DSD (.dsf, .dff), TTA, Matroska, and anything else FFmpeg decodes. by says FFmpeg. Whatever decodes a file, its result passes one place (finish): a sample
   that isn't finite, or is absurdly large (a damaged file), becomes silence. Never called on the audio thread. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "plat.h"
#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#include "decode/dr_wav.h"
#include "decode/dr_flac.h"
#include "decode/dr_mp3.h"
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "decode/stb_vorbis.c"
#include <opusfile.h>
#include <wavpack.h>
#include "decode.h"
#include "mfdecode.h"
#include "ffdecode.h"

/* The size limit, in one place for every decoder: 2 GiB of decoded float. A long recording still fits; anything
   bigger is refused with TOO_LONG. */
static size_t dec_max_floats = (size_t)1 << 29;            /* tests may lower it (flag 2) */
#define DEC_MAX_FLOATS dec_max_floats
#define TOO_LONG "it's too long to load (over 2 GB decoded)"
#define NOT_SOUND "it isn't a sound file Samplosis can read"
#define CANCELLED "cancelled"

/* decode_memory_ex's cancel hook, for the calling thread only: asked by grow_need once per chunk, so every decoder
   stops within a chunk of being asked, without each one taking a parameter */
static __declspec(thread) int (*t_cancel)(void *);
static __declspec(thread) void *t_cancel_arg;

/* codecs/mac_shim.cpp: open (a handle, or NULL and a message), read up to 4096 interleaved float frames (how many,
   0 at the end, -1 on a decoder failure), close */
void *smp_ape_open(const unsigned char *b, size_t len, unsigned *ch, unsigned *sr, unsigned long long *claimed, char *err, int cap);
int smp_ape_read(void *h, float *out, int frames);
void smp_ape_close(void *h);
void smp_ape_set_debug_flags(long f);
/* TESTS ONLY: 1 = Monkey's Audio throws inside its guard; 2 = the size limit is 65536 floats instead of 2 GiB;
   4, 8, 16, 32 and 64 are the Windows stage's (SMP_MF_TEST_* in mfdecode.h); 128, 256 and 512 the FFmpeg stage's
   (SMP_FF_TEST_* in ffdecode.h) */
SMP_API void smp_test_only_decode_flags(long f) {
  smp_ape_set_debug_flags(f & 1);
  smp_mf_set_test_flags(f & (SMP_MF_TEST_SLOW | SMP_MF_TEST_NO_MF | SMP_MF_TEST_BUFFER_FAILS_FIRST |
                             SMP_MF_TEST_BUFFER_FAILS_THIRD | SMP_MF_TEST_BUSY));
  smp_ff_set_test_flags(f & (SMP_FF_TEST_STALL_SHORT | SMP_FF_TEST_MISSING | SMP_FF_TEST_SLOW_START));
  dec_max_floats = f & 2 ? 65536 : (size_t)1 << 29;
}
SMP_API void smp_free(void *p) { free(p); }

/* the sampler's extensions: what Browse and the File combo box list */
static const wchar_t *const audio_ext[] = {
  L".wav", L".flac", L".mp3", L".ogg", L".aif", L".aiff", L".aifc", L".w64", L".rf64", L".caf", L".raw", L".pcm",
  L".opus", L".wv", L".ape", L".m4a", L".aac", L".mp4", L".wma", L".ac3", L".amr", L".3gp", L".alac",
  L".dsf", L".dff", L".tta", L".mka", L".webm", L".au", L".snd", L".mpc", L".spx", L".oga", L".m4b", L".eac3",
  L".dts", L".mlp", L".thd", L".voc", L".mov", L".mkv"};
int smp_audio_extensions(const wchar_t ***list) {
  *list = (const wchar_t **)audio_ext;
  return (int)(sizeof audio_ext / sizeof audio_ext[0]);
}

static const char *ext_of(const char *path) {
  const char *dot = strrchr(path, '.'), *s1 = strrchr(path, '\\'), *s2 = strrchr(path, '/');
  if (!dot || (s1 && s1 > dot) || (s2 && s2 > dot)) return "";
  return dot;
}
int smp_is_raw_name(const char *path) {
  const char *e = ext_of(path);
  return !_stricmp(e, ".raw") || !_stricmp(e, ".pcm");
}

/* The file's bytes. The path is UTF-8, so names in any language open on Windows. */
static unsigned char *slurp(const char *path, size_t *len) {
  FILE *fp = NULL; unsigned char *b = NULL; long long n;
#ifdef _WIN32
  wchar_t w[2048];
  if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048)) fp = _wfopen(w, L"rb");
#else
  {   /* paths the shared code built Windows-style ("dir\\name") */
    char *q = strdup(path);
    if (!q) return NULL;
    for (char *c = q; *c; c++) if (*c == '\\') *c = '/';
    fp = fopen(q, "rb");
    free(q);
  }
#endif
  if (!fp) return NULL;
  _fseeki64(fp, 0, SEEK_END); n = _ftelli64(fp); _fseeki64(fp, 0, SEEK_SET);
  b = malloc(n > 0 ? (size_t)n : 1);                /* an empty file reads as no bytes, not as a failure */
  if (b && n > 0 && fread(b, 1, (size_t)n, fp) != (size_t)n) { free(b); b = NULL; }
  fclose(fp);
  *len = n > 0 ? (size_t)n : 0;
  return b;
}

/* interleaved frames to channel after channel */
static float *planar(const float *x, unsigned long long n, unsigned ch) {
  float *out = malloc(sizeof(float) * (size_t)(n ? n : 1) * ch);
  if (out)
    for (unsigned long long i = 0; i < n; i++)
      for (unsigned c = 0; c < ch; c++) out[(size_t)c * n + i] = x[(size_t)i * ch + c];
  return out;
}

/* PCM bytes to float, channel after channel: bytes per sample 1 to 4 (integer) or 4 and 8 (float); 8-bit samples
   are signed unless is_unsigned */
static float *pcm_planar(const unsigned char *p, uint64_t n, int ch, int bytes, int is_float, int is_unsigned, int big) {
  float *out = malloc(sizeof(float) * (size_t)(n ? n : 1) * (size_t)ch);
  uint64_t i; int c, k;
  if (!out) return NULL;
  for (i = 0; i < n; i++)
    for (c = 0; c < ch; c++) {
      const unsigned char *s = p + ((size_t)i * ch + c) * bytes;
      uint64_t u = 0; float v;
      for (k = 0; k < bytes; k++) u |= (uint64_t)s[big ? k : bytes - 1 - k] << (8 * (bytes - 1 - k));
      if (is_float && bytes == 4) { uint32_t w = (uint32_t)u; memcpy(&v, &w, 4); }   /* finish() makes NaN safe */
      else if (is_float) { double d; memcpy(&d, &u, 8); v = fabs(d) < 1e30 ? (float)d : 0.0f; }
      else if (is_unsigned) v = ((float)u - 128.0f) / 128.0f;
      else {
        int64_t sv = (int64_t)(u << (64 - 8 * bytes)) >> (64 - 8 * bytes);   /* sign-extend */
        v = (float)((double)sv / (double)(1ull << (8 * bytes - 1)));
      }
      out[(size_t)c * n + i] = v;
    }
  return out;
}

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const unsigned char *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

/* CAF: a big-endian chunk file; 'desc' says the format, 'data' holds it (after a 4-byte edit count). Only 'lpcm'
   (integer or float PCM) reads here; anything else says what it is. */
static float *caf_read(const unsigned char *b, size_t len, unsigned *ch, unsigned *sr, unsigned long long *n, char *err, int cap) {
  size_t at = 8; int have_desc = 0;
  double rate = 0; uint32_t fmt = 0, flags = 0, bpp = 0, fpp = 0, chans = 0, bits = 0;
  while (at + 12 <= len) {
    const unsigned char *h = b + at;
    uint64_t size = be64(h + 4);
    size_t body = at + 12, left = len - body;
    if (!memcmp(h, "desc", 4) && size >= 32 && left >= 32) {
      uint64_t r = be64(h + 12); memcpy(&rate, &r, 8);
      fmt = be32(h + 20); flags = be32(h + 24); bpp = be32(h + 28); fpp = be32(h + 32);
      chans = be32(h + 36); bits = be32(h + 40);
      have_desc = 1;
    } else if (!memcmp(h, "data", 4)) {
      uint64_t data = size == UINT64_MAX || size > left ? left : size;   /* -1: to the end of the file */
      int is_float = flags & 1, little = (flags & 2) != 0, bytes;
      if (!have_desc) break;
      if (fmt != 0x6C70636D) {                                      /* 'lpcm' */
        char id[5] = {(char)(fmt >> 24), (char)(fmt >> 16), (char)(fmt >> 8), (char)fmt, 0};
        snprintf(err, (size_t)cap, "it's a CAF file with '%s' audio, which Samplosis can't read yet", id);
        return NULL;
      }
      bytes = chans ? (int)((bpp ? bpp : chans * ((bits + 7) / 8)) / chans) : 0;
      if (!chans || chans > 64 || !(rate >= 1 && rate <= 1e6) || (fpp && fpp != 1) ||
          (bpp && bpp != (uint32_t)bytes * chans) ||
          (is_float ? !((bits == 32 && bytes == 4) || (bits == 64 && bytes == 8))
                    : !(bits >= 1 && bytes >= 1 && bytes <= 4 && (uint32_t)bytes == (bits + 7) / 8))) {
        snprintf(err, (size_t)cap, "it's a CAF file with %u-bit %s samples in %d bytes, which Samplosis can't read",
                 bits, is_float ? "float" : "integer", bytes);
        return NULL;
      }
      if (data < 4) break;
      *ch = chans; *sr = (unsigned)(rate + 0.5);
      *n = (data - 4) / ((uint64_t)bytes * chans);
      if (!*n) break;                                               /* no whole frame: cut short */
      if (*n * chans > DEC_MAX_FLOATS) { snprintf(err, (size_t)cap, TOO_LONG); return NULL; }
      {
        float *x = pcm_planar(b + body + 4, *n, (int)chans, bytes, is_float, 0, !little);
        if (!x) snprintf(err, (size_t)cap, "out of memory");
        return x;
      }
    }
    if (size == UINT64_MAX || size > left) break;
    at = body + (size_t)size;
  }
  snprintf(err, (size_t)cap, "it's a CAF file without its audio, or it's cut short");
  return NULL;
}

/* raw PCM as the Settings say */
static float *raw_read(const unsigned char *b, size_t len, unsigned *ch, unsigned *sr, unsigned long long *n, char *err, int cap) {
  static const int bytes_of[] = {2, 3, 4, 4, 1};
  SmpSettings s; int bytes;
  smp_settings_get(&s);
  bytes = bytes_of[s.raw_format >= 0 && s.raw_format <= SMP_RAW_U8 ? s.raw_format : 0];
  *ch = (unsigned)(s.raw_channels == 1 ? 1 : 2); *sr = (unsigned)s.raw_rate;
  *n = len / ((size_t)bytes * *ch);
  if (!*n) { snprintf(err, (size_t)cap, "it's a raw file too short to hold one sample"); return NULL; }
  if (*n * *ch > DEC_MAX_FLOATS) { snprintf(err, (size_t)cap, TOO_LONG); return NULL; }
  {
    float *x = pcm_planar(b, *n, (int)*ch, bytes, s.raw_format == SMP_RAW_F32, s.raw_format == SMP_RAW_U8,
                          s.raw_endian == SMP_RAW_BIG);
    if (!x) snprintf(err, (size_t)cap, "out of memory");
    return x;
  }
}

/* A growing interleaved buffer. Every decoder reads into one in chunks, because a header's length can't be trusted
   (a damaged or crafted file may claim billions of frames). */
typedef struct { float *x; size_t n, cap; int too_long, cancelled; } Grow;   /* n and cap count floats */
static int grow_need(Grow *g, size_t more) {
  if (t_cancel && t_cancel(t_cancel_arg)) { g->cancelled = 1; return 0; }
  if (more > DEC_MAX_FLOATS || g->n + more > DEC_MAX_FLOATS) { g->too_long = 1; return 0; }
  if (g->n + more > g->cap) {
    size_t c = g->cap ? g->cap : 65536;
    float *y;
    while (c < g->n + more) c *= 2;
    if (c > DEC_MAX_FLOATS) c = DEC_MAX_FLOATS;
    if (!(y = realloc(g->x, c * sizeof(float)))) return 0;
    g->x = y; g->cap = c;
  }
  return 1;
}
/* Room reserved up front for what the header claims (frames x channels): a good file then needs no regrowth. Never
   more than 64 floats per byte of the file, nor the limit, so a lying header costs nothing. */
static void grow_hint(Grow *g, unsigned long long claimed_floats, size_t len) {
  unsigned long long h = claimed_floats, most = (unsigned long long)len * 64;
  if (h > most) h = most;
  if (h > DEC_MAX_FLOATS) h = DEC_MAX_FLOATS;
  if (h > g->cap) grow_need(g, (size_t)h);
}
/* a failed grow: the buffer freed and the reason */
static float *grow_fail(Grow *g, char *err, int cap) {
  free(g->x); g->x = NULL;
  snprintf(err, (size_t)cap, "%s", g->cancelled ? CANCELLED : g->too_long ? TOO_LONG : "out of memory");
  return NULL;
}
#define DEC_CHUNK 4096                                        /* frames per read for the decoders that take a count */

/* Opus through opusfile, always at 48000 Hz (opusfile drops the encoder's pre-skip). A chained file whose links have
   different channel counts reads as stereo. */
#define OPUS_CHUNK (5760 * 8)                                 /* 120 ms of 8 channels, the most one call gives */
static float *opus_read(const unsigned char *b, size_t len, unsigned *ch, unsigned *sr, unsigned long long *n, char *err, int cap) {
  int e = 0, links, i, c, same = 1;
  OggOpusFile *of = op_open_memory(b, len, &e);
  Grow g = {0};
  ogg_int64_t total;
  if (!of) { snprintf(err, (size_t)cap, "it's an Opus file Samplosis can't read (it may be damaged)"); return NULL; }
  links = op_link_count(of);
  c = op_channel_count(of, 0);
  for (i = 1; i < links; i++) if (op_channel_count(of, i) != c) same = 0;
  if (!same) c = 2;
  if (c < 1 || c > 8) {
    op_free(of);
    snprintf(err, (size_t)cap, "it's an Opus file with %d channels, which Samplosis can't read", c);
    return NULL;
  }
  total = op_pcm_total(of, -1);
  if (total > 0) grow_hint(&g, (unsigned long long)total * c + OPUS_CHUNK, len);
  for (;;) {
    int got;
    if (!grow_need(&g, OPUS_CHUNK)) { op_free(of); return grow_fail(&g, err, cap); }
    got = same ? op_read_float(of, g.x + g.n, OPUS_CHUNK, NULL) : op_read_float_stereo(of, g.x + g.n, OPUS_CHUNK);
    if (got == OP_HOLE) continue;                             /* a gap in a damaged file: read on past it */
    if (got <= 0) break;
    g.n += (size_t)got * c;
  }
  op_free(of);
  if (!g.n) { free(g.x); snprintf(err, (size_t)cap, "it's an Opus file without its audio, or it's damaged"); return NULL; }
  *ch = (unsigned)c; *sr = 48000; *n = g.n / c;
  return g.x;
}

/* WavPack from memory: a reader over the bytes that never goes outside them */
typedef struct { const unsigned char *b; int64_t len, at; } WvMem;
static int32_t wv_read(void *id, void *data, int32_t k) {
  WvMem *m = id; int64_t left = m->at < m->len ? m->len - m->at : 0;
  if (k < 0) k = 0;
  if (k > left) k = (int32_t)left;
  if (k) memcpy(data, m->b + m->at, (size_t)k);
  m->at += k;
  return k;
}
static int32_t wv_write(void *id, void *data, int32_t k) { (void)id; (void)data; (void)k; return 0; }
static int64_t wv_pos(void *id) { return ((WvMem *)id)->at; }
static int wv_seek_to(WvMem *m, int64_t to) { if (to < 0 || to > m->len) return -1; m->at = to; return 0; }
static int wv_set_abs(void *id, int64_t pos) { return wv_seek_to(id, pos); }
static int wv_set_rel(void *id, int64_t d, int mode) {
  WvMem *m = id;
  return wv_seek_to(m, (mode == SEEK_SET ? 0 : mode == SEEK_CUR ? m->at : m->len) + d);
}
static int wv_push_back(void *id, int c) { WvMem *m = id; if (m->at <= 0) return EOF; m->at--; return c; }
static int64_t wv_length(void *id) { return ((WvMem *)id)->len; }
static int wv_can_seek(void *id) { (void)id; return 1; }
static int wv_truncate(void *id) { (void)id; return -1; }
static int wv_close(void *id) { (void)id; return 0; }

/* WavPack: integer samples of 1 to 4 bytes, or float (normalized to +/-1); DSD files read as PCM */
static float *wavpack_read(const unsigned char *b, size_t len, unsigned *ch, unsigned *sr, unsigned long long *n, char *err, int cap) {
  static WavpackStreamReader64 rd = {wv_read, wv_write, wv_pos, wv_set_abs, wv_set_rel, wv_push_back, wv_length,
                                     wv_can_seek, wv_truncate, wv_close};
  WvMem m = {b, (int64_t)len, 0};
  char why[90] = "";
  WavpackContext *w = WavpackOpenFileInputEx64(&rd, &m, NULL, why, OPEN_NORMALIZE | OPEN_DSD_AS_PCM, 0);
  int c, bytes, is_float;
  uint32_t rate, i, got;
  int64_t total;
  int32_t *buf;
  Grow g = {0};
  double scale;
  if (!w) { snprintf(err, (size_t)cap, "it's a WavPack file Samplosis can't read (it may be damaged)"); return NULL; }
  c = WavpackGetNumChannels(w); bytes = WavpackGetBytesPerSample(w); rate = WavpackGetSampleRate(w);
  is_float = (WavpackGetMode(w) & MODE_FLOAT) != 0;
  if (c < 1 || c > 64 || bytes < 1 || bytes > 4 || rate < 1 || rate > 1000000) {
    WavpackCloseFile(w);
    snprintf(err, (size_t)cap, "it's a WavPack file with a format Samplosis can't read");
    return NULL;
  }
  scale = 1.0 / (double)(1u << (8 * bytes - 1));
  total = WavpackGetNumSamples64(w);
  if (total > 0) grow_hint(&g, (unsigned long long)total * c, len);
  if (!(buf = malloc(sizeof(int32_t) * DEC_CHUNK * (size_t)c))) {
    WavpackCloseFile(w); free(g.x); snprintf(err, (size_t)cap, "out of memory"); return NULL;
  }
  while ((got = WavpackUnpackSamples(w, buf, DEC_CHUNK)) > 0) {
    if (!grow_need(&g, (size_t)got * c)) { free(buf); WavpackCloseFile(w); return grow_fail(&g, err, cap); }
    for (i = 0; i < got * (uint32_t)c; i++) {
      float v;
      if (is_float) memcpy(&v, &buf[i], 4);
      else v = (float)((double)buf[i] * scale);
      g.x[g.n + i] = v;
    }
    g.n += (size_t)got * c;
  }
  free(buf);
  WavpackCloseFile(w);
  if (!g.n) { free(g.x); snprintf(err, (size_t)cap, "it's a WavPack file without its audio, or it's damaged"); return NULL; }
  *ch = (unsigned)c; *sr = rate; *n = g.n / c;
  return g.x;
}

/* Monkey's Audio through the shim; a decoder failure (a damaged frame) ends the audio there */
static float *ape_read(const unsigned char *b, size_t len, unsigned *ch, unsigned *sr, unsigned long long *n, char *err, int cap) {
  unsigned long long claimed = 0;
  void *h = smp_ape_open(b, len, ch, sr, &claimed, err, cap);
  Grow g = {0};
  int got;
  if (!h) return NULL;
  grow_hint(&g, claimed * *ch, len);
  for (;;) {
    if (!grow_need(&g, (size_t)DEC_CHUNK * *ch)) { smp_ape_close(h); return grow_fail(&g, err, cap); }
    if ((got = smp_ape_read(h, g.x + g.n, DEC_CHUNK)) <= 0) break;
    g.n += (size_t)got * *ch;
  }
  smp_ape_close(h);
  if (!g.n) { free(g.x); snprintf(err, (size_t)cap, "it's a damaged Monkey's Audio file"); return NULL; }
  *n = g.n / *ch;
  return g.x;
}

/* The built-in decoders, read in chunks the same way (their read-it-all calls trust the header's length). */
static float *wav_read(const unsigned char *b, size_t len, unsigned *ch, unsigned *sr, unsigned long long *n, char *err, int cap) {
  drwav w; Grow g = {0}; drwav_uint64 got;
  if (!drwav_init_memory(&w, b, len, NULL)) return NULL;
  if (!w.channels || !w.sampleRate) { drwav_uninit(&w); return NULL; }
  *ch = w.channels; *sr = w.sampleRate;
  grow_hint(&g, w.totalPCMFrameCount * w.channels, len);
  for (;;) {
    if (!grow_need(&g, (size_t)DEC_CHUNK * w.channels)) { drwav_uninit(&w); return grow_fail(&g, err, cap); }
    if (!(got = drwav_read_pcm_frames_f32(&w, DEC_CHUNK, g.x + g.n))) break;
    g.n += (size_t)got * w.channels;
  }
  drwav_uninit(&w);
  *n = g.n / *ch;
  return g.x;
}
static float *flac_read(const unsigned char *b, size_t len, unsigned *ch, unsigned *sr, unsigned long long *n, char *err, int cap) {
  drflac *f = drflac_open_memory(b, len, NULL); Grow g = {0}; drflac_uint64 got;
  if (!f) return NULL;
  if (!f->channels || !f->sampleRate) { drflac_close(f); return NULL; }
  *ch = f->channels; *sr = f->sampleRate;
  grow_hint(&g, f->totalPCMFrameCount * f->channels, len);
  for (;;) {
    if (!grow_need(&g, (size_t)DEC_CHUNK * f->channels)) { drflac_close(f); return grow_fail(&g, err, cap); }
    if (!(got = drflac_read_pcm_frames_f32(f, DEC_CHUNK, g.x + g.n))) break;
    g.n += (size_t)got * f->channels;
  }
  drflac_close(f);
  *n = g.n / *ch;
  return g.x;
}
static float *mp3_read(const unsigned char *b, size_t len, unsigned *ch, unsigned *sr, unsigned long long *n, char *err, int cap) {
  drmp3 m; Grow g = {0}; drmp3_uint64 got;
  if (!drmp3_init_memory(&m, b, len, NULL)) return NULL;
  if (!m.channels || !m.sampleRate) { drmp3_uninit(&m); return NULL; }
  *ch = m.channels; *sr = m.sampleRate;
  for (;;) {
    if (!grow_need(&g, (size_t)DEC_CHUNK * m.channels)) { drmp3_uninit(&m); return grow_fail(&g, err, cap); }
    if (!(got = drmp3_read_pcm_frames_f32(&m, DEC_CHUNK, g.x + g.n))) break;
    g.n += (size_t)got * m.channels;
  }
  drmp3_uninit(&m);
  *n = g.n / *ch;
  return g.x;
}
static float *vorbis_read(const unsigned char *b, size_t len, unsigned *ch, unsigned *sr, unsigned long long *n, char *err, int cap) {
  int e = 0, got;
  stb_vorbis *v = len < 0x7FFFFFFF ? stb_vorbis_open_memory(b, (int)len, &e, NULL) : NULL;
  stb_vorbis_info info;
  Grow g = {0};
  if (!v) return NULL;
  info = stb_vorbis_get_info(v);
  if (info.channels < 1 || !info.sample_rate) { stb_vorbis_close(v); return NULL; }
  *ch = (unsigned)info.channels; *sr = info.sample_rate;
  grow_hint(&g, (unsigned long long)stb_vorbis_stream_length_in_samples(v) * info.channels, len);
  for (;;) {
    if (!grow_need(&g, (size_t)DEC_CHUNK * info.channels)) { stb_vorbis_close(v); return grow_fail(&g, err, cap); }
    got = stb_vorbis_get_samples_float_interleaved(v, info.channels, g.x + g.n, DEC_CHUNK * info.channels);
    if (got <= 0) break;
    g.n += (size_t)got * info.channels;
  }
  stb_vorbis_close(v);
  *n = g.n / *ch;
  return g.x;
}

/* the built-in decoders, then the library ones; D_WINDOWS is a format only the system decoder (Windows' or the
   Mac's) or FFmpeg reads, from a file */
enum { D_NONE, D_WAV, D_FLAC, D_VORBIS, D_MP3, D_CAF, D_OPUS, D_WAVPACK, D_APE, D_WINDOWS };
#define D_IS_LIBRARY(d) ((d) >= D_OPUS && (d) <= D_APE)

/* an Ogg file whose first packet is OpusHead: the first page's header is 27 bytes, then its segment table */
static int is_ogg_opus(const unsigned char *b, size_t len) {
  size_t at;
  if (len < 27 || memcmp(b, "OggS", 4)) return 0;
  at = 27 + (size_t)b[26];
  return at + 8 <= len && !memcmp(b + at, "OpusHead", 8);
}

/* which decoder the first bytes ask for, or D_NONE. strict: container magics only (a raw file's first bytes can
   look like an MPEG frame sync or "ID3" by chance) */
/* The first bytes of formats only the system decoder or FFmpeg read. Without this the built-in decoders are tried on
   them in turn, and dr_mp3, which skips to the first thing that looks like an MPEG frame, "reads" a few frames of
   noise from an AU or DTS file's audio: a sample a fraction of a second long, and the right decoder never tried. */
static int foreign(const unsigned char *b, size_t len) {
  static const struct { const char *m; unsigned char n, at; } k[] = {
    {".snd", 4, 0},                                        /* AU */
    {"\x7F\xFE\x80\x01", 4, 0}, {"\xFE\x7F\x01\x80", 4, 0},       /* DTS, 16-bit words either byte order */
    {"\x1F\xFF\xE8\x00", 4, 0}, {"\xFF\x1F\x00\xE8", 4, 0},       /* DTS, 14-bit words */
    {"\x64\x58\x20\x25", 4, 0},                             /* DTS-HD substream */
    {"\x0B\x77", 2, 0}, {"\x77\x0B", 2, 0},                  /* AC-3 and E-AC-3 */
    {"\xF8\x72\x6F\xBA", 4, 4}, {"\xF8\x72\x6F\xBB", 4, 4},   /* TrueHD, MLP */
    {"Creative Voice File", 19, 0}, {"TTA1", 4, 0}, {"MPCK", 4, 0}, {"MP+", 3, 0},
    {"\x1A\x45\xDF\xA3", 4, 0},                             /* Matroska and WebM */
    {"DSD ", 4, 0}, {"FRM8", 4, 0},                         /* DSF, DFF */
    {"ajkg", 4, 0},                                         /* Shorten */
  };
  for (unsigned i = 0; i < sizeof k / sizeof *k; i++)
    if (len >= (size_t)k[i].at + k[i].n && !memcmp(b + k[i].at, k[i].m, k[i].n)) return 1;
  return 0;
}

static int sniff(const unsigned char *b, size_t len, int strict) {
  static const unsigned char w64[16] = {'r', 'i', 'f', 'f', 0x2E, 0x91, 0xCF, 0x11, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00};
  static const unsigned char asf[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
  if (len >= 12 && (!memcmp(b, "RIFF", 4) || !memcmp(b, "RIFX", 4) || !memcmp(b, "RF64", 4) || !memcmp(b, "BW64", 4) ||
                    !memcmp(b, "FORM", 4))) return D_WAV;
  if (len >= 16 && !memcmp(b, w64, 16)) return D_WAV;
  if (len >= 8 && !memcmp(b, "caff", 4)) return D_CAF;
  if (len >= 4 && !memcmp(b, "fLaC", 4)) return D_FLAC;
  if (is_ogg_opus(b, len)) return D_OPUS;
  if (len >= 4 && !memcmp(b, "OggS", 4)) return D_VORBIS;
  if (len >= 32 && !memcmp(b, "wvpk", 4)) return D_WAVPACK;
  if (len >= 32 && !memcmp(b, "MAC ", 4)) return D_APE;
  if ((len >= 12 && !memcmp(b + 4, "ftyp", 4)) || (len >= 16 && !memcmp(b, asf, 16)) || (len >= 6 && !memcmp(b, "#!AMR", 5)))
    return D_WINDOWS;                                       /* MPEG-4 and 3GP, ASF (WMA), AMR */
  if (strict) return D_NONE;
  if (foreign(b, len)) return D_WINDOWS;                   /* a format the built-in decoders don't read */
  if (len >= 10 && !memcmp(b, "ID3", 3)) {                 /* an ID3 tag first: FLAC, Monkey's Audio or MP3 after it */
    size_t skip = 10 + ((size_t)(b[6] & 127) << 21 | (size_t)(b[7] & 127) << 14 | (size_t)(b[8] & 127) << 7 | (b[9] & 127));
    if (b[5] & 0x10) skip += 10;
    if (skip + 4 <= len && !memcmp(b + skip, "MAC ", 4)) return D_APE;
    if (skip + 7 <= len && b[skip] == 0xFF && (b[skip + 1] & 0xF6) == 0xF0) return D_WINDOWS;   /* ADTS AAC */
    return skip + 4 <= len && !memcmp(b + skip, "fLaC", 4) ? D_FLAC : D_MP3;
  }
  if (len >= 7 && b[0] == 0xFF && (b[1] & 0xF6) == 0xF0) return D_WINDOWS;   /* ADTS AAC: an MPEG sync with layer 0 */
  if (len >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) return D_MP3;
  return D_NONE;
}

/* one decoder on the bytes: float channel after channel, or NULL */
static float *run(int d, const unsigned char *b, size_t len, unsigned *ch, unsigned *sr, unsigned long long *n, char *err, int cap) {
  float *x = NULL, *out;
  *ch = 0; *sr = 0; *n = 0;
  if (d == D_CAF) {                                  /* the same final check as the others: some audio */
    out = caf_read(b, len, ch, sr, n, err, cap);
    if (out && !(*n && *ch && *sr)) {
      free(out); out = NULL;
      snprintf(err, (size_t)cap, "it's a CAF file without its audio, or it's cut short");
    }
    return out;
  }
  /* interleaved, in a Grow; a library decoder writes its own message when it fails */
  x = d == D_WAV ? wav_read(b, len, ch, sr, n, err, cap) : d == D_FLAC ? flac_read(b, len, ch, sr, n, err, cap)
    : d == D_MP3 ? mp3_read(b, len, ch, sr, n, err, cap) : d == D_VORBIS ? vorbis_read(b, len, ch, sr, n, err, cap)
    : d == D_OPUS ? opus_read(b, len, ch, sr, n, err, cap) : d == D_WAVPACK ? wavpack_read(b, len, ch, sr, n, err, cap)
    : d == D_APE ? ape_read(b, len, ch, sr, n, err, cap) : NULL;
  if (!x) return NULL;
  if (!(*n && *ch && *sr)) { free(x); return NULL; }
  if (*ch == 1) {                                    /* mono is already channel after channel: no second copy, */
    float *y = realloc(x, sizeof(float) * (size_t)*n);   /* only the Grow's spare room handed back */
    return y ? y : x;
  }
  out = planar(x, *n, *ch);
  free(x);
  if (!out) snprintf(err, (size_t)cap, "out of memory");
  return out;
}

/* The chain on bytes in memory: the decoder the first bytes ask for, then (a raw name) raw PCM, then the built-in
   decoders in turn. A decoder's own message (CAF's, or a library's) is kept when nothing reads it. *lib says a
   library decoded it. */
static float *chain(const unsigned char *b, size_t len, const char *raw_name, unsigned *ch, unsigned *sr,
                    unsigned long long *n, int *lib, char *err, int cap) {
  static const int order[] = {D_WAV, D_FLAC, D_VORBIS, D_MP3};
  int first = sniff(b, len, raw_name != NULL), i; float *x;
  char why[300] = "";
  *lib = 0;
  if (first != D_NONE && (x = run(first, b, len, ch, sr, n, why, sizeof why))) { *lib = D_IS_LIBRARY(first); return x; }
  /* it read, but too much; or it was cancelled: either way no other decoder is tried */
  if (!strcmp(why, TOO_LONG) || !strcmp(why, CANCELLED)) { snprintf(err, (size_t)cap, "%s", why); return NULL; }
  if (raw_name) {
    if ((x = raw_read(b, len, ch, sr, n, why, sizeof why))) return x;
    snprintf(err, (size_t)cap, "%s", why);
    return NULL;
  }
  if (first == D_CAF || D_IS_LIBRARY(first)) { snprintf(err, (size_t)cap, "%s", why); return NULL; }
  if (first == D_WINDOWS) { snprintf(err, (size_t)cap, NOT_SOUND); return NULL; }   /* only Windows reads it */
  for (i = 0; i < 4; i++) {
    if (order[i] == first) continue;
    why[0] = 0;
    if ((x = run(order[i], b, len, ch, sr, n, why, sizeof why))) return x;
    if (!strcmp(why, TOO_LONG) || !strcmp(why, CANCELLED)) { snprintf(err, (size_t)cap, "%s", why); return NULL; }
  }
  snprintf(err, (size_t)cap, NOT_SOUND);
  return NULL;
}

/* The one place every decoder's output passes: too long is refused, and a sample that isn't finite or is absurdly
   large (|x| > 1e6, from a damaged file) becomes 0, so it never reaches the cache or the engine. */
static float *finish(float *x, unsigned ch, unsigned sr, unsigned long long n, int *chans, long *frames, int *rate,
                     char *err, int cap) {
  size_t i, k = (size_t)n * ch;
  if (n * ch > DEC_MAX_FLOATS) { free(x); snprintf(err, (size_t)cap, TOO_LONG); return NULL; }   /* CAF and raw */
  for (i = 0; i < k; i++)
    if (!(fabsf(x[i]) <= 1e6f)) x[i] = 0.0f;                  /* NaN fails the test too */
  *chans = (int)ch; *frames = (long)n; *rate = (int)sr;
  return x;
}

float *decode_memory_ex(const void *bytes, size_t len, int *chans, long *frames, int *rate,
                        int (*cancel)(void *), void *arg, char *err, int cap) {
  unsigned ch = 0, sr = 0; unsigned long long n = 0; int lib = 0;
  float *x;
  if (cancel && cancel(arg)) { snprintf(err, (size_t)cap, CANCELLED); return NULL; }
  t_cancel = cancel; t_cancel_arg = arg;
  x = bytes ? chain((const unsigned char *)bytes, len, NULL, &ch, &sr, &n, &lib, err, cap) : NULL;
  t_cancel = NULL; t_cancel_arg = NULL;
  if (!bytes) snprintf(err, (size_t)cap, "no audio");
  if (!x) return NULL;
  return finish(x, ch, sr, n, chans, frames, rate, err, cap);
}
float *decode_memory(const void *bytes, size_t len, int *chans, long *frames, int *rate, char *err, int cap) {
  return decode_memory_ex(bytes, len, chans, frames, rate, NULL, NULL, err, cap);
}
float *smp_decode_memory(const void *bytes, size_t len, int *chans, long *frames, int *rate, char *err, int cap) {
  return decode_memory(bytes, len, chans, frames, rate, err, cap);
}

/* the extensions a file Windows decodes usually has: when Windows has no Media Foundation, these say so */
static int windows_name(const char *path) {
  static const char *const ext[] = {".m4a", ".mp4", ".aac", ".wma", ".3gp", ".amr", ".ac3", ".alac"};
  const char *e = ext_of(path);
  for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++)
    if (!_stricmp(e, ext[i])) return 1;
  return 0;
}

/* the extensions of the formats our own decoders read: a damaged file named so isn't a format that needs FFmpeg */
static int own_name(const char *path) {
  static const char *const ext[] = {".wav", ".flac", ".mp3", ".ogg", ".aif", ".aiff", ".aifc", ".w64", ".rf64",
                                    ".caf", ".opus", ".wv", ".ape"};
  const char *e = ext_of(path);
  for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++)
    if (!_stricmp(e, ext[i])) return 1;
  return 0;
}

/* The Windows stage on a file the chain refused. Its result, channel after channel, or NULL; err keeps the chain's
   message unless that was only "not a sound file" and Windows knows better (no decoder for it, damaged), or Windows
   stopped (too long, too slow, cancelled). */
static float *windows_stage(const char *path, int edit, double start, double dur, unsigned *ch, unsigned *sr,
                            unsigned long long *n, int (*cancel)(void *), void *arg, int *res, char *err, int cap) {
  char why[300] = ""; float *g = NULL, *x = NULL; size_t floats = 0; int trimmed = 0;
  int r = *res = smp_mf_decode(path, DEC_MAX_FLOATS, &g, &floats, ch, sr, &trimmed, cancel, arg, why, sizeof why);
  if (r == SMP_MF_OK) {
    *n = floats / *ch;
    if (edit && !trimmed && start >= 0 && start < 1e6 && dur >= 0 && dur < 1e7) {
      /* an MPEG-4 edit list (absurd values ignored): the coder's start delay, and the length */
      unsigned long long cut = (unsigned long long)(start * *sr + 0.5), keep = (unsigned long long)(dur * *sr + 0.5);
      if (cut < *n) {
        memmove(g, g + (size_t)cut * *ch, (size_t)(*n - cut) * *ch * sizeof(float));
        *n -= cut;
        if (keep && keep < *n) *n = keep;
      }
    }
    if (!(x = planar(g, *n, *ch))) snprintf(err, (size_t)cap, "out of memory");
  } else if (r == SMP_MF_TOO_LONG) {
    snprintf(err, (size_t)cap, TOO_LONG);
  } else if (r == SMP_MF_STOPPED) {
    snprintf(err, (size_t)cap, "%s", why);
  } else if (r == SMP_MF_NO_MF && !strcmp(err, NOT_SOUND) && windows_name(path)) {
    snprintf(err, (size_t)cap, "Windows Media Foundation isn't installed");
  } else if (r == SMP_MF_BUSY && !strcmp(err, NOT_SOUND)) {
    snprintf(err, (size_t)cap, "Windows decoding is busy or stuck");
  } else if (r == SMP_MF_FAILED && !strcmp(err, NOT_SOUND)) {
    snprintf(err, (size_t)cap, "%s", why);
  }
  free(g);
  return x;
}

/* The FFmpeg stage on a file nothing else read. Its result, channel after channel, or NULL. err becomes FFmpeg's
   reason only when the earlier message was a general one (not specific: our own decoder didn't know the format), and
   "FFmpeg isn't installed" only when it was the plain "not a sound file" and the name isn't one our decoders or
   Windows read (a damaged .wav doesn't need FFmpeg); it stays as it was when FFmpeg can't open the file either. Stopping (cancelled, too slow, too long) always says so. */
static float *ffmpeg_stage(const char *path, unsigned *ch, unsigned *sr, unsigned long long *n, int (*cancel)(void *),
                           void (*stage)(void *), void *arg, int specific, char *err, int cap) {
  char why[300] = ""; float *g = NULL, *x = NULL; size_t floats = 0;
  int r = smp_ff_decode(path, DEC_MAX_FLOATS, &g, &floats, ch, sr, cancel, stage, arg, why, sizeof why);
  if (r == SMP_FF_OK) {
    *n = floats / *ch;
    if (!(x = planar(g, *n, *ch))) snprintf(err, (size_t)cap, "out of memory");
  } else if (r == SMP_FF_TOO_LONG) {
    snprintf(err, (size_t)cap, TOO_LONG);
  } else if (r == SMP_FF_STOPPED) {
    snprintf(err, (size_t)cap, "%s", why);
  } else if (r == SMP_FF_MISSING && !strcmp(err, NOT_SOUND) && !own_name(path) && !windows_name(path)) {
    snprintf(err, (size_t)cap, "FFmpeg isn't installed; this format needs it");
  } else if (r == SMP_FF_FAILED && !specific) {
    snprintf(err, (size_t)cap, "%s", why);
  }
  free(g);
  return x;
}

float *decode_audio_full(const char *path, int *chans, long *frames, int *rate, SmpDecBy *by,
                         int (*cancel)(void *), void (*stage)(void *), void *arg, char *err, int cap) {
  size_t len = 0; unsigned char *b; float *x;
  unsigned ch = 0, sr = 0; unsigned long long n = 0; int lib = 0, raw = smp_is_raw_name(path);
  if (by) *by = SMP_DEC_BUILTIN;
  if (cancel && cancel(arg)) { snprintf(err, (size_t)cap, "cancelled"); return NULL; }
  b = slurp(path, &len);
  if (!b) { snprintf(err, (size_t)cap, "can't open %s", path); return NULL; }
  t_cancel = cancel; t_cancel_arg = arg;                    /* the built-in decoders stop on cancel too */
  x = chain(b, len, raw ? path : NULL, &ch, &sr, &n, &lib, err, cap);
  t_cancel = NULL; t_cancel_arg = NULL;
  if (!x && !strcmp(err, CANCELLED)) { free(b); return NULL; }
  if (!x && !raw && len && strcmp(err, TOO_LONG)) {         /* Windows, then FFmpeg, after everything of our own */
    double start = 0, dur = 0;
    int edit = smp_mp4_edit(b, len, &start, &dur), specific = strcmp(err, NOT_SOUND) != 0, wr = 0;
    free(b);
    if (cancel && cancel(arg)) { snprintf(err, (size_t)cap, "cancelled"); return NULL; }
    if ((x = windows_stage(path, edit, start, dur, &ch, &sr, &n, cancel, arg, &wr, err, cap)) != NULL) {
      if (by) *by = SMP_DEC_WINDOWS;
    } else {
      if (wr == SMP_MF_TOO_LONG) return NULL;
      if (cancel && cancel(arg)) { snprintf(err, (size_t)cap, "cancelled"); return NULL; }
      if (!(x = ffmpeg_stage(path, &ch, &sr, &n, cancel, stage, arg, specific, err, cap))) return NULL;
      if (by) *by = SMP_DEC_FFMPEG;
    }
    if (cap > 0) err[0] = 0;                                /* the chain's refusal no longer applies */
  } else free(b);
  if (!x) return NULL;
  if (by && lib) *by = SMP_DEC_LIBRARY;
  return finish(x, ch, sr, n, chans, frames, rate, err, cap);
}

float *decode_audio_ex(const char *path, int *chans, long *frames, int *rate, SmpDecBy *by,
                       int (*cancel)(void *), void *arg, char *err, int cap) {
  return decode_audio_full(path, chans, frames, rate, by, cancel, NULL, arg, err, cap);
}

float *decode_audio(const char *path, int *chans, long *frames, int *rate, char *err, int cap) {
  return decode_audio_ex(path, chans, frames, rate, NULL, NULL, NULL, err, cap);
}
