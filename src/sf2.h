/* sf2.h: the SoundFont types shared by sf2.c and the files that play a font. The public ones (SmpSfMod,
   SmpSfSample, SmpSfRegion) are in samplosis.h. */
#ifndef SF2_H
#define SF2_H
#include <stdint.h>
#include <windows.h>
#include "samplosis.h"

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
} SmpSrc;

struct SmpFont {
  char *path;
  uint64_t size, mtime;                 /* the file's, to tell when it changed */
  uint64_t smpl_off, smpl_size;         /* the 16-bit sample data: file offset and size in bytes */
  uint64_t sm24_off, sm24_size;         /* the low bytes for 24-bit samples; size 0 when absent or unusable */
  int is_sf3;                           /* any sample is compressed (SF3 or SF4) */
  int version;                          /* ifil's major number (1 for SoundFont 1; 0 when the file has no ifil) */
  int is_pack;                          /* SF2Pack: smpl is one compressed stream of the whole 16-bit sample data */
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
  volatile LONG64 loaded_bytes;
  HANDLE *fh;                           /* per path: one shared read handle for streaming (full sharing, positional
                                           reads), made by the loader when it first needs it, closed with the font */
  /* the loader thread's own: */
  struct SfPcm **held;                  /* per sample: the buffer it owns, published or retired */
  uint8_t *state;                       /* per sample: SF_ST_* */
  uint8_t *need;                        /* per sample: scratch for one pass */
  HANDLE *ph;                           /* per path: the handle one pass opened by name (closed at its end) */
  ULONGLONG *idle_since;                /* per sample: when a published sample stopped being needed (0: needed) */
  /* SF2Pack (the loader's own, but for pack_key, set at open): the pack decoded into a pinned 16-bit cache file */
  uint64_t pack_key;
  int pack_state;                       /* SF_PACK_* */
  SmpEntry *pack_entry;                 /* open while the font is, so the cache file is never trimmed */
  uint64_t pack_frames;                 /* frames in the decoded pack (sample ends are clamped to it at load) */
  /* a problem found after open (a pack that can't be unpacked), for smp_sf_problem: written once by the loader,
     then has_problem is set */
  volatile LONG has_problem;
  char problem[300];
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
  void *tail;                           /* the post-loop head: tail_frames frames from frame tail_start (its own
                                           allocation, with pads), or NULL */
  uint32_t tail_start, tail_frames;
} SfPcm;

enum { SF_ST_NONE, SF_ST_PUBLISHED, SF_ST_RETIRED, SF_ST_BROKEN };

/* sf2.c: parse a font's headers into a new SmpFont, and free one */
SmpFont *sf_parse(const char *path, char *err, int cap);
void sf_free(SmpFont *f);
/* sffont.c: one more hold of a sample the caller already holds (a voice takes one for its streamer) */
void sf_hold(SmpFont *f, int sample);
/* cache.c: a positional read on a handle opened with FILE_FLAG_OVERLAPPED, waited for on an event of the calling
   thread's own, so threads sharing the handle read in parallel. 1 when the read worked (*got may be short at the
   end of the file). A thread that has used it calls smp_pread_thread_done before it ends. */
int smp_pread(HANDLE h, uint64_t off, void *buf, DWORD n, DWORD *got);
void smp_pread_thread_done(void);

#endif
