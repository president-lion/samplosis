/* sf2.h: the SoundFont types shared by sf2.c and the files that play a font. The public ones (SmpSfMod,
   SmpSfSample, SmpSfRegion) are in samplosis.h. */
#ifndef SF2_H
#define SF2_H
#include <stdint.h>
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
};

#endif
