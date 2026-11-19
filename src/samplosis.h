#ifndef SAMPLOSIS_H
#define SAMPLOSIS_H
#include <stdint.h>
#ifdef _WIN32
#define SMP_API __declspec(dllexport)
#else
#define SMP_API __attribute__((visibility("default")))
/* MSVC's __declspec(thread) and __declspec(dllexport), as clang and GCC spell them */
#ifndef __declspec
#define __declspec(x) SMP_DECLSPEC_##x
#define SMP_DECLSPEC_thread __thread
#define SMP_DECLSPEC_dllexport __attribute__((visibility("default")))
#define SMP_DECLSPEC_noinline __attribute__((noinline))
#endif
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
#define SMP_NPARAMS 31           /* the sampler's parameters */
/* the SoundFont player's parameters follow in the same table */
enum { SMP_P_SF_VOLUME = 31, SMP_P_SF_VOICES = 32, SMP_P_SF_EFFECTS = 33, SMP_P_SF_TRANSPOSE = 34 };
#define SMP_NALLPARAMS 35        /* the whole table, and the CLAP parameter count */

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
/* ffmpeg_path: FFmpeg's ffmpeg.exe, UTF-8, empty to search for it. raw_*: how files ending .raw or .pcm read:
   raw_rate (8000 to 192000, default 44100), raw_channels (1 or 2, default 2), raw_format, raw_endian. */
enum { SMP_RAW_S16 = 0, SMP_RAW_S24 = 1, SMP_RAW_S32 = 2, SMP_RAW_F32 = 3, SMP_RAW_U8 = 4 };
enum { SMP_RAW_LITTLE = 0, SMP_RAW_BIG = 1 };
typedef struct {
  char cache_dir[1024]; double cache_limit_gb; int stretcher, keep_tone;
  char ffmpeg_path[1024];
  int raw_rate, raw_channels, raw_format, raw_endian;
} SmpSettings;
SMP_API void smp_settings_get(SmpSettings *s);        /* defaults when the file is missing; reloads when it changes */
SMP_API int smp_settings_set(const SmpSettings *s);   /* 0 when saved */
int smp_raw_rates(const int **list);                  /* the rates raw_rate may be, lowest first; returns how many */

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
/* Mono cache files, for the SoundFont engine's decoded samples (SF3 and later compressed formats): the same .smpc
   header with 1 channel, then float32 mono. A SoundFont sample is one channel, so it isn't stored twice as stereo.
   They are trimmed like the others (never while open). A mono entry has no head of its own (the font holds one). */
SMP_API int smp_cache_write_mono(uint64_t key, const float *x, uint64_t frames, int rate);  /* 0 ok */
SMP_API SmpEntry *smp_cache_open_mono(uint64_t key);  /* NULL if missing or not a mono file */
SMP_API long smp_entry_read_mono(SmpEntry *e, uint64_t frame, float *x, long frames);  /* any one thread at a time
                                    per call; positional, so threads may share an entry */
/* 16-bit cache files, for a decoded SF2Pack: the same 32-byte .smpc header (version 2, 1 channel), then int16 mono, so
   the pack's sample headers point into it as they would into an SF2's smpl chunk. Opening one pins it (trim never
   deletes it) until it is closed; the font reads it by its path (smp_cache_path) like any sample file. */
/* 0 ok; 4 when cancel (may be NULL, asked between 4 MB pieces) said stop */
SMP_API int smp_cache_write_i16(uint64_t key, const int16_t *x, uint64_t frames, int rate, int (*cancel)(void *), void *arg);
SMP_API SmpEntry *smp_cache_open_i16(uint64_t key);   /* NULL if missing, short, or not a 16-bit file */
int smp_cache_path(uint64_t key, char *utf8, int cap); /* the .smpc file's full path for key; 0 when there is none */
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
  int by;                               /* what decoded it (SmpDecBy in decode.h): 0 built in, 1 a library,
                                           2 Windows, 3 FFmpeg */
} SmpSample;
SMP_API SmpSample *smp_sample_load(const char *path, char *err, int cap);   /* NULL and a plain message on failure */
/* the same, for a worker thread: cancel (may be NULL) is asked now and then and a nonzero answer stops the decode
   ("cancelled"); stage (may be NULL) is called with arg just before FFmpeg runs */
SmpSample *smp_sample_load_ex(const char *path, int (*cancel)(void *), void (*stage)(void *), void *arg, char *err, int cap);
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
/* A region's conditions (SFZ's, which Decent Sampler and GigaStudio instruments map onto). Every field defaults to
   "always" (sf_cond_defaults in sf2.h); the SF2 family's loaders leave them so. The player checks them at note-on,
   and at note-off for release triggers; sfsynth.c's top comment says exactly how each one works. */
enum { SMP_TRIG_ATTACK = 0, SMP_TRIG_RELEASE = 1, SMP_TRIG_FIRST = 2, SMP_TRIG_LEGATO = 3 };
typedef struct {
  uint8_t trigger;              /* SMP_TRIG_* */
  uint16_t seq_len, seq_pos;    /* round robin: plays when (counter % seq_len) + 1 == seq_pos; 0 = always */
  float lorand, hirand;         /* random 0..1: plays when lorand <= the note's draw < hirand (0 and 1 by default) */
  int8_t sw_lokey, sw_hikey, sw_last, sw_down, sw_up, sw_previous;   /* keyswitches; -1 = unused */
  int8_t sw_default;            /* the keyswitch taken as last pressed before any is; -1 = none */
  uint8_t locc_n; struct { uint8_t cc, lo, hi; } locc[4];           /* CC ranges: each must hold at note-on */
  int8_t xfin_lokey, xfin_hikey, xfout_lokey, xfout_hikey;          /* crossfades; -1 = none */
  uint8_t xfin_lovel, xfin_hivel, xfout_lovel, xfout_hivel;         /* 0, 0, 127, 127 by default (no fade) */
  uint8_t xf_curve;             /* 0 power, 1 gain */
  uint32_t group, off_by;       /* SFZ group / off_by (exclusive class is SF2's narrower form); off_by 0 = none */
  uint8_t off_mode;             /* 0 fast, 1 normal (release) */
  float rt_decay;               /* dB per second held, for release triggers */
  float amp_veltrack;           /* percent; NaN = use SF2 modulators */
  /* added for SFZ (Task 9); the SF2 family leaves them at their defaults */
  float gain;                   /* an amplitude factor beyond what initialAttenuation can say (SFZ volume above
                                   0 dB); 1 */
  const float *amp_velcurve;    /* 128 points, 0..1: the velocity curve amp_veltrack tracks instead of the
                                   default (vel / 127)^2; NULL for the default. Lives in the font. */
  uint16_t polyphony;           /* at most this many voices of the region's group sound at once on a channel; 0 none */
  uint16_t note_polyphony;      /* and at most this many of one key of that group; 0 none */
  uint8_t one_shot;             /* note-offs and the pedals don't release it: it plays its sample to the end */
  uint8_t fil_type;             /* SMP_FIL_*: the kind of filter initialFilterFc and Q set */
  uint8_t fil_unity;            /* the resonant filter keeps its passband at unity, the peak at +Q (SFZ, as sfizz and
                                   sforzando do); 0: SF2.04's mapping (the passband lowered by half the peak) */
  uint8_t pair;                 /* the second (right) channel of a stereo sample: its region follows the one before
                                   it, and the player counts and stops the two as one voice */
  uint32_t end_frame;           /* the region ends at this frame of its sample (clamped to the loaded length); 0: the
                                   end generators say (SFZ's end, so a decode shorter than its header still fits) */
  /* added for GigaStudio (Task 11) */
  float rt_linear;              /* a release trigger's level falls by this fraction of full per second held, in a
                                   straight line, to silence (GigaStudio's release trigger decay as LinuxSampler
                                   plays it: 1 - rt_linear x seconds); 0 = none */
  /* added for SFZ's randoms and level tracking, worked out once per note */
  float delay, delay_random;    /* the voice starts this many seconds late, plus a random 0..delay_random */
  float amp_random;             /* a random 0..amp_random dB (either sign) on the level */
  float pitch_random;           /* a random -pitch_random..+pitch_random cents */
  float amp_keytrack;           /* dB per key away from amp_keycenter */
  uint8_t amp_keycenter;
  /* added for Decent Sampler's knobs */
  uint16_t layer;               /* the preset's layer (its <group>, 1-based) whose gain and tuning the knobs set at
                                   play time (SfKnobs, sfknob.h); 0: none, the region's own values are all there is */
} SmpSfCond;
enum { SMP_FIL_LP = 0, SMP_FIL_HP = 1, SMP_FIL_BP = 2, SMP_FIL_BR = 3,      /* 2-pole lowpass, highpass, bandpass, notch */
       SMP_FIL_LP1 = 4, SMP_FIL_HP1 = 5 };                                  /* 1-pole lowpass and highpass (6 dB an octave) */
typedef struct {
  uint8_t lokey, hikey, lovel, hivel; int sample;            /* index into the font's samples */
  int16_t gen[SMP_SF_NGEN];
  const SmpSfMod *imod; int nimod;   /* instrument level, the 10 default modulators merged in (identical ones replaced) */
  const SmpSfMod *pmod; int npmod;   /* preset level, added on top */
  SmpSfCond cond;
  int rr_set;                        /* its round-robin set within its preset (worked out at open), -1 without seq */
} SmpSfRegion;
typedef struct SmpFont SmpFont;
SMP_API SmpFont *smp_sf_open(const char *path, char *err, int cap);  /* headers only; NULL and a plain reason */
SMP_API void smp_sf_close(SmpFont *f);
/* The window's background loads. smp_sf_parse_detached reads a font's headers (an SFZ's sample file headers too)
   on any thread, into a font not yet shared; cancel (may be NULL) stops an SFZ's reading ("cancelled"). The main
   thread then either adopts it (smp_sf_adopt: it joins the registry with one reference, as smp_sf_open's result,
   or is freed for the same file already open, whose reference is returned; NULL and a reason) or discards it. */
SMP_API SmpFont *smp_sf_parse_detached(const char *path, int (*cancel)(void *), void *arg, char *err, int cap);
/* the same, with progress (may be NULL): progress[1] is set to an SFZ's (Decent Sampler's, EXS's) sample files and
   progress[0] counts them as they are read, for "Loading x.sfz: 412 of 1449 samples". With a cancel given, an
   SFZ's compressed sample files (FLAC, Ogg, MP3, WavPack) are also decoded into the cache as they are read
   (prefill), so each file on a cold disk is visited once, not twice; up to half the cache's limit. */
SMP_API SmpFont *smp_sf_parse_detached_ex(const char *path, int (*cancel)(void *), void *arg, volatile long *progress,
                                          char *err, int cap);
SMP_API SmpFont *smp_sf_adopt(SmpFont *f, char *err, int cap);
SMP_API void smp_sf_discard(SmpFont *f);
SMP_API int smp_sf_preset_count(const SmpFont *f);
SMP_API int smp_sf_preset_info(const SmpFont *f, int i, int *bank, int *program, char *name, int cap); /* sorted by
                                    bank, then program; 1 ok, 0 when i is out of range. Any pointer may be NULL */
SMP_API int smp_sf_find_preset(const SmpFont *f, int bank, int program);   /* exact; -1 if none */
SMP_API int smp_sf_regions(const SmpFont *f, int preset, const SmpSfRegion **out);  /* the region count, and *out
                                    their array (NULL when none). Built at open and never changed, so any thread may
                                    read them until the font is closed */
SMP_API int smp_sf_sample_count(const SmpFont *f);
SMP_API const SmpSfSample *smp_sf_sample(const SmpFont *f, int i);         /* NULL when i is out of range */
/* Sharing and loading (sffont.c). smp_sf_open returns the font already open when the same file (full path,
   case-insensitive, with the same size and modified time) is open, and adds a reference; smp_sf_close drops one,
   and the last frees everything. Open and close are main thread only. Sample data loads on a background thread
   for the presets in use, and is freed 2 s after no preset in use needs it. */
SMP_API void smp_sf_use(SmpFont *f, int preset, int delta);   /* any thread, audio too: a synth starts (+1) or
                                    stops (-1) using a preset (public index), and the loader is woken */
/* What smp_sf_pcm_acquire holds: the sample's head in memory (int16 or float mono, by is_float), the frame
   counts, the loop (frames from the sample's start, clamped to it), and a stream handle for the frames past the
   head (NULL when the head is the whole sample: under 1 s). A few zero frames are readable before head[0] and after
   the head's last frame. A sample that streams and has a loop with frames after it also keeps a post-loop head:
   tail_frames frames from frame tail_start (in tail, the same type as head), so a loop-until-release can go on past
   its loop end at once while the streamer catches up. Everything stays valid until release, even if the sample is
   retired meanwhile. */
typedef struct {
  const void *head; int is_float;
  uint32_t head_frames, frames, loop_start, loop_end;
  const void *stream;
  const void *tail; uint32_t tail_start, tail_frames;
  uint32_t rate;                /* the sample's rate as loaded (a decoded file's own rate), 0 when unknown */
} SmpSfView;
SMP_API int smp_sf_pcm_acquire(SmpFont *f, int sample, SmpSfView *view);   /* any thread, audio too: 1 and the view
                                    when the sample is loaded (held until release); 0 (nothing to release, view
                                    zeroed) when it isn't. view may be NULL */
SMP_API long smp_sf_stream_read(const void *stream, uint64_t frame, float *out, long n);   /* never the audio
                                    thread (it reads the disk); while the sample is held: n frames from frame (from the
                                    sample's start) as float. Returns the frames read; the rest of out is zeroed */
SMP_API void smp_sf_pcm_release(SmpFont *f, int sample);   /* any thread: after a non-NULL acquire */
SMP_API int smp_sf_wait_idle(SmpFont *f, int timeout_ms);   /* tests: 0 once every sample the presets in use
                                    need is loaded (or broken, or can't load for now: the file is gone or memory ran out; the loader
                                    keeps trying every 500 ms), 1 on timeout. Pending retires don't count as busy */
SMP_API int smp_sf_loaded_samples(const SmpFont *f);        /* samples in memory */
SMP_API int smp_sf_load_progress(const SmpFont *f, int *total);   /* of the samples the presets in use need (*total, may
                                    be NULL), how many are loaded (or can't be), as of the loader's last pass */
SMP_API uint64_t smp_sf_loaded_bytes(const SmpFont *f);
/* a problem found after the font opened, in plain words (an SF2Pack that can't be unpacked: "can't unpack
   x.sf2pack (it may be damaged)", "the cache folder can't hold x.sf2pack's unpacked samples"), or "" when there is
   none. Any thread; a text once given stays valid (an SFZ's may later be replaced, once, by a longer one that adds a
   sample that doesn't decode). */
SMP_API const char *smp_sf_problem(const SmpFont *f);
/* SFZ: how many different opcodes (and headers, as "<name>") the file used that this can't play, and the first of
   them in first (may be NULL); GigaStudio: the same for its settings and dimensions (by plain names, such as "EG3
   (pitch envelope)"); 0 for every other format. Any thread. */
SMP_API int smp_sf_ignored(const SmpFont *f, char *first, int cap);
SMP_API void smp_sf_hint(SmpFont *f, int sample);          /* any thread, the audio thread too: sample is wanted now (a
                                    note waits for it), so the loader fetches it, or its decode is moved to the front of
                                    the workers' queue, ahead of the rest of the font */
SMP_API void smp_sf_test_pause(int on);                     /* tests: the loader thread loads nothing while on */
SMP_API void smp_sf_test_close_race(int on);                /* tests: the decode worker takes no job, and a decode
                                    being queued waits for its font's close to begin (sffont.c) */
SMP_API int smp_sf_test_jobs(int *waits);                   /* tests: decode jobs waiting for the worker, and how
                                    many decodes have waited for a close so far (waits may be NULL) */
SMP_API void smp_sf_test_grace(int ms);                     /* tests: the time before an unneeded sample is freed
                                    (0 or less: back to 2000) */
SMP_API void smp_sf_test_whole(int on);                     /* tests: samples loaded from now on are held whole (as
                                    in stage 2), so a render can be compared with a streamed one */
SMP_API long smp_sf_test_decodes(void);                     /* tests: compressed SoundFont samples decoded so far */
SMP_API long smp_sf_test_gig_guards(void);                  /* tests: crashes inside libgig that gigshim.cpp's last-resort
                                    guards turned into refusals (0 when libgig handled every damaged file itself) */
SMP_API void smp_sf_test_gig_fault(int where);              /* tests: 1 makes the next GigaStudio parse, 2 the next
                                    decode open, crash inside its guard (after which this process refuses them all) */
/* sfvoice.c, tests: plays one note of a preset (public index) on a default channel (GM reset controllers, bend
   centred, a 200 cent bend range), every region the key and velocity match, and writes frames of stereo into
   out_lr (frames * 2 floats, interleaved). The note is released at frame release_at (negative: never). Uses the
   preset and waits for its samples first (main thread). Returns the number of voices started, -1 on error. */
SMP_API int smp_sf_test_note(SmpFont *f, int preset, int key, int vel, double rate, long frames, long release_at,
                             float *out_lr);
/* sftest.c, tests only: a font built from a description, so region conditions can be tested without a loader for
   them. One item per line:
     sample NAME FRAMES RATE ROOT sine HZ AMP [loop START END]     (a generated sine, 16-bit)
     preset BANK PROGRAM NAME
     region name=value ...    (in the last preset: sample=N, lokey hikey lovel hivel, gN=V for generator N, and the
                               condition fields by their SmpSfCond names; locc<N>=lo and hicc<N>=hi for CC N;
                               trigger=attack|release|first|legato, xf_curve=power|gain, off_mode=fast|normal)
   The samples are written to path, and the font is registered under it, so smp_synth_load(path) (or smp_sf_open)
   while it is open shares it. Returns it with one reference (smp_sf_close), or NULL and a reason. Main thread. */
/* sfknob.c, tests: a Decent Sampler preset's knobs, bindings, effects and CC bindings as text (public preset index):
   the length, 0 for a preset without them */
SMP_API int smp_sf_test_knobs_text(const SmpFont *f, int preset, char *out, int cap);
SMP_API SmpFont *smp_sf_test_font(const char *desc, const char *path, char *err, int cap);

/* The SoundFont player (sfsynth.c): 16 MIDI channels on one font, up to 1024 voices, 16 stereo outputs.
   Threads: main (new, free, load, script, collect, font), audio (midi, process, reset), and set_param from any
   thread. See the contract at the top of sfsynth.c: free and load never close a font a voice still plays. */
typedef struct SmpSynth SmpSynth;
SMP_API SmpSynth *smp_synth_new(double rate, int max_block);
SMP_API void smp_synth_free(SmpSynth *s);   /* main thread, while nothing processes */
SMP_API int smp_synth_load_font(SmpSynth *s, SmpFont *f, const char *path, char *err, int cap);  /* smp_synth_load
                                    for a font already opened (smp_sf_open or smp_sf_adopt), under path (NULL: the
                                    font's own): the player takes its reference, also on failure (0 and a reason).
                                    Main thread */
SMP_API int smp_synth_load(SmpSynth *s, const char *path, char *err, int cap); /* 1 ok; the audio thread switches
                                    at its next block, and the old font is closed once it has (smp_synth_collect) */
SMP_API void smp_synth_midi(SmpSynth *s, const uint8_t *msg, int len, int at);  /* audio thread, queued like
                                    note_on: note on and off, CC, program, pressure, poly pressure, bend, and the GM,
                                    GS and XG reset SysEx; at is the frame offset in the next block */
SMP_API void smp_synth_process(SmpSynth *s, float *const *outs /* 32: L,R per output */, int n);  /* audio thread */
SMP_API void smp_synth_reset(SmpSynth *s);       /* audio thread: every voice off (5 ms), the effect tails faded
                                    out (10 ms), controllers reset, and the sf_used presets let go */
SMP_API void smp_synth_flush(SmpSynth *s, int now);   /* audio thread, or main while nothing processes: every voice
                                    off (now: at once, else a 5 ms fade), controllers reset, reverb and chorus tails
                                    cleared (now: at once, else faded out over 10 ms); the sf_used presets stay held (their countdown or the next state lets
                                    them go). For a host's reset, which comes on stop, seek and loop */
SMP_API void smp_synth_set_param(SmpSynth *s, int id, double v);   /* ids SMP_P_SF_*, any thread */
/* A Decent Sampler preset's knobs (sfknob.h). The knobs shown: One preset mode, the chosen preset's; All channels,
   every knobbed preset's (labelled "<preset>: <label>" when the font has more than one), at most 16 (the font's
   first 8 knobbed presets keep values; the rest play at their presets' defaults). Values are in the knob's own
   units (its minValue..maxValue), one per knob, shared by the window and the CCs bound to it; a load resets them
   to the presets' values. count, info, get and set: main thread (set: any thread); by_preset is for the state
   lines (public preset index and knob number, 0-based; set 1 writes *v, 0 reads it; 0 when there is no such
   knob). value in SmpKnobInfo is the knob's default. */
typedef struct { char label[96]; int preset, knob, vtype, words; double lo, hi, value; } SmpKnobInfo;
SMP_API void smp_synth_set_tempo(SmpSynth *s, double bpm);   /* the host's tempo, for Decent Sampler's tempo-synced
                                    delays (120 until set; 20..999); any thread */
SMP_API int smp_synth_knob_count(SmpSynth *s);
SMP_API int smp_synth_knob_info(SmpSynth *s, int i, SmpKnobInfo *out);   /* 1 ok */
SMP_API double smp_synth_knob_get(SmpSynth *s, int i);
SMP_API void smp_synth_knob_set(SmpSynth *s, int i, double v);
SMP_API int smp_synth_knob_by_preset(SmpSynth *s, int preset, int knob, double *v, int set);
/* sfknob.c: a knob value in words ("40 percent", "plus 3 semitones", "1/8 dotted", "0.25") and back (the same
   words, or a bare number in the knob's units; 0 when the text isn't one) */
SMP_API void smp_knob_text(const SmpKnobInfo *k, double v, char *out, int cap);
SMP_API int smp_knob_parse(const SmpKnobInfo *k, const char *text, double *v);
SMP_API double smp_synth_get_param(const SmpSynth *s, int id);
SMP_API int smp_synth_script_write(const SmpSynth *s, char *out, int cap);     /* like snprintf; main thread */
SMP_API int smp_synth_script_apply(SmpSynth *s, const char *text, char *err, int cap);  /* 0 ok; nothing changes
                                    on error and err says "line N: ...". Lines not starting with sf_ or soundfont
                                    (the sampler's, plays) are ignored, so one state text serves both */
SMP_API int smp_synth_script_cfg(SmpSynth *s, const char *text, char *err, int cap);  /* main thread: like apply,
                                    but only the configuration lines (sf_mode, sf_preset, sf_channel,
                                    sf_channel_preset, sf_output) are read; with none of a kind, its default. Every
                                    other line is ignored: the font, sf_used and the parameters stay as they are */
/* test helper: events one per line, "midi <byte> <byte> ... <frame>" (decimal; a SysEx is all its bytes) and
   "reset <frame>"; starts from silence with every channel reset. With waiting on (the default) it waits for the
   loader before each block and at notes whose samples aren't loaded, so the render is exact; with it off it
   runs in real time. Writes output 1 (stereo), or all 32 channels when all_outputs is 1. 0 ok. */
SMP_API int smp_synth_render_test(SmpSynth *s, const char *events, long frames, int block, const char *wav_out,
                                  int all_outputs);
SMP_API void smp_synth_test_offline(SmpSynth *s, int wait);       /* tests: render_test's waiting on (1) or off */
SMP_API int smp_synth_wait_loaded(SmpSynth *s, int timeout_ms);   /* tests: 0 once every preset in use is loaded */
SMP_API int smp_synth_collect(SmpSynth *s);   /* main thread: closes replaced fonts the audio thread has let go
                                    of; returns how many still wait */
SMP_API SmpFont *smp_synth_font(const SmpSynth *s);   /* main thread: the font loaded last, or NULL */
/* for the plugin (plugin.c) */
SMP_API void smp_synth_collect_now(SmpSynth *s);   /* main thread, only while nothing processes (the plugin
                                    isn't active): takes the newest font and configuration as the audio thread
                                    would, then closes every replaced font */
SMP_API int smp_synth_unload(SmpSynth *s);   /* main thread: no font (silence); the old one closes like a replaced
                                    one (smp_synth_collect). 1 ok, 0 out of memory (the font stays) */
SMP_API void smp_synth_set_offline(SmpSynth *s, int on);   /* any thread: an offline render (the host's
                                    clap.render): process waits for the loader at each block and at notes whose
                                    samples aren't loaded, so the render is exact. Off: real time, never waits */
SMP_API void smp_synth_skip(SmpSynth *s, int n);   /* audio thread: a block the player doesn't play (the sampler
                                    plays). It takes the font and configuration, ends every voice at once and drops
                                    the queued events, so replaced fonts can still close. The player goes dormant:
                                    the channels' presets and the sf_used ones are let go (their samples are freed
                                    after the grace time; the font stays open), and the next process uses the
                                    channels' presets again */
SMP_API int smp_synth_loading(const SmpSynth *s);   /* main thread: presets in use whose samples are still
                                    loading (0 when the loader has caught up) */
SMP_API int smp_synth_channel_preset(const SmpSynth *s, int ch, int *bank, int *program);   /* main thread: the
                                    preset channel ch (1 to 16) plays now; 1 found, 0 none */
SMP_API int smp_synth_voices(const SmpSynth *s, int *peak);   /* sounding voices (not counting ones fading out
                                    after a steal or kill); peak: the most since the last render_test */
SMP_API long smp_synth_underruns(const SmpSynth *s);   /* stream underruns so far: blocks in which a voice past its
                                    head found its ring empty (it fades out and back in, and never stalls) */
SMP_API long smp_synth_underruns_onset(const SmpSynth *s);   /* tests: of the underruns, those of voices whose first
                                    streamed read hadn't come yet (a note's onset), the rest being voices already fed */
SMP_API void smp_synth_test_stream_pause(SmpSynth *s, int on);   /* tests: the streamer reads nothing */
SMP_API void smp_sfs_test_unsettled(int on);   /* tests: no streamer pass counts as settled (sfs_sync never returns;
                                    smp_synth_collect still closes retired fonts) */
SMP_API int smp_sfs_test_features(int mask);   /* tests: the streamer features on (sfstream.h SFS_F_*; -1: the
                                    default set); returns the mask before */
SMP_API void smp_sfs_test_read_hist(long *out, int n, int reset);   /* tests: the streamers' read latencies so far
                                    (from issue to completion), counts in buckets under 1, 2, 5, 10, 20, 50, 100,
                                    200, 500 ms and over; then out[10] reads, out[11] kilobytes. reset: from zero */
SMP_API int smp_synth_test_voice(const SmpSynth *s, int i, int *chan, int *key, int *released);   /* tests: 1 when
                                    voice slot i (0..1087) is sounding, with its channel (0..15), key and release */
SMP_API void smp_synth_test_seed(SmpSynth *s, unsigned seed);   /* tests: seeds the random draw of lorand/hirand
                                    (main thread while nothing processes) */
#ifdef __cplusplus
}
#endif
#endif
