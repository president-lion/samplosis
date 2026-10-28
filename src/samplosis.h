#ifndef SAMPLOSIS_H
#define SAMPLOSIS_H
#include <stdint.h>
#ifdef _WIN32
#define SMP_API __declspec(dllexport)
#else
#define SMP_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
SMP_API const char *smp_version(void);

/* parameter ids (also the REAPER/CLAP parameter ids) */
enum {
  SMP_P_ROOT = 0, SMP_P_TRANSPOSE = 1, SMP_P_FINE = 2, SMP_P_VOLUME = 3, SMP_P_PAN = 4,
  SMP_P_ATTACK = 5, SMP_P_DECAY = 6, SMP_P_SUSTAIN = 7, SMP_P_RELEASE = 8,
  SMP_P_VELOCITY_SENS = 9, SMP_P_MIN_VOLUME = 10, SMP_P_MODE = 11,
  SMP_P_NOTE_LO = 12, SMP_P_NOTE_HI = 13, SMP_P_PITCH_LO = 14, SMP_P_VEL_LO = 15, SMP_P_VEL_HI = 16,
  SMP_P_OBEY_NOTE_OFF = 17, SMP_P_LOOP = 18, SMP_P_LOOP_START = 19, SMP_P_START = 20, SMP_P_END = 21,
  SMP_P_BEND_RANGE = 22, SMP_P_PITCH_HI = 23, SMP_P_REVERSE = 24, SMP_P_KEEP_LENGTH = 25,
  SMP_P_VOICES = 26, SMP_P_PROBABILITY = 27, SMP_P_SPEED = 28
};
#define SMP_NPARAMS 29

/* how a parameter reads in words */
enum { SMP_K_NOTE, SMP_K_SEMI, SMP_K_CENT, SMP_K_DB, SMP_K_PAN, SMP_K_MS, SMP_K_PERCENT,
       SMP_K_CHOICE, SMP_K_BOOL, SMP_K_COUNT, SMP_K_POSITION };

typedef struct { const char *name; double lo, hi, def; int kind; } SmpParamInfo;

SMP_API const SmpParamInfo *smp_param_info(int id); /* NULL when id is out of range */

SMP_API int smp_note_parse(const char *s);                  /* C-1 = 0, C4 = 60; -1 if not a note */
SMP_API void smp_note_name(int n, char *out, int cap);      /* sharps: 61 = "C#4" */
SMP_API int smp_note_from_filename(const char *path);       /* last note-like token of the base name, or -1 */
SMP_API double smp_detect_pitch(const float *mono, long n, int rate); /* fractional MIDI note, or -1 */
SMP_API int smp_value_text(int param, double v, char *out, int cap);  /* returns length written */
SMP_API int smp_value_parse(int param, const char *s, double *v);    /* 1 ok (clamped to range), 0 fail */
/* settings: one "name value" per line in %APPDATA%\Samplosis\settings.txt (or $SAMPLOSIS_SETTINGS) */
typedef struct { char cache_dir[1024]; double cache_limit_gb; } SmpSettings;
SMP_API void smp_settings_get(SmpSettings *s);        /* defaults when the file is missing; reloads when it changes */
SMP_API int smp_settings_set(const SmpSettings *s);   /* 0 when saved */

/* disk cache of decoded audio: <16 hex>.smpc, 32 byte header then float32 stereo interleaved */
typedef struct SmpEntry SmpEntry;
SMP_API uint64_t smp_key(const char *desc);           /* 64-bit FNV-1a */
SMP_API int smp_cache_write(uint64_t key, const float *lr, uint64_t frames, int rate); /* 0 ok; .part then rename */
SMP_API SmpEntry *smp_cache_open(uint64_t key);       /* NULL if missing; touches last-write time */
SMP_API void smp_cache_close(SmpEntry *e);
SMP_API uint64_t smp_entry_frames(const SmpEntry *e);
SMP_API int smp_entry_rate(const SmpEntry *e);
SMP_API int smp_entry_whole(const SmpEntry *e);       /* 1 when the head holds the whole file */
SMP_API const float *smp_entry_head(const SmpEntry *e, uint64_t *head_frames); /* immutable after open */
SMP_API long smp_entry_read(SmpEntry *e, uint64_t frame, float *lr, long frames); /* streamer thread only */
SMP_API uint64_t smp_cache_trim(uint64_t limit_bytes); /* deletes oldest closed files; returns bytes left */
SMP_API int smp_cache_ok(char *why, int cap);         /* 1 if the cache folder works, else 0 and a reason */

/* a sample imported into the cache */
typedef struct SmpSample {
  char path[1024]; char name[256];      /* name: the file's base name */
  uint64_t src_key, rev_key;            /* rev_key is made on first use */
  int rate; uint64_t frames;
  int root;                             /* from the file name, else detected, else 60 */
  int root_from;                        /* 0 name, 1 detected, 2 default */
  SmpEntry *fwd, *rev;                  /* rev NULL until needed */
} SmpSample;
SMP_API SmpSample *smp_sample_load(const char *path, char *err, int cap);   /* NULL and a plain message on failure */
SMP_API SmpSample *smp_sample_load_known(const char *path, uint64_t src_key, int root, char *err, int cap); /* source may be gone */
SMP_API SmpEntry *smp_sample_reversed(SmpSample *s);                        /* makes or opens the reversed copy */
SMP_API void smp_sample_free(SmpSample *s);
SMP_API long smp_debug_decodes(void);                                       /* decode_audio calls so far */

/* the voice engine. Threads: main (new/free/set_sample/collect), audio (process; note_on/off/bend are queued by
   the same thread right before process, as a CLAP host does), the engine's own streamer thread, and set_param
   from any thread. The engine owns every sample handed to set_sample. */
typedef struct SmpEngine SmpEngine;
SMP_API SmpEngine *smp_engine_new(double rate, int max_block);   /* starts the streamer thread; preallocates 64 voices */
SMP_API void smp_engine_free(SmpEngine *e);
SMP_API void smp_engine_set_sample(SmpEngine *e, SmpSample *s);  /* main thread; atomic swap, sets root to the sample's;
                                                                    old one freed once nothing can reach it */
SMP_API const SmpSample *smp_engine_sample(const SmpEngine *e);  /* main thread only: the current sample or NULL; valid until the next set_sample/collect */
SMP_API void smp_engine_collect(SmpEngine *e);                   /* main thread: frees replaced samples that are safe to free */
SMP_API void smp_engine_set_param(SmpEngine *e, int id, double v);   /* any thread; atomic; clamped to the range */
SMP_API double smp_engine_get_param(const SmpEngine *e, int id);
SMP_API void smp_engine_note_on(SmpEngine *e, int key, int vel, int at);  /* at: frame offset in the next block */
SMP_API void smp_engine_note_off(SmpEngine *e, int key, int at);
SMP_API void smp_engine_bend(SmpEngine *e, double semis_fraction /* -1..1 */, int at);
SMP_API void smp_engine_process(SmpEngine *e, float *l, float *r, int n);  /* audio thread */
SMP_API int smp_engine_active_voices(const SmpEngine *e);       /* sounding voices, not counting stolen ones fading out */
SMP_API long smp_engine_underruns(const SmpEngine *e);
SMP_API void smp_engine_seed(SmpEngine *e, unsigned seed);
/* keep length: Rubber Band R3 copies, one per integer shift (and speed), made in the background */
SMP_API int smp_render_shift(SmpSample *s, int reversed, int semis, double speed); /* renders one copy now, on this
                                    thread, into the cache (key kl|<src or rev key>|<semis>|<speed %.2f>|r3); 0 ok */
SMP_API long smp_debug_renders(void);                            /* Rubber Band runs so far */
SMP_API void smp_debug_render_flags(long f);   /* tests: 1 = Rubber Band throws (caught), 2 = no 4-octave limit */
SMP_API int smp_engine_render_status(SmpEngine *e, int *ready, int *total, char *why, int cap); /* 1 when every copy
                                    the key range needs is ready; why is a plain reason when renders can't run */
SMP_API int smp_engine_wait_renders(SmpEngine *e, int timeout_ms); /* tests: 0 once the render thread is idle, 1 timeout */
/* test helper: plays notes offline in blocks and writes a stereo float32 wav; events one per line:
   "on key vel frame", "off key frame", "bend fraction frame". Starts from silence (no voices, no bend) and waits for the streamer after each block and
   at note-ons that start past the head, so offline renders are exact. 0 ok. */
SMP_API int smp_engine_render_test(SmpEngine *e, const char *events, long frames, int block, const char *wav_out);
/* the instrument as text (script.c): "# Samplosis 0.1.0", an optional `sample "path" key=<16 hex>` line, then
   `name value-in-words` for every parameter in id order. Paths are quoted with no escapes (backslashes are literal).
   Main thread only. */
SMP_API int smp_script_write(const SmpEngine *e, char *out, int cap);   /* returns the length needed, like snprintf */
SMP_API int smp_script_apply(SmpEngine *e, const char *text, char *err, int cap); /* 0 ok; on error nothing changes and
                                    err says "line N: ..." */
#ifdef __cplusplus
}
#endif
#endif
