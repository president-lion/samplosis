#pragma once
/* The decode layer. Called from the main thread or worker threads, never from the audio thread. Every call returns a
   sound file's samples as float, channel after channel (malloc'd), or NULL and a plain message in err. Paths are
   UTF-8. Built in: WAV in any encoding with RIFF, RIFX, RF64 and Wave64 (W64) containers, AIFF and AIFF-C, FLAC,
   MP3, Ogg Vorbis, CAF (PCM and float), and raw PCM (.raw, .pcm) read as the Settings say. From the vendored
   libraries in codecs/: Opus, WavPack and Monkey's Audio. A sample that isn't finite, or is absurdly large, comes
   back as 0. */
#include <stddef.h>
#include <wchar.h>
#include "samplosis.h"

typedef enum { SMP_DEC_BUILTIN, SMP_DEC_LIBRARY, SMP_DEC_WINDOWS, SMP_DEC_FFMPEG } SmpDecBy;

/* The decode chain: the built-in decoders by the file's first bytes, then CAF, then raw PCM by the extension. by
   says what decoded it (may be NULL); cancel, when it isn't NULL, is asked now and then and a nonzero answer stops
   the decode. */
float *decode_audio_ex(const char *path, int *chans, long *frames, int *rate, SmpDecBy *by,
                       int (*cancel)(void *), void *arg, char *err, int cap);
/* decode_audio_ex with no by and no cancel */
float *decode_audio(const char *path, int *chans, long *frames, int *rate, char *err, int cap);
/* Bytes in memory: the built-in and library decoders only, for compressed SoundFont streams (FLAC, Ogg Vorbis, Opus
   and WavPack). Free the result with free(), or smp_free from outside the plugin. */
SMP_API float *decode_memory(const void *bytes, size_t len, int *chans, long *frames, int *rate, char *err, int cap);

/* the extensions the sampler lists in Browse and the File combo box (".wav", ...); returns how many. Some need a
   decoder that a later stage adds, or Windows, or FFmpeg. */
SMP_API int smp_audio_extensions(const wchar_t ***list);
/* 1 when the path ends .raw or .pcm (read with the Settings' raw format) */
int smp_is_raw_name(const char *path);

/* FFmpeg (fffind.c): the Settings path when that file is there, else ffmpeg.exe on PATH, else the WinGet package
   folder. 1 and the full path when found, else 0. */
int smp_ffmpeg_find_w(wchar_t *out, int cap);
SMP_API int smp_ffmpeg_find(char *out, int cap);     /* the same, UTF-8 */

/* free() for memory the decode layer gave out, for callers outside the plugin (the tests) */
SMP_API void smp_free(void *p);
/* tests: 1 = the Monkey's Audio decoder throws inside its guard (the shim must catch it) */
SMP_API void smp_debug_decode_flags(long f);
