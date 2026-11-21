/* sfds.c: Decent Sampler presets (.dspreset; decentsampler-developers-guide.readthedocs.io) read into the SoundFont
   model through sfsfz.c's builder. A preset is one preset (bank 0, program 0, named after the file); each <sample>
   is a region (two for a stereo file), fed to the builder as SFZ opcodes, so sample files, probing, missing files,
   stereo pairs, start heads and the region conditions all work as they do for SFZ.

   The XML. yxml (src\codecs\yxml, MIT) parses it byte by byte; a small tree is kept of the elements down to
   <sample> (DecentSampler, groups, group, sample: four levels), with their attributes. Deeper elements (the
   bindings inside <ui> and so on) are read past. Damaged XML never crashes: yxml checks it is well formed (with a
   fixed 64 KB stack, which bounds names and nesting), an attribute value keeps at most 64 KB, the tree at most
   SF_MAX_REGIONS * 2 elements and 4M attributes, and the file at most 32 MB. XML that isn't well formed doesn't
   open ("its XML is damaged near line N").

   Levels. <groups> holds the defaults for every group, <group> for its samples; a <sample>'s own attribute wins,
   then its group's, then <groups>' (the envelope, ranges, loop, trigger, round robin, tags and the rest). Three
   kinds are combined instead, as Decent Sampler does:
   - volume: <groups>' volume (globalVolume wins over it), <group>'s (groupVolume wins) and <sample>'s multiply;
     each is linear (1 is unity) or decibels with a dB suffix;
   - tuning (semitones): <groups>' globalTuning, <group>'s groupTuning and the sample's tuning add;
   - pan (-100..100): globalPan, groupPan and the sample's pan add, and the sum is clamped.
   A group with enabled="false" is left out, unless some binding in <ui> can turn it on (a button's state, below):
   then it is a layer like the others, silent until it is on.
   A group's own <effects> are its regions' filters: the first lowpass, lowpass_1pl, highpass or bandpass is SFZ's
   fil (fil_type, cutoff, resonance), the second fil2; a Q (Decent Sampler's resonance) is the resonance in dB its
   peak stands above the passband (sf_q_db; a Q under a Butterworth's is one), and any other effect there is
   counted.

   The attributes and their SFZ opcodes (units in brackets):
   - path: sample (relative to the .dspreset's folder; slashes either way);
   - rootNote, loNote, hiNote, loVel, hiVel: pitch_keycenter, lokey, hikey, lovel, hivel (MIDI numbers);
   - start, end, loopStart, loopEnd: offset, end, loop_start, loop_end (frames; end and loopEnd are the last frame
     played, as Decent Sampler's defaults of the length minus 1 say);
   - loopEnabled: true is loop_continuous, looping the whole sample when no loop points are given and the file has
     none; false is no_loop; left out, the file's own loop counts (SFZ's rule: the guide gives loopEnabled no
     default, and says embedded loop markers are used when loopStart and loopEnd aren't given); loopDuringRelease
     false is loop_sustain (the loop let go at the release). loopCrossfade is counted (a loop here has no fade);
   - ampEnvEnabled false: one_shot (the sample plays to its end whatever the note-offs), unless it loops;
   - attack, decay, sustain, release (seconds; sustain 0..1, as percent), and ampEnvAttack, ampEnvDecay,
     ampEnvSustain, ampEnvRelease, which win over them on the same element; ampEnvDelay, ampEnvHold: ampeg_*.
     Decent Sampler's guide gives no defaults, so SFZ's apply (instant attack and release, full sustain);
   - pitchKeyTrack, ampVelTrack (0..1): pitch_keytrack, amp_veltrack (percent). Velocity sets the gain linearly
     (the guide: at ampVelTrack 1, velocity 63 is gain 0.5), so every region gets a straight velocity curve
     (amp_velcurve_127=1) in place of SFZ's squared one;
   - trigger: attack, release, first, legato (continuous is counted and plays as attack);
   - releaseTriggerDecay: rt_decay (dB a second with a dB suffix, either sign, as the guide turns a positive value
     into a decay; a linear value v, "the linear gain decay factor per second" with 0 for no decay, is taken as
     the share lost a second, so -20 log10(1 - v) dB);
   - seqMode round_robin: seq_length and seq_position (seqPosition 1 by default; seqLength 0 or left out is the
     highest seqPosition among the instrument's round-robin samples, since the guide asks for seqLength when sets of
     different lengths are mixed; the counter is the player's, per set of regions with the same ranges); random
     and true_random: the note's random draw split by position, lorand (position - 1) / length to hirand position
     / length, the length found the same way among random samples (both kinds may repeat a sample, where the
     guide's random avoids repeating one); always: every time;
   - tags and silencedByTags: SFZ's group and off_by. Each tag named in some silencedByTags gets a number; a
     sample's group is its first such tag (its own tags, then its group's), its off_by the first tag of its
     silencedByTags. A sample with two such tags, or silenced by two, is counted ("tags beyond the first"): the
     player has one group and one off_by per region;
   - silencingMode fast or normal: off_mode;
   - loCCn and hiCCn: loccN and hiccN (-1, the default, is no range).
   - output1Target MAIN_OUTPUT: where everything plays anyway (another output is counted).
   Counted as ignored (smp_sf_ignored): every other attribute of <groups>, <group> and <sample> (glide, loop
   crossfades, curves, slices and so on; names as written, so rootnote isn't rootNote, as in Decent Sampler), and
   every element beside <groups>, <ui>, <effects>, <midi> and <modulators> under <DecentSampler> (<tags>,
   <keyswitches> ...), as "<name>". name, enabled and the root's own attributes are read past.

   Knobs (read_knobs, sfknob.h). <ui>'s <control>s and <labeled-knob>s (in its <tab>s), with their <binding>s, the
   <effects> in order, <midi>'s <cc> bindings to a knob's VALUE and the <modulators> become the preset's SfKnobs,
   handed to the builder (sfz_set_knobs) with the font owning it; a preset with no knob, effect or modulator gets
   none and is built as before. A <button> is a knob of whole values, 0 to its <state>s less 1 (value its start),
   each state's bindings applying while it is in that state. Bindings set: amp AMP_VOLUME, GLOBAL_TUNING and
   GROUP_TUNING (instrument, or groups by groupIndex, by tags, or level="tag" by identifier); amp ENV_ATTACK,
   ENV_DECAY, ENV_SUSTAIN and ENV_RELEASE (a group's amplitude envelope, live); general or amp ENABLED (groups on
   and off; translationValue true or false); effect parameters of the <effects> chain, LEVEL being a gain's; a
   group's own filters' FX_FILTER_FREQUENCY and FX_FILTER_RESONANCE (level group or tag, effectIndex counting the
   group's effects), live on its regions; and modulator ENABLED, MOD_AMOUNT, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN,
   ENV_RELEASE and FREQUENCY (modulatorIndex counting <modulators>' children).
   The effect gain (level, levelUnit linear or db) is a factor on the whole preset, its LEVEL knobs live.
   Modulators: <envelope> (attack, decay, sustain, release), <lfo> (shape sine, square, saw or triangle, frequency)
   and <midiCC> (number), each with modAmount and enabled, become on every region they reach one of SFZ's flex
   envelopes or LFOs (egN_ / lfoN_ for the Nth modulator; a midiCC an LFO whose source is the controller), through
   their <binding>s: effect FX_FILTER_FREQUENCY on a group's own filter with modBehavior multiply (the depth in
   cents from the translation's two ends: a table from 1 to 64 is six octaves), and amp GLOBAL_TUNING or
   GROUP_TUNING with modBehavior add (an LFO swings from the middle of the two ends to either). The player keeps
   their values live (samplosis_dsmod: on or off times the amount on the depths, the LFO's rate, the envelope's
   times at note-on); scope is read past (each voice runs its own). Other modulator bindings, and other modulator
   kinds, are counted. Each enabled <group> is a layer (1, 2 ... in file order, the region's cond.layer). A group's or
   <groups>' volume or tuning that some knob binds is left out of the regions and kept in the layer instead, so the
   player can set it (a binding replaces the attribute, as Decent Sampler's does); what no knob binds stays baked
   in. CCs bound to a knob lose their default modulators (samplosis_unbind_cc), and a preset with a reverb effect
   sends nothing to the shared reverb (samplosis_no_reverb_send). Menus, XY pads, drawbars, multi_state knobs,
   bindings to anything else, effect types other than filters, gain, reverb and delay, and CC bindings that aren't
   to a knob are counted by name; the picture (images, the keyboard's colours, positions, styles) is read past.
   Attributes with no space between them (name="a"next="b"), which Decent Sampler reads, are given one first
   (ds_fix_attr_spaces). Decent Sampler plays a centred voice 5.3 dB below its source (its
   limiter's headroom); that isn't copied, so presets may sound louder here. A cap reached while reading (too many
   elements, attributes or silencing tags) is counted too, so a partly loaded preset says so. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <yxml.h>
#include "sf2.h"

#define DS_MAX_TEXT (32u << 20)
#define DS_STACK 65536                   /* yxml's stack: element and attribute names, and the nesting */
#define DS_MAX_VALUE 65536               /* an attribute value's bytes kept */
#define DS_MAX_ELEMS (SF_MAX_REGIONS * 2)
#define DS_MAX_ATTRS (1 << 22)
#define DS_LEVELS 7                      /* DecentSampler, groups, group, sample (or effects, effect); DecentSampler,
                                            ui, tab, button, state, binding */

typedef struct { int name, value; } Attr;               /* offsets into the string pool */
typedef struct { int name, first_attr, nattr, first_child, last_child, next; } El;

/* the attributes this reads, by number */
enum {
  A_PATH, A_ROOT, A_LONOTE, A_HINOTE, A_LOVEL, A_HIVEL, A_START, A_END, A_LOOPSTART, A_LOOPEND, A_LOOPENABLED,
  A_ATTACK, A_DECAY, A_SUSTAIN, A_RELEASE, A_ENVATTACK, A_ENVDECAY, A_ENVSUSTAIN, A_ENVRELEASE, A_ENVDELAY,
  A_ENVHOLD, A_VOLUME, A_GLOBALVOLUME, A_GROUPVOLUME, A_PAN, A_GROUPPAN, A_GLOBALPAN, A_TUNING, A_GROUPTUNING,
  A_GLOBALTUNING, A_PITCHKEYTRACK, A_AMPVELTRACK, A_SEQMODE, A_SEQLENGTH, A_SEQPOSITION, A_TRIGGER, A_TAGS,
  A_SILENCEDBY, A_SILENCINGMODE, A_RTDECAY, A_AMPENVENABLED, A_ENABLED, A_NAME, A_OUTPUT1, A_LOOPDURINGREL,
  A_LOOPXFADE, A_LOOPXFADEMODE, NA
};
static const char *const anames[NA] = {
  "path", "rootNote", "loNote", "hiNote", "loVel", "hiVel", "start", "end", "loopStart", "loopEnd", "loopEnabled",
  "attack", "decay", "sustain", "release", "ampEnvAttack", "ampEnvDecay", "ampEnvSustain", "ampEnvRelease",
  "ampEnvDelay", "ampEnvHold", "volume", "globalVolume", "groupVolume", "pan", "groupPan", "globalPan", "tuning",
  "groupTuning", "globalTuning", "pitchKeyTrack", "ampVelTrack", "seqMode", "seqLength", "seqPosition", "trigger",
  "tags", "silencedByTags", "silencingMode", "releaseTriggerDecay", "ampEnvEnabled", "enabled", "name", "output1Target",
  "loopDuringRelease", "loopCrossfade", "loopCrossfadeMode"
};

#define MAX_CC 4
/* one element's attributes looked up: the value of each known one (NULL when absent), and its CC ranges */
typedef struct {
  const char *a[NA];
  int ncc; int cc[MAX_CC]; const char *cclo[MAX_CC], *cchi[MAX_CC];
  /* its tags (pass 1, read once per element): the first silencing tag's number and whether there is another one;
     the same for its silencedByTags */
  int tag_id, tag_more, by_id, by_more;
} Look;

/* a string to number map (open addressing) */
typedef struct { char **key; int *val; int n, cap; } Map;

typedef struct {
  char *pool; size_t npool, cappool;
  El *el; int nel, capel;
  Attr *at; int nat, capat;
  Map silencers;                         /* tags named in some silencedByTags, numbered from 1 */
  int seq_max[2];                        /* the highest seqPosition of round_robin [0] and random [1] samples */
  int pass;                              /* 0: the scan before the builder; 1: feeding it */
  SfzBuild *b;
  int (*cancel)(void *); void *carg;
  /* caps reached while reading: told in the ignored count */
  int elems_capped, attrs_capped, tags_capped;
  char num[64];
  /* Decent Sampler's knobs (pass 1, made afresh for each feed): the description, or NULL for a preset without
     knobs or effects (built as before); and the layer of the group being walked */
  SfKnobs *k;
  int layer;
  /* a preset whose knobs no player keeps values for (past the font's SF_KNOB_PRESETS knobbed presets; knobbed
     counts them, shared by a bundle's presets, NULL for a lone one), or a layer past SF_KNOB_LAYERS: its knobs'
     starting levels (bgain, bcents per layer) baked into its regions instead */
  int *knobbed, bake_all;
  float *bgain, *bcents;
  int *gmap, ngroups, capg;               /* every <group> by its index (groupIndex counts disabled ones too, as
                                            Decent Sampler does): its layer, or 0 for a disabled one */
  int live[4];                           /* by SF_KT_*: some knob sets it, so it plays through the layer instead of
                                            being baked into the regions */
  uint8_t *greach; int capr;             /* every <group> by its index: 1 when some binding can turn it on (a group
                                            that starts disabled is then a layer, silent until it is) */
  int cur_grp, cur_gidx;                 /* the <group> being walked: its element and its index */
  int mods[SF_MOD_MAX], nmods;           /* <modulators>' children this plays, in order (modulatorIndex) */
  float bmod[SF_MOD_MAX * SF_MP_N];      /* their values: the knobs' starting ones (sf_knobs_levels_ex) */
  /* several presets in one font (sf_parse_ds_many): this preset's folder as UTF-8 with a trailing backslash, put
     before its relative sample paths (the builder's own folder is the font's); empty for a lone preset */
  char dir[1024];
  char pathbuf[2100];
} D;

/* ---- small helpers ---- */

static uint64_t fnv(const void *b, size_t n) {
  const uint8_t *p = b; uint64_t h = 1469598103934665603ull; size_t i;
  for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}

static int grow(void **a, int *cap, int need, size_t each) {
  void *n; int c;
  if (need <= *cap) return 1;
  c = *cap ? *cap : 64;
  while (c < need) { if (c > (1 << 28)) return 0; c *= 2; }
  if (!(n = realloc(*a, (size_t)c * each))) return 0;
  *a = n; *cap = c;
  return 1;
}

static int sp(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* a number; 0 when the text isn't one */
static int num(const char *v, double *out) {
  char *e; double d;
  if (!v) return 0;
  while (sp(*v)) v++;
  d = strtod(v, &e);
  if (e == v || !isfinite(d)) return 0;
  *out = d;
  return 1;
}

/* a level: decibels with a dB suffix, else linear (1 is 0 dB); 0 when the text isn't a number */
static int level_db(const char *v, double *db) {
  char *e; double d;
  if (!v) return 0;
  while (sp(*v)) v++;
  d = strtod(v, &e);
  if (e == v || !isfinite(d)) return 0;
  while (sp(*e)) e++;
  if ((e[0] == 'd' || e[0] == 'D') && (e[1] == 'b' || e[1] == 'B')) *db = d;
  else *db = d > 0 ? 20 * log10(d) : -144;
  if (*db < -144) *db = -144;
  if (*db > 24) *db = 24;
  return 1;
}

static int is_true(const char *v) {
  while (v && sp(*v)) v++;
  return v && (!_strnicmp(v, "true", 4) || *v == '1');
}

static int is_false(const char *v) {
  while (v && sp(*v)) v++;
  return v && (!_strnicmp(v, "false", 5) || *v == '0');
}

static int word(const char *v, const char *w) {
  size_t n = strlen(w);
  while (v && sp(*v)) v++;
  if (!v || _strnicmp(v, w, n)) return 0;
  v += n;
  while (sp(*v)) v++;
  return !*v;
}

/* ---- the map of tags ---- */

static int map_find(const Map *m, const char *k, size_t n) {
  unsigned i;
  if (!m->cap) return 0;
  i = (unsigned)fnv(k, n) & (unsigned)(m->cap - 1);
  while (m->key[i]) {
    if (strlen(m->key[i]) == n && !memcmp(m->key[i], k, n)) return m->val[i];
    i = (i + 1) & (unsigned)(m->cap - 1);
  }
  return 0;
}

/* adds k (n bytes) with the next number unless it is there; 0 when memory ran out */
static int map_add(Map *m, const char *k, size_t n) {
  unsigned i;
  if (map_find(m, k, n)) return 1;
  if ((m->n + 1) * 2 > m->cap) {
    int nc = m->cap ? m->cap * 2 : 64, j;
    char **nk = calloc((size_t)nc, sizeof *nk); int *nv = calloc((size_t)nc, sizeof *nv);
    if (!nk || !nv || m->n >= (1 << 20)) { free(nk); free(nv); return 0; }
    for (j = 0; j < m->cap; j++) if (m->key[j]) {
      unsigned q = (unsigned)fnv(m->key[j], strlen(m->key[j])) & (unsigned)(nc - 1);
      while (nk[q]) q = (q + 1) & (unsigned)(nc - 1);
      nk[q] = m->key[j]; nv[q] = m->val[j];
    }
    free(m->key); free(m->val); m->key = nk; m->val = nv; m->cap = nc;
  }
  i = (unsigned)fnv(k, n) & (unsigned)(m->cap - 1);
  while (m->key[i]) i = (i + 1) & (unsigned)(m->cap - 1);
  if (!(m->key[i] = malloc(n + 1))) return 0;
  memcpy(m->key[i], k, n); m->key[i][n] = 0;
  m->val[i] = ++m->n;
  return 1;
}

static void map_free(Map *m) {
  int j;
  for (j = 0; j < m->cap; j++) free(m->key[j]);
  free(m->key); free(m->val);
  memset(m, 0, sizeof *m);
}

/* ---- the XML into the tree ---- */

static int pool_add(D *d, const char *s, size_t n) {
  size_t need = d->npool + n + 1;
  if (need > d->cappool) {
    size_t c = d->cappool ? d->cappool : 4096;
    char *np;
    while (c < need) c *= 2;
    if (c > ((size_t)DS_MAX_TEXT * 3 + DS_MAX_VALUE) || !(np = realloc(d->pool, c))) return -1;
    d->pool = np; d->cappool = c;
  }
  memcpy(d->pool + d->npool, s, n); d->pool[d->npool + n] = 0;
  d->npool += n + 1;
  return (int)(d->npool - n - 1);
}

/* Decent Sampler (JUCE's XML parser) reads an attribute that follows another's closing quote with no space between
   them (name="a"next="b"); yxml, rightly, doesn't. This copies in to out (room for 2 n bytes) with one space put
   there, inside tags only: text, comments, CDATA, processing instructions and declarations are copied as they are,
   and so is a quote of the other kind inside a value. Returns out's length. */
static size_t ds_fix_attr_spaces(const char *in, size_t n, char *out) {
  enum { TEXT, TAG, VALUE, COMMENT, CDATA, PI, DECL } st = TEXT;
  char q = 0;
  int after_value = 0;
  size_t i, o = 0;
  for (i = 0; i < n; i++) {
    char c = in[i];
    switch (st) {
      case TEXT:
        if (c == '<') {
          if (i + 3 < n && !memcmp(in + i, "<!--", 4)) st = COMMENT;
          else if (i + 8 < n && !memcmp(in + i, "<![CDATA[", 9)) st = CDATA;
          else if (i + 1 < n && in[i + 1] == '?') st = PI;
          else if (i + 1 < n && in[i + 1] == '!') st = DECL;
          else { st = TAG; after_value = 0; }
        }
        break;
      case TAG:
        if (after_value && (isalpha((unsigned char)c) || c == '_' || c == ':' || (unsigned char)c >= 0x80)) out[o++] = ' ';
        after_value = 0;
        if (c == '"' || c == '\'') { st = VALUE; q = c; }
        else if (c == '>') st = TEXT;
        break;
      case VALUE:
        if (c == q) { st = TAG; after_value = 1; }
        break;
      case COMMENT: if (c == '>' && i >= 2 && in[i - 1] == '-' && in[i - 2] == '-') st = TEXT; break;
      case CDATA: if (c == '>' && i >= 2 && in[i - 1] == ']' && in[i - 2] == ']') st = TEXT; break;
      case PI: if (c == '>' && i >= 1 && in[i - 1] == '?') st = TEXT; break;
      case DECL: if (c == '>') st = TEXT; break;
    }
    out[o++] = c;
  }
  return o;
}

/* the tree of t (n bytes): 1, or 0 with why */
static int parse_xml(D *d, const char *t, size_t n, char *why, int wcap) {
  yxml_t x;
  void *stack = malloc(DS_STACK);
  char *val = malloc(DS_MAX_VALUE + 1);
  int cur[DS_LEVELS], depth = 0, attr_el = -1, ok = 0, k, vfull = 0;
  size_t vn = 0, i;
  if (!stack || !val) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
  for (k = 0; k < DS_LEVELS; k++) cur[k] = -1;
  yxml_init(&x, stack, DS_STACK);
  for (i = 0; i < n; i++) {
    yxml_ret_t r = yxml_parse(&x, (unsigned char)t[i]);
    if (r < 0) { snprintf(why, (size_t)wcap, "its XML is damaged near line %u", (unsigned)x.line); goto done; }
    switch (r) {
      case YXML_ELEMSTART: {
        int parent = depth > 0 && depth <= DS_LEVELS ? cur[depth - 1] : -1, e = -1;
        if (depth < DS_LEVELS && (depth == 0 || parent >= 0) && d->nel >= DS_MAX_ELEMS) d->elems_capped = 1;
        if (depth < DS_LEVELS && (depth == 0 || parent >= 0) && d->nel < DS_MAX_ELEMS &&
            grow((void **)&d->el, &d->capel, d->nel + 1, sizeof *d->el)) {
          int nm = pool_add(d, x.elem, strlen(x.elem));
          if (nm < 0) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
          e = d->nel++;
          d->el[e].name = nm; d->el[e].first_attr = d->nat; d->el[e].nattr = 0;
          d->el[e].first_child = d->el[e].last_child = d->el[e].next = -1;
          if (parent >= 0) {
            if (d->el[parent].last_child >= 0) d->el[d->el[parent].last_child].next = e;
            else d->el[parent].first_child = e;
            d->el[parent].last_child = e;
          }
        }
        if (depth < DS_LEVELS) cur[depth] = e;
        depth++;
        attr_el = depth <= DS_LEVELS ? cur[depth - 1] : -1;
        break;
      }
      case YXML_ELEMEND:
        if (depth > 0) depth--;
        if (depth < DS_LEVELS) cur[depth] = -1;
        attr_el = -1;
        break;
      case YXML_ATTRSTART: vn = 0; vfull = 0; break;
      case YXML_ATTRVAL: {
        size_t L = strlen(x.data);
        /* past 64 KB nothing more is kept, so the value is its first 64 KB, never pieces spliced together */
        if (!vfull && vn + L <= DS_MAX_VALUE) { memcpy(val + vn, x.data, L); vn += L; }
        else vfull = 1;
        break;
      }
      case YXML_ATTREND:
        if (attr_el >= 0 && d->nat >= DS_MAX_ATTRS) d->attrs_capped = 1;
        if (attr_el >= 0 && d->el[attr_el].first_attr + d->el[attr_el].nattr == d->nat && d->nat < DS_MAX_ATTRS &&
            grow((void **)&d->at, &d->capat, d->nat + 1, sizeof *d->at)) {
          int an = pool_add(d, x.attr, strlen(x.attr)), av = an >= 0 ? pool_add(d, val, vn) : -1;
          if (av < 0) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
          d->at[d->nat].name = an; d->at[d->nat].value = av; d->nat++;
          d->el[attr_el].nattr++;
        }
        break;
      default: break;
    }
  }
  if (yxml_eof(&x) < 0) { snprintf(why, (size_t)wcap, "its XML is damaged (it ends early)"); goto done; }
  if (!d->nel || strcmp(d->pool + d->el[0].name, "DecentSampler")) {
    snprintf(why, (size_t)wcap, "it has no <DecentSampler> element");
    goto done;
  }
  ok = 1;
done:
  free(stack); free(val);
  return ok;
}

/* ---- the levels ---- */

static void ignored(D *d, const char *name) {
  if (d->pass == 1) sfz_ignored(d->b, name);
}

/* the next tag of a comma-separated list from *p: its start and length (0 at the end) */
static size_t next_tag(const char **p, const char **start) {
  const char *s = *p, *e;
  size_t n;
  while (*s && (sp(*s) || *s == ',')) s++;
  for (e = s; *e && *e != ','; e++) {}
  *p = e;
  for (n = (size_t)(e - s); n && sp(s[n - 1]); n--) {}
  *start = s;
  return n;
}

/* an element's attributes by number; others counted (unless count is 0) */
static void look(D *d, int e, Look *l, int count) {
  int k, j;
  memset(l, 0, sizeof *l);
  if (e < 0) return;
  for (k = 0; k < d->el[e].nattr; k++) {
    const Attr *a = &d->at[d->el[e].first_attr + k];
    const char *nm = d->pool + a->name, *v = d->pool + a->value;
    for (j = 0; j < NA && strcmp(nm, anames[j]); j++) {}
    if (j < NA) { l->a[j] = v; continue; }
    if ((!strncmp(nm, "loCC", 4) || !strncmp(nm, "hiCC", 4)) && nm[4] >= '0' && nm[4] <= '9') {
      char *end; long c = strtol(nm + 4, &end, 10);
      if (!*end && c >= 0 && c <= 127) {
        int i;
        for (i = 0; i < l->ncc && l->cc[i] != c; i++) {}
        if (i < MAX_CC) {
          if (i == l->ncc) { l->cc[i] = (int)c; l->cclo[i] = l->cchi[i] = NULL; l->ncc++; }
          if (nm[0] == 'l') l->cclo[i] = v; else l->cchi[i] = v;
          continue;
        }
      }
    }
    if (count) ignored(d, nm);
  }
  /* tags, read once per element: pass 0 numbers every tag named in a silencedByTags; pass 1 finds this element's
     first silencing tag and first silencer (and whether it has more than one of either) */
  {
    const char *v, *t; size_t n;
    if (d->pass == 0) {
      if ((v = l->a[A_SILENCEDBY]) != NULL)
        while ((n = next_tag(&v, &t)) != 0) if (!map_add(&d->silencers, t, n)) { d->tags_capped = 1; break; }
      return;
    }
    if ((v = l->a[A_TAGS]) != NULL)
      while ((n = next_tag(&v, &t)) != 0) {
        int id = map_find(&d->silencers, t, n);
        if (!id || id == l->tag_id) continue;
        if (!l->tag_id) l->tag_id = id; else { l->tag_more = 1; break; }
      }
    if ((v = l->a[A_SILENCEDBY]) != NULL)
      while ((n = next_tag(&v, &t)) != 0) {
        int id = map_find(&d->silencers, t, n);
        if (!id || id == l->by_id) continue;
        if (!l->by_id) l->by_id = id; else { l->by_more = 1; break; }
      }
  }
}

/* the value of attribute a for a sample: its own, else its group's, else <groups>' */
static const char *inh(const Look *s, const Look *g, const Look *gs, int a) {
  return s->a[a] ? s->a[a] : g->a[a] ? g->a[a] : gs->a[a];
}

/* an envelope value: ampEnvX wins over x on the same element, the sample's over its group's over <groups>' */
static const char *env(const Look *s, const Look *g, const Look *gs, int a_env, int a) {
  const Look *lv[3] = {s, g, gs};
  int k;
  for (k = 0; k < 3; k++) {
    if (lv[k]->a[a_env]) return lv[k]->a[a_env];
    if (a >= 0 && lv[k]->a[a]) return lv[k]->a[a];
  }
  return NULL;
}

static int note_num(const char *v, int def) {
  double x;
  if (!num(v, &x)) return def;
  x = floor(x + 0.5);
  return x < -1 ? -1 : x > 128 ? 128 : (int)x;
}

enum { SEQ_ALWAYS, SEQ_RR, SEQ_RANDOM, SEQ_BAD };

static int seq_mode(const char *v) {
  if (!v || word(v, "always")) return SEQ_ALWAYS;
  if (word(v, "round_robin")) return SEQ_RR;
  if (word(v, "random") || word(v, "true_random")) return SEQ_RANDOM;
  return SEQ_BAD;
}

static void op(D *d, const char *name, const char *value) { sfz_opcode(d->b, name, value); }

static void opn(D *d, const char *name, double v) {
  snprintf(d->num, sizeof d->num, "%.9g", v);
  sfz_opcode(d->b, name, d->num);
}

static const char *el_name(const D *d, int e);
static const char *attr(const D *d, int e, const char *name);
static float attr_f(const D *d, int e, const char *name, float def);
static int tags_meet(const char *a, const char *b);

/* a group effect's type as SFZ's filter (fil_type), or NULL for one that isn't a filter */
static const char *fx_fil(const char *ty) {
  if (!ty) return NULL;
  if (word(ty, "lowpass") || word(ty, "lowpass_4pl")) return "lpf_2p";
  if (word(ty, "lowpass_1pl")) return "lpf_1p";
  if (word(ty, "highpass")) return "hpf_2p";
  if (word(ty, "bandpass")) return "bpf_2p";
  return NULL;
}

/* group element grp's own <effects>: its first two filters' elements (-1 none) in fil, and, by its effects' order
   (effectIndex), each one's filter number (0, 1, or -1 for any other effect) in slot (n of them, at most 16) */
static int group_fx(const D *d, int grp, int fil[2], int slot[16]) {
  int c, e, n = 0, nf = 0;
  fil[0] = fil[1] = -1;
  if (grp < 0) return 0;
  for (c = d->el[grp].first_child; c >= 0; c = d->el[c].next) {
    if (strcmp(el_name(d, c), "effects")) continue;
    for (e = d->el[c].first_child; e >= 0; e = d->el[e].next) {
      if (strcmp(el_name(d, e), "effect")) continue;
      if (n < 16) slot[n] = -1;
      if (fx_fil(attr(d, e, "type")) && nf < 2) { fil[nf] = e; if (n < 16) slot[n] = nf; nf++; }
      n++;
    }
  }
  return n < 16 ? n : 16;
}

/* the region's filters from its group's own effects: the first filter is SFZ's fil, the second fil2; any other
   effect there is counted */
static void group_effects(D *d) {
  int fil[2], slot[16], n, c, e, i;
  n = group_fx(d, d->cur_grp, fil, slot);
  if (!n) return;
  for (i = 0; i < 2; i++) {
    double x;
    if (fil[i] < 0) continue;
    op(d, i ? "fil2_type" : "fil_type", fx_fil(attr(d, fil[i], "type")));
    x = attr_f(d, fil[i], "frequency", 22000);
    opn(d, i ? "cutoff2" : "cutoff", x < 1 ? 1 : x > 22000 ? 22000 : x);
    opn(d, i ? "resonance2" : "resonance", sf_q_db(attr_f(d, fil[i], "resonance", 0.7f)));
  }
  for (c = d->el[d->cur_grp].first_child; c >= 0; c = d->el[c].next) {
    if (strcmp(el_name(d, c), "effects")) continue;
    for (e = d->el[c].first_child; e >= 0; e = d->el[e].next) {
      const char *ty;
      char w[80];
      if (strcmp(el_name(d, e), "effect") || e == fil[0] || e == fil[1]) continue;
      ty = attr(d, e, "type");
      if (fx_fil(ty)) ignored(d, "group filters past the second");
      else { snprintf(w, sizeof w, "group effect %.60s", ty ? ty : "(no type)"); ignored(d, w); }
    }
  }
}

/* 1 when binding element b (level group or tag) reaches the <group> being walked */
static int reaches_group(const D *d, int b) {
  const char *lv = attr(d, b, "level"), *tags = lv && word(lv, "tag") ? attr(d, b, "identifier") : NULL;
  const char *gi = attr(d, b, "groupIndex") ? attr(d, b, "groupIndex") : attr(d, b, "position");
  double x;
  if (!tags) tags = attr(d, b, "tags");
  if (tags) return tags_meet(tags, attr(d, d->cur_grp, "tags"));
  return num(gi, &x) && (int)x == d->cur_gidx;
}

/* a translation's two ends: its output at the source's 0 and 1 (a table's first and last points) */
static void trans_ends(const D *d, int b, double *y0, double *y1) {
  const char *tab = attr(d, b, "translationTable");
  *y0 = attr_f(d, b, "translationOutputMin", 0); *y1 = attr_f(d, b, "translationOutputMax", 1);
  if (tab && attr(d, b, "translation") && word(attr(d, b, "translation"), "table")) {
    const char *p = tab; char *e; double x, y; int first = 1;
    while (*p) {
      while (sp(*p) || *p == ';') p++;
      if (!*p) break;
      x = strtod(p, &e); if (e == p) break; p = e; while (sp(*p)) p++;
      if (*p != ',') break;
      p++; y = strtod(p, &e); if (e == p) break; p = e;
      if (first) { *y0 = y; first = 0; }
      *y1 = y; (void)x;
    }
  }
  if (is_true(attr(d, b, "translationReversed"))) { double t = *y0; *y0 = *y1; *y1 = t; }
}

/* the modulators onto the region: each <envelope>, <lfo> and <midiCC> that reaches it (through a binding to its
   group's filters' frequency, multiplied, or to the tuning, added) becomes one of SFZ's flex envelopes or LFOs
   (egN_ / lfoN_ with N the modulator's number), its depths from the binding's two ends. A player keeps the
   modulators' values live (samplosis_dsmod); baked, their starting scale goes into the depths. */
static void modulators(D *d, int baked) {
  int i, b, fil[2], slot[16], nfx = group_fx(d, d->cur_grp, fil, slot);
  int live = d->k && !baked;
  char nm[64];
  for (i = 0; i < d->nmods; i++) {
    int m = d->mods[i], env = !strcmp(el_name(d, m), "envelope"), cc = !strcmp(el_name(d, m), "midiCC");
    double dep[3] = {0, 0, 0}, scale = live ? 1 : d->bmod[i * SF_MP_N + SF_MP_ENABLED];   /* pitch, cutoff, cutoff2 */
    const float *mv = d->bmod + i * SF_MP_N;
    for (b = d->el[m].first_child; b >= 0; b = d->el[b].next) {
      const char *ty = attr(d, b, "type"), *par = attr(d, b, "parameter"), *lv = attr(d, b, "level");
      const char *beh = attr(d, b, "modBehavior");
      double y0, y1, x;
      char w[80];
      if (strcmp(el_name(d, b), "binding")) continue;
      trans_ends(d, b, &y0, &y1);
      if (ty && word(ty, "effect") && par && word(par, "FX_FILTER_FREQUENCY") && lv && (word(lv, "group") || word(lv, "tag")) &&
          (!beh || word(beh, "multiply"))) {
        const char *ix = attr(d, b, "effectIndex") ? attr(d, b, "effectIndex") : attr(d, b, "position");
        if (!reaches_group(d, b)) continue;
        if (!num(ix, &x) || x < 0 || x >= nfx || slot[(int)x] < 0) { ignored(d, "modulator binding to an effect that isn't there"); continue; }
        if (y0 > 0 && y1 > 0) dep[1 + slot[(int)x]] += 1200 * log2(y1 / y0);
      } else if (ty && word(ty, "amp") && par && (word(par, "GLOBAL_TUNING") || word(par, "GROUP_TUNING")) && (!beh || word(beh, "add"))) {
        if (lv && (word(lv, "group") || word(lv, "tag")) && !reaches_group(d, b)) continue;
        dep[0] += 100 * (y1 - y0) / (env || cc ? 1 : 2);   /* an LFO swings both ways about the middle */
      } else {
        snprintf(w, sizeof w, "modulator binding %.40s%s%.20s", par ? par : "(no parameter)", beh ? " " : "", beh ? beh : "");
        ignored(d, w);
      }
    }
    if (dep[0] == 0 && dep[1] == 0 && dep[2] == 0) continue;
    if (!live && scale <= 0) continue;
    if (env) {
      snprintf(nm, sizeof nm, "eg%02d_", i + 1);
#define EGO(k, val) do { char t[64]; snprintf(t, sizeof t, "%s%s", nm, k); opn(d, t, val); } while (0)
      EGO("time0", 0); EGO("level0", 0);
      EGO("time1", mv[SF_MP_ATTACK]); EGO("level1", 1);
      EGO("time2", mv[SF_MP_DECAY]); EGO("level2", mv[SF_MP_SUSTAIN]);
      EGO("sustain", 2);
      EGO("time3", mv[SF_MP_RELEASE]); EGO("level3", 0);
    } else {
      const char *sh = attr(d, m, "shape");
      snprintf(nm, sizeof nm, "lfo%02d_", i + 1);
      if (cc) EGO("samplosis_cc", attr_f(d, m, "number", 1));
      else {
        EGO("freq", mv[SF_MP_FREQ]);
        EGO("wave", sh && word(sh, "square") ? 3 : sh && (word(sh, "saw") || word(sh, "sawtooth")) ? 6 : sh && word(sh, "triangle") ? 0 : 1);
      }
    }
    if (dep[0] != 0) EGO("pitch", dep[0] * scale);
    if (dep[1] != 0) EGO("cutoff", dep[1] * scale);
    if (dep[2] != 0) EGO("cutoff2", dep[2] * scale);
    if (live) EGO("samplosis_dsmod", i + 1);
#undef EGO
  }
}

/* one <sample> (with its group's and <groups>' looks): pass 0 notes its round-robin position; pass 1 feeds it */
static void sample(D *d, const Look *s, const Look *g, const Look *gs) {
  const char *v, *path = s->a[A_PATH];
  int lokey = note_num(inh(s, g, gs, A_LONOTE), 0), hikey = note_num(inh(s, g, gs, A_HINOTE), 127);
  int lovel = note_num(inh(s, g, gs, A_LOVEL), 0), hivel = note_num(inh(s, g, gs, A_HIVEL), 127);
  int mode = seq_mode(inh(s, g, gs, A_SEQMODE)), pos = 1, len = 0, k, baked;
  double x, total;
  if ((v = inh(s, g, gs, A_SEQPOSITION)) != NULL && num(v, &x)) pos = x < 1 ? 1 : x > 65535 ? 65535 : (int)floor(x + 0.5);
  if ((v = inh(s, g, gs, A_SEQLENGTH)) != NULL && num(v, &x)) len = x < 0 ? 0 : x > 65535 ? 65535 : (int)floor(x + 0.5);
  if (d->pass == 0) {
    if ((mode == SEQ_RR || mode == SEQ_RANDOM) && pos > d->seq_max[mode == SEQ_RANDOM]) d->seq_max[mode == SEQ_RANDOM] = pos;
    return;
  }
  if (!path || !path[0]) return;                     /* no sample to play */
  /* volume, tuning and pan: the levels combined */
  /* with knobs, a group's or <groups>' volume or tuning that some knob sets plays through the layer instead (or,
     where no player can reach it, at the knobs' starting levels, baked in) */
  baked = d->k && d->bgain && (d->bake_all || d->layer > SF_KNOB_LAYERS);
  if (baked && d->bgain[d->layer] <= 0) return;      /* a group that is off and no player can turn on */
  sfz_header(d->b, "region");
  if (d->dir[0] && !(path[0] == '/' || path[0] == '\\' || (path[0] && path[1] == ':'))) {
    snprintf(d->pathbuf, sizeof d->pathbuf, "%s%s", d->dir, path);
    op(d, "sample", d->pathbuf);
  } else op(d, "sample", path);
  opn(d, "pitch_keycenter", note_num(inh(s, g, gs, A_ROOT), 60));
  opn(d, "lokey", lokey); opn(d, "hikey", hikey); opn(d, "lovel", lovel); opn(d, "hivel", hivel);
  if (d->k && !baked) opn(d, "samplosis_layer", d->layer);
  if (baked && d->layer > SF_KNOB_LAYERS && !d->bake_all) ignored(d, "layers past the 1024th (at their knobs' starting levels)");
  total = 0;
  if (baked) total += d->bgain[d->layer] > 0 ? 20 * log10(d->bgain[d->layer]) : -144;
  if (!d->live[SF_KT_GLOBAL_VOL] && level_db(gs->a[A_GLOBALVOLUME] ? gs->a[A_GLOBALVOLUME] : gs->a[A_VOLUME], &x)) total += x;
  if (!d->live[SF_KT_GROUP_VOL] && level_db(g->a[A_GROUPVOLUME] ? g->a[A_GROUPVOLUME] : g->a[A_VOLUME], &x)) total += x;
  if (level_db(s->a[A_VOLUME], &x)) total += x;
  opn(d, "volume", total < -144 ? -144 : total > 24 ? 24 : total);   /* the builder's range */
  total = 0;
  if (baked) total += d->bcents[d->layer] / 100.0;
  if (!d->live[SF_KT_GLOBAL_TUNE] && num(gs->a[A_GLOBALTUNING], &x)) total += x;
  if (!d->live[SF_KT_GROUP_TUNE] && num(g->a[A_GROUPTUNING], &x)) total += x;
  if (num(inh(s, g, gs, A_TUNING), &x)) total += x;
  opn(d, "transpose", total < -120 ? -120 : total > 120 ? 120 : total);
  total = 0;
  if (num(gs->a[A_GLOBALPAN], &x)) total += x;
  if (num(g->a[A_GROUPPAN], &x)) total += x;
  if (num(inh(s, g, gs, A_PAN), &x)) total += x;
  opn(d, "pan", total < -100 ? -100 : total > 100 ? 100 : total);
  if (num(inh(s, g, gs, A_PITCHKEYTRACK), &x)) opn(d, "pitch_keytrack", x * 100);
  if (num(inh(s, g, gs, A_AMPVELTRACK), &x)) opn(d, "amp_veltrack", x * 100);
  op(d, "amp_velcurve_127", "1");      /* gain linear in velocity (63 is 0.5 at ampVelTrack 1), as the guide says */
  /* playback and the loop */
  if ((v = inh(s, g, gs, A_START)) != NULL) op(d, "offset", v);
  if ((v = inh(s, g, gs, A_END)) != NULL) op(d, "end", v);
  if ((v = inh(s, g, gs, A_LOOPSTART)) != NULL) op(d, "loop_start", v);
  if ((v = inh(s, g, gs, A_LOOPEND)) != NULL) op(d, "loop_end", v);
  v = inh(s, g, gs, A_LOOPENABLED);
  if (is_true(v)) { op(d, "loop_mode", "loop_continuous"); sfz_loop_whole(d->b); }
  else if (is_false(v)) op(d, "loop_mode", "no_loop");
  if (!is_false(v) && is_false(inh(s, g, gs, A_LOOPDURINGREL))) op(d, "loop_mode", "loop_sustain");   /* out at the release */
  if (is_false(inh(s, g, gs, A_AMPENVENABLED)) && !is_true(v)) op(d, "loop_mode", "one_shot");
  if (num(inh(s, g, gs, A_LOOPXFADE), &x) && x > 0) ignored(d, "loopCrossfade");
  /* the amplitude envelope */
  if ((v = env(s, g, gs, A_ENVDELAY, -1)) != NULL) op(d, "ampeg_delay", v);
  if ((v = env(s, g, gs, A_ENVATTACK, A_ATTACK)) != NULL) op(d, "ampeg_attack", v);
  if ((v = env(s, g, gs, A_ENVHOLD, -1)) != NULL) op(d, "ampeg_hold", v);
  if ((v = env(s, g, gs, A_ENVDECAY, A_DECAY)) != NULL) op(d, "ampeg_decay", v);
  if ((v = env(s, g, gs, A_ENVSUSTAIN, A_SUSTAIN)) != NULL && num(v, &x)) opn(d, "ampeg_sustain", x * 100);
  if ((v = env(s, g, gs, A_ENVRELEASE, A_RELEASE)) != NULL) op(d, "ampeg_release", v);
  /* conditions */
  if ((v = inh(s, g, gs, A_TRIGGER)) != NULL) {
    if (word(v, "attack") || word(v, "release") || word(v, "first") || word(v, "legato")) {
      const char *t; size_t n = next_tag(&v, &t);
      char w[16];
      if (n < sizeof w) { memcpy(w, t, n); w[n] = 0; op(d, "trigger", w); }
    } else {
      char w[64]; snprintf(w, sizeof w, "trigger=%.40s", v); ignored(d, w);
    }
  }
  if ((v = inh(s, g, gs, A_RTDECAY)) != NULL) {
    char *e; double r;
    const char *q = v;
    while (sp(*q)) q++;
    r = strtod(q, &e);
    if (e != q && isfinite(r)) {
      while (sp(*e)) e++;
      if ((e[0] == 'd' || e[0] == 'D') && (e[1] == 'b' || e[1] == 'B')) r = fabs(r);   /* a decay either sign */
      else r = r <= 0 ? 0 : r >= 1 ? 200 : -20 * log10(1 - r);   /* linear: the share lost a second */
      opn(d, "rt_decay", r < 0 ? 0 : r > 200 ? 200 : r);
    }
  }
  if (mode == SEQ_RR || mode == SEQ_RANDOM) {
    if (!len) len = d->seq_max[mode == SEQ_RANDOM] > 0 ? d->seq_max[mode == SEQ_RANDOM] : pos;
    if (len < 1) len = 1;
    if (pos > len) pos = len;
    if (mode == SEQ_RR) { opn(d, "seq_length", len); opn(d, "seq_position", pos); }
    else { opn(d, "lorand", (double)(pos - 1) / len); opn(d, "hirand", (double)pos / len); }
  } else if (mode == SEQ_BAD) {
    char w[64]; snprintf(w, sizeof w, "seqMode=%.40s", inh(s, g, gs, A_SEQMODE)); ignored(d, w);
  }
  /* tags: the sample's group is its first silencing tag (its own, then its group's), off_by its first silencer */
  {
    const Look *byl = s->a[A_SILENCEDBY] ? s : g->a[A_SILENCEDBY] ? g : gs;
    int grp = s->tag_id ? s->tag_id : g->tag_id, by = byl->by_id;
    int extra = s->tag_more || g->tag_more || (s->tag_id && g->tag_id && s->tag_id != g->tag_id) || byl->by_more;
    if (grp) opn(d, "group", grp);
    if (by) opn(d, "off_by", by);
    if (extra) ignored(d, "tags beyond the first");
    if ((v = inh(s, g, gs, A_SILENCINGMODE)) != NULL) {
      if (word(v, "fast")) op(d, "off_mode", "fast");
      else if (word(v, "normal")) op(d, "off_mode", "normal");
      else { char w[64]; snprintf(w, sizeof w, "silencingMode=%.40s", v); ignored(d, w); }
    }
  }
  /* the output: everything plays on the main one here, which is where MAIN_OUTPUT (the usual value) sends it */
  if ((v = inh(s, g, gs, A_OUTPUT1)) != NULL && !word(v, "MAIN_OUTPUT")) {
    char w[64]; snprintf(w, sizeof w, "output1Target=%.40s", v); ignored(d, w);
  }
  group_effects(d);
  modulators(d, baked);
  /* CC ranges: the sample's own, else its group's, else <groups>' */
  {
    const Look *lv[3] = {s, g, gs};
    char nm[16];
    for (k = 0; k < 3; k++) {
      int i;
      if (!lv[k]->ncc) continue;
      for (i = 0; i < lv[k]->ncc; i++) {
        if (lv[k]->cclo[i] && num(lv[k]->cclo[i], &x) && x >= 0) { snprintf(nm, sizeof nm, "locc%d", lv[k]->cc[i]); opn(d, nm, x); }
        if (lv[k]->cchi[i] && num(lv[k]->cchi[i], &x) && x >= 0) { snprintf(nm, sizeof nm, "hicc%d", lv[k]->cc[i]); opn(d, nm, x); }
      }
      break;
    }
  }
}

/* the whole tree, once a pass: 0 when cancelled */
static int walk(D *d) {
  int c, done = 0, gidx;
  Look gs, g, s;
  d->layer = 0;
  for (c = d->el[0].first_child; c >= 0; c = d->el[c].next) {
    int grp;
    if (strcmp(d->pool + d->el[c].name, "groups")) {
      const char *nm = d->pool + d->el[c].name;
      char w[64];
      if (!strcmp(nm, "ui") || !strcmp(nm, "effects") || !strcmp(nm, "midi") || !strcmp(nm, "modulators")) continue;   /* read by read_knobs */
      snprintf(w, sizeof w, "<%.50s>", nm); ignored(d, w);
      continue;
    }
    look(d, c, &gs, 1);
    gidx = 0;
    for (grp = d->el[c].first_child; grp >= 0; grp = d->el[grp].next) {
      int smp;
      if (strcmp(d->pool + d->el[grp].name, "group")) {
        char w[64]; snprintf(w, sizeof w, "<%.50s>", d->pool + d->el[grp].name); ignored(d, w);
        continue;
      }
      look(d, grp, &g, 1);
      gidx++;
      if (is_false(g.a[A_ENABLED]) && !(d->greach && gidx - 1 < d->capr && d->greach[gidx - 1])) continue;
      d->layer++;
      d->cur_grp = grp; d->cur_gidx = gidx - 1;
      for (smp = d->el[grp].first_child; smp >= 0; smp = d->el[smp].next) {
        if (!strcmp(d->pool + d->el[smp].name, "effects")) continue;   /* the group's own: on each region */
        if (strcmp(d->pool + d->el[smp].name, "sample")) {
          char w[64]; snprintf(w, sizeof w, "<%.50s>", d->pool + d->el[smp].name); ignored(d, w);
          continue;
        }
        look(d, smp, &s, 1);
        sample(d, &s, &g, &gs);
        if ((++done & 0xFFF) == 0 && (d->b ? sfz_cancelled(d->b) : d->cancel && d->cancel(d->carg))) return 0;
      }
    }
  }
  return 1;
}

/* ---- the knobs: <ui> controls and their bindings, <effects>, <midi> CC bindings (sfknob.h) ---- */

static const char *el_name(const D *d, int e) { return d->pool + d->el[e].name; }

static const char *attr(const D *d, int e, const char *name) {
  int k;
  for (k = 0; k < d->el[e].nattr; k++) {
    const Attr *a = &d->at[d->el[e].first_attr + k];
    if (!strcmp(d->pool + a->name, name)) return d->pool + a->value;
  }
  return NULL;
}

static float attr_f(const D *d, int e, const char *name, float def) {
  double x;
  return num(attr(d, e, name), &x) ? (float)x : def;
}

/* 1 when the comma-separated lists a and b share a tag */
static int tags_meet(const char *a, const char *b) {
  const char *p = a, *t, *q, *u;
  size_t n, m;
  if (!a || !b) return 0;
  while ((n = next_tag(&p, &t)) != 0) {
    q = b;
    while ((m = next_tag(&q, &u)) != 0) if (m == n && !memcmp(t, u, n)) return 1;
  }
  return 0;
}

/* a binding's or a CC's translation; linear's output defaults to lo..hi */
static void read_trans(const D *d, int e, SfTrans *t, float lo, float hi) {
  const char *tr = attr(d, e, "translation"), *tab = attr(d, e, "translationTable");
  memset(t, 0, sizeof *t);
  t->lo = attr_f(d, e, "translationOutputMin", lo);
  t->hi = attr_f(d, e, "translationOutputMax", hi);
  if (is_true(attr(d, e, "translationReversed"))) { float x = t->lo; t->lo = t->hi; t->hi = x; }
  if (tr && word(tr, "fixed_value")) {
    const char *tv = attr(d, e, "translationValue");
    t->kind = SF_TR_FIXED;
    t->fixed = tv && (word(tv, "true") || word(tv, "false")) ? (word(tv, "true") ? 1.0f : 0.0f) : attr_f(d, e, "translationValue", 0);
  }
  else if (tr && word(tr, "table") && tab) {
    const char *p = tab;
    t->kind = SF_TR_TABLE;
    while (*p && t->npts < SF_TABLE_MAX) {
      char *e1, *e2; double x, y;
      while (sp(*p) || *p == ';') p++;
      if (!*p) break;
      x = strtod(p, &e1);
      if (e1 == p) break;
      p = e1; while (sp(*p)) p++;
      if (*p != ',') break;
      p++;
      y = strtod(p, &e2);
      if (e2 == p) break;
      p = e2;
      t->x[t->npts] = (float)x; t->y[t->npts] = (float)y; t->npts++;
    }
    if (!t->npts) t->kind = SF_TR_LINEAR;
  }
}

typedef struct { const char *tags; float vol, tune, fx[4]; uint8_t en; } Layer;

/* the layers (enabled <group>s in order) with their tags, volume and tuning; and <groups>' own; and d->gmap */
static int read_layers(D *d, Layer **out, float *gvol, float *gtune) {
  int c, grp, n = 0, cap = 0;
  double x;
  *out = NULL; *gvol = 1; *gtune = 0;
  d->ngroups = 0;
  for (c = d->el[0].first_child; c >= 0; c = d->el[c].next) {
    if (strcmp(el_name(d, c), "groups")) continue;
    if (level_db(attr(d, c, "globalVolume") ? attr(d, c, "globalVolume") : attr(d, c, "volume"), &x)) *gvol = (float)pow(10, x / 20);
    if (num(attr(d, c, "globalTuning"), &x)) *gtune = (float)x;
    for (grp = d->el[c].first_child; grp >= 0; grp = d->el[grp].next) {
      Layer *L;
      if (strcmp(el_name(d, grp), "group")) continue;
      if (!grow((void **)&d->gmap, &d->capg, d->ngroups + 1, sizeof *d->gmap)) return -1;
      {
        int on = !is_false(attr(d, grp, "enabled")), reach = d->greach && d->ngroups < d->capr && d->greach[d->ngroups];
        int fil[2], slot[16], i;
        d->gmap[d->ngroups++] = on || reach ? n + 1 : 0;
        if (!on && !reach) continue;
        if (!grow((void **)out, &cap, n + 1, sizeof **out)) return -1;
        L = &(*out)[n++];
        L->en = (uint8_t)on;
        group_fx(d, grp, fil, slot);
        for (i = 0; i < 2; i++) {
          L->fx[2 * i] = fil[i] >= 0 ? attr_f(d, fil[i], "frequency", 22000) : 0;
          L->fx[2 * i + 1] = fil[i] >= 0 ? attr_f(d, fil[i], "resonance", 0.7f) : 0;
        }
      }
      L->tags = attr(d, grp, "tags");
      L->vol = level_db(attr(d, grp, "groupVolume") ? attr(d, grp, "groupVolume") : attr(d, grp, "volume"), &x) ? (float)pow(10, x / 20) : 1;
      L->tune = num(attr(d, grp, "groupTuning"), &x) ? (float)x : 0;
    }
  }
  return n;
}

static int add_layer(SfKnobs *k, int *cap, int layer) {
  if (!grow((void **)&k->layer_list, cap, k->nlayer_list + 1, sizeof *k->layer_list)) return 0;
  k->layer_list[k->nlayer_list++] = (uint16_t)layer;
  return 1;
}

/* a binding's layers (level group by groupIndex or tags, level tag by identifier) into b: 0 when it names no
   group (counted) or memory ran out */
static int bind_layers(D *d, int e, SfKnobs *k, SfKBind *b, const Layer *layers, int nlayer, int *lcap) {
  const char *level = attr(d, e, "level"), *tags = level && word(level, "tag") ? attr(d, e, "identifier") : NULL;
  const char *gi = attr(d, e, "groupIndex") ? attr(d, e, "groupIndex") : attr(d, e, "position");
  double x; int i;
  if (!tags) tags = attr(d, e, "tags");
  b->first_layer = (uint16_t)k->nlayer_list;
  if (tags) {
    for (i = 0; i < nlayer; i++) if (tags_meet(tags, layers[i].tags) && !add_layer(k, lcap, i + 1)) return 0;
  } else if (num(gi, &x) && x >= 0 && x < d->ngroups) {
    if (d->gmap[(int)x] && !add_layer(k, lcap, d->gmap[(int)x])) return 0;   /* a disabled group: nothing to set */
  } else { ignored(d, "binding to a group that isn't there"); return 0; }
  b->nlayers = (uint16_t)(k->nlayer_list - b->first_layer);
  return 1;
}

/* one <binding> of knob n: added to it, or counted */
static void read_binding(D *d, int e, SfKnobs *k, SfKnob *n, const Layer *layers, int nlayer, int *lcap) {
  const char *type = attr(d, e, "type"), *level = attr(d, e, "level"), *par = attr(d, e, "parameter");
  int group = level && (word(level, "group") || word(level, "tag"));
  SfKBind b;
  char w[80];
  memset(&b, 0, sizeof b);
  b.param = SF_FXP_NONE; b.state = -1;
  if (!par) { ignored(d, "binding without a parameter"); return; }
  if (n->nb >= SF_KBIND_MAX) { ignored(d, "too many bindings on a knob"); return; }
  if (type && (word(type, "general") || word(type, "amp")) && word(par, "ENABLED") && group) {
    b.target = SF_KT_GROUP_EN;
    if (!bind_layers(d, e, k, &b, layers, nlayer, lcap)) return;
  } else if (type && word(type, "amp") && (word(par, "ENV_ATTACK") || word(par, "ENV_DECAY") || word(par, "ENV_SUSTAIN") ||
                                           word(par, "ENV_RELEASE"))) {   /* the amplitude envelope, per group */
    b.target = SF_KT_GROUP_ENV;
    b.param = (int8_t)(word(par, "ENV_ATTACK") ? 0 : word(par, "ENV_DECAY") ? 1 : word(par, "ENV_SUSTAIN") ? 2 : 3);
    if (group) { if (!bind_layers(d, e, k, &b, layers, nlayer, lcap)) return; }
    else {                                      /* the instrument's: every layer */
      int i;
      b.first_layer = (uint16_t)k->nlayer_list;
      for (i = 0; i < nlayer; i++) if (!add_layer(k, lcap, i + 1)) return;
      b.nlayers = (uint16_t)(k->nlayer_list - b.first_layer);
    }
  } else if (type && word(type, "amp") && (word(par, "AMP_VOLUME") || word(par, "GLOBAL_TUNING") || word(par, "GROUP_TUNING"))) {
    if (word(par, "GLOBAL_TUNING")) b.target = SF_KT_GLOBAL_TUNE;
    else if (word(par, "GROUP_TUNING")) b.target = group ? SF_KT_GROUP_TUNE : SF_KT_GLOBAL_TUNE;
    else b.target = group ? SF_KT_GROUP_VOL : SF_KT_GLOBAL_VOL;
    if ((b.target == SF_KT_GROUP_VOL || b.target == SF_KT_GROUP_TUNE) && !bind_layers(d, e, k, &b, layers, nlayer, lcap)) return;
  } else if (type && word(type, "modulator")) {
    static const struct { const char *name; int param; } M[] = {
      {"ENABLED", SF_MP_ENABLED}, {"MOD_AMOUNT", SF_MP_AMOUNT}, {"ENV_ATTACK", SF_MP_ATTACK}, {"ENV_DECAY", SF_MP_DECAY},
      {"ENV_SUSTAIN", SF_MP_SUSTAIN}, {"ENV_RELEASE", SF_MP_RELEASE}, {"FREQUENCY", SF_MP_FREQ}, {"LFO_RATE", SF_MP_FREQ},
    };
    const char *ix = attr(d, e, "modulatorIndex") ? attr(d, e, "modulatorIndex") : attr(d, e, "position");
    double x; size_t i;
    for (i = 0; i < sizeof M / sizeof *M && !word(par, M[i].name); i++) {}
    if (i == sizeof M / sizeof *M) { snprintf(w, sizeof w, "binding %.60s", par); ignored(d, w); return; }
    if (!num(ix, &x) || x < 0 || x >= d->nmods) { ignored(d, "binding to a modulator that isn't there"); return; }
    b.target = SF_KT_MOD; b.mod = (uint8_t)x; b.param = (int8_t)M[i].param;
  } else if (type && word(type, "effect") && group) {   /* a group's own effect: its filters' frequency and resonance */
    const char *ix = attr(d, e, "effectIndex") ? attr(d, e, "effectIndex") : attr(d, e, "position");
    double x; int fil[2], slot[16], nfx, gel = -1, c, gi = 0;
    if (!word(par, "FX_FILTER_FREQUENCY") && !word(par, "FX_FILTER_RESONANCE") && !word(par, "FX_FILTER_Q")) {
      snprintf(w, sizeof w, "binding %.60s on a group's effect", par); ignored(d, w); return;
    }
    if (!bind_layers(d, e, k, &b, layers, nlayer, lcap)) return;
    if (b.nlayers) {                            /* the first layer's group: its effects' order */
      int want = k->layer_list[b.first_layer];
      for (c = d->el[0].first_child; c >= 0 && gel < 0; c = d->el[c].next) {
        int g2;
        if (strcmp(el_name(d, c), "groups")) continue;
        for (g2 = d->el[c].first_child; g2 >= 0; g2 = d->el[g2].next) {
          if (strcmp(el_name(d, g2), "group")) continue;
          if (gi < d->ngroups && d->gmap[gi] == want) { gel = g2; break; }
          gi++;
        }
      }
    }
    nfx = group_fx(d, gel, fil, slot);
    if (!num(ix, &x) || x < 0 || x >= nfx || slot[(int)x] < 0) {
      if (b.nlayers) ignored(d, "binding to a group's effect that isn't there");
      return;
    }
    b.target = SF_KT_LFX; b.fx = (uint8_t)slot[(int)x]; b.param = (int8_t)(word(par, "FX_FILTER_FREQUENCY") ? SF_FXP_FREQ : SF_FXP_Q);
  } else if (type && word(type, "effect")) {
    static const struct { const char *name; int param; } P[] = {
      {"FX_FILTER_FREQUENCY", SF_FXP_FREQ}, {"FX_FILTER_RESONANCE", SF_FXP_Q}, {"FX_FILTER_Q", SF_FXP_Q},
      {"FX_REVERB_WET_LEVEL", SF_FXP_WET}, {"FX_REVERB_ROOM_SIZE", SF_FXP_ROOM}, {"FX_REVERB_DAMPING", SF_FXP_DAMP},
      {"FX_WET_LEVEL", SF_FXP_WET}, {"FX_MIX", SF_FXP_WET}, {"FX_DELAY_TIME", SF_FXP_TIME}, {"FX_FEEDBACK", SF_FXP_FEEDBACK},
      {"LEVEL", SF_FXP_LEVEL}, {"FX_GAIN", SF_FXP_LEVEL}, {"FX_LEVEL", SF_FXP_LEVEL},
    };
    const char *ix = attr(d, e, "effectIndex") ? attr(d, e, "effectIndex") : attr(d, e, "position");
    double x; size_t i;
    for (i = 0; i < sizeof P / sizeof *P && !word(par, P[i].name); i++) {}
    if (i == sizeof P / sizeof *P) { snprintf(w, sizeof w, "binding %.60s", par); ignored(d, w); return; }
    if (!num(ix, &x) || x < 0 || x >= k->nfx || k->fx[(int)x].type == SF_FX_NONE) { ignored(d, "binding to an effect that isn't there"); return; }
    if ((P[i].param == SF_FXP_LEVEL) != (k->fx[(int)x].type == SF_FX_GAIN)) { snprintf(w, sizeof w, "binding %.60s", par); ignored(d, w); return; }
    b.target = SF_KT_FX; b.fx = (uint8_t)x; b.param = (int8_t)P[i].param;
  } else { snprintf(w, sizeof w, "binding %.60s", par); ignored(d, w); return; }
  read_trans(d, e, &b.tr, n->lo, n->hi);
  if (b.target == SF_KT_GLOBAL_TUNE || b.target == SF_KT_GROUP_TUNE) n->words = SF_KW_SEMITONES;
  n->b[n->nb++] = b;
}

/* one <control> or <labeled-knob> */
static void read_knob(D *d, int e, SfKnobs *k, const char **ktags, const Layer *layers, int nlayer, int *lcap) {
  SfKnob *n;
  const char *vt = attr(d, e, "valueType") ? attr(d, e, "valueType") : attr(d, e, "type"), *lab;
  int c;
  if (vt && word(vt, "multi_state")) { ignored(d, "valueType multi_state"); return; }
  if (k->nknob >= SF_KNOB_MAX) { ignored(d, "too many knobs"); return; }
  n = &k->knob[k->nknob];
  memset(n, 0, sizeof *n);
  lab = attr(d, e, "label") ? attr(d, e, "label") : attr(d, e, "parameterName");
  if (lab && *lab) snprintf(n->label, sizeof n->label, "%s", lab); else snprintf(n->label, sizeof n->label, "Knob %d", k->nknob + 1);
  n->vtype = vt && word(vt, "integer") ? SF_KV_INT : vt && word(vt, "musical_time") ? SF_KV_MTIME : SF_KV_FLOAT;
  n->lo = attr_f(d, e, "minValue", 0);
  n->hi = attr_f(d, e, "maxValue", n->vtype == SF_KV_MTIME ? 20.0f : 1.0f);
  if (n->hi < n->lo) { float x = n->lo; n->lo = n->hi; n->hi = x; }
  n->value = attr_f(d, e, "value", n->lo > 0 ? n->lo : n->hi < 0 ? n->hi : 0);
  if (n->value < n->lo) n->value = n->lo;
  if (n->value > n->hi) n->value = n->hi;
  if (n->vtype != SF_KV_FLOAT) n->value = (float)floor(n->value + 0.5);
  n->words = n->vtype == SF_KV_MTIME ? SF_KW_MTIME : n->vtype == SF_KV_INT ? SF_KW_INT :
             n->lo == 0 && n->hi == 1 ? SF_KW_PERCENT : SF_KW_NUMBER;
  ktags[k->nknob] = attr(d, e, "tags");
  k->nknob++;
  for (c = d->el[e].first_child; c >= 0; c = d->el[c].next) {
    if (!strcmp(el_name(d, c), "binding")) read_binding(d, c, k, n, layers, nlayer, lcap);
  }
  if (n->vtype == SF_KV_INT && n->words == SF_KW_NUMBER) n->words = SF_KW_INT;
}

/* a <button>: a knob of whole values 0 .. its states less 1, each <state>'s bindings applying while it is in it */
static void read_button(D *d, int e, SfKnobs *k, const char **ktags, const Layer *layers, int nlayer, int *lcap) {
  SfKnob *n;
  const char *lab;
  int c, b, ns = 0;
  for (c = d->el[e].first_child; c >= 0; c = d->el[c].next) if (!strcmp(el_name(d, c), "state")) ns++;
  if (!ns) return;                              /* a button that sets nothing */
  if (k->nknob >= SF_KNOB_MAX) { ignored(d, "too many knobs"); return; }
  n = &k->knob[k->nknob];
  memset(n, 0, sizeof *n);
  lab = attr(d, e, "label") ? attr(d, e, "label") : attr(d, e, "parameterName") ? attr(d, e, "parameterName") : attr(d, e, "tooltip");
  if (lab && *lab) snprintf(n->label, sizeof n->label, "%s", lab); else snprintf(n->label, sizeof n->label, "Button %d", k->nknob + 1);
  n->vtype = SF_KV_INT; n->words = SF_KW_INT;
  n->lo = 0; n->hi = (float)(ns - 1);
  n->value = (float)floor(attr_f(d, e, "value", 0) + 0.5);
  if (n->value < 0) n->value = 0;
  if (n->value > n->hi) n->value = n->hi;
  ktags[k->nknob] = attr(d, e, "tags");
  k->nknob++;
  ns = 0;
  for (c = d->el[e].first_child; c >= 0; c = d->el[c].next) {
    if (strcmp(el_name(d, c), "state")) continue;
    for (b = d->el[c].first_child; b >= 0; b = d->el[b].next) {
      int had = n->nb;
      if (strcmp(el_name(d, b), "binding")) continue;
      read_binding(d, b, k, n, layers, nlayer, lcap);
      if (n->nb > had) n->b[n->nb - 1].state = (int8_t)ns;
    }
    ns++;
  }
}

/* a <ui>'s tab (or the <ui> itself): its knobs, and the controls this can't play counted */
static void read_controls(D *d, int parent, SfKnobs *k, const char **ktags, const Layer *layers, int nlayer, int *lcap) {
  int c;
  for (c = d->el[parent].first_child; c >= 0; c = d->el[c].next) {
    const char *nm = el_name(d, c);
    char w[64];
    if (!strcmp(nm, "control") || !strcmp(nm, "labeled-knob") || !strcmp(nm, "knob")) read_knob(d, c, k, ktags, layers, nlayer, lcap);
    else if (!strcmp(nm, "tab")) read_controls(d, c, k, ktags, layers, nlayer, lcap);
    else if (!strcmp(nm, "button")) read_button(d, c, k, ktags, layers, nlayer, lcap);
    else if (!strcmp(nm, "menu") || !strcmp(nm, "xyPad") || !strcmp(nm, "drawbars") ||
             !strcmp(nm, "slider")) {
      snprintf(w, sizeof w, "<%.50s>", nm); ignored(d, w);
    }
    /* the rest (labels, images, the keyboard's colours) is the picture: read past */
  }
}

static void read_effects(D *d, int parent, SfKnobs *k) {
  int c;
  for (c = d->el[parent].first_child; c >= 0; c = d->el[c].next) {
    const char *ty;
    SfFxDesc *x;
    char w[80];
    if (strcmp(el_name(d, c), "effect")) continue;
    if (k->nfx >= SF_FX_MAX) { ignored(d, "too many effects"); return; }
    x = &k->fx[k->nfx++];
    memset(x, 0, sizeof *x);
    ty = attr(d, c, "type");
    x->freq = attr_f(d, c, "frequency", 22000); x->q = attr_f(d, c, "resonance", 0.7f);
    x->room = attr_f(d, c, "roomSize", 0.7f); x->damp = attr_f(d, c, "damping", 0.3f);
    x->time = attr_f(d, c, "delayTime", 0.7f); x->feedback = attr_f(d, c, "feedback", 0.2f);
    x->offset = attr_f(d, c, "stereoOffset", 0); x->fb_cutoff = attr_f(d, c, "feedbackCutoff", 22000);
    x->musical = attr(d, c, "delayTimeFormat") && word(attr(d, c, "delayTimeFormat"), "musical_time");
    if (ty && (word(ty, "lowpass") || word(ty, "lowpass_4pl"))) x->type = SF_FX_LP;
    else if (ty && word(ty, "lowpass_1pl")) x->type = SF_FX_LP1;
    else if (ty && word(ty, "highpass")) x->type = SF_FX_HP;
    else if (ty && word(ty, "bandpass")) x->type = SF_FX_BP;
    else if (ty && word(ty, "reverb")) { x->type = SF_FX_REVERB; k->has_reverb = 1; }
    else if (ty && word(ty, "delay")) x->type = SF_FX_DELAY;
    else if (ty && word(ty, "gain")) {
      const char *u = attr(d, c, "levelUnit");
      x->type = SF_FX_GAIN; x->db = u && word(u, "db");
      x->level = attr_f(d, c, "level", x->db ? 0.0f : 1.0f);
    }
    else { x->type = SF_FX_NONE; snprintf(w, sizeof w, "effect %.60s", ty ? ty : "(no type)"); ignored(d, w); }
    x->wet = attr_f(d, c, "wetLevel", x->type == SF_FX_REVERB ? 0 : 0.5f);
  }
}

static void read_midi(D *d, int parent, SfKnobs *k, const char **ktags) {
  int c, b;
  for (c = d->el[parent].first_child; c >= 0; c = d->el[c].next) {
    double x;
    if (strcmp(el_name(d, c), "cc") || !num(attr(d, c, "number"), &x) || x < 0 || x > 127) {
      if (strcmp(el_name(d, c), "cc")) { char w[64]; snprintf(w, sizeof w, "<midi> <%.40s>", el_name(d, c)); ignored(d, w); }
      continue;
    }
    for (b = d->el[c].first_child; b >= 0; b = d->el[b].next) {
      const char *lv = attr(d, b, "level"), *ty = attr(d, b, "type"), *par = attr(d, b, "parameter");
      const char *ct = attr(d, b, "controlTags"), *ci = attr(d, b, "controlIndex") ? attr(d, b, "controlIndex") : attr(d, b, "position");
      double y; int n, hit = 0;
      if (strcmp(el_name(d, b), "binding")) continue;
      if (!lv || !word(lv, "ui") || !ty || !word(ty, "control") || (par && !word(par, "VALUE"))) {
        char w[80]; snprintf(w, sizeof w, "midi binding %.60s", par ? par : "(no parameter)"); ignored(d, w);
        continue;
      }
      for (n = 0; n < k->nknob; n++) {
        if (ct ? !tags_meet(ct, ktags[n]) : !(num(ci, &y) && (int)y == n)) continue;
        if (k->ncc >= SF_KCC_MAX) { ignored(d, "too many CC bindings"); return; }
        k->cc[k->ncc].cc = (uint8_t)x; k->cc[k->ncc].knob = (uint8_t)n;
        read_trans(d, b, &k->cc[k->ncc].tr, k->knob[n].lo, k->knob[n].hi);
        k->ncc++;
        k->bound[(int)x >> 3] |= (uint8_t)(1 << ((int)x & 7));
        hit = 1;
      }
      if (!hit) ignored(d, "midi binding to a knob that isn't there");
    }
  }
}

/* <modulators>' children this plays (envelope, lfo, midiCC: d->mods, in order) and their starting values; the
   others counted */
static void read_modulators(D *d, SfKnobs *k) {
  int c, m;
  d->nmods = 0;
  for (c = d->el[0].first_child; c >= 0; c = d->el[c].next) {
    if (strcmp(el_name(d, c), "modulators")) continue;
    for (m = d->el[c].first_child; m >= 0; m = d->el[m].next) {
      const char *nm = el_name(d, m);
      float *v;
      if (strcmp(nm, "envelope") && strcmp(nm, "lfo") && strcmp(nm, "midiCC")) {
        char w[64]; snprintf(w, sizeof w, "<modulators> <%.40s>", nm); ignored(d, w);
        continue;
      }
      if (d->nmods >= SF_MOD_MAX) { ignored(d, "too many modulators"); return; }
      d->mods[d->nmods] = m;
      if (k) {
        v = k->mod[d->nmods];
        v[SF_MP_ENABLED] = is_false(attr(d, m, "enabled")) ? 0.0f : 1.0f;
        v[SF_MP_AMOUNT] = attr_f(d, m, "modAmount", 1);
        v[SF_MP_ATTACK] = attr_f(d, m, "attack", 0); v[SF_MP_DECAY] = attr_f(d, m, "decay", 0);
        v[SF_MP_SUSTAIN] = attr_f(d, m, "sustain", 1); v[SF_MP_RELEASE] = attr_f(d, m, "release", 0);
        v[SF_MP_FREQ] = attr_f(d, m, "frequency", 1);
        k->nmod = d->nmods + 1;
      }
      d->nmods++;
    }
  }
}

/* the groups some binding in e's subtree (a <ui>) can turn on: d->greach by group index */
static void scan_reach(D *d, int e) {
  int c;
  for (c = d->el[e].first_child; c >= 0; c = d->el[c].next) {
    const char *par, *lv, *tags, *gi;
    if (strcmp(el_name(d, c), "binding")) { scan_reach(d, c); continue; }
    par = attr(d, c, "parameter"); lv = attr(d, c, "level");
    if (!par || !word(par, "ENABLED") || !lv || !(word(lv, "group") || word(lv, "tag"))) continue;
    tags = word(lv, "tag") ? attr(d, c, "identifier") : NULL;
    if (!tags) tags = attr(d, c, "tags");
    gi = attr(d, c, "groupIndex") ? attr(d, c, "groupIndex") : attr(d, c, "position");
    {
      int g0, g2, n = 0; double x;
      for (g0 = d->el[0].first_child; g0 >= 0; g0 = d->el[g0].next) {
        if (strcmp(el_name(d, g0), "groups")) continue;
        for (g2 = d->el[g0].first_child; g2 >= 0; g2 = d->el[g2].next) {
          if (strcmp(el_name(d, g2), "group")) continue;
          if (n < d->capr && (tags ? tags_meet(tags, attr(d, g2, "tags")) : num(gi, &x) && (int)x == n)) d->greach[n] = 1;
          n++;
        }
      }
    }
  }
}

static void find_reach(D *d) {
  int c, g, n = 0;
  for (c = d->el[0].first_child; c >= 0; c = d->el[c].next)
    if (!strcmp(el_name(d, c), "groups"))
      for (g = d->el[c].first_child; g >= 0; g = d->el[g].next) if (!strcmp(el_name(d, g), "group")) n++;
  free(d->greach); d->greach = n ? calloc((size_t)n, 1) : NULL; d->capr = d->greach ? n : 0;
  if (!d->greach) return;
  for (c = d->el[0].first_child; c >= 0; c = d->el[c].next) if (!strcmp(el_name(d, c), "ui")) scan_reach(d, c);
}

/* the preset's knobs, effects and CC bindings: NULL when it has no knob and no effect (or memory ran out) */
static SfKnobs *read_knobs(D *d) {
  SfKnobs *k = calloc(1, sizeof *k);
  Layer *layers = NULL;
  const char *ktags[SF_KNOB_MAX];
  int nlayer, c, lcap = 0, i;
  float gvol, gtune;
  if (!k) return NULL;
  memset((void *)ktags, 0, sizeof ktags);
  nlayer = read_layers(d, &layers, &gvol, &gtune);
  if (nlayer < 0) goto fail;
  read_modulators(d, k);
  for (c = d->el[0].first_child; c >= 0; c = d->el[c].next) if (!strcmp(el_name(d, c), "effects")) read_effects(d, c, k);
  for (c = d->el[0].first_child; c >= 0; c = d->el[c].next) if (!strcmp(el_name(d, c), "ui")) read_controls(d, c, k, ktags, layers, nlayer, &lcap);
  for (c = d->el[0].first_child; c >= 0; c = d->el[c].next) if (!strcmp(el_name(d, c), "midi")) read_midi(d, c, k, ktags);
  if (!k->nknob && !k->nfx && !k->nmod) goto fail;
  k->nlayer = nlayer;
  k->layer_vol = calloc((size_t)nlayer + 1, sizeof *k->layer_vol);
  k->layer_tune = calloc((size_t)nlayer + 1, sizeof *k->layer_tune);
  k->layer_en = calloc((size_t)nlayer + 1, sizeof *k->layer_en);
  k->layer_fx = calloc((size_t)nlayer + 1, sizeof *k->layer_fx);
  if (!k->layer_vol || !k->layer_tune || !k->layer_en || !k->layer_fx) goto fail;
  for (i = 0; i < nlayer; i++) { k->layer_en[i + 1] = layers[i].en; memcpy(k->layer_fx[i + 1], layers[i].fx, sizeof layers[i].fx); }
  /* only what some knob sets plays through the layers; the rest stays baked into the regions (1 and 0 here) */
  memset(d->live, 0, sizeof d->live);
  for (i = 0; i < k->nknob; i++) { int j; for (j = 0; j < k->knob[i].nb; j++) if (k->knob[i].b[j].target < 4) d->live[k->knob[i].b[j].target] = 1; }
  k->global_vol = d->live[SF_KT_GLOBAL_VOL] ? gvol : 1; k->global_tune = d->live[SF_KT_GLOBAL_TUNE] ? gtune : 0;
  for (i = 0; i < nlayer; i++) {
    k->layer_vol[i + 1] = d->live[SF_KT_GROUP_VOL] ? layers[i].vol : 1;
    k->layer_tune[i + 1] = d->live[SF_KT_GROUP_TUNE] ? layers[i].tune : 0;
  }
  free(layers);
  return k;
fail:
  free(layers);
  sf_knobs_free(k);
  return NULL;
}

static int feed_ds(SfzBuild *b, void *ctx, const wchar_t *full, char *why, int wcap) {
  D *d = ctx;
  int ok, taken = 0, i;
  (void)full;
  d->b = b; d->pass = 1;
  sfz_header(b, "global");                           /* SFZ's defaults; every value is given per region */
  if (d->elems_capped) sfz_ignored(b, "too many elements (some samples left out)");
  if (d->attrs_capped) sfz_ignored(b, "too many attributes (some left out)");
  if (d->tags_capped) sfz_ignored(b, "too many silencing tags (some left out)");
  memset(d->bmod, 0, sizeof d->bmod);
  if ((d->k = read_knobs(d)) != NULL) {
    int nl = d->k->nlayer;
    d->bgain = (float *)malloc(((size_t)nl + 1) * sizeof(float));
    d->bcents = (float *)malloc(((size_t)nl + 1) * sizeof(float));
    if (d->bgain && d->bcents) sf_knobs_levels_ex(d->k, NULL, nl, d->bgain, d->bcents, NULL, d->bmod);
    else { free(d->bgain); free(d->bcents); d->bgain = d->bcents = NULL; }
    d->bake_all = d->knobbed && ++*d->knobbed > SF_KNOB_PRESETS && d->bgain;
    if (d->bake_all) ignored(d, "knobs of presets past the 8th (at their starting values)");
    else {
      for (i = 0; i < 128; i++) if ((d->k->bound[i >> 3] >> (i & 7)) & 1) opn(d, "samplosis_unbind_cc", i);
      if (d->k->has_reverb) op(d, "samplosis_no_reverb_send", "1");
      taken = sfz_set_knobs(b, d->k);
    }
  }
  ok = walk(d);
  if (!taken) sf_knobs_free(d->k);
  d->k = NULL;
  free(d->bgain); free(d->bcents); d->bgain = d->bcents = NULL; d->bake_all = 0;
  memset(d->live, 0, sizeof d->live);
  if (!ok) { snprintf(why, (size_t)wcap, "cancelled"); return 0; }
  return 1;
}

int sf_is_ds(const char *path, const uint8_t *head, size_t n) {
  const char *dot = path ? strrchr(path, '.') : NULL;
  size_t i;
  if (dot && !_stricmp(dot, ".dspreset")) return 1;
  for (i = 0; i + 14 <= n; i++) if (!memcmp(head + i, "<DecentSampler", 14)) return 1;
  return 0;
}

static void free_d(D *d) {
  if (!d) return;
  free(d->pool); free(d->el); free(d->at); free(d->gmap); free(d->greach);
  map_free(&d->silencers);
  free(d);
}

/* one preset's file into a new D, its tree walked once (round-robin lengths, silencing tags): the D, or NULL with
   why */
static D *load_ds(const char *path, int (*cancel)(void *), void *carg, char *why, int wcap) {
  D *d = calloc(1, sizeof *d);
  wchar_t *full = sf_full_path(path);
  HANDLE h = INVALID_HANDLE_VALUE;
  LARGE_INTEGER sz; DWORD got = 0;
  char *t = NULL;
  int ok = 0;
  if (!d || !full) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
  h = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) { snprintf(why, (size_t)wcap, "it can't be read"); goto done; }
  if (!GetFileSizeEx(h, &sz) || sz.QuadPart > DS_MAX_TEXT) { snprintf(why, (size_t)wcap, "it's too big to be a preset"); goto done; }
  if (!(t = malloc((size_t)sz.QuadPart + 1))) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
  if (sz.QuadPart && (!ReadFile(h, t, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart)) {
    snprintf(why, (size_t)wcap, "it can't be read");
    goto done;
  }
  CloseHandle(h); h = INVALID_HANDLE_VALUE;
  {
    char *fixed = malloc((size_t)sz.QuadPart * 2 + 1);
    size_t fn;
    if (!fixed) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
    fn = ds_fix_attr_spaces(t, (size_t)sz.QuadPart, fixed);
    free(t); t = fixed;
    if (!parse_xml(d, t, fn, why, wcap)) goto done;
  }
  free(t); t = NULL;
  d->pass = 0;
  d->cancel = cancel; d->carg = carg;
  find_reach(d);
  if (!walk(d)) { snprintf(why, (size_t)wcap, "cancelled"); goto done; }
  ok = 1;
done:
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  free(t); free(full);
  if (!ok) { free_d(d); d = NULL; }
  return d;
}

int sf_parse_ds(SmpFont *f, const char *path, int (*cancel)(void *), void *carg, char *why, int wcap) {
  D *d = load_ds(path, cancel, carg, why, wcap);
  int ok;
  if (!d) return 0;
  ok = sfz_build(f, path, feed_ds, d, cancel, carg, why, wcap);
  if (ok == 0 && !strcmp(why, "it has no regions with samples")) snprintf(why, (size_t)wcap, "it has no samples to play");
  free_d(d);
  return ok;
}

/* several presets: each one's file name (without its extension) names it */
typedef struct { D **d; const char *const *paths; int n; int knobbed; } Many;

static int feed_many(SfzBuild *b, void *ctx, const wchar_t *full, char *why, int wcap) {
  Many *m = ctx;
  int k;
  m->knobbed = 0;                                    /* each pass counts the knobbed presets afresh */
  for (k = 0; k < m->n; k++) {
    const char *p = m->paths[k], *base = p, *c, *dot;
    char name[64];
    size_t L;
    for (c = p; *c; c++) if (*c == '\\' || *c == '/') base = c + 1;
    dot = strrchr(base, '.');
    L = dot && dot > base ? (size_t)(dot - base) : strlen(base);
    if (L >= sizeof name) L = sizeof name - 1;
    memcpy(name, base, L); name[L] = 0;
    sfz_preset(b, name);
    m->d[k]->knobbed = &m->knobbed;
    if (!feed_ds(b, m->d[k], full, why, wcap)) return 0;
  }
  return 1;
}

int sf_parse_ds_many(SmpFont *f, const char *font_path, const char *const *presets, int n, int (*cancel)(void *),
                     void *carg, char *why, int wcap) {
  Many m;
  int ok = 0, k;
  m.d = calloc((size_t)(n > 0 ? n : 1), sizeof *m.d); m.paths = presets; m.n = n;
  if (!m.d) { snprintf(why, (size_t)wcap, "out of memory"); return 0; }
  for (k = 0; k < n; k++) {
    const char *c, *slash = NULL;
    char w[300];
    if (!(m.d[k] = load_ds(presets[k], cancel, carg, w, sizeof w))) {
      const char *base = presets[k];
      for (c = presets[k]; *c; c++) if (*c == '\\' || *c == '/') base = c + 1;
      snprintf(why, (size_t)wcap, "%s: %s", base, w);
      ok = -1;
      goto done;
    }
    for (c = presets[k]; *c; c++) if (*c == '\\' || *c == '/') slash = c;
    if (slash && (size_t)(slash - presets[k] + 1) < sizeof m.d[k]->dir) {
      memcpy(m.d[k]->dir, presets[k], (size_t)(slash - presets[k] + 1));
      m.d[k]->dir[slash - presets[k] + 1] = 0;
    }
  }
  ok = sfz_build(f, font_path, feed_many, &m, cancel, carg, why, wcap);
  if (ok == 0 && !strcmp(why, "it has no regions with samples")) snprintf(why, (size_t)wcap, "it has no samples to play");
done:
  for (k = 0; k < n; k++) free_d(m.d[k]);
  free(m.d);
  return ok;
}
