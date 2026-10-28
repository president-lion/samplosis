// stretch.cpp: the two stretchers keep length can use, Signalsmith Stretch and Rubber Band R3, behind one call, each
// inside try/catch so no C++ exception ever crosses into C (and from there into the host, which would crash it). Built
// with /EHs (not /EHsc): the Rubber Band C API functions are extern "C", and /EHsc would let the compiler assume they
// never throw and drop the catch.
#include <exception>
#include <new>
#include <stdexcept>
#include <vector>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "rubberband/rubberband/rubberband-c.h"
#include "signalsmith/signalsmith-stretch.h"

#define RB_CHUNK 4096
#define SS_CHUNK 16384

static volatile long g_flags;   // tests: 1 = throw inside the guarded region

extern "C" void smp_rb_set_debug_flags(long f) { g_flags = f; }

typedef int (*CancelFn)(void *);

static void drain(RubberBandState st, float *out, unsigned long long T, unsigned long long *got, float *ob) {
  int a;
  while ((a = rubberband_available(st)) > 0) {
    float *o[2] = { ob, ob + RB_CHUNK };
    unsigned m = a > RB_CHUNK ? RB_CHUNK : (unsigned)a, i;
    m = rubberband_retrieve(st, o, m);
    if (!m) break;
    for (i = 0; i < m && *got < T; i++, (*got)++) { out[2 * *got] = ob[i]; out[2 * *got + 1] = ob[RB_CHUNK + i]; }
  }
}

struct StateGuard {
  RubberBandState st;
  StateGuard() : st(0) {}
  ~StateGuard() { if (st) { try { rubberband_delete(st); } catch (...) {} } }
};

// Rubber Band R3, offline (it studies the whole sample first), finer engine, channels together
static int rubber_band(int keep_tone, const float *L, const float *R, unsigned F, int rate, double time_ratio,
                       double pitch_scale, float *out, unsigned long long T, CancelFn cancel, void *arg, char *why,
                       int cap) {
  StateGuard g;
  std::vector<float> ob(2 * RB_CHUNK);
  unsigned long long got = 0, i;
  int opt = RubberBandOptionProcessOffline | RubberBandOptionEngineFiner | RubberBandOptionChannelsTogether;
  if (keep_tone) opt |= RubberBandOptionFormantPreserved;
  g.st = rubberband_new((unsigned)rate, 2, opt, time_ratio, pitch_scale);
  if (!g.st) { snprintf(why, (size_t)cap, "Rubber Band couldn't start"); return 1; }
  if (g_flags & 1) throw std::runtime_error("forced test failure");
  rubberband_set_expected_input_duration(g.st, F);
  rubberband_set_max_process_size(g.st, RB_CHUNK);
  for (i = 0; i < F; i += RB_CHUNK) {                    // study the whole sample
    unsigned n = (unsigned)(F - i < RB_CHUNK ? F - i : RB_CHUNK);
    const float *in[2] = { L + i, R + i };
    rubberband_study(g.st, in, n, i + n >= F);
    if (cancel && cancel(arg)) return 2;
  }
  for (i = 0; i < F; i += RB_CHUNK) {                    // then process it, retrieving as it goes
    unsigned n = (unsigned)(F - i < RB_CHUNK ? F - i : RB_CHUNK);
    const float *in[2] = { L + i, R + i };
    rubberband_process(g.st, in, n, i + n >= F);
    drain(g.st, out, T, &got, ob.data());
    if (cancel && cancel(arg)) return 2;
  }
  drain(g.st, out, T, &got, ob.data());
  return 0;
}

// Signalsmith Stretch, default preset. The input runs F frames and then inputLatency frames of silence (so the
// processing time reaches the end); the first inputLatency frames go in by seek, so output frame 0 lines up with
// input frame 0 after outputLatency frames of pre-roll, which are dropped. The rest goes in by chunks, each given the
// output length that keeps the running ratio at T/F, and flush gives the last outputLatency frames.
static int signalsmith_stretch(int keep_tone, const float *L, const float *R, unsigned F, int rate, double pitch_scale,
                       float *out, unsigned long long T, CancelFn cancel, void *arg) {
  signalsmith::stretch::SignalsmithStretch<float> s;
  s.presetDefault(2, (float)rate);
  s.setTransposeFactor((float)pitch_scale);
  if (keep_tone) s.setFormantFactor(1, true);           // the formants stay put; their base frequency is estimated
  if (g_flags & 1) throw std::runtime_error("forced test failure");
  const unsigned long long il = (unsigned long long)s.inputLatency(), ol = (unsigned long long)s.outputLatency();
  const unsigned long long ch = il > SS_CHUNK || ol > SS_CHUNK ? (il > ol ? il : ol) : SS_CHUNK;
  std::vector<float> ib(2 * ch), ob(2 * (ch + 2));
  float *in[2] = { ib.data(), ib.data() + ch }, *o[2] = { ob.data(), ob.data() + ch + 2 };
  unsigned long long made = 0;                            // output frames so far, pre-roll included
  auto fill = [&](unsigned long long at, unsigned long long n) {   // input frames at.., silence past F
    for (unsigned long long k = 0; k < n; k++) {
      bool real = at + k < F;
      in[0][k] = real ? L[at + k] : 0.f; in[1][k] = real ? R[at + k] : 0.f;
    }
  };
  auto keep = [&](unsigned long long n) {
    for (unsigned long long k = 0; k < n; k++, made++)
      if (made >= ol && made - ol < T) { out[2 * (made - ol)] = o[0][k]; out[2 * (made - ol) + 1] = o[1][k]; }
  };
  fill(0, il);
  s.seek(in, (int)il, (double)F / (double)T);
  // F input frames remain (F - il of the sample, then il of silence); in steps whose output is at most ch frames
  unsigned long long step = (unsigned long long)floor((double)ch * (double)F / (double)T);
  if (step < 1) step = 1;
  if (step > ch) step = ch;
  for (unsigned long long done = 0; done < F;) {
    unsigned long long n = F - done < step ? F - done : step;
    unsigned long long m = (unsigned long long)floor((double)(done + n) * (double)T / (double)F + 0.5) -
                           (unsigned long long)floor((double)done * (double)T / (double)F + 0.5);
    fill(il + done, n);
    s.process(in, (int)n, o, (int)m);
    keep(m);
    done += n;
    if (cancel && cancel(arg)) return 2;
  }
  s.flush(o, (int)ol, (float)((double)F / (double)T));   // at the same rate, or the end comes out louder
  keep(ol);
  return 0;
}

// engine: 0 Signalsmith Stretch, 1 Rubber Band R3. keep_tone: formants kept where they were.
// L, R: F frames each. out: T stereo frames, zeroed by the caller; frames past T are dropped, short of T stay silent.
// 0 ok, 1 failed (why says how), 2 cancelled.
extern "C" int smp_stretch(int engine, int keep_tone, const float *L, const float *R, unsigned F, int rate,
                           double time_ratio, double pitch_scale, float *out, unsigned long long T, CancelFn cancel,
                           void *arg, char *why, int cap) {
  const char *name = engine ? "Rubber Band" : "Signalsmith Stretch";
  try {
    if (engine) return rubber_band(keep_tone, L, R, F, rate, time_ratio, pitch_scale, out, T, cancel, arg, why, cap);
    return signalsmith_stretch(keep_tone, L, R, F, rate, pitch_scale, out, T, cancel, arg);
  } catch (const std::bad_alloc &) {
    snprintf(why, (size_t)cap, "%s ran out of memory", name);
  } catch (const std::exception &x) {
    snprintf(why, (size_t)cap, "%s failed (%s)", name, x.what());
  } catch (...) {
    snprintf(why, (size_t)cap, "%s failed", name);
  }
  return 1;
}
