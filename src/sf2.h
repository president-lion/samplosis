/* sf2.h: the SoundFont types shared by sf2.c and the files that play a font. The public ones (SmpSfMod,
   SmpSfSample, SmpSfRegion) are in samplosis.h. */
#ifndef SF2_H
#define SF2_H
#include <stdint.h>
#include "plat.h"
#include "samplosis.h"
#include "sfknob.h"

/* generator ids used by the parser (the SF2.04 numbers) */
enum { SF_GEN_INSTRUMENT = 41, SF_GEN_KEYRANGE = 43, SF_GEN_VELRANGE = 44, SF_GEN_SAMPLEID = 53 };

/* the number of default modulators (SF2.04 section 8.4) that start every region's instrument list */
#define SF_NDEFMOD 10

/* sample types (shdr sfSampleType). SF3 adds 0x10 for compressed data (Ogg Vorbis, or FLAC in files from the KKQ
   fork of sf2convert); SF4 (Cognitone's sf2convert, ifil 4) adds 0x20 for FLAC. Either way the codec is told by the
   stream's own first bytes, so both bits mean "compressed" (0x20 only from version 3 on, where nothing else uses
   it). */
enum { SF_MONO = 1, SF_RIGHT = 2, SF_LEFT = 4, SF_LINKED = 8, SF_VORBIS = 0x10, SF_FLAC = 0x20, SF_ROM = 0x8000 };
#define SF_COMPRESSED (SF_VORBIS | SF_FLAC)

/* SF2Pack: the decoded pack is a 16-bit cache file; this is the size of its header, before the first frame */
#define SF_PACK_HDR 32
/* a decoded pack's limit in frames: decode_memory's ceiling (2 GiB of float), so 1 GiB of 16-bit sample data */
#define SF_PACK_MAX_FRAMES (1u << 29)

/* the generator SoundFont 1 uses for a sample's root key and tuning (SF2 reserves the number) */
#define SF1_GEN_SAMPLEPITCH 55

/* the hydra's records as read from the file. Each array keeps its terminal record (EOP, EOI, the last bag)
   so a zone's range is always [x[i], x[i+1]); the counts below leave the terminal record out. */
typedef struct { char name[21]; uint16_t program, bank, bag; } SfPhdr;
typedef struct { char name[21]; uint16_t bag; } SfInst;
typedef struct { uint16_t gen, mod; } SfBag;
typedef struct { uint16_t oper; int16_t amount; } SfGen;   /* a range is lo = amount & 255, hi = amount >> 8 (unsigned) */

/* Where a sample's audio lives (the plan's shared definition). SF2 smpl data is PCM_FILE (S16LE, one channel);
   SF3's Ogg Vorbis streams are DECODED: decoded once by the loader into a mono float cache file, then streamed
   from there. */
typedef enum { SMP_SRC_PCM_FILE, SMP_SRC_DECODED } SmpSrcKind;
enum { SMP_PCM_S16LE, SMP_PCM_S24LE, SMP_PCM_S32LE, SMP_PCM_F32LE, SMP_PCM_U8, SMP_PCM_S16BE, SMP_PCM_S24BE };
typedef struct {
  SmpSrcKind kind;
  int path_id;               /* index into the font's path table (the sf2 itself, or a sample file) */
  uint64_t offset;           /* PCM_FILE: byte offset of the first frame; DECODED: of the compressed bytes */
  uint64_t length;           /* DECODED: the compressed bytes at offset (0: the whole file, through decode_audio_ex) */
  uint8_t enc;               /* PCM_FILE: SMP_PCM_* */
  uint8_t channels, channel; /* PCM_FILE: frames are interleaved over channels; this sample is one channel */
  uint64_t key;              /* DECODED: cache key of a decoded mono float copy in the disk cache (sffont.c) */
  uint8_t wrap;              /* DECODED: the bytes are a DLS wave's LIST chunk, which reads as a RIFF WAVE file once
                                its two ids are changed (LIST to RIFF, wave to WAVE); sffont.c does that */
  uint32_t skip;             /* DECODED: the sample starts this many frames into the decoded file (an SFZ start
                                head, see sfsfz.c); the cache file holds the whole channel, so the key leaves it out */
  uint8_t gen;               /* SF_GEN_*: a sample made in memory instead of read (SFZ's *sine, *silence, *noise) */
  uint8_t guessed;           /* DECODED: the loader made it one of a stereo pair without knowing the file's channels
                                (an SFZ sample of a format with no header probe); a mono decode then fills both, at
                                1/sqrt(2) each, so the pair sounds as a centred mono sample would */
  uint8_t giga;              /* DECODED: a compressed GigaStudio wave, which libgig decodes (gigshim.cpp); offset and
                                length are its data chunk's (they key the cache), index is its place in the wave pool */
  uint32_t index;
} SmpSrc;
enum { SF_GEN_NONE = 0, SF_GEN_SINE, SF_GEN_SILENCE, SF_GEN_NOISE };
/* the made samples' lengths and rates: a 1024-frame sine cycle at the rate that makes it C4 (261.63 Hz), so it plays
   at the region's pitch_keycenter; 1024 frames of silence and 32768 of white noise at 44100 Hz. All loop whole. */
#define SF_GEN_FRAMES 1024
#define SF_GEN_SINE_RATE 267905u
#define SF_GEN_NOISE_FRAMES 32768

#define SF_HINTS 16                       /* pending voices' samples a font remembers (a power of two) */
struct SmpFont {
  char *path;
  uint64_t size, mtime;                 /* the file's, to tell when it changed */
  uint64_t smpl_off, smpl_size;         /* the 16-bit sample data: file offset and size in bytes */
  uint64_t sm24_off, sm24_size;         /* the low bytes for 24-bit samples; size 0 when absent or unusable */
  int is_sf3;                           /* any sample is compressed (SF3 or SF4) */
  int version;                          /* ifil's major number (1 for SoundFont 1; 0 when the file has no ifil) */
  int is_pack;                          /* SF2Pack: smpl is one compressed stream of the whole 16-bit sample data */
  int is_dls;                           /* DLS (sfdls.c): only phdr, shdr, src, paths, order and regions are filled */
  int dls_ignored;                      /* DLS: connection blocks (or their output curves) that map to nothing here */
  int dls_boosts;                       /* DLS: regions with a gain above 0 (see sfdls.c) */
  int is_sfz;                           /* SFZ (sfsfz.c): paths, src, shdr, phdr, order and regions are filled */
  int is_ds;                            /* Decent Sampler (sfds.c, through sfsfz.c's builder): is_sfz is set too */
  int is_exs;                           /* Logic EXS (sfexs.c, through sfsfz.c's builder): is_sfz is set too */
  SfKnobs **knobs;                      /* Decent Sampler: per phdr preset, its knobs, effects and CC bindings, or
                                           NULL (a preset without them); NULL when no preset has them */
  uint64_t dslib_key; int dslib_pinned; /* a Decent Sampler bundle: its unpacked folder's cache key, pinned while
                                           the font lives (sf_free unpins it) */
  int is_gig;                           /* GigaStudio (gigshim.cpp, through libgig): filled as DLS is */
  void *gig;                            /* GigaStudio: the loader's libgig file for decoding compressed waves, opened
                                           when the first is needed (sf_gig_decode), freed by sf_free; or NULL */
  int sfz_ignored;                      /* SFZ and GigaStudio: different opcodes, headers or settings used that this
                                           can't play */
  char sfz_first_ignored[64];           /* SFZ and GigaStudio: the first of them */
  SfPhdr *phdr; int nphdr;
  SfBag *pbag; int npbag;
  SmpSfMod *pmod; int npmod;
  SfGen *pgen; int npgen;
  SfInst *inst; int ninst;
  SfBag *ibag; int nibag;
  SmpSfMod *imod; int nimod;
  SfGen *igen; int nigen;
  SmpSfSample *shdr; int nshdr;
  SmpSrc *src;                          /* per sample: where its audio lives */
  char **paths; int npaths;             /* the files path_id names; paths[0] is the font itself */
  int has_long;                         /* some sample may be 1 s or longer, so a player needs its streamer */
  int *order;                           /* phdr indices sorted by bank, then program (the public preset order) */
  /* the flattened presets, built once at open in one allocation (regions is its start): phdr preset i owns
     regions [rstart[i], rstart[i+1]). The modulator lists live in the same block, shared where identical. */
  SmpSfRegion *regions; int nregions;
  int *rstart;
  /* Built by sf_finish_regions at open, immutable after:
     - swmask: per phdr preset, 16 bytes: a 128-bit mask of the keyswitch keys (the regions' sw_lokey..sw_hikey
       ranges and sw_last keys), which set the last keyswitch and never sound. NULL when no region has one.
     - pflags: per phdr preset, SF_PF_*.
     - kix_off, kix: per phdr preset (unless SF_PF_NOINDEX), the regions that cover each key: for key k, kix entries
       [kix_off[129 p + k], kix_off[129 p + k + 1]) are region indices within the preset, in region order.
     - max_sets: the most round-robin sets any one preset has (a player sizes its counters by it). */
  uint8_t *swmask;
  uint8_t *pflags;
  uint32_t *kix_off, *kix;
  int max_sets;

  /* ---- sharing and loading (sffont.c) ---- */
  wchar_t *wpath;                       /* the full path, compared case-insensitively by the registry */
  int refs;                             /* smp_sf_open calls not yet closed (main thread only) */
  volatile LONG closing;                /* set under the registry lock when the last reference goes */
  /* safe from any thread: */
  volatile LONG *users;                 /* per public preset index: synths using it (smp_sf_use) */
  volatile LONG *voices;                /* per sample: holders between pcm_acquire and pcm_release */
  struct SfPcm *volatile *pcm;          /* per sample: the published data, or NULL */
  volatile LONG use_gen, done_gen;      /* use() calls so far, and the count the loader last finished */
  volatile LONG stuck_gen;              /* the count of the loader's last pass that could load nothing more for now
                                           (the file can't be opened, or memory ran out): waiters stop waiting */
  volatile LONG nloaded;                /* samples in memory (published or retired) */
  int16_t cc_init[128]; int has_cc_init;  /* SFZ's set_ccN: controllers' values on a reset, -1 for one not given */
  volatile LONG need_n, need_ready;     /* the loader's last pass: samples the presets in use need, and how many of them are loaded or broken */
  volatile LONG64 loaded_bytes;
  HANDLE *fh;                           /* per path: one shared read handle for streaming (full sharing, positional
                                           reads), made by the loader when it first needs it, closed with the font */
  /* the loader thread's own: */
  struct SfPcm **held;                  /* per sample: the buffer it owns, published or retired */
  uint8_t *state;                       /* per sample: SF_ST_* */
  uint8_t *need;                        /* per sample: scratch for one pass */
  HANDLE *ph;                           /* per path: the handle one pass opened by name (closed at its end) */
  ULONGLONG *idle_since;                /* per sample: when a published sample stopped being needed (0: needed) */
  int keys_done;                        /* sf_source_keys has run */
  volatile uint8_t *dq;                 /* per sample: SF_DQ_* (sffont.c), the decode worker's job state */
  struct SfPcm **dres;                  /* per sample: what the worker made, for the loader to publish */
  uint8_t *dbroken;                     /* per sample: the worker's *broken */
  volatile LONG pack_q;                 /* the worker has the SF2Pack's unpacking */
  volatile LONG progress;               /* the loader's: counts samples tried and decode pieces (offline waits) */
  volatile LONG hint[SF_HINTS], hint_w; /* samples pending voices want first (smp_sf_hint), as index + 1 */
  volatile LONG64 wrote_bytes;          /* decoded bytes written to the cache since it was last trimmed */
  /* SF2Pack (the loader's own, but for pack_key, set at open): the pack decoded into a pinned 16-bit cache file */
  uint64_t pack_key;
  volatile int pack_state;              /* SF_PACK_* (set by the decode worker, read by the loader) */
  SmpEntry *pack_entry;                 /* open while the font is, so the cache file is never trimmed */
  uint64_t pack_frames;                 /* frames in the decoded pack (sample ends are clamped to it at load) */
  /* a problem found after open (a pack that can't be unpacked), for smp_sf_problem: written once by the loader,
     then has_problem is set */
  volatile LONG has_problem;
  char problem[300];
  /* a second problem found later (an SFZ sample that doesn't decode, after the open's own problem): problem2 holds
     both, written once, then has_problem2 is set */
  volatile LONG has_problem2;
  char problem2[620];
};

enum { SF_PACK_NONE, SF_PACK_READY, SF_PACK_BROKEN };

/* A loaded sample: its head in memory, and how to read the rest. The head is the first quarter second, or the whole
   sample when it is under 1 s (then head_frames == frames and nothing streams). SF_PCM_PAD zero frames sit before
   data[0] and after the head's last frame, so interpolation may read a frame or two past either end without
   leaving the allocation. Loop points count frames from the sample's start and are already clamped:
   0 <= loop_start <= loop_end <= frames. */
#define SF_PCM_PAD 8
typedef struct SfPcm {
  uint32_t frames, head_frames, loop_start, loop_end;
  int is_float;                         /* data is float (DECODED, or PCM wider than 16 bits), else int16 */
  uint64_t bytes;                       /* the whole allocation */
  void *data;                           /* points SF_PCM_PAD frames into the zeroed area after this header */
  /* the rest of the sample, when head_frames < frames */
  HANDLE h;                             /* PCM_FILE: the font's shared handle for the file (closed with the font) */
  uint64_t offset;                      /* PCM_FILE: byte offset of frame 0 */
  uint8_t enc, channels, channel;       /* PCM_FILE */
  SmpEntry *entry;                      /* DECODED: the mono float cache file, open while the sample is loaded */
  uint32_t skip;                        /* DECODED: frame 0 is this frame of the cache file */
  uint32_t rate;                        /* the rate as loaded: a decoded file's own, else the sample header's */
  void *tail;                           /* the post-loop head: tail_frames frames from frame tail_start (its own
                                           allocation, with pads), or NULL */
  uint32_t tail_start, tail_frames;
} SfPcm;

enum { SF_ST_NONE, SF_ST_PUBLISHED, SF_ST_RETIRED, SF_ST_BROKEN };

/* sf2.c: parse a font's headers into a new SmpFont, and free one. sf_parse_ex may run on any thread (it touches no
   registry); cancel (may be NULL) is asked now and then while an SFZ's files are read, and stops it ("cancelled"). */
SmpFont *sf_parse(const char *path, char *err, int cap);
SmpFont *sf_parse_ex(const char *path, int (*cancel)(void *), void *carg, char *err, int cap);
/* sffont.c: the cache keys of a font's DECODED samples and of its decoded pack (sf_parse_ex calls it; setup does for
   fonts built another way). Any thread; looks at each sample file's size and modified time. */
void sf_source_keys(SmpFont *f);
/* sffont.c, for the player's offline renders: a pending voice's sample is loaded first (any thread); and the wait
   for it (sample) or for everything the presets in use need (-1), with no fixed cap while the loader keeps making
   progress (0 done, 1 gave up after a minute without any) */
volatile LONG *sf_get_open_progress(void);       /* sfsfz.c: that progress (NULL when none), for sfdslib.c */
void sf_set_open_progress(volatile LONG *prog);   /* sfsfz.c: this thread's next SFZ open counts its sample files into prog[0] of prog[1] */
int smp_sf_wait_loaded(SmpFont *f, int sample);
void sf_free(SmpFont *f);
/* sf2.c, for the other loaders that build SF2's model themselves: every generator's default; a value held to its
   generator's range; SF2's ten default modulators; merging a zone's modulators into a list (identical ones
   replaced, invalid ones skipped; returns the new length); and the sorted preset order (0 when out of memory) */
void sf_gen_defaults(int16_t *gen);
int sf_gen_clamp(int i, int v);
const SmpSfMod *sf_default_mods(void);
int sf_merge_mods(SmpSfMod *list, int n, const SmpSfMod *zm, int nz);
int sf_build_order(SmpFont *f);
/* sf2.c: a region's conditions at "always" (every loader calls it for each region it makes); and, once a font's
   regions are built, the round-robin set ids and the keyswitch masks (sffont.c calls it at open; 0 out of memory).
   A round-robin set is the regions of one preset with the same key and velocity ranges and the same conditions
   except seq_pos (trigger, seq_len, the random range, keyswitches and CC ranges); ids count from 0 per preset. */
void sf_cond_defaults(SmpSfCond *c);
int sf_finish_regions(SmpFont *f);
const uint8_t *sf_preset_swmask(const SmpFont *f, int preset);   /* public preset index: its 16 bytes, or NULL */
/* per-preset flags: it has release-trigger regions; it has regions that can stop others (an exclusive class or a
   group); it has no key index (too many key-wide regions for a modest one) */
enum { SF_PF_RELEASE = 1, SF_PF_ROOM = 2, SF_PF_NOINDEX = 4 };
/* public preset index: the preset's regions (*regs) and how many of them a note of key may concern. With *idx
   set, those are regs[idx[0..n-1]], in region order; with *idx NULL, every region of the preset (n of them).
   *flags gets the preset's SF_PF_*. Any thread. */
int sf_key_regions(const SmpFont *f, int preset, int key, const SmpSfRegion **regs, const uint32_t **idx, int *flags);
/* sffont.c: registers a font a builder made (f->path, size and mtime set; wpath is taken over), as smp_sf_open does
   after parsing: finishes its regions, sets up its loader arrays and adds it to the registry with one reference.
   On failure the font is freed and NULL returned with a reason. */
SmpFont *sf_register(SmpFont *f, wchar_t *full, char *err, int cap);
wchar_t *sf_full_path(const char *path);
#define SF_MAX_ZONE_MODS 1024
#define SF_MAX_REGIONS (1 << 17)
#define SF_MAX_POOL_MODS (1 << 20)
/* sfdls.c: fills f from a DLS file (RIFF form "DLS ", already checked) open on h: 1 when it did, 0 with why when it
   can't, -1 for a GigaStudio file (sf2.c then hands it to sf_parse_gig) */
int sf_parse_dls(SmpFont *f, HANDLE h, uint64_t size, const char *path, char *why, int wcap);
/* gigshim.cpp (C++, every exception caught inside): fills f from a GigaStudio file through libgig: 1 when it did, 0
   with why (a reason to put in brackets, or "cancelled"), -1 with why the whole reason (libgig crashed, now or
   before in this process: every later GigaStudio file is refused). Any thread. sf_gig_decode decodes a compressed
   wave (c->giga) on the loader thread: float, channel after channel, as decode_audio gives, or NULL with err; cancel
   is asked as it goes. sf_gig_free frees the font's libgig file (sf_free calls it). sf_gig_crashed: 1 once libgig
   has crashed in this process. */
int sf_parse_gig(SmpFont *f, const char *path, int (*cancel)(void *), void *carg, char *why, int wcap);
int sf_gig_crashed(void);
float *sf_gig_decode(SmpFont *f, const SmpSrc *c, int *chans, long *frames, int *rate, int (*cancel)(void *),
                     void *carg, char *err, int cap);
void sf_gig_free(SmpFont *f);
/* sfsfz.c: fills f from an SFZ file (f->size, mtime and path set): 1 when it did; 0 with why (a reason to put in
   brackets) or -1 with why (the whole reason) when it can't. Main thread (it reads every sample file's header). */
int sf_parse_sfz(SmpFont *f, const char *path, int (*cancel)(void *), void *carg, char *why, int wcap);
/* an SFZ sample whose length isn't known until the loader decodes it (that length then counts) */
#define SF_UNKNOWN_FRAMES (1u << 26)
/* sfsfz.c: 1 when a file should be read as SFZ: its name ends .sfz, or its first bytes (head, n of them) are text with
   an SFZ header in them */
int sf_is_sfz(const char *path, const uint8_t *head, size_t n);
/* sfsfz.c's builder, shared by formats read into the same model (sfds.c, sfexs.c). sfz_build does what sf_parse_sfz
   does (same return values) with feed in place of the .sfz text: feed is called twice (the first pass collects the
   sample files, the second builds the regions) and gives the levels and opcodes by SFZ's names and values through
   sfz_header ("global", "group", "region" ...) and sfz_opcode; sfz_ignored counts a name this can't play (once,
   second pass only); sfz_loop_whole makes the current region loop the whole sample when neither its opcodes nor its
   file give a loop. feed returns 0 with why to fail. full is the file's full path. */
typedef struct SfzBuild SfzBuild;
typedef int (*SfzFeed)(SfzBuild *b, void *ctx, const wchar_t *full, char *why, int wcap);
int sfz_build(SmpFont *f, const char *path, SfzFeed feed, void *ctx, int (*cancel)(void *), void *carg, char *why,
              int wcap);
void sfz_header(SfzBuild *b, const char *name);
void sfz_opcode(SfzBuild *b, const char *name, const char *value);
void sfz_ignored(SfzBuild *b, const char *name);
int sfz_cancelled(SfzBuild *b);
void sfz_loop_whole(SfzBuild *b);
/* sfz_preset starts a new preset (bank 0, programs 0, 1, 2 ... in call order; name cut to 20 characters): the
   regions fed after it are its own. Called at the same places in both passes. A feed that never calls it makes one
   preset named after the file. */
void sfz_preset(SfzBuild *b, const char *name);
/* sfz_set_knobs hands the current preset its Decent Sampler knobs: 1 when taken (the font frees it), which is in
   the second pass only; 0 and the caller keeps it. */
int sfz_set_knobs(SfzBuild *b, SfKnobs *k);
/* sfds.c: Decent Sampler. sf_is_ds: the name ends .dspreset, or the first bytes have a <DecentSampler element.
   sf_parse_ds: as sf_parse_sfz, for a .dspreset */
int sf_is_ds(const char *path, const uint8_t *head, size_t n);
int sf_parse_ds(SmpFont *f, const char *path, int (*cancel)(void *), void *carg, char *why, int wcap);
/* sf_parse_ds_many: one font (under font_path) of n presets, one per .dspreset in presets[], each named after its
   file and reading its samples from beside it. Same return values. */
int sf_parse_ds_many(SmpFont *f, const char *font_path, const char *const *presets, int n, int (*cancel)(void *),
                     void *carg, char *why, int wcap);
/* sfdslib.c: Decent Sampler bundles. sf_is_dslib: the name ends .dslibrary, or the file starts as a zip does.
   sf_parse_dslib: unpacks the bundle into the cache folder once (<cache>\dslib\<key>\) and reads its presets with
   sf_parse_ds_many. Same return values as sf_parse_sfz. */
/* cache.c, for sfdslib.c: an unpacked bundle's folder for key; a key pinned (never trimmed) until unpinned, once
   per pin; a folder deleted with everything under it (1 when it is gone) */
int smp_cache_dslib_path(uint64_t key, wchar_t *out, int cap);
void smp_cache_pin(uint64_t key);
void smp_cache_unpin(uint64_t key);
int smp_cache_delete_tree(const wchar_t *d);
int sf_is_dslib(const char *path, const uint8_t *head, size_t n);
int sf_parse_dslib(SmpFont *f, const char *path, int (*cancel)(void *), void *carg, char *why, int wcap);
/* sfexs.c: Logic's EXS24 and Sampler instruments. sf_is_exs: the name ends .exs, or the first bytes are an EXS
   block header. sf_parse_exs: as sf_parse_sfz, for an .exs (its samples are looked for near it) */
int sf_is_exs(const char *path, const uint8_t *head, size_t n);
int sf_parse_exs(SmpFont *f, const char *path, int (*cancel)(void *), void *carg, char *why, int wcap);
/* sffont.c: one more hold of a sample the caller already holds (a voice takes one for its streamer) */
void sf_hold(SmpFont *f, int sample);
/* cache.c: a positional read on a handle opened with FILE_FLAG_OVERLAPPED, waited for on an event of the calling
   thread's own, so threads sharing the handle read in parallel. 1 when the read worked (*got may be short at the
   end of the file). A thread that has used it calls smp_pread_thread_done before it ends. */
int smp_pread(HANDLE h, uint64_t off, void *buf, DWORD n, DWORD *got);
/* sffont.c: bytes per sample of an SMP_PCM_* encoding; n frames interleaved over ch channels to channel c alone, as
   float (of) or int16 (oi). In place is fine when a frame is at most 4 bytes and the bytes sit at the end of the
   float stretch they become (each float is written only after its frame's bytes are read, and never over a later
   frame's). */
int sf_pcm_bytes(int enc);
/* cache.c, for the SoundFont engine's mono cache files: the overlapped handle, bytes per frame (4 float, 2 int16),
   the byte offset of a frame, and frames in the file's own format */
HANDLE smp_entry_handle(const SmpEntry *e);
int smp_entry_bps(const SmpEntry *e);
uint64_t smp_entry_data_offset(const SmpEntry *e, uint64_t frame);
long smp_entry_read_raw(SmpEntry *e, uint64_t frame, void *out, long frames);
void sf_pcm_convert(const uint8_t *b, int enc, int ch, int c, uint32_t n, float *of, int16_t *oi);
void smp_pread_thread_done(void);

#endif
