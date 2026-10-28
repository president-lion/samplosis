// decode.c: an impulse file for convolve, read whole, in any of the formats impulse responses come in: WAV in any
// encoding (PCM 8 to 32 bits, float 32 or 64, A-law, mu-law, ADPCM; RF64 and W64 too) and AIFF through dr_wav, FLAC
// through dr_flac, MP3 through dr_mp3, Ogg Vorbis through stb_vorbis. All four are public domain (or MIT-0), single
// files in decode/ (dr_libs 8159355, stb 003501f).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include "decode.h"

// The file's bytes. The path is UTF-8, so names in any language open on Windows.
static unsigned char *slurp(const char *path, size_t *len)
{
   FILE *fp = NULL;
#ifdef _WIN32
   wchar_t w[2048];
   if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048)) fp = _wfopen(w, L"rb");
#else
   fp = fopen(path, "rb");
#endif
   if (!fp) return NULL;
   fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
   unsigned char *b = n > 0 ? malloc((size_t)n) : NULL;
   if (b && fread(b, 1, (size_t)n, fp) != (size_t)n) { free(b); b = NULL; }
   fclose(fp);
   *len = (size_t)n;
   return b;
}

float *decode_audio(const char *path, int *chans, long *frames, int *rate, char *err, int cap)
{
   size_t len = 0;
   unsigned char *b = slurp(path, &len);
   if (!b) { snprintf(err, (size_t)cap, "convolve: can't open %s", path); return NULL; }
   float *x = NULL; unsigned int ch = 0, sr = 0; unsigned long long n = 0; int lib = 0;
   if ((x = drwav_open_memory_and_read_pcm_frames_f32(b, len, &ch, &sr, &n, NULL))) lib = 1;
   else if ((x = drflac_open_memory_and_read_pcm_frames_f32(b, len, &ch, &sr, &n, NULL))) lib = 2;
   else {
      int e = 0;
      stb_vorbis *v = stb_vorbis_open_memory(b, (int)len, &e, NULL);
      if (v) {
         stb_vorbis_info info = stb_vorbis_get_info(v);
         unsigned int total = stb_vorbis_stream_length_in_samples(v);
         ch = (unsigned)info.channels; sr = info.sample_rate;
         x = malloc(sizeof(float) * ((size_t)total + 1) * ch);
         if (x) n = (unsigned long long)stb_vorbis_get_samples_float_interleaved(v, info.channels, x, (int)(total * ch));
         stb_vorbis_close(v); lib = 3;
      } else {                                         // MP3 last: it finds frames in nearly anything
         drmp3_config c;
         if ((x = drmp3_open_memory_and_read_pcm_frames_f32(b, len, &c, &n, NULL))) { ch = c.channels; sr = c.sampleRate; lib = 4; }
      }
   }
   free(b);
   if (!x || !n || !ch || !sr) {
      if (x) { if (lib == 1) drwav_free(x, NULL); else if (lib == 2) drflac_free(x, NULL); else if (lib == 4) drmp3_free(x, NULL); else free(x); }
      snprintf(err, (size_t)cap, "convolve: %s isn't a sound file this can read (WAV, AIFF, FLAC, MP3 or Ogg)", path);
      return NULL;
   }
   float *out = malloc(sizeof(float) * (size_t)n * ch);    // channel after channel
   if (out)
      for (unsigned long long i = 0; i < n; i++)
         for (unsigned c = 0; c < ch; c++) out[(size_t)c * n + i] = x[(size_t)i * ch + c];
   if (lib == 1) drwav_free(x, NULL); else if (lib == 2) drflac_free(x, NULL); else if (lib == 4) drmp3_free(x, NULL); else free(x);
   if (!out) { snprintf(err, (size_t)cap, "convolve: out of memory"); return NULL; }
   *chans = (int)ch; *frames = (long)n; *rate = (int)sr;
   return out;
}
