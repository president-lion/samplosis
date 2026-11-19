// rb.cpp: Rubber Band for fx.c's PitchShift. rubberband/ holds the exact source pedalboard 0.9.26 vendors
// (https://github.com/BreakfastQuay/Rubberband.git at 2be46b0dffb13273a67396c77bc9278736bb03d2, only the files the
// single-file build uses; GPL, see rubberband/COPYING), built with the configuration pedalboard's setup.py gives it
// on Windows, so its output matches pedalboard's. fx.c uses Rubber Band's own C API (rubberband-c.h).
#define ALREADY_CONFIGURED 1
#define USE_BQRESAMPLER 1
#define USE_BUILTIN_FFT 1
#define NO_THREADING 1
#define _HAS_STD_BYTE 0
#define NOMINMAX 1
#ifdef __APPLE__
#define HAVE_VDSP 1                 /* Accelerate's vector functions (macOS has no sincosf) */
#endif
#include "rubberband/single/RubberBandSingle.cpp"
