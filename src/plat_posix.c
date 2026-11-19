/* The Win32 subset on POSIX (macOS); see plat_posix.h. Not built on Windows. */
#ifndef _WIN32
#include "plat_posix.h"
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <time.h>
#include <limits.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#endif

/* ================================================================ errors */
static __thread DWORD t_err;
DWORD GetLastError(void) { return t_err; }
void SetLastError(DWORD e) { t_err = e; }
static void set_errno_err(int e) {
  switch (e) {
  case 0: t_err = 0; break;
  case ENOENT: t_err = ERROR_FILE_NOT_FOUND; break;
  case ENOTDIR: t_err = ERROR_PATH_NOT_FOUND; break;
  case EACCES: case EPERM: case EROFS: t_err = ERROR_ACCESS_DENIED; break;
  case EEXIST: t_err = ERROR_ALREADY_EXISTS; break;
  case ENOTEMPTY: t_err = ERROR_DIR_NOT_EMPTY; break;
  case ENOSPC: t_err = ERROR_DISK_FULL; break;
  case EMFILE: case ENFILE: t_err = ERROR_TOO_MANY_OPEN_FILES; break;
  case ENOMEM: t_err = ERROR_NOT_ENOUGH_MEMORY; break;
  case ENAMETOOLONG: t_err = ERROR_FILENAME_EXCED_RANGE; break;
  case EBADF: t_err = ERROR_INVALID_HANDLE; break;
  case EINVAL: t_err = ERROR_INVALID_PARAMETER; break;
  case ETIMEDOUT: t_err = ERROR_SEM_TIMEOUT; break;
  case ENETUNREACH: t_err = ERROR_NETWORK_UNREACHABLE; break;
  case EHOSTUNREACH: t_err = ERROR_HOST_UNREACHABLE; break;
  default: t_err = 0x20000000u | (DWORD)e; break;       /* an errno with no Win32 name */
  }
}

/* ================================================================ UTF-8 <-> UTF-32 */
static int utf8_decode(const unsigned char *s, size_t n, size_t *i, uint32_t *cp) {
  unsigned c = s[*i];
  int len; uint32_t v, min;
  if (c < 0x80) { *cp = c; (*i)++; return 1; }
  if ((c & 0xE0) == 0xC0) { len = 2; v = c & 0x1F; min = 0x80; }
  else if ((c & 0xF0) == 0xE0) { len = 3; v = c & 0x0F; min = 0x800; }
  else if ((c & 0xF8) == 0xF0) { len = 4; v = c & 0x07; min = 0x10000; }
  else { (*i)++; return 0; }
  if (*i + len > n) { *i = n; return 0; }
  for (int k = 1; k < len; k++) {
    unsigned d = s[*i + k];
    if ((d & 0xC0) != 0x80) { *i += k; return 0; }
    v = (v << 6) | (d & 0x3F);
  }
  *i += len;
  if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
  *cp = v;
  return 1;
}

/* code page 437, bytes 0x80 to 0xFF */
static const uint16_t cp437[128] = {
  0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
  0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
  0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
  0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
  0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
  0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
  0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
  0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0 };

int MultiByteToWideChar(UINT cp, DWORD flags, const char *s, int n, wchar_t *w, int cap) {
  size_t len = n < 0 ? strlen(s) + 1 : (size_t)n, i = 0;
  int out = 0;
  if (cp == CP_OEMCP) {                     /* one byte, one character */
    for (i = 0; i < len; i++) {
      unsigned char b = (unsigned char)s[i];
      if (cap) { if (out >= cap) { t_err = ERROR_INSUFFICIENT_BUFFER; return 0; } w[out] = b < 0x80 ? b : cp437[b - 0x80]; }
      out++;
    }
    return out;
  }
  /* CP_ACP is UTF-8 here too */
  while (i < len) {
    uint32_t c;
    if (!utf8_decode((const unsigned char *)s, len, &i, &c)) {
      if (flags & MB_ERR_INVALID_CHARS) { t_err = ERROR_NO_UNICODE_TRANSLATION; return 0; }
      c = 0xFFFD;
    }
    if (cap) {
      if (out >= cap) { t_err = ERROR_INSUFFICIENT_BUFFER; return 0; }
      w[out] = (wchar_t)c;
    }
    out++;
  }
  return out;
}

int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t *w, int n, char *s, int cap, const char *def, BOOL *used) {
  (void)cp; (void)def;
  size_t len = n < 0 ? wcslen(w) + 1 : (size_t)n;
  int out = 0;
  if (used) *used = FALSE;
  for (size_t i = 0; i < len; i++) {
    uint32_t c = (uint32_t)w[i];
    unsigned char b[4]; int k;
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
      if (flags & WC_ERR_INVALID_CHARS) { t_err = ERROR_NO_UNICODE_TRANSLATION; return 0; }
      c = 0xFFFD;
    }
    if (c < 0x80) { b[0] = (unsigned char)c; k = 1; }
    else if (c < 0x800) { b[0] = 0xC0 | (c >> 6); b[1] = 0x80 | (c & 0x3F); k = 2; }
    else if (c < 0x10000) { b[0] = 0xE0 | (c >> 12); b[1] = 0x80 | ((c >> 6) & 0x3F); b[2] = 0x80 | (c & 0x3F); k = 3; }
    else { b[0] = 0xF0 | (c >> 18); b[1] = 0x80 | ((c >> 12) & 0x3F); b[2] = 0x80 | ((c >> 6) & 0x3F); b[3] = 0x80 | (c & 0x3F); k = 4; }
    if (cap) {
      if (out + k > cap) { t_err = ERROR_INSUFFICIENT_BUFFER; return 0; }
      memcpy(s + out, b, (size_t)k);
    }
    out += k;
  }
  return out;
}

int CompareStringOrdinal(const wchar_t *a, int na, const wchar_t *b, int nb, BOOL ic) {
  size_t la = na < 0 ? wcslen(a) : (size_t)na, lb = nb < 0 ? wcslen(b) : (size_t)nb;
  for (size_t i = 0; i < la && i < lb; i++) {
    wint_t x = (wint_t)a[i], y = (wint_t)b[i];
    if (ic) { x = towupper(x); y = towupper(y); }
    if (x != y) return x < y ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
  }
  return la == lb ? CSTR_EQUAL : la < lb ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
}

DWORD CharLowerBuffW(LPWSTR s, DWORD n) { for (DWORD i = 0; i < n; i++) s[i] = (wchar_t)towlower((wint_t)s[i]); return n; }
LPWSTR CharLowerW(LPWSTR s) { for (wchar_t *p = s; *p; p++) *p = (wchar_t)towlower((wint_t)*p); return s; }

int smp_path_utf8(const wchar_t *w, char *out, size_t cap) {
  if (!w || !cap) return 0;
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
  if (n <= 0) return 0;
  for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
  return 1;
}

/* a path for the file system, on the heap (paths can be long; callers free) */
static char *upath(const wchar_t *w) {
  size_t cap = wcslen(w) * 4 + 1;
  char *s = malloc(cap);
  if (!s) { t_err = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
  if (!smp_path_utf8(w, s, cap)) { free(s); t_err = ERROR_INVALID_NAME; return NULL; }
  return s;
}

static int home_dir(char *out, size_t cap) {
  const char *h = getenv("HOME");
  if (!h || !*h || strlen(h) >= cap) return 0;
  strcpy(out, h);
  return 1;
}

/* APPDATA and LOCALAPPDATA have no meaning here; the Mac's own folders stand in for them, so code that asks for them
   puts its files where Mac apps keep theirs. */
DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD cap) {
  char n8[256], v[4096];
  const char *val = NULL;
  if (WideCharToMultiByte(CP_UTF8, 0, name, -1, n8, sizeof n8, NULL, NULL) <= 0) return 0;
  if (!strcmp(n8, "APPDATA") || !strcmp(n8, "LOCALAPPDATA")) {
    if (!home_dir(v, sizeof v - 64)) return 0;
    strcat(v, "/Library/Application Support");
    val = v;
  } else if (!strcmp(n8, "TEMP") || !strcmp(n8, "TMP")) {
    val = getenv("TMPDIR");
    if (!val) val = "/tmp";
  } else {
    val = getenv(n8);
  }
  if (!val) { t_err = 203; return 0; }                 /* ERROR_ENVVAR_NOT_FOUND */
  int need = MultiByteToWideChar(CP_UTF8, 0, val, -1, NULL, 0);
  if (need <= 0) return 0;
  if (!buf || (DWORD)need > cap) return (DWORD)need;    /* the size needed, counting the terminator */
  MultiByteToWideChar(CP_UTF8, 0, val, -1, buf, (int)cap);
  return (DWORD)need - 1;
}

/* ================================================================ time */
static uint64_t now_ns(void) {
#ifdef __APPLE__
  static mach_timebase_info_data_t tb;
  if (!tb.denom) mach_timebase_info(&tb);
  return mach_absolute_time() * tb.numer / tb.denom;
#else
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
ULONGLONG GetTickCount64(void) { return now_ns() / 1000000ull; }
DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }
BOOL QueryPerformanceCounter(LARGE_INTEGER *c) { c->QuadPart = (LONGLONG)now_ns(); return TRUE; }
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000ll; return TRUE; }

#define EPOCH_DIFF 116444736000000000ull     /* 1601 to 1970 in 100 ns units */
static void ts_to_ft(const struct timespec *ts, FILETIME *ft) {
  uint64_t v = (uint64_t)ts->tv_sec * 10000000ull + (uint64_t)ts->tv_nsec / 100 + EPOCH_DIFF;
  ft->dwLowDateTime = (DWORD)v; ft->dwHighDateTime = (DWORD)(v >> 32);
}
static void ft_to_ts(const FILETIME *ft, struct timespec *ts) {
  uint64_t v = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
  v = v > EPOCH_DIFF ? v - EPOCH_DIFF : 0;
  ts->tv_sec = (time_t)(v / 10000000ull); ts->tv_nsec = (long)(v % 10000000ull) * 100;
}
void GetSystemTimeAsFileTime(FILETIME *ft) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts_to_ft(&ts, ft); }
void Sleep(DWORD ms) {
  struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}
BOOL SwitchToThread(void) { sched_yield(); return TRUE; }

/* ================================================================ system */
void GetSystemInfo(SYSTEM_INFO *si) {
  memset(si, 0, sizeof *si);
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  si->dwNumberOfProcessors = n > 0 ? (DWORD)n : 1;
  si->dwPageSize = (DWORD)getpagesize();
  si->dwAllocationGranularity = 65536;
}
DWORD GetActiveProcessorCount(WORD group) { (void)group; long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (DWORD)n : 1; }
DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
DWORD GetCurrentThreadId(void) {
#ifdef __APPLE__
  uint64_t id = 0; pthread_threadid_np(NULL, &id); return (DWORD)id;
#else
  return (DWORD)(uintptr_t)pthread_self();
#endif
}

/* VirtualAlloc: reserve maps the range inaccessible, commit makes pages readable and writable, decommit gives them
   back, release unmaps. Release passes no size, so reservations are remembered. */
typedef struct VReg { char *p; size_t n; struct VReg *next; } VReg;
static VReg *g_vregs;
static pthread_mutex_t g_vlock = PTHREAD_MUTEX_INITIALIZER;
static size_t page_round(size_t n) { size_t pg = (size_t)getpagesize(); return (n + pg - 1) & ~(pg - 1); }
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect) {
  (void)protect;
  if (!size) { t_err = ERROR_INVALID_PARAMETER; return NULL; }
  if (!addr || (type & MEM_RESERVE)) {
    size_t n = page_round(size);
    int prot = (type & MEM_COMMIT) ? PROT_READ | PROT_WRITE : PROT_NONE;
    void *p = mmap(NULL, n, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { set_errno_err(errno); return NULL; }
    VReg *r = malloc(sizeof *r);
    if (!r) { munmap(p, n); t_err = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
    r->p = p; r->n = n;
    pthread_mutex_lock(&g_vlock); r->next = g_vregs; g_vregs = r; pthread_mutex_unlock(&g_vlock);
    return p;
  }
  /* commit within a reservation */
  size_t pg = (size_t)getpagesize();
  char *a = (char *)((uintptr_t)addr & ~(pg - 1));
  size_t n = page_round((size_t)((char *)addr - a) + size);
  if (mprotect(a, n, PROT_READ | PROT_WRITE)) { set_errno_err(errno); return NULL; }
  return addr;
}
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type) {
  if (type & MEM_RELEASE) {
    pthread_mutex_lock(&g_vlock);
    VReg **pp = &g_vregs, *r = NULL;
    while (*pp) { if ((*pp)->p == addr) { r = *pp; *pp = r->next; break; } pp = &(*pp)->next; }
    pthread_mutex_unlock(&g_vlock);
    if (!r) { t_err = ERROR_INVALID_PARAMETER; return FALSE; }
    munmap(r->p, r->n);
    free(r);
    return TRUE;
  }
  if (type & MEM_DECOMMIT) {
    size_t pg = (size_t)getpagesize();
    char *a = (char *)((uintptr_t)addr & ~(pg - 1));
    size_t n = page_round((size_t)((char *)addr - a) + size);
    madvise(a, n, MADV_DONTNEED);
    mprotect(a, n, PROT_NONE);
    return TRUE;
  }
  return FALSE;
}

static int g_self_module;
BOOL GetModuleHandleExW(DWORD flags, LPCWSTR addr, HMODULE *out) { (void)flags; (void)addr; *out = &g_self_module; return TRUE; }
BOOL FreeLibrary(HMODULE m) { (void)m; return TRUE; }
void FreeLibraryAndExitThread(HMODULE m, DWORD code) { (void)m; pthread_exit((void *)(uintptr_t)code); }

/* ================================================================ handles */
enum { H_EVENT = 1, H_THREAD, H_FILE, H_FIND, H_CURTHREAD };
typedef struct SmpFind { DIR *d; char *dir; char *pat; } SmpFind;
struct SmpHandle {
  int type;
  /* event */
  int manual, signaled;
  /* thread */
  pthread_t th; LPTHREAD_START_ROUTINE fn; void *arg; int started, finished, detached, joined; DWORD code;
  int refs;                                       /* the handle and the running thread */
  /* file */
  int fd; int delete_on_close; char *path;
  /* find */
  SmpFind f;
};

/* One lock and condition for every event and thread: waits are rare and short, and WaitForMultipleObjects can then
   wait on any mix of handles. */
static pthread_mutex_t g_hlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_hcond = PTHREAD_COND_INITIALIZER;

static struct SmpHandle g_curthread = { H_CURTHREAD };
HANDLE GetCurrentThread(void) { return &g_curthread; }

HANDLE CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name) {
  (void)sa; (void)name;
  HANDLE h = calloc(1, sizeof *h);
  if (!h) { t_err = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
  h->type = H_EVENT; h->manual = manual; h->signaled = initial; h->fd = -1;
  return h;
}
BOOL SetEvent(HANDLE h) {
  if (!h || h->type != H_EVENT) { t_err = ERROR_INVALID_HANDLE; return FALSE; }
  pthread_mutex_lock(&g_hlock); h->signaled = 1; pthread_cond_broadcast(&g_hcond); pthread_mutex_unlock(&g_hlock);
  return TRUE;
}
BOOL ResetEvent(HANDLE h) {
  if (!h || h->type != H_EVENT) { t_err = ERROR_INVALID_HANDLE; return FALSE; }
  pthread_mutex_lock(&g_hlock); h->signaled = 0; pthread_mutex_unlock(&g_hlock);
  return TRUE;
}

/* under g_hlock: is h signaled; consumes an auto-reset event when take */
static int ready(HANDLE h, int take) {
  if (h->type == H_EVENT) { if (!h->signaled) return 0; if (take && !h->manual) h->signaled = 0; return 1; }
  if (h->type == H_THREAD) return h->finished;
  return 1;                                       /* files and the rest count as signaled, as on Windows */
}
static void deadline(DWORD ms, struct timespec *ts) {
  clock_gettime(CLOCK_REALTIME, ts);
  ts->tv_sec += ms / 1000; ts->tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) {
  struct timespec ts; DWORD i;
  if (!n) { t_err = ERROR_INVALID_PARAMETER; return WAIT_FAILED; }
  for (i = 0; i < n; i++) if (!hs[i] || hs[i] == INVALID_HANDLE_VALUE) { t_err = ERROR_INVALID_HANDLE; return WAIT_FAILED; }
  if (ms != INFINITE) deadline(ms, &ts);
  pthread_mutex_lock(&g_hlock);
  for (;;) {
    if (all) {
      for (i = 0; i < n && ready(hs[i], 0); i++) {}
      if (i == n) { for (i = 0; i < n; i++) ready(hs[i], 1); pthread_mutex_unlock(&g_hlock); return WAIT_OBJECT_0; }
    } else {
      for (i = 0; i < n; i++) if (ready(hs[i], 1)) { pthread_mutex_unlock(&g_hlock); return WAIT_OBJECT_0 + i; }
    }
    if (ms == 0) break;
    if (ms == INFINITE) pthread_cond_wait(&g_hcond, &g_hlock);
    else if (pthread_cond_timedwait(&g_hcond, &g_hlock, &ts) == ETIMEDOUT) {
      /* one last look, then time out */
      if (all) { for (i = 0; i < n && ready(hs[i], 0); i++) {} if (i == n) continue; }
      else { for (i = 0; i < n; i++) if (ready(hs[i], 0)) break; if (i < n) continue; }
      break;
    }
  }
  pthread_mutex_unlock(&g_hlock);
  return WAIT_TIMEOUT;
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return WaitForMultipleObjects(1, &h, FALSE, ms); }

static void handle_unref(HANDLE h) {
  /* under g_hlock */
  if (--h->refs == 0) {
    pthread_mutex_unlock(&g_hlock);
    free(h);
    pthread_mutex_lock(&g_hlock);
  }
}
static void *thread_main(void *a) {
  HANDLE h = a;
  pthread_mutex_lock(&g_hlock);
  while (!h->started) pthread_cond_wait(&g_hcond, &g_hlock);
  pthread_mutex_unlock(&g_hlock);
  DWORD code = h->fn(h->arg);
  pthread_mutex_lock(&g_hlock);
  h->code = code; h->finished = 1;
  pthread_cond_broadcast(&g_hcond);
  handle_unref(h);
  pthread_mutex_unlock(&g_hlock);
  return NULL;
}
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD id) {
  (void)sa;
  HANDLE h = calloc(1, sizeof *h);
  pthread_attr_t at;
  if (!h) { t_err = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
  h->type = H_THREAD; h->fn = fn; h->arg = arg; h->fd = -1; h->refs = 2;
  h->started = !(flags & CREATE_SUSPENDED);
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  /* Windows' default stack is 1 MB and Samplosis's workers were sized for it; the secondary-thread default here is
     512 KB, so ask for at least 1 MB */
  size_t ss = stack > (1u << 20) ? stack : (1u << 20);
  pthread_attr_setstacksize(&at, page_round(ss));
  if (pthread_create(&h->th, &at, thread_main, h)) { pthread_attr_destroy(&at); free(h); t_err = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
  pthread_attr_destroy(&at);
  if (id) *id = 0;
  return h;
}
DWORD ResumeThread(HANDLE h) {
  if (!h || h->type != H_THREAD) return (DWORD)-1;
  pthread_mutex_lock(&g_hlock);
  DWORD was = h->started ? 0 : 1;
  h->started = 1; pthread_cond_broadcast(&g_hcond);
  pthread_mutex_unlock(&g_hlock);
  return was;
}
BOOL GetExitCodeThread(HANDLE h, LPDWORD code) {
  if (!h || h->type != H_THREAD) return FALSE;
  pthread_mutex_lock(&g_hlock); *code = h->finished ? h->code : 259 /* STILL_ACTIVE */; pthread_mutex_unlock(&g_hlock);
  return TRUE;
}
BOOL SetThreadPriority(HANDLE h, int prio) {
  /* background work gets a lower QoS on the calling thread; other threads' priorities are left as they are */
#ifdef __APPLE__
  if (h == &g_curthread) {
    pthread_set_qos_class_self_np(prio < 0 ? QOS_CLASS_UTILITY : prio > 0 ? QOS_CLASS_USER_INITIATED : QOS_CLASS_DEFAULT, 0);
  }
#else
  (void)h; (void)prio;
#endif
  return TRUE;
}
BOOL CancelSynchronousIo(HANDLE h) { (void)h; t_err = 1168; return FALSE; }   /* ERROR_NOT_FOUND: nothing to cancel */

static BOOL file_close(HANDLE h);
static BOOL find_close(HANDLE h);
BOOL CloseHandle(HANDLE h) {
  if (!h || h == INVALID_HANDLE_VALUE) { t_err = ERROR_INVALID_HANDLE; return FALSE; }
  switch (h->type) {
  case H_EVENT: free(h); return TRUE;
  case H_THREAD: pthread_mutex_lock(&g_hlock); handle_unref(h); pthread_mutex_unlock(&g_hlock); return TRUE;
  case H_FILE: return file_close(h);
  case H_FIND: return find_close(h);
  case H_CURTHREAD: return TRUE;
  }
  t_err = ERROR_INVALID_HANDLE;
  return FALSE;
}

/* ================================================================ locks */
void InitializeCriticalSection(CRITICAL_SECTION *cs) {
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);     /* critical sections can be entered again */
  pthread_mutex_init(&cs->m, &a);
  pthread_mutexattr_destroy(&a);
}
BOOL InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION *cs, DWORD spin) { (void)spin; InitializeCriticalSection(cs); return TRUE; }
void DeleteCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_destroy(&cs->m); }

static BOOL cond_wait(pthread_cond_t *c, pthread_mutex_t *m, DWORD ms) {
  if (ms == INFINITE) { pthread_cond_wait(c, m); return TRUE; }
  struct timespec ts; deadline(ms, &ts);
  if (pthread_cond_timedwait(c, m, &ts) == ETIMEDOUT) { t_err = ERROR_TIMEOUT; return FALSE; }
  return TRUE;
}
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *v, SRWLOCK *l, DWORD ms, ULONG flags) { (void)flags; return cond_wait(&v->c, &l->m, ms); }
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *v, CRITICAL_SECTION *cs, DWORD ms) { return cond_wait(&v->c, &cs->m, ms); }

BOOL InitOnceExecuteOnce(INIT_ONCE *o, PINIT_ONCE_FN fn, PVOID p, PVOID *ctx) {
  if (__atomic_load_n(&o->done, __ATOMIC_ACQUIRE)) return TRUE;
  pthread_mutex_lock(&o->m);
  BOOL ok = TRUE;
  if (!o->done) { ok = fn(o, p, ctx); if (ok) __atomic_store_n(&o->done, 1, __ATOMIC_RELEASE); }
  pthread_mutex_unlock(&o->m);
  return ok;
}

/* ================================================================ thread pool */
struct SmpTpWork {
  PTP_WORK_CALLBACK fn; void *arg; PTP_POOL pool;
  int queued, running;                   /* submissions waiting, callbacks running */
  struct SmpTpWork *next_q;              /* in the pool's queue while queued > 0 */
};
struct SmpTpPool {
  pthread_mutex_t m; pthread_cond_t c, idle;
  struct SmpTpWork *head, *tail;
  int max, threads, busy, quit;
};
static void *pool_main(void *a) {
  PTP_POOL p = a;
  pthread_mutex_lock(&p->m);
  for (;;) {
    while (!p->quit && !p->head) pthread_cond_wait(&p->c, &p->m);
    if (p->quit && !p->head) break;
    PTP_WORK w = p->head;
    if (--w->queued == 0) { p->head = w->next_q; if (!p->head) p->tail = NULL; w->next_q = NULL; }
    else { p->head = w->next_q; if (!p->head) p->tail = NULL; w->next_q = NULL;   /* again at the back */
           if (p->tail) p->tail->next_q = w; else p->head = w; p->tail = w; }
    w->running++; p->busy++;
    pthread_mutex_unlock(&p->m);
    w->fn(NULL, w->arg, w);
    pthread_mutex_lock(&p->m);
    w->running--; p->busy--;
    pthread_cond_broadcast(&p->idle);
  }
  p->threads--;
  pthread_cond_broadcast(&p->idle);
  pthread_mutex_unlock(&p->m);
  return NULL;
}
PTP_POOL CreateThreadpool(PVOID reserved) {
  (void)reserved;
  PTP_POOL p = calloc(1, sizeof *p);
  if (!p) return NULL;
  pthread_mutex_init(&p->m, NULL); pthread_cond_init(&p->c, NULL); pthread_cond_init(&p->idle, NULL);
  p->max = (int)GetActiveProcessorCount(0);
  return p;
}
void SetThreadpoolThreadMaximum(PTP_POOL p, DWORD n) { pthread_mutex_lock(&p->m); p->max = n ? (int)n : 1; pthread_mutex_unlock(&p->m); }
BOOL SetThreadpoolThreadMinimum(PTP_POOL p, DWORD n) { (void)p; (void)n; return TRUE; }
void CloseThreadpool(PTP_POOL p) {
  if (!p) return;
  pthread_mutex_lock(&p->m);
  p->quit = 1; pthread_cond_broadcast(&p->c);
  while (p->threads) pthread_cond_wait(&p->idle, &p->m);
  pthread_mutex_unlock(&p->m);
  free(p);
}
static PTP_POOL g_default_pool;
static pthread_once_t g_default_once = PTHREAD_ONCE_INIT;
static void default_pool(void) { g_default_pool = CreateThreadpool(NULL); }
PTP_WORK CreateThreadpoolWork(PTP_WORK_CALLBACK fn, PVOID arg, PTP_CALLBACK_ENVIRON env) {
  PTP_WORK w = calloc(1, sizeof *w);
  if (!w) return NULL;
  w->fn = fn; w->arg = arg;
  if (env && env->Pool) w->pool = env->Pool;
  else { pthread_once(&g_default_once, default_pool); w->pool = g_default_pool; }
  return w;
}
void SubmitThreadpoolWork(PTP_WORK w) {
  PTP_POOL p = w->pool;
  pthread_mutex_lock(&p->m);
  if (w->queued++ == 0) { w->next_q = NULL; if (p->tail) p->tail->next_q = w; else p->head = w; p->tail = w; }
  if (p->threads < p->max && p->threads - p->busy <= 0) {
    pthread_t t; pthread_attr_t at;
    pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 8u << 20);           /* the stretchers like a deep stack */
    if (!pthread_create(&t, &at, pool_main, p)) p->threads++;
    pthread_attr_destroy(&at);
  }
  pthread_cond_signal(&p->c);
  pthread_mutex_unlock(&p->m);
}
void WaitForThreadpoolWorkCallbacks(PTP_WORK w, BOOL cancel_pending) {
  PTP_POOL p = w->pool;
  pthread_mutex_lock(&p->m);
  if (cancel_pending && w->queued) {
    PTP_WORK *pp = &p->head, prev = NULL;
    while (*pp && *pp != w) { prev = *pp; pp = &(*pp)->next_q; }
    if (*pp) { *pp = w->next_q; if (p->tail == w) p->tail = prev; w->next_q = NULL; }
    w->queued = 0;
  }
  while (w->running || w->queued) pthread_cond_wait(&p->idle, &p->m);
  pthread_mutex_unlock(&p->m);
}
void CloseThreadpoolWork(PTP_WORK w) { if (w) { WaitForThreadpoolWorkCallbacks(w, TRUE); free(w); } }

/* ================================================================ files */
HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
  (void)share; (void)sa; (void)tmpl;
  char *p = upath(path);
  int of = 0, fd;
  struct stat st;
  if (!p) return INVALID_HANDLE_VALUE;
  if ((access & GENERIC_READ) && (access & GENERIC_WRITE)) of = O_RDWR;
  else if (access & GENERIC_WRITE) of = O_WRONLY;
  else of = O_RDONLY;
  switch (disp) {
  case CREATE_NEW: of |= O_CREAT | O_EXCL; break;
  case CREATE_ALWAYS: of |= O_CREAT | O_TRUNC; break;
  case OPEN_ALWAYS: of |= O_CREAT; break;
  case TRUNCATE_EXISTING: of |= O_TRUNC; break;
  default: break;
  }
  if ((access & (GENERIC_READ | GENERIC_WRITE)) == 0) of = O_RDONLY;      /* attribute access only */
  fd = open(p, of | O_CLOEXEC, 0644);
  if (fd < 0) {
    int e = errno;
    /* Windows says "path not found" when a folder on the way is missing */
    if (e == ENOENT) {
      char *slash = strrchr(p, '/');
      if (slash && slash != p) { *slash = 0; if (stat(p, &st) != 0) e = ENOTDIR; *slash = '/'; }
    }
    if (e == EEXIST && disp == CREATE_NEW) { free(p); t_err = ERROR_FILE_EXISTS; return INVALID_HANDLE_VALUE; }
    free(p); set_errno_err(e); return INVALID_HANDLE_VALUE;
  }
  if (!fstat(fd, &st) && S_ISDIR(st.st_mode) && !(flags & FILE_FLAG_BACKUP_SEMANTICS)) {
    close(fd); free(p); t_err = ERROR_ACCESS_DENIED; return INVALID_HANDLE_VALUE;
  }
#ifdef __APPLE__
  if (flags & FILE_FLAG_RANDOM_ACCESS) fcntl(fd, F_RDAHEAD, 0);
#endif
  HANDLE h = calloc(1, sizeof *h);
  if (!h) { close(fd); free(p); t_err = ERROR_NOT_ENOUGH_MEMORY; return INVALID_HANDLE_VALUE; }
  h->type = H_FILE; h->fd = fd;
  if (flags & FILE_FLAG_DELETE_ON_CLOSE) { h->delete_on_close = 1; h->path = p; } else free(p);
  /* CREATE_ALWAYS and OPEN_ALWAYS report whether the file was there */
  t_err = 0;
  return h;
}
static BOOL file_close(HANDLE h) {
  int r = close(h->fd);
  if (h->delete_on_close && h->path) unlink(h->path);
  free(h->path);
  free(h);
  if (r) { set_errno_err(errno); return FALSE; }
  return TRUE;
}
static int is_file(HANDLE h) { if (!h || h == INVALID_HANDLE_VALUE || h->type != H_FILE) { t_err = ERROR_INVALID_HANDLE; return 0; } return 1; }

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED o) {
  ssize_t r;
  if (!is_file(h)) return FALSE;
  if (o) {
    off_t at = (off_t)(((uint64_t)o->OffsetHigh << 32) | o->Offset);
    do r = pread(h->fd, buf, n, at); while (r < 0 && errno == EINTR);
    if (r < 0) { set_errno_err(errno); o->Internal = t_err; o->InternalHigh = 0; if (got) *got = 0; return FALSE; }
    o->Internal = 0; o->InternalHigh = (ULONG_PTR)r;
    if (got) *got = (DWORD)r;
    if (o->hEvent) SetEvent(o->hEvent);
    if (r == 0 && n > 0) { t_err = ERROR_HANDLE_EOF; o->Internal = ERROR_HANDLE_EOF; return FALSE; }
    return TRUE;
  }
  do r = read(h->fd, buf, n); while (r < 0 && errno == EINTR);
  if (r < 0) { set_errno_err(errno); if (got) *got = 0; return FALSE; }
  if (got) *got = (DWORD)r;
  return TRUE;
}
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED o) {
  ssize_t r; size_t done = 0;
  if (!is_file(h)) return FALSE;
  while (done < n) {
    if (o) {
      off_t at = (off_t)((((uint64_t)o->OffsetHigh << 32) | o->Offset) + done);
      r = pwrite(h->fd, (const char *)buf + done, n - done, at);
    } else r = write(h->fd, (const char *)buf + done, n - done);
    if (r < 0) { if (errno == EINTR) continue; set_errno_err(errno); if (put) *put = (DWORD)done; return FALSE; }
    done += (size_t)r;
  }
  if (put) *put = (DWORD)done;
  if (o) { o->Internal = 0; o->InternalHigh = done; if (o->hEvent) SetEvent(o->hEvent); }
  return TRUE;
}
BOOL GetOverlappedResult(HANDLE h, LPOVERLAPPED o, LPDWORD got, BOOL wait) {
  (void)h; (void)wait;
  if (got) *got = (DWORD)o->InternalHigh;
  if (o->Internal) { t_err = (DWORD)o->Internal; return FALSE; }
  return TRUE;
}
BOOL CancelIoEx(HANDLE h, LPOVERLAPPED o) { (void)h; (void)o; t_err = 1168; return FALSE; }
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER to, PLARGE_INTEGER now, DWORD how) {
  if (!is_file(h)) return FALSE;
  off_t r = lseek(h->fd, (off_t)to.QuadPart, how == FILE_BEGIN ? SEEK_SET : how == FILE_CURRENT ? SEEK_CUR : SEEK_END);
  if (r < 0) { set_errno_err(errno); return FALSE; }
  if (now) now->QuadPart = r;
  return TRUE;
}
BOOL SetEndOfFile(HANDLE h) {
  if (!is_file(h)) return FALSE;
  off_t at = lseek(h->fd, 0, SEEK_CUR);
  if (at < 0 || ftruncate(h->fd, at)) { set_errno_err(errno); return FALSE; }
  return TRUE;
}
BOOL FlushFileBuffers(HANDLE h) { if (!is_file(h)) return FALSE; if (fsync(h->fd)) { set_errno_err(errno); return FALSE; } return TRUE; }
BOOL GetFileSizeEx(HANDLE h, PLARGE_INTEGER size) {
  struct stat st;
  if (!is_file(h)) return FALSE;
  if (fstat(h->fd, &st)) { set_errno_err(errno); return FALSE; }
  size->QuadPart = st.st_size;
  return TRUE;
}
#ifdef __APPLE__
#define ST_MTIM(st) (st).st_mtimespec
#define ST_ATIM(st) (st).st_atimespec
#define ST_BTIM(st) (st).st_birthtimespec
#else
#define ST_MTIM(st) (st).st_mtim
#define ST_ATIM(st) (st).st_atim
#define ST_BTIM(st) (st).st_ctim
#endif
BOOL GetFileTime(HANDLE h, LPFILETIME created, LPFILETIME accessed, LPFILETIME written) {
  struct stat st;
  if (!is_file(h)) return FALSE;
  if (fstat(h->fd, &st)) { set_errno_err(errno); return FALSE; }
  if (created) ts_to_ft(&ST_BTIM(st), created);
  if (accessed) ts_to_ft(&ST_ATIM(st), accessed);
  if (written) ts_to_ft(&ST_MTIM(st), written);
  return TRUE;
}
BOOL SetFileTime(HANDLE h, const FILETIME *created, const FILETIME *accessed, const FILETIME *written) {
  struct timespec ts[2] = { { 0, UTIME_OMIT }, { 0, UTIME_OMIT } };
  (void)created;
  if (!is_file(h)) return FALSE;
  if (accessed) ft_to_ts(accessed, &ts[0]);
  if (written) ft_to_ts(written, &ts[1]);
  if (futimens(h->fd, ts)) { set_errno_err(errno); return FALSE; }
  return TRUE;
}

static DWORD attrs_of(const struct stat *st, const char *name) {
  DWORD a = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
  if (S_ISLNK(st->st_mode)) a |= FILE_ATTRIBUTE_REPARSE_POINT;
  if (!(st->st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
  if (name && name[0] == '.' && strcmp(name, ".") && strcmp(name, "..")) a |= FILE_ATTRIBUTE_HIDDEN;
  return a ? a : FILE_ATTRIBUTE_NORMAL;
}
BOOL SetFileAttributesW(LPCWSTR path, DWORD attrs) {
  struct stat st;
  char *p = upath(path);
  if (!p) return FALSE;
  int r = stat(p, &st);
  if (!r) r = chmod(p, (attrs & FILE_ATTRIBUTE_READONLY) ? (st.st_mode & ~(mode_t)0222) : (st.st_mode | S_IWUSR));
  int e = errno;
  free(p);
  if (r) { set_errno_err(e); return FALSE; }
  return TRUE;
}
DWORD GetFileAttributesW(LPCWSTR path) {
  struct stat st;
  char *p = upath(path);
  if (!p) return INVALID_FILE_ATTRIBUTES;
  if (stat(p, &st)) { set_errno_err(errno); free(p); return INVALID_FILE_ATTRIBUTES; }
  const char *b = strrchr(p, '/');
  DWORD a = attrs_of(&st, b ? b + 1 : p);
  free(p);
  return a;
}
BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS lvl, LPVOID out) {
  struct stat st; WIN32_FILE_ATTRIBUTE_DATA *d = out;
  (void)lvl;
  char *p = upath(path);
  if (!p) return FALSE;
  if (stat(p, &st)) { set_errno_err(errno); free(p); return FALSE; }
  const char *b = strrchr(p, '/');
  d->dwFileAttributes = attrs_of(&st, b ? b + 1 : p);
  ts_to_ft(&ST_BTIM(st), &d->ftCreationTime); ts_to_ft(&ST_ATIM(st), &d->ftLastAccessTime); ts_to_ft(&ST_MTIM(st), &d->ftLastWriteTime);
  d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32); d->nFileSizeLow = (DWORD)st.st_size;
  free(p);
  return TRUE;
}

/* FindFirstFile: "folder\pattern" with * and ? wildcards, matched without regard to case as Windows does */
static BOOL find_fill(HANDLE h, LPWIN32_FIND_DATAW fd) {
  struct dirent *e;
  while ((e = readdir(h->f.d)) != NULL) {
    if (fnmatch(h->f.pat, e->d_name, FNM_CASEFOLD | FNM_NOESCAPE)) continue;
    size_t dl = strlen(h->f.dir), nl = strlen(e->d_name);
    char *full = malloc(dl + nl + 2);
    struct stat st;
    if (!full) { t_err = ERROR_NOT_ENOUGH_MEMORY; return FALSE; }
    memcpy(full, h->f.dir, dl); full[dl] = '/'; memcpy(full + dl + 1, e->d_name, nl + 1);
    memset(fd, 0, sizeof *fd);
    if (!stat(full, &st) || !lstat(full, &st)) {
      fd->dwFileAttributes = attrs_of(&st, e->d_name);
      ts_to_ft(&ST_BTIM(st), &fd->ftCreationTime); ts_to_ft(&ST_ATIM(st), &fd->ftLastAccessTime); ts_to_ft(&ST_MTIM(st), &fd->ftLastWriteTime);
      fd->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32); fd->nFileSizeLow = (DWORD)st.st_size;
    } else fd->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    free(full);
    if (MultiByteToWideChar(CP_UTF8, 0, e->d_name, -1, fd->cFileName, MAX_PATH) <= 0) continue;
    return TRUE;
  }
  t_err = ERROR_NO_MORE_FILES;
  return FALSE;
}
HANDLE FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW fd) {
  char *p = upath(pattern), *slash;
  if (!p) return INVALID_HANDLE_VALUE;
  HANDLE h = calloc(1, sizeof *h);
  if (!h) { free(p); t_err = ERROR_NOT_ENOUGH_MEMORY; return INVALID_HANDLE_VALUE; }
  h->type = H_FIND; h->fd = -1;
  slash = strrchr(p, '/');
  if (slash) { *slash = 0; h->f.dir = strdup(*p ? p : "/"); h->f.pat = strdup(slash + 1); }
  else { h->f.dir = strdup("."); h->f.pat = strdup(p); }
  free(p);
  if (!h->f.dir || !h->f.pat || !(h->f.d = opendir(h->f.dir))) {
    int e = errno;
    free(h->f.dir); free(h->f.pat); free(h);
    set_errno_err(e == ENOENT ? ENOTDIR : e);
    return INVALID_HANDLE_VALUE;
  }
  if (!strcmp(h->f.pat, "*.*")) h->f.pat[1] = 0;      /* Windows' "*.*" also matches names without a dot */
  if (!find_fill(h, fd)) { closedir(h->f.d); free(h->f.dir); free(h->f.pat); free(h); t_err = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE_VALUE; }
  return h;
}
HANDLE FindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS lvl, LPVOID fd, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
  (void)lvl; (void)op; (void)filter; (void)flags;
  return FindFirstFileW(pattern, fd);
}
BOOL FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW fd) {
  if (!h || h == INVALID_HANDLE_VALUE || h->type != H_FIND) { t_err = ERROR_INVALID_HANDLE; return FALSE; }
  return find_fill(h, fd);
}
static BOOL find_close(HANDLE h) { closedir(h->f.d); free(h->f.dir); free(h->f.pat); free(h); return TRUE; }
BOOL FindClose(HANDLE h) {
  if (!h || h == INVALID_HANDLE_VALUE || h->type != H_FIND) { t_err = ERROR_INVALID_HANDLE; return FALSE; }
  return find_close(h);
}

BOOL CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES sa) {
  (void)sa;
  char *p = upath(path);
  if (!p) return FALSE;
  size_t n = strlen(p);
  while (n > 1 && p[n - 1] == '/') p[--n] = 0;
  int r = mkdir(p, 0755);
  int e = errno;
  free(p);
  if (r) { set_errno_err(e == ENOENT ? ENOTDIR : e); return FALSE; }
  return TRUE;
}
BOOL RemoveDirectoryW(LPCWSTR path) { char *p = upath(path); if (!p) return FALSE; int r = rmdir(p); int e = errno; free(p); if (r) { set_errno_err(e); return FALSE; } return TRUE; }
BOOL DeleteFileW(LPCWSTR path) { char *p = upath(path); if (!p) return FALSE; int r = unlink(p); int e = errno; free(p); if (r) { set_errno_err(e); return FALSE; } return TRUE; }
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
  char *a = upath(from), *b = upath(to);
  struct stat st;
  int r = -1, e = EINVAL;
  if (a && b) {
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && !lstat(b, &st)) { e = EEXIST; r = -1; }
    else { r = rename(a, b); e = errno; }
  }
  free(a); free(b);
  if (r) { set_errno_err(e); return FALSE; }
  return TRUE;
}

/* the absolute form of a path, without looking at the disk: relative paths join the current folder, "." and ".."
   are resolved. It comes back with backslashes ("\Users\me\x.sfz"), the separator the shared code splits and joins
   paths on (a leading backslash reads as absolute there, as a share path would); every call that reaches the file
   system turns them back into slashes. */
DWORD GetFullPathNameW(LPCWSTR path, DWORD cap, LPWSTR out, LPWSTR *file_part) {
  char *p = upath(path), cwd[PATH_MAX];
  if (!p) return 0;
  size_t need = strlen(p) + PATH_MAX + 2;
  char *src = malloc(need), *res = malloc(need);
  if (!src || !res) { free(p); free(src); free(res); t_err = ERROR_NOT_ENOUGH_MEMORY; return 0; }
  if (p[0] == '/') strcpy(src, p);
  else { if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/"); snprintf(src, need, "%s/%s", cwd, p); }
  free(p);
  /* resolve segments */
  size_t rn = 0;
  char *seg = src;
  res[0] = 0;
  while (*seg) {
    while (*seg == '/') seg++;
    if (!*seg) break;
    char *end = strchr(seg, '/');
    size_t L = end ? (size_t)(end - seg) : strlen(seg);
    if (L == 1 && seg[0] == '.') {}
    else if (L == 2 && seg[0] == '.' && seg[1] == '.') { while (rn > 0 && res[rn - 1] != '/') rn--; if (rn > 0) rn--; res[rn] = 0; }
    else { res[rn++] = '/'; memcpy(res + rn, seg, L); rn += L; res[rn] = 0; }
    seg += L;
  }
  if (!rn) { res[0] = '/'; res[1] = 0; rn = 1; }
  /* keep a trailing separator the caller wrote */
  size_t sl = wcslen(path);
  if (sl && (path[sl - 1] == L'/' || path[sl - 1] == L'\\') && res[rn - 1] != '/') { res[rn++] = '/'; res[rn] = 0; }
  free(src);
  int wn = MultiByteToWideChar(CP_UTF8, 0, res, -1, NULL, 0);
  if (wn <= 0) { free(res); return 0; }
  if (!out || (DWORD)wn > cap) { free(res); return (DWORD)wn; }
  MultiByteToWideChar(CP_UTF8, 0, res, -1, out, (int)cap);
  free(res);
  for (wchar_t *c = out; *c; c++) if (*c == L'/') *c = L'\\';
  if (file_part) { wchar_t *s = wcsrchr(out, L'\\'); *file_part = s && s[1] ? s + 1 : NULL; }
  return (DWORD)wn - 1;
}

UINT GetDriveTypeW(LPCWSTR root) {
  char *p = upath(root);
  struct statfs sf;
  UINT t = DRIVE_FIXED;
  if (!p) return DRIVE_UNKNOWN;
  if (statfs(p, &sf)) t = DRIVE_NO_ROOT_DIR;
#ifdef __APPLE__
  else if (!(sf.f_flags & MNT_LOCAL)) t = DRIVE_REMOTE;
  else if (!strncmp(sf.f_mntonname, "/Volumes/", 9)) t = DRIVE_REMOVABLE;
#endif
  free(p);
  return t;
}
BOOL GetVolumePathNameW(LPCWSTR path, LPWSTR out, DWORD cap) {
  char *p = upath(path);
  struct statfs sf;
  if (!p) return FALSE;
  int r = statfs(p, &sf);
  free(p);
  if (r) { set_errno_err(errno); return FALSE; }
#ifdef __APPLE__
  return MultiByteToWideChar(CP_UTF8, 0, sf.f_mntonname, -1, out, (int)cap) > 0;
#else
  return MultiByteToWideChar(CP_UTF8, 0, "/", -1, out, (int)cap) > 0;
#endif
}
BOOL GetDiskFreeSpaceExW(LPCWSTR dir, PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER free_) {
  char *p = upath(dir);
  struct statfs sf;
  if (!p) return FALSE;
  int r = statfs(p, &sf);
  free(p);
  if (r) { set_errno_err(errno); return FALSE; }
  if (avail) avail->QuadPart = (ULONGLONG)sf.f_bavail * sf.f_bsize;
  if (total) total->QuadPart = (ULONGLONG)sf.f_blocks * sf.f_bsize;
  if (free_) free_->QuadPart = (ULONGLONG)sf.f_bfree * sf.f_bsize;
  return TRUE;
}

FILE *_wfopen(const wchar_t *path, const wchar_t *mode) {
  char m[16];
  char *p = upath(path);
  if (!p) return NULL;
  if (WideCharToMultiByte(CP_UTF8, 0, mode, -1, m, sizeof m, NULL, NULL) <= 0) { free(p); return NULL; }
  FILE *f = fopen(p, m);
  if (!f) set_errno_err(errno);
  free(p);
  return f;
}
#endif
