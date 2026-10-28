// rbsafe.cpp: every Rubber Band call render.c makes, inside try/catch, so no C++ exception ever crosses into C (and
// from there into the host, which would crash it). Built with /EHs (not /EHsc): the Rubber Band C API functions
// are extern "C", and /EHsc would let the compiler assume they never throw and drop the catch.
#include <exception>
#include <new>
#include <stdexcept>
#include <stdio.h>
#include <string.h>
#include "rubberband/rubberband/rubberband-c.h"

#define RB_CHUNK 4096

static volatile long g_flags;   // tests: 1 = throw inside the guarded region

extern "C" void smp_rb_set_debug_flags(long f) { g_flags = f; }
extern "C" long smp_rb_debug_flags(void) { return g_flags; }

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

// L, R: F frames each. out: T stereo frames, zeroed by the caller; frames past T are dropped, short of T stay silent.
// 0 ok, 1 failed (why says how), 2 cancelled.
extern "C" int smp_rb_render(const float *L, const float *R, unsigned F, int rate, double time_ratio,
                             double pitch_scale, float *out, unsigned long long T, int (*cancel)(void *), void *arg,
                             float *ob, char *why, int cap) {
  try {
    StateGuard g;
    unsigned long long got = 0, i;
    g.st = rubberband_new((unsigned)rate, 2,
                          RubberBandOptionProcessOffline | RubberBandOptionEngineFiner | RubberBandOptionChannelsTogether,
                          time_ratio, pitch_scale);
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
      drain(g.st, out, T, &got, ob);
      if (cancel && cancel(arg)) return 2;
    }
    drain(g.st, out, T, &got, ob);
    return 0;
  } catch (const std::bad_alloc &) {
    snprintf(why, (size_t)cap, "Rubber Band ran out of memory");
  } catch (const std::exception &x) {
    snprintf(why, (size_t)cap, "Rubber Band failed (%s)", x.what());
  } catch (...) {
    snprintf(why, (size_t)cap, "Rubber Band failed");
  }
  return 1;
}
