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
  SMP_P_VOICES = 26, SMP_P_PROBABILITY = 27, SMP_P_SPEED = 28, SMP_P_GLIDE = 29, SMP_P_GLIDE_MODE = 30
};
#define SMP_NPARAMS 31

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
/* stretcher: SMP_STRETCH_SIGNALSMITH (the default) or SMP_STRETCH_RUBBERBAND; keep_tone: 1 keeps the instrument's
   formants where they were when keep length shifts it (off by default) */
enum { SMP_STRETCH_SIGNALSMITH = 0, SMP_STRETCH_RUBBERBAND = 1 };
typedef struct { char cache_dir[1024]; double cache_limit_gb; int stretcher, keep_tone; } SmpSettings;
SMP_API void smp_settings_get(SmpSettings *s);        /* defaults when the file is missing; reloads when it changes */
SMP_API int smp_settings_set(const SmpSettings *s);   /* 0 when saved */

/* disk cache of decoded audio: <16 hex>.smpc, 32 byte header then float32 stereo interleaved */
typedef struct SmpEntry SmpEntry;
SMP_API uint64_t smp_key(const char *desc);           /* 64-bit FNV-1a */
SMP_API int smp_cache_write(uint64_t key, const float *lr, uint64_t frames, int rate); /* 0 ok; .part then rename */
SMP_API SmpEntry *smp_cache_open(uint64_t key);       /* NULL if missing; touches last-write time; held whole up to 1 s */
SMP_API SmpEntry *smp_cache_open_copy(uint64_t key);  /* the same for keep-length copies: held whole only up to 0.25 s */
SMP_API void smp_cache_close(SmpEntry *e);
SMP_API uint64_t smp_entry_frames(const SmpEntry *e);
SMP_API int smp_entry_rate(const SmpEntry *e);
SMP_API int smp_entry_whole(const SmpEntry *e);       /* 1 when the head holds the whole file */
SMP_API const float *smp_entry_head(const SmpEntry *e, uint64_t *head_frames); /* immutable after open */
SMP_API long smp_entry_read(SmpEntry *e, uint64_t frame, float *lr, long frames); /* streamer thread only */
SMP_API uint64_t smp_cache_trim(uint64_t limit_bytes); /* deletes oldest closed files and .part files over an hour old;
                                                         returns bytes left */
SMP_API int smp_cache_ok(char *why, int cap);         /* 1 if the cache folder works, else 0 and a reason */
SMP_API uint64_t smp_cache_used_bytes(void);         /* the .smpc files' total size (scans the folder) */

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
SMP_API void smp_engine_reset(SmpEngine *e, int now);   /* audio thread, or while nothing processes: every voice ends
                                    (now: at once; else a 5 ms fade, done within the next block or two) and bend
                                    goes back to center */
SMP_API int smp_engine_active_voices(const SmpEngine *e);       /* sounding voices, not counting stolen ones fading out */
SMP_API long smp_engine_underruns(const SmpEngine *e);
SMP_API void smp_engine_seed(SmpEngine *e, unsigned seed);
/* keep length: Rubber Band R3 copies, one per integer shift (and speed), made in the background */
SMP_API int smp_render_shift(SmpSample *s, int reversed, int semis, double speed); /* renders one copy now, on this
                                    thread, into the cache (key kl|<src or rev key>|<semis>|<speed %.2f>|r3); 0 ok */
SMP_API long smp_debug_renders(void);                            /* Rubber Band runs so far */
SMP_API void smp_debug_render_flags(long f);   /* tests: 1 = Rubber Band throws (caught), 2 = no 4-octave limit */
SMP_API int smp_engine_render_status(SmpEngine *e, int *ready, int *total, char *why, int cap); /* 1 when every copy
                                    wanted is ready (the key range's shifts within an octave of the root, and those
                                    of keys played further out); why is a plain reason when renders can't run */
SMP_API int smp_engine_wait_renders(SmpEngine *e, int timeout_ms); /* tests: 0 once the render thread is idle, 1 timeout */
SMP_API const SmpEntry *smp_debug_kl_entry(SmpEngine *e, int semis);  /* tests: the copy published for a shift, or NULL */
SMP_API void smp_debug_kl_counts(SmpEngine *e, int *published, int *retiring);   /* tests: copies in the table, and
                                    retired ones not yet freed */
SMP_API void smp_debug_stream_pause(SmpEngine *e, int on);      /* tests: the streamer does nothing (forces underruns) */
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

/* SoundFonts (sf2.c). Generator ids are the SF2.04 numbers, 0 startAddrsOffset to 58 overridingRootKey. */
#define SMP_SF_NGEN 60
typedef struct { uint16_t src, dest; int16_t amount; uint16_t amt_src, trans; } SmpSfMod;  /* as in pmod/imod */
typedef struct {
  char name[21]; uint32_t start, end, loop_start, loop_end;   /* SF2: sample frames into smpl; SF3: see Task 3 */
  uint32_t rate; uint8_t pitch; int8_t correction; uint16_t link, type;
} SmpSfSample;
typedef struct {
  uint8_t lokey, hikey, lovel, hivel; int sample;            /* index into the font's samples */
  int16_t gen[SMP_SF_NGEN];
  const SmpSfMod *imod; int nimod;   /* instrument level, the 10 default modulators merged in (identical ones replaced) */
  const SmpSfMod *pmod; int npmod;   /* preset level, added on top */
} SmpSfRegion;
typedef struct SmpFont SmpFont;
SMP_API SmpFont *smp_sf_open(const char *path, char *err, int cap);  /* headers only; NULL and a plain reason */
SMP_API void smp_sf_close(SmpFont *f);
SMP_API int smp_sf_preset_count(const SmpFont *f);
SMP_API int smp_sf_preset_info(const SmpFont *f, int i, int *bank, int *program, char *name, int cap); /* sorted by
                                    bank, then program; 1 ok, 0 when i is out of range. Any pointer may be NULL */
SMP_API int smp_sf_find_preset(const SmpFont *f, int bank, int program);   /* exact; -1 if none */
SMP_API int smp_sf_sample_count(const SmpFont *f);
SMP_API const SmpSfSample *smp_sf_sample(const SmpFont *f, int i);         /* NULL when i is out of range */
#ifdef __cplusplus
}
#endif
#endif
