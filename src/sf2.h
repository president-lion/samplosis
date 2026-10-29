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

/* sample types (shdr sfSampleType); SF3 adds 0x10 for Ogg Vorbis data */
enum { SF_MONO = 1, SF_RIGHT = 2, SF_LEFT = 4, SF_LINKED = 8, SF_VORBIS = 0x10, SF_ROM = 0x8000 };

/* the hydra's records as read from the file. Each array keeps its terminal record (EOP, EOI, the last bag)
   so a zone's range is always [x[i], x[i+1]); the counts below leave the terminal record out. */
typedef struct { char name[21]; uint16_t program, bank, bag; } SfPhdr;
typedef struct { char name[21]; uint16_t bag; } SfInst;
typedef struct { uint16_t gen, mod; } SfBag;
typedef struct { uint16_t oper; int16_t amount; } SfGen;   /* a range is lo = amount & 255, hi = amount >> 8 (unsigned) */

struct SmpFont {
  char *path;
  uint64_t size, mtime;                 /* the file's, to tell when it changed */
  uint64_t smpl_off, smpl_size;         /* the 16-bit sample data: file offset and size in bytes */
  uint64_t sm24_off, sm24_size;         /* the low bytes for 24-bit samples; size 0 when absent or unusable */
  int is_sf3;                           /* any sample is Ogg Vorbis */
  SfPhdr *phdr; int nphdr;
  SfBag *pbag; int npbag;
  SmpSfMod *pmod; int npmod;
  SfGen *pgen; int npgen;
  SfInst *inst; int ninst;
  SfBag *ibag; int nibag;
  SmpSfMod *imod; int nimod;
  SfGen *igen; int nigen;
  SmpSfSample *shdr; int nshdr;
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
  /* the loader thread's own: */
  struct SfPcm **held;                  /* per sample: the buffer it owns, published or retired */
  uint8_t *state;                       /* per sample: SF_ST_* */
  uint8_t *need;                        /* per sample: scratch for one pass */
  ULONGLONG *idle_since;                /* per sample: when a published sample stopped being needed (0: needed) */
};

/* A loaded sample. SF_PCM_PAD zero frames sit before data[0] and after data[frames - 1], so 4-point
   interpolation may read a few frames past either end without leaving the allocation. Loop points count frames
   from data[0] and are already clamped: 0 <= loop_start <= loop_end <= frames. */
#define SF_PCM_PAD 8
typedef struct SfPcm {
  uint32_t frames, loop_start, loop_end;
  uint64_t bytes;                       /* the whole allocation */
  int16_t *data;                        /* points SF_PCM_PAD frames into the zeroed area after this header */
} SfPcm;

enum { SF_ST_NONE, SF_ST_PUBLISHED, SF_ST_RETIRED, SF_ST_BROKEN };

/* sf2.c: parse a font's headers into a new SmpFont, and free one */
SmpFont *sf_parse(const char *path, char *err, int cap);
void sf_free(SmpFont *f);

#endif
