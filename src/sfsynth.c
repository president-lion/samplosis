/* sfsynth.c: the SoundFont player. It holds the 16 MIDI channels and their state, picks each channel's preset,
   allocates and steals voices (sfvoice.c), and mixes them into 16 stereo outputs. It also reads and writes its
   state lines and has a test render.

   Threads and the contract that keeps fonts safe:
   - The audio thread (smp_synth_midi, process, reset) never allocates, locks, waits or touches the disk. Its
     only kernel calls are the loader's SetEvent inside smp_sf_use. (The one exception is the offline test
     render, which waits for the loader on purpose so its output is exact.)
   - The main thread loads fonts and applies state. It hands things to the audio thread three ways:
     1. Parameters: atomic doubles, read at the start of every block.
     2. The mode, preset, channel, pinned presets and outputs: a seqlock. The audio thread copies them at the
        start of a block only when the copy is whole, and otherwise keeps the last one.
     3. The font: smp_synth_load publishes the new font's pointer. At its next block the audio thread kills
        every voice of the old font at once (each releases its sample), drops the old font's preset use
        counts, and makes the new font current. The old font goes on a retired list with the block counter
        (epoch) of that moment. smp_synth_collect closes it only after two more blocks have finished and the
        audio thread no longer has it as current. So a font is never closed while a voice holds its samples or
        its presets are counted as used.
   - smp_synth_free is called on the main thread while nothing processes (after the plugin's deactivate). It
     kills every voice at once (each releases its sample), drops every use count, and only then closes the
     fonts. smp_sf_close joins the loader thread when the last font goes, so it must never run inside
     DllMain or process detach. Free is therefore never called from there either.
   - The presets of the state's sf_used line are used (smp_sf_use) by the main thread when the state loads. They
     are released once, by whichever side gets there first: the audio thread 10 s after the first note-on it gets
     (a project loads long before Play is pressed), at smp_synth_reset or when the player goes dormant (the
     sampler plays), or the main thread when the next state or font replaces them. A small state word decides
     who does it. The plugin's reset on stop, seek and loop is smp_synth_flush, which keeps them.

   Region conditions (SmpSfCond, as sfzformat.com defines the SFZ opcodes; the SF2 family leaves them at "always",
   so its regions play exactly as before). Their state is per channel, in fixed arrays inside the synth, and in
   the round-robin counters, which smp_synth_load sizes on the main thread (see Seq below). An event looks only at
   the regions that cover its key (the font's key index, sf_key_regions), and skips the passes its preset can't
   need (no release regions, nothing that stops other voices).
   - trigger: attack regions start at note-on only, release regions at note-off only. first plays only when no
     other key of the channel is down, legato only when one is (keys physically down, keyswitches aside).
   - Round robin (seq_len, seq_pos): a counter per round-robin set (sf_finish_regions: one preset's regions with
     the same key and velocity ranges and the same conditions but seq_pos), per channel. It advances once per
     note-on (or note-off, for release triggers) on which the set is otherwise eligible, and the region plays when
     (the count before that note % seq_len) + 1 == seq_pos, so the first note plays seq_pos 1. There is a counter
     for every set of the font's largest preset. The counters start again on a GM, GS or XG reset, a font change
     and a channel's preset change; they are kept across a host's stop, seek and loop (smp_synth_flush), so a song
     that stops and plays on keeps cycling.
   - Random (lorand, hirand): one draw in [0, 1) per note-on (xorshift32, seeded the same in every new synth;
     smp_synth_test_seed), shared by every region of the note; a region plays when lorand <= draw < hirand (or
     hirand is 1 or more). A note's release triggers use its note-on's draw.
   - Keyswitches: keys in a region's sw_lokey..sw_hikey, and its sw_last key, set the channel's last keyswitch;
     those keys never sound (sf_preset_swmask). sw_last plays when the last keyswitch (or, before any since the
     channel's preset was chosen, the region's sw_default) is that key; sw_down and sw_up test whether that key is
     down now (any key, a keyswitch or not); sw_previous whether the channel's previous note-on (keyswitches too)
     was that key.
   - CC ranges (locc): each controller within its range at the moment the region would start.
   - Release triggers: at note-off, or when the sustain pedal (or the sostenuto that caught the key) lets go, with
     the note-on's velocity and the time from the note-on to then (rt_decay; sfvoice.c). They count against the
     voice limit and can be stolen, but no note-off or pedal releases them: they play their sample through its own
     envelope (one whose sample loops is released at once, so it can't drone on). A key struck again while its
     release waits for the pedal drops that waiting release: the new note's own note-off (or the pedal after it)
     plays one, timed from the new strike, and a pedal let go while the key is down plays none. All Notes Off and
     Sound Off drop waiting releases without playing them.
   - Crossfades that put a note at 0 start no voice at all.
   - group and off_by: a region starting in group G stops this channel's voices whose region has off_by G (off_by
     0 is none), with that voice's off_mode: fast is the 5 ms kill, normal its release. SF2's exclusive classes
     work as before beside them.
   - Crossfades and amp_veltrack: a fixed gain worked out when the voice starts (sfvoice.c).
   - polyphony and note_polyphony (SFZ): before a region starts, while its group already has that many voices
     sounding on the channel (not released or fading), or that many on its key, the oldest stops by its own
     off_mode (poly_limit).
   - one_shot (SFZ loop_mode=one_shot): note-offs and the pedals don't release the voice (release_voice); an
     off_by stop with off_mode normal and the polyphony limits do (stop_voice), and kills always do. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "sf2.h"
#include "sfvoice.h"
#include "sffx.h"
#include "sfstream.h"

#define NCH 16
#define NOUT 16
#define MAXV 1088                 /* the voice array, allocated once: more than sf_voices' top (1024), so
                                     voices stolen at the limit still have slots to fade out in */
#define MAXEV 4096                /* queued MIDI events per block */
#define MAXUSED 1024              /* presets in an sf_used line */
#define KILL_MS 5.0               /* a stolen or killed voice fades out over this */
#define PENDING_MS 1000           /* a voice waiting for its sample is dropped after this much real time (not in an
                                     offline render, which never drops one) */
#define HOLD_SECONDS 10.0         /* sf_used presets are let go this long after the first note-on after the state
                                     loaded (a project loads long before Play is pressed) */
#define G_EXCL 57                 /* exclusiveClass */
#define OFFLINE_STEP 256          /* an offline render with streaming voices plays at most this many frames between
                                     looks at the rings (fewer at high pitches: see render) */
#define COLLECT_WAIT_MS 200       /* collect waits this long for the streamer, else tries again on its next call */
#define NPAR (SMP_NALLPARAMS - SMP_NPARAMS)
#define G_MODES 54                /* sampleModes */

enum { V_FREE, V_PENDING, V_LIVE };
enum { EV_MSG, EV_SYSRESET, EV_RESET };
enum { MODE_ONE = 0, MODE_ALL = 1 };
/* who has the sf_used presets: nobody, held (either side may release), the main thread or the audio thread
   releasing them right now */
enum { U_NONE = 0, U_HELD = 1, U_MAIN = 3, U_AUDIO = 4 };

typedef struct { int at, kind; uint8_t b[3]; } Ev;

typedef struct {
  int mode;                       /* MODE_ONE or MODE_ALL */
  int bank, program;              /* one mode's preset */
  int channel;                    /* one mode: 0 every channel, else 1..16 */
  int pin[NCH];                   /* all mode: bank * 128 + program the channel always plays, or -1 */
  int out[NCH];                   /* all mode: the channel's output, 0..15 */
} Cfg;

typedef struct {
  SmpSfChan c;                    /* what the modulators read */
  int msb, lsb;                   /* bank select as it came in */
  int bank, program;              /* taken at the last program change (bank select acts then, as MIDI says) */
  int preset;                     /* the public preset index this channel uses (smp_sf_use), or -1 */
  int rpn_msb, rpn_lsb, nrpn;     /* the selected parameter; 127/127 is none */
  int range_semis, range_cents, fine14;   /* RPN 0 and RPN 1 as their data entry registers */
  /* region conditions (see the top) */
  uint8_t held[128];              /* keys down now, the pedals aside */
  uint8_t on_vel[128];            /* each key's last note-on velocity, for its release triggers */
  uint64_t on_at[128];            /* and its frame on the synth's clock */
  float rnd[128];                 /* and its random draw */
  uint8_t rel_wait[128];          /* a note-off came while a pedal held the key: its release triggers wait */
  uint8_t ksost[128];             /* the sostenuto pedal caught the key */
  int sw_last;                    /* the last keyswitch pressed, -1 none */
  int prev_key;                   /* the previous note-on's key, -1 none */
} Chan;

/* Round-robin counters: n per channel (by the channel preset's set ids) and n event stamps, in one allocation.
   smp_synth_load makes a bigger one, on the main thread, when a font needs more sets than the last held, and
   publishes it before the font; the audio thread takes it with the font. Old ones stay until smp_synth_free, so
   the audio thread never uses a freed one (sizes only grow, so there are few). */
typedef struct { int n; uint32_t *stamp; uint32_t seq[1]; } Seq;

typedef struct {
  int st;                         /* V_FREE, V_PENDING (waiting for its sample) or V_LIVE */
  int chan, key, vel;
  int keydown;                    /* the key is still held */
  int sost;                       /* sostenuto caught this note */
  int excl;                       /* the region's exclusive class */
  int rt;                         /* a release trigger: no note-off or pedal releases it */
  double held;                    /* a release trigger's seconds held, else -1 (kept for a pending start) */
  uint64_t age;                   /* when it started; smaller is older */
  const SmpSfRegion *reg;         /* the region a pending voice waits to start */
  ULONGLONG since;                 /* when a pending voice began to wait (GetTickCount64, real time) */
  uint64_t vid;                   /* this start's own number, so a partner link can be checked */
  int partner; uint64_t partner_vid;   /* the other half of a stereo pair (SmpSfCond.pair), -1 none */
  uint32_t ev;                    /* the note event (serial) that started it */
} Slot;

typedef struct { SmpFont *f; LONG64 a0; } Retired;

struct SmpSynth {
  double rate; int max_block;
  volatile LONG64 p[NPAR];        /* parameters 31..34, any thread */

  /* main thread */
  char path[1024];                /* the loaded font's path as given */
  SmpFont *font;                  /* the font loaded last (owns one reference) */
  Retired *ret; int nret, cret;   /* replaced fonts, each owning one reference */
  Cfg mcfg;                       /* the main thread's copy of the configuration */

  /* hand-offs */
  SmpFont *volatile want;         /* the font the audio thread should play */
  SmpFont *volatile cur;          /* the font the audio thread plays (written by it) */
  volatile LONG64 epoch;          /* blocks finished */
  volatile LONG cfg_seq;          /* odd while the main thread writes cfg_shared */
  Cfg cfg_shared;
  volatile LONG used_state;       /* U_* */
  volatile LONG used_left;        /* frames until the audio thread lets the sf_used presets go */
  volatile LONG used_armed;       /* the countdown runs: a note-on came after the state loaded */
  SmpFont *used_font; int used[MAXUSED]; int nused;
  volatile LONG chan_preset[NCH]; /* the channels' presets, for writing sf_used (-1 none) */

  /* audio thread */
  Cfg cfg; LONG cfg_seen;
  Chan ch[NCH];
  SmpSfVoice *v; Slot *sl;
  uint64_t age;
  uint64_t protect;               /* during a note-on: voices this old or newer were started by it (0: none) */
  uint64_t clock;                 /* frames processed so far */
  uint64_t now;                   /* the frame of the event being handled, on that clock */
  uint32_t rng;                   /* xorshift32 state for the random draw */
  uint32_t serial;                /* note events so far: a round-robin set advances once per event */
  uint64_t vid;                   /* voice starts so far (Slot.vid) */
  const SmpSfRegion *last_reg; int last_slot; uint64_t last_vid;   /* this event's last start, for stereo pairs */
  Seq *sq;                        /* the counters the audio thread uses (taken with the font), or NULL */
  Seq *volatile sq_want;          /* the newest counters smp_synth_load made (main thread writes) */
  Seq **sq_all; int nsq;          /* every Seq made, freed by smp_synth_free (main thread) */
  Ev ev[MAXEV]; int nev;
  float *rev, *cho;               /* effect sends of output 1, one block at a time */
  SmpSfFx *fx;                    /* the reverb and chorus: their output joins output 1 before sf_volume */
  double gain;                    /* the volume gain at the end of the last block */
  int offline;                    /* wait for the loader (the offline test render) */
  volatile LONG render_offline;   /* wait for the loader (the host renders offline: smp_synth_set_offline) */
  int test_wait;                  /* render_test's waiting, on by default */
  int dormant;                    /* the player isn't playing (smp_synth_skip: the sampler plays). The channels'
                                     presets aren't used (their samples can be freed), but chan_preset still says
                                     what each channel would play; the next process uses them again */
  volatile LONG peak;
  /* streaming: the streamer thread and its rings, made when the first font with long samples loads (main thread)
     and kept until free; the voice slots are its slots */
  SfStream *volatile st;
  volatile LONG underruns;
};

static double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

/* ---- parameters ---- */
static LONG64 bits_of(double v) { LONG64 b; memcpy(&b, &v, 8); return b; }
static double param(const SmpSynth *s, int id) { LONG64 b = s->p[id - SMP_NPARAMS]; double v; memcpy(&v, &b, 8); return v; }

SMP_API void smp_synth_set_param(SmpSynth *s, int id, double v) {
  const SmpParamInfo *pi;
  if (!s || id < SMP_NPARAMS || id >= SMP_NALLPARAMS || v != v) return;
  pi = smp_param_info(id);
  InterlockedExchange64(&s->p[id - SMP_NPARAMS], bits_of(clampd(v, pi->lo, pi->hi)));
}

SMP_API double smp_synth_get_param(const SmpSynth *s, int id) {
  if (id < SMP_NPARAMS || id >= SMP_NALLPARAMS) return 0;
  return s ? param(s, id) : smp_param_info(id)->def;
}

/* ---- configuration hand-off (seqlock) ---- */
static void cfg_default(Cfg *c) {
  int i;
  memset(c, 0, sizeof *c);
  c->mode = MODE_ALL;
  for (i = 0; i < NCH; i++) { c->pin[i] = -1; c->out[i] = 0; }
}

static void cfg_publish(SmpSynth *s) {
  InterlockedIncrement(&s->cfg_seq);           /* odd: being written */
  _ReadWriteBarrier();
  memcpy(&s->cfg_shared, &s->mcfg, sizeof s->mcfg);
  _ReadWriteBarrier();
  InterlockedIncrement(&s->cfg_seq);           /* even: whole */
}

/* audio thread: 1 when a new configuration was taken */
static int cfg_take(SmpSynth *s) {
  LONG a = s->cfg_seq; Cfg c;
  if (a == s->cfg_seen || (a & 1)) return 0;
  MemoryBarrier();
  memcpy(&c, &s->cfg_shared, sizeof c);
  MemoryBarrier();
  if (s->cfg_seq != a) return 0;                /* written meanwhile: keep the last one this block */
  s->cfg = c; s->cfg_seen = a;
  return 1;
}

/* ---- channels ---- */
static void chan_controllers(Chan *h) {    /* Reset All Controllers, as GM's recommended practice (RP-015) has it */
  int k;
  h->c.cc[1] = 0; h->c.cc[11] = 127;
  for (k = 64; k <= 67; k++) h->c.cc[k] = 0;
  h->c.bend = 8192; h->c.pressure = 0;
  for (k = 0; k < 128; k++) h->c.poly[k] = 0;
  h->rpn_msb = h->rpn_lsb = 127; h->nrpn = 0;
}

static void chan_defaults(Chan *h) {        /* power on, and the GM, GS and XG resets */
  int keep = h->preset;
  memset(h, 0, sizeof *h);
  h->preset = keep;
  h->c.cc[7] = 100; h->c.cc[8] = 64; h->c.cc[10] = 64; h->c.cc[11] = 127;
  h->c.cc[91] = 40;                         /* GM2's (and FluidSynth's) default reverb send */
  h->c.bend = 8192; h->c.bend_range_cents = 200;
  h->range_semis = 2; h->range_cents = 0; h->fine14 = 8192;
  h->rpn_msb = h->rpn_lsb = 127;
  h->sw_last = h->prev_key = -1;           /* the condition state too: no keys, keyswitch or counters */
}

/* the condition state that names keys and set ids of the font: on a font change, all of it */
static void chan_cond_reset(Chan *h) {
  memset(h->held, 0, sizeof h->held); memset(h->rel_wait, 0, sizeof h->rel_wait); memset(h->ksost, 0, sizeof h->ksost);
  h->sw_last = h->prev_key = -1;
}

/* the round-robin counters of channel c (c < 0: every channel) start again */
static void seq_clear(SmpSynth *s, int c) {
  Seq *q = s->sq;
  if (!q || q->n <= 0) return;
  if (c < 0) memset(q->seq, 0, sizeof(uint32_t) * (size_t)q->n * NCH);
  else memset(q->seq + (size_t)c * q->n, 0, sizeof(uint32_t) * (size_t)q->n);
}

static int listens(const SmpSynth *s, int c) {
  return s->cfg.mode == MODE_ALL || s->cfg.channel == 0 || s->cfg.channel - 1 == c;
}

static int out_of(const SmpSynth *s, int c) { return s->cfg.mode == MODE_ALL ? s->cfg.out[c] : 0; }

static int has_bank(const SmpFont *f, int bank) {
  int i, n = smp_sf_preset_count(f), b;
  for (i = 0; i < n; i++) if (smp_sf_preset_info(f, i, &b, NULL, NULL, 0) && b == bank) return 1;
  return 0;
}

/* the program fallback: the exact bank and program; then bank 0 with the same program (for drums, a bank of 127
   or more: bank 128 with the same program, as XG's kits are numbered, then bank 128 program 0); then the first
   preset in the font */
static int lookup(const SmpFont *f, int bank, int program, int drum) {
  int p = smp_sf_find_preset(f, bank, program);
  if (p < 0 && drum) p = smp_sf_find_preset(f, 128, program);
  if (p < 0) p = drum ? smp_sf_find_preset(f, 128, 0) : smp_sf_find_preset(f, 0, program);
  if (p < 0 && smp_sf_preset_count(f) > 0) p = 0;
  return p;
}

/* the preset channel c should play now, or -1 */
static int want_preset(const SmpSynth *s, int c) {
  const SmpFont *f = s->cur; const Chan *h = &s->ch[c]; int bank;
  if (!f) return -1;
  if (s->cfg.mode == MODE_ONE) {
    if (!listens(s, c)) return -1;
    return lookup(f, s->cfg.bank, s->cfg.program, s->cfg.bank >= 127);
  }
  if (s->cfg.pin[c] >= 0) {
    bank = s->cfg.pin[c] / 128;
    return lookup(f, bank, s->cfg.pin[c] % 128, bank >= 127);
  }
  bank = h->bank;
  if (c == 9 && bank != 127 && bank != 128) bank = 128;   /* channel 10: drums unless a drum bank came in */
  return lookup(f, bank, h->program, bank >= 127);
}

/* use the channel's new preset and release the old one (smp_sf_use; safe on the audio thread) */
static void resolve(SmpSynth *s, int c) {
  Chan *h = &s->ch[c]; int p = want_preset(s, c);
  if (p == h->preset) return;
  if (p >= 0) smp_sf_use(s->cur, p, 1);
  if (h->preset >= 0) smp_sf_use(s->cur, h->preset, -1);
  h->preset = p;
  seq_clear(s, c);                            /* set ids are the preset's own */
  h->sw_last = -1;                            /* so the new preset's sw_default applies */
  InterlockedExchange(&s->chan_preset[c], p);
}

static void resolve_all(SmpSynth *s) { int c; for (c = 0; c < NCH; c++) resolve(s, c); }

/* ---- voices ---- */
static int sounding(const SmpSynth *s, int i) { return s->sl[i].st == V_LIVE && !s->v[i].killing; }

static void slot_free(SmpSynth *s, int i) { s->sl[i].st = V_FREE; }

static void kill_voice(SmpSynth *s, int i, double ms) {
  if (s->sl[i].st == V_PENDING) { slot_free(s, i); return; }
  if (s->sl[i].st != V_LIVE) return;
  smp_sfv_kill(&s->v[i], ms);
  if (!s->v[i].live) slot_free(s, i);
}

/* the other half of slot i's stereo pair, while it is still that voice; -1 when there is none */
static int partner_of(const SmpSynth *s, int i) {
  int k = s->sl[i].partner;
  if (k < 0 || k >= MAXV || s->sl[k].st == V_FREE || s->sl[k].vid != s->sl[i].partner_vid) return -1;
  return k;
}

/* a note-off or a pedal's let-go: a one-shot region (SFZ loop_mode=one_shot) ignores it, even while pending */
static void release_voice(SmpSynth *s, int i) {
  if (s->sl[i].reg && s->sl[i].reg->cond.one_shot) return;
  if (s->sl[i].st == V_PENDING) { slot_free(s, i); return; }
  if (s->sl[i].st == V_LIVE) smp_sfv_release(&s->v[i]);
}

/* a stop by another region (off_by with off_mode normal, or a polyphony limit): the release, one-shots too */
static void stop_voice(SmpSynth *s, int i) {
  if (s->sl[i].st == V_PENDING) { slot_free(s, i); return; }
  if (s->sl[i].st == V_LIVE) smp_sfv_off(&s->v[i]);
}

/* how loud a voice counts for stealing. One still in its delay or attack is judged by the level it is rising
   to (its attenuation), not by its envelope so far, or every new note of a chord would look the quietest. */
static double steal_level(const SmpSfVoice *v) {
  if (!v->released && v->venv.stage <= SMP_SFV_ATTACK) return v->atten_gain * (v->killing ? v->fade : 1);   /* delay, attack */
  return smp_sfv_level(v);
}

/* the voice to steal: released voices first, then the quietest, then the oldest. Never one the note-on being
   handled has just started (as FluidSynth does), so a chord or a layered note keeps all its voices. */
static int victim(const SmpSynth *s, int except) {
  int i, best = -1, brel = 0; double blev = 0;
  for (i = 0; i < MAXV; i++) {
    int rel; double lev;
    if (i == except || !sounding(s, i) || (s->protect && s->sl[i].age >= s->protect)) continue;
    rel = s->v[i].released; lev = steal_level(&s->v[i]);
    if (best < 0 || rel > brel || (rel == brel && (lev < blev || (lev == blev && s->sl[i].age < s->sl[best].age)))) {
      best = i; brel = rel; blev = lev;
    }
  }
  return best;
}

/* keeps the sounding voices within sf_voices, voice `except` (just started) aside */
static void limit(SmpSynth *s, int except) {
  int i, n = 0, lim = (int)param(s, SMP_P_SF_VOICES);
  for (i = 0; i < MAXV; i++) if (sounding(s, i)) n++;
  while (n > lim) {
    int k = victim(s, except), pk;
    if (k < 0) break;
    pk = partner_of(s, k);
    kill_voice(s, k, KILL_MS);
    n--;
    if (pk >= 0 && pk != except && sounding(s, pk)) { kill_voice(s, pk, KILL_MS); n--; }   /* a pair goes together */
  }
  if (n > s->peak) s->peak = n;
}

/* a free slot: an unused one, else the oldest fading voice, else a victim, ended at once */
static int free_slot(SmpSynth *s) {
  int i, k = -1;
  for (i = 0; i < MAXV; i++) if (s->sl[i].st == V_FREE) return i;
  for (i = 0; i < MAXV; i++)
    if (s->sl[i].st == V_LIVE && s->v[i].killing && (k < 0 || s->sl[i].age < s->sl[k].age)) k = i;
  if (k < 0) for (i = 0; i < MAXV; i++) if (s->sl[i].st == V_PENDING) { k = i; break; }
  if (k < 0) k = victim(s, -1);
  if (k < 0) return -1;
  {
    int pk = partner_of(s, k);
    if (pk >= 0) kill_voice(s, pk, KILL_MS);              /* its other half fades with it */
  }
  kill_voice(s, k, 0);
  slot_free(s, k);
  return k;
}

/* starts slot i's region; 1 when it sounds */
static int try_start(SmpSynth *s, int i) {
  Slot *t = &s->sl[i];
  if (!smp_sfv_start(&s->v[i], s->cur, t->reg, t->key, t->vel, &s->ch[t->chan].c, s->rate,
                     floor(param(s, SMP_P_SF_TRANSPOSE) + 0.5), s->st, i, t->held))
    return 0;
  if (t->rt && ((t->reg->gen[G_MODES] & 3) == 1 || (t->reg->gen[G_MODES] & 3) == 3))
    smp_sfv_release(&s->v[i]);                /* a looping release trigger would never end: released at once */
  /* an offline render waits for the streamer when the note's first frames are past its head */
  if ((s->offline || s->render_offline) && s->st && smp_sfv_needs_ring_now(&s->v[i])) sfs_sync(s->st);
  t->st = V_LIVE;
  t->age = ++s->age;
  limit(s, i);
  return 1;
}

/* one voice stopped by another region (a polyphony limit), by its own off_mode (fast: the 5 ms kill; normal: its
   release), so that it no longer counts as sounding */
static void stop_by_mode(SmpSynth *s, int i) {
  if (s->sl[i].reg && s->sl[i].reg->cond.off_mode == 1 && s->sl[i].st == V_LIVE) {
    smp_sfv_off(&s->v[i]);
    if (!s->v[i].released) kill_voice(s, i, KILL_MS);       /* never loop forever on one that can't release */
  } else kill_voice(s, i, KILL_MS);
  if (s->sl[i].st == V_PENDING) slot_free(s, i);
}

/* SFZ's polyphony and note_polyphony, before region r starts on channel c: while the channel already has that
   many voices of r's group sounding (not released or fading), or that many of r's group on this key, the oldest
   of them stops (stop_by_mode). A stereo pair counts once (its follower isn't counted) and stops together; voices
   the current note event started are never counted or stopped, so a layered or stereo note keeps every voice. */
static void poly_limit(SmpSynth *s, int c, int key, const SmpSfRegion *r) {
  int pass;
  for (pass = 0; pass < 2; pass++) {
    int lim = pass ? r->cond.note_polyphony : r->cond.polyphony;
    if (!lim) continue;
    for (;;) {
      int i, n = 0, old = -1, pk;
      for (i = 0; i < MAXV; i++) {
        Slot *t = &s->sl[i];
        if (t->st == V_FREE || t->chan != c || !t->reg || t->reg->cond.group != r->cond.group) continue;
        if (t->reg->cond.pair || t->ev == s->serial) continue;
        if (pass && t->key != key) continue;
        if (t->st == V_LIVE && (s->v[i].released || s->v[i].killing)) continue;
        n++;
        if (old < 0 || t->age < s->sl[old].age) old = i;
      }
      if (n < lim || old < 0) break;
      pk = partner_of(s, old);
      stop_by_mode(s, old);
      if (pk >= 0) stop_by_mode(s, pk);
    }
  }
}

/* held: a release trigger's seconds held (it starts with its key up), else negative */
static void start_region(SmpSynth *s, int c, int key, int vel, const SmpSfRegion *r, double held) {
  int i; Slot *t;
  if (r->cond.polyphony || r->cond.note_polyphony) poly_limit(s, c, key, r);
  i = free_slot(s);
  if (i < 0) return;
  t = &s->sl[i];
  memset(t, 0, sizeof *t);
  t->chan = c; t->key = key; t->vel = vel; t->keydown = held < 0; t->reg = r; t->excl = r->gen[G_EXCL];
  t->rt = held >= 0; t->held = held;
  t->vid = ++s->vid; t->ev = s->serial; t->partner = -1;
  /* a stereo pair's follower links with its leader, the region just before it, started by this same event */
  if (r->cond.pair && s->last_reg == r - 1 && s->last_slot >= 0 && s->last_slot != i && s->sl[s->last_slot].vid == s->last_vid &&
      s->sl[s->last_slot].st != V_FREE) {
    t->partner = s->last_slot; t->partner_vid = s->last_vid;
    s->sl[s->last_slot].partner = i; s->sl[s->last_slot].partner_vid = t->vid;
  }
  s->last_reg = r; s->last_slot = i; s->last_vid = t->vid;
  t->st = V_PENDING;
  t->since = GetTickCount64();
  if (try_start(s, i)) return;
  if (s->offline || s->render_offline) {   /* an offline render waits, so the note starts on its frame */
    smp_sf_wait_idle(s->cur, 10000);
    if (try_start(s, i)) return;
  }
  /* pending: it starts on the first block its sample is there (late, from the beginning) */
}

static int sustained(const Chan *h) { return h->c.cc[64] >= 64; }
static int sostenuto(const Chan *h) { return h->c.cc[66] >= 64; }

/* ---- region conditions (see the top) ---- */

static int bit(const uint8_t *m, int key) { return m && key >= 0 && key < 128 && (m[key >> 3] >> (key & 7) & 1); }

/* the random draw, in [0, 1): xorshift32's top 24 bits */
static float draw(SmpSynth *s) {
  uint32_t x = s->rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  s->rng = x;
  return (float)((x >> 8) * (1.0 / 16777216.0));
}

/* 1 when region r plays for this note event on channel c: a note-on (release 0; others is how many other keys are
   down) or a note-off's release triggers (release 1). Its round-robin set advances here, once per event (serial),
   when everything else holds, so call this for every region of the event, in any order, as often as needed. */
static int cond_ok(SmpSynth *s, int c, const SmpSfRegion *r, int key, int vel, int release, float rnd, int others) {
  Chan *h = &s->ch[c]; const SmpSfCond *d = &r->cond; int i;
  if (key < r->lokey || key > r->hikey || vel < r->lovel || vel > r->hivel) return 0;
  if (release) { if (d->trigger != SMP_TRIG_RELEASE) return 0; }
  else switch (d->trigger) {
    case SMP_TRIG_ATTACK: break;
    case SMP_TRIG_FIRST: if (others) return 0; break;
    case SMP_TRIG_LEGATO: if (!others) return 0; break;
    default: return 0;
  }
  if (rnd < d->lorand || (rnd >= d->hirand && d->hirand < 1)) return 0;
  if (d->sw_lokey >= 0 && key >= d->sw_lokey && key <= d->sw_hikey) return 0;
  if (d->sw_last >= 0 && (h->sw_last >= 0 ? h->sw_last : d->sw_default) != d->sw_last) return 0;
  if (d->sw_down >= 0 && !h->held[d->sw_down]) return 0;
  if (d->sw_up >= 0 && h->held[d->sw_up]) return 0;
  if (d->sw_previous >= 0 && h->prev_key != d->sw_previous) return 0;
  for (i = 0; i < d->locc_n && i < 4; i++)
    if (h->c.cc[d->locc[i].cc & 127] < d->locc[i].lo || h->c.cc[d->locc[i].cc & 127] > d->locc[i].hi) return 0;
  if (d->seq_len && r->rr_set >= 0) {
    Seq *q = s->sq; uint32_t *cnt;
    if (!q || r->rr_set >= q->n) return d->seq_pos == 1;   /* never (the counters fit the font): the first step */
    cnt = &q->seq[(size_t)c * q->n + r->rr_set];
    if (q->stamp[r->rr_set] != s->serial) { q->stamp[r->rr_set] = s->serial; (*cnt)++; }
    if ((int)((*cnt - 1) % d->seq_len) + 1 != d->seq_pos) return 0;
  }
  return 1;
}

/* before region r starts on channel c: its exclusive class (SF2) ends the channel's voices of that class (5 ms),
   and its group stops the channel's voices whose off_by is that group, each by its own off_mode */
static void make_room(SmpSynth *s, int c, const SmpSfRegion *r) {
  int i, ex = r->gen[G_EXCL]; uint32_t g = r->cond.group;
  for (i = 0; i < MAXV; i++) {
    Slot *t = &s->sl[i];
    if (t->st == V_FREE || t->chan != c) continue;
    if (ex && t->excl == ex) kill_voice(s, i, KILL_MS);
    else if (g && t->reg && t->reg->cond.off_by == g) {
      if (t->reg->cond.off_mode == 1) stop_voice(s, i); else kill_voice(s, i, KILL_MS);
    }
  }
}

/* the regions of one note event (only those covering its key, by the font's key index; regs, idx and n from
   sf_key_regions): first what they stop (exclusive classes, off_by), then they start. That first pass runs only
   for a preset with regions that can stop others, and then only on those regions. A region whose crossfades put
   the note at 0 starts no voice. */
static void start_event(SmpSynth *s, int c, int key, int vel, int release, float rnd, int others, double held,
                        const SmpSfRegion *regs, const uint32_t *idx, int n, int flags) {
  int k;
  s->serial++;
  s->last_reg = NULL; s->last_slot = -1;
  if (flags & SF_PF_ROOM)
    for (k = 0; k < n; k++) {
      const SmpSfRegion *r = &regs[idx ? idx[k] : k];
      if (!r->gen[G_EXCL] && !r->cond.group) continue;
      if (cond_ok(s, c, r, key, vel, release, rnd, others)) make_room(s, c, r);
    }
  s->protect = s->age + 1;                      /* the voices this event starts are never its victims */
  for (k = 0; k < n; k++) {
    const SmpSfRegion *r = &regs[idx ? idx[k] : k];
    if (cond_ok(s, c, r, key, vel, release, rnd, others) && !smp_sfv_xf_silent(r, key, vel))
      start_region(s, c, key, vel, r, held);
  }
  s->protect = 0;
}

/* a key's release triggers, now: at the note-on's velocity and draw, held for the time since the note-on. Nothing
   to do (not even a look at the regions) for a preset with no release regions. */
static void release_triggers(SmpSynth *s, int c, int key) {
  Chan *h = &s->ch[c]; const SmpSfRegion *regs; const uint32_t *idx; int n, flags;
  if (!listens(s, c) || !s->cur || h->preset < 0) return;
  n = sf_key_regions(s->cur, h->preset, key, &regs, &idx, &flags);
  if (!(flags & SF_PF_RELEASE) || bit(sf_preset_swmask(s->cur, h->preset), key)) return;   /* or a keyswitch key */
  start_event(s, c, key, h->on_vel[key], 1, h->rnd[key], 0, (double)(s->now - h->on_at[key]) / s->rate,
              regs, idx, n, flags);
}

static void note_off(SmpSynth *s, int c, int key) {
  Chan *h = &s->ch[c]; int i;
  for (i = 0; i < MAXV; i++) {
    Slot *t = &s->sl[i];
    if (t->st == V_FREE || t->chan != c || t->key != key || !t->keydown) continue;
    t->keydown = 0;
    if (sustained(h) || (t->sost && sostenuto(h))) continue;   /* the pedals hold it */
    release_voice(s, i);
  }
  if (h->held[key]) {
    h->held[key] = 0;
    if (sustained(h) || (h->ksost[key] && sostenuto(h))) h->rel_wait[key] = 1;   /* at the pedal's let-go */
    else release_triggers(s, c, key);
  }
}

static void note_on(SmpSynth *s, int c, int key, int vel) {
  Chan *h = &s->ch[c]; const uint8_t *mask; const SmpSfRegion *regs; const uint32_t *idx; int i, n, flags, others = 0;
  if (!listens(s, c) || !s->cur || h->preset < 0) return;
  mask = sf_preset_swmask(s->cur, h->preset);
  for (i = 0; i < 128; i++) if (i != key && h->held[i] && !bit(mask, i)) others++;
  h->held[key] = 1; h->ksost[key] = 0;
  h->rel_wait[key] = 0;                         /* struck again: a release still waiting for the pedal is dropped */
  if (bit(mask, key)) {                         /* a keyswitch: it never sounds */
    h->sw_last = key;
    h->prev_key = key;
    return;
  }
  h->on_vel[key] = (uint8_t)vel; h->on_at[key] = s->now; h->rnd[key] = draw(s);
  /* the same key again: the earlier voices of that key are released (as FluidSynth does); release triggers
     play on */
  for (i = 0; i < MAXV; i++) {
    Slot *t = &s->sl[i];
    if (t->st == V_FREE || t->chan != c || t->key != key || t->rt) continue;
    t->keydown = 0; t->sost = 0;
    release_voice(s, i);
  }
  n = sf_key_regions(s->cur, h->preset, key, &regs, &idx, &flags);
  start_event(s, c, key, vel, 0, h->rnd[key], others, -1, regs, idx, n, flags);
  h->prev_key = key;
}

/* the voices of channel c whose keys are up and whose pedals have let go are released, and the release triggers
   that waited for the pedals play */
static void let_go(SmpSynth *s, int c) {
  Chan *h = &s->ch[c]; int i;
  for (i = 0; i < MAXV; i++) {
    Slot *t = &s->sl[i];
    if (t->st == V_FREE || t->chan != c || t->keydown || t->rt) continue;
    if (t->sost && !sostenuto(h)) t->sost = 0;
    if (sustained(h) || t->sost) continue;
    release_voice(s, i);
  }
  for (i = 0; i < 128; i++) {
    if (!h->rel_wait[i] || h->held[i] || sustained(h) || (h->ksost[i] && sostenuto(h))) continue;
    h->rel_wait[i] = 0;
    release_triggers(s, c, i);
  }
}

static void update_chan(SmpSynth *s, int c) {
  int i;
  for (i = 0; i < MAXV; i++) if (s->sl[i].st == V_LIVE && s->sl[i].chan == c) smp_sfv_update(&s->v[i], &s->ch[c].c);
}

static void all_notes_off(SmpSynth *s, int c) {   /* CC123: every key up, the pedals still respected; no release
                                                     triggers (a stop shouldn't burst into release samples) */
  int i;
  for (i = 0; i < MAXV; i++) if (s->sl[i].st != V_FREE && s->sl[i].chan == c) s->sl[i].keydown = 0;
  memset(s->ch[c].held, 0, sizeof s->ch[c].held);
  memset(s->ch[c].rel_wait, 0, sizeof s->ch[c].rel_wait);
  let_go(s, c);
}

static void sound_off(SmpSynth *s, int c) {       /* CC120, and resets (c < 0: every channel) */
  int i;
  for (i = 0; i < MAXV; i++) if (s->sl[i].st != V_FREE && (c < 0 || s->sl[i].chan == c)) kill_voice(s, i, KILL_MS);
  for (i = 0; i < NCH; i++) if (c < 0 || i == c) memset(s->ch[i].rel_wait, 0, sizeof s->ch[i].rel_wait);
}

static void tuning(Chan *h) {
  h->c.bend_range_cents = h->range_semis * 100 + h->range_cents;
  h->c.fine_cents = (int)floor((h->fine14 - 8192) * 100.0 / 8192.0 + 0.5);
}

/* data entry: CC6 (msb 1) or CC38 (msb 0) for the selected RPN; NRPNs are ignored */
static void data_entry(Chan *h, int msb, int v) {
  if (h->nrpn || h->rpn_msb != 0) return;
  switch (h->rpn_lsb) {
    case 0: if (msb) h->range_semis = v; else h->range_cents = v; break;               /* bend range */
    case 1: h->fine14 = msb ? (v << 7 | (h->fine14 & 127)) : ((h->fine14 & ~127) | v); break;   /* fine */
    case 2: if (msb) h->c.coarse_semis = v - 64; break;                              /* coarse tuning */
    default: return;
  }
  tuning(h);
}

static void control(SmpSynth *s, int c, int n, int v) {
  Chan *h = &s->ch[c];
  int was_sus = sustained(h), was_sost = sostenuto(h);
  if (n >= 120) {
    if (n == 120) sound_off(s, c);
    else if (n == 121) { chan_controllers(h); tuning(h); let_go(s, c); update_chan(s, c); }
    else if (n >= 123) all_notes_off(s, c);    /* 123, and the mode messages 124..127 that imply it */
    /* 122, Local Control, has nothing to do in a synth without a keyboard */
    return;
  }
  h->c.cc[n] = (uint8_t)v;
  switch (n) {
    case 0: h->msb = v; break;
    case 32: h->lsb = v; break;
    case 6: data_entry(h, 1, v); break;
    case 38: data_entry(h, 0, v); break;
    case 98: case 99: h->nrpn = 1; break;
    case 100: h->rpn_lsb = v; h->nrpn = 0; break;
    case 101: h->rpn_msb = v; h->nrpn = 0; break;
    case 64: if (was_sus && !sustained(h)) let_go(s, c); break;
    case 66:
      if (!was_sost && sostenuto(h)) {         /* catches the notes down right now */
        int i;
        for (i = 0; i < MAXV; i++) if (s->sl[i].st != V_FREE && s->sl[i].chan == c && s->sl[i].keydown) s->sl[i].sost = 1;
        for (i = 0; i < 128; i++) h->ksost[i] = h->held[i];
      } else if (was_sost && !sostenuto(h)) {
        let_go(s, c);
        memset(h->ksost, 0, sizeof h->ksost);
      }
      break;
  }
  update_chan(s, c);
}

static void program(SmpSynth *s, int c, int p) {
  Chan *h = &s->ch[c];
  /* CC0 is the bank; CC32 counts only when CC0 is 0 and the font has that bank (XG style) */
  h->bank = h->msb;
  if (h->msb == 0 && h->lsb != 0 && s->cur && has_bank(s->cur, h->lsb)) h->bank = h->lsb;
  h->program = p;
  resolve(s, c);
}

static void system_reset(SmpSynth *s) {           /* GM, GS and XG reset: every channel back to power-on */
  int c;
  sound_off(s, -1);
  for (c = 0; c < NCH; c++) chan_defaults(&s->ch[c]);
  seq_clear(s, -1);
  resolve_all(s);
}

/* ---- sf_used presets ---- */
static void used_release(SmpSynth *s, LONG who) {   /* releases them if held; who is U_MAIN or U_AUDIO */
  int i;
  if (InterlockedCompareExchange(&s->used_state, who, U_HELD) != U_HELD) return;
  for (i = 0; i < s->nused; i++) smp_sf_use(s->used_font, s->used[i], -1);
  InterlockedExchange(&s->used_state, U_NONE);
}

static void used_drop_main(SmpSynth *s) {
  used_release(s, U_MAIN);
  while (s->used_state == U_AUDIO) Sleep(0);       /* the audio thread is releasing them: a moment */
}

static void used_set(SmpSynth *s, SmpFont *f, const int *list, int n) {
  int i;
  used_drop_main(s);
  if (!f || n <= 0) return;
  s->used_font = f; s->nused = n;
  for (i = 0; i < n; i++) { s->used[i] = list[i]; smp_sf_use(f, list[i], 1); }
  InterlockedExchange(&s->used_left, (LONG)(HOLD_SECONDS * s->rate));
  InterlockedExchange(&s->used_armed, 0);
  InterlockedExchange(&s->used_state, U_HELD);
}

/* ---- life cycle ---- */
SMP_API SmpSynth *smp_synth_new(double rate, int max_block) {
  SmpSynth *s; int i;
  if (!(rate > 0) || max_block <= 0) return NULL;
  s = (SmpSynth *)calloc(1, sizeof *s);
  if (!s) return NULL;
  s->rate = rate; s->max_block = max_block;
  s->v = (SmpSfVoice *)calloc(MAXV, sizeof *s->v);
  s->sl = (Slot *)calloc(MAXV, sizeof *s->sl);
  s->rev = (float *)calloc((size_t)max_block, sizeof(float));
  s->cho = (float *)calloc((size_t)max_block, sizeof(float));
  s->fx = smp_sffx_new(rate);
  if (!s->v || !s->sl || !s->rev || !s->cho || !s->fx) {
    free(s->v); free(s->sl); free(s->rev); free(s->cho); smp_sffx_free(s->fx); free(s); return NULL;
  }
  for (i = SMP_NPARAMS; i < SMP_NALLPARAMS; i++) s->p[i - SMP_NPARAMS] = bits_of(smp_param_info(i)->def);
  s->rng = 0x9E3779B9u;                        /* the same draws in every new synth */
  for (i = 0; i < NCH; i++) { s->ch[i].preset = -1; chan_defaults(&s->ch[i]); s->chan_preset[i] = -1; }
  cfg_default(&s->mcfg); cfg_default(&s->cfg);
  cfg_publish(s); s->cfg_seen = s->cfg_seq;
  s->gain = 1; s->test_wait = 1;
  return s;
}

/* the audio side lets go of its font: every voice ends at once and every use count is dropped */
static void drop_font(SmpSynth *s) {
  int i;
  for (i = 0; i < MAXV; i++) { if (s->sl[i].st == V_LIVE) smp_sfv_kill(&s->v[i], 0); s->sl[i].st = V_FREE; }
  for (i = 0; i < NCH; i++) {
    if (s->ch[i].preset >= 0 && s->cur) smp_sf_use(s->cur, s->ch[i].preset, -1);
    s->ch[i].preset = -1;
    InterlockedExchange(&s->chan_preset[i], -1);
    chan_cond_reset(&s->ch[i]);
  }
}

SMP_API void smp_synth_free(SmpSynth *s) {
  int i;
  if (!s) return;
  drop_font(s);
  sfs_free(s->st);                             /* every voice has ended: the streamer releases its holds and stops */
  s->st = NULL;
  used_drop_main(s);
  if (s->font) smp_sf_close(s->font);
  for (i = 0; i < s->nret; i++) smp_sf_close(s->ret[i].f);
  for (i = 0; i < s->nsq; i++) free(s->sq_all[i]);
  free(s->sq_all);
  free(s->ret); free(s->v); free(s->sl); free(s->rev); free(s->cho);
  smp_sffx_free(s->fx);
  free(s);
}

SMP_API int smp_synth_collect(SmpSynth *s) {
  int i = 0, synced = 0;
  if (!s) return 0;
  while (i < s->nret) {
    SmpFont *f = s->ret[i].f;
    if (s->epoch >= s->ret[i].a0 + 2 && (s->cur != f || f == s->font)) {
      /* the audio thread has let go of f and ended its voices; the streamer lets go on its next whole pass */
      if (s->st && !synced) {
        if (!sfs_sync_for(s->st, COLLECT_WAIT_MS)) break;   /* a slow disk: closed on a later call */
        synced = 1;
      }
      if (s->used_font == f && f != s->font) used_drop_main(s);
      smp_sf_close(f);
      s->ret[i] = s->ret[--s->nret];
      continue;
    }
    i++;
  }
  return s->nret;
}

SMP_API SmpFont *smp_synth_font(const SmpSynth *s) { return s ? s->font : NULL; }

SMP_API int smp_synth_load(SmpSynth *s, const char *path, char *err, int cap) {
  SmpFont *f;
  if (err && cap > 0) err[0] = 0;
  if (!s || !path) { if (err && cap > 0) snprintf(err, (size_t)cap, "no player or no path"); return 0; }
  if (strlen(path) >= sizeof s->path) { if (err && cap > 0) snprintf(err, (size_t)cap, "the path is too long"); return 0; }
  f = smp_sf_open(path, err, cap);
  if (!f) return 0;
  return smp_synth_load_font(s, f, path, err, cap);
}

SMP_API int smp_synth_load_font(SmpSynth *s, SmpFont *f, const char *path, char *err, int cap) {
  SmpFont *old;
  if (!s || !f) { if (f) smp_sf_close(f); if (err && cap > 0) snprintf(err, (size_t)cap, "no player or no font"); return 0; }
  if (!path) path = f->path;
  if (strlen(path) >= sizeof s->path) { smp_sf_close(f); if (err && cap > 0) snprintf(err, (size_t)cap, "the path is too long"); return 0; }
  if (f->has_long && !s->st) {                 /* long samples stream: the streamer starts now, before any note */
    SfStream *st = sfs_new(MAXV);
    if (!st) { smp_sf_close(f); if (err && cap > 0) snprintf(err, (size_t)cap, "out of memory"); return 0; }
    InterlockedExchangePointer((PVOID volatile *)&s->st, st);
  }
  if (s->nret == s->cret) {                    /* room on the retired list first, so nothing can fail later */
    int c = s->cret ? s->cret * 2 : 4;
    Retired *n = (Retired *)realloc(s->ret, sizeof *n * (size_t)c);
    if (!n) { smp_sf_close(f); if (err && cap > 0) snprintf(err, (size_t)cap, "out of memory"); return 0; }
    s->ret = n; s->cret = c;
  }
  if (f->max_sets > 0 && (!s->sq_want || s->sq_want->n < f->max_sets)) {   /* round-robin counters that fit */
    size_t nn = (size_t)f->max_sets;
    Seq **all = (Seq **)realloc(s->sq_all, sizeof *all * (size_t)(s->nsq + 1)), *q;
    if (all) s->sq_all = all;
    q = all ? (Seq *)calloc(1, sizeof(Seq) + sizeof(uint32_t) * (nn * NCH + nn)) : NULL;
    if (!q) { smp_sf_close(f); if (err && cap > 0) snprintf(err, (size_t)cap, "out of memory"); return 0; }
    q->n = f->max_sets; q->stamp = q->seq + nn * NCH;
    s->sq_all[s->nsq++] = q;
    InterlockedExchangePointer((PVOID volatile *)&s->sq_want, q);   /* before the font, which it must fit */
  }
  if (s->used_font != f) used_drop_main(s);    /* the old font's sf_used presets mean nothing now */
  old = s->font;
  s->font = f;
  snprintf(s->path, sizeof s->path, "%s", path);
  InterlockedExchangePointer((PVOID volatile *)&s->want, f);
  if (old) { s->ret[s->nret].f = old; s->ret[s->nret].a0 = s->epoch; s->nret++; }
  smp_synth_collect(s);
  return 1;
}

/* ---- events ---- */
static int is_reset_sysex(const uint8_t *m, int len) {
  if (len == 6 && m[1] == 0x7E && m[3] == 0x09 && m[4] == 0x01 && m[5] == 0xF7) return 1;          /* GM */
  if (len == 11 && m[1] == 0x41 && m[3] == 0x42 && m[4] == 0x12 && m[5] == 0x40 && m[6] == 0x00 &&
      m[7] == 0x7F && m[8] == 0x00 && m[9] == 0x41 && m[10] == 0xF7) return 1;                     /* GS */
  if (len == 9 && m[1] == 0x43 && (m[2] & 0xF0) == 0x10 && m[3] == 0x4C && m[4] == 0 && m[5] == 0 &&
      m[6] == 0x7E && m[7] == 0 && m[8] == 0xF7) return 1;                                         /* XG */
  return 0;
}

static void push(SmpSynth *s, int kind, const uint8_t *b, int at) {
  Ev *e;
  if (s->nev >= MAXEV) return;
  e = &s->ev[s->nev++];
  e->kind = kind; e->at = at < 0 ? 0 : at;
  e->b[0] = b ? b[0] : 0; e->b[1] = b ? b[1] : 0; e->b[2] = b ? b[2] : 0;
}

SMP_API void smp_synth_midi(SmpSynth *s, const uint8_t *msg, int len, int at) {
  uint8_t b[3] = {0, 0, 0}; int need, st;
  if (!s || !msg || len < 1) return;
  if (msg[0] == 0xF0) { if (is_reset_sysex(msg, len)) push(s, EV_SYSRESET, NULL, at); return; }
  st = msg[0] & 0xF0;
  if (st < 0x80 || st == 0xF0) return;
  need = (st == 0xC0 || st == 0xD0) ? 2 : 3;
  if (len < need) return;
  b[0] = msg[0]; b[1] = msg[1] & 127; b[2] = need == 3 ? (msg[2] & 127) : 0;
  push(s, EV_MSG, b, at);
}

static void reset_now(SmpSynth *s);

static void handle(SmpSynth *s, const Ev *e) {
  int c = e->b[0] & 15, d1 = e->b[1], d2 = e->b[2];
  Chan *h = &s->ch[c];
  if (e->kind == EV_SYSRESET) { system_reset(s); return; }
  if (e->kind == EV_RESET) { reset_now(s); return; }
  switch (e->b[0] & 0xF0) {
    case 0x80: note_off(s, c, d1); break;
    case 0x90:
      if (!d2) { note_off(s, c, d1); break; }
      if (s->used_state == U_HELD && !s->used_armed) InterlockedExchange(&s->used_armed, 1);   /* the hold's 10 s start */
      note_on(s, c, d1, d2);
      break;
    case 0xA0: h->c.poly[d1] = d2; update_chan(s, c); break;
    case 0xB0: control(s, c, d1, d2); break;
    case 0xC0: program(s, c, d1); break;
    case 0xD0: h->c.pressure = d1; update_chan(s, c); break;
    case 0xE0: h->c.bend = d1 | d2 << 7; update_chan(s, c); break;
  }
}

/* every voice off and the reverb and chorus tails gone (at once, or faded: voices over 5 ms, tails over 10 ms, so
   a stop, seek or loop doesn't click), controllers reset; the sf_used presets stay */
static void flush_now(SmpSynth *s, int now) {
  int c, i;
  if (now) { for (i = 0; i < MAXV; i++) { if (s->sl[i].st == V_LIVE) smp_sfv_kill(&s->v[i], 0); s->sl[i].st = V_FREE; } }
  else sound_off(s, -1);
  for (c = 0; c < NCH; c++) {
    chan_controllers(&s->ch[c]); tuning(&s->ch[c]);
    /* no key is down any more; the keyswitch, the previous note and the round-robin counters stay */
    memset(s->ch[c].held, 0, sizeof s->ch[c].held);
    memset(s->ch[c].rel_wait, 0, sizeof s->ch[c].rel_wait);
    memset(s->ch[c].ksost, 0, sizeof s->ch[c].ksost);
  }
  if (now) smp_sffx_clear(s->fx); else smp_sffx_fade_out(s->fx);
}

/* the flush, and the sf_used presets let go */
static void reset_now(SmpSynth *s) {
  flush_now(s, 0);
  used_release(s, U_AUDIO);                    /* the song's own program changes have settled by now */
}

/* ---- process ---- */
static void render_voices(SmpSynth *s, float *const *outs, int a, int b, int base) {
  int i;
  for (i = 0; i < MAXV; i++) {
    int o; float *l, *r;
    if (s->sl[i].st != V_LIVE) continue;
    o = out_of(s, s->sl[i].chan);
    if (!outs[2 * o] || !outs[2 * o + 1]) o = 0;
    l = outs[2 * o]; r = outs[2 * o + 1];
    /* effect sends only from output 1: separated channels stay dry */
    if (!smp_sfv_render(&s->v[i], l + a, r + a, o == 0 ? s->rev + (a - base) : NULL,
                        o == 0 ? s->cho + (a - base) : NULL, b - a))
      slot_free(s, i);
  }
}

static int any_streaming(const SmpSynth *s) {
  int i;
  for (i = 0; i < MAXV; i++) if (s->sl[i].st == V_LIVE && s->v[i].ss) return 1;
  return 0;
}

static int any_ring_low(SmpSynth *s) {
  int i;
  for (i = 0; i < MAXV; i++) if (s->sl[i].st == V_LIVE && smp_sfv_ring_low(&s->v[i])) return 1;
  return 0;
}

/* frames a step may play: the fastest streaming voice (with room for its pitch to rise in the step) must not go
   through more than a quarter ring, so that and the voice's look-ahead (another quarter at most) fit in the half
   ring a ring holds ahead of it after a top-up */
static int offline_step(const SmpSynth *s) {
  int i; double rmax = 1, n;
  for (i = 0; i < MAXV; i++) {
    if (s->sl[i].st != V_LIVE || !s->v[i].ss) continue;
    if (!s->v[i].primed) return SMP_SFV_CTL;    /* a new voice: its pitch is known after its first stretch */
    if (s->v[i].ratio > rmax) rmax = s->v[i].ratio;
  }
  n = (SF_RING / 4 - 64) / (rmax * 1.5);
  return n >= OFFLINE_STEP ? OFFLINE_STEP : n < 1 ? 1 : (int)n;
}

/* Offline, voices that stream are played a step at a time (offline_step), and before a step in which any ring holds
   less than half a ring ahead of its voice the streamer tops every ring up, so the render is exact at any pitch.
   The steps change nothing else: every voice carries its state. */
static void render(SmpSynth *s, float *const *outs, int a, int b, int base) {
  if ((s->offline || s->render_offline) && s->st && any_streaming(s)) {
    while (a < b) {
      int step = offline_step(s), e = b - a > step ? a + step : b;
      if (any_ring_low(s)) sfs_sync(s->st);
      render_voices(s, outs, a, e, base);
      a = e;
    }
    return;
  }
  render_voices(s, outs, a, b, base);
}

/* the start of a block: configuration, the font, the sf_used countdown (once a note-on armed it), pending voices */
static void block_start(SmpSynth *s, int n) {
  SmpFont *w = s->want; int i, changed = cfg_take(s);
  if (w != s->cur) {
    drop_font(s);
    s->sq = s->sq_want;                         /* published before the font, so it fits w */
    seq_clear(s, -1);
    InterlockedExchangePointer((PVOID volatile *)&s->cur, w);
    changed = 1;
  }
  if (s->dormant) {                             /* only what the channels would play, without using it */
    if (changed) for (i = 0; i < NCH; i++) InterlockedExchange(&s->chan_preset[i], want_preset(s, i));
  } else if (changed) resolve_all(s);
  if (s->used_state == U_HELD && s->used_armed && InterlockedExchangeAdd(&s->used_left, -n) - n <= 0)
    used_release(s, U_AUDIO);
  for (i = 0; i < MAXV; i++) {
    Slot *t = &s->sl[i];
    if (t->st != V_PENDING) continue;
    if (try_start(s, i)) continue;
    /* waiting longer than PENDING_MS of real time: dropped. Timed in real time, not in frames, so a render that
       runs faster than real time without telling us (no clap.render) still gets its late notes; an offline
       render never drops one (it waits for the loader, and a missing file must not lose later notes either) */
    if (!s->offline && !s->render_offline && GetTickCount64() - t->since > PENDING_MS) slot_free(s, i);
  }
  limit(s, -1);                                 /* sf_voices may have been lowered */
}

SMP_API void smp_synth_process(SmpSynth *s, float *const *outs, int n) {
  int i, j, k = 0, base;
  double g0, g1, vol;
  if (!s || !outs || n <= 0 || !outs[0] || !outs[1]) { if (s) s->nev = 0; return; }
  for (i = 0; i < 2 * NOUT; i++) if (outs[i]) memset(outs[i], 0, sizeof(float) * (size_t)n);
  if (s->dormant) {                             /* playing again: the channels' presets are used again */
    block_start(s, n);
    s->dormant = 0;
    resolve_all(s);
    if (s->render_offline && s->cur) smp_sf_wait_idle(s->cur, 10000);
  } else {
    if (s->render_offline && s->cur) smp_sf_wait_idle(s->cur, 10000);   /* offline: voices waiting start now */
    block_start(s, n);
  }
  for (i = 1; i < s->nev; i++) {               /* stable sort by offset */
    Ev x = s->ev[i];
    for (j = i - 1; j >= 0 && s->ev[j].at > x.at; j--) s->ev[j + 1] = s->ev[j];
    s->ev[j + 1] = x;
  }
  for (base = 0; base < n; base += s->max_block) {   /* the send buffers hold max_block frames */
    int end = n - base < s->max_block ? n : base + s->max_block, pos = base;
    memset(s->rev, 0, sizeof(float) * (size_t)(end - base));
    memset(s->cho, 0, sizeof(float) * (size_t)(end - base));
    for (; k < s->nev; k++) {
      int at = s->ev[k].at >= n ? n - 1 : s->ev[k].at;
      if (at >= end) break;
      if (at > pos) { render(s, outs, pos, at, base); pos = at; }
      s->now = s->clock + (uint64_t)at;
      handle(s, &s->ev[k]);
    }
    render(s, outs, pos, end, base);
    /* the effects add to output 1 here, before sf_volume below, so their return gets the same gain and ramp */
    smp_sffx_enable(s->fx, param(s, SMP_P_SF_EFFECTS) >= 0.5);
    smp_sffx_process(s->fx, s->rev, s->cho, outs[0] + base, outs[1] + base, end - base);
  }
  s->nev = 0;
  /* sf_volume, stepped over the block */
  vol = param(s, SMP_P_SF_VOLUME);
  g1 = vol <= -60 ? 0 : pow(10.0, vol / 20.0);
  g0 = s->gain;
  if (g0 != 1 || g1 != 1) {
    for (i = 0; i < 2 * NOUT; i++) {
      float *o = outs[i];
      if (!o) continue;
      for (j = 0; j < n; j++) o[j] *= (float)(g0 + (g1 - g0) * (j + 1) / n);
    }
  }
  s->gain = g1;
  if (s->st) {                                  /* underruns counted, and the streamer woken when voices stream */
    int any = 0;
    for (i = 0; i < MAXV; i++) {
      if (s->v[i].underrun) { s->v[i].underrun = 0; InterlockedIncrement(&s->underruns); }
      if (s->v[i].live && s->v[i].ss) any = 1;
    }
    if (any) sfs_wake(s->st);
  }
  s->clock += (uint64_t)n;
  InterlockedIncrement64(&s->epoch);
}

SMP_API void smp_synth_reset(SmpSynth *s) {
  if (!s) return;
  reset_now(s);
  s->nev = 0;
}

SMP_API void smp_synth_flush(SmpSynth *s, int now) {
  if (!s) return;
  flush_now(s, now);
  s->nev = 0;
}

SMP_API int smp_synth_voices(const SmpSynth *s, int *peak) {
  int i, n = 0;
  if (peak) *peak = s ? s->peak : 0;
  if (!s) return 0;
  for (i = 0; i < MAXV; i++) if (sounding(s, i)) n++;
  return n;
}

SMP_API long smp_synth_underruns(const SmpSynth *s) { return s ? s->underruns : 0; }

SMP_API void smp_synth_test_stream_pause(SmpSynth *s, int on) { if (s && s->st) sfs_pause(s->st, on); }

SMP_API int smp_synth_test_voice(const SmpSynth *s, int i, int *chan, int *key, int *released) {
  if (!s || i < 0 || i >= MAXV || !sounding(s, i)) return 0;
  if (chan) *chan = s->sl[i].chan;
  if (key) *key = s->sl[i].key;
  if (released) *released = s->v[i].released;
  return 1;
}

SMP_API int smp_synth_wait_loaded(SmpSynth *s, int timeout_ms) {
  return s && s->font ? smp_sf_wait_idle(s->font, timeout_ms) : 0;
}

SMP_API void smp_synth_test_offline(SmpSynth *s, int wait) { if (s) s->test_wait = wait ? 1 : 0; }

SMP_API void smp_synth_test_seed(SmpSynth *s, unsigned seed) { if (s) s->rng = seed ? seed : 0x9E3779B9u; }

/* ---- for the plugin ---- */
SMP_API int smp_synth_unload(SmpSynth *s) {
  if (!s || !s->font) return 1;
  if (s->nret == s->cret) {
    int c = s->cret ? s->cret * 2 : 4;
    Retired *n = (Retired *)realloc(s->ret, sizeof *n * (size_t)c);
    if (!n) return 0;
    s->ret = n; s->cret = c;
  }
  used_drop_main(s);
  s->ret[s->nret].f = s->font; s->ret[s->nret].a0 = s->epoch; s->nret++;
  s->font = NULL; s->path[0] = 0;
  InterlockedExchangePointer((PVOID volatile *)&s->want, NULL);
  smp_synth_collect(s);
  return 1;
}

SMP_API void smp_synth_collect_now(SmpSynth *s) {
  if (!s) return;
  block_start(s, 0);                           /* nothing processes: do the audio thread's switch here */
  InterlockedExchangeAdd64(&s->epoch, 2);
  smp_synth_collect(s);
}

SMP_API void smp_synth_set_offline(SmpSynth *s, int on) { if (s) InterlockedExchange(&s->render_offline, on ? 1 : 0); }

SMP_API void smp_synth_skip(SmpSynth *s, int n) {
  int i;
  if (!s) return;
  s->nev = 0;
  block_start(s, n < 0 ? 0 : n);
  for (i = 0; i < MAXV; i++) { if (s->sl[i].st == V_LIVE) smp_sfv_kill(&s->v[i], 0); s->sl[i].st = V_FREE; }
  if (!s->dormant) {                           /* not playing: let the channels' presets and the sf_used ones go */
    for (i = 0; i < NCH; i++) {
      if (s->ch[i].preset >= 0 && s->cur) smp_sf_use(s->cur, s->ch[i].preset, -1);
      s->ch[i].preset = -1;                    /* chan_preset keeps what the channel would play */
    }
    s->dormant = 1;
  }
  used_release(s, U_AUDIO);                    /* also a state loaded while dormant: nothing would arm its hold */
  InterlockedIncrement64(&s->epoch);
}

SMP_API int smp_synth_loading(const SmpSynth *s) {
  SmpFont *f = s ? s->font : NULL; int i, k, n = 0, np;
  if (!f || InterlockedCompareExchange(&f->done_gen, 0, 0) == InterlockedCompareExchange(&f->use_gen, 0, 0)) return 0;
  np = smp_sf_preset_count(f);
  for (i = 0; i < np; i++) {
    const SmpSfRegion *r; int nr;
    if (InterlockedCompareExchange(&f->users[i], 0, 0) <= 0) continue;
    nr = smp_sf_regions(f, i, &r);
    for (k = 0; k < nr; k++) {
      int sm = r[k].sample;
      /* not there yet and not broken (the loader's state byte: a stale read only delays the count) */
      if (sm >= 0 && sm < f->nshdr && !f->pcm[sm] && f->state[sm] != SF_ST_BROKEN) { n++; break; }
    }
  }
  return n;
}

SMP_API int smp_synth_channel_preset(const SmpSynth *s, int ch, int *bank, int *program) {
  LONG p;
  if (!s || !s->font || ch < 1 || ch > NCH || s->cur != s->font) return 0;
  p = s->chan_preset[ch - 1];
  return p >= 0 && smp_sf_preset_info(s->font, (int)p, bank, program, NULL, 0);
}

/* ---- state lines ---- */
typedef struct { char *out; int cap; int n; } Out;

static void put(Out *o, const char *str) {
  size_t l = strlen(str);
  if (o->out && o->cap > 0 && (size_t)o->n < (size_t)o->cap) {
    size_t room = (size_t)o->cap - 1 - (size_t)o->n, c = l < room ? l : room;
    memcpy(o->out + o->n, str, c);
    o->out[o->n + (int)c] = 0;
  }
  o->n += (int)l;
}

SMP_API int smp_synth_script_write(const SmpSynth *s, char *out, int cap) {
  Out o = {out, cap, 0};
  char line[1200]; const Cfg *c; int i;
  if (out && cap > 0) out[0] = 0;
  if (!s) return 0;
  c = &s->mcfg;
  if (s->font) { snprintf(line, sizeof line, "soundfont \"%s\"\n", s->path); put(&o, line); }
  put(&o, c->mode == MODE_ONE ? "sf_mode one\n" : "sf_mode all\n");
  snprintf(line, sizeof line, "sf_preset %d %d\n", c->bank, c->program); put(&o, line);
  if (c->channel) snprintf(line, sizeof line, "sf_channel %d\n", c->channel);
  else snprintf(line, sizeof line, "sf_channel all\n");
  put(&o, line);
  for (i = 0; i < NCH; i++)
    if (c->pin[i] >= 0) { snprintf(line, sizeof line, "sf_channel_preset %d %d %d\n", i + 1, c->pin[i] / 128, c->pin[i] % 128); put(&o, line); }
  for (i = 0; i < NCH; i++)
    if (c->out[i]) { snprintf(line, sizeof line, "sf_output %d %d\n", i + 1, c->out[i] + 1); put(&o, line); }
  if (s->font) {                                /* the presets in use: the channels' and the held sf_used ones */
    int n = smp_sf_preset_count(s->font), any = 0;
    char *mark = n > 0 ? (char *)calloc((size_t)n, 1) : NULL;
    if (mark) {
      if (s->cur == s->font)
        for (i = 0; i < NCH; i++) { LONG p = s->chan_preset[i]; if (p >= 0 && p < n) mark[p] = 1; }
      if (s->used_state == U_HELD && s->used_font == s->font)
        for (i = 0; i < s->nused; i++) if (s->used[i] >= 0 && s->used[i] < n) mark[s->used[i]] = 1;
      for (i = 0; i < n; i++) {
        int b, p;
        if (!mark[i] || !smp_sf_preset_info(s->font, i, &b, &p, NULL, 0)) continue;
        snprintf(line, sizeof line, "%s%d:%d", any ? " " : "sf_used ", b, p);
        put(&o, line); any = 1;
      }
      if (any) put(&o, "\n");
      free(mark);
    }
  }
  for (i = SMP_NPARAMS; i < SMP_NALLPARAMS; i++) {
    char v[64];
    smp_value_text(i, param(s, i), v, sizeof v);
    snprintf(line, sizeof line, "%s %s\n", smp_param_info(i)->name, v);
    put(&o, line);
  }
  return o.n;
}

static void fail(char *err, int cap, int line, const char *msg) {
  if (err && cap > 0) snprintf(err, (size_t)cap, "line %d: %s", line, msg);
}

/* reads n whole numbers from str into v, each within [lo, hi]; 1 ok */
static int ints(const char *str, int n, int *v, const int *lo, const int *hi) {
  int i;
  for (i = 0; i < n; i++) {
    char *e; long x;
    while (isspace((unsigned char)*str)) str++;
    if (!isdigit((unsigned char)*str)) return 0;
    x = strtol(str, &e, 10);
    if (e == str || (*e && !isspace((unsigned char)*e)) || x < lo[i] || x > hi[i]) return 0;
    v[i] = (int)x; str = e;
  }
  while (isspace((unsigned char)*str)) str++;
  return *str == 0;
}

static int is_cfg_line(const char *name) {
  return !strcmp(name, "sf_mode") || !strcmp(name, "sf_preset") || !strcmp(name, "sf_channel") ||
         !strcmp(name, "sf_channel_preset") || !strcmp(name, "sf_output");
}

/* cfg_only: only the configuration lines are read and applied; the font, sf_used and the parameters stay */
static int script_apply(SmpSynth *s, const char *text, char *err, int cap, int cfg_only) {
  Cfg c; double val[NPAR]; char have[NPAR] = {0};
  char path[1024] = ""; int has_path = 0, path_line = 0;
  int ub[MAXUSED], up[MAXUSED], nu = 0, has_used = 0;
  const char *p = text; int lineno = 0, i;
  char m[1400];
  if (err && cap > 0) err[0] = 0;
  if (!s || !text) { if (err && cap > 0) snprintf(err, (size_t)cap, "no player or no text"); return 1; }
  cfg_default(&c);
  while (*p) {
    const char *eol = p; char buf[1100]; size_t len; char *str, *name, *rest;
    while (*eol && *eol != '\n') eol++;
    len = (size_t)(eol - p);
    lineno++;
    if (len >= sizeof buf) {                   /* a long line is somebody else's unless it is ours */
      const char *q = p;
      while (q < eol && isspace((unsigned char)*q)) q++;
      if (!strncmp(q, "sf_", 3) || !strncmp(q, "soundfont", 9)) { fail(err, cap, lineno, "the line is too long"); return 1; }
      p = *eol ? eol + 1 : eol; continue;
    }
    memcpy(buf, p, len); buf[len] = 0;
    p = *eol ? eol + 1 : eol;
    str = buf;
    while (isspace((unsigned char)*str)) str++;
    len = strlen(str);
    while (len && isspace((unsigned char)str[len - 1])) str[--len] = 0;
    name = rest = str;
    while (*rest && !isspace((unsigned char)*rest)) rest++;
    if (*rest) { *rest++ = 0; while (isspace((unsigned char)*rest)) rest++; }
    if (cfg_only && !is_cfg_line(name)) continue;
    if (!strcmp(name, "soundfont")) {
      char *q, *end;
      if (*rest != '"') { fail(err, cap, lineno, "soundfont needs the path in quotes, like soundfont \"D:\\fonts\\GM.sf2\""); return 1; }
      q = rest + 1;
      end = strchr(q, '"');
      if (!end) { fail(err, cap, lineno, "soundfont: the closing quote is missing"); return 1; }
      *end = 0;
      if (!*q) { fail(err, cap, lineno, "soundfont: the path is empty"); return 1; }
      if (end[1]) { snprintf(m, sizeof m, "soundfont: unexpected '%s' after the path", end + 1); fail(err, cap, lineno, m); return 1; }
      if (strlen(q) >= sizeof path) { fail(err, cap, lineno, "soundfont: the path is too long"); return 1; }
      snprintf(path, sizeof path, "%s", q); has_path = 1; path_line = lineno;
      continue;
    }
    if (strncmp(name, "sf_", 3)) continue;     /* the sampler's lines, plays, comments: not ours */
    if (!*rest) { snprintf(m, sizeof m, "%s needs a value", name); fail(err, cap, lineno, m); return 1; }
    if (!strcmp(name, "sf_mode")) {
      if (!strcmp(rest, "one")) c.mode = MODE_ONE;
      else if (!strcmp(rest, "all")) c.mode = MODE_ALL;
      else { snprintf(m, sizeof m, "sf_mode is one or all, not '%s'", rest); fail(err, cap, lineno, m); return 1; }
    } else if (!strcmp(name, "sf_preset")) {
      int v[2], lo[2] = {0, 0}, hi[2] = {16383, 127};
      if (!ints(rest, 2, v, lo, hi)) { snprintf(m, sizeof m, "sf_preset needs a bank and a program (0 to 127), not '%s'", rest); fail(err, cap, lineno, m); return 1; }
      c.bank = v[0]; c.program = v[1];
    } else if (!strcmp(name, "sf_channel")) {
      int v[1], lo[1] = {1}, hi[1] = {16};
      if (!strcmp(rest, "all")) c.channel = 0;
      else if (ints(rest, 1, v, lo, hi)) c.channel = v[0];
      else { snprintf(m, sizeof m, "sf_channel is all or 1 to 16, not '%s'", rest); fail(err, cap, lineno, m); return 1; }
    } else if (!strcmp(name, "sf_channel_preset")) {
      int v[3], lo[3] = {1, 0, 0}, hi[3] = {16, 16383, 127};
      if (!ints(rest, 3, v, lo, hi)) { snprintf(m, sizeof m, "sf_channel_preset needs a channel (1 to 16), a bank and a program, not '%s'", rest); fail(err, cap, lineno, m); return 1; }
      c.pin[v[0] - 1] = v[1] * 128 + v[2];
    } else if (!strcmp(name, "sf_output")) {
      int v[2], lo[2] = {1, 1}, hi[2] = {16, 16};
      if (!ints(rest, 2, v, lo, hi)) { snprintf(m, sizeof m, "sf_output needs a channel and an output, 1 to 16 each, not '%s'", rest); fail(err, cap, lineno, m); return 1; }
      c.out[v[0] - 1] = v[1] - 1;
    } else if (!strcmp(name, "sf_used")) {
      const char *q = rest;
      has_used = 1; nu = 0;
      while (*q) {
        char *e; long b, pr;
        if (!isdigit((unsigned char)*q)) goto bad_used;
        b = strtol(q, &e, 10);
        if (*e != ':' || !isdigit((unsigned char)e[1])) goto bad_used;
        q = e + 1;
        pr = strtol(q, &e, 10);
        if ((*e && !isspace((unsigned char)*e)) || b > 16383 || pr > 127) goto bad_used;
        if (nu < MAXUSED) { ub[nu] = (int)b; up[nu] = (int)pr; nu++; }
        q = e;
        while (isspace((unsigned char)*q)) q++;
      }
      continue;
    bad_used:
      snprintf(m, sizeof m, "sf_used lists presets as bank:program, like 0:25 128:0, not '%s'", rest);
      fail(err, cap, lineno, m); return 1;
    } else {
      int id = -1;
      for (i = SMP_NPARAMS; i < SMP_NALLPARAMS; i++) if (!strcmp(name, smp_param_info(i)->name)) id = i;
      if (id < 0) { snprintf(m, sizeof m, "unknown setting '%s'", name); fail(err, cap, lineno, m); return 1; }
      if (!smp_value_parse(id, rest, &val[id - SMP_NPARAMS])) {
        snprintf(m, sizeof m, "%s: can't read '%s'", name, rest); fail(err, cap, lineno, m); return 1;
      }
      have[id - SMP_NPARAMS] = 1;
    }
  }
  if (cfg_only) { s->mcfg = c; cfg_publish(s); return 0; }
  /* every line is good; the font is the last thing that can fail, so load it before changing anything */
  if (has_path && !(s->font && !_stricmp(path, s->path))) {
    char why[600] = "";
    if (!smp_synth_load(s, path, why, sizeof why)) {
      snprintf(m, sizeof m, "can't load the SoundFont \"%s\": %s", path, why[0] ? why : "it didn't open");
      fail(err, cap, path_line, m);
      return 1;
    }
  }
  s->mcfg = c;
  cfg_publish(s);
  for (i = SMP_NPARAMS; i < SMP_NALLPARAMS; i++)
    smp_synth_set_param(s, i, have[i - SMP_NPARAMS] ? val[i - SMP_NPARAMS] : smp_param_info(i)->def);
  {
    int list[MAXUSED], n = 0, k;
    if (has_used && s->font)
      for (k = 0; k < nu; k++) {
        int q = smp_sf_find_preset(s->font, ub[k], up[k]), d;
        for (d = 0; d < n && q >= 0; d++) if (list[d] == q) q = -1;
        if (q >= 0) list[n++] = q;
      }
    used_set(s, s->font, list, n);
  }
  smp_synth_collect(s);
  return 0;
}

SMP_API int smp_synth_script_apply(SmpSynth *s, const char *text, char *err, int cap) {
  return script_apply(s, text, err, cap, 0);
}

SMP_API int smp_synth_script_cfg(SmpSynth *s, const char *text, char *err, int cap) {
  return script_apply(s, text, err, cap, 1);
}

/* ---- the test render ---- */
typedef struct { long frame; int kind, len; uint8_t b[32]; } TEv;

static void wav_header(FILE *f, uint32_t frames, double rate, int ch) {
  uint32_t data = frames * 4 * (uint32_t)ch, riff = 36 + data, sr = (uint32_t)(rate + 0.5), br = sr * 4 * (uint32_t)ch, fmt = 16;
  uint16_t tag = 3, nch = (uint16_t)ch, align = (uint16_t)(4 * ch), bits = 32;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
  fwrite(&tag, 2, 1, f); fwrite(&nch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
  fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

SMP_API int smp_synth_render_test(SmpSynth *s, const char *events, long frames, int block, const char *wav_out,
                                  int all_outputs) {
  TEv *ev = NULL; int nev = 0, cap = 0, k = 0, nch = all_outputs ? 2 * NOUT : 2, i;
  const char *p = events; float *buf = NULL, *il = NULL; float *outs[2 * NOUT];
  FILE *f; wchar_t w[2048]; long b; LARGE_INTEGER freq, t0;
  if (!s || !wav_out || frames < 0 || block <= 0) return 1;
  while (p && *p) {                            /* "midi <byte> ... <frame>" and "reset <frame>" */
    TEv t; char word[16]; int used = 0; const char *nl = strchr(p, '\n');
    const char *lend = nl ? nl : p + strlen(p);
    memset(&t, 0, sizeof t);
    if (sscanf(p, "%15s%n", word, &used) == 1 && p + used <= lend) {
      long nums[40]; int nn = 0; const char *q = p + used;
      while (q < lend && nn < 40) {
        char *e; long x;
        while (q < lend && isspace((unsigned char)*q)) q++;
        if (q >= lend) break;
        x = strtol(q, &e, 10);
        if (e == q) { free(ev); return 2; }
        nums[nn++] = x; q = e;
      }
      if (!strcmp(word, "midi") && nn >= 2 && nn - 1 <= 32) {
        t.kind = EV_MSG; t.len = nn - 1; t.frame = nums[nn - 1];
        for (i = 0; i < t.len; i++) t.b[i] = (uint8_t)nums[i];
      } else if (!strcmp(word, "reset") && nn == 1) {
        t.kind = EV_RESET; t.frame = nums[0];
      } else { free(ev); return 2; }
      if (nev == cap) { TEv *n2; cap = cap ? cap * 2 : 64; n2 = (TEv *)realloc(ev, sizeof *ev * (size_t)cap); if (!n2) { free(ev); return 3; } ev = n2; }
      ev[nev++] = t;
    }
    p = nl ? nl + 1 : NULL;
  }
  for (i = 1; i < nev; i++) {                  /* stable sort by frame */
    TEv x = ev[i]; int j;
    for (j = i - 1; j >= 0 && ev[j].frame > x.frame; j--) ev[j + 1] = ev[j];
    ev[j + 1] = x;
  }
  if (!MultiByteToWideChar(CP_UTF8, 0, wav_out, -1, w, 2048) || !(f = _wfopen(w, L"wb"))) { free(ev); return 4; }
  buf = (float *)malloc(sizeof(float) * (size_t)block * 2 * NOUT);
  il = (float *)malloc(sizeof(float) * (size_t)block * (size_t)nch);
  if (!buf || !il) { free(buf); free(il); free(ev); fclose(f); return 3; }
  for (i = 0; i < 2 * NOUT; i++) outs[i] = buf + (size_t)i * (size_t)block;
  wav_header(f, (uint32_t)frames, s->rate, nch);
  /* every render starts from silence, with every channel at power-on */
  block_start(s, 0);
  s->dormant = 0;
  for (i = 0; i < MAXV; i++) { if (s->sl[i].st == V_LIVE) smp_sfv_kill(&s->v[i], 0); s->sl[i].st = V_FREE; }
  for (i = 0; i < NCH; i++) chan_defaults(&s->ch[i]);
  seq_clear(s, -1);
  resolve_all(s);
  s->nev = 0; s->peak = 0;
  smp_sffx_clear(s->fx);
  s->offline = s->test_wait;
  QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
  for (b = 0; b < frames; b += block) {
    int n = (int)(frames - b < block ? frames - b : block), j;
    if (s->offline && s->font) smp_sf_wait_idle(s->font, 10000);
    for (; k < nev && ev[k].frame < b + n; k++) {
      int at = (int)(ev[k].frame - b);
      if (at < 0) at = 0;
      if (ev[k].kind == EV_RESET) push(s, EV_RESET, NULL, at);   /* smp_synth_reset, at its frame */
      else smp_synth_midi(s, ev[k].b, ev[k].len, at);
    }
    smp_synth_process(s, outs, n);
    for (j = 0; j < n; j++) for (i = 0; i < nch; i++) il[(size_t)j * (size_t)nch + (size_t)i] = outs[i][j];
    fwrite(il, sizeof(float) * (size_t)nch, (size_t)n, f);
    if (!s->offline) {                          /* real time: never faster than the audio would play */
      LARGE_INTEGER now; double due = (double)(b + n) / s->rate;
      for (;;) {
        QueryPerformanceCounter(&now);
        if ((double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart >= due) break;
        Sleep(1);
      }
    }
  }
  s->offline = 0;
  free(buf); free(il); free(ev);
  return fclose(f) ? 5 : 0;
}
