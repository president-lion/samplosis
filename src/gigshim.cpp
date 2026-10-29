/* gigshim.cpp: GigaStudio (.gig) files through libgig 4.6.0 (src\codecs\libgig, GPL 2 or later), behind a C API.
   libgig is C++ and reports damage by throwing; every entry point here catches every exception (libgig's own,
   std::exception and anything else) and returns a plain reason, so no exception ever reaches the C code.

   sf_parse_gig reads a .gig into the SoundFont model, the same one sf2.c, sfdls.c and sfsfz.c fill:
   - Instruments become presets: the bank is 128 for a drum instrument, else the bank select MSB, or its LSB when
     the MSB is 0 (as the DLS loader reads them); the program is the instrument's; the name is its INAM.
   - Samples: one per channel of each wave (a third channel or more is left out). Uncompressed 16-bit and 24-bit
     waves are PCM_FILE at their data chunk's offset, in the .gig or in its extension file (.gx01...), which gets its
     own entry in the path table. 8-bit waves are PCM_FILE as unsigned bytes. Compressed waves (an ewav chunk) are
     DECODED: libgig decodes them once, on the loader thread, into the cache (sf_gig_decode). Waves in any other
     format are left out, counted as "wave format".
   - Each region's dimension regions become regions with conditions (SmpSfCond):
     - velocity: the velocity range, worked out through libgig's own lookup, so gig2 upper limits, gig3 custom
       zones and even splits all come out as libgig plays them;
     - layer: every layer is a region of its own (they all sound);
     - samplechannel: a stereo wave in zone 0 plays as a pair (two regions panned hard left and right, the second
       with cond.pair); mono waves in its zones play as a left and right pair; other zones of a stereo wave are
       left out (LinuxSampler plays the same);
     - releasetrigger: zone 1 is trigger=release, with GigaStudio's release trigger decay as rt_linear (a straight
       fall of 0.01053 x (256 >> decay) of full level per second held, as LinuxSampler plays it);
     - roundrobin and roundrobinkeyboard: seq_len = zones, seq_pos = zone + 1 (the counter is per region set, where
       LinuxSampler keeps one per key or one per channel; documented in the report);
     - random: lorand and hirand split 0..1 into the zones;
     - keyboard (keyswitches): the instrument's dimension key range is sw_lokey..sw_hikey; each key of a zone gets
       a copy of its regions with that key as sw_last, and the range's lowest key is sw_default (zone 0 plays until
       a keyswitch is pressed, as LinuxSampler starts);
     - the controller dimensions (mod wheel, breath, foot, portamento time, effect 1 and 2, general purpose 1 to 8,
       the pedals, effect depths 1 to 5): a CC range (locc), at most 4 a region;
     - anything else (channel aftertouch, smart MIDI, a fifth CC dimension, unknown kinds) is counted as ignored and
       plays the zone its value 0 selects.
   - Per dimension region: EG1 to the volume envelope (pre-attack and the second decay approximated, see eg_vol),
     EG2 to the modulation envelope, which sweeps the filter up from 21 Hz to its cutoff (EG2 scales the cutoff in
     LinuxSampler); LFO1 (amplitude) or else LFO2 (filter) to the modulation LFO; LFO3 to the vibrato LFO; their
     mod wheel, breath, foot and aftertouch controls as modulators; the filter's type, cutoff, resonance and key
     tracking; unity note, fine tune (with the instrument's) and pitch tracking; the sample start offset; the
     first loop (gig loops are forward loops that go on through the release: sampleModes 1); pan; the sample
     attenuation (a boost goes to cond.gain); the velocity response curve as cond.amp_velcurve (amp_veltrack 100);
     a velocity crossfade as xfin/xfout; the key group as exclusiveClass; the instrument's pitch bend range as
     the pitch wheel's modulator.
   - What doesn't map is counted, once per kind, in sfz_ignored (smp_sf_ignored): see note_ignored's callers.

   Threads. libgig keeps some state in statics (instance counts and a shared decompression buffer), so every open,
   parse and close of a libgig file runs under one lock (g_lock). A decode reads its own file's sample with its own
   decompression buffer and touches no static, so its reads don't take the lock. A font closed while the lock is
   held hands its file to the holder, which frees it before letting go (sf_gig_free never waits).

   Damage. libgig throws for most damage; the crashes, unbounded writes and file-sized allocations the damage tests
   and review found in it are fixed in the vendored copy (VERSION.txt lists them). As a last resort, each entry
   point also runs inside a structured-exception guard (seh_filter). A crash it catches leaks that call's libgig
   objects, and from then on every GigaStudio open and decode in the process is refused with a plain reason (save
   the project and restart), since the heap can't be trusted after it.

   Paths are UTF-8 throughout: libgig's RIFF.cpp is patched to open them with CreateFileW. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exception>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "codecs/libgig/gig.h"
extern "C" {
#include "sf2.h"
}

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;   /* taken and let go explicitly, so a guard that catches a crash can let go of it */
volatile LONG g_guards;          /* crashes the guards caught (tests: smp_sf_test_gig_guards) */
volatile LONG g_crashed;         /* a guard caught a crash: every later open and decode is refused (see seh_filter) */
volatile LONG g_fault;           /* tests: 1 crashes the next parse, 2 the next decode open, inside the guard */
const char *const CRASHED = "A GigaStudio file crashed the reader; save your project under a new name and restart REAPER";

/* a parse or decode stopped by its cancel hook (thrown from libgig's progress callback, or between our steps) */
struct Cancelled {};

/* a wave's data chunk, which libgig keeps protected: read through a pointer to member named by a derived class,
   which the language allows (the class is never made) */
struct SampleData : gig::Sample {
  static RIFF::Chunk *of(gig::Sample *s) { return s->*(&SampleData::pCkData); }
};

/* SF2 generator numbers used here */
enum {
  G_START = 0, G_MODLFO_PITCH = 5, G_VIBLFO_PITCH = 6, G_FC = 8, G_Q = 9, G_MODLFO_FC = 10, G_MODENV_FC = 11,
  G_MODLFO_VOL = 13, G_PAN = 17, G_FREQ_MODLFO = 22, G_FREQ_VIBLFO = 24, G_ATTACK_MODENV = 26, G_DECAY_MODENV = 28,
  G_SUSTAIN_MODENV = 29, G_RELEASE_MODENV = 30, G_ATTACK_VOL = 34, G_HOLD_VOL = 35, G_DECAY_VOL = 36,
  G_SUSTAIN_VOL = 37, G_RELEASE_VOL = 38, G_KEYRANGE = 43, G_VELRANGE = 44, G_LOOPSTART = 2, G_LOOPEND = 3,
  G_LOOPSTART_COARSE = 45, G_LOOPEND_COARSE = 50, G_ATTEN = 48, G_COARSE = 51, G_FINE = 52, G_SAMPLEID = 53,
  G_MODES = 54, G_SCALE = 56, G_EXCLUSIVE = 57, G_ROOTKEY = 58
};

#define MAX_SAMPLES 65535                 /* sample (channel) cap, as the DLS loader's */
#define DECODE_CHUNK 65536                /* frames a decode reads between cancel checks */
#define MAX_DECODE_FRAMES (1u << 26)      /* sffont.c's MAX_DECODED_FRAMES: at most 2 channels of this as float is
                                             512 MiB, well under decode's 2 GiB ceiling */

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
int iround(double v) { return (int)floor(v + 0.5); }

void say(char *why, int wcap, const char *text) {
  if (!why || wcap <= 0) return;
  snprintf(why, (size_t)wcap, "%s", text);
  for (char *p = why; *p; p++) if (*p == '\r' || *p == '\n' || *p == '\t') *p = ' ';
}

/* seconds to timecents; 1 ms or less is "no time" (-12000) */
int tc(double s) { return s > 0.001 ? iround(1200 * log2(s)) : -12000; }
/* Hz to SF2 absolute cents */
int abscents(double hz) { return hz > 0 ? iround(1200 * log2(hz / 8.176)) : -16000; }

/* An open libgig file: the RIFF file and the gig file over it (deleted in that order's reverse). It is always made
   with new and deleted only on a normal return or after a C++ exception, never after a guard caught a crash, when
   its tree may be half-built: it is then left (leaked). libgig opens the UTF-8 path itself (its RIFF.cpp is
   patched to CreateFileW), and finds extension files by the same UTF-8 folder. */
struct Gig {
  RIFF::File *riff = nullptr;
  gig::File *file = nullptr;
  ~Gig() { delete file; delete riff; }
};

/* the progress callback: asks the cancel hook, and throws to stop libgig where it is */
struct CancelHook { int (*cancel)(void *); void *arg; };
void on_progress(RIFF::progress_t *p) {
  CancelHook *h = p ? (CancelHook *)p->custom : nullptr;
  if (h && h->cancel && h->cancel(h->arg)) throw Cancelled();
}

/* the names of what doesn't map, counted once each */
struct Ignored {
  std::set<std::string> names;
  std::string first;
  void add(const char *name) {
    if (names.insert(name).second && first.empty()) first = name;
  }
};

/* ---- building the regions ---- */

/* what a dimension means here */
enum DimKind { DK_VALUE0, DK_VEL, DK_LAYER, DK_CHANNEL, DK_RELEASE, DK_RR, DK_RANDOM, DK_KEYSW, DK_CC };

DimKind dim_kind(gig::dimension_t d) {
  switch (d) {
    case gig::dimension_velocity: return DK_VEL;
    case gig::dimension_layer: return DK_LAYER;
    case gig::dimension_samplechannel: return DK_CHANNEL;
    case gig::dimension_releasetrigger: return DK_RELEASE;
    case gig::dimension_roundrobin: case gig::dimension_roundrobinkeyboard: return DK_RR;
    case gig::dimension_random: return DK_RANDOM;
    case gig::dimension_keyboard: return DK_KEYSW;
    case gig::dimension_modwheel: case gig::dimension_breath: case gig::dimension_foot:
    case gig::dimension_portamentotime: case gig::dimension_effect1: case gig::dimension_effect2:
    case gig::dimension_genpurpose1: case gig::dimension_genpurpose2: case gig::dimension_genpurpose3:
    case gig::dimension_genpurpose4: case gig::dimension_sustainpedal: case gig::dimension_portamento:
    case gig::dimension_sostenutopedal: case gig::dimension_softpedal: case gig::dimension_genpurpose5:
    case gig::dimension_genpurpose6: case gig::dimension_genpurpose7: case gig::dimension_genpurpose8:
    case gig::dimension_effect1depth: case gig::dimension_effect2depth: case gig::dimension_effect3depth:
    case gig::dimension_effect4depth: case gig::dimension_effect5depth:
      return DK_CC;   /* these enum values are the controller numbers */
    default: return DK_VALUE0;
  }
}

const char *dim_name(gig::dimension_t d) {
  switch (d) {
    case gig::dimension_channelaftertouch: return "channel aftertouch dimension";
    case gig::dimension_smartmidi: return "smart MIDI dimension";
    default: return "unknown dimension";
  }
}

/* One region's dimensions, worked out once. */
struct Dims {
  int n = 0;
  gig::dimension_def_t *def[8];
  int bitpos[8], kind[8];
  int vel = -1;                 /* the velocity dimension, or -1 */
  int chan = -1;                /* the samplechannel dimension, or -1 */
  int zone0[8];                 /* DK_VALUE0: the zone value 0 selects */
};

/* the zone a value selects in a normal (non-velocity) dimension, as libgig's GetDimensionRegionIndexByValue
   works it out (a value above every gig3 upper limit is taken as the last zone, where libgig's index would wrap) */
int zone_of(gig::Region *rg, const Dims &D, int d, int v) {
  const gig::dimension_def_t *dd = D.def[d];
  int zones = dd->zones > 0 ? dd->zones : 1, z;
  if (dd->split_type == gig::split_type_bit) return clampi(v, 0, zones - 1);
  if (rg->pDimensionRegions[0] && rg->pDimensionRegions[0]->DimensionUpperLimits[d]) {
    for (z = 0; z < zones; z++) {
      int at = z << D.bitpos[d];
      gig::DimensionRegion *r = at < 256 ? rg->pDimensionRegions[at] : nullptr;
      if (r && v <= r->DimensionUpperLimits[d]) break;
    }
    return z < zones ? z : zones - 1;
  }
  z = dd->zone_size > 0 ? (int)(v / dd->zone_size) : 0;
  return clampi(z, 0, zones - 1);
}

/* the zone of dimension d in dimension region index i */
int zone_in(const Dims &D, int d, int i) { return (i >> D.bitpos[d]) & ((1 << D.def[d]->bits) - 1); }

/* the lowest and highest values of dimension d that select zone z (normal dimensions); 0 when none do */
int zone_range(gig::Region *rg, const Dims &D, int d, int z, int *lo, int *hi) {
  int v, got = 0;
  for (v = 0; v < 128; v++)
    if (zone_of(rg, D, d, v) == z) { if (!got) *lo = v; *hi = v; got = 1; }
  return got;
}

/* the velocity range dimension region i plays: every velocity whose lookup (with the other dimensions at the
   values that select i's zones) gives i. 0 when none does. */
int vel_range(gig::Region *rg, const Dims &D, int i, int *lo, int *hi) {
  uint vals[8] = {0};
  int d, v, got = 0;
  for (d = 0; d < D.n; d++) {
    int z = zone_in(D, d, i), a, b;
    if (d == D.vel) continue;
    if (D.def[d]->split_type == gig::split_type_bit) vals[d] = (uint)z;
    else if (zone_range(rg, D, d, z, &a, &b)) vals[d] = (uint)a;
    else return 0;
  }
  for (v = 0; v < 128; v++) {
    vals[D.vel] = (uint)v;
    if (rg->GetDimensionRegionIndexByValue(vals) == i) { if (!got) *lo = v; *hi = v; got = 1; }
  }
  return got;
}

/* the model being built */
struct Out {
  std::vector<SmpSfRegion> regs;
  std::vector<int> moff, nmod, curve;           /* per region: its modulator list, its velocity curve (or -1) */
  std::vector<SmpSfMod> mods;
  std::map<std::string, int> mod_lists;         /* identical lists shared */
  std::vector<float> curves;                    /* 128 a curve */
  std::map<std::string, int> curve_ids;
  Ignored ign;
  int boosts = 0;
};

int add_mods(Out &o, const SmpSfMod *m, int n) {
  std::string key((const char *)m, (size_t)n * sizeof *m);
  auto it = o.mod_lists.find(key);
  if (it != o.mod_lists.end()) return it->second;
  if (o.mods.size() + (size_t)n > SF_MAX_POOL_MODS) throw std::length_error("it has too many modulators");
  int at = (int)o.mods.size();
  o.mods.insert(o.mods.end(), m, m + n);
  o.mod_lists[key] = at;
  return at;
}

int add_curve(Out &o, const float *c) {
  std::string key((const char *)c, 128 * sizeof(float));
  auto it = o.curve_ids.find(key);
  if (it != o.curve_ids.end()) return it->second;
  int id = (int)(o.curves.size() / 128);
  o.curves.insert(o.curves.end(), c, c + 128);
  o.curve_ids[key] = id;
  return id;
}

/* a controller of an LFO as an SF2 modulator source (0: none): the mod wheel, breath (CC2), foot (CC4) or channel
   pressure, linear and unipolar */
uint16_t lfo_source(int which, int ctrl) {
  /* the enums: 1 external only, 3 internal and external (the external one being the second listed in each enum),
     2 and 4 the third. LFO1: wheel, breath; LFO2: wheel, foot; LFO3: wheel, aftertouch. */
  int ext = ctrl == 1 || ctrl == 3 ? 1 : ctrl == 2 || ctrl == 4 ? 2 : 0;
  if (!ext) return 0;
  if (ext == 1) return 0x0081;                                     /* CC1 */
  return which == 1 ? 0x0082 : which == 2 ? 0x0084 : 0x000D;       /* CC2, CC4, channel pressure */
}
bool lfo_internal(int ctrl) { return ctrl == 0 || ctrl == 3 || ctrl == 4; }

/* the amplitude LFO's depth (0..1200 for none to full) as modLfoToVolume cB: LinuxSampler scales the level by
   1 - depth x lfo, so the swing from top to bottom is -20 log10(1 - depth) dB; SF2 swings the generator both ways */
int lfo1_cb(double depth) {
  double d = clampd(depth / 1200.0, 0, 0.999);
  return clampi(iround(-200 * log10(1 - d) / 2), 0, 960);
}
/* the filter LFO's depth as modLfoToFilterFc cents: the cutoff is scaled by 1 - depth x lfo on LinuxSampler's
   0..127 scale, which is about the 4th power in Hz */
int lfo2_cents(double depth) {
  double d = clampd(depth / 1200.0, 0, 0.999);
  return clampi(iround(4 * 1200 * log2(1 / (1 - d)) / 2), 0, 12000);
}

/* the filter cutoff on LinuxSampler's 0..127 scale, in Hz, as it maps its own filter types: 21 Hz to 18 kHz */
double cutoff_hz(double v) {
  double x = (clampd(v, 0, 127) + 29.0) / (127.0 + 29.0);
  return x * x * x * x * 18000.0;
}

/* per region of the file (a key range), what every dimension region shares */
struct RegionCtx {
  gig::Instrument *ins;
  gig::Region *rg;
  int lokey, hikey;
  int sw_lo, sw_hi;             /* the instrument's keyswitch range, or -1 */
};

/* the generators, modulators and conditions of one dimension region, before the dimensions' own conditions */
struct Built {
  int v[SMP_SF_NGEN];
  SmpSfCond cond;
  SmpSfMod mods[32]; int nmods;
  int curve;
};

void build_dimreg(Out &o, const RegionCtx &c, gig::DimensionRegion *dr, const SmpSfSample *sh, Built &b) {
  int16_t def[SMP_SF_NGEN];
  int i;
  sf_gen_defaults(def);
  for (i = 0; i < SMP_SF_NGEN; i++) b.v[i] = def[i];
  memset(&b.cond, 0, sizeof b.cond);
  sf_cond_defaults(&b.cond);
  b.nmods = 0;

  /* modulators: SF2's CC7, CC10, CC11, CC91 and CC93 ones; the pitch wheel by the instrument's bend range
     (SF2's velocity ones are replaced by the velocity curve, its vibrato ones by LFO3's own controls) */
  static const SmpSfMod base[5] = {
    {0x0587, 48, 960, 0, 0}, {0x028A, 17, 500, 0, 0}, {0x058B, 48, 960, 0, 0}, {0x00DB, 16, 200, 0, 0},
    {0x00DD, 15, 200, 0, 0}};
  for (i = 0; i < 5; i++) b.mods[b.nmods++] = base[i];
  {
    SmpSfMod w = {0x020E, 59, (int16_t)clampi(c.ins->PitchbendRange * 100, 0, 12700), 0, 0};
    b.mods[b.nmods++] = w;
  }

  /* pitch: the dimension region's unity note and fine tune (DLS's wsmp), the instrument's fine tune */
  {
    int tune = (int)dr->FineTune + (int)c.ins->FineTune;
    tune = clampi(tune, -12000, 12000);
    b.v[G_COARSE] = tune / 100;
    b.v[G_FINE] = tune - 100 * (tune / 100);
    b.v[G_ROOTKEY] = clampi(dr->UnityNote, 0, 127);
    if (!dr->PitchTrack) b.v[G_SCALE] = 0;
  }

  /* level: the sample attenuation (a linear factor from the wave's gain); a boost goes to cond.gain */
  {
    double sa = dr->SampleAttenuation;
    if (!(sa > 0)) sa = 1;
    double db = -20 * log10(sa);
    if (db >= 0) b.v[G_ATTEN] = clampi(iround(db * 10 / 0.4), 0, 32767);
    else { b.cond.gain = (float)clampd(sa, 1, 16); o.boosts++; }
  }
  /* the velocity response curve: LinuxSampler's level at each velocity, through cond.amp_velcurve */
  {
    float cv[128];
    for (i = 0; i < 128; i++) cv[i] = (float)clampd(dr->GetVelocityAttenuation((uint8_t)i), 0, 1);
    b.cond.amp_veltrack = 100;
    b.curve = add_curve(o, cv);
  }
  b.v[G_PAN] = clampi(iround(dr->Pan * 500.0 / 64.0), -500, 500);
  b.v[G_EXCLUSIVE] = clampi(c.rg->KeyGroup, 0, 127);
  b.v[G_START] = clampi(dr->SampleStartOffset, 0, 32767);

  /* the attenuation controller: a velocity crossfade maps; one on a CC or aftertouch can't */
  if (dr->AttenuationController.type == gig::attenuation_ctrl_t::type_velocity && dr->Crossfade.out_end) {
    b.cond.xfin_lovel = dr->Crossfade.in_start; b.cond.xfin_hivel = dr->Crossfade.in_end;
    b.cond.xfout_lovel = dr->Crossfade.out_start; b.cond.xfout_hivel = dr->Crossfade.out_end;
    b.cond.xf_curve = 1;
  } else if (dr->AttenuationController.type != gig::attenuation_ctrl_t::type_none) {
    o.ign.add(dr->AttenuationController.type == gig::attenuation_ctrl_t::type_velocity ? "velocity attenuation"
                                                                                      : "controller crossfade");
  }

  /* EG1, the volume envelope. Its decay 1 falls from the top to the sustain level in Decay1 seconds; the voice's
     decay falls 96 dB in its time, dB-linear (as LinuxSampler's exponential decay is), so the time is scaled by
     96 / the sustain's dB. Without infinite sustain, decay 2 then falls to nothing: approximated as one fall to
     silence over both decay times. EG1Hold (decay waits for the loop start) becomes a hold until the loop start at
     the sample's own rate. */
  {
    double sus = clampd(dr->EG1Sustain / 1000.0, 0, 1);
    int sus_cb = sus <= 0 ? 1440 : clampi(iround(-200 * log10(sus)), 0, 1440);
    b.v[G_ATTACK_VOL] = tc(dr->EG1Attack);
    if (dr->EG1InfiniteSustain) {
      double sdb = sus_cb / 10.0;
      b.v[G_SUSTAIN_VOL] = sus_cb;
      b.v[G_DECAY_VOL] = sdb >= 0.5 ? tc(dr->EG1Decay1 * 96.0 / sdb) : tc(dr->EG1Decay1);
    } else {
      b.v[G_SUSTAIN_VOL] = 1440;
      b.v[G_DECAY_VOL] = tc(dr->EG1Decay1 + dr->EG1Decay2);
    }
    b.v[G_RELEASE_VOL] = tc(dr->EG1Release);
    if (dr->EG1Hold && dr->SampleLoops && dr->pSampleLoops && sh->rate) {
      double h = (double)dr->pSampleLoops[0].LoopStart / sh->rate - dr->EG1Attack;
      if (h > 0.001) b.v[G_HOLD_VOL] = tc(h);
    }
    if (dr->EG1PreAttack) o.ign.add("EG1 pre-attack");
    if (dr->EG1Controller.type != gig::eg1_ctrl_t::type_none &&
        (dr->EG1ControllerAttackInfluence || dr->EG1ControllerDecayInfluence || dr->EG1ControllerReleaseInfluence))
      o.ign.add("EG1 controller");
  }
  /* EG2, the modulation envelope (it only drives the filter here). SF2's decay falls linearly from the top to the
     sustain level in decay x (1 - sustain). */
  {
    double sus = clampd(dr->EG2Sustain / 1000.0, 0, 1);
    b.v[G_ATTACK_MODENV] = tc(dr->EG2Attack);
    if (dr->EG2InfiniteSustain) {
      b.v[G_SUSTAIN_MODENV] = iround(1000 - sus * 1000);
      b.v[G_DECAY_MODENV] = sus < 0.999 ? tc(dr->EG2Decay1 / (1 - sus)) : tc(dr->EG2Decay1);
    } else {
      b.v[G_SUSTAIN_MODENV] = 1000;
      b.v[G_DECAY_MODENV] = tc(dr->EG2Decay1 + dr->EG2Decay2);
    }
    b.v[G_RELEASE_MODENV] = tc(dr->EG2Release);
    if (dr->VCFEnabled && dr->EG2PreAttack) o.ign.add("EG2 pre-attack");
    if (dr->VCFEnabled && dr->EG2Controller.type != gig::eg2_ctrl_t::type_none &&
        (dr->EG2ControllerAttackInfluence || dr->EG2ControllerDecayInfluence || dr->EG2ControllerReleaseInfluence))
      o.ign.add("EG2 controller");
  }

  /* the filter: LinuxSampler multiplies the cutoff by EG2's level, so EG2 sweeps it from 21 Hz up to the cutoff */
  if (dr->VCFEnabled) {
    int top = abscents(cutoff_hz(dr->VCFCutoff)), low = abscents(cutoff_hz(0));
    uint8_t t = (uint8_t)dr->VCFType;
    b.v[G_FC] = low;
    b.v[G_MODENV_FC] = clampi(top - low, 0, 12000);
    b.v[G_Q] = clampi(iround(dr->VCFResonance / 127.0 * 200), 0, 960);
    b.cond.fil_unity = 1;
    if (t == 0x02 || (t >= 0x21 && t <= 0x26)) b.cond.fil_type = SMP_FIL_HP;
    else if (t == 0x01 || t == 0x32) b.cond.fil_type = SMP_FIL_BP;
    else if (t == 0x03 || t == 0x42) b.cond.fil_type = SMP_FIL_BR;
    else b.cond.fil_type = SMP_FIL_LP;
    if (dr->VCFKeyboardTracking) {
      /* the cutoff follows the key at 100 cents a key from the breakpoint: key (linear, 0..127) to cutoff */
      SmpSfMod k = {0x0003, 8, 12800, 0, 0};
      b.mods[b.nmods++] = k;
      b.v[G_FC] -= 100 * dr->VCFKeyboardTrackingBreakpoint;
    }
    if (dr->VCFCutoffController != gig::vcf_cutoff_ctrl_none && dr->VCFCutoffController != gig::vcf_cutoff_ctrl_none2)
      o.ign.add("filter cutoff controller");
    else if (dr->VCFVelocityScale)
      o.ign.add("velocity to filter cutoff");
    if (dr->VCFResonanceController != gig::vcf_res_ctrl_none) o.ign.add("filter resonance controller");
  }

  /* LFOs: SF2 has one modulation LFO, so LFO1 (amplitude) takes it when it is used, else LFO2 (filter) */
  {
    bool l1 = (lfo_internal(dr->LFO1Controller) && dr->LFO1InternalDepth) ||
              (lfo_source(1, dr->LFO1Controller) && dr->LFO1ControlDepth);
    bool l2 = dr->VCFEnabled && ((lfo_internal(dr->LFO2Controller) && dr->LFO2InternalDepth) ||
                                 (lfo_source(2, dr->LFO2Controller) && dr->LFO2ControlDepth));
    if (l1) {
      b.v[G_FREQ_MODLFO] = abscents(dr->LFO1Frequency);
      if (lfo_internal(dr->LFO1Controller)) b.v[G_MODLFO_VOL] = lfo1_cb(dr->LFO1InternalDepth);
      if (uint16_t s = lfo_source(1, dr->LFO1Controller)) {
        SmpSfMod m = {s, G_MODLFO_VOL, (int16_t)lfo1_cb(dr->LFO1ControlDepth), 0, 0};
        if (dr->LFO1ControlDepth) b.mods[b.nmods++] = m;
      }
      if (l2 && fabs(dr->LFO2Frequency - dr->LFO1Frequency) > 0.01) o.ign.add("filter LFO beside the amplitude LFO");
    }
    if (l2 && (!l1 || fabs(dr->LFO2Frequency - dr->LFO1Frequency) <= 0.01)) {
      b.v[G_FREQ_MODLFO] = abscents(dr->LFO2Frequency);
      if (lfo_internal(dr->LFO2Controller)) b.v[G_MODLFO_FC] = -lfo2_cents(dr->LFO2InternalDepth);
      if (uint16_t s = lfo_source(2, dr->LFO2Controller)) {
        SmpSfMod m = {s, G_MODLFO_FC, (int16_t)-lfo2_cents(dr->LFO2ControlDepth), 0, 0};
        if (dr->LFO2ControlDepth) b.mods[b.nmods++] = m;
      }
    }
    /* LFO3, pitch: the vibrato LFO, its depth in cents */
    b.v[G_FREQ_VIBLFO] = abscents(dr->LFO3Frequency);
    if (lfo_internal(dr->LFO3Controller)) b.v[G_VIBLFO_PITCH] = clampi(dr->LFO3InternalDepth, -1200, 1200);
    if (uint16_t s = lfo_source(3, dr->LFO3Controller)) {
      SmpSfMod m = {s, G_VIBLFO_PITCH, (int16_t)clampi(dr->LFO3ControlDepth, -1200, 1200), 0, 0};
      if (dr->LFO3ControlDepth) b.mods[b.nmods++] = m;
    }
  }

  if (dr->EG3Depth) o.ign.add("EG3 (pitch envelope)");
  if (dr->DimensionBypass != gig::dim_bypass_ctrl_none) o.ign.add("dimension bypass");
  if (dr->SustainDefeat) o.ign.add("sustain defeat");
  if (dr->MSDecode) o.ign.add("mid/side decode");

  /* the loop: the dimension region's first (the wave's own smpl loop is the sample header's); a different one goes
     through the loop offset generators */
  if (dr->SampleLoops && dr->pSampleLoops) {
    const DLS::sample_loop_t &L = dr->pSampleLoops[0];
    int64_t ls = L.LoopStart, le = (int64_t)L.LoopStart + L.LoopLength;
    int64_t ds = ls - sh->loop_start, de = le - sh->loop_end;
    if (L.LoopType != gig::loop_type_normal) o.ign.add("ping-pong or backward loop");
    if (dr->SampleLoops > 1) o.ign.add("second loop");
    if (L.LoopLength > 0 && ds >= -0x7fffffffll && ds <= 0x7fffffffll && de >= -0x7fffffffll && de <= 0x7fffffffll) {
      b.v[G_MODES] = 1;
      b.v[G_LOOPSTART] = (int)(ds % 32768); b.v[G_LOOPSTART_COARSE] = clampi((int)(ds / 32768), -32768, 32767);
      b.v[G_LOOPEND] = (int)(de % 32768); b.v[G_LOOPEND_COARSE] = clampi((int)(de / 32768), -32768, 32767);
    }
  }
}

/* adds one region (one channel of a dimension region's wave) */
void emit(Out &o, const Built &b, const SmpSfCond &cond, int lokey, int hikey, int lovel, int hivel, int sample,
          int pan) {
  SmpSfRegion r;
  SmpSfMod list[64];
  int i, n;
  if (o.regs.size() >= SF_MAX_REGIONS) throw std::length_error("it has too many regions");
  memset(&r, 0, sizeof r);
  r.lokey = (uint8_t)lokey; r.hikey = (uint8_t)hikey; r.lovel = (uint8_t)lovel; r.hivel = (uint8_t)hivel;
  r.sample = sample;
  r.cond = cond;
  for (i = 0; i < SMP_SF_NGEN; i++) {
    int v = b.v[i];
    if (i == G_PAN) v += pan;
    r.gen[i] = (int16_t)sf_gen_clamp(i, v);
  }
  r.gen[G_KEYRANGE] = (int16_t)(lokey | hikey << 8);
  r.gen[G_VELRANGE] = (int16_t)(lovel | hivel << 8);
  r.gen[G_SAMPLEID] = (int16_t)(sample > 32767 ? 32767 : sample);
  n = sf_merge_mods(list, 0, b.mods, b.nmods);
  o.regs.push_back(r);
  o.moff.push_back(add_mods(o, list, n));
  o.nmod.push_back(n);
  o.curve.push_back(b.curve);
}

struct SampleInfo { int first = -1, nch = 0; bool pcm = false; };

/* one region of the file: every dimension region it can play */
void build_region(Out &o, const RegionCtx &c, std::map<gig::Sample *, int> &sidx, const std::vector<SampleInfo> &si,
                  const SmpFont *f) {
  gig::Region *rg = c.rg;
  Dims D;
  int d, bp = 0, i;
  int cc_used = 0;
  D.n = clampi((int)rg->Dimensions, 0, 8);
  for (d = 0; d < D.n; d++) {
    D.def[d] = &rg->pDimensionDefinitions[d];
    D.bitpos[d] = bp;
    bp += D.def[d]->bits;
    if (D.def[d]->bits > 8 || D.def[d]->zones > (1 << D.def[d]->bits)) bp = 99;
  }
  /* more than 8 bits, or more zones than a dimension's bits can number: damaged, and libgig's lookups would index
     past its 256 dimension regions, so the region is left out */
  if (bp > 8) { o.ign.add("damaged dimensions"); return; }
  /* libgig's GetDimensionRegionIndexByValue (used for the velocity ranges) reads dimension region 0 and, for each
     normal dimension with gig3 limits, the dimension region of each of its zones, without checking them: a region
     where any of those is missing is damaged, and left out before a lookup could touch it */
  for (d = 0; d < D.n; d++) {
    if (D.def[d]->dimension != gig::dimension_velocity) continue;
    if (!rg->pDimensionRegions[0]) { o.ign.add("damaged dimensions"); return; }
    for (int e = 0; e < D.n; e++) {
      if (e == d || D.def[e]->split_type != gig::split_type_normal || !rg->pDimensionRegions[0]->DimensionUpperLimits[e])
        continue;
      for (int z = 0; z < D.def[e]->zones; z++)
        if (!rg->pDimensionRegions[z << D.bitpos[e]]) { o.ign.add("damaged dimensions"); return; }
    }
    break;
  }
  for (d = 0; d < D.n; d++) {
    D.kind[d] = dim_kind(D.def[d]->dimension);
    D.zone0[d] = 0;
    if (D.kind[d] == DK_VEL) { if (D.vel < 0) D.vel = d; else D.kind[d] = DK_VALUE0; }
    if (D.kind[d] == DK_CHANNEL) { if (D.chan < 0) D.chan = d; else D.kind[d] = DK_VALUE0; }
    if (D.kind[d] == DK_KEYSW && (c.sw_lo < 0 || c.sw_hi < c.sw_lo)) {
      o.ign.add("keyswitch dimension without a key range");
      D.kind[d] = DK_VALUE0;
    }
    if (D.kind[d] == DK_CC && ++cc_used > 4) { o.ign.add("a fifth controller dimension"); D.kind[d] = DK_VALUE0; }
    else if (D.kind[d] == DK_VALUE0 && dim_kind(D.def[d]->dimension) == DK_VALUE0) o.ign.add(dim_name(D.def[d]->dimension));
    if (D.kind[d] == DK_VALUE0) D.zone0[d] = zone_of(rg, D, d, 0);
  }

  for (i = 0; i < 1 << bp; i++) {
    /* a samplechannel dimension's zones are taken together, from zone 0, so a left and right pair stay adjacent */
    if (D.chan >= 0 && zone_in(D, D.chan, i) != 0) continue;
    int zones = D.chan >= 0 ? clampi(D.def[D.chan]->zones, 1, 1 << D.def[D.chan]->bits) : 1, z;
    int prev_mono = 0;
    for (z = 0; z < zones; z++) {
      int at = D.chan >= 0 ? i | z << D.bitpos[D.chan] : i;
      gig::DimensionRegion *dr = at < 256 ? rg->pDimensionRegions[at] : nullptr;
      if (!dr || !dr->pSample) { prev_mono = 0; continue; }
      auto it = sidx.find(dr->pSample);
      if (it == sidx.end() || si[it->second].first < 0) { prev_mono = 0; continue; }
      const SampleInfo &s = si[it->second];
      if (z > 0 && s.nch > 1) continue;                     /* a stereo wave plays from zone 0 as a pair */
      /* conditions from the dimensions; skip a dimension region no note can reach */
      SmpSfCond cond;
      int lovel = 0, hivel = 127, ok = 1, keysw = -1;
      Built b;
      build_dimreg(o, c, dr, &f->shdr[s.first], b);
      cond = b.cond;
      for (d = 0; d < D.n && ok; d++) {
        int zz = zone_in(D, d, at), zn = clampi(D.def[d]->zones, 1, 256), lo, hi;
        switch (D.kind[d]) {
          case DK_VEL: ok = vel_range(rg, D, at, &lovel, &hivel); break;
          case DK_LAYER: case DK_CHANNEL: ok = zz < zn; break;
          case DK_RELEASE:
            if (zz == 1) {
              cond.trigger = SMP_TRIG_RELEASE;
              cond.rt_linear = (float)(0.01053 * (256 >> clampi(dr->ReleaseTriggerDecay, 0, 8)));
            } else ok = zz == 0;
            break;
          case DK_RR: ok = zz < zn; cond.seq_len = (uint16_t)zn; cond.seq_pos = (uint16_t)(zz + 1); break;
          case DK_RANDOM: ok = zz < zn; cond.lorand = (float)zz / zn; cond.hirand = zz + 1 >= zn ? 1.0f : (float)(zz + 1) / zn; break;
          case DK_KEYSW: ok = zz < zn; keysw = d; break;
          case DK_CC:
            ok = zone_range(rg, D, d, zz, &lo, &hi);
            if (ok && cond.locc_n < 4) {
              cond.locc[cond.locc_n].cc = (uint8_t)D.def[d]->dimension;
              cond.locc[cond.locc_n].lo = (uint8_t)lo; cond.locc[cond.locc_n].hi = (uint8_t)hi;
              cond.locc_n++;
            }
            break;
          default: ok = zz == D.zone0[d]; break;
        }
      }
      if (!ok) { prev_mono = 0; continue; }
      if (dr->NoNoteOffReleaseTrigger) o.ign.add("no note-off release trigger");
      /* the channels: a stereo wave is a pair; mono waves in a samplechannel dimension's zones 0 and 1 are one */
      int pans[2] = {0, 0}, nch = s.nch > 1 ? 2 : 1;
      SmpSfCond c2 = cond;
      if (nch == 2) { pans[0] = -500; pans[1] = 500; }
      else if (D.chan >= 0 && zones >= 2 && z <= 1) pans[0] = z == 0 ? -500 : 500;
      /* (a follower must come right after its leader, which keyswitch copies would break) */
      if (nch == 1 && D.chan >= 0 && z == 1 && prev_mono && keysw < 0) c2.pair = 1;
      int k0 = keysw >= 0 ? c.sw_lo : 0, k1 = keysw >= 0 ? c.sw_hi : 0, k;
      for (k = k0; k <= k1; k++) {
        if (keysw >= 0) {
          int n = c.sw_hi - c.sw_lo + 1, zn = clampi(D.def[keysw]->zones, 1, 256);
          if ((k - c.sw_lo) * zn / n != zone_in(D, keysw, at)) continue;
          c2.sw_lokey = (int8_t)c.sw_lo; c2.sw_hikey = (int8_t)c.sw_hi; c2.sw_last = (int8_t)k;
          c2.sw_default = (int8_t)c.sw_lo;
        }
        if (nch == 2) {
          SmpSfCond right = c2;
          right.pair = 1;
          emit(o, b, c2, c.lokey, c.hikey, lovel, hivel, s.first, pans[0]);
          emit(o, b, right, c.lokey, c.hikey, lovel, hivel, s.first + 1, pans[1]);
        } else {
          emit(o, b, c2, c.lokey, c.hikey, lovel, hivel, s.first, pans[0]);
        }
      }
      prev_mono = nch == 1 && z == 0;
    }
  }
}

void name20(char *out, const std::string &s, const char *fallback, int n) {
  if (!s.empty()) snprintf(out, 21, "%.20s", s.c_str());
  else snprintf(out, 21, fallback, n);
}

/* g: the caller's owner of the libgig file (see Gig), set as soon as it exists */
/* Compressed waves are scanned when first decoded, not when the file opens (libgig's LazyScan, our patch): a
   project with a big compressed .gig opens reading headers only. Until a wave is decoded its length is an upper
   bound from its data's size; the decode gives the real one. */
static struct LazyScanOn { LazyScanOn() { gig::Sample::LazyScan = true; } } g_lazy_scan_on;

int parse(SmpFont *f, const char *path, int (*cancel)(void *), void *carg, char *why, int wcap, Gig *&gp) {
  CancelHook hook = {cancel, carg};
  RIFF::progress_t prog;
  prog.callback = on_progress;
  prog.custom = &hook;
  gp = new Gig;
  Gig &g = *gp;
  if (g_fault == 1) { g_fault = 0; *(volatile int *)nullptr = 0; }   /* tests: a crash inside the guard */
  g.riff = new RIFF::File(path);
  if (g.riff->GetListType() != RIFF_TYPE_DLS) { say(why, wcap, "it isn't a DLS or GigaStudio file"); return 0; }
  g.file = new gig::File(g.riff);

  /* samples, a wave at a time (libgig opens the extension files as it goes) */
  size_t nwaves = 0;
  std::map<gig::Sample *, int> sidx;
  std::vector<SampleInfo> si;
  std::vector<gig::Sample *> waves;
  std::map<RIFF::File *, int> path_of;
  std::vector<std::string> paths;
  path_of[g.riff] = 0;
  paths.push_back(path);
  for (gig::Sample *s; (s = g.file->GetSample(nwaves, &prog)) != nullptr; nwaves++) {
    if (cancel && cancel(carg)) throw Cancelled();
    sidx[s] = (int)nwaves;
    waves.push_back(s);
  }
  si.resize(nwaves);
  int nsamp = 0, odd = 0, cut = 0;
  for (size_t w = 0; w < nwaves; w++) {
    gig::Sample *s = waves[w];
    RIFF::Chunk *data = SampleData::of(s);
    int ch = s->Channels > 2 ? 2 : (int)s->Channels;
    if (ch < 1 || !s->SamplesPerSecond || !s->SamplesTotal || !data) continue;
    /* plain PCM plays from the file; GigaStudio's compression is decoded by libgig; anything else can't play */
    si[w].pcm = !s->Compressed && s->FormatTag == 1 && (s->BitDepth == 8 || s->BitDepth == 16 || s->BitDepth == 24) &&
                s->FrameSize == (uint)(s->Channels * (s->BitDepth / 8)) &&
                (uint64_t)s->SamplesTotal * s->FrameSize <= data->GetSize();
    if (!si[w].pcm && !s->Compressed) { odd = 1; continue; }
    if (nsamp + ch > MAX_SAMPLES) break;
    si[w].first = nsamp; si[w].nch = ch;
    nsamp += ch;
  }
  f->nshdr = nsamp;
  f->shdr = (SmpSfSample *)calloc(nsamp ? (size_t)nsamp : 1, sizeof *f->shdr);
  f->src = (SmpSrc *)calloc(nsamp ? (size_t)nsamp : 1, sizeof *f->src);
  if (!f->shdr || !f->src) throw std::bad_alloc();
  for (size_t w = 0; w < nwaves; w++) {
    gig::Sample *s = waves[w];
    if (si[w].first < 0) continue;
    RIFF::Chunk *data = SampleData::of(s);
    RIFF::File *file = data->GetFile();
    int pid;
    auto pf = path_of.find(file);
    if (pf != path_of.end()) pid = pf->second;
    else {
      pid = (int)paths.size();
      path_of[file] = pid;
      paths.push_back(file->GetFileName());
    }
    data->SetPos(0);
    uint64_t off = data->GetFilePos(), len = data->GetSize();
    uint64_t frames = s->SamplesTotal;
    bool pcm = si[w].pcm;
    if (pcm) {
      /* a PCM wave ends at its file's end at the latest (libgig already keeps the data chunk inside it) */
      uint64_t fsize = file->GetCurrentFileSize();
      uint64_t room = fsize > off ? (fsize - off) / s->FrameSize : 0;
      if (frames > room) frames = room;
    }
    /* a compressed wave can't hold more frames than its data allows: at best 2048 frames in 268 bytes (GigaStudio's
       densest frame), so a damaged frame count is held to that before anything is sized by it */
    if (!pcm && frames > (len / 268 + 1) * 2048) frames = (len / 268 + 1) * 2048;
    /* one length rule for every wave: one longer than 2^26 frames (25 minutes at 44.1 kHz) is cut there, and
       counted; sf_gig_decode reads no further */
    if (frames > MAX_DECODE_FRAMES) { frames = MAX_DECODE_FRAMES; cut = 1; }
    if (!frames) {
      for (int k = 0; k < si[w].nch; k++) f->src[si[w].first + k].kind = SMP_SRC_PCM_FILE;   /* empty: never loads */
      continue;
    }
    for (int k = 0; k < si[w].nch; k++) {
      SmpSfSample *sh = &f->shdr[si[w].first + k];
      SmpSrc *c = &f->src[si[w].first + k];
      name20(sh->name, s->pInfo ? s->pInfo->Name : std::string(), "Wave %d", (int)w + 1);
      sh->start = 0; sh->end = (uint32_t)frames; sh->rate = s->SamplesPerSecond;
      sh->pitch = (uint8_t)clampi((int)s->MIDIUnityNote, 0, 127); sh->correction = 0; sh->link = 0; sh->type = SF_MONO;
      if (s->Loops && s->LoopEnd >= s->LoopStart) {
        sh->loop_start = s->LoopStart;
        sh->loop_end = s->LoopEnd + 1;                   /* libgig's LoopEnd is the loop's last frame */
      }
      c->path_id = pid; c->channel = (uint8_t)k; c->channels = (uint8_t)s->Channels;
      if (pcm) {
        c->kind = SMP_SRC_PCM_FILE; c->offset = off;
        c->enc = (uint8_t)(s->BitDepth == 8 ? SMP_PCM_U8 : s->BitDepth == 16 ? SMP_PCM_S16LE : SMP_PCM_S24LE);
        if (s->SamplesTotal >= s->SamplesPerSecond) f->has_long = 1;
      } else {
        /* decoded by libgig (sf_gig_decode): the data chunk's place keys the cache, index finds the wave */
        c->kind = SMP_SRC_DECODED; c->offset = off; c->length = len ? len : 1; c->giga = 1; c->index = (uint32_t)w;
        f->has_long = 1;
      }
    }
  }
  f->paths = (char **)calloc(paths.size(), sizeof *f->paths);
  if (!f->paths) throw std::bad_alloc();
  f->npaths = (int)paths.size();
  for (size_t k = 0; k < paths.size(); k++) if (!(f->paths[k] = _strdup(paths[k].c_str()))) throw std::bad_alloc();

  /* instruments */
  Out o;
  if (odd) o.ign.add("wave format");
  if (cut) o.ign.add("wave over 25 minutes (cut there)");
  std::vector<SfPhdr> ph;
  std::vector<int> rstart;
  for (size_t n = 0; ; n++) {
    gig::Instrument *ins = g.file->GetInstrument(n, &prog);
    if (!ins) break;
    if (cancel && cancel(carg)) throw Cancelled();
    if (ph.size() >= 65535) break;
    SfPhdr p;
    memset(&p, 0, sizeof p);
    name20(p.name, ins->pInfo ? ins->pInfo->Name : std::string(), "Instrument %d", (int)n + 1);
    if (ins->IsDrum) p.bank = 128;
    else p.bank = (uint16_t)((ins->MIDIBankCoarse & 127) ? (ins->MIDIBankCoarse & 127) : (ins->MIDIBankFine & 127));
    p.program = (uint16_t)(ins->MIDIProgram & 127);
    ph.push_back(p);
    rstart.push_back((int)o.regs.size());
    if (ins->Attenuation) o.ign.add("instrument attenuation");
    if (ins->GetMidiRule(0)) o.ign.add("MIDI rules");
    if (ins->ScriptSlotCount()) o.ign.add("instrument scripts");
    RegionCtx c;
    c.ins = ins;
    c.sw_lo = ins->DimensionKeyRange.low <= 127 ? ins->DimensionKeyRange.low : -1;
    c.sw_hi = ins->DimensionKeyRange.high <= 127 ? ins->DimensionKeyRange.high : -1;
    for (size_t r = 0; ; r++) {
      gig::Region *rg = ins->GetRegionAt(r);
      if (!rg) break;
      if (cancel && cancel(carg)) throw Cancelled();
      c.rg = rg;
      c.lokey = rg->KeyRange.low; c.hikey = rg->KeyRange.high > 127 ? 127 : rg->KeyRange.high;
      if (c.lokey > c.hikey) continue;
      build_region(o, c, sidx, si, f);
    }
  }
  if (ph.empty()) { say(why, wcap, "it has no instruments"); return 0; }
  rstart.push_back((int)o.regs.size());

  /* the model: presets, then the one block of regions, preset starts, modulator lists and velocity curves */
  int nins = (int)ph.size(), nr = (int)o.regs.size();
  f->nphdr = nins;
  f->phdr = (SfPhdr *)calloc((size_t)nins + 1, sizeof *f->phdr);
  f->order = (int *)calloc((size_t)nins + 1, sizeof *f->order);
  if (!f->phdr || !f->order) throw std::bad_alloc();
  memcpy(f->phdr, ph.data(), (size_t)nins * sizeof *f->phdr);
  {
    size_t sreg = (size_t)nr * sizeof(SmpSfRegion), sst = ((size_t)nins + 1) * sizeof(int);
    size_t smod = o.mods.size() * sizeof(SmpSfMod), mod_at = (sreg + sst + 7) & ~(size_t)7;
    size_t cur_at = (mod_at + smod + 7) & ~(size_t)7, total = cur_at + o.curves.size() * sizeof(float);
    char *block = (char *)calloc(1, total ? total : 1);
    if (!block) throw std::bad_alloc();
    f->regions = (SmpSfRegion *)block;
    f->rstart = (int *)(block + sreg);
    SmpSfMod *mods = (SmpSfMod *)(block + mod_at);
    float *curves = (float *)(block + cur_at);
    if (smod) memcpy(mods, o.mods.data(), smod);
    if (!o.curves.empty()) memcpy(curves, o.curves.data(), o.curves.size() * sizeof(float));
    for (int i = 0; i < nr; i++) {
      SmpSfRegion *r = &f->regions[i];
      *r = o.regs[i];
      r->imod = mods + o.moff[i]; r->nimod = o.nmod[i];
      r->pmod = nullptr; r->npmod = 0;
      r->cond.amp_velcurve = o.curve[i] >= 0 ? curves + (size_t)o.curve[i] * 128 : nullptr;
    }
    memcpy(f->rstart, rstart.data(), sst);
    f->nregions = nr;
  }
  if (!sf_build_order(f)) throw std::bad_alloc();
  f->sfz_ignored = (int)o.ign.names.size();
  snprintf(f->sfz_first_ignored, sizeof f->sfz_first_ignored, "%s", o.ign.first.c_str());
  f->dls_boosts = o.boosts;
  return 1;
}

/* the reason an exception gives */
void reason_of(const std::string &m, char *why, int wcap) { say(why, wcap, m.empty() ? "it can't be read" : m.c_str()); }

/* The structured exceptions a damaged file could still raise inside libgig (a bad pointer, a division by zero), for
   the last-resort guards below; C++ exceptions are caught where they are thrown. The crashes the damage tests found
   are fixed in libgig itself (see VERSION.txt); a guard only catches one not yet found. After a trip, nothing more
   is trusted: libgig's objects from that call are leaked rather than destroyed, and g_crashed refuses every later
   GigaStudio open and decode in this process, with CRASHED as the reason, since the crash may have damaged the
   heap and only a restart is safe. */
int seh_filter(unsigned long code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_DATATYPE_MISALIGNMENT: case EXCEPTION_ILLEGAL_INSTRUCTION:
      return EXCEPTION_EXECUTE_HANDLER;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
}

void tripped(char *why, int wcap) {
  InterlockedIncrement(&g_guards);
  InterlockedExchange(&g_crashed, 1);
  say(why, wcap, CRASHED);
}

/* Freeing a libgig file touches its statics, so it needs g_lock. A font closed while another file is being parsed
   doesn't wait for that: its file goes on a list that whoever holds the lock frees before letting go. */
SRWLOCK g_pending_lock = SRWLOCK_INIT;
std::vector<Gig *> *g_pending;
void free_caught(Gig *g) { try { delete g; } catch (...) {} }
void free_guarded(Gig *g) {
  __try { free_caught(g); } __except (seh_filter(GetExceptionCode())) { InterlockedIncrement(&g_guards); InterlockedExchange(&g_crashed, 1); }
}
void drain_pending(void) {   /* g_lock held */
  for (;;) {
    Gig *g = nullptr;
    AcquireSRWLockExclusive(&g_pending_lock);
    if (g_pending && !g_pending->empty()) { g = g_pending->back(); g_pending->pop_back(); }
    ReleaseSRWLockExclusive(&g_pending_lock);
    if (!g) return;
    if (!g_crashed) free_guarded(g);              /* after a crash nothing more is freed */
  }
}
bool pending_empty(void) {
  AcquireSRWLockExclusive(&g_pending_lock);
  bool e = !g_pending || g_pending->empty();
  ReleaseSRWLockExclusive(&g_pending_lock);
  return e;
}
void lock_gig(void) { AcquireSRWLockExclusive(&g_lock); }
void unlock_gig(void) {
  for (;;) {
    drain_pending();
    ReleaseSRWLockExclusive(&g_lock);
    /* a file added after the drain is freed here, or by the next holder if someone else has the lock now */
    if (pending_empty() || !TryAcquireSRWLockExclusive(&g_lock)) return;
  }
}

int parse_caught(SmpFont *f, const char *path, int (*cancel)(void *), void *carg, char *why, int wcap) {
  Gig *g = nullptr;
  int r = 0;
  try {
    r = parse(f, path, cancel, carg, why, wcap, g);
  } catch (const Cancelled &) {
    say(why, wcap, "cancelled");
  } catch (const RIFF::Exception &e) {
    reason_of(e.Message, why, wcap);
  } catch (const std::bad_alloc &) {
    say(why, wcap, "out of memory");
  } catch (const std::length_error &e) {
    say(why, wcap, e.what());
  } catch (const std::exception &e) {
    say(why, wcap, e.what() && *e.what() ? e.what() : "it can't be read");
  } catch (...) {
    say(why, wcap, "it can't be read");
  }
  free_caught(g);                                  /* normal and C++-exception paths only */
  return r;
}

int parse_guarded(SmpFont *f, const char *path, int (*cancel)(void *), void *carg, char *why, int wcap) {
  __try {
    return parse_caught(f, path, cancel, carg, why, wcap);
  } __except (seh_filter(GetExceptionCode())) {
    tripped(why, wcap);
    return -1;
  }
}

/* one decode: its arguments, and what it holds, so the guard can let go of it all */
struct Decode {
  SmpFont *f; const SmpSrc *c; int *chans; long *frames; int *rate; int (*cancel)(void *); void *carg;
  char *err; int cap;
  float *out; void *raw; gig::buffer_t dbuf; bool locked;
};

void decode_unlock(Decode &d) { if (d.locked) { d.locked = false; unlock_gig(); } }

float *decode(Decode &d) {
  Gig *g = (Gig *)d.f->gig;
  if (!g) {
    /* the loader thread's libgig file for this font, opened when its first compressed wave is needed and kept
       until the font is freed. Opening reads every wave's header and scans each compressed wave's frames, under
       the lock, with the cancel hook asked as libgig goes (a closing font stops it). */
    CancelHook hook = {d.cancel, d.carg};
    RIFF::progress_t prog;
    prog.callback = on_progress;
    prog.custom = &hook;
    lock_gig();
    d.locked = true;
    g = new Gig;
    try {
      if (g_fault == 2) { g_fault = 0; *(volatile int *)nullptr = 0; }   /* tests: a crash inside the guard */
      g->riff = new RIFF::File(d.f->path);
      g->file = new gig::File(g->riff);
      g->file->GetSample(0, &prog);
    } catch (...) { free_caught(g); throw; }
    d.f->gig = g;
    decode_unlock(d);
  }
  gig::Sample *s = g->file->GetSample(d.c->index);
  if (s) s->EnsureScanned();                     /* its frames scanned now (lazily: see g_lazy_scan_on) */
  if (!s || !s->SamplesTotal || !s->Channels || !s->Compressed) { say(d.err, d.cap, "the wave is missing"); return nullptr; }
  /* the same rules as the parse: no more frames than the data can hold (2048 in 268 bytes at best), cut at 2^26 */
  uint64_t n = s->SamplesTotal, most = (SampleData::of(s)->GetSize() / 268 + 1) * 2048;
  if (n > most) n = most;
  if (n > MAX_DECODE_FRAMES) n = MAX_DECODE_FRAMES;
  int ch = (int)s->Channels, bytes = s->BitDepth == 24 ? 3 : 2;
  if (ch > 2) { say(d.err, d.cap, "the wave has more than two channels"); return nullptr; }
  d.out = (float *)malloc((size_t)n * ch * sizeof(float));
  d.raw = malloc((size_t)DECODE_CHUNK * ch * bytes);
  if (!d.out || !d.raw) { say(d.err, d.cap, "out of memory"); return nullptr; }
  d.dbuf = gig::Sample::CreateDecompressionBuffer(DECODE_CHUNK);
  s->SetPos(0);
  uint64_t at = 0;
  while (at < n) {
    if (d.cancel && d.cancel(d.carg)) throw Cancelled();
    uint64_t want = n - at > DECODE_CHUNK ? DECODE_CHUNK : n - at;
    uint64_t got = s->Read(d.raw, want, &d.dbuf);
    if (!got) break;
    if (got > want) got = want;
    for (uint64_t i = 0; i < got; i++)
      for (int k = 0; k < ch; k++) {
        float v;
        if (bytes == 2) v = ((const int16_t *)d.raw)[i * ch + k] / 32768.0f;
        else {
          const uint8_t *p = (const uint8_t *)d.raw + (i * ch + k) * 3;
          int32_t x = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
          v = x / 8388608.0f;
        }
        d.out[(size_t)k * n + at + i] = v;   /* channel after channel, as decode_audio gives */
      }
    at += got;
  }
  if (!at) { say(d.err, d.cap, "the wave doesn't decode"); return nullptr; }
  if (at < n) {
    /* a short read (damage): the frames there are, moved down channel by channel */
    for (int k = 1; k < ch; k++) memmove(d.out + (size_t)k * at, d.out + (size_t)k * n, (size_t)at * sizeof(float));
  }
  *d.chans = ch; *d.frames = (long)at; *d.rate = (int)s->SamplesPerSecond;
  float *r = d.out;
  d.out = nullptr;
  return r;
}

float *decode_caught(Decode &d) {
  float *r = nullptr;
  try {
    r = decode(d);
  } catch (const Cancelled &) {
    say(d.err, d.cap, "cancelled");
  } catch (const RIFF::Exception &e) {
    reason_of(e.Message, d.err, d.cap);
  } catch (const std::bad_alloc &) {
    say(d.err, d.cap, "out of memory");
  } catch (const std::exception &e) {
    say(d.err, d.cap, e.what() && *e.what() ? e.what() : "it can't be read");
  } catch (...) {
    say(d.err, d.cap, "it can't be read");
  }
  decode_unlock(d);
  return r;
}

float *decode_guarded(Decode *d) {
  __try {
    return decode_caught(*d);
  } __except (seh_filter(GetExceptionCode())) {
    d->f->gig = nullptr;                                  /* leaked: it may be half-built */
    d->out = nullptr; d->raw = nullptr;                   /* (leaked too: the heap isn't trusted now) */
    d->dbuf.pStart = nullptr;
    tripped(d->err, d->cap);
    if (d->locked) { d->locked = false; ReleaseSRWLockExclusive(&g_lock); }
    return nullptr;
  }
}

} /* namespace */

extern "C" int sf_gig_crashed(void) { return g_crashed != 0; }

extern "C" int sf_parse_gig(SmpFont *f, const char *path, int (*cancel)(void *), void *carg, char *why, int wcap) {
  int r;
  if (why && wcap > 0) why[0] = 0;
  if (g_crashed) { say(why, wcap, CRASHED); return -1; }
  lock_gig();
  if (g_crashed) { ReleaseSRWLockExclusive(&g_lock); say(why, wcap, CRASHED); return -1; }   /* a decode crashed meanwhile */
  r = parse_guarded(f, path, cancel, carg, why, wcap);
  if (g_crashed) ReleaseSRWLockExclusive(&g_lock);        /* nothing pending is freed after a crash */
  else unlock_gig();
  return r;
}

extern "C" float *sf_gig_decode(SmpFont *f, const SmpSrc *c, int *chans, long *frames, int *rate,
                                int (*cancel)(void *), void *carg, char *err, int cap) {
  Decode d;
  float *r;
  if (err && cap > 0) err[0] = 0;
  if (g_crashed) { say(err, cap, CRASHED); return nullptr; }
  d.f = f; d.c = c; d.chans = chans; d.frames = frames; d.rate = rate; d.cancel = cancel; d.carg = carg;
  d.err = err; d.cap = cap; d.out = nullptr; d.raw = nullptr; d.locked = false;
  r = decode_guarded(&d);
  free(d.out); free(d.raw);
  if (d.dbuf.pStart) gig::Sample::DestroyDecompressionBuffer(d.dbuf);
  return r;
}

/* the font's libgig file for decoding, if one was opened (sf_free calls it): freed now when the lock is free, else
   by the lock's holder when it lets go, so closing a font never waits for another file's parse; after a crash it is
   left (leaked) */
extern "C" void sf_gig_free(SmpFont *f) {
  Gig *g;
  if (!f || !f->gig) return;
  g = (Gig *)f->gig;
  f->gig = nullptr;
  if (g_crashed) return;
  AcquireSRWLockExclusive(&g_pending_lock);
  try {
    if (!g_pending) g_pending = new std::vector<Gig *>;
    g_pending->push_back(g);
  } catch (...) { g = nullptr; }                          /* out of memory: leaked rather than block */
  ReleaseSRWLockExclusive(&g_pending_lock);
  if (TryAcquireSRWLockExclusive(&g_lock)) unlock_gig();
}

extern "C" long smp_sf_test_gig_guards(void) { return g_guards; }
extern "C" void smp_sf_test_gig_fault(int where) { g_fault = where; }
