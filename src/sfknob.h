/* sfknob.h: a Decent Sampler preset's knobs (<ui> controls and their bindings), its effects chain (<effects>) and
   its MIDI CC bindings (<midi>), as sfds.c reads them: one immutable description per preset of a font
   (SmpFont.knobs), built at open. The player (sfsynth.c) keeps the knobs' values and turns them into the layers'
   gains and tunings and the effects' parameters; sfdsfx.c plays the chain. */
#ifndef SFKNOB_H
#define SFKNOB_H
#include <stdint.h>

#define SF_KNOB_MAX 64                   /* knobs a preset keeps */
#define SF_KBIND_MAX 8                   /* bindings a knob keeps */
#define SF_FX_MAX 8                      /* effects a chain keeps */
#define SF_KCC_MAX 64                    /* CC bindings a preset keeps */
#define SF_TABLE_MAX 16                  /* points of a translation table */
#define SF_KNOB_PRESETS 8                /* a font's knobbed presets whose knobs a player keeps values for; the rest
                                            have their knobs' starting values baked into their regions */
#define SF_KNOB_LAYERS 1024              /* a preset's layers a player reaches; later ones are baked likewise */
#define SF_MOD_MAX 8                     /* <modulators> a preset keeps */

enum { SF_KV_FLOAT, SF_KV_INT, SF_KV_MTIME };                        /* valueType */
enum { SF_KW_NUMBER, SF_KW_PERCENT, SF_KW_SEMITONES, SF_KW_MTIME, SF_KW_INT };   /* how the window reads it */
enum { SF_TR_LINEAR, SF_TR_TABLE, SF_TR_FIXED };
/* a translation: linear from lo to hi over the knob's range; a table of points (the knob's value to the output,
   straight lines between, flat outside); or a fixed value */
typedef struct { uint8_t kind, npts; float lo, hi, fixed; float x[SF_TABLE_MAX], y[SF_TABLE_MAX]; } SfTrans;
/* SF_KT_GROUP_EN: groups on or off (a button's state); SF_KT_LFX: a group's own effect (fx 0 or 1 of the group's,
   param SF_FXP_FREQ or SF_FXP_Q), per layer; SF_KT_MOD: a modulator's parameter (mod, param SF_MP_*);
   SF_KT_GROUP_ENV: the amplitude envelope's attack, decay, sustain or release (param 0..3), per layer */
enum { SF_KT_GROUP_VOL, SF_KT_GLOBAL_VOL, SF_KT_GROUP_TUNE, SF_KT_GLOBAL_TUNE, SF_KT_FX, SF_KT_GROUP_EN, SF_KT_LFX,
       SF_KT_MOD, SF_KT_GROUP_ENV };
#define SF_LFX_N 8                       /* a layer's live values (sf_knobs_levels_ex's lfx): see there */
enum { SF_FXP_NONE = -1, SF_FXP_FREQ = 0, SF_FXP_Q, SF_FXP_WET, SF_FXP_ROOM, SF_FXP_DAMP, SF_FXP_TIME, SF_FXP_FEEDBACK,
       SF_FXP_LEVEL };
/* a modulator's values as a player keeps them (sf_knobs_levels' kmod, SF_MP_N a modulator): ENABLED (0 or 1),
   MOD_AMOUNT, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE (seconds, sustain 0..1) and FREQUENCY (Hz). In kmod
   SF_MP_ENABLED is the modulator's scale: its amount when it is on, else 0. */
enum { SF_MP_ENABLED, SF_MP_AMOUNT, SF_MP_ATTACK, SF_MP_DECAY, SF_MP_SUSTAIN, SF_MP_RELEASE, SF_MP_FREQ, SF_MP_N };
/* a binding: what it sets (SF_KT_*); for an effect, which (index into fx, or 0..1 of a group's own) and what
   (SF_FXP_*); for a group one, the layers it covers (layer_list[first_layer .. first_layer + nlayers)); for a
   modulator, which (mod) and what (param, SF_MP_*). state: a button's state the binding belongs to (it applies
   only while the button is in it), or -1 for a knob's */
typedef struct { uint8_t target, fx, mod; int8_t param, state; uint16_t first_layer, nlayers; SfTrans tr; } SfKBind;
typedef struct { char label[64]; uint8_t vtype, words, nb; float lo, hi, value; SfKBind b[SF_KBIND_MAX]; } SfKnob;
enum { SF_FX_LP, SF_FX_LP1, SF_FX_HP, SF_FX_BP, SF_FX_REVERB, SF_FX_DELAY, SF_FX_GAIN, SF_FX_NONE };
/* an effect as the preset gives it (Decent Sampler's defaults where it doesn't): filters freq (Hz) and q; reverb
   room, damp, wet; delay time (seconds, or a musical-time index when musical), feedback, offset (stereoOffset,
   seconds), fb_cutoff (Hz; 22000 is off) and wet; gain its level (linear, or dB when db). A gain plays as a factor
   on the preset's layers (sf_knobs_levels), not in the chain. SF_FX_NONE holds the place of a kind this can't
   play, so the bindings' effect numbers still line up. */
typedef struct { uint8_t type, musical, db; float freq, q, room, damp, wet, time, feedback, offset, fb_cutoff, level; } SfFxDesc;
/* a CC that sets a knob: the knob (0-based) and the translation from cc / 127 */
typedef struct { uint8_t cc, knob; SfTrans tr; } SfKcc;

typedef struct SfKnobs {
  int nknob; SfKnob knob[SF_KNOB_MAX];
  int nfx; SfFxDesc fx[SF_FX_MAX];
  int ncc; SfKcc cc[SF_KCC_MAX];
  uint8_t bound[16];                 /* a bit per CC: bound to a knob (its General MIDI meaning is off here) */
  int nlayer;                        /* regions' cond.layer runs 1..nlayer: one per enabled <group>, in file order */
  float *layer_vol, *layer_tune;     /* per layer (1..nlayer; [0] unused): the group's own volume (linear) and
                                        tuning (semitones), which a group binding replaces */
  float global_vol, global_tune;     /* <groups>' volume (linear) and globalTuning, which an instrument binding
                                        replaces */
  uint16_t *layer_list; int nlayer_list;
  int has_reverb;                    /* a reverb effect: its regions send nothing to the player's shared reverb */
  /* added for buttons, groups' own effects and modulators */
  uint8_t *layer_en;                 /* per layer: the group's enabled (a group a button can turn on is a layer
                                        even when it starts off) */
  float (*layer_fx)[4];              /* per layer: its group's first two filters' frequency (Hz, 0 none) and
                                        resonance (Q), [0] [1] the first's, [2] [3] the second's */
  int nmod; float mod[SF_MOD_MAX][SF_MP_N];   /* <modulators>' starting values (SF_MP_ENABLED 0 or 1) */
} SfKnobs;

void sf_knobs_free(SfKnobs *k);
/* a translation of knob value v (whose range is lo..hi) */
float sf_trans(const SfTrans *t, float v, float lo, float hi);
/* the layers' gains (linear) and tunings (cents), lgain[1..nl] and lcents[1..nl], for knob values values[i]
   (NULL: each knob's starting value): every binding sets its target over the preset's own levels, in knob order
   then binding order; effect bindings are left to the caller */
void sf_knobs_levels(const SfKnobs *k, const double *values, int nl, float *lgain, float *lcents);
/* the same, and (either may be NULL) each layer's live values, lfx[1..nl]: its group filters' (cents from the
   first's frequency, its resonance in dB or NAN for a layer without one, the same for the second) and its amplitude
   envelope's as a knob sets them (attack, decay, release in seconds, sustain 0..1; NAN where none does), [4] to [7];
   and the modulators' values, kmod (k->nmod * SF_MP_N) */
void sf_knobs_levels_ex(const SfKnobs *k, const double *values, int nl, float *lgain, float *lcents,
                        float (*lfx)[SF_LFX_N], float *kmod);
/* a Q as SFZ's resonance (dB the peak stands above the passband, 0 for a Q at or below a Butterworth's) */
float sf_q_db(float q);
/* a knob's value as its bindings read it: clamped, and whole for integer and musical-time knobs */
double sf_knob_value(const SfKnob *n, double v);
/* a musical-time index's length in beats (a quarter note is one): Decent Sampler's list of 21, index clamped */
double sf_mtime_beats(int index);
/* its words: "1/4", "1/8 dotted", "1/4 triplet" */
const char *sf_mtime_name(int index);

#endif
