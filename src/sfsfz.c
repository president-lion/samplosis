/* sfsfz.c: SFZ instruments (sfzformat.com) read into the SoundFont model: one .sfz is one preset (bank 0, program 0,
   named after the file), each <region> one SF2-style region (two for a stereo sample), with the region conditions
   of samplosis.h for SFZ's logic. The player and voice then play it like any SoundFont.

   The text. Comments (// to the line's end and block comments) are blanked first. #define $NAME value is a textual
   replacement on everything after it (a value may use earlier defines, expanded when it is defined, and ends where
   an opcode's value would); #include "file" reads another file in place, looked for from the root .sfz's folder
   and then the including file's, at most 16 deep, never one already being read (so a cycle stops), at most 4096
   includes and 64 MB of text in all. Either directive may start any word of a line, not only the line, as
   sforzando and sfizz read them. Headers are <control>, <global>, <master>,
   <group> and <region>, inheriting in that order (a <global> starts the master and group again from it, a
   <master> the group, and each <region> starts from the group); <curve>, <effect> and <midi> are read past (their
   opcodes do nothing here). An opcode is name=value, and a value runs to the next name= or header or the line's
   end, so sample paths may have spaces; values are trimmed. Header and opcode names, note names and the words of
   enumerated values are taken in any case (sfzformat.com writes them lower case; being lenient breaks no valid
   file). Note names: c4 is 60 (sfzformat.com's convention, middle C), with # or b; <control>'s note_offset and
   octave_offset add to every key value after them. Damaged text never crashes: the reads are bounded, NUL bytes
   count as spaces, a line is expanded to at most 64 KB, and what can't be read is skipped.

   Two passes. The text is read twice: the first pass only collects the sample files the regions name; then every
   file is probed, on up to four threads; the second pass builds the regions from the probed files. The text is
   cheap to read again; the files are what cost (a big library on a USB disk is thousands of opens and seeks).

   Sample files. A sample path is relative to the root .sfz's folder (not an included file's), after <control>'s
   default_path; slashes either way. Each file is probed once, from its header only, never decoded here:
   - PCM WAV (8-bit unsigned, 16-, 24- and 32-bit integer, 32-bit float; WAVE_FORMAT_EXTENSIBLE too) and AIFF or
     AIFF-C (16- and 24-bit big-endian, and 16-bit "sowt") stream straight from the file (SMP_SRC_PCM_FILE: the
     data chunk's offset, encoding and channel count). A WAV's smpl chunk gives the first loop and the unity note.
   - Everything else is SMP_SRC_DECODED: the loader thread decodes the file once into the disk cache, keyed by the
     sample file's own path, size and modified time, and streams it from there. The header gives what it can:
     FLAC's STREAMINFO, Ogg Vorbis' and Opus' identification header and last page, MP3's first frame header,
     WavPack's block header, and a compressed WAV's fmt and fact chunks or an AIFF-C's COMM. What it can't give
     is left for the loader: an unknown length (SF_UNKNOWN_FRAMES; the loaded length counts, and the region's end is
     an absolute frame, cond.end_frame), an unknown rate (the loaded rate counts: SmpSfView.rate), and unknown
     channels, taken as two (a mono file then plays the same in both ears).
   - Probe results (length, rate, channels, loop, unity note, data offset and encoding) are kept in a small file
     next to the decode cache, one per .sfz (sfzprobe1|its path), each entry with its file's size and modified
     time; the next open only looks at each file's size and time and reuses the entry when both match.
   - A file that can't be found is missing; one that can't be read, or whose path can't be made, is unreadable.
     Their regions are skipped, and the font's problem says "Missing N samples (first: name)" (and "N samples can't
     be read (first: name)"). With no region left, the file doesn't open, with that as the reason. After a network
     error on a share (or a drive that isn't ready), no other file under that root is tried: each would wait for
     the same timeout.
   - *sine, *silence and *noise are made in memory (sf2.h's SF_GEN_*), looping whole.
   - Stereo. The voices are mono, so a stereo sample makes two regions, one per channel, like an SF2 stereo pair:
     the left channel panned to (pan - width) and the right to (pan + width), in SF2 pan units (pan and width are
     SFZ percent; width 100, the default, puts the channels hard left and right, so a centred stereo sample plays
     at its own level in each ear). The right one is marked as the pair's follower (cond.pair), so the player counts
     and stops the two as one voice. A file with more channels plays its first two.

   Start heads. A region's offset is its first frame. The loader keeps only a sample's first quarter second in
   memory, so a note starting far into a long sample would have nothing to play until the streamer read it. So a
   region whose offset lies past half of that head gets a sample of its own that starts at the offset (the same
   file, from offset * frame bytes on for PCM_FILE, or skip frames into the decoded copy for DECODED, which shares
   the base sample's cache file), with a head of its own: the note starts at once. One such sample is made per file,
   channel and distinct offset, and only for regions with offsets, so memory stays bounded by the regions. A region
   whose loop starts before its offset keeps the base sample (its loop needs frames before the offset).

   Opcodes and their conversions to SF2 generators (SF2.04 section 8.1.3 units) are documented where finish_region
   builds a region; opcodes this can't play are counted (smp_sf_ignored: distinct names, the first kept) and do
   nothing. The SFZ semantics that need the player (round robin, keyswitches, release triggers, crossfades, groups,
   polyphony, one-shots, amp_veltrack and its curve) go into SmpSfCond.

   Shared. Everything after the text (the two passes, probing, the probe cache, regions, stereo pairs, start heads,
   missing files and the ignored count) is sfz_build's, which other formats use too: a feed gives it headers and
   opcodes by SFZ's names (sfz_header, sfz_opcode), once a pass. sfds.c feeds Decent Sampler presets that way, and sfexs.c Logic's .exs instruments. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sf2.h"
#include "decode.h"

#define MAX_TEXT (32u << 20)             /* the largest .sfz (or included file) read */
#define MAX_ALL_TEXT (64ull << 20)       /* all the text read through includes, in one pass */
#define MAX_INCLUDES 4096
#define MAX_DEPTH 16
#define MAX_DEFINES 4096
#define DEF_SLOTS 8192                   /* the define table's hash slots (twice MAX_DEFINES) */
#define MAX_DEF_VALUE 4096
#define MAX_EXPANSIONS 4096              /* defines replaced in one line, at most */
#define MAX_LINE 65536                   /* a line after defines, and a value, at most */
#define MAX_NAME 128
#define MAX_SPATH 1024                   /* a sample opcode's value, and default_path */
#define MAX_SAMPLES (1 << 20)
#define MAX_SET (1 << 16)                /* names a string set holds, at most */
#define MAX_CHUNKS 4096                  /* chunks walked in one WAV or AIFF header */
#define PROBE_THREADS 32                 /* measured on a USB hard disk, cold, 1449 FLACs: 4 threads 26 s, 8 17 s, 16 12 s,
                                            32 10-12 s, 64 25 s (the disk queues the reads; past 32 they fight) */
#define MAX_DEAD 64                      /* share roots given up on */
#define WPATH (MAX_PATH * 4)

/* the SF2 generators set here (SF2.04 section 8.1.2) */
enum {
  G_START = 0, G_LOOPSTART = 2, G_LOOPEND = 3, G_STARTCOARSE = 4, G_MODLFO_PITCH = 5,
  G_VIBLFO_PITCH = 6, G_MODENV_PITCH = 7, G_FC = 8, G_Q = 9, G_MODLFO_FC = 10, G_MODENV_FC = 11,
  G_MODLFO_VOL = 13, G_PAN = 17, G_DELAY_MODLFO = 21, G_FREQ_MODLFO = 22,
  G_DELAY_VIBLFO = 23, G_FREQ_VIBLFO = 24, G_DELAY_MODENV = 25, G_DELAY_VOLENV = 33,
  G_LOOPSTARTCOARSE = 45, G_ATTEN = 48, G_LOOPENDCOARSE = 50, G_COARSE = 51, G_FINE = 52, G_MODES = 54,
  G_SCALE = 56, G_ROOTKEY = 58
};

enum { LOOP_NONE, LOOP_ONESHOT, LOOP_CONT, LOOP_SUSTAIN };   /* SFZ loop_mode */
enum { LV_NONE, LV_CONTROL, LV_GLOBAL, LV_MASTER, LV_GROUP, LV_REGION, LV_OTHER };
enum { F_PENDING, F_OK, F_MISSING, F_UNREADABLE };

#define MAX_CCM 16                       /* controller modulations a region keeps */
/* what a controller modulates (Opts.ccm): level (dB), pitch (cents), cutoff (cents), pan (percent), the amplitude
   envelope's stages (seconds, sustain percent), the vibrato's depth (cents) and rate (Hz) */
enum { CM_GAIN, CM_PITCH, CM_CUTOFF, CM_PAN, CM_AMPEG0 /* delay; +1 attack ... +5 release */, CM_VIBDEPTH = CM_AMPEG0 + 6,
       CM_VIBFREQ };

/* an LFO as SFZ gives it: Hz, seconds, and its depths (cents to pitch, dB to volume, cents to cutoff) */
typedef struct { double freq, delay, pitch, volume, cutoff; } Lfo;

/* One level's opcodes (global, master, group or region), in SFZ's own units, each at SFZ's default until set. A
   level starts as a copy of the one above it. */
typedef struct {
  char sample[MAX_SPATH];
  int lokey, hikey, lovel, hivel, keycenter;   /* keycenter -2: pitch_keycenter=sample, the file's own root */
  double keytrack, tune, transpose;            /* cents per key, cents, semitones */
  double volume, amplitude, pan, width, veltrack;   /* dB, percent, -100..100, percent, percent */
  int velcurve_n; uint8_t velset[128]; float velcurve[128];   /* amp_velcurve_N points given */
  double ampeg[6], fileg[6], pitcheg[6];       /* delay, attack, hold, decay (seconds), sustain (percent), release */
  double fileg_depth, pitcheg_depth;           /* cents */
  int fileg_set, pitcheg_set;
  int fil_type;                                /* SMP_FIL_*, or -1: a kind the voice can't do */
  double cutoff, resonance, fil_veltrack, fil_keytrack, fil_keycenter;   /* Hz (-1 none), dB, cents, cents/key */
  Lfo amplfo, fillfo, pitchlfo, lfo1, lfo2;
  int lfo1_set, lfo2_set;
  double offset, end, loop_start, loop_end;    /* frames; end and the loop points -1 when not given */
  int loop_mode;                               /* LOOP_*, -1 when not given */
  int trigger, seq_length, seq_position;
  double lorand, hirand;
  int sw_lokey, sw_hikey, sw_last, sw_down, sw_up, sw_previous, sw_default;
  int ncc; uint8_t cc[4], cclo[4], cchi[4];
  int xfin_lokey, xfin_hikey, xfout_lokey, xfout_hikey, xfin_lovel, xfin_hivel, xfout_lovel, xfout_hivel, xf_curve;
  /* controller crossfades: per controller and direction, lo and hi (-1 not given) */
  int nxfcc; struct { uint8_t cc, out; int lo, hi; } xfcc[4]; int xf_cccurve;
  uint32_t group, off_by; int off_mode;
  double rt_decay; int polyphony, note_polyphony;
  double bend_up, bend_down; int bend_set;      /* cents */
  int loop_whole;                              /* Decent Sampler's loopEnabled: with no loop given or in the file,
                                                  the whole sample loops */
  /* the randoms and level tracking the player works out at each note (SmpSfCond) */
  double delay, delay_random, amp_random, pitch_random, amp_keytrack;   /* s, s, dB, cents, dB per key */
  int amp_keycenter;
  /* controllers modulating the region (gain_ccN, pitch_onccN, ampeg_decayccN...): the last one given per CC and
     target wins */
  int nccm; struct { uint8_t cc, target; double amount; } ccm[MAX_CCM];
  int layer;                                   /* samplosis_layer (Decent Sampler's knobs): cond.layer */
} Opts;

/* where a sample start head begins, and its sample per channel */
typedef struct Head { uint32_t at; int id[2]; struct Head *next; } Head;

/* what a probe learns about a file, and what the probe cache keeps of it */
typedef struct {
  int kind, enc, channels, block, pitch;
  int guessed;                       /* channels taken as 2 without knowing (no header probe for the format) */
  uint32_t rate, frames, ls, le;     /* rate 0: unknown; frames SF_UNKNOWN_FRAMES: unknown; le exclusive, 0 0: none */
  uint64_t offset;                   /* PCM_FILE: the first frame's byte offset */
} Info;

/* a sample file (or a made sample) */
typedef struct {
  wchar_t *wpath, *lw;               /* the full path and its lower-case form (NULL for a made sample) */
  char *as_written;                  /* the sample opcode's value that first named it, for messages */
  volatile LONG status;              /* F_* */
  int noted;                         /* its missing or unreadable state is counted */
  int from_cache;                    /* its probe came from the probe cache */
  uint64_t size, mtime;
  int gen;                           /* SF_GEN_*: a made sample */
  Info in;
  int path_id;
  int base[2];                       /* its samples from frame 0, per channel (-1 until made) */
  Head *heads;
} File;

/* a set of strings (open addressing; full compares, so distinct names are counted exactly) */
typedef struct { char **s; int n, cap; } StrSet;

/* a probe cache entry as read back */
typedef struct { wchar_t *lw; uint64_t size, mtime; Info in; } Cached;

/* a region being built: its modulators (an offset into the pool) and its velocity curve (index, or -1) */
typedef struct { SmpSfRegion r; int mod_off, nmod, curve; } Reg;

typedef struct SfzBuild {
  SmpFont *f;
  const char *root_name;             /* the .sfz's file name, for messages */
  wchar_t root_dir[WPATH];           /* its folder, with a trailing backslash */
  int collect;                       /* the first pass: only collect the files */
  int (*cancel)(void *); void *carg; volatile LONG cancelled;
  Opts global, master, group, region;
  int level;
  char default_path[MAX_SPATH];
  int note_offset, octave_offset;
  int16_t cc_init[128];              /* set_ccN: a controller's value before any MIDI sets it, -1 when not given */
  /* defines: names (with the $) and values, and an open-addressing table of their indices */
  char **def_name, **def_val; int ndef; int *def_slot;
  /* includes */
  const wchar_t *stack[MAX_DEPTH + 1]; int nincludes; uint64_t text_bytes;
  char *line, *val, *tmp;            /* MAX_LINE + 1 each */
  StrSet ign, badpath;               /* ignored names; sample paths that couldn't be made */
  /* files */
  File *files; int nfiles, capfiles; int *fslot; int capslot;
  int made[4];                       /* the made samples' files by SF_GEN_*, -1 until used */
  int missing, unreadable; char first_missing[160], first_unreadable[160];
  /* probing: the probe cache read back, share roots given up on */
  Cached *cache; int ncache; int *cslot; int capcslot;
  int probed;                        /* files probed now (not from the cache) */
  SRWLOCK dead_lock; wchar_t dead[MAX_DEAD][WPATH / 4]; int ndead;
  volatile LONG next_probe;
  int *porder;                       /* the files in probe order: by path, so a cold disk reads them folder by folder */
  /* prefill (a background open only): compressed sample files decoded into the cache as they are probed */
  volatile LONG prefill;             /* 1 while it goes on (0 once the budget runs out) */
  volatile LONG64 pf_left;           /* decoded bytes it may still write (half the cache limit) */
  volatile LONG64 pf_wrote;          /* decoded bytes written */
  volatile LONG *prog;               /* the caller's progress: [0] files done, [1] files in all (may be NULL) */
  /* the model being built */
  SmpSfSample *shdr; SmpSrc *src; int ns, caps;
  char **paths; int npaths, cappaths;
  Reg *regs; int nr, capr; int too_many;
  SmpSfMod *mods; int nm, capm;
  float *curves; int nc, capc;
  int swdef;                         /* the instrument's sw_default (the last given), -1 none */
  int oom;
  /* presets (sfz_preset): where each starts in regs and its name; none given is one preset named after the file */
  struct { int start; char name[21]; } *pre; int npre, cappre;
  /* Decent Sampler's knobs, per preset (sfz_set_knobs; index 0 also when no preset was named), and the current
     preset's internal global opcodes: CCs bound to knobs (their default modulators are left out) and no send to the
     shared reverb */
  SfKnobs **pk; int cappk;
  uint8_t unbound[16]; int no_rev_send;
} P;

/* ---- small helpers ---- */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static int ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f' || c == 0; }
static int namech(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$'; }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int ieq(const char *a, const char *b) { while (*a && lower(*a) == lower(*b)) { a++; b++; } return !*a && !*b; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint64_t fnv(const void *b, size_t n) {
  const uint8_t *p = b; uint64_t h = 1469598103934665603ull; size_t i;
  for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
  return h ? h : 1;
}

static int grow(void **a, int *cap, int need, size_t each) {
  void *n; int c;
  if (need <= *cap) return 1;
  c = *cap ? *cap : 16;
  while (c < need) { if (c > (1 << 28)) return 0; c *= 2; }
  if (!(n = realloc(*a, (size_t)c * each))) return 0;
  *a = n; *cap = c;
  return 1;
}

static const char *base_name(const char *path) {
  const char *b = path, *q;
  for (q = path; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
  return b;
}

static int cancelled(P *p) {
  if (p->cancelled) return 1;
  if (p->cancel && p->cancel(p->carg)) { InterlockedExchange(&p->cancelled, 1); return 1; }
  return 0;
}

/* adds x: 1 when it is new, 0 when it was there, -1 when the set is full or memory ran out (x isn't counted) */
static int strset_add(StrSet *t, const char *x) {
  unsigned i;
  if (t->n * 2 >= t->cap) {
    int nc = t->cap ? t->cap * 2 : 64, k;
    char **ns;
    if (t->n >= MAX_SET || !(ns = calloc((size_t)nc, sizeof *ns))) return -1;
    for (k = 0; k < t->cap; k++) if (t->s[k]) {
      unsigned j = (unsigned)fnv(t->s[k], strlen(t->s[k])) & (unsigned)(nc - 1);
      while (ns[j]) j = (j + 1) & (unsigned)(nc - 1);
      ns[j] = t->s[k];
    }
    free(t->s); t->s = ns; t->cap = nc;
  }
  i = (unsigned)fnv(x, strlen(x)) & (unsigned)(t->cap - 1);
  while (t->s[i]) {
    if (!strcmp(t->s[i], x)) return 0;
    i = (i + 1) & (unsigned)(t->cap - 1);
  }
  if (!(t->s[i] = _strdup(x))) return -1;
  t->n++;
  return 1;
}

static void strset_free(StrSet *t) {
  int k;
  for (k = 0; k < t->cap; k++) free(t->s[k]);
  free(t->s); t->s = NULL; t->n = t->cap = 0;
}

/* an opcode (or header, as "<name>") this can't play: counted once per distinct name, the first kept (second pass
   only, so nothing counts twice) */
static void ignored(P *p, const char *name) {
  if (p->collect || strset_add(&p->ign, name) != 1) return;
  if (!p->f->sfz_ignored++) snprintf(p->f->sfz_first_ignored, sizeof p->f->sfz_first_ignored, "%s", name);
}

/* a number; 0 when the value isn't one (it is then left alone) */
static int num(const char *v, double *out) {
  char *e; double d;
  while (ws(*v)) v++;
  d = strtod(v, &e);
  if (e == v || !isfinite(d)) return 0;
  *out = d;
  return 1;
}

static int inum(const char *v, int *out) {
  double d;
  if (!num(v, &d)) return 0;
  *out = (int)clampd(floor(d + 0.5), -2147483647.0, 2147483647.0);
  return 1;
}

/* a key: a MIDI number or a note name (c4 = 60, with # or b, octaves from -1), plus <control>'s offsets */
static int key(P *p, const char *v, int *out) {
  static const int semis[7] = {9, 11, 0, 2, 4, 5, 7};   /* a b c d e f g */
  int k;
  while (ws(*v)) v++;
  if ((*v >= '0' && *v <= '9') || *v == '-' || *v == '+') {
    if (!inum(v, &k)) return 0;
    if (k < 0) { *out = -1; return 1; }                    /* -1: none (sw_*, or a region that never plays) */
  } else {
    char c = lower(*v); int acc = 0, oct;
    char *e;
    if (c < 'a' || c > 'g') return 0;
    v++;
    if (*v == '#') { acc = 1; v++; } else if (*v == 'b' && (v[1] == '-' || (v[1] >= '0' && v[1] <= '9'))) { acc = -1; v++; }
    oct = (int)strtol(v, &e, 10);
    if (e == v || oct < -1 || oct > 9) return 0;
    k = 12 * (oct + 1) + semis[c - 'a'] + acc;
  }
  k += p->note_offset + 12 * p->octave_offset;
  *out = clampi(k, -1, 127);
  return 1;
}

static int seconds_tc(double s, int hi) {      /* seconds to SF2 timecents (1 ms and less: the shortest) */
  if (!(s > 0.001)) return -12000;
  return clampi((int)floor(1200.0 * log2(s) + 0.5), -12000, hi);
}

static int hz_cents(double hz, int lo, int hi) {   /* Hz to SF2 absolute cents (8.176 Hz is 0) */
  if (!(hz > 0)) return lo;
  return clampi((int)floor(1200.0 * log2(hz / 8.176) + 0.5), lo, hi);
}

/* a frame offset split into SF2's fine and coarse (32768-frame) generators */
static void addr(int16_t *gen, int fine, int coarse, int64_t v) {
  int64_t c = v / 32768;
  if (c > 32767) c = 32767;
  if (c < -32768) c = -32768;
  gen[coarse] = (int16_t)c;
  gen[fine] = (int16_t)clampi((int)(v - c * 32768), -32768, 32767);
}

static void opts_default(Opts *o) {
  memset(o, 0, sizeof *o);
  o->lokey = 0; o->hikey = 127; o->lovel = 0; o->hivel = 127; o->keycenter = 60;
  o->keytrack = 100; o->amplitude = 100; o->width = 100; o->veltrack = 100;
  o->ampeg[4] = 100;                           /* ampeg_sustain 100 %; fileg_ and pitcheg_sustain 0 */
  o->fil_type = SMP_FIL_LP; o->cutoff = -1; o->fil_keycenter = 60;
  o->end = o->loop_start = o->loop_end = -1; o->loop_mode = -1;
  o->trigger = SMP_TRIG_ATTACK; o->hirand = 1;
  o->sw_lokey = o->sw_hikey = o->sw_last = o->sw_down = o->sw_up = o->sw_previous = o->sw_default = -1;
  o->xfin_lokey = o->xfin_hikey = o->xfout_lokey = o->xfout_hikey = -1;
  o->xfin_lovel = 0; o->xfin_hivel = 0; o->xfout_lovel = 127; o->xfout_hivel = 127;
  o->bend_up = 200; o->bend_down = -200;
  o->amp_keycenter = 60;
}

/* ---- opcodes ---- */

static int env_stage(const char *s) {
  static const char *names[6] = {"delay", "attack", "hold", "decay", "sustain", "release"};
  int i;
  for (i = 0; i < 6; i++) if (!strcmp(s, names[i])) return i;
  return -1;
}

/* an LFO opcode's last part: freq, delay, pitch, volume, cutoff */
static double *lfo_field(Lfo *l, const char *s) {
  if (!strcmp(s, "freq")) return &l->freq;
  if (!strcmp(s, "delay")) return &l->delay;
  if (!strcmp(s, "pitch")) return &l->pitch;
  if (!strcmp(s, "volume")) return &l->volume;
  if (!strcmp(s, "cutoff")) return &l->cutoff;
  return NULL;
}

/* names that only label things, or hints for other players: read past without counting */
static int label(const char *nm) {
  size_t n = strlen(nm);
  return !strncmp(nm, "hint_", 5) || !strncmp(nm, "label_", 6) || (n > 6 && !strcmp(nm + n - 6, "_label")) ||
         !strcmp(nm, "image") || !strcmp(nm, "md5") || !strcmp(nm, "output") || !strcmp(nm, "off_time");
}

/* name = prefix then a CC number (0..127) then nothing: the number, else -1 */
static int cc_suffix(const char *nm, const char *prefix) {
  size_t n = strlen(prefix); char *e; long k;
  if (strncmp(nm, prefix, n) || nm[n] < '0' || nm[n] > '9') return -1;
  k = strtol(nm + n, &e, 10);
  return !*e && k >= 0 && k <= 127 ? (int)k : -1;
}

/* a controller modulation: replaces the same CC and target's, else is added (up to MAX_CCM) */
static void add_ccm(P *p, Opts *o, const char *nm, int cc, int target, const char *v) {
  double d; int i;
  if (!num(v, &d)) return;
  for (i = 0; i < o->nccm && !(o->ccm[i].cc == cc && o->ccm[i].target == target); i++) {}
  if (i == o->nccm) { if (o->nccm >= MAX_CCM) { ignored(p, nm); return; } o->nccm++; }
  o->ccm[i].cc = (uint8_t)cc; o->ccm[i].target = (uint8_t)target; o->ccm[i].amount = d;
}

/* SFZ's controller opcodes, SFZ 1 and 2 spellings: 1 when nm was one */
static int cc_opcode(P *p, Opts *o, const char *nm, const char *v) {
  static const struct { const char *prefix; int target; } T[] = {
    {"gain_cc", CM_GAIN}, {"gain_oncc", CM_GAIN}, {"volume_oncc", CM_GAIN}, {"amp_cc", CM_GAIN},
    {"pitch_cc", CM_PITCH}, {"pitch_oncc", CM_PITCH}, {"tune_cc", CM_PITCH}, {"tune_oncc", CM_PITCH},
    {"cutoff_cc", CM_CUTOFF}, {"cutoff_oncc", CM_CUTOFF}, {"pan_cc", CM_PAN}, {"pan_oncc", CM_PAN},
    {"ampeg_delaycc", CM_AMPEG0}, {"ampeg_delay_oncc", CM_AMPEG0}, {"ampeg_attackcc", CM_AMPEG0 + 1},
    {"ampeg_attack_oncc", CM_AMPEG0 + 1}, {"ampeg_holdcc", CM_AMPEG0 + 2}, {"ampeg_hold_oncc", CM_AMPEG0 + 2},
    {"ampeg_decaycc", CM_AMPEG0 + 3}, {"ampeg_decay_oncc", CM_AMPEG0 + 3}, {"ampeg_sustaincc", CM_AMPEG0 + 4},
    {"ampeg_sustain_oncc", CM_AMPEG0 + 4}, {"ampeg_releasecc", CM_AMPEG0 + 5}, {"ampeg_release_oncc", CM_AMPEG0 + 5},
    {"pitchlfo_depthcc", CM_VIBDEPTH}, {"pitchlfo_depth_oncc", CM_VIBDEPTH}, {"pitchlfo_freqcc", CM_VIBFREQ},
    {"pitchlfo_freq_oncc", CM_VIBFREQ},
  };
  int i, cc;
  if ((cc = cc_suffix(nm, "set_cc")) >= 0) {   /* a level's set_cc (<control> is where it belongs): the same */
    double d; if (num(v, &d)) p->cc_init[cc] = (int16_t)clampi((int)floor(d + 0.5), 0, 127);
    return 1;
  }
  for (i = 0; i < (int)(sizeof T / sizeof *T); i++)
    if ((cc = cc_suffix(nm, T[i].prefix)) >= 0) { add_ccm(p, o, nm, cc, T[i].target, v); return 1; }
  return 0;
}

/* one opcode of a level (nm is lower case; v trimmed) */
static void opcode(P *p, Opts *o, const char *nm, const char *v) {
  double d; int k, i;
  char w[64];
#define IS(s) (!strcmp(nm, s))
  if (!strncmp(nm, "samplosis_", 10)) {      /* the builder's own, fed by sfds.c for Decent Sampler's knobs */
    if (IS("samplosis_layer") && num(v, &d)) o->layer = clampi((int)d, 0, 65535);
    else if (IS("samplosis_unbind_cc") && num(v, &d) && d >= 0 && d <= 127) p->unbound[(int)d >> 3] |= (uint8_t)(1 << ((int)d & 7));
    else if (IS("samplosis_no_reverb_send")) p->no_rev_send = 1;
    return;
  }
  if (!strncmp(nm, "xfin_", 5) || !strncmp(nm, "xfout_", 6)) {   /* controller crossfades: xfin_locc67=64 */
    int out = nm[2] == 'o', hi; const char *s = nm + (out ? 6 : 5);
    if ((!strncmp(s, "locc", 4) || !strncmp(s, "hicc", 4)) && s[4] >= '0' && s[4] <= '9') {
      char *e; long n = strtol(s + 4, &e, 10);
      hi = s[0] == 'h';
      if (!*e && n >= 0 && n <= 127 && inum(v, &k)) {
        for (i = 0; i < o->nxfcc && (o->xfcc[i].cc != n || o->xfcc[i].out != out); i++) {}
        if (i == o->nxfcc) {
          if (o->nxfcc >= 4) { ignored(p, nm); return; }        /* four controller crossfades a region */
          o->xfcc[i].cc = (uint8_t)n; o->xfcc[i].out = (uint8_t)out; o->xfcc[i].lo = o->xfcc[i].hi = -1; o->nxfcc++;
        }
        if (hi) o->xfcc[i].hi = clampi(k, 0, 127); else o->xfcc[i].lo = clampi(k, 0, 127);
        return;
      }
      ignored(p, nm);
      return;
    }
  }
  if (IS("xf_cccurve")) {
    if (ieq(v, "power")) o->xf_cccurve = 0;
    else if (ieq(v, "gain")) o->xf_cccurve = 1;
    else { snprintf(w, sizeof w, "%s=%.40s", nm, v); ignored(p, w); }
    return;
  }
  if (strstr(nm, "cc") && cc_opcode(p, o, nm, v)) return;   /* the controller opcodes first: pitchlfo_depthccN isn't an LFO field */
  /* the sample */
  if (IS("sample")) {
    size_t n = strlen(v);
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') { v++; n -= 2; }
    if (n >= sizeof o->sample) n = sizeof o->sample - 1;
    memcpy(o->sample, v, n); o->sample[n] = 0;
    return;
  }
  if (p->collect) return;                          /* the first pass only wants the samples */
  /* key and velocity ranges, pitch */
  if (IS("lokey")) { key(p, v, &o->lokey); return; }
  if (IS("hikey")) { key(p, v, &o->hikey); return; }
  if (IS("key")) { if (key(p, v, &k)) o->lokey = o->hikey = o->keycenter = k; return; }
  if (IS("lovel")) { inum(v, &o->lovel); return; }
  if (IS("hivel")) { inum(v, &o->hivel); return; }
  if (IS("pitch_keycenter")) { if (ieq(v, "sample")) o->keycenter = -2; else key(p, v, &o->keycenter); return; }
  if (IS("pitch_keytrack")) { num(v, &o->keytrack); return; }
  if (IS("tune") || IS("pitch")) { num(v, &o->tune); return; }
  if (IS("transpose")) { num(v, &o->transpose); return; }
  /* level and pan */
  if (IS("volume")) { num(v, &o->volume); return; }
  if (IS("amplitude")) { num(v, &o->amplitude); return; }
  if (IS("pan")) { num(v, &o->pan); return; }
  if (IS("width")) { num(v, &o->width); return; }
  if (IS("amp_veltrack")) { num(v, &o->veltrack); return; }
  if (IS("amp_keytrack")) { num(v, &o->amp_keytrack); return; }
  if (IS("amp_keycenter")) { int kc; if (key(p, v, &kc) && kc >= 0) o->amp_keycenter = kc; return; }
  if (IS("amp_random")) { num(v, &o->amp_random); return; }
  if (IS("pitch_random")) { num(v, &o->pitch_random); return; }
  if (IS("delay")) { if (num(v, &d)) o->delay = d < 0 ? 0 : d; return; }
  if (IS("delay_random")) { if (num(v, &d)) o->delay_random = d < 0 ? 0 : d; return; }
  if (!strncmp(nm, "amp_velcurve_", 13)) {
    char *e; long n = strtol(nm + 13, &e, 10);
    if (e != nm + 13 && !*e && n >= 0 && n <= 127 && num(v, &d)) {
      if (!o->velset[n]) o->velcurve_n++;
      o->velset[n] = 1; o->velcurve[n] = (float)clampd(d, 0, 1);
      return;
    }
    ignored(p, nm);
    return;
  }
  /* envelopes */
  if (!strncmp(nm, "ampeg_", 6) && (i = env_stage(nm + 6)) >= 0) { num(v, &o->ampeg[i]); return; }
  if (!strncmp(nm, "fileg_", 6) && (i = env_stage(nm + 6)) >= 0) { if (num(v, &o->fileg[i])) o->fileg_set = 1; return; }
  if (!strncmp(nm, "pitcheg_", 8) && (i = env_stage(nm + 8)) >= 0) { if (num(v, &o->pitcheg[i])) o->pitcheg_set = 1; return; }
  if (IS("fileg_depth")) { if (num(v, &o->fileg_depth)) o->fileg_set = 1; return; }
  if (IS("pitcheg_depth")) { if (num(v, &o->pitcheg_depth)) o->pitcheg_set = 1; return; }
  /* the filter */
  if (IS("fil_type") || IS("filtype")) {
    if (ieq(v, "lpf_2p")) o->fil_type = SMP_FIL_LP;
    else if (ieq(v, "hpf_2p")) o->fil_type = SMP_FIL_HP;
    else if (ieq(v, "bpf_2p")) o->fil_type = SMP_FIL_BP;
    else if (ieq(v, "brf_2p")) o->fil_type = SMP_FIL_BR;
    else if (ieq(v, "lpf_1p")) o->fil_type = SMP_FIL_LP1;
    else if (ieq(v, "hpf_1p")) o->fil_type = SMP_FIL_HP1;
    else { o->fil_type = -1; snprintf(w, sizeof w, "fil_type=%.40s", v); ignored(p, w); }
    return;
  }
  if (IS("cutoff")) { num(v, &o->cutoff); return; }
  if (IS("resonance")) { num(v, &o->resonance); return; }
  if (IS("fil_veltrack")) { num(v, &o->fil_veltrack); return; }
  if (IS("fil_keytrack")) { num(v, &o->fil_keytrack); return; }
  if (IS("fil_keycenter")) { int kc; if (key(p, v, &kc) && kc >= 0) o->fil_keycenter = kc; return; }
  /* LFOs: SFZ 1's amplfo_, fillfo_ and pitchlfo_, and SFZ 2's lfo1_ and lfo2_ */
  if (!strncmp(nm, "amplfo_", 7) || !strncmp(nm, "fillfo_", 7) || !strncmp(nm, "pitchlfo_", 9)) {
    Lfo *l = nm[0] == 'a' ? &o->amplfo : nm[1] == 'i' ? &o->fillfo : &o->pitchlfo;
    const char *s = nm + (nm[0] == 'p' ? 9 : 7);
    double *x = NULL;
    if (!strcmp(s, "freq")) x = &l->freq;
    else if (!strcmp(s, "delay")) x = &l->delay;
    else if (!strcmp(s, "depth")) x = l == &o->amplfo ? &l->volume : l == &o->fillfo ? &l->cutoff : &l->pitch;
    if (x) num(v, x); else ignored(p, nm);
    return;
  }
  if (!strncmp(nm, "lfo", 3) && nm[3] >= '0' && nm[3] <= '9') {
    char *e; long n = strtol(nm + 3, &e, 10);
    double *x = NULL;
    if (*e == '_') {
      if (n == 1) { x = lfo_field(&o->lfo1, e + 1); if (x) o->lfo1_set = 1; }
      else if (n == 2 && (!strcmp(e + 1, "freq") || !strcmp(e + 1, "delay") || !strcmp(e + 1, "pitch"))) {
        x = lfo_field(&o->lfo2, e + 1); o->lfo2_set = 1;
      }
    }
    if (x) num(v, x); else ignored(p, nm);
    return;
  }
  /* sample playback */
  if (IS("offset")) { if (num(v, &d)) o->offset = d < 0 ? 0 : d; return; }
  if (IS("end")) { if (num(v, &d)) o->end = d < 0 ? -1 : d; return; }
  if (IS("loop_start") || IS("loopstart")) { if (num(v, &d)) o->loop_start = d < 0 ? 0 : d; return; }
  if (IS("loop_end") || IS("loopend")) { if (num(v, &d)) o->loop_end = d < 0 ? -1 : d; return; }
  if (IS("loop_mode") || IS("loopmode")) {
    if (ieq(v, "no_loop")) o->loop_mode = LOOP_NONE;
    else if (ieq(v, "one_shot")) o->loop_mode = LOOP_ONESHOT;
    else if (ieq(v, "loop_continuous")) o->loop_mode = LOOP_CONT;
    else if (ieq(v, "loop_sustain")) o->loop_mode = LOOP_SUSTAIN;
    else { snprintf(w, sizeof w, "loop_mode=%.40s", v); ignored(p, w); }
    return;
  }
  /* conditions */
  if (IS("trigger")) {
    if (ieq(v, "attack")) o->trigger = SMP_TRIG_ATTACK;
    else if (ieq(v, "release") || ieq(v, "release_key")) o->trigger = SMP_TRIG_RELEASE;   /* release_key: as release */
    else if (ieq(v, "first")) o->trigger = SMP_TRIG_FIRST;
    else if (ieq(v, "legato")) o->trigger = SMP_TRIG_LEGATO;
    else { snprintf(w, sizeof w, "trigger=%.40s", v); ignored(p, w); }
    return;
  }
  if (IS("seq_length")) { inum(v, &o->seq_length); return; }
  if (IS("seq_position")) { inum(v, &o->seq_position); return; }
  if (IS("lorand")) { num(v, &o->lorand); return; }
  if (IS("hirand")) { num(v, &o->hirand); return; }
  if (IS("sw_lokey")) { key(p, v, &o->sw_lokey); return; }
  if (IS("sw_hikey")) { key(p, v, &o->sw_hikey); return; }
  if (IS("sw_last")) { key(p, v, &o->sw_last); return; }
  if (IS("sw_down")) { key(p, v, &o->sw_down); return; }
  if (IS("sw_up")) { key(p, v, &o->sw_up); return; }
  if (IS("sw_previous")) { key(p, v, &o->sw_previous); return; }
  if (IS("sw_default")) { key(p, v, &o->sw_default); return; }
  if ((!strncmp(nm, "locc", 4) || !strncmp(nm, "hicc", 4)) && nm[4] >= '0' && nm[4] <= '9') {
    char *e; long n = strtol(nm + 4, &e, 10);
    if (!*e && n >= 0 && n <= 127 && inum(v, &k)) {
      for (i = 0; i < o->ncc && o->cc[i] != n; i++) {}
      if (i == o->ncc) {
        if (o->ncc >= 4) { ignored(p, nm); return; }          /* the player holds four CC ranges a region */
        o->cc[i] = (uint8_t)n; o->cclo[i] = 0; o->cchi[i] = 127; o->ncc++;
      }
      if (nm[0] == 'l') o->cclo[i] = (uint8_t)clampi(k, 0, 127); else o->cchi[i] = (uint8_t)clampi(k, 0, 127);
      return;
    }
    ignored(p, nm);
    return;
  }
  if (IS("xfin_lokey")) { key(p, v, &o->xfin_lokey); return; }
  if (IS("xfin_hikey")) { key(p, v, &o->xfin_hikey); return; }
  if (IS("xfout_lokey")) { key(p, v, &o->xfout_lokey); return; }
  if (IS("xfout_hikey")) { key(p, v, &o->xfout_hikey); return; }
  if (IS("xfin_lovel")) { inum(v, &o->xfin_lovel); return; }
  if (IS("xfin_hivel")) { inum(v, &o->xfin_hivel); return; }
  if (IS("xfout_lovel")) { inum(v, &o->xfout_lovel); return; }
  if (IS("xfout_hivel")) { inum(v, &o->xfout_hivel); return; }
  if (IS("xf_velcurve") || IS("xf_keycurve")) {
    if (ieq(v, "power")) o->xf_curve = 0;
    else if (ieq(v, "gain")) o->xf_curve = 1;
    else { snprintf(w, sizeof w, "%s=%.40s", nm, v); ignored(p, w); }
    return;
  }
  if (IS("group") || IS("polyphony_group")) { if (inum(v, &k)) o->group = k > 0 ? (uint32_t)k : 0; return; }
  if (IS("off_by") || IS("offby")) { if (inum(v, &k)) o->off_by = k > 0 ? (uint32_t)k : 0; return; }
  if (IS("off_mode")) {
    if (ieq(v, "fast")) o->off_mode = 0;
    else if (ieq(v, "normal") || ieq(v, "time")) o->off_mode = 1;   /* time (with off_time): the region's release */
    else { snprintf(w, sizeof w, "off_mode=%.40s", v); ignored(p, w); }
    return;
  }
  if (IS("rt_decay")) { num(v, &o->rt_decay); return; }
  if (IS("polyphony")) { inum(v, &o->polyphony); return; }
  if (IS("note_polyphony")) { inum(v, &o->note_polyphony); return; }
  if (IS("bend_up") || IS("bendup")) { if (num(v, &o->bend_up)) o->bend_set = 1; return; }
  if (IS("bend_down") || IS("benddown")) { if (num(v, &o->bend_down)) o->bend_set = 1; return; }
  if (label(nm)) return;
  ignored(p, nm);
#undef IS
}

/* <control>'s own opcodes */
static void control_opcode(P *p, const char *nm, const char *v) {
  if (!strcmp(nm, "default_path")) {
    size_t n = strlen(v);
    if (n >= sizeof p->default_path) n = sizeof p->default_path - 1;
    memcpy(p->default_path, v, n); p->default_path[n] = 0;
  } else if (!strcmp(nm, "note_offset")) {
    int k; if (inum(v, &k)) p->note_offset = clampi(k, -127, 127);
  } else if (!strcmp(nm, "octave_offset")) {
    int k; if (inum(v, &k)) p->octave_offset = clampi(k, -10, 10);
  } else if (cc_suffix(nm, "set_cc") >= 0) {
    cc_opcode(p, NULL, nm, v);
  } else if (!label(nm)) {
    ignored(p, nm);
  }
}

/* ---- sample files: probing ---- */

static int rd_at(HANDLE h, uint64_t off, void *buf, DWORD n) {
  LARGE_INTEGER li; DWORD got = 0;
  li.QuadPart = (LONGLONG)off;
  if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return 0;
  return ReadFile(h, buf, n, &got, NULL) && got == n;
}

static void unknown_info(Info *in) {
  in->kind = SMP_SRC_DECODED; in->channels = 2; in->rate = 0; in->frames = SF_UNKNOWN_FRAMES;
  in->ls = in->le = 0; in->offset = 0; in->enc = 0; in->block = 0;
}

/* RIFF WAVE: 1 when it is plain PCM (in filled for PCM_FILE); 2 when it needs the decoder, with the channels and
   rate from its fmt chunk (and its length from a fact chunk when there is one); 0 when it can't be read this way */
static int probe_wav(HANDLE h, uint64_t size, Info *in) {
  uint8_t c[8], b[64];
  uint64_t pos = 12, doff = 0, dsize = 0;
  int have_fmt = 0, have_data = 0, k;
  unsigned tag = 0, ch = 0, block = 0, bits = 0, sub = 0;
  uint32_t rate = 0, unity = 60, lstart = 0, lend = 0, fact = 0; int have_loop = 0, have_fact = 0;
  for (k = 0; k < MAX_CHUNKS && pos + 8 <= size; k++) {
    uint32_t n;
    if (!rd_at(h, pos, c, 8)) break;
    n = le32(c + 4);
    if (!memcmp(c, "fmt ", 4) && n >= 16 && pos + 8 + 16 <= size) {
      DWORD m = n < 40 ? n : 40;
      if (pos + 8 + m > size || !rd_at(h, pos + 8, b, m)) break;
      tag = le16(b); ch = le16(b + 2); rate = le32(b + 4); block = le16(b + 12); bits = le16(b + 14);
      if (tag == 0xFFFE && m >= 26) sub = le16(b + 24);
      have_fmt = 1;
    } else if (!memcmp(c, "data", 4) && !have_data) {
      doff = pos + 8;
      dsize = (n == 0xFFFFFFFFu || n == 0 || n > size - doff) ? size - doff : n;
      have_data = 1;
    } else if (!memcmp(c, "fact", 4) && n >= 4 && pos + 12 <= size) {
      if (rd_at(h, pos + 8, b, 4)) { fact = le32(b); have_fact = 1; }
    } else if (!memcmp(c, "smpl", 4) && n >= 36 && pos + 8 + 36 <= size) {
      DWORD m = n < 60 ? n : 60;
      if (pos + 8 + m <= size && rd_at(h, pos + 8, b, m)) {
        if (le32(b + 12) <= 127) unity = le32(b + 12);
        if (le32(b + 28) >= 1 && m >= 60) { lstart = le32(b + 44); lend = le32(b + 48); have_loop = 1; }
      }
    }
    pos += 8 + (uint64_t)n + (n & 1);
  }
  if (!have_fmt || !have_data || !ch || !rate) return 0;
  in->pitch = (int)unity;
  if (tag == 0xFFFE) tag = sub;
  in->channels = (int)ch; in->rate = rate; in->offset = doff;
  if (tag == 1 && bits == 8 && block == ch) in->enc = SMP_PCM_U8;
  else if (tag == 1 && bits == 16 && block == 2 * ch) in->enc = SMP_PCM_S16LE;
  else if (tag == 1 && bits == 24 && block == 3 * ch) in->enc = SMP_PCM_S24LE;
  else if (tag == 1 && bits == 32 && block == 4 * ch) in->enc = SMP_PCM_S32LE;
  else if (tag == 3 && bits == 32 && block == 4 * ch) in->enc = SMP_PCM_F32LE;
  else {                                                        /* compressed (ADPCM and the like): the decoder's */
    in->kind = SMP_SRC_DECODED; in->offset = 0; in->enc = 0;
    in->frames = have_fact && fact > 0 && fact < SF_UNKNOWN_FRAMES ? fact : SF_UNKNOWN_FRAMES;
    return 2;
  }
  in->block = (int)block;
  if (dsize / block >= SF_UNKNOWN_FRAMES) return 0;             /* very long: the decoder's business (it refuses) */
  in->frames = (uint32_t)(dsize / block);
  if (have_loop && lend >= lstart && lstart < in->frames) {
    in->ls = lstart; in->le = lend + 1 < in->frames ? lend + 1 : in->frames;   /* smpl's end is the last frame */
  }
  in->kind = SMP_SRC_PCM_FILE;
  return in->frames > 0;
}

/* AIFF and AIFF-C: 1 when its data can stream as PCM_FILE; 2 for a compressed AIFF-C (the decoder's), with what
   COMM says; 0 when it can't be read this way */
static int probe_aiff(HANDLE h, uint64_t size, Info *in, int aifc) {
  uint8_t c[8], b[26];
  uint64_t pos = 12, doff = 0, dsize = 0;
  int have_comm = 0, have_data = 0, k;
  unsigned ch = 0, bits = 0; uint32_t frames = 0; double rate = 0; char comp[5] = "NONE";
  for (k = 0; k < MAX_CHUNKS && pos + 8 <= size; k++) {
    uint32_t n;
    if (!rd_at(h, pos, c, 8)) break;
    n = be32(c + 4);
    if (!memcmp(c, "COMM", 4) && n >= 18) {
      DWORD m = n < 22 ? n : 22;
      if (pos + 8 + m > size || !rd_at(h, pos + 8, b, m)) break;
      ch = be16(b); frames = be32(b + 2); bits = be16(b + 6);
      {   /* the rate: an 80-bit extended float */
        int ex = ((b[8] & 0x7F) << 8 | b[9]) - 16383 - 63;
        uint64_t mant = (uint64_t)be32(b + 10) << 32 | be32(b + 14);
        rate = ldexp((double)mant, ex);
      }
      if (aifc && m >= 22) { memcpy(comp, b + 18, 4); comp[4] = 0; }
      have_comm = 1;
    } else if (!memcmp(c, "SSND", 4) && n >= 8) {
      uint8_t o[4];
      if (pos + 16 > size || !rd_at(h, pos + 8, o, 4)) break;
      doff = pos + 16 + be32(o);
      dsize = doff < size ? size - doff : 0;
      if (n - 8 < dsize) dsize = n - 8;
      have_data = 1;
    }
    pos += 8 + (uint64_t)n + (n & 1);
  }
  if (!have_comm || !ch || !(rate >= 1 && rate < 4e6)) return 0;
  in->channels = (int)ch; in->rate = (uint32_t)(rate + 0.5); in->pitch = 60; in->offset = doff;
  if (bits == 16 && (!memcmp(comp, "NONE", 4) || !memcmp(comp, "twos", 4))) in->enc = SMP_PCM_S16BE;
  else if (bits == 24 && (!memcmp(comp, "NONE", 4) || !memcmp(comp, "twos", 4))) in->enc = SMP_PCM_S24BE;
  else if (bits == 16 && !memcmp(comp, "sowt", 4)) in->enc = SMP_PCM_S16LE;
  else {
    in->kind = SMP_SRC_DECODED; in->offset = 0; in->enc = 0;
    in->frames = frames > 0 && frames < SF_UNKNOWN_FRAMES ? frames : SF_UNKNOWN_FRAMES;
    return 2;
  }
  if (!have_data) return 0;
  in->block = (int)ch * (bits / 8);
  in->frames = (uint32_t)(dsize / (uint64_t)in->block < frames ? dsize / (uint64_t)in->block : frames);
  in->kind = SMP_SRC_PCM_FILE;
  return in->frames > 0;
}

/* the size of an ID3v2 tag at the file's start (0 without one) */
static uint64_t id3_size(const uint8_t *b) {
  if (memcmp(b, "ID3", 3)) return 0;
  return 10 + ((uint64_t)(b[6] & 127) << 21 | (b[7] & 127) << 14 | (b[8] & 127) << 7 | (b[9] & 127));
}

/* FLAC: STREAMINFO's rate, channels and length (skipping an ID3 tag); 0 when it isn't FLAC */
static int probe_flac(HANDLE h, uint64_t size, Info *in) {
  uint8_t b[42];
  uint64_t at, total;
  if (!rd_at(h, 0, b, 10)) return 0;
  at = id3_size(b);
  if (at + 42 > size || !rd_at(h, at, b, 42) || memcmp(b, "fLaC", 4) || (b[4] & 127) != 0) return 0;
  in->rate = (uint32_t)b[18] << 12 | (uint32_t)b[19] << 4 | b[20] >> 4;
  in->channels = ((b[20] >> 1) & 7) + 1;
  total = (uint64_t)(b[21] & 15) << 32 | (uint64_t)be32(b + 22);
  in->frames = total && total < SF_UNKNOWN_FRAMES ? (uint32_t)total : SF_UNKNOWN_FRAMES;
  return 1;
}

/* MP3: the first frame header's channel mode and rate (after an ID3 tag); the length stays unknown */
static int probe_mp3(HANDLE h, uint64_t size, Info *in) {
  static const uint32_t rates[3] = {44100, 48000, 32000};
  uint8_t b[4096];
  uint64_t at;
  DWORD n, i;
  if (!rd_at(h, 0, b, 10)) return 0;
  at = id3_size(b);
  if (at >= size) return 0;
  n = size - at < sizeof b ? (DWORD)(size - at) : (DWORD)sizeof b;
  if (n < 4 || !rd_at(h, at, b, n)) return 0;
  for (i = 0; i + 4 <= n; i++) {
    unsigned ver = (b[i + 1] >> 3) & 3, layer = (b[i + 1] >> 1) & 3, ri = (b[i + 2] >> 2) & 3, bi = b[i + 2] >> 4;
    if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0 || ver == 1 || !layer || ri == 3 || !bi || bi == 15) continue;
    in->rate = rates[ri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    in->channels = (b[i + 3] >> 6) == 3 ? 1 : 2;
    return 1;
  }
  return 0;
}

/* WavPack: the first block header's flags (mono, rate index) and total samples */
static int probe_wv(HANDLE h, Info *in) {
  static const uint32_t rates[15] = {6000, 8000, 9600, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
                                     64000, 88200, 96000, 192000};
  uint8_t b[32];
  uint32_t flags, total, ri;
  if (!rd_at(h, 0, b, 32) || memcmp(b, "wvpk", 4)) return 0;
  total = le32(b + 12); flags = le32(b + 24); ri = (flags >> 23) & 15;
  in->channels = (flags & 4) ? 1 : 2;
  in->rate = ri < 15 ? rates[ri] : 0;
  in->frames = total && total != 0xFFFFFFFFu && total < SF_UNKNOWN_FRAMES ? total : SF_UNKNOWN_FRAMES;
  return 1;
}

/* Ogg Vorbis or Opus: the first page's identification header, and the length from the last page's granule */
static int probe_ogg(HANDLE h, uint64_t size, Info *in) {
  uint8_t b[64], *t;
  DWORD n = size < 65536 ? (DWORD)size : 65536, i;
  int64_t gran = -1; int preskip = 0;
  if (size < 64 || !rd_at(h, 0, b, 64) || memcmp(b, "OggS", 4) || 27u + b[26] + 19 > 64) return 0;
  t = b + 27 + b[26];
  if (!memcmp(t, "\x01vorbis", 7)) { in->channels = t[11]; in->rate = le32(t + 12); }
  else if (!memcmp(t, "OpusHead", 8)) { in->channels = t[9]; preskip = le16(t + 10); in->rate = 48000; }
  else return 0;
  if (!in->channels) in->channels = 2;
  if (n >= 27 && (t = malloc(n)) != NULL) {
    if (rd_at(h, size - n, t, n))
      for (i = n - 27 + 1; i-- > 0;)
        if (!memcmp(t + i, "OggS", 4) && t[i + 4] == 0) {
          int64_t g = (int64_t)((uint64_t)le32(t + i + 6) | (uint64_t)le32(t + i + 10) << 32);
          if (g >= 0) { gran = g; break; }
        }
    free(t);
  }
  gran -= preskip;
  in->frames = gran > 0 && gran < SF_UNKNOWN_FRAMES ? (uint32_t)gran : SF_UNKNOWN_FRAMES;
  return 1;
}

/* a path's root: "\\server\share\" for a share, "X:\" for a drive (empty when neither) */
static void path_root(const wchar_t *w, wchar_t *out, int cap) {
  int n = 0, slashes = 0;
  out[0] = 0;
  if (w[0] == L'\\' && w[1] == L'\\') {
    for (n = 0; w[n] && n < cap - 1; n++) { if (w[n] == L'\\' && ++slashes == 4) { n++; break; } }
  } else if (w[0] && w[1] == L':') n = 2;
  if (n >= cap) n = cap - 1;
  memcpy(out, w, (size_t)n * sizeof(wchar_t)); out[n] = 0;
}

static int dead_root(P *p, const wchar_t *root) {
  int k, dead = 0;
  if (!root[0]) return 0;
  AcquireSRWLockShared(&p->dead_lock);
  for (k = 0; k < p->ndead && !dead; k++) dead = !_wcsicmp(p->dead[k], root);
  ReleaseSRWLockShared(&p->dead_lock);
  return dead;
}

/* Errors that say a share or a drive can't be reached (every other file under it would wait for the same timeout),
   as against an error about the file itself */
static int network_error(DWORD e) {
  return e == ERROR_BAD_NETPATH || e == ERROR_BAD_NET_NAME || e == ERROR_NETNAME_DELETED || e == ERROR_UNEXP_NET_ERR ||
         e == ERROR_NETWORK_UNREACHABLE || e == ERROR_HOST_UNREACHABLE || e == ERROR_SEM_TIMEOUT || e == ERROR_NOT_READY ||
         e == ERROR_INVALID_DRIVE;
}

/* tests: the next probes of files whose path contains g_fail_part fail with g_fail_code (smp_sf_test_probe_error) */
static wchar_t g_fail_part[64]; static DWORD g_fail_code;
SMP_API void smp_sf_test_probe_error(const char *name_part, unsigned long code) {
  g_fail_part[0] = 0; g_fail_code = 0;
  if (name_part && name_part[0] && MultiByteToWideChar(CP_UTF8, 0, name_part, -1, g_fail_part, 64)) g_fail_code = code;
}
static int injected(const wchar_t *path, DWORD *e) {
  if (!g_fail_code || !g_fail_part[0] || !wcsstr(path, g_fail_part)) return 0;
  *e = g_fail_code;
  return 1;
}

/* A root is given up only after a network or not-ready error, and only for a share (\\server\share) or a drive
   Windows calls remote or removable: one odd error on a local disk never makes its other files missing. */
static void give_up_root(P *p, const wchar_t *root, DWORD e) {
  if (!root[0] || !network_error(e)) return;
  if (!(root[0] == L'\\' && root[1] == L'\\')) {
    wchar_t r[4] = {root[0], L':', L'\\', 0};
    UINT t = GetDriveTypeW(r);
    if (t != DRIVE_REMOTE && t != DRIVE_REMOVABLE) return;
  }
  AcquireSRWLockExclusive(&p->dead_lock);
  if (p->ndead < MAX_DEAD) { wcsncpy(p->dead[p->ndead], root, WPATH / 4 - 1); p->dead[p->ndead][WPATH / 4 - 1] = 0; p->ndead++; }
  ReleaseSRWLockExclusive(&p->dead_lock);
}

static const Cached *cache_find(P *p, const wchar_t *lw) {
  unsigned i;
  if (!p->capcslot) return NULL;
  i = (unsigned)fnv(lw, wcslen(lw) * sizeof(wchar_t)) & (unsigned)(p->capcslot - 1);
  while (p->cslot[i] >= 0) {
    if (!wcscmp(p->cache[p->cslot[i]].lw, lw)) return &p->cache[p->cslot[i]];
    i = (i + 1) & (unsigned)(p->capcslot - 1);
  }
  return NULL;
}

/* Prefill. A background open (the window's) reads each compressed sample file whole as it probes it, decodes it on
   the probe thread and writes its channels to the decode cache, exactly as the loader's decode would (the same keys),
   so the loader then only reads heads from the cache. On a cold hard disk each file costs a seek; this visits each
   once instead of twice (once for the header here, once for the decode later), and the decoding runs on all the
   probe threads at once. It knows the channels for sure, so nothing is guessed. Only files a built-in or library
   decoder reads from memory (FLAC, Ogg Vorbis, Opus, MP3, WavPack, compressed WAV), up to 64 MB each, and only while
   the decoded total stays under half the cache's limit; the rest is left to the loader. A file whose copies are all
   in the cache already isn't read. */
#define PF_MAX_FILE (64u << 20)
#define PF_SMALL (2u << 20)                /* a file over this decodes alone (g_pf_big): 32 threads of small ones at once are
                                              well under 1 GB, the big ones one at a time */
static SRWLOCK g_pf_big = SRWLOCK_INIT;
static int pf_cancel(void *a) { return cancelled((P *)a); }

static uint64_t pf_key(const char *u, const File *fl, int ch) {
  char d[2048 * 3 + 160];
  snprintf(d, sizeof d, "sfdec1|%s|%llu|%llu|0|0|%u", u, (unsigned long long)fl->size, (unsigned long long)fl->mtime, (unsigned)ch);
  return smp_key(d);
}

static void prefill(P *p, File *fl, HANDLE h) {
  char u[2048 * 3], err[256];
  int nch = fl->in.channels > 2 ? 2 : fl->in.channels, c, ch = 0, r = 0, have = 1, big;
  long frames = 0;
  LONG64 est;
  uint8_t *bytes;
  float *x;
  DWORD got = 0;
  if (!p->prefill || fl->in.guessed || nch < 1 || fl->size == 0 || fl->size > PF_MAX_FILE) return;
  if (!WideCharToMultiByte(CP_UTF8, 0, fl->wpath, -1, u, (int)sizeof u, NULL, NULL)) return;
  for (c = 0; c < nch && have; c++) {          /* there already? (by name only: opening each costs a scan) */
    char cp[1200 * 3]; wchar_t wc[1200];
    if (!smp_cache_path(pf_key(u, fl, c), cp, (int)sizeof cp) || !MultiByteToWideChar(CP_UTF8, 0, cp, -1, wc, 1200) ||
        GetFileAttributesW(wc) == INVALID_FILE_ATTRIBUTES) have = 0;
  }
  if (have) return;                                             /* decoded before: nothing to do */
  est = fl->in.frames < SF_UNKNOWN_FRAMES ? (LONG64)fl->in.frames * nch * 4 : (LONG64)fl->size * 8;
  if (InterlockedExchangeAdd64(&p->pf_left, -est) - est < 0) { InterlockedExchange(&p->prefill, 0); return; }
  big = fl->size > PF_SMALL;
  if (big) AcquireSRWLockExclusive(&g_pf_big);                /* one big file at a time: memory stays bounded */
  if (!(bytes = malloc((size_t)fl->size))) { if (big) ReleaseSRWLockExclusive(&g_pf_big); return; }
  {
    OVERLAPPED o; memset(&o, 0, sizeof o);
    if (!ReadFile(h, bytes, (DWORD)fl->size, &got, &o) || got != fl->size) { free(bytes); if (big) ReleaseSRWLockExclusive(&g_pf_big); return; }
  }
  x = decode_memory_ex(bytes, (size_t)fl->size, &ch, &frames, &r, pf_cancel, p, err, sizeof err);
  free(bytes);
  if (x && ch >= 1 && frames > 0 && frames < (long)SF_UNKNOWN_FRAMES && r > 0) {
    for (c = 0; c < ch && c < 2; c++)
      if (smp_cache_write_mono(pf_key(u, fl, c), x + (size_t)c * (size_t)frames, (uint64_t)frames, r) == 0)
        InterlockedExchangeAdd64(&p->pf_wrote, (LONG64)frames * 4);
    fl->in.channels = ch; fl->in.frames = (uint32_t)frames; fl->in.rate = (uint32_t)r;
  }
  free(x);                                                      /* (one that didn't decode: the loader tries it its own way, and says) */
  if (big) ReleaseSRWLockExclusive(&g_pf_big);
}


/* A sample file's format, from its header (never decoded here): its size and time first, which may find it in the
   probe cache; then PCM_FILE when it streams as it is, else DECODED with what the header says. Sets status. Any
   thread (the probe workers): it touches only fl, and the dead roots under their lock. */
static void probe(P *p, File *fl) {
  WIN32_FILE_ATTRIBUTE_DATA a;
  wchar_t root[WPATH / 4];
  const Cached *c;
  HANDLE h;
  uint8_t b[12];
  int ok = 0;
  DWORD e = 0;
  path_root(fl->wpath, root, WPATH / 4);
  if (dead_root(p, root)) { fl->status = F_MISSING; return; }
  if (injected(fl->wpath, &e) || !GetFileAttributesExW(fl->wpath, GetFileExInfoStandard, &a)) {
    if (!e) e = GetLastError();
    give_up_root(p, root, e);
    fl->status = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || network_error(e) ? F_MISSING : F_UNREADABLE;
    return;
  }
  if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { fl->status = F_UNREADABLE; return; }
  fl->size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
  fl->mtime = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
  if ((c = cache_find(p, fl->lw)) != NULL && c->size == fl->size && c->mtime == fl->mtime) {
    fl->in = c->in; fl->from_cache = 1; fl->status = F_OK;
    return;
  }
  h = CreateFileW(fl->wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    e = GetLastError();
    give_up_root(p, root, e);
    fl->status = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? F_MISSING : F_UNREADABLE;
    return;
  }
  fl->in.pitch = 60;
  unknown_info(&fl->in);
  fl->in.pitch = 60;
  if (fl->size >= 12 && rd_at(h, 0, b, 12)) {
    uint64_t size = fl->size;
    if (!memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) ok = probe_wav(h, size, &fl->in);
    else if (!memcmp(b, "FORM", 4) && (!memcmp(b + 8, "AIFF", 4) || !memcmp(b + 8, "AIFC", 4))) ok = probe_aiff(h, size, &fl->in, b[11] == 'C');
    else if (!memcmp(b, "fLaC", 4) || !memcmp(b, "ID3", 3)) { ok = probe_flac(h, size, &fl->in); if (!ok) ok = probe_mp3(h, size, &fl->in); }
    else if (!memcmp(b, "OggS", 4)) ok = probe_ogg(h, size, &fl->in);
    else if (!memcmp(b, "wvpk", 4)) ok = probe_wv(h, &fl->in);
    else if (b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) ok = probe_mp3(h, size, &fl->in);
    if (!ok) { int pitch = fl->in.pitch; unknown_info(&fl->in); fl->in.pitch = pitch; fl->in.guessed = 1; }
    if (fl->in.kind != SMP_SRC_PCM_FILE) fl->in.kind = SMP_SRC_DECODED;
    if (fl->in.channels <= 0) fl->in.channels = 2;
    if (ok && fl->in.kind == SMP_SRC_DECODED) prefill(p, fl, h);
    fl->status = F_OK;   /* any other format is left to the loader's decoder, which says if it can't */
  } else {
    fl->status = F_UNREADABLE;                                  /* empty, or it can't be read */
  }
  CloseHandle(h);
  if (fl->status == F_OK) InterlockedIncrement((volatile LONG *)&p->probed);
}

static DWORD WINAPI probe_worker(void *arg) {
  P *p = arg;
  for (;;) {
    LONG i = InterlockedIncrement(&p->next_probe) - 1;
    if (i >= p->nfiles || cancelled(p)) break;
    if (p->porder) i = p->porder[i];
    if (p->files[i].status == F_PENDING && p->files[i].wpath) probe(p, &p->files[i]);
    if (p->prog) InterlockedIncrement(&p->prog[0]);
  }
  return 0;
}

static const File *g_sort_files;             /* probe_all's sort (under g_sort_lock) */
static SRWLOCK g_sort_lock = SRWLOCK_INIT;
static int by_path(const void *a, const void *b) {
  const wchar_t *x = g_sort_files[*(const int *)a].lw, *y = g_sort_files[*(const int *)b].lw;
  if (!x || !y) return (x != NULL) - (y != NULL);
  return wcscmp(x, y);
}

/* every file the first pass found, probed on up to PROBE_THREADS threads (this one among them), in path order: the
   files of a library usually lie on the disk in the order they were copied, folder by folder, so a cold hard disk
   moves forward instead of jumping between folders the way the instrument's text names them */
static void probe_all(P *p) {
  HANDLE t[PROBE_THREADS - 1]; int nt = 0, k, pending = 0, want = PROBE_THREADS;
  for (k = 0; k < p->nfiles; k++) if (p->files[k].status == F_PENDING) pending++;
  if ((p->porder = malloc((size_t)(p->nfiles ? p->nfiles : 1) * sizeof(int))) != NULL) {
    for (k = 0; k < p->nfiles; k++) p->porder[k] = k;
    AcquireSRWLockExclusive(&g_sort_lock);
    g_sort_files = p->files;
    qsort(p->porder, (size_t)p->nfiles, sizeof(int), by_path);
    ReleaseSRWLockExclusive(&g_sort_lock);
  }
  if (p->prog) { InterlockedExchange(&p->prog[0], 0); InterlockedExchange(&p->prog[1], p->nfiles); }
  p->next_probe = 0;
  for (k = 0; k < want - 1 && pending > 8 * (k + 1); k++)
    if ((t[nt] = CreateThread(NULL, 0, probe_worker, p, CREATE_SUSPENDED, NULL)) != NULL) {
      SetThreadPriority(t[nt], THREAD_PRIORITY_BELOW_NORMAL);   /* they decode too (prefill): never ahead of the host */
      ResumeThread(t[nt]);
      nt++;
    }
  probe_worker(p);
  if (nt) WaitForMultipleObjects((DWORD)nt, t, TRUE, INFINITE);
  for (k = 0; k < nt; k++) CloseHandle(t[k]);
  free(p->porder); p->porder = NULL;
}

/* ---- the probe cache: one small file per .sfz next to the decode cache ----
   "SFZP" u32 version (2), u32 count, then per entry: u16 path length (wchar_t), the lower-case path, u64 size,
   u64 mtime, and Info's fields (i32 kind, enc, channels, block, pitch, guessed; u32 rate, frames, ls, le; u64
   offset): PCACHE_ENTRY bytes after the path. */

#define PCACHE_MAX_ENTRIES (1 << 20)
#define PCACHE_VERSION 2
#define PCACHE_ENTRY (16 + 24 + 16 + 8)
#define PCACHE_MIN (2 + 2 + PCACHE_ENTRY)          /* the smallest entry: a one-character path */

/* tests: what the last SFZ open did with its probe cache */
static volatile LONG g_probed, g_cached, g_wrote;
static char g_pcache_path[WPATH * 3];
SMP_API void smp_sf_test_sfz_stats(int *probed, int *cached, int *wrote, char *path, int cap) {
  if (probed) *probed = (int)g_probed;
  if (cached) *cached = (int)g_cached;
  if (wrote) *wrote = (int)g_wrote;
  if (path && cap > 0) snprintf(path, (size_t)cap, "%s", g_pcache_path);
}

static int pcache_path(const wchar_t *sfz, wchar_t *out, int cap) {
  char u[WPATH * 3], d[WPATH * 3 + 16], file[WPATH * 3];
  wchar_t lw[WPATH];
  size_t n;
  wcsncpy(lw, sfz, WPATH - 1); lw[WPATH - 1] = 0;
  CharLowerBuffW(lw, (DWORD)wcslen(lw));
  if (!WideCharToMultiByte(CP_UTF8, 0, lw, -1, u, (int)sizeof u, NULL, NULL)) return 0;
  snprintf(d, sizeof d, "sfzprobe1|%s", u);
  if (!smp_cache_path(smp_key(d), file, (int)sizeof file)) return 0;
  n = strlen(file);
  if (n < 5 || strcmp(file + n - 5, ".smpc")) return 0;
  memcpy(file + n - 5, ".sfzp", 6);                             /* not a .smpc: the cache's own reads skip it */
  return MultiByteToWideChar(CP_UTF8, 0, file, -1, out, cap) > 0;
}

static void pcache_read(P *p, const wchar_t *sfz) {
  wchar_t path[WPATH]; HANDLE h; LARGE_INTEGER sz; uint8_t *b = NULL; DWORD got = 0; uint64_t at = 12; uint32_t n, k;
  if (!pcache_path(sfz, path, WPATH)) return;
  WideCharToMultiByte(CP_UTF8, 0, path, -1, g_pcache_path, (int)sizeof g_pcache_path, NULL, NULL);
  h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return;
  if (GetFileSizeEx(h, &sz) && sz.QuadPart >= 12 && sz.QuadPart < (256 << 20) && (b = malloc((size_t)sz.QuadPart)) != NULL &&
      ReadFile(h, b, (DWORD)sz.QuadPart, &got, NULL) && got == (DWORD)sz.QuadPart && !memcmp(b, "SFZP", 4) &&
      le32(b + 4) == PCACHE_VERSION && (n = le32(b + 8)) <= PCACHE_MAX_ENTRIES && n <= got / PCACHE_MIN &&
      (p->cache = calloc(n ? n : 1, sizeof *p->cache)) != NULL) {   /* a count the file can't hold: not believed */
    for (k = 0; k < n; k++) {
      Cached *c = &p->cache[k]; unsigned L; const uint8_t *q;
      if (at + 2 > got) break;
      L = le16(b + at);
      if (!L || at + 2 + (uint64_t)L * 2 + PCACHE_ENTRY > got || !(c->lw = malloc(((size_t)L + 1) * sizeof(wchar_t)))) break;
      memcpy(c->lw, b + at + 2, (size_t)L * 2); c->lw[L] = 0;
      q = b + at + 2 + (size_t)L * 2;
      memcpy(&c->size, q, 8); memcpy(&c->mtime, q + 8, 8); q += 16;
      c->in.kind = (int)le32(q); c->in.enc = (int)le32(q + 4); c->in.channels = (int)le32(q + 8);
      c->in.block = (int)le32(q + 12); c->in.pitch = (int)le32(q + 16); c->in.guessed = le32(q + 20) == 1;
      c->in.rate = le32(q + 24); c->in.frames = le32(q + 28); c->in.ls = le32(q + 32); c->in.le = le32(q + 36);
      memcpy(&c->in.offset, q + 40, 8);
      /* a damaged entry ends the reading: what it says could make a sample read past its file */
      if ((c->in.kind != SMP_SRC_PCM_FILE && c->in.kind != SMP_SRC_DECODED) || c->in.channels < 1 || c->in.channels > 255 ||
          c->in.enc < 0 || c->in.enc > SMP_PCM_S24BE || c->in.block < 0 || c->in.block > 255 * 4 || !c->in.frames ||
          c->in.le > c->in.frames || c->in.ls > c->in.le || c->in.pitch < 0 || c->in.pitch > 127 ||
          (c->in.kind == SMP_SRC_PCM_FILE && (!c->in.block || !c->in.rate ||
           c->in.offset + (uint64_t)c->in.frames * (uint64_t)c->in.block > c->size))) { free(c->lw); c->lw = NULL; break; }
      at += 2 + (uint64_t)L * 2 + PCACHE_ENTRY;
      p->ncache++;
    }
  }
  free(b);
  CloseHandle(h);
  if (p->ncache) {
    int cap = 64, i;
    while (cap < 2 * p->ncache) cap *= 2;
    if (!(p->cslot = malloc((size_t)cap * sizeof(int)))) return;
    for (i = 0; i < cap; i++) p->cslot[i] = -1;
    p->capcslot = cap;
    for (i = 0; i < p->ncache; i++) {
      unsigned j = (unsigned)fnv(p->cache[i].lw, wcslen(p->cache[i].lw) * sizeof(wchar_t)) & (unsigned)(cap - 1);
      while (p->cslot[j] >= 0) j = (j + 1) & (unsigned)(cap - 1);
      p->cslot[j] = i;
    }
  }
}

/* written again only when a file had to be probed; to a temporary name, then moved over the old one */
static void pcache_write(P *p, const wchar_t *sfz) {
  wchar_t path[WPATH], part[WPATH + 40]; char why[200];
  uint8_t *b; size_t cap = 12, at = 12; uint32_t n = 0; int k;
  HANDLE h; DWORD w = 0;
  if (!p->probed || !pcache_path(sfz, path, WPATH) || !smp_cache_ok(why, (int)sizeof why)) return;
  for (k = 0; k < p->nfiles; k++) if (p->files[k].status == F_OK && p->files[k].lw) cap += 2 + wcslen(p->files[k].lw) * 2 + PCACHE_ENTRY;
  if (!(b = malloc(cap))) return;
  for (k = 0; k < p->nfiles && n < PCACHE_MAX_ENTRIES; k++) {
    const File *fl = &p->files[k]; size_t L; uint8_t *q;
    if (fl->status != F_OK || !fl->lw || (L = wcslen(fl->lw)) == 0 || L > 65535) continue;
    q = b + at;
    q[0] = (uint8_t)L; q[1] = (uint8_t)(L >> 8);
    memcpy(q + 2, fl->lw, L * 2);
    q += 2 + L * 2;
    memcpy(q, &fl->size, 8); memcpy(q + 8, &fl->mtime, 8); q += 16;
    {
      int32_t iv[6] = {fl->in.kind, fl->in.enc, fl->in.channels, fl->in.block, fl->in.pitch, fl->in.guessed ? 1 : 0};
      uint32_t uv[4] = {fl->in.rate, fl->in.frames, fl->in.ls, fl->in.le};
      memcpy(q, iv, 24); memcpy(q + 24, uv, 16); memcpy(q + 40, &fl->in.offset, 8);
    }
    at += 2 + L * 2 + PCACHE_ENTRY;
    n++;
  }
  memcpy(b, "SFZP", 4);
  { uint32_t v = PCACHE_VERSION; memcpy(b + 4, &v, 4); memcpy(b + 8, &n, 4); }
  _snwprintf(part, WPATH + 39, L"%s.%lu.part", path, GetCurrentThreadId());
  part[WPATH + 39] = 0;
  h = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h != INVALID_HANDLE_VALUE) {
    int ok = WriteFile(h, b, (DWORD)at, &w, NULL) && w == (DWORD)at;
    CloseHandle(h);
    if (!ok || !MoveFileExW(part, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(part);
    else InterlockedExchange(&g_wrote, 1);
  }
  free(b);
}

/* ---- sample files: the table ---- */

/* the file table's slot for a lower-cased full path (open addressing on its hash) */
static int *file_slot(P *p, const wchar_t *lw) {
  unsigned i = (unsigned)fnv(lw, wcslen(lw) * sizeof(wchar_t)) & (unsigned)(p->capslot - 1);
  for (;;) {
    int *s = &p->fslot[i];
    if (*s < 0 || !wcscmp(p->files[*s].lw, lw)) return s;
    i = (i + 1) & (unsigned)(p->capslot - 1);
  }
}

static int add_path(P *p, const char *u) {
  char *d;
  if (!grow((void **)&p->paths, &p->cappaths, p->npaths + 1, sizeof *p->paths) || !(d = _strdup(u))) { p->oom = 1; return -1; }
  p->paths[p->npaths] = d;
  return p->npaths++;
}

static int new_file(P *p) {
  File *fl;
  if (!grow((void **)&p->files, &p->capfiles, p->nfiles + 1, sizeof *p->files)) { p->oom = 1; return -1; }
  fl = &p->files[p->nfiles];
  memset(fl, 0, sizeof *fl);
  fl->base[0] = fl->base[1] = -1;
  fl->path_id = -1;
  return p->nfiles++;
}

static void count_bad(P *p, int status, const char *name) {
  if (status == F_MISSING) {
    if (!p->missing++) snprintf(p->first_missing, sizeof p->first_missing, "%s", name);
  } else {
    if (!p->unreadable++) snprintf(p->first_unreadable, sizeof p->first_unreadable, "%s", name);
  }
}

/* The file a sample opcode names (index), or -1 when its regions are skipped (missing, unreadable, unknown). The
   first pass only adds files to the table; the second finds them probed (or probes one the first didn't see). */
static int sample_file(P *p, const char *name) {
  wchar_t w[WPATH], full[WPATH], lw[WPATH], rel[WPATH];
  char joined[MAX_SPATH * 2 + 4], u[WPATH * 3];
  int *slot, i, absolute;
  size_t k;
  File *fl;
  if (name[0] == '*') {                                       /* made in memory */
    int g = !_stricmp(name, "*sine") ? SF_GEN_SINE : !_stricmp(name, "*silence") ? SF_GEN_SILENCE :
            !_stricmp(name, "*noise") ? SF_GEN_NOISE : 0;
    if (p->collect) return -1;
    if (!g) { char t[80]; snprintf(t, sizeof t, "sample=%.60s", name); ignored(p, t); return -1; }
    if (p->made[g] >= 0) return p->made[g];
    if ((i = new_file(p)) < 0) return -1;
    fl = &p->files[i];
    fl->gen = g; fl->status = F_OK; fl->in.kind = SMP_SRC_PCM_FILE; fl->in.channels = 1; fl->in.pitch = 60; fl->path_id = 0;
    fl->in.rate = g == SF_GEN_SINE ? SF_GEN_SINE_RATE : 44100;
    fl->in.frames = g == SF_GEN_NOISE ? SF_GEN_NOISE_FRAMES : SF_GEN_FRAMES;
    fl->in.ls = 0; fl->in.le = fl->in.frames;
    p->made[g] = i;
    return i;
  }
  /* the path: absolute as it is, else the root's folder, default_path, then the name; slashes made backslashes */
  absolute = (name[0] && name[1] == ':') || name[0] == '\\' || name[0] == '/';
  if (absolute) snprintf(joined, sizeof joined, "%s", name);
  else snprintf(joined, sizeof joined, "%s%s", p->default_path, name);
  for (k = 0; joined[k]; k++) if (joined[k] == '/') joined[k] = '\\';
  if ((!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, joined, -1, rel, WPATH) &&
       !MultiByteToWideChar(CP_ACP, 0, joined, -1, rel, WPATH)) || !rel[0]) goto bad_path;
  if (absolute || (rel[0] && rel[1] == L':') || rel[0] == L'\\') {
    if (wcslen(rel) >= WPATH) goto bad_path;
    wcscpy(w, rel);
  } else if (_snwprintf(w, WPATH - 1, L"%s%s", p->root_dir, rel) < 0) goto bad_path;   /* too long: never cut short */
  w[WPATH - 1] = 0;
  { DWORD n = GetFullPathNameW(w, WPATH, full, NULL); if (!n || n >= WPATH) goto bad_path; }
  wcscpy(lw, full);
  CharLowerBuffW(lw, (DWORD)wcslen(lw));
  if (p->nfiles + 1 > p->capslot / 2) {                       /* the table at most half full */
    int nc = p->capslot ? p->capslot * 2 : 256, j, *ns = malloc((size_t)nc * sizeof(int)), *old = p->fslot, oc = p->capslot;
    if (!ns) { p->oom = 1; return -1; }
    for (j = 0; j < nc; j++) ns[j] = -1;
    p->fslot = ns; p->capslot = nc;
    for (j = 0; j < oc; j++) if (old[j] >= 0) *file_slot(p, p->files[old[j]].lw) = old[j];
    free(old);
  }
  slot = file_slot(p, lw);
  if (*slot < 0) {
    if ((i = new_file(p)) < 0) return -1;
    fl = &p->files[i];
    if (!(fl->wpath = _wcsdup(full)) || !(fl->lw = _wcsdup(lw)) || !(fl->as_written = _strdup(name))) { p->oom = 1; return -1; }
    *slot = i;
  }
  i = *slot;
  if (p->collect) return -1;                                  /* probed between the passes */
  fl = &p->files[i];
  if (fl->status == F_PENDING) probe(p, fl);                  /* named only now (the text changed?): probed here */
  if (fl->status != F_OK) {
    if (!fl->noted) { fl->noted = 1; count_bad(p, fl->status, fl->as_written); }
    return -1;
  }
  if (fl->path_id < 0) {
    if (!WideCharToMultiByte(CP_UTF8, 0, full, -1, u, (int)sizeof u, NULL, NULL)) { fl->status = F_UNREADABLE; fl->noted = 1; count_bad(p, F_UNREADABLE, name); return -1; }
    if ((fl->path_id = add_path(p, u)) < 0) return -1;
  }
  return i;
bad_path:
  /* a path that can't be made (bad text, too long): unreadable, counted once per name */
  if (!p->collect && strset_add(&p->badpath, name) == 1) count_bad(p, F_UNREADABLE, name);
  return -1;
}

/* a sample for channel ch of a file, starting at frame at (0: the file's own start); -1 when no more can be made */
static int new_sample(P *p, File *fl, int ch, uint32_t at) {
  SmpSfSample *h; SmpSrc *c;
  const wchar_t *bn;
  int old = p->caps;
  if (p->ns >= MAX_SAMPLES || !grow((void **)&p->shdr, &p->caps, p->ns + 2, sizeof *p->shdr)) { p->oom = p->ns < MAX_SAMPLES; return -1; }
  if (!p->src || p->caps != old) {               /* src grows with shdr */
    if (!(c = realloc(p->src, (size_t)p->caps * sizeof *p->src))) { p->oom = 1; return -1; }
    p->src = c;
  }
  h = &p->shdr[p->ns]; c = &p->src[p->ns];
  memset(h, 0, sizeof *h); memset(c, 0, sizeof *c);
  if (fl->wpath) {
    bn = wcsrchr(fl->wpath, L'\\'); bn = bn ? bn + 1 : fl->wpath;
    WideCharToMultiByte(CP_UTF8, 0, bn, -1, h->name, (int)sizeof h->name - 1, NULL, NULL);
    h->name[sizeof h->name - 1] = 0;
  } else {
    snprintf(h->name, sizeof h->name, "%s", fl->gen == SF_GEN_SINE ? "*sine" : fl->gen == SF_GEN_NOISE ? "*noise" : "*silence");
  }
  h->start = 0; h->end = fl->in.frames - at;
  if (fl->in.le > fl->in.ls && fl->in.le > at) { h->loop_start = fl->in.ls > at ? fl->in.ls - at : 0; h->loop_end = fl->in.le - at; }
  h->rate = fl->in.rate ? fl->in.rate : 44100;   /* unknown: the loaded rate counts (SmpSfView.rate) */
  h->pitch = (uint8_t)clampi(fl->in.pitch, 0, 127); h->type = SF_MONO;
  c->kind = (SmpSrcKind)fl->in.kind; c->path_id = fl->path_id; c->gen = (uint8_t)fl->gen;
  c->guessed = (uint8_t)(fl->in.guessed && fl->in.kind == SMP_SRC_DECODED);
  c->channels = (uint8_t)clampi(fl->in.channels, 1, 255); c->channel = (uint8_t)ch;
  if (fl->in.kind == SMP_SRC_PCM_FILE) { c->offset = fl->in.offset + (uint64_t)at * (uint64_t)fl->in.block; c->enc = (uint8_t)fl->in.enc; }
  else c->skip = at;
  if (h->end - h->start >= h->rate) p->f->has_long = 1;
  return p->ns++;
}

static int get_sample(P *p, File *fl, int ch, uint32_t at) {
  Head *hd;
  if (!at) {
    if (fl->base[ch] < 0) fl->base[ch] = new_sample(p, fl, ch, 0);
    return fl->base[ch];
  }
  for (hd = fl->heads; hd && hd->at != at; hd = hd->next) {}
  if (!hd) {
    if (!(hd = calloc(1, sizeof *hd))) { p->oom = 1; return -1; }
    hd->at = at; hd->id[0] = hd->id[1] = -1; hd->next = fl->heads; fl->heads = hd;
  }
  if (hd->id[ch] < 0) hd->id[ch] = new_sample(p, fl, ch, at);
  return hd->id[ch];
}

/* ---- building a region ---- */

/* SFZ envelope values into SF2's six envelope generators from base (delay ... release): times as timecents,
   sustain as centibels of attenuation (volume) or tenths of a percent below full (modulation) */
static void env_gens(int16_t *gen, int base, const double *e, int vol) {
  int i;
  for (i = 0; i < 6; i++) {
    if (i == 4) {
      if (vol) gen[base + 4] = (int16_t)(e[4] >= 100 ? 0 : e[4] <= 0 ? 1440 : clampi((int)floor(-200.0 * log10(e[4] / 100.0) + 0.5), 0, 1440));
      else gen[base + 4] = (int16_t)clampi((int)floor((100 - clampd(e[4], 0, 100)) * 10 + 0.5), 0, 1000);
    } else {
      gen[base + i] = (int16_t)seconds_tc(e[i], (i == 0 || i == 2) ? 5000 : 8000);
    }
  }
}

/* the region's modulators into the pool (sharing an identical list among the last few made); returns its offset */
static int add_mods(P *p, const SmpSfMod *m, int n) {
  int k;
  for (k = p->nm - n; k >= 0 && k >= p->nm - 64 * n; k -= n)
    if (!memcmp(p->mods + k, m, (size_t)n * sizeof *m)) return k;
  if (!grow((void **)&p->mods, &p->capm, p->nm + n, sizeof *p->mods)) { p->oom = 1; return -1; }
  memcpy(p->mods + p->nm, m, (size_t)n * sizeof *m);
  p->nm += n;
  return p->nm - n;
}

/* amp_velcurve_N's points as 128 values, linear between the points given (0 at 0 and 1 at 127 unless given) */
static int add_curve(P *p, const Opts *o) {
  float c[128]; int i, last = 0;
  float lv = o->velset[0] ? o->velcurve[0] : 0;
  c[0] = lv;
  for (i = 1; i < 128; i++) {
    if (o->velset[i] || i == 127) {
      float v = o->velset[i] ? o->velcurve[i] : 1;
      int j;
      for (j = last; j <= i; j++) c[j] = lv + (v - lv) * (float)(j - last) / (float)(i - last);
      last = i; lv = v;
    }
  }
  if (p->nc && !memcmp(p->curves + (size_t)(p->nc - 1) * 128, c, sizeof c)) return p->nc - 1;
  if (!grow((void **)&p->curves, &p->capc, (p->nc + 1) * 128, sizeof(float))) { p->oom = 1; return -1; }
  memcpy(p->curves + (size_t)p->nc * 128, c, sizeof c);
  return p->nc++;
}

/* The region now complete in p->region: its sample, its generators (SF2 units; each conversion below) and
   conditions, as one region, or two for a stereo sample. The first pass only notes its sample file. */
static void finish_region(P *p) {
  const Opts *o = &p->region;
  File *fl; int fi, nch, ch, lm, uses_loop, nmod = 0, moff, curve = -1;
  int lokey, hikey, lovel, hivel;
  int64_t start, endx, ls_abs, le_abs;
  uint32_t at = 0, hf, rate;
  SmpSfRegion r;
  SmpSfMod m[12 + MAX_CCM];
  double db, fc_raw;
  if (p->collect) { if (o->sample[0]) sample_file(p, o->sample); return; }
  if (o->sw_default >= 0) p->swdef = o->sw_default;
  if (!o->sample[0] || p->too_many || p->oom) return;
  if (o->hikey < 0 || o->lokey > 127 || o->hivel < 0 || o->lovel > 127) return;   /* never plays */
  lokey = clampi(o->lokey, 0, 127); hikey = clampi(o->hikey, 0, 127);
  lovel = clampi(o->lovel, 0, 127); hivel = clampi(o->hivel, 0, 127);
  if (lokey > hikey || lovel > hivel) return;
  if ((fi = sample_file(p, o->sample)) < 0) return;
  fl = &p->files[fi];
  nch = fl->in.channels >= 2 ? 2 : 1;
  rate = fl->in.rate ? fl->in.rate : 44100;
  /* the cutoff in absolute cents, its key-tracking centre taken off (the filter's and the modulators' start) */
  fc_raw = o->cutoff > 0 ? 1200.0 * log2(o->cutoff / 8.176) - clampd(o->fil_keytrack, -255, 255) * o->fil_keycenter : 13500;

  /* Sample playback, in frames of the file. offset is the first frame; end the last one played (inclusive, as
     sfzformat.com has it), so the region plays [offset, end + 1), given as an absolute frame (cond.end_frame), so
     it holds even when the file's length is only known once it is decoded; loop_start and loop_end likewise
     (loop_end inclusive), defaulting to the file's own loop. loop_mode defaults to loop_continuous when the file
     has a loop, else no_loop (sfzformat.com): no_loop is sampleModes 0, loop_continuous 1, loop_sustain 3 (loop
     until release, then on to the end), and one_shot is sampleModes 0 with the one_shot condition (note-offs don't
     release it). */
  start = (int64_t)clampd(floor(o->offset + 0.5), 0, fl->in.frames ? fl->in.frames - 1 : 0);
  endx = o->end >= 0 ? (int64_t)clampd(floor(o->end + 0.5) + 1, 0, fl->in.frames) : (int64_t)fl->in.frames;
  if (endx <= start) return;
  ls_abs = o->loop_start >= 0 ? (int64_t)clampd(floor(o->loop_start + 0.5), 0, fl->in.frames) : fl->in.ls;
  le_abs = o->loop_end >= 0 ? (int64_t)clampd(floor(o->loop_end + 0.5) + 1, 0, fl->in.frames) : fl->in.le;
  if (o->loop_whole && o->loop_end < 0 && !(fl->in.le > fl->in.ls)) le_abs = fl->in.frames;
  lm = o->loop_mode >= 0 ? o->loop_mode : (fl->in.le > fl->in.ls ? LOOP_CONT : LOOP_NONE);
  uses_loop = (lm == LOOP_CONT || lm == LOOP_SUSTAIN) && le_abs > ls_abs;
  if (!uses_loop && lm != LOOP_ONESHOT) lm = LOOP_NONE;
  /* a start head when the offset is past half the head the loader keeps (see the top) */
  hf = rate / 4 > 1024 ? rate / 4 : 1024;
  if (start > 0 && !fl->gen && fl->in.frames >= rate && hf < fl->in.frames && start > hf / 2 && (!uses_loop || ls_abs >= start))
    at = (uint32_t)start;

  memset(&r, 0, sizeof r);
  sf_gen_defaults(r.gen);
  sf_cond_defaults(&r.cond);
  r.lokey = (uint8_t)lokey; r.hikey = (uint8_t)hikey; r.lovel = (uint8_t)lovel; r.hivel = (uint8_t)hivel;
  r.gen[43] = (int16_t)(lokey | hikey << 8);
  r.gen[44] = (int16_t)(lovel | hivel << 8);
  r.gen[G_MODES] = (int16_t)(lm == LOOP_CONT ? 1 : lm == LOOP_SUSTAIN ? 3 : 0);
  if (o->end >= 0) r.cond.end_frame = (uint32_t)(endx - at > 0 ? endx - at : 1);

  /* Pitch. pitch_keycenter is overridingRootKey (=sample leaves it unset, so the file's smpl unity note counts);
     pitch_keytrack (cents per key) is scaleTuning, -1200..1200 as SFZ allows (the voice takes it as it is, so a
     negative one plays lower going up); transpose (semitones) and tune (cents) together are coarseTune and
     fineTune. */
  r.gen[G_ROOTKEY] = (int16_t)(o->keycenter == -2 ? -1 : clampi(o->keycenter, 0, 127));
  r.gen[G_SCALE] = (int16_t)clampi((int)floor(o->keytrack + 0.5), -1200, 1200);
  {
    int cents = (int)clampd(floor(o->transpose * 100 + o->tune + 0.5), -12099, 12099);
    int semis = cents / 100;
    r.gen[G_COARSE] = (int16_t)clampi(semis, -120, 120);
    r.gen[G_FINE] = (int16_t)clampi(cents - semis * 100, -99, 99);
  }

  /* Level. volume (dB) plus amplitude (percent, as 20 log10(amplitude / 100) dB): at or below 0 dB it is
     initialAttenuation, whose centibels the voice takes times 0.4 (the EMU factor), so dB x -25, up to 144 dB;
     above 0 dB, which attenuation can't say, it is the condition gain 10^(dB / 20). amp_veltrack (percent, 100 by
     default) and amp_velcurve_N go to the conditions, which replace SF2's velocity-to-attenuation modulator. */
  db = o->volume + (o->amplitude >= 100 ? (o->amplitude == 100 ? 0 : 20 * log10(o->amplitude / 100)) :
                    o->amplitude <= 0 ? -144 : 20 * log10(o->amplitude / 100));
  db = clampd(db, -144, 24);
  if (db <= 0) r.gen[G_ATTEN] = (int16_t)clampi((int)floor(-db * 25 + 0.5), 0, 3600);
  else r.cond.gain = (float)pow(10.0, db / 20);
  r.cond.amp_veltrack = (float)clampd(o->veltrack, -100, 100);
  if (o->velcurve_n) curve = add_curve(p, o);

  /* The amplitude envelope (ampeg_*) is the volume envelope (generators 33 to 38), the filter's (fileg_*) or else
     the pitch's (pitcheg_*) the modulation envelope (25 to 30): SF2 has one modulation envelope, so when a region
     has both, fileg's times are used for both depths. fileg_depth is modEnvToFilterFc and pitcheg_depth
     modEnvToPitch, in cents. */
  env_gens(r.gen, G_DELAY_VOLENV, o->ampeg, 1);
  if (o->fileg_set) env_gens(r.gen, G_DELAY_MODENV, o->fileg, 0);
  else if (o->pitcheg_set) env_gens(r.gen, G_DELAY_MODENV, o->pitcheg, 0);
  if (o->fileg_set) r.gen[G_MODENV_FC] = (int16_t)clampi((int)floor(o->fileg_depth + 0.5), -12000, 12000);
  if (o->pitcheg_set) r.gen[G_MODENV_PITCH] = (int16_t)clampi((int)floor(o->pitcheg_depth + 0.5), -12000, 12000);

  /* The filter: only with a cutoff (Hz to absolute cents, 1500..13500 as SF2 allows) and a kind the voice has
     (lpf_2p, the default, hpf_2p, bpf_2p, brf_2p; others were counted as ignored and get no filter). resonance
     (dB) is initialFilterQ (cB): the peak's height above the passband, which stays at unity (cond.fil_unity, as
     sfizz and sforzando have it; SF2.04 lowers the passband instead). fil_veltrack (cents at full velocity) is a
     velocity-to-cutoff modulator; fil_keytrack (cents per key from fil_keycenter) a key-to-cutoff modulator, its
     centre taken off the cutoff. */
  if (o->cutoff > 0 && o->fil_type >= 0) {
    double kt = clampd(o->fil_keytrack, -255, 255);
    r.cond.fil_type = (uint8_t)o->fil_type;
    r.cond.fil_unity = 1;
    r.gen[G_FC] = (int16_t)clampi((int)floor(fc_raw + 0.5), 1500, 13500);
    r.gen[G_Q] = (int16_t)clampi((int)floor(o->resonance * 10 + 0.5), 0, 960);
  }

  /* LFOs. SF2 has a modulation LFO (to pitch, cutoff and volume) and a vibrato LFO (to pitch), both triangles
     (SFZ's are sines by default: the same depth and rate, a slightly different shape). lfo1_* (or, without it,
     amplfo_* and fillfo_*, sharing one rate: amplfo's when both) is the modulation LFO; lfo2_* (or pitchlfo_*) the
     vibrato LFO. Rates are Hz as absolute cents, delays seconds as timecents, pitch and cutoff depths cents, and the
     volume depth dB as centibels (modLfoToVolume). An LFO with no rate does nothing. */
  {
    const Lfo *a = o->lfo1_set ? &o->lfo1 : (o->amplfo.freq > 0 ? &o->amplfo : &o->fillfo);
    double vol = o->lfo1_set ? o->lfo1.volume : o->amplfo.freq > 0 ? o->amplfo.volume : 0;
    double fc = o->lfo1_set ? o->lfo1.cutoff : o->fillfo.freq > 0 ? o->fillfo.cutoff : 0;
    double pit = o->lfo1_set ? o->lfo1.pitch : 0;
    const Lfo *b = o->lfo2_set ? &o->lfo2 : &o->pitchlfo;
    if (a->freq > 0 && (vol != 0 || fc != 0 || pit != 0)) {
      r.gen[G_FREQ_MODLFO] = (int16_t)hz_cents(a->freq, -16000, 4500);
      r.gen[G_DELAY_MODLFO] = (int16_t)seconds_tc(a->delay, 5000);
      r.gen[G_MODLFO_VOL] = (int16_t)clampi((int)floor(vol * 10 + 0.5), -960, 960);
      r.gen[G_MODLFO_FC] = (int16_t)clampi((int)floor(fc + 0.5), -12000, 12000);
      r.gen[G_MODLFO_PITCH] = (int16_t)clampi((int)floor(pit + 0.5), -12000, 12000);
    }
    double bf = b->freq; int depth_cc = 0, k;
    for (k = 0; k < o->nccm; k++) {
      /* pitchlfo_freqccN: the rate at the controller's set_cc value (SF2's rate is in cents, so a controller can't
         move it in Hz; the instrument's starting value counts) */
      if (o->ccm[k].target == CM_VIBFREQ) bf += o->ccm[k].amount * (p->cc_init[o->ccm[k].cc] > 0 ? p->cc_init[o->ccm[k].cc] : 0) / 127.0;
      if (o->ccm[k].target == CM_VIBDEPTH && o->ccm[k].amount != 0) depth_cc = 1;
    }
    if (bf > 0 && (b->pitch != 0 || depth_cc)) {
      r.gen[G_FREQ_VIBLFO] = (int16_t)hz_cents(bf, -16000, 4500);
      r.gen[G_DELAY_VIBLFO] = (int16_t)seconds_tc(b->delay, 5000);
      r.gen[G_VIBLFO_PITCH] = (int16_t)clampi((int)floor(b->pitch + 0.5), -12000, 12000);
    }
  }

  /* Modulators: SF2's defaults for CC7 and CC11 (volume, expression), CC10 (pan) and CC91 and CC93 (the effect
     sends), and the pitch wheel; not velocity to cutoff (SFZ has fil_veltrack for that), nor the mod wheel and
     channel pressure to vibrato (SFZ has no default vibrato). The velocity-to-attenuation default isn't needed:
     amp_veltrack replaces it. With bend_up or bend_down given, the wheel's range is theirs instead of the
     channel's RPN 0: f(x) = A x + B |x| for the wheel at x in -1..1, with A = (up - down) / 2 and B = (up + down)
     / 2, so x = 1 gives bend_up and x = -1 gives bend_down (two modulators, the second with the absolute value
     transform). */
  {
    static const SmpSfMod base[5] = {
      {0x0587, 48, 960, 0, 0}, {0x028A, 17, 500, 0, 0}, {0x058B, 48, 960, 0, 0}, {0x00DB, 16, 200, 0, 0}, {0x00DD, 15, 200, 0, 0},
    };
    {                                           /* bound to a Decent Sampler knob: not its General MIDI default */
      int k;
      for (k = 0; k < 5; k++) {
        int cc = base[k].src & 0x7F;
        if ((p->unbound[cc >> 3] >> (cc & 7)) & 1) continue;
        if (cc == 91 && p->no_rev_send) continue;
        m[nmod++] = base[k];
      }
    }
    if (!o->bend_set) { SmpSfMod w = {0x020E, 59, 12700, 0x0010, 0}; m[nmod++] = w; }
    else {
      double up = clampd(o->bend_up, -9600, 9600), dn = clampd(o->bend_down, -9600, 9600);
      int A = (int)floor((up - dn) / 2 + 0.5), B = (int)floor((up + dn) / 2 + 0.5);
      SmpSfMod w = {0x020E, 59, (int16_t)A, 0, 0}, x = {0x020E, 59, (int16_t)B, 0, 2};
      m[nmod++] = w;
      if (B) m[nmod++] = x;
    }
    if (o->cutoff > 0 && o->fil_type >= 0) {
      /* A cutoff below SF2's floor (1500 cents, 20 Hz) that velocity opens (cutoff=10 fil_veltrack=20000, a common
         trick): the generator stays at the floor and the modulator takes what was cut off it, so full velocity
         opens exactly where SFZ puts it (sfizz's reading); softer notes open a little more than SFZ's straight line
         from 10 Hz would. */
      double vt = o->fil_veltrack;
      if (fc_raw < 1500 && vt > 0) vt = fc_raw + vt - 1500 > 0 ? fc_raw + vt - 1500 : 0;
      if (vt != 0) { SmpSfMod w = {0x0002, 8, (int16_t)clampi((int)floor(vt + 0.5), -32768, 32767), 0, 0}; m[nmod++] = w; }
      if (o->fil_keytrack != 0) { SmpSfMod w = {0x0003, 8, (int16_t)clampi((int)floor(clampd(o->fil_keytrack, -255, 255) * 128 + 0.5), -32768, 32767), 0, 0}; m[nmod++] = w; }
    }
    /* Controllers (gain_ccN and the rest): a linear modulator from the CC (0x0080 | N: unipolar, positive) to the
       generator. Level in dB is attenuation in cB (no EMU factor on modulators); pan percent is SF2's tenths;
       cutoff only with a filter. An envelope time in seconds can't be added in timecents, so the modulator's
       amount is the difference in timecents between the stage at the controller's 0 and at its 127 (exact at both
       ends, in between geometric); the sustain level likewise in cB. */
    {
      int k;
      for (k = 0; k < o->nccm; k++) {
        int cc = o->ccm[k].cc, tg = o->ccm[k].target, dest = -1;
        double a = o->ccm[k].amount, amt = 0;
        if (tg == CM_GAIN) { dest = G_ATTEN; amt = -a * 10; }
        else if (tg == CM_PITCH) { dest = 59; amt = a; }
        else if (tg == CM_CUTOFF) { if (o->cutoff > 0 && o->fil_type >= 0) { dest = 8; amt = a; } }
        else if (tg == CM_PAN) { dest = 17; amt = a * 5; }
        else if (tg == CM_VIBDEPTH) { dest = 6; amt = a; }
        else if (tg >= CM_AMPEG0 && tg < CM_AMPEG0 + 6) {
          int st = tg - CM_AMPEG0;
          dest = G_DELAY_VOLENV + st;
          if (st == 4) {
            double s0 = clampd(o->ampeg[4], 0.01, 100), s1 = clampd(o->ampeg[4] + a, 0.01, 100);
            amt = -200.0 * log10(s1 / 100.0) - -200.0 * log10(s0 / 100.0);
          } else {
            double t0 = o->ampeg[st] > 0.001 ? o->ampeg[st] : 0.001, t1 = o->ampeg[st] + a > 0.001 ? o->ampeg[st] + a : 0.001;
            amt = 1200.0 * log2(t1 / t0);
          }
        }
        if (dest < 0 || amt == 0) continue;
        { SmpSfMod w = {(uint16_t)(0x0080 | cc), (uint16_t)dest, (int16_t)clampi((int)floor(amt + 0.5), -32768, 32767), 0, 0}; m[nmod++] = w; }
      }
    }
    if ((moff = add_mods(p, m, nmod)) < 0) return;
  }

  /* Conditions (SmpSfCond, as Task 8's player reads them). seq_position defaults to 1 when seq_length is given;
     a keyswitch or key-crossfade range given at one end only gets SFZ's default for the other (sw_lokey 0,
     sw_hikey 127, xfin_lokey and xfin_hikey 0, xfout_lokey and xfout_hikey 127); sw_default is the instrument's,
     set on every region at the end. off_mode time is taken as normal (the region's own release). */
  {
    SmpSfCond *c = &r.cond;
    int i;
    c->trigger = (uint8_t)o->trigger;
    if (o->seq_length > 0) {
      c->seq_len = (uint16_t)clampi(o->seq_length, 1, 65535);
      c->seq_pos = (uint16_t)(o->seq_position > 0 ? clampi(o->seq_position, 1, 65535) : 1);
    }
    c->lorand = (float)clampd(o->lorand, 0, 1); c->hirand = (float)clampd(o->hirand, 0, 1);
    if (o->sw_lokey >= 0 || o->sw_hikey >= 0) {
      c->sw_lokey = (int8_t)(o->sw_lokey >= 0 ? o->sw_lokey : 0);
      c->sw_hikey = (int8_t)(o->sw_hikey >= 0 ? o->sw_hikey : 127);
    }
    c->sw_last = (int8_t)o->sw_last; c->sw_down = (int8_t)o->sw_down; c->sw_up = (int8_t)o->sw_up;
    c->sw_previous = (int8_t)o->sw_previous;
    c->locc_n = (uint8_t)o->ncc;
    for (i = 0; i < o->ncc; i++) { c->locc[i].cc = o->cc[i]; c->locc[i].lo = o->cclo[i]; c->locc[i].hi = o->cchi[i]; }
    if (o->xfin_lokey >= 0 || o->xfin_hikey >= 0) {
      c->xfin_lokey = (int8_t)(o->xfin_lokey >= 0 ? o->xfin_lokey : 0);
      c->xfin_hikey = (int8_t)(o->xfin_hikey >= 0 ? o->xfin_hikey : 0);
    }
    if (o->xfout_lokey >= 0 || o->xfout_hikey >= 0) {
      c->xfout_lokey = (int8_t)(o->xfout_lokey >= 0 ? o->xfout_lokey : 127);
      c->xfout_hikey = (int8_t)(o->xfout_hikey >= 0 ? o->xfout_hikey : 127);
    }
    c->xfin_lovel = (uint8_t)clampi(o->xfin_lovel, 0, 127); c->xfin_hivel = (uint8_t)clampi(o->xfin_hivel, 0, 127);
    c->xfout_lovel = (uint8_t)clampi(o->xfout_lovel, 0, 127); c->xfout_hivel = (uint8_t)clampi(o->xfout_hivel, 0, 127);
    c->xf_curve = (uint8_t)o->xf_curve;
    {                                           /* SFZ's defaults for an end not given: xfin_ 0, xfout_ 127 */
      int i2;
      for (i2 = 0; i2 < o->nxfcc && i2 < 4; i2++) {
        int d0 = o->xfcc[i2].out ? 127 : 0;
        c->xfcc[i2].cc = o->xfcc[i2].cc; c->xfcc[i2].out = o->xfcc[i2].out;
        c->xfcc[i2].lo = (uint8_t)(o->xfcc[i2].lo >= 0 ? o->xfcc[i2].lo : d0);
        c->xfcc[i2].hi = (uint8_t)(o->xfcc[i2].hi >= 0 ? o->xfcc[i2].hi : d0);
      }
      c->xfcc_n = (uint8_t)(o->nxfcc < 4 ? o->nxfcc : 4);
      c->xf_cccurve = (uint8_t)o->xf_cccurve;
    }
    c->group = o->group; c->off_by = o->off_by; c->off_mode = (uint8_t)o->off_mode;
    c->rt_decay = (float)clampd(o->rt_decay, 0, 200);
    c->polyphony = (uint16_t)clampi(o->polyphony, 0, 65535);
    c->note_polyphony = (uint16_t)clampi(o->note_polyphony, 0, 65535);
    c->one_shot = (uint8_t)(lm == LOOP_ONESHOT);
    c->delay = (float)clampd(o->delay, 0, 100); c->delay_random = (float)clampd(o->delay_random, 0, 100);
    c->amp_random = (float)clampd(o->amp_random, -96, 96); c->pitch_random = (float)clampd(o->pitch_random, 0, 9600);
    c->amp_keytrack = (float)clampd(o->amp_keytrack, -96, 12); c->amp_keycenter = (uint8_t)clampi(o->amp_keycenter, 0, 127);
    c->layer = (uint16_t)o->layer;
  }

  /* one region a channel: the sample from frame `at`, the address offsets relative to that sample (fine plus
     coarse, as the voice reads them), and the channel's pan; the second is the pair's follower */
  for (ch = 0; ch < nch; ch++) {
    int sid = get_sample(p, fl, ch, at);
    const SmpSfSample *sh;
    Reg *g;
    double pan;
    if (sid < 0) return;
    if (p->nr >= SF_MAX_REGIONS) { p->too_many = 1; return; }
    if (!grow((void **)&p->regs, &p->capr, p->nr + 1, sizeof *p->regs)) { p->oom = 1; return; }
    sh = &p->shdr[sid];
    g = &p->regs[p->nr++];
    g->r = r; g->mod_off = moff; g->nmod = nmod; g->curve = curve;
    g->r.sample = sid;
    g->r.cond.pair = (uint8_t)(ch == 1);
    g->r.gen[53] = (int16_t)(sid < 32767 ? sid : 32767);
    addr(g->r.gen, G_START, G_STARTCOARSE, start - at);
    if (uses_loop) {
      addr(g->r.gen, G_LOOPSTART, G_LOOPSTARTCOARSE, ls_abs - at - sh->loop_start);
      addr(g->r.gen, G_LOOPEND, G_LOOPENDCOARSE, le_abs - at - sh->loop_end);
    }
    /* pan (-100..100) is SF2 pan x 5; a stereo pair's channels sit width (percent) either side of it */
    pan = nch == 1 ? o->pan : ch == 0 ? o->pan - o->width : o->pan + o->width;
    g->r.gen[G_PAN] = (int16_t)clampi((int)floor(pan * 5 + 0.5), -500, 500);
  }
}

/* ---- the text ---- */

static void header(P *p, const char *s, size_t n) {
  char h[40]; size_t i;
  while (n && ws(*s)) { s++; n--; }
  while (n && ws(s[n - 1])) n--;
  if (n >= sizeof h) n = sizeof h - 1;
  for (i = 0; i < n; i++) h[i] = lower(s[i]);
  h[n] = 0;
  if (p->level == LV_REGION) finish_region(p);
  if (!strcmp(h, "control")) p->level = LV_CONTROL;
  else if (!strcmp(h, "global")) { opts_default(&p->global); p->master = p->global; p->group = p->global; p->level = LV_GLOBAL; }
  else if (!strcmp(h, "master")) { p->master = p->global; p->group = p->master; p->level = LV_MASTER; }
  else if (!strcmp(h, "group")) { p->group = p->master; p->level = LV_GROUP; }
  else if (!strcmp(h, "region")) { p->region = p->group; p->level = LV_REGION; }
  else if (!strcmp(h, "curve") || !strcmp(h, "effect") || !strcmp(h, "midi")) p->level = LV_OTHER;
  else { char t[48]; snprintf(t, sizeof t, "<%s>", h); ignored(p, t); p->level = LV_OTHER; }
}

static void opcode_at(P *p, const char *name, size_t nn, const char *v, size_t vn) {
  char nm[MAX_NAME]; size_t i;
  if (nn >= sizeof nm) nn = sizeof nm - 1;
  for (i = 0; i < nn; i++) nm[i] = lower(name[i]);
  nm[nn] = 0;
  if (vn > MAX_LINE) vn = MAX_LINE;
  memcpy(p->val, v, vn); p->val[vn] = 0;
  for (i = 0; i < vn; i++) if (!p->val[i]) p->val[i] = ' ';
  switch (p->level) {
    case LV_CONTROL: control_opcode(p, nm, p->val); break;
    /* the levels below were copied at the header and have nothing of their own yet: a region straight after
       <global> or <master> (no <group> between) inherits what they set */
    case LV_GLOBAL: opcode(p, &p->global, nm, p->val); p->master = p->global; p->group = p->global; break;
    case LV_MASTER: opcode(p, &p->master, nm, p->val); p->group = p->master; break;
    case LV_GROUP: opcode(p, &p->group, nm, p->val); break;
    case LV_REGION: opcode(p, &p->region, nm, p->val); break;
    default: break;                  /* before any header, or in <curve>, <effect>, <midi>: nothing to do */
  }
}

/* the defines' slot for a name of n bytes (with its $): its index, or where it would go (a negative -1 - slot) */
static int def_find(P *p, const char *s, size_t n) {
  unsigned i = (unsigned)fnv(s, n) & (DEF_SLOTS - 1); int k;
  for (k = 0; k < DEF_SLOTS; k++, i = (i + 1) & (DEF_SLOTS - 1)) {
    int d = p->def_slot[i];
    if (d < 0) return -1 - (int)i;
    if (strlen(p->def_name[d]) == n && !memcmp(p->def_name[d], s, n)) return d;
  }
  return -1 - DEF_SLOTS;
}

/* s (n bytes) with every known $NAME replaced by its value (the longest name that is defined), into out; returns
   the length (at most cap) */
static size_t expand(P *p, const char *s, size_t n, char *out, size_t cap) {
  size_t i = 0, o = 0; int reps = 0;
  while (i < n && o < cap) {
    if (s[i] == '$' && reps < MAX_EXPANSIONS) {
      size_t k = i + 1, L;
      while (k < n && namech(s[k]) && s[k] != '$' && k - i < 64) k++;
      for (L = k - i; L >= 2; L--) {
        int d = def_find(p, s + i, L);
        if (d >= 0) {
          size_t vl = strlen(p->def_val[d]);
          if (vl > cap - o) vl = cap - o;
          memcpy(out + o, p->def_val[d], vl); o += vl; i += L; reps++;
          break;
        }
      }
      if (L >= 2) continue;
    }
    out[o++] = s[i++];
  }
  return o;
}

static void define(P *p, const char *s, size_t n) {
  size_t a = 0, b, vs, ve;
  int d; char *val;
  while (a < n && ws(s[a])) a++;
  if (a >= n || s[a] != '$') return;
  for (b = a + 1; b < n && namech(s[b]) && s[b] != '$'; b++) {}
  if (b - a < 2 || b - a > 63) return;
  for (vs = b; vs < n && ws(s[vs]); vs++) {}
  for (ve = n; ve > vs && ws(s[ve - 1]); ve--) {}
  ve = vs + expand(p, s + vs, ve - vs, p->tmp, MAX_DEF_VALUE);
  if (!(val = malloc(ve - vs + 1))) { p->oom = 1; return; }
  memcpy(val, p->tmp, ve - vs); val[ve - vs] = 0;
  d = def_find(p, s + a, b - a);
  if (d >= 0) { free(p->def_val[d]); p->def_val[d] = val; return; }
  if (d == -1 - DEF_SLOTS || p->ndef >= MAX_DEFINES) { free(val); return; }
  if (!(p->def_name[p->ndef] = malloc(b - a + 1))) { free(val); p->oom = 1; return; }
  memcpy(p->def_name[p->ndef], s + a, b - a); p->def_name[p->ndef][b - a] = 0;
  p->def_val[p->ndef] = val;
  p->def_slot[-1 - d] = p->ndef++;
}

static int process_file(P *p, const wchar_t *wpath, int depth, char *why, int wcap);

static void include(P *p, const char *s, size_t n, const wchar_t *dir, int depth) {
  char name[MAX_SPATH]; wchar_t w[WPATH], rel[WPATH], full[WPATH];
  size_t a = 0, b, k; char why[8]; DWORD fn;
  while (a < n && ws(s[a])) a++;
  if (a < n && s[a] == '"') { a++; for (b = a; b < n && s[b] != '"'; b++) {} }
  else for (b = a; b < n && !ws(s[b]); b++) {}
  if (b <= a || b - a >= sizeof name) return;
  memcpy(name, s + a, b - a); name[b - a] = 0;
  for (k = 0; name[k]; k++) if (name[k] == '/') name[k] = '\\';
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, rel, WPATH) &&
      !MultiByteToWideChar(CP_ACP, 0, name, -1, rel, WPATH)) return;
  if ((rel[0] && rel[1] == L':') || rel[0] == L'\\') { if (wcslen(rel) >= WPATH) return; wcscpy(w, rel); }
  else {
    /* the root .sfz's folder first, as sforzando and sfizz look (Data\group.txt including "Data/region.txt"),
       then the including file's own folder */
    wchar_t root[WPATH], *sl; DWORD at;
    wcsncpy(root, p->stack[0], WPATH - 1); root[WPATH - 1] = 0;
    sl = wcsrchr(root, L'\\');
    if (sl) sl[1] = 0; else root[0] = 0;
    if (_snwprintf(w, WPATH - 1, L"%s%s", root, rel) < 0) return;   /* too long: never cut short */
    w[WPATH - 1] = 0;
    at = GetFileAttributesW(w);
    if ((at == INVALID_FILE_ATTRIBUTES || (at & FILE_ATTRIBUTE_DIRECTORY)) &&
        _snwprintf(w, WPATH - 1, L"%s%s", dir, rel) < 0) return;
  }
  w[WPATH - 1] = 0;
  fn = GetFullPathNameW(w, WPATH, full, NULL);
  if (!fn || fn >= WPATH) return;
  process_file(p, full, depth + 1, why, (int)sizeof why);   /* a missing or bad include is skipped */
}

/* 7 or 8 when a #define or #include starts s (n bytes), not followed by more of a name; else 0 */
static size_t directive_at(const char *s, size_t n) {
  if (n >= 7 && !memcmp(s, "#define", 7) && (n == 7 || !namech(s[7]))) return 7;
  if (n >= 8 && !memcmp(s, "#include", 8) && (n == 8 || !namech(s[8]))) return 8;
  return 0;
}

/* where a value starting at i ends: a header, or a space before the next name=, header or directive, or the line's
   end (trailing spaces left for the caller to trim) */
static size_t value_end(const char *s, size_t n, size_t i) {
  while (i < n) {
    if (s[i] == '<') break;
    if (ws(s[i])) {
      size_t j = i, k;
      while (j < n && ws(s[j])) j++;
      if (j >= n || s[j] == '<' || directive_at(s + j, n - j)) break;
      for (k = j; k < n && namech(s[k]); k++) {}
      if (k > j && k < n && s[k] == '=') break;
      i = j;
      continue;
    }
    i++;
  }
  return i;
}

static void do_text(P *p, const char *s, size_t n);

/* one line (n bytes, no newline). A #define or #include may start the line or any word in it (sforzando and sfizz
   read '<region> #define $KEY 60 lokey=59 #include "x.txt"'), so the line is taken in order: the text before a
   directive, the directive, then the rest, which sees what the directive defined or read. A define's value ends
   where an opcode's would, so it may hold spaces; a # that starts no directive stays in a value (sample=a #1.wav). */
static void do_line(P *p, const char *s, size_t n, const wchar_t *dir, int depth) {
  size_t i = 0;
  while (i < n && !p->oom) {
    size_t d = i, L = 0;
    for (; d < n; d++) if (s[d] == '#' && (d == 0 || ws(s[d - 1])) && (L = directive_at(s + d, n - d))) break;
    if (d > i) do_text(p, s + i, d - i);
    if (d >= n) break;
    if (L == 7) {                                   /* #define $NAME value */
      size_t a = d + 7, e;
      while (a < n && ws(s[a])) a++;
      if (a < n && s[a] == '$') for (a++; a < n && namech(s[a]) && s[a] != '$'; a++) {}
      while (a < n && ws(s[a])) a++;
      if (a < n && s[a] == '<') for (e = a; e < n && !ws(s[e]); e++) {}   /* a header as the value: one word */
      else e = value_end(s, n, a);
      define(p, s + d + 7, e - d - 7);
      i = e;
    } else {                                        /* #include "file" (or a bare name, to the next space) */
      size_t a = d + 8, b;
      while (a < n && ws(s[a])) a++;
      if (a < n && s[a] == '"') { for (b = a + 1; b < n && s[b] != '"'; b++) {} if (b < n) b++; }
      else for (b = a; b < n && !ws(s[b]); b++) {}
      if (p->ndef) include(p, p->line, expand(p, s + a, b - a, p->line, MAX_LINE), dir, depth);
      else include(p, s + a, b - a, dir, depth);
      i = b;
    }
  }
}

/* text with no directive in it: headers and opcodes, after the defines are replaced */
static void do_text(P *p, const char *s, size_t n) {
  size_t i;
  if (p->ndef && memchr(s, '$', n)) { n = expand(p, s, n, p->line, MAX_LINE); s = p->line; }
  i = 0;
  while (i < n && !p->oom) {
    size_t a, nn, vs, ve;
    while (i < n && ws(s[i])) i++;
    if (i >= n) break;
    if (s[i] == '<') {
      size_t j = i + 1;
      while (j < n && s[j] != '>' && s[j] != '<' && j - i < 40) j++;
      if (j < n && s[j] == '>') { header(p, s + i + 1, j - i - 1); i = j + 1; }
      else i = j > i + 1 ? j : i + 1;
      continue;
    }
    a = i;
    while (i < n && !ws(s[i]) && s[i] != '=' && s[i] != '<') i++;
    if (i >= n || s[i] != '=') continue;            /* not name=value: skipped */
    nn = i - a;
    vs = ++i;
    i = value_end(s, n, i);                         /* the value runs to the next name= or header */
    for (ve = i; ve > vs && ws(s[ve - 1]); ve--) {}
    if (nn) opcode_at(p, s + a, nn, s + vs, ve - vs);
  }
}

/* comments blanked in place (// to the line's end, block comments), newlines kept */
static void strip_comments(char *t, size_t n) {
  size_t i = 0;
  while (i < n) {
    if (t[i] == '/' && i + 1 < n && t[i + 1] == '/') {
      while (i < n && t[i] != '\n') t[i++] = ' ';
    } else if (t[i] == '/' && i + 1 < n && t[i + 1] == '*') {
      t[i++] = ' '; t[i++] = ' ';
      while (i < n && !(t[i] == '*' && i + 1 < n && t[i + 1] == '/')) { if (t[i] != '\n') t[i] = ' '; i++; }
      if (i < n) { t[i++] = ' '; t[i++] = ' '; }
    } else i++;
  }
}

/* reads and processes one file (the root at depth 0); 0 with why when the root can't be read */
static int process_file(P *p, const wchar_t *wpath, int depth, char *why, int wcap) {
  HANDLE h; LARGE_INTEGER sz; DWORD got = 0; char *t; size_t n, i, a, lines = 0;
  wchar_t dir[WPATH], *slash;
  int k;
  if (depth > MAX_DEPTH || (depth > 0 && p->nincludes >= MAX_INCLUDES) || cancelled(p)) return 0;
  for (k = 0; k < depth; k++) if (CompareStringOrdinal(p->stack[k], -1, wpath, -1, TRUE) == CSTR_EQUAL) return 0;   /* a cycle */
  if (depth > 0) p->nincludes++;
  h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) { snprintf(why, (size_t)wcap, "it can't be read"); return 0; }
  if (!GetFileSizeEx(h, &sz) || sz.QuadPart > MAX_TEXT || p->text_bytes + (uint64_t)sz.QuadPart > MAX_ALL_TEXT) {
    CloseHandle(h);
    snprintf(why, (size_t)wcap, "it's too big to be an SFZ file");
    return 0;
  }
  n = (size_t)sz.QuadPart;
  if (!(t = malloc(n + 1))) { CloseHandle(h); p->oom = 1; snprintf(why, (size_t)wcap, "out of memory"); return 0; }
  if (n && (!ReadFile(h, t, (DWORD)n, &got, NULL) || got != n)) { CloseHandle(h); free(t); snprintf(why, (size_t)wcap, "it can't be read"); return 0; }
  CloseHandle(h);
  p->text_bytes += n;
  t[n] = 0;
  wcsncpy(dir, wpath, WPATH - 1); dir[WPATH - 1] = 0;
  slash = wcsrchr(dir, L'\\');
  if (slash) slash[1] = 0; else dir[0] = 0;
  p->stack[depth] = wpath;
  strip_comments(t, n);
  for (a = i = 0; i <= n && !p->oom; i++) {
    if (i == n || t[i] == '\n') {
      do_line(p, t + a, i - a, dir, depth);
      a = i + 1;
      if ((++lines & 0xFFF) == 0 && p->cancel && cancelled(p)) break;   /* every 4096 lines */
    }
  }
  free(t);
  return 1;
}

/* ---- the whole file ---- */

int sf_is_sfz(const char *path, const uint8_t *head, size_t n) {
  static const char *hdr[] = {"<region>", "<group>", "<control>", "<global>", "<master>"};
  const char *dot = path ? strrchr(path, '.') : NULL;
  size_t i, k, a = 0;
  /* XML (a Decent Sampler preset, say) is never SFZ, whatever its name */
  if (n >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF) a = 3;   /* a UTF-8 byte order mark */
  while (a < n && (head[a] == ' ' || head[a] == '\t' || head[a] == '\r' || head[a] == '\n')) a++;
  if (n - a >= 5 && !memcmp(head + a, "<?xml", 5)) return 0;
  for (i = 0; i + 14 <= n; i++) if (!memcmp(head + i, "<DecentSampler", 14)) return 0;
  if (dot && !_stricmp(dot, ".sfz")) return 1;
  for (k = 0; k < sizeof hdr / sizeof *hdr; k++) {
    size_t L = strlen(hdr[k]);
    for (i = 0; i + L <= n; i++) if (!memcmp(head + i, hdr[k], L)) return 1;
  }
  return 0;
}

/* back to the state at the text's start, for the second pass (the files and the defines' table stay) */
static void restart_text(P *p) {
  int i;
  for (i = 0; i < p->ndef; i++) { free(p->def_name[i]); free(p->def_val[i]); p->def_name[i] = p->def_val[i] = NULL; }
  p->ndef = 0;
  for (i = 0; i < DEF_SLOTS; i++) p->def_slot[i] = -1;
  opts_default(&p->global); p->master = p->group = p->region = p->global;
  p->level = LV_NONE;
  p->default_path[0] = 0; p->note_offset = p->octave_offset = 0;
  for (i = 0; i < 128; i++) p->cc_init[i] = -1;
  p->nincludes = 0; p->text_bytes = 0;
  p->swdef = -1;
  memset(p->unbound, 0, sizeof p->unbound); p->no_rev_send = 0;
}

static void free_parser(P *p) {
  int i;
  for (i = 0; i < p->ndef; i++) { free(p->def_name[i]); free(p->def_val[i]); }
  free(p->def_name); free(p->def_val); free(p->def_slot);
  free(p->line); free(p->val); free(p->tmp);
  strset_free(&p->ign); strset_free(&p->badpath);
  for (i = 0; i < p->nfiles; i++) {
    Head *h = p->files[i].heads;
    while (h) { Head *nx = h->next; free(h); h = nx; }
    free(p->files[i].wpath); free(p->files[i].lw); free(p->files[i].as_written);
  }
  free(p->files); free(p->fslot);
  for (i = 0; i < p->ncache; i++) free(p->cache[i].lw);
  free(p->cache); free(p->cslot);
  if (p->paths) for (i = 0; i < p->npaths; i++) free(p->paths[i]);
  free(p->paths); free(p->shdr); free(p->src); free(p->regs); free(p->mods); free(p->curves); free(p->pre);
  if (p->pk) for (i = 0; i < p->cappk; i++) sf_knobs_free(p->pk[i]);
  free(p->pk);
}

/* ---- the builder other formats share (sfds.c, sfexs.c): they feed headers and opcodes as SFZ names and values ---- */

void sfz_header(SfzBuild *p, const char *name) { header(p, name, strlen(name)); }
void sfz_opcode(SfzBuild *p, const char *name, const char *value) { opcode_at(p, name, strlen(name), value, strlen(value)); }
void sfz_ignored(SfzBuild *p, const char *name) { ignored(p, name); }
int sfz_cancelled(SfzBuild *p) { return cancelled(p); }

/* the current preset's knobs (Decent Sampler): taken (1) in the second pass only, freed with the font; 0 in the
   first, when the caller keeps it */
int sfz_set_knobs(SfzBuild *p, SfKnobs *k) {
  int at = p->npre ? p->npre - 1 : 0, old = p->cappk;
  if (p->collect || !k) return 0;
  if (!grow((void **)&p->pk, &p->cappk, at + 1, sizeof *p->pk)) { p->oom = 1; return 0; }
  if (p->cappk > old) memset(p->pk + old, 0, (size_t)(p->cappk - old) * sizeof *p->pk);
  sf_knobs_free(p->pk[at]);
  p->pk[at] = k;
  return 1;
}
void sfz_loop_whole(SfzBuild *p) { if (p->level == LV_REGION) p->region.loop_whole = 1; }

/* a new preset from here on (its regions are the ones fed after this call): the levels start again from SFZ's
   defaults; the files, the defines and the instrument-wide set_cc stay. Called the same way in both passes. */
void sfz_preset(SfzBuild *p, const char *name) {
  size_t L = strlen(name);
  if (p->level == LV_REGION) finish_region(p);
  opts_default(&p->global); p->master = p->group = p->region = p->global;
  p->level = LV_NONE;
  p->default_path[0] = 0; p->note_offset = p->octave_offset = 0;
  memset(p->unbound, 0, sizeof p->unbound); p->no_rev_send = 0;
  if (p->collect) return;
  if (!grow((void **)&p->pre, &p->cappre, p->npre + 1, sizeof *p->pre)) { p->oom = 1; return; }
  if (L > 20) L = 20;
  p->pre[p->npre].start = p->nr;
  memcpy(p->pre[p->npre].name, name, L); p->pre[p->npre].name[L] = 0;
  p->npre++;
}

/* an SFZ file's feed: its text, with its includes */
static int feed_sfz(SfzBuild *p, void *ctx, const wchar_t *full, char *why, int wcap) {
  (void)ctx;
  return process_file(p, full, 0, why, wcap);
}

/* the calling thread's open progress for the next sfz_build (smp_sf_parse_detached_ex), or NULL */
static __declspec(thread) volatile LONG *t_open_progress;
void sf_set_open_progress(volatile LONG *prog) { t_open_progress = prog; }
volatile LONG *sf_get_open_progress(void) { return t_open_progress; }

int sf_parse_sfz(SmpFont *f, const char *path, int (*cancel)(void *), void *carg, char *why, int wcap) {
  return sfz_build(f, path, feed_sfz, NULL, cancel, carg, why, wcap);
}

int sfz_build(SmpFont *f, const char *path, SfzFeed feed, void *ctx, int (*cancel)(void *), void *carg, char *why,
              int wcap) {
  P *p = calloc(1, sizeof *p);
  wchar_t *full = NULL;
  int ok = 0, i;
  char problem[sizeof f->problem];
  if (!p) { snprintf(why, (size_t)wcap, "out of memory"); return 0; }
  p->f = f;
  p->root_name = base_name(path);
  p->cancel = cancel; p->carg = carg;
  InitializeSRWLock(&p->dead_lock);
  for (i = 0; i < 4; i++) p->made[i] = -1;
  p->def_name = calloc(MAX_DEFINES, sizeof(char *)); p->def_val = calloc(MAX_DEFINES, sizeof(char *));
  p->def_slot = malloc(DEF_SLOTS * sizeof(int));
  p->line = malloc(MAX_LINE + 1); p->val = malloc(MAX_LINE + 1); p->tmp = malloc(MAX_LINE + 1);
  full = sf_full_path(path);
  if (!p->def_name || !p->def_val || !p->def_slot || !p->line || !p->val || !p->tmp || !full) {
    snprintf(why, (size_t)wcap, "out of memory");
    goto done;
  }
  restart_text(p);
  {
    wchar_t *slash;
    wcsncpy(p->root_dir, full, WPATH - 1); p->root_dir[WPATH - 1] = 0;
    slash = wcsrchr(p->root_dir, L'\\');
    if (slash) slash[1] = 0; else p->root_dir[0] = 0;
  }
  if (add_path(p, path) != 0) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }   /* paths[0]: the .sfz */

  /* the first pass collects the files, which are probed (the probe cache first) before the second builds */
  p->collect = 1;
  if (!feed(p, ctx, full, why, wcap)) goto done;
  if (p->level == LV_REGION) finish_region(p);
  InterlockedExchange(&g_wrote, 0); g_pcache_path[0] = 0;
  pcache_read(p, full);
  p->prog = t_open_progress;
  if (cancel) {                                 /* a background open (only it has a cancel): prefill, within budget */
    SmpSettings st; char cwhy[8];
    smp_settings_get(&st);
    if (st.cache_limit_gb > 0 && smp_cache_ok(cwhy, sizeof cwhy)) {
      p->pf_left = (LONG64)(st.cache_limit_gb * 1024.0 * 1024.0 * 1024.0 / 2);
      p->prefill = 1;
    }
  }
  probe_all(p);
  if (p->pf_wrote) {                            /* the cache keeps to its limit: one folder scan for them all */
    SmpSettings st;
    smp_settings_get(&st);
    smp_cache_trim((uint64_t)(st.cache_limit_gb * 1024.0 * 1024.0 * 1024.0));
  }
  {
    int k, cached = 0;
    for (k = 0; k < p->nfiles; k++) cached += p->files[k].from_cache;
    InterlockedExchange(&g_probed, p->probed); InterlockedExchange(&g_cached, cached);
  }
  if (cancelled(p)) { snprintf(why, (size_t)wcap, "cancelled"); goto done; }
  restart_text(p);
  p->collect = 0;
  if (!feed(p, ctx, full, why, wcap)) goto done;
  if (p->level == LV_REGION) finish_region(p);
  if (p->too_many) ignored(p, "too many regions (some left out)");   /* a partly loaded instrument says so */
  if (cancelled(p)) { snprintf(why, (size_t)wcap, "cancelled"); goto done; }
  if (p->oom) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
  pcache_write(p, full);

  /* what was missing or unreadable, for the font's problem (or the whole reason when nothing is left) */
  problem[0] = 0;
  if (p->missing)
    snprintf(problem, sizeof problem, "Missing %d sample%s (first: %s)", p->missing, p->missing == 1 ? "" : "s", p->first_missing);
  if (p->unreadable) {
    size_t L = strlen(problem);
    snprintf(problem + L, sizeof problem - L, "%s%d sample%s can't be read (first: %s)", L ? "; " : "",
             p->unreadable, p->unreadable == 1 ? "" : "s", p->first_unreadable);
  }
  if (!p->nr) {
    if (problem[0]) { snprintf(why, (size_t)wcap, "can't play %s: %s", p->root_name, problem); ok = -1; }
    else snprintf(why, (size_t)wcap, "it has no regions with samples");
    goto done;
  }

  /* the model: the presets (one unless the feed called sfz_preset) owning the regions in order, the samples, and
     the one block of regions, preset starts,
     modulator lists and velocity curves */
  if (p->swdef >= 0) for (i = 0; i < p->nr; i++) p->regs[i].r.cond.sw_default = (int8_t)p->swdef;
  for (i = 0; i < 128; i++) { f->cc_init[i] = p->cc_init[i]; if (p->cc_init[i] >= 0) f->has_cc_init = 1; }
  /* No send to the shared reverb until the music asks (CC91): SFZ players (sfizz, sforzando) have no reverb of
     their own, so GM's default send of 40 put a reverb on every instrument that its author never heard */
  if (f->cc_init[91] < 0) { f->cc_init[91] = 0; f->has_cc_init = 1; }
  f->version = 2;
  if (!p->npre) {                               /* one preset, named after the file */
    const char *b = p->root_name, *dot = strrchr(b, '.');
    size_t L = dot && dot > b ? (size_t)(dot - b) : strlen(b);
    if (!grow((void **)&p->pre, &p->cappre, 1, sizeof *p->pre)) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
    if (L > 20) L = 20;
    p->pre[0].start = 0; memcpy(p->pre[0].name, b, L); p->pre[0].name[L] = 0;
    p->npre = 1;
  }
  f->phdr = calloc((size_t)p->npre + 1, sizeof *f->phdr);
  f->order = calloc((size_t)p->npre, sizeof *f->order);
  f->shdr = calloc((size_t)p->ns + 1, sizeof *f->shdr);
  f->src = calloc((size_t)p->ns, sizeof *f->src);
  if (!f->phdr || !f->order || !f->shdr || !f->src) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
  for (i = 0; i < p->npre; i++) {               /* bank 0, programs in the order given */
    memcpy(f->phdr[i].name, p->pre[i].name, sizeof f->phdr[i].name);
    f->phdr[i].program = (uint16_t)i;
  }
  f->nphdr = p->npre;
  memcpy(f->shdr, p->shdr, (size_t)p->ns * sizeof *f->shdr);
  memcpy(f->src, p->src, (size_t)p->ns * sizeof *f->src);
  f->nshdr = p->ns;
  f->paths = p->paths; f->npaths = p->npaths; p->paths = NULL; p->npaths = 0;
  {
    size_t sreg = (size_t)p->nr * sizeof(SmpSfRegion), sst = ((size_t)p->npre + 1) * sizeof(int);
    size_t smod = (size_t)p->nm * sizeof(SmpSfMod), mod_at = (sreg + sst + 7) & ~(size_t)7;
    size_t cur_at = (mod_at + smod + 7) & ~(size_t)7, total = cur_at + (size_t)p->nc * 128 * sizeof(float);
    char *block = calloc(1, total ? total : 1);
    SmpSfMod *mods; float *curves;
    if (!block) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
    f->regions = (SmpSfRegion *)block;
    f->rstart = (int *)(block + sreg);
    mods = (SmpSfMod *)(block + mod_at);
    curves = (float *)(block + cur_at);
    if (smod) memcpy(mods, p->mods, smod);
    if (p->nc) memcpy(curves, p->curves, (size_t)p->nc * 128 * sizeof(float));
    for (i = 0; i < p->nr; i++) {
      SmpSfRegion *r = &f->regions[i];
      *r = p->regs[i].r;
      r->imod = mods + p->regs[i].mod_off; r->nimod = p->regs[i].nmod;
      r->pmod = NULL; r->npmod = 0;
      r->cond.amp_velcurve = p->regs[i].curve >= 0 ? curves + (size_t)p->regs[i].curve * 128 : NULL;
    }
    for (i = 0; i < p->npre; i++) f->rstart[i] = p->pre[i].start;
    f->rstart[p->npre] = p->nr;
    f->nregions = p->nr;
  }
  if (p->pk) {                                  /* Decent Sampler's knobs: the font owns them from here */
    int any = 0;
    for (i = 0; i < p->npre && i < p->cappk; i++) any |= p->pk[i] != NULL;
    if (any) {
      if (!(f->knobs = calloc((size_t)p->npre, sizeof *f->knobs))) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
      for (i = 0; i < p->npre && i < p->cappk; i++) { f->knobs[i] = p->pk[i]; p->pk[i] = NULL; }
    }
  }
  if (!sf_build_order(f)) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
  if (problem[0]) { snprintf(f->problem, sizeof f->problem, "%s", problem); f->has_problem = 1; }
  ok = 1;
done:
  free(full);
  free_parser(p);
  free(p);
  return ok;
}
