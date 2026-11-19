/* Platform layer. On Windows this is just windows.h. Elsewhere, plat_posix.h supplies the Win32 types and calls the
   code uses (threads, locks, events, atomics, wide-character file paths and folders), built on POSIX, so the shared
   code compiles unchanged on both. */
#ifndef SMP_PLAT_H
#define SMP_PLAT_H
#ifdef _WIN32
#include "plat.h"
#else
#include "plat_posix.h"
#endif
#endif
