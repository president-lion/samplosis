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
      in nearly anything).
   Later stages add decoders after these. Whatever decodes a file, its result passes one place (finish): a sample
   that isn't finite, or is absurdly large (a damaged file), becomes silence. Never called on the audio thread. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#endif
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

/* codecs/mac_shim.cpp: interleaved float, or NULL and a message */
float *smp_ape_decode(const unsigned char *b, size_t len, unsigned *ch, unsigned *sr, unsigned long long *n, char *err, int cap);
void smp_ape_set_debug_flags(long f);
/* tests: 1 = the Monkey's Audio decoder throws inside its guard */
SMP_API void smp_debug_decode_flags(long f) { smp_ape_set_debug_flags(f & 1); }
SMP_API void smp_free(void *p) { free(p); }

/* the sampler's extensions: what Browse and the File combo box list */
static const wchar_t *const audio_ext[] = {
  L".wav", L".flac", L".mp3", L".ogg", L".aif", L".aiff", L".aifc", L".w64", L".rf64", L".caf", L".raw", L".pcm",
  L".opus", L".wv", L".ape", L".m4a", L".aac", L".mp4", L".wma", L".ac3", L".amr", L".3gp", L".alac"};
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
  fp = fopen(path, "rb");
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
  {
    float *x = pcm_planar(b, *n, (int)*ch, bytes, s.raw_format == SMP_RAW_F32, s.raw_format == SMP_RAW_U8,
                          s.raw_endian == SMP_RAW_BIG);
    if (!x) snprintf(err, (size_t)cap, "out of memory");
    return x;
  }
}

/* A growing interleaved buffer, for decoders that can't say their length for sure (a damaged file may lie). */
typedef struct { float *x; size_t n, cap; } Grow;            /* n and cap count floats */
static int grow_need(Grow *g, size_t more) {
  if (g->n + more > g->cap) {
    size_t c = g->cap ? g->cap : 65536;
    float *y;
    while (c < g->n + more) c *= 2;
    if (!(y = realloc(g->x, c * sizeof(float)))) return 0;
    g->x = y; g->cap = c;
  }
  return 1;
}

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
  if (total > 0 && total < 0x7FFFFFFF && (unsigned long long)total * c < (unsigned long long)len * 2048 + 4800000)
    grow_need(&g, (size_t)total * c + OPUS_CHUNK);           /* a hint: the right size for a good file */
  for (;;) {
    int got;
    if (!grow_need(&g, OPUS_CHUNK)) { free(g.x); op_free(of); snprintf(err, (size_t)cap, "out of memory"); return NULL; }
    got = same ? op_read_float(of, g.x + g.n, OPUS_CHUNK, NULL) : op_read_float_stereo(of, g.x + g.n, OPUS_CHUNK);
    if (got == OP_HOLE) continue;                             /* a gap in a damaged file: read on past it */
    if (got <= 0) break;
    g.n += (size_t)got * c;
    if (g.n / c > 0x7FFFFFFF) break;
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
  if (total > 0 && total < 0x7FFFFFFF && (unsigned long long)total * c < (unsigned long long)len * 1024 + 4800000)
    grow_need(&g, (size_t)total * c);                        /* a hint: the right size for a good file */
  if (!(buf = malloc(sizeof(int32_t) * 4096 * (size_t)c))) {
    WavpackCloseFile(w); free(g.x); snprintf(err, (size_t)cap, "out of memory"); return NULL;
  }
  while ((got = WavpackUnpackSamples(w, buf, 4096)) > 0) {
    if (!grow_need(&g, (size_t)got * c)) {
      free(buf); free(g.x); WavpackCloseFile(w); snprintf(err, (size_t)cap, "out of memory"); return NULL;
    }
    for (i = 0; i < got * (uint32_t)c; i++) {
      float v;
      if (is_float) memcpy(&v, &buf[i], 4);
      else v = (float)((double)buf[i] * scale);
      g.x[g.n + i] = v;
    }
    g.n += (size_t)got * c;
    if (g.n / c > 0x7FFFFFFF) break;
  }
  free(buf);
  WavpackCloseFile(w);
  if (!g.n) { free(g.x); snprintf(err, (size_t)cap, "it's a WavPack file without its audio, or it's damaged"); return NULL; }
  *ch = (unsigned)c; *sr = rate; *n = g.n / c;
  return g.x;
}

/* the built-in decoders, then the library ones */
enum { D_NONE, D_WAV, D_FLAC, D_VORBIS, D_MP3, D_CAF, D_OPUS, D_WAVPACK, D_APE };
#define D_IS_LIBRARY(d) ((d) >= D_OPUS)

/* an Ogg file whose first packet is OpusHead: the first page's header is 27 bytes, then its segment table */
static int is_ogg_opus(const unsigned char *b, size_t len) {
  size_t at;
  if (len < 27 || memcmp(b, "OggS", 4)) return 0;
  at = 27 + (size_t)b[26];
  return at + 8 <= len && !memcmp(b + at, "OpusHead", 8);
}

/* which decoder the first bytes ask for, or D_NONE. strict: container magics only (a raw file's first bytes can
   look like an MPEG frame sync or "ID3" by chance) */
static int sniff(const unsigned char *b, size_t len, int strict) {
  static const unsigned char w64[16] = {'r', 'i', 'f', 'f', 0x2E, 0x91, 0xCF, 0x11, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00};
  if (len >= 12 && (!memcmp(b, "RIFF", 4) || !memcmp(b, "RIFX", 4) || !memcmp(b, "RF64", 4) || !memcmp(b, "BW64", 4) ||
                    !memcmp(b, "FORM", 4))) return D_WAV;
  if (len >= 16 && !memcmp(b, w64, 16)) return D_WAV;
  if (len >= 8 && !memcmp(b, "caff", 4)) return D_CAF;
  if (len >= 4 && !memcmp(b, "fLaC", 4)) return D_FLAC;
  if (is_ogg_opus(b, len)) return D_OPUS;
  if (len >= 4 && !memcmp(b, "OggS", 4)) return D_VORBIS;
  if (len >= 32 && !memcmp(b, "wvpk", 4)) return D_WAVPACK;
  if (len >= 32 && !memcmp(b, "MAC ", 4)) return D_APE;
  if (strict) return D_NONE;
  if (len >= 10 && !memcmp(b, "ID3", 3)) {                 /* an ID3 tag first: FLAC, Monkey's Audio or MP3 after it */
    size_t skip = 10 + ((size_t)(b[6] & 127) << 21 | (size_t)(b[7] & 127) << 14 | (size_t)(b[8] & 127) << 7 | (b[9] & 127));
    if (b[5] & 0x10) skip += 10;
    if (skip + 4 <= len && !memcmp(b + skip, "MAC ", 4)) return D_APE;
    return skip + 4 <= len && !memcmp(b + skip, "fLaC", 4) ? D_FLAC : D_MP3;
  }
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
  if (D_IS_LIBRARY(d)) {                             /* interleaved from the library, with its own message */
    x = d == D_OPUS ? opus_read(b, len, ch, sr, n, err, cap)
      : d == D_WAVPACK ? wavpack_read(b, len, ch, sr, n, err, cap)
      : smp_ape_decode(b, len, ch, sr, n, err, cap);
    if (!x) return NULL;
    out = *n && *ch && *sr ? planar(x, *n, *ch) : NULL;
    free(x);
    if (!out) snprintf(err, (size_t)cap, "out of memory");
    return out;
  }
  if (d == D_WAV) x = drwav_open_memory_and_read_pcm_frames_f32(b, len, ch, sr, n, NULL);
  else if (d == D_FLAC) x = drflac_open_memory_and_read_pcm_frames_f32(b, len, ch, sr, n, NULL);
  else if (d == D_MP3) {
    drmp3_config c;
    if ((x = drmp3_open_memory_and_read_pcm_frames_f32(b, len, &c, n, NULL))) { *ch = c.channels; *sr = c.sampleRate; }
  } else if (d == D_VORBIS && len < 0x7FFFFFFF) {
    int e = 0;
    stb_vorbis *v = stb_vorbis_open_memory(b, (int)len, &e, NULL);
    if (v) {
      stb_vorbis_info info = stb_vorbis_get_info(v);
      unsigned int total = stb_vorbis_stream_length_in_samples(v);
      *ch = (unsigned)info.channels; *sr = info.sample_rate;
      x = malloc(sizeof(float) * ((size_t)total + 1) * *ch);
      if (x) *n = (unsigned long long)stb_vorbis_get_samples_float_interleaved(v, info.channels, x, (int)(total * *ch));
      stb_vorbis_close(v);
    }
  }
  if (!x) return NULL;
  out = *n && *ch && *sr ? planar(x, *n, *ch) : NULL;
  if (d == D_WAV) drwav_free(x, NULL); else if (d == D_FLAC) drflac_free(x, NULL); else if (d == D_MP3) drmp3_free(x, NULL); else free(x);
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
  if (raw_name) {
    if ((x = raw_read(b, len, ch, sr, n, why, sizeof why))) return x;
    snprintf(err, (size_t)cap, "%s", why);
    return NULL;
  }
  if (first == D_CAF || D_IS_LIBRARY(first)) { snprintf(err, (size_t)cap, "%s", why); return NULL; }
  for (i = 0; i < 4; i++)
    if (order[i] != first && (x = run(order[i], b, len, ch, sr, n, why, sizeof why))) return x;
  snprintf(err, (size_t)cap, "it isn't a sound file Samplosis can read");
  return NULL;
}

/* The one place every decoder's output passes: too long is refused, and a sample that isn't finite or is absurdly
   large (|x| > 1e6, from a damaged file) becomes 0, so it never reaches the cache or the engine. */
static float *finish(float *x, unsigned ch, unsigned sr, unsigned long long n, int *chans, long *frames, int *rate,
                     char *err, int cap) {
  size_t i, k = (size_t)n * ch;
  if (n > 0x7FFFFFFF) { free(x); snprintf(err, (size_t)cap, "it's too long to load"); return NULL; }
  for (i = 0; i < k; i++)
    if (!(fabsf(x[i]) <= 1e6f)) x[i] = 0.0f;                  /* NaN fails the test too */
  *chans = (int)ch; *frames = (long)n; *rate = (int)sr;
  return x;
}

float *decode_memory(const void *bytes, size_t len, int *chans, long *frames, int *rate, char *err, int cap) {
  unsigned ch = 0, sr = 0; unsigned long long n = 0; int lib = 0;
  float *x = bytes ? chain((const unsigned char *)bytes, len, NULL, &ch, &sr, &n, &lib, err, cap) : NULL;
  if (!bytes) snprintf(err, (size_t)cap, "no audio");
  if (!x) return NULL;
  return finish(x, ch, sr, n, chans, frames, rate, err, cap);
}

float *decode_audio_ex(const char *path, int *chans, long *frames, int *rate, SmpDecBy *by,
                       int (*cancel)(void *), void *arg, char *err, int cap) {
  size_t len = 0; unsigned char *b; float *x;
  unsigned ch = 0, sr = 0; unsigned long long n = 0; int lib = 0;
  if (by) *by = SMP_DEC_BUILTIN;
  if (cancel && cancel(arg)) { snprintf(err, (size_t)cap, "cancelled"); return NULL; }
  b = slurp(path, &len);
  if (!b) { snprintf(err, (size_t)cap, "can't open %s", path); return NULL; }
  x = chain(b, len, smp_is_raw_name(path) ? path : NULL, &ch, &sr, &n, &lib, err, cap);
  free(b);
  if (!x) return NULL;
  if (by && lib) *by = SMP_DEC_LIBRARY;
  return finish(x, ch, sr, n, chans, frames, rate, err, cap);
}

float *decode_audio(const char *path, int *chans, long *frames, int *rate, char *err, int cap) {
  return decode_audio_ex(path, chans, frames, rate, NULL, NULL, NULL, err, cap);
}
