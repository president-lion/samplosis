/* sfsfz.c: SFZ instruments (sfzformat.com) read into the SoundFont model: one .sfz is one preset (bank 0, program 0,
   named after the file), each <region> one SF2-style region (two for a stereo sample), with the region conditions
   of samplosis.h for SFZ's logic. The player and voice then play it like any SoundFont.

   The text. Comments (// to the line's end and block comments) are blanked first. #define $NAME value is a textual
   replacement on every later line (a value may use earlier defines, expanded when it is defined); #include "file"
   reads another file in place, relative to the including file, at most 16 deep, never one already being read (so a
   cycle stops), at most 4096 includes and 256 MB of text in all. Headers are <control>, <global>, <master>,
   <group> and <region>, inheriting in that order (a <global> starts the master and group again from it, a
   <master> the group, and each <region> starts from the group); <curve>, <effect> and <midi> are read past (their
   opcodes do nothing here). An opcode is name=value, and a value runs to the next name= or header or the line's
   end, so sample paths may have spaces; values are trimmed. Header and opcode names, note names and the words of
   enumerated values are taken in any case (sfzformat.com writes them lower case; being lenient breaks no valid
   file). Note names: c4 is 60 (sfzformat.com's convention, middle C), with # or b; <control>'s note_offset and
   octave_offset add to every key value after them. Damaged text never crashes: the reads are bounded, NUL bytes
   count as spaces, a line is expanded to at most 64 KB, and what can't be read is skipped.

   Sample files. A sample path is relative to the root .sfz's folder (not an included file's), after <control>'s
   default_path; slashes either way. Every file is opened once at load (here, on the main thread) to learn its
   format:
   - PCM WAV (8-bit unsigned, 16-, 24- and 32-bit integer, 32-bit float; WAVE_FORMAT_EXTENSIBLE too) and AIFF or
     AIFF-C (16- and 24-bit big-endian, and 16-bit "sowt") stream straight from the file (SMP_SRC_PCM_FILE: the
     data chunk's offset, encoding and channel count). A WAV's smpl chunk gives the first loop and the unity note.
   - Everything else the decode layer reads (FLAC, Ogg Vorbis, Opus, MP3, other WAV encodings, and so on) is
     SMP_SRC_DECODED: the loader thread decodes the file once into the disk cache, keyed by the sample file's own
     path, size and modified time, and streams it from there. FLAC and Ogg headers give the length and rate here;
     any other format is decoded once now just to learn them.
   - A file that can't be opened is missing; one that can't be read is unreadable. Their regions are skipped, and
     the font's problem says "Missing N samples (first: name)" (and "N samples can't be read (first: name)"). With
     no region left, the file doesn't open, with that as the reason.
   - *sine, *silence and *noise are made in memory (sf2.h's SF_GEN_*), looping whole.
   - Stereo. The voices are mono, so a stereo sample makes two regions, one per channel, like an SF2 stereo pair:
     the left channel panned to (pan - width) and the right to (pan + width), in SF2 pan units (pan and width are
     SFZ percent; width 100, the default, puts the channels hard left and right, so a centred stereo sample plays
     at its own level in each ear). A file with more channels plays its first two.

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
   polyphony, one-shots, amp_veltrack and its curve) go into SmpSfCond. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sf2.h"
#include "decode.h"

#define MAX_TEXT (32u << 20)             /* the largest .sfz (or included file) read */
#define MAX_ALL_TEXT (256ull << 20)      /* all the text read through includes */
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
#define SEEN_SLOTS 4096                  /* the ignored-name set */
#define MAX_CHUNKS 4096                  /* chunks walked in one WAV or AIFF header */

/* the SF2 generators set here (SF2.04 section 8.1.2) */
enum {
  G_START = 0, G_END = 1, G_LOOPSTART = 2, G_LOOPEND = 3, G_STARTCOARSE = 4, G_MODLFO_PITCH = 5,
  G_VIBLFO_PITCH = 6, G_MODENV_PITCH = 7, G_FC = 8, G_Q = 9, G_MODLFO_FC = 10, G_MODENV_FC = 11,
  G_ENDCOARSE = 12, G_MODLFO_VOL = 13, G_PAN = 17, G_DELAY_MODLFO = 21, G_FREQ_MODLFO = 22,
  G_DELAY_VIBLFO = 23, G_FREQ_VIBLFO = 24, G_DELAY_MODENV = 25, G_DELAY_VOLENV = 33,
  G_LOOPSTARTCOARSE = 45, G_ATTEN = 48, G_LOOPENDCOARSE = 50, G_COARSE = 51, G_FINE = 52, G_MODES = 54,
  G_SCALE = 56, G_ROOTKEY = 58
};

enum { LOOP_NONE, LOOP_ONESHOT, LOOP_CONT, LOOP_SUSTAIN };   /* SFZ loop_mode */
enum { LV_NONE, LV_CONTROL, LV_GLOBAL, LV_MASTER, LV_GROUP, LV_REGION, LV_OTHER };
enum { F_OK, F_MISSING, F_UNREADABLE };

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
  uint32_t group, off_by; int off_mode;
  double rt_decay; int polyphony, note_polyphony;
  double bend_up, bend_down; int bend_set;      /* cents */
} Opts;

/* where a sample start head begins, and its sample per channel */
typedef struct Head { uint32_t at; int id[2]; struct Head *next; } Head;

/* a sample file (or a made sample), probed once */
typedef struct {
  wchar_t *wpath;                    /* the full path (NULL for a made sample) */
  int status;                        /* F_* */
  int gen;                           /* SF_GEN_*: a made sample */
  SmpSrcKind kind; int enc, channels, block;   /* block: bytes a frame, for PCM_FILE */
  uint32_t rate, frames, ls, le;     /* the file's first loop, le exclusive; 0, 0 when none */
  uint64_t offset;                   /* PCM_FILE: the first frame's byte offset */
  int pitch;                         /* the unity note from a smpl chunk, else 60 */
  int path_id;
  int base[2];                       /* its samples from frame 0, per channel (-1 until made) */
  Head *heads;
} File;

/* a region being built: its modulators (an offset into the pool) and its velocity curve (index, or -1) */
typedef struct { SmpSfRegion r; int mod_off, nmod, curve; } Reg;

typedef struct {
  SmpFont *f;
  const char *root_name;             /* the .sfz's file name, for messages */
  wchar_t root_dir[MAX_PATH * 4];    /* its folder, with a trailing backslash */
  Opts global, master, group, region, base;
  int level;
  char default_path[MAX_SPATH];
  int note_offset, octave_offset;
  /* defines: names (with the $) and values, and an open-addressing table of their indices */
  char **def_name, **def_val; int ndef; int *def_slot;
  /* includes */
  const wchar_t *stack[MAX_DEPTH + 1]; int nincludes; uint64_t text_bytes;
  char *line, *val, *tmp;            /* MAX_LINE + 1 each */
  /* ignored names */
  uint64_t *seen; int nseen;
  /* files */
  File *files; int nfiles, capfiles; int *fslot; int capslot;
  int made[4];                       /* the made samples' files by SF_GEN_*, -1 until used */
  int missing, unreadable; char first_missing[160], first_unreadable[160];
  /* the model being built */
  SmpSfSample *shdr; SmpSrc *src; int ns, caps;
  char **paths; int npaths, cappaths;
  Reg *regs; int nr, capr; int too_many;
  SmpSfMod *mods; int nm, capm;
  float *curves; int nc, capc;
  int swdef;                         /* the instrument's sw_default (the last given), -1 none */
  int oom;
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

/* an opcode (or header, as "<name>") this can't play: counted once per distinct name, the first kept */
static void ignored(P *p, const char *name) {
  uint64_t h = fnv(name, strlen(name));
  unsigned i = (unsigned)h & (SEEN_SLOTS - 1), k;
  for (k = 0; k < SEEN_SLOTS; k++, i = (i + 1) & (SEEN_SLOTS - 1)) {
    if (p->seen[i] == h) return;
    if (!p->seen[i]) break;
  }
  if (k < SEEN_SLOTS && p->nseen < SEEN_SLOTS / 2) { p->seen[i] = h; p->nseen++; }
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

static int seconds_tc(double s, int hi) {      /* seconds to SF2 timecents (0 and less: the shortest) */
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
}

/* ---- opcodes ---- */

static int env_stage(const char *s) {
  static const char *names[6] = {"delay", "attack", "hold", "decay", "sustain", "release"};
  int i;
  for (i = 0; i < 6; i++) if (!strcmp(s, names[i])) return i;
  return -1;
}

/* an LFO opcode's last part: freq, delay, pitch (or depth for pitchlfo), volume, cutoff */
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

/* one opcode of a level (nm is lower case; v trimmed) */
static void opcode(P *p, Opts *o, const char *nm, const char *v) {
  double d; int k, i;
  char w[64];
#define IS(s) (!strcmp(nm, s))
  /* the sample */
  if (IS("sample")) {
    size_t n = strlen(v);
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') { v++; n -= 2; }
    if (n >= sizeof o->sample) n = sizeof o->sample - 1;
    memcpy(o->sample, v, n); o->sample[n] = 0;
    return;
  }
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
  } else if (!label(nm)) {
    ignored(p, nm);
  }
}

/* ---- sample files ---- */

static int rd_at(HANDLE h, uint64_t off, void *buf, DWORD n) {
  LARGE_INTEGER li; DWORD got = 0;
  li.QuadPart = (LONGLONG)off;
  if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return 0;
  return ReadFile(h, buf, n, &got, NULL) && got == n;
}

/* RIFF WAVE: 1 when it is plain PCM (fl filled for PCM_FILE), 0 when it needs the decoder */
static int probe_wav(HANDLE h, uint64_t size, File *fl) {
  uint8_t c[8], b[64];
  uint64_t pos = 12, doff = 0, dsize = 0;
  int have_fmt = 0, have_data = 0, k;
  unsigned tag = 0, ch = 0, block = 0, bits = 0, sub = 0;
  uint32_t rate = 0, unity = 60, lstart = 0, lend = 0; int have_loop = 0;
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
  if (tag == 0xFFFE) tag = sub;
  fl->channels = (int)ch; fl->rate = rate; fl->pitch = (int)unity; fl->offset = doff;
  if (tag == 1 && bits == 8 && block == ch) fl->enc = SMP_PCM_U8;
  else if (tag == 1 && bits == 16 && block == 2 * ch) fl->enc = SMP_PCM_S16LE;
  else if (tag == 1 && bits == 24 && block == 3 * ch) fl->enc = SMP_PCM_S24LE;
  else if (tag == 1 && bits == 32 && block == 4 * ch) fl->enc = SMP_PCM_S32LE;
  else if (tag == 3 && bits == 32 && block == 4 * ch) fl->enc = SMP_PCM_F32LE;
  else return 0;
  fl->block = (int)block;
  if (dsize / block > 0xFFFFFFF0u) return 0;                  /* past 4 G frames: the decoder's business */
  fl->frames = (uint32_t)(dsize / block);
  if (have_loop && lend >= lstart && lstart < fl->frames) {
    fl->ls = lstart; fl->le = lend + 1 < fl->frames ? lend + 1 : fl->frames;   /* smpl's end is the last frame */
  }
  fl->kind = SMP_SRC_PCM_FILE;
  return fl->frames > 0;
}

/* AIFF and AIFF-C: 1 when its data can stream as PCM_FILE */
static int probe_aiff(HANDLE h, uint64_t size, File *fl, int aifc) {
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
  if (!have_comm || !have_data || !ch || !(rate >= 1 && rate < 4e6)) return 0;
  fl->channels = (int)ch; fl->rate = (uint32_t)(rate + 0.5); fl->pitch = 60; fl->offset = doff;
  if (bits == 16 && (!memcmp(comp, "NONE", 4) || !memcmp(comp, "twos", 4))) fl->enc = SMP_PCM_S16BE;
  else if (bits == 24 && (!memcmp(comp, "NONE", 4) || !memcmp(comp, "twos", 4))) fl->enc = SMP_PCM_S24BE;
  else if (bits == 16 && !memcmp(comp, "sowt", 4)) fl->enc = SMP_PCM_S16LE;
  else return 0;
  fl->block = (int)ch * (bits / 8);
  fl->frames = (uint32_t)(dsize / (uint64_t)fl->block < frames ? dsize / (uint64_t)fl->block : frames);
  fl->kind = SMP_SRC_PCM_FILE;
  return fl->frames > 0;
}

/* FLAC: STREAMINFO's rate, channels and length (skipping an ID3 tag); 0 when they can't be had this way */
static int probe_flac(HANDLE h, uint64_t size, File *fl) {
  uint8_t b[42];
  uint64_t at = 0, total;
  if (!rd_at(h, 0, b, 10)) return 0;
  if (!memcmp(b, "ID3", 3)) at = 10 + ((uint64_t)(b[6] & 127) << 21 | (b[7] & 127) << 14 | (b[8] & 127) << 7 | (b[9] & 127));
  if (at + 42 > size || !rd_at(h, at, b, 42) || memcmp(b, "fLaC", 4) || (b[4] & 127) != 0) return 0;
  fl->rate = (uint32_t)b[18] << 12 | (uint32_t)b[19] << 4 | b[20] >> 4;
  fl->channels = ((b[20] >> 1) & 7) + 1;
  total = (uint64_t)(b[21] & 15) << 32 | (uint64_t)be32(b + 22);
  if (!fl->rate || !total || total > 0xFFFFFFF0u) return 0;
  fl->frames = (uint32_t)total;
  return 1;
}

/* Ogg Vorbis or Opus: the first page's identification header, and the length from the last page's granule */
static int probe_ogg(HANDLE h, uint64_t size, File *fl) {
  uint8_t b[64], *t;
  DWORD n = size < 65536 ? (DWORD)size : 65536;
  int64_t gran = -1; int preskip = 0; DWORD i;
  if (size < 64 || !rd_at(h, 0, b, 64) || memcmp(b, "OggS", 4) || 27u + b[26] + 19 > 64) return 0;
  t = b + 27 + b[26];
  if (!memcmp(t, "\x01vorbis", 7)) { fl->channels = t[11]; fl->rate = le32(t + 12); }
  else if (!memcmp(t, "OpusHead", 8)) { fl->channels = t[9]; preskip = le16(t + 10); fl->rate = 48000; }
  else return 0;
  if (!fl->channels || !fl->rate) return 0;
  if (!(t = malloc(n))) return 0;
  if (rd_at(h, size - n, t, n)) {
    for (i = n >= 27 ? n - 27 : 0; i + 1 > 0; i--) {
      if (!memcmp(t + i, "OggS", 4) && t[i + 4] == 0) {
        int64_t g = (int64_t)((uint64_t)le32(t + i + 6) | (uint64_t)le32(t + i + 10) << 32);
        if (g >= 0) { gran = g; break; }
      }
      if (!i) break;
    }
  }
  free(t);
  gran -= preskip;
  if (gran <= 0 || gran > 0xFFFFFFF0ll) return 0;
  fl->frames = (uint32_t)gran;
  return 1;
}

/* A sample file's format, once: PCM_FILE when it streams as it is, else DECODED with its length and rate from its
   header (FLAC, Ogg) or from decoding it once now. Sets status. */
static void probe(P *p, File *fl, const char *upath) {
  HANDLE h = CreateFileW(fl->wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  LARGE_INTEGER sz; uint8_t b[12]; int ok = 0;
  (void)p;
  fl->pitch = 60;
  if (h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    fl->status = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_NAME ||
                  e == ERROR_BAD_NETPATH || e == ERROR_INVALID_DRIVE) ? F_MISSING : F_UNREADABLE;
    return;
  }
  if (GetFileSizeEx(h, &sz) && sz.QuadPart >= 12 && rd_at(h, 0, b, 12)) {
    uint64_t size = (uint64_t)sz.QuadPart;
    if (!memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) ok = probe_wav(h, size, fl);
    else if (!memcmp(b, "FORM", 4) && (!memcmp(b + 8, "AIFF", 4) || !memcmp(b + 8, "AIFC", 4))) ok = probe_aiff(h, size, fl, b[11] == 'C');
    if (!ok) {
      fl->ls = fl->le = 0;
      fl->kind = SMP_SRC_DECODED;
      if (!memcmp(b, "fLaC", 4) || !memcmp(b, "ID3", 3)) ok = probe_flac(h, size, fl);
      else if (!memcmp(b, "OggS", 4)) ok = probe_ogg(h, size, fl);
    }
  }
  CloseHandle(h);
  if (!ok) {
    /* any other format the decode layer reads: decoded once now to learn it (the loader decodes it again into the
       cache when a preset needs it) */
    int ch = 0, rate = 0; long frames = 0; char err[256];
    float *x = decode_audio(upath, &ch, &frames, &rate, err, sizeof err);
    if (!x) { fl->status = F_UNREADABLE; return; }
    free(x);
    if (ch <= 0 || frames <= 0 || rate <= 0) { fl->status = F_UNREADABLE; return; }
    fl->kind = SMP_SRC_DECODED; fl->channels = ch; fl->frames = (uint32_t)frames; fl->rate = (uint32_t)rate;
    fl->ls = fl->le = 0;
  }
  fl->status = F_OK;
}

/* the file table's slot for a lower-cased full path (open addressing on its hash) */
static int *file_slot(P *p, const wchar_t *lw) {
  uint64_t hsh = fnv(lw, wcslen(lw) * sizeof(wchar_t));
  unsigned i = (unsigned)hsh & (unsigned)(p->capslot - 1);
  for (;;) {
    int *s = &p->fslot[i];
    wchar_t tmp[MAX_PATH * 4];
    if (*s < 0) return s;
    wcsncpy(tmp, p->files[*s].wpath, MAX_PATH * 4 - 1); tmp[MAX_PATH * 4 - 1] = 0;
    CharLowerBuffW(tmp, (DWORD)wcslen(tmp));
    if (!wcscmp(tmp, lw)) return s;
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

static void note_bad(P *p, File *fl, const char *as_written) {
  if (fl->status == F_MISSING) {
    if (!p->missing++) snprintf(p->first_missing, sizeof p->first_missing, "%s", as_written);
  } else if (fl->status == F_UNREADABLE) {
    if (!p->unreadable++) snprintf(p->first_unreadable, sizeof p->first_unreadable, "%s", as_written);
  }
}

/* the file a sample opcode names (index), or -1 when its regions are skipped (missing, unreadable, unknown) */
static int sample_file(P *p, const char *name) {
  wchar_t w[MAX_PATH * 4], full[MAX_PATH * 4], lw[MAX_PATH * 4];
  char joined[MAX_SPATH * 2 + 4], u[MAX_PATH * 12];
  int *slot, i, absolute;
  size_t k;
  if (name[0] == '*') {                                       /* made in memory */
    int g = !_stricmp(name, "*sine") ? SF_GEN_SINE : !_stricmp(name, "*silence") ? SF_GEN_SILENCE :
            !_stricmp(name, "*noise") ? SF_GEN_NOISE : 0;
    File *fl;
    if (!g) { char t[80]; snprintf(t, sizeof t, "sample=%.60s", name); ignored(p, t); return -1; }
    if (p->made[g] >= 0) return p->made[g];
    if ((i = new_file(p)) < 0) return -1;
    fl = &p->files[i];
    fl->gen = g; fl->status = F_OK; fl->kind = SMP_SRC_PCM_FILE; fl->channels = 1; fl->pitch = 60; fl->path_id = 0;
    fl->rate = g == SF_GEN_SINE ? SF_GEN_SINE_RATE : 44100;
    fl->frames = g == SF_GEN_NOISE ? SF_GEN_NOISE_FRAMES : SF_GEN_FRAMES;
    fl->ls = 0; fl->le = fl->frames;
    p->made[g] = i;
    return i;
  }
  /* the path: absolute as it is, else the root's folder, default_path, then the name; slashes made backslashes */
  absolute = (name[0] && name[1] == ':') || name[0] == '\\' || name[0] == '/';
  if (absolute) snprintf(joined, sizeof joined, "%s", name);
  else snprintf(joined, sizeof joined, "%s%s", p->default_path, name);
  for (k = 0; joined[k]; k++) if (joined[k] == '/') joined[k] = '\\';
  {
    wchar_t rel[MAX_PATH * 4];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, joined, -1, rel, MAX_PATH * 4) &&
        !MultiByteToWideChar(CP_ACP, 0, joined, -1, rel, MAX_PATH * 4)) return -1;
    if (absolute || (rel[0] && rel[1] == L':') || rel[0] == L'\\') wcsncpy(w, rel, MAX_PATH * 4 - 1);
    else _snwprintf(w, MAX_PATH * 4 - 1, L"%s%s", p->root_dir, rel);
    w[MAX_PATH * 4 - 1] = 0;
  }
  if (!GetFullPathNameW(w, MAX_PATH * 4, full, NULL)) return -1;
  wcscpy(lw, full);
  CharLowerBuffW(lw, (DWORD)wcslen(lw));
  if (p->nfiles + 1 > p->capslot / 2) {                       /* the table at most half full */
    int nc = p->capslot ? p->capslot * 2 : 256, j, *ns = malloc((size_t)nc * sizeof(int)), *old = p->fslot, oc = p->capslot;
    if (!ns) { p->oom = 1; return -1; }
    for (j = 0; j < nc; j++) ns[j] = -1;
    p->fslot = ns; p->capslot = nc;
    for (j = 0; j < oc; j++) if (old[j] >= 0 && p->files[old[j]].wpath) {
      wchar_t t[MAX_PATH * 4];
      wcscpy(t, p->files[old[j]].wpath); CharLowerBuffW(t, (DWORD)wcslen(t));
      *file_slot(p, t) = old[j];
    }
    free(old);
  }
  slot = file_slot(p, lw);
  if (*slot >= 0) return p->files[*slot].status == F_OK ? *slot : -1;
  if ((i = new_file(p)) < 0) return -1;
  if (!(p->files[i].wpath = _wcsdup(full))) { p->oom = 1; return -1; }
  *slot = i;
  if (!WideCharToMultiByte(CP_UTF8, 0, full, -1, u, (int)sizeof u, NULL, NULL)) { p->files[i].status = F_UNREADABLE; note_bad(p, &p->files[i], name); return -1; }
  probe(p, &p->files[i], u);
  if (p->files[i].status != F_OK) { note_bad(p, &p->files[i], name); return -1; }
  if ((p->files[i].path_id = add_path(p, u)) < 0) { p->files[i].status = F_UNREADABLE; return -1; }
  return i;
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
  h->start = 0; h->end = fl->frames - at;
  if (fl->le > fl->ls && fl->le > at) { h->loop_start = fl->ls > at ? fl->ls - at : 0; h->loop_end = fl->le - at; }
  h->rate = fl->rate; h->pitch = (uint8_t)clampi(fl->pitch, 0, 127); h->type = SF_MONO;
  c->kind = fl->kind; c->path_id = fl->path_id; c->gen = (uint8_t)fl->gen;
  c->channels = (uint8_t)clampi(fl->channels, 1, 255); c->channel = (uint8_t)ch;
  if (fl->kind == SMP_SRC_PCM_FILE) { c->offset = fl->offset + (uint64_t)at * (uint64_t)fl->block; c->enc = (uint8_t)fl->enc; }
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

/* the region's modulators into the pool (sharing an identical list made just before); returns its offset */
static int add_mods(P *p, const SmpSfMod *m, int n) {
  int k;
  for (k = p->nm - n; k >= 0 && k >= p->nm - 64 * n; k -= n ? n : 1) {   /* the last few lists */
    if (!memcmp(p->mods + k, m, (size_t)n * sizeof *m)) return k;
    if (!n) return k;
  }
  if (!grow((void **)&p->mods, &p->capm, p->nm + n, sizeof *p->mods)) { p->oom = 1; return -1; }
  memcpy(p->mods + p->nm, m, (size_t)n * sizeof *m);
  p->nm += n;
  return p->nm - n;
}

/* amp_velcurve_N's points as 128 values, linear between the points given (0 at 0 and 1 at 127 unless given) */
static int add_curve(P *p, const Opts *o) {
  float c[128]; int i, last = 0;
  float lv = o->velset[0] ? o->velcurve[0] : 0;
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
   conditions, as one region, or two for a stereo sample. */
static void finish_region(P *p) {
  const Opts *o = &p->region;
  File *fl; int fi, nch, ch, lm, uses_loop, nmod = 0, moff, curve = -1;
  int lokey, hikey, lovel, hivel;
  int64_t start, endx, ls_abs, le_abs;
  uint32_t at = 0, hf;
  SmpSfRegion r;
  SmpSfMod m[12];
  double db;
  if (o->sw_default >= 0) p->swdef = o->sw_default;
  if (!o->sample[0] || p->too_many || p->oom) return;
  if (o->hikey < 0 || o->lokey > 127 || o->hivel < 0 || o->lovel > 127) return;   /* never plays */
  lokey = clampi(o->lokey, 0, 127); hikey = clampi(o->hikey, 0, 127);
  lovel = clampi(o->lovel, 0, 127); hivel = clampi(o->hivel, 0, 127);
  if (lokey > hikey || lovel > hivel) return;
  if ((fi = sample_file(p, o->sample)) < 0) return;
  fl = &p->files[fi];
  nch = fl->channels >= 2 ? 2 : 1;

  /* Sample playback, in frames of the file. offset is the first frame; end the last one played (inclusive, as
     sfzformat.com has it), so the region plays [offset, end + 1); loop_start and loop_end likewise (loop_end
     inclusive), defaulting to the file's own loop. loop_mode defaults to loop_continuous when the file has a loop,
     else no_loop (sfzformat.com): no_loop is sampleModes 0, loop_continuous 1, loop_sustain 3 (loop until release,
     then on to the end), and one_shot is sampleModes 0 with the one_shot condition (note-offs don't release it). */
  start = (int64_t)clampd(floor(o->offset + 0.5), 0, fl->frames ? fl->frames - 1 : 0);
  endx = o->end >= 0 ? (int64_t)clampd(floor(o->end + 0.5) + 1, 0, fl->frames) : (int64_t)fl->frames;
  if (endx <= start) return;
  ls_abs = o->loop_start >= 0 ? (int64_t)clampd(floor(o->loop_start + 0.5), 0, fl->frames) : fl->ls;
  le_abs = o->loop_end >= 0 ? (int64_t)clampd(floor(o->loop_end + 0.5) + 1, 0, fl->frames) : fl->le;
  lm = o->loop_mode >= 0 ? o->loop_mode : (fl->le > fl->ls ? LOOP_CONT : LOOP_NONE);
  uses_loop = (lm == LOOP_CONT || lm == LOOP_SUSTAIN) && le_abs > ls_abs;
  if (!uses_loop && lm != LOOP_ONESHOT) lm = LOOP_NONE;
  /* a start head when the offset is past half the head the loader keeps (see the top) */
  hf = fl->rate / 4 > 1024 ? fl->rate / 4 : 1024;
  if (start > 0 && !fl->gen && fl->frames >= fl->rate && hf < fl->frames && start > hf / 2 && (!uses_loop || ls_abs >= start))
    at = (uint32_t)start;

  memset(&r, 0, sizeof r);
  sf_gen_defaults(r.gen);
  sf_cond_defaults(&r.cond);
  r.lokey = (uint8_t)lokey; r.hikey = (uint8_t)hikey; r.lovel = (uint8_t)lovel; r.hivel = (uint8_t)hivel;
  r.gen[43] = (int16_t)(lokey | hikey << 8);
  r.gen[44] = (int16_t)(lovel | hivel << 8);
  r.gen[G_MODES] = (int16_t)(lm == LOOP_CONT ? 1 : lm == LOOP_SUSTAIN ? 3 : 0);

  /* Pitch. pitch_keycenter is overridingRootKey (=sample leaves it unset, so the file's smpl unity note counts);
     pitch_keytrack (cents per key) is scaleTuning, 0..1200; transpose (semitones) and tune (cents) together are
     coarseTune and fineTune. */
  r.gen[G_ROOTKEY] = (int16_t)(o->keycenter == -2 ? -1 : clampi(o->keycenter, 0, 127));
  r.gen[G_SCALE] = (int16_t)clampi((int)floor(o->keytrack + 0.5), 0, 1200);
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
     (dB) is initialFilterQ (cB), which the voice reads as the peak's height above the passband, as SFZ means it.
     fil_veltrack (cents at full velocity) is a velocity-to-cutoff modulator; fil_keytrack (cents per key from
     fil_keycenter) a key-to-cutoff modulator, its centre taken off the cutoff. */
  if (o->cutoff > 0 && o->fil_type >= 0) {
    double kt = clampd(o->fil_keytrack, -255, 255);
    r.cond.fil_type = (uint8_t)o->fil_type;
    r.gen[G_FC] = (int16_t)clampi((int)floor(1200.0 * log2(o->cutoff / 8.176) - kt * o->fil_keycenter + 0.5), 1500, 13500);
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
    if (b->freq > 0 && b->pitch != 0) {
      r.gen[G_FREQ_VIBLFO] = (int16_t)hz_cents(b->freq, -16000, 4500);
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
    memcpy(m, base, sizeof base); nmod = 5;
    if (!o->bend_set) { SmpSfMod w = {0x020E, 59, 12700, 0x0010, 0}; m[nmod++] = w; }
    else {
      double up = clampd(o->bend_up, -9600, 9600), dn = clampd(o->bend_down, -9600, 9600);
      int A = (int)floor((up - dn) / 2 + 0.5), B = (int)floor((up + dn) / 2 + 0.5);
      SmpSfMod w = {0x020E, 59, (int16_t)A, 0, 0}, x = {0x020E, 59, (int16_t)B, 0, 2};
      m[nmod++] = w;
      if (B) m[nmod++] = x;
    }
    if (r.cond.fil_type == o->fil_type && o->cutoff > 0 && o->fil_type >= 0) {
      if (o->fil_veltrack != 0) { SmpSfMod w = {0x0002, 8, (int16_t)clampi((int)floor(o->fil_veltrack + 0.5), -12000, 12000), 0, 0}; m[nmod++] = w; }
      if (o->fil_keytrack != 0) { SmpSfMod w = {0x0003, 8, (int16_t)clampi((int)floor(clampd(o->fil_keytrack, -255, 255) * 128 + 0.5), -32768, 32767), 0, 0}; m[nmod++] = w; }
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
    c->group = o->group; c->off_by = o->off_by; c->off_mode = (uint8_t)o->off_mode;
    c->rt_decay = (float)clampd(o->rt_decay, 0, 200);
    c->polyphony = (uint16_t)clampi(o->polyphony, 0, 65535);
    c->note_polyphony = (uint16_t)clampi(o->note_polyphony, 0, 65535);
    c->one_shot = (uint8_t)(lm == LOOP_ONESHOT);
  }

  /* one region a channel: the sample from frame `at`, the address offsets relative to that sample (fine plus
     coarse, as the voice reads them), and the channel's pan */
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
    g->r.gen[53] = (int16_t)(sid < 32767 ? sid : 32767);
    addr(g->r.gen, G_START, G_STARTCOARSE, start - at);
    addr(g->r.gen, G_END, 12, endx - (int64_t)at - (int64_t)sh->end);
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
    case LV_GLOBAL: opcode(p, &p->global, nm, p->val); break;
    case LV_MASTER: opcode(p, &p->master, nm, p->val); break;
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
  char name[MAX_SPATH]; wchar_t w[MAX_PATH * 4], rel[MAX_PATH * 4], full[MAX_PATH * 4];
  size_t a = 0, b, k; char why[8];
  while (a < n && ws(s[a])) a++;
  if (a < n && s[a] == '"') { a++; for (b = a; b < n && s[b] != '"'; b++) {} }
  else for (b = a; b < n && !ws(s[b]); b++) {}
  if (b <= a || b - a >= sizeof name) return;
  memcpy(name, s + a, b - a); name[b - a] = 0;
  for (k = 0; name[k]; k++) if (name[k] == '/') name[k] = '\\';
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, rel, MAX_PATH * 4) &&
      !MultiByteToWideChar(CP_ACP, 0, name, -1, rel, MAX_PATH * 4)) return;
  if ((rel[0] && rel[1] == L':') || rel[0] == L'\\') wcsncpy(w, rel, MAX_PATH * 4 - 1);
  else _snwprintf(w, MAX_PATH * 4 - 1, L"%s%s", dir, rel);
  w[MAX_PATH * 4 - 1] = 0;
  if (!GetFullPathNameW(w, MAX_PATH * 4, full, NULL)) return;
  process_file(p, full, depth + 1, why, (int)sizeof why);   /* a missing or bad include is skipped */
}

/* one line (n bytes, no newline): a directive, or headers and opcodes */
static void do_line(P *p, const char *s, size_t n, const wchar_t *dir, int depth) {
  size_t i = 0;
  while (i < n && ws(s[i])) i++;
  if (i < n && s[i] == '#') {
    if (n - i >= 7 && !memcmp(s + i, "#define", 7)) { define(p, s + i + 7, n - i - 7); return; }
    if (n - i >= 8 && !memcmp(s + i, "#include", 8)) {
      size_t m = p->ndef ? expand(p, s + i + 8, n - i - 8, p->line, MAX_LINE) : n - i - 8;
      include(p, p->ndef ? p->line : s + i + 8, m, dir, depth);
      return;
    }
  }
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
    while (i < n) {                                 /* the value runs to the next name= or header */
      if (s[i] == '<') break;
      if (ws(s[i])) {
        size_t j = i, k;
        while (j < n && ws(s[j])) j++;
        if (j >= n || s[j] == '<') break;
        for (k = j; k < n && namech(s[k]); k++) {}
        if (k > j && k < n && s[k] == '=') break;
        i = j;
        continue;
      }
      i++;
    }
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
  HANDLE h; LARGE_INTEGER sz; DWORD got = 0; char *t; size_t n, i, a;
  wchar_t dir[MAX_PATH * 4], *slash;
  int k;
  if (depth > MAX_DEPTH || (depth > 0 && p->nincludes >= MAX_INCLUDES)) return 0;
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
  wcsncpy(dir, wpath, MAX_PATH * 4 - 1); dir[MAX_PATH * 4 - 1] = 0;
  slash = wcsrchr(dir, L'\\');
  if (slash) slash[1] = 0; else dir[0] = 0;
  p->stack[depth] = wpath;
  strip_comments(t, n);
  for (a = i = 0; i <= n && !p->oom; i++) {
    if (i == n || t[i] == '\n') { do_line(p, t + a, i - a, dir, depth); a = i + 1; }
  }
  free(t);
  return 1;
}

/* ---- the whole file ---- */

int sf_is_sfz(const char *path, const uint8_t *head, size_t n) {
  static const char *hdr[] = {"<region>", "<group>", "<control>", "<global>", "<master>"};
  const char *dot = path ? strrchr(path, '.') : NULL;
  size_t i, k;
  if (dot && !_stricmp(dot, ".sfz")) return 1;
  for (k = 0; k < sizeof hdr / sizeof *hdr; k++) {
    size_t L = strlen(hdr[k]);
    for (i = 0; i + L <= n; i++) if (!memcmp(head + i, hdr[k], L)) return 1;
  }
  return 0;
}

static void free_parser(P *p) {
  int i;
  for (i = 0; i < p->ndef; i++) { free(p->def_name[i]); free(p->def_val[i]); }
  free(p->def_name); free(p->def_val); free(p->def_slot);
  free(p->line); free(p->val); free(p->tmp); free(p->seen);
  for (i = 0; i < p->nfiles; i++) {
    Head *h = p->files[i].heads;
    while (h) { Head *nx = h->next; free(h); h = nx; }
    free(p->files[i].wpath);
  }
  free(p->files); free(p->fslot);
  if (p->paths) for (i = 0; i < p->npaths; i++) free(p->paths[i]);
  free(p->paths); free(p->shdr); free(p->src); free(p->regs); free(p->mods); free(p->curves);
}

int sf_parse_sfz(SmpFont *f, const char *path, char *why, int wcap) {
  P *p = calloc(1, sizeof *p);
  wchar_t *full = NULL;
  int ok = 0, i;
  char problem[sizeof f->problem];
  if (!p) { snprintf(why, (size_t)wcap, "out of memory"); return 0; }
  p->f = f;
  p->root_name = base_name(path);
  p->swdef = -1;
  for (i = 0; i < 4; i++) p->made[i] = -1;
  opts_default(&p->global); p->master = p->group = p->region = p->global;
  p->def_name = calloc(MAX_DEFINES, sizeof(char *)); p->def_val = calloc(MAX_DEFINES, sizeof(char *));
  p->def_slot = malloc(DEF_SLOTS * sizeof(int));
  p->line = malloc(MAX_LINE + 1); p->val = malloc(MAX_LINE + 1); p->tmp = malloc(MAX_LINE + 1);
  p->seen = calloc(SEEN_SLOTS, sizeof(uint64_t));
  full = sf_full_path(path);
  if (!p->def_name || !p->def_val || !p->def_slot || !p->line || !p->val || !p->tmp || !p->seen || !full) {
    snprintf(why, (size_t)wcap, "out of memory");
    goto done;
  }
  for (i = 0; i < DEF_SLOTS; i++) p->def_slot[i] = -1;
  {
    wchar_t *slash;
    wcsncpy(p->root_dir, full, MAX_PATH * 4 - 1); p->root_dir[MAX_PATH * 4 - 1] = 0;
    slash = wcsrchr(p->root_dir, L'\\');
    if (slash) slash[1] = 0; else p->root_dir[0] = 0;
  }
  if (add_path(p, path) != 0) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }   /* paths[0]: the .sfz */
  if (!process_file(p, full, 0, why, wcap)) goto done;
  if (p->level == LV_REGION) finish_region(p);
  if (p->oom) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }

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

  /* the model: one preset owning every region, the samples, and the one block of regions, preset starts,
     modulator lists and velocity curves */
  if (p->swdef >= 0) for (i = 0; i < p->nr; i++) p->regs[i].r.cond.sw_default = (int8_t)p->swdef;
  f->version = 2;
  f->phdr = calloc(2, sizeof *f->phdr);
  f->order = calloc(1, sizeof *f->order);
  f->shdr = calloc((size_t)p->ns + 1, sizeof *f->shdr);
  f->src = calloc((size_t)p->ns, sizeof *f->src);
  if (!f->phdr || !f->order || !f->shdr || !f->src) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
  {
    const char *b = p->root_name, *dot = strrchr(b, '.');
    size_t L = dot && dot > b ? (size_t)(dot - b) : strlen(b);
    if (L > 20) L = 20;
    memcpy(f->phdr[0].name, b, L); f->phdr[0].name[L] = 0;
  }
  f->nphdr = 1;
  memcpy(f->shdr, p->shdr, (size_t)p->ns * sizeof *f->shdr);
  memcpy(f->src, p->src, (size_t)p->ns * sizeof *f->src);
  f->nshdr = p->ns;
  f->paths = p->paths; f->npaths = p->npaths; p->paths = NULL; p->npaths = 0;
  {
    size_t sreg = (size_t)p->nr * sizeof(SmpSfRegion), sst = 2 * sizeof(int);
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
    f->rstart[0] = 0; f->rstart[1] = p->nr;
    f->nregions = p->nr;
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
