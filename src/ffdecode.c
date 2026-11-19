/* ffdecode.c: the FFmpeg stage of the decode chain, the last thing tried. A file that nothing built in, no library and
   not Windows reads is handed to the ffmpeg.exe the user installed (fffind.c finds it: the Settings path, PATH, the
   WinGet folder). FFmpeg is only ever run, never shipped.

   Two runs. ffprobe.exe (beside ffmpeg.exe) says the first sound stream's rate and channels, as JSON:
     ffprobe -v error -hide_banner -select_streams a:0 -show_entries stream=sample_rate,channels:format=duration
             -of json file:<path>
   A file it can't open, or with no sound stream, isn't FFmpeg's either (the chain's message stands). Then ffmpeg
   decodes that stream to 32-bit float at its own rate and channels, written to a pipe:
     ffmpeg -hide_banner -nostdin -v error -i file:<path> -map 0:a:0 -c:a pcm_f32le -f f32le -ac <ch> -ar <rate> pipe:1
   ("file:" so a name like "concat:x" is only ever a file name). A run that ends with an error gives FFmpeg's own
   reason: the last line it wrote to stderr, without its "[decoder @ 0x...]" prefix.

   Never hangs, never leaves a program behind. Each run is a child process made with CREATE_NO_WINDOW at below-normal
   priority, inheriting only its three handles: stdin is NUL, stdout and stderr are named pipes this side reads with
   overlapped I/O, so the wait can stop every 50 ms to ask cancel and check the stall timer (60 s with no output).
   On a cancel, the stall timer, or output past the 2 GiB ceiling, the child is killed and waited for. Every child is
   in a job object of its own that kills it when the job's handle closes, so a host that crashes or unloads the
   plugin mid-decode doesn't leave FFmpeg running. Paths are wide strings, quoted the way the C runtime reads them, so
   names with spaces and in any language work.

   Called from the main thread or worker threads (the sampler's loader), never from the audio thread. */
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "decode.h"
#include "ffdecode.h"

#ifdef _WIN32
#define SMP_FFPROBE_NAME L"ffprobe.exe"
#define SMP_FFPROBE_NAME8 "ffprobe.exe"
#define SMP_FFMPEG_NAME8 "ffmpeg.exe"
#else
#define SMP_FFPROBE_NAME L"ffprobe"
#define SMP_FFPROBE_NAME8 "ffprobe"
#define SMP_FFMPEG_NAME8 "ffmpeg"
#endif

static volatile LONG test_flags;               /* TESTS ONLY: SMP_FF_TEST_* */
void smp_ff_set_test_flags(long f) { InterlockedExchange(&test_flags, (LONG)f); }

#ifdef _WIN32
/* one argument onto a command line, quoted the way the C runtime reads it back: backslashes are doubled before a
   quote (the closing one too), and a quote is escaped. 0 when it doesn't fit. */
static int put_arg(wchar_t *cmd, size_t cap, size_t *at, const wchar_t *a) {
  size_t i, bs;
  if (*at + 2 * wcslen(a) + 4 >= cap) return 0;
  if (*at) cmd[(*at)++] = L' ';
  cmd[(*at)++] = L'"';
  for (i = 0;; i++) {
    for (bs = 0; a[i] == L'\\'; i++) bs++;
    if (!a[i]) { while (bs--) { cmd[(*at)++] = L'\\'; cmd[(*at)++] = L'\\'; } break; }
    if (a[i] == L'"') { bs = 2 * bs + 1; while (bs--) cmd[(*at)++] = L'\\'; }
    else while (bs--) cmd[(*at)++] = L'\\';
    cmd[(*at)++] = a[i];
  }
  cmd[(*at)++] = L'"';
  cmd[*at] = 0;
  return 1;
}
#else
/* POSIX: no command line to quote; the arguments are kept one after another, each ended by a 0, and the list ends
   with an empty one (run_tool splits them back into argv) */
static int put_arg(wchar_t *cmd, size_t cap, size_t *at, const wchar_t *a) {
  size_t n = wcslen(a);
  if (*at + n + 2 >= cap) return 0;
  wmemcpy(cmd + *at, a, n);
  *at += n;
  cmd[(*at)++] = 0;
  cmd[*at] = 0;
  return 1;
}
#endif
static int put_args(wchar_t *cmd, size_t cap, size_t *at, const wchar_t *const *a) {
  for (; *a; a++) if (!put_arg(cmd, cap, at, *a)) return 0;
  return 1;
}

/* what a run writes: stdout whole (up to max bytes), and the start of stderr */
typedef struct {
  char *buf; size_t n, cap, max;
  int too_long, no_memory;
  char errs[4096]; size_t en;
  DWORD code;
} Out;
static int out_put(Out *o, const char *b, DWORD k) {
  if (k > o->max || o->n + k > o->max) { o->too_long = 1; return 0; }
  if (o->n + k > o->cap) {
    size_t c = o->cap ? o->cap : 65536; char *y;
    while (c < o->n + k) c *= 2;
    if (c > o->max) c = o->max;
    if (!(y = realloc(o->buf, c))) { o->no_memory = 1; return 0; }
    o->buf = y; o->cap = c;
  }
  memcpy(o->buf + o->n, b, k);
  o->n += k;
  return 1;
}

#ifdef _WIN32
/* one of the child's output pipes, read with overlapped I/O */
#define RD_SIZE 65536
typedef struct { HANDLE h; OVERLAPPED ov; int pending, open; char b[RD_SIZE]; } Reader;
static void rd_next(Reader *r) {
  if (!r->open || r->pending) return;
  ResetEvent(r->ov.hEvent);
  if (ReadFile(r->h, r->b, RD_SIZE, NULL, &r->ov) || GetLastError() == ERROR_IO_PENDING) r->pending = 1;
  else r->open = 0;                            /* the child closed it (it is ending) */
}
/* a finished read: its byte count, or -1 when the pipe has closed */
static long rd_done(Reader *r) {
  DWORD got = 0;
  r->pending = 0;
  if (GetOverlappedResult(r->h, &r->ov, &got, FALSE)) return (long)got;
  r->open = 0;
  return -1;
}

/* a pipe the child writes into: the read end is ours (overlapped, not inherited), the write end is the child's */
static volatile LONG pipe_no;
static int make_pipe(HANDLE *rd, HANDLE *wr) {
  wchar_t name[160]; SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
  swprintf(name, 160, L"\\\\.\\pipe\\samplosis-ff-%lu-%ld-%llu", GetCurrentProcessId(), InterlockedIncrement(&pipe_no),
           (unsigned long long)GetTickCount64());
  *rd = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 1 << 20, 0, NULL);
  *wr = INVALID_HANDLE_VALUE;
  if (*rd == INVALID_HANDLE_VALUE) return 0;
  *wr = CreateFileW(name, GENERIC_WRITE, 0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (*wr == INVALID_HANDLE_VALUE) { CloseHandle(*rd); *rd = INVALID_HANDLE_VALUE; return 0; }
  return 1;
}
static void close_h(HANDLE *h) { if (*h && *h != INVALID_HANDLE_VALUE) CloseHandle(*h); *h = INVALID_HANDLE_VALUE; }

#endif

enum { RUN_DONE, RUN_CANCELLED, RUN_STALLED, RUN_REFUSED, RUN_NO_START };

#ifdef _WIN32

/* Runs exe with the command line cmd until it ends, or until cancel says so, it writes nothing to stdout for stall
   ms, or out refuses its output (then it is killed). Either way it has ended (and been waited for) on return. */
static int run_tool(const wchar_t *exe, wchar_t *cmd, Out *o, int (*cancel)(void *), void *arg, ULONGLONG stall) {
  HANDLE out_w = INVALID_HANDLE_VALUE, err_w = INVALID_HANDLE_VALUE, nul = INVALID_HANDLE_VALUE, job = NULL;
  HANDLE inherit[3];
  Reader *rd = calloc(2, sizeof *rd);         /* [0] stdout, [1] stderr */
  SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
  STARTUPINFOEXW si; PROCESS_INFORMATION pi; SIZE_T asz = 0;
  LPPROC_THREAD_ATTRIBUTE_LIST al = NULL;
  int res = RUN_NO_START, i, started = 0;
  ULONGLONG last, exited = 0;
  memset(&pi, 0, sizeof pi);
  if (!rd) return RUN_NO_START;
  rd[0].h = rd[1].h = INVALID_HANDLE_VALUE;
  if (!make_pipe(&rd[0].h, &out_w) || !make_pipe(&rd[1].h, &err_w)) goto out;
  nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
  if (nul == INVALID_HANDLE_VALUE) goto out;
  for (i = 0; i < 2; i++) {
    if (!(rd[i].ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL))) goto out;
    rd[i].open = 1;
  }
  /* only these three handles reach the child, whatever else the host has made inheritable meanwhile */
  inherit[0] = nul; inherit[1] = out_w; inherit[2] = err_w;
  InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
  if (!(al = malloc(asz)) || !InitializeProcThreadAttributeList(al, 1, 0, &asz)) { free(al); al = NULL; goto out; }
  if (!UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL, NULL)) goto out;
  memset(&si, 0, sizeof si);
  si.StartupInfo.cb = sizeof si;
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.StartupInfo.wShowWindow = SW_HIDE;
  si.StartupInfo.hStdInput = nul; si.StartupInfo.hStdOutput = out_w; si.StartupInfo.hStdError = err_w;
  si.lpAttributeList = al;
  /* the job kills the child when its handle closes: here, or when the process holding it dies */
  if ((job = CreateJobObjectW(NULL, NULL)) != NULL) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    memset(&li, 0, sizeof li);
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li)) { CloseHandle(job); job = NULL; }
  }
  if (!job) goto out;                            /* FFmpeg never runs outside a job that can kill it */
  if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE,
                      CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                      NULL, NULL, &si.StartupInfo, &pi)) goto out;
  started = 1;
  if (!AssignProcessToJobObject(job, pi.hProcess)) {   /* before it runs a single instruction, or not at all */
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    goto out;
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  close_h(&out_w); close_h(&err_w); close_h(&nul);       /* the child's ends are the child's alone now */
  res = RUN_DONE;
  last = GetTickCount64();
  rd_next(&rd[0]); rd_next(&rd[1]);
  for (;;) {
    HANDLE ev[2]; int ne = 0;
    for (i = 0; i < 2; i++) if (rd[i].pending) ev[ne++] = rd[i].ov.hEvent;
    if (!ne) break;                              /* both pipes closed: the child is ending */
    /* the child has ended but something it started still holds a pipe open: what is left in the pipes is read for
       200 ms, then it's done (not a 60 s stall) */
    if (!exited && WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) exited = GetTickCount64();
    if (exited && GetTickCount64() - exited > 200) break;
    WaitForMultipleObjects((DWORD)ne, ev, FALSE, exited ? 20 : 50);
    for (i = 0; i < 2 && res == RUN_DONE; i++) {
      long got;
      if (!rd[i].pending || WaitForSingleObject(rd[i].ov.hEvent, 0) != WAIT_OBJECT_0) continue;
      got = rd_done(&rd[i]);
      if (got > 0 && i == 0) {
        last = GetTickCount64();
        if (!out_put(o, rd[i].b, (DWORD)got)) res = RUN_REFUSED;
      } else if (got > 0) {                      /* stderr: its start is kept, the rest drained */
        size_t k = (size_t)got < sizeof o->errs - 1 - o->en ? (size_t)got : sizeof o->errs - 1 - o->en;
        memcpy(o->errs + o->en, rd[i].b, k); o->en += k; o->errs[o->en] = 0;
      }
      rd_next(&rd[i]);
    }
    if (res != RUN_DONE) break;
    if (cancel && cancel(arg)) { res = RUN_CANCELLED; break; }
    if (GetTickCount64() - last > stall) { res = RUN_STALLED; break; }
  }
  if (res != RUN_DONE) {                         /* stopped: the child goes now */
    if (job) TerminateJobObject(job, 1);
    TerminateProcess(pi.hProcess, 1);
  }
  for (i = 0; i < 2; i++)                        /* no read may still be using the buffers when they're freed */
    if (rd[i].pending) { DWORD got; CancelIoEx(rd[i].h, &rd[i].ov); GetOverlappedResult(rd[i].h, &rd[i].ov, &got, TRUE); rd[i].pending = 0; }
  if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_TIMEOUT) {   /* it closed its output but won't end */
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
  }
  GetExitCodeProcess(pi.hProcess, &o->code);
out:
  if (started) CloseHandle(pi.hProcess);
  if (job) CloseHandle(job);                     /* kills anything of the child's still in it */
  if (al) DeleteProcThreadAttributeList(al);
  free(al);
  close_h(&out_w); close_h(&err_w); close_h(&nul);
  for (i = 0; i < 2; i++) { close_h(&rd[i].h); if (rd[i].ov.hEvent) CloseHandle(rd[i].ov.hEvent); }
  free(rd);
  return res;
}

#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
extern char **environ;

/* POSIX: the same contract as on Windows. The child gets /dev/null on stdin and two pipes, and nothing else (on macOS
   POSIX_SPAWN_CLOEXEC_DEFAULT closes every other descriptor); it runs in a process group of its own at a lower
   priority, so stopping it kills whatever it started too. The output is read with poll, every 50 ms asking cancel
   and checking the stall timer. */
static int run_tool(const wchar_t *exe, wchar_t *cmd, Out *o, int (*cancel)(void *), void *arg, ULONGLONG stall) {
  char *argv[64], exe8[4096];
  int nargs = 0, res = RUN_NO_START, i, outp[2] = {-1, -1}, errp[2] = {-1, -1}, status = 0;
  pid_t pid = -1;
  posix_spawn_file_actions_t fa; posix_spawnattr_t at;
  int have_fa = 0, have_at = 0;
  char *buf = NULL;
  ULONGLONG last, exited = 0;
  if (!smp_path_utf8(exe, exe8, sizeof exe8)) return RUN_NO_START;
  for (wchar_t *a = cmd; *a && nargs < 63; a += wcslen(a) + 1) {
    size_t cap = wcslen(a) * 4 + 1;
    if (!(argv[nargs] = malloc(cap)) || WideCharToMultiByte(CP_UTF8, 0, a, -1, argv[nargs], (int)cap, NULL, NULL) <= 0) goto out;
    nargs++;
  }
  argv[nargs] = NULL;
  if (!nargs || !(buf = malloc(65536)) || pipe(outp) || pipe(errp)) goto out;
  for (i = 0; i < 2; i++) { fcntl(i ? errp[0] : outp[0], F_SETFL, O_NONBLOCK); fcntl(i ? errp[0] : outp[0], F_SETFD, FD_CLOEXEC); }
  if (posix_spawn_file_actions_init(&fa)) goto out;
  have_fa = 1;
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, outp[1], 1);
  posix_spawn_file_actions_adddup2(&fa, errp[1], 2);
  if (posix_spawnattr_init(&at)) goto out;
  have_at = 1;
  short fl = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  fl |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
  posix_spawnattr_setflags(&at, fl);
  posix_spawnattr_setpgroup(&at, 0);
  { sigset_t none, def; sigemptyset(&none); sigemptyset(&def); sigaddset(&def, SIGPIPE);
    posix_spawnattr_setsigmask(&at, &none); posix_spawnattr_setsigdefault(&at, &def); }
  argv[0] = exe8;                                /* (the list's first is the program, as on Windows) */
  if (posix_spawn(&pid, exe8, &fa, &at, argv, environ)) { pid = -1; argv[0] = NULL; goto out; }
  argv[0] = NULL;
  setpriority(PRIO_PROCESS, (id_t)pid, 10);      /* below normal, as on Windows */
  close(outp[1]); outp[1] = -1; close(errp[1]); errp[1] = -1;
  res = RUN_DONE;
  last = GetTickCount64();
  for (;;) {
    struct pollfd pf[2]; int np = 0;
    if (outp[0] >= 0) { pf[np].fd = outp[0]; pf[np].events = POLLIN; np++; }
    if (errp[0] >= 0) { pf[np].fd = errp[0]; pf[np].events = POLLIN; np++; }
    if (!np) break;                              /* both pipes closed: the child is ending */
    if (!exited && waitpid(pid, &status, WNOHANG) == pid) { exited = GetTickCount64(); pid = -pid - 1; }
    if (exited && GetTickCount64() - exited > 200) break;
    poll(pf, (nfds_t)np, exited ? 20 : 50);
    for (i = 0; i < np && res == RUN_DONE; i++) {
      if (!(pf[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      int is_out = pf[i].fd == outp[0];
      ssize_t got = read(pf[i].fd, buf, 65536);
      if (got > 0 && is_out) {
        last = GetTickCount64();
        if (!out_put(o, buf, (DWORD)got)) res = RUN_REFUSED;
      } else if (got > 0) {                      /* stderr: its start is kept, the rest drained */
        size_t k = (size_t)got < sizeof o->errs - 1 - o->en ? (size_t)got : sizeof o->errs - 1 - o->en;
        memcpy(o->errs + o->en, buf, k); o->en += k; o->errs[o->en] = 0;
      } else if (got == 0 || (errno != EAGAIN && errno != EINTR)) {
        if (is_out) { close(outp[0]); outp[0] = -1; } else { close(errp[0]); errp[0] = -1; }
      }
    }
    if (res != RUN_DONE) break;
    if (cancel && cancel(arg)) { res = RUN_CANCELLED; break; }
    if (GetTickCount64() - last > stall) { res = RUN_STALLED; break; }
  }
  if (pid > 0) {
    if (res != RUN_DONE) kill(-pid, SIGKILL);    /* stopped: the child and its group go now */
    ULONGLONG t0 = GetTickCount64();
    while (waitpid(pid, &status, WNOHANG) == 0) {
      if (GetTickCount64() - t0 > 10000) { kill(-pid, SIGKILL); waitpid(pid, &status, 0); break; }   /* won't end */
      Sleep(10);
    }
  }
  o->code = WIFEXITED(status) ? (DWORD)WEXITSTATUS(status) : 1;
out:
  if (pid == -1 && res == RUN_DONE) res = RUN_NO_START;
  if (have_fa) posix_spawn_file_actions_destroy(&fa);
  if (have_at) posix_spawnattr_destroy(&at);
  for (i = 0; i < 2; i++) { if (outp[i] >= 0) close(outp[i]); if (errp[i] >= 0) close(errp[i]); }
  for (i = 1; i < nargs; i++) free(argv[i]);
  free(buf);
  return res;
}
#endif

/* FFmpeg's reason: the last line it wrote to stderr, without "[decoder @ 0x...] " prefixes, or fallback */
static void ff_reason(const Out *o, const char *fallback, char *err, int cap) {
  char t[4096], *line = NULL, *p, *e;
  memcpy(t, o->errs, o->en); t[o->en] = 0;
  for (p = t; *p; p = e) {                       /* the last line with something in it */
    char *s = p, *end;
    e = strchr(p, '\n');
    end = e ? e : p + strlen(p);
    if (e) e++; else e = end;
    while (end > s && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n')) end--;
    while (s < end && (*s == ' ' || *s == '\t')) s++;
    if (end > s) { *end = 0; line = s; }
  }
  while (line && line[0] == '[' && (p = strstr(line, "] ")) != NULL) line = p + 2;
  snprintf(err, (size_t)cap, "%s", line && *line ? line : fallback);
}

/* a number after "key": in the JSON text (quoted or not), or -1 */
static double json_num(const char *t, const char *key) {
  const char *p = strstr(t, key);
  if (!p) return -1;
  p += strlen(key);
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':' || *p == '"') p++;
  if (!(*p >= '0' && *p <= '9')) return -1;
  return strtod(p, NULL);
}

static int stopped(int r, char *err, int cap) {
  snprintf(err, (size_t)cap, "%s", r == RUN_CANCELLED ? "cancelled" : "FFmpeg took too long to decode it");
  return SMP_FF_STOPPED;
}
static int too_long_path(char *err, int cap) {
  snprintf(err, (size_t)cap, "the file's path is too long for FFmpeg");
  return SMP_FF_FAILED;
}

int smp_ff_decode(const char *path, size_t max_floats, float **x, size_t *floats, unsigned *ch, unsigned *sr,
                  int (*cancel)(void *), void (*stage)(void *), void *arg, char *err, int cap) {
  enum { CMD = 8192 };
  wchar_t ff[1100], probe[1100], *slash, *fpath = NULL, *cmd = NULL, num_ch[16], num_sr[16];
  long flags = InterlockedCompareExchange(&test_flags, 0, 0);
  ULONGLONG stall = flags & SMP_FF_TEST_STALL_SHORT ? 500 : 60000;
  Out po, o;
  WIN32_FILE_ATTRIBUTE_DATA fa;
  double rate, chans, dur;
  int n, r, res = SMP_FF_NOT_MINE;
  size_t at;
  *x = NULL; *floats = 0; *ch = 0; *sr = 0;
  memset(&po, 0, sizeof po); memset(&o, 0, sizeof o);
  if ((flags & SMP_FF_TEST_MISSING) || !smp_ffmpeg_find_w(ff, 1100)) return SMP_FF_MISSING;
  wcscpy(probe, ff);
  slash = wcsrchr(probe, L'\\');
  { wchar_t *s2 = wcsrchr(probe, L'/'); if (s2 && (!slash || s2 > slash)) slash = s2; }
  if (!slash || (size_t)(slash - probe) + 13 > 1100) return SMP_FF_MISSING;
  wcscpy(slash + 1, SMP_FFPROBE_NAME);
  if (GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES) {
    snprintf(err, (size_t)cap, "FFmpeg's " SMP_FFPROBE_NAME8 " isn't beside " SMP_FFMPEG_NAME8 ", and it's needed");
    return SMP_FF_FAILED;
  }
  n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  if (n <= 0 || !(fpath = malloc(sizeof(wchar_t) * ((size_t)n + 5))) || !(cmd = malloc(sizeof(wchar_t) * CMD))) {
    free(fpath);
    return SMP_FF_NOT_MINE;
  }
  wcscpy(fpath, L"file:");
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, fpath + 5, n);
  if (stage) stage(arg);
  if (flags & SMP_FF_TEST_SLOW_START) {          /* TESTS ONLY: FFmpeg held back 1.5 s */
    ULONGLONG began = GetTickCount64();
    while (GetTickCount64() - began < 1500) {
      if (cancel && cancel(arg)) { res = stopped(RUN_CANCELLED, err, cap); goto done; }
      Sleep(20);
    }
  }
  /* the first sound stream's rate and channels */
  {
    const wchar_t *a[] = {probe, L"-v", L"error", L"-hide_banner", L"-select_streams", L"a:0", L"-show_entries",
                          L"stream=sample_rate,channels:format=duration", L"-of", L"json", fpath, NULL};
    at = 0;
    if (!put_args(cmd, CMD, &at, a)) { res = too_long_path(err, cap); goto done; }
  }
  po.max = 1 << 18;
  r = run_tool(probe, cmd, &po, cancel, arg, stall);
  if (r == RUN_CANCELLED || r == RUN_STALLED) { res = stopped(r, err, cap); goto done; }
  if (r == RUN_NO_START) { snprintf(err, (size_t)cap, "FFmpeg couldn't be started"); res = SMP_FF_FAILED; goto done; }
  if (r != RUN_DONE || po.code != 0 || !po.buf) goto done;   /* FFmpeg can't open it either */
  if (!out_put(&po, "", 1)) goto done;           /* the JSON as a string */
  {
    const char *streams = strstr(po.buf, "\"streams\""), *fmt = strstr(po.buf, "\"format\"");
    if (!streams) goto done;
    rate = json_num(streams, "\"sample_rate\""); chans = json_num(streams, "\"channels\"");
    dur = fmt ? json_num(fmt, "\"duration\"") : -1;
  }
  if (rate < 0 || chans < 0) goto done;          /* no sound stream */
  if (!(chans >= 1 && chans <= 64 && rate >= 1 && rate <= 3000000)) {
    snprintf(err, (size_t)cap, "FFmpeg says its sound has %.0f channels at %.0f Hz, which Samplosis can't use", chans, rate);
    res = SMP_FF_FAILED;
    goto done;
  }
  /* the decode */
  swprintf(num_ch, 16, L"%d", (int)chans); swprintf(num_sr, 16, L"%d", (int)rate);
  {
    const wchar_t *a[] = {ff, L"-hide_banner", L"-nostdin", L"-v", L"error", L"-i", fpath, L"-map", L"0:a:0",
                          L"-c:a", L"pcm_f32le", L"-f", L"f32le", L"-ac", num_ch, L"-ar", num_sr, L"pipe:1", NULL};
    at = 0;
    if (!put_args(cmd, CMD, &at, a)) { res = too_long_path(err, cap); goto done; }
  }
  o.max = max_floats * sizeof(float);
  if (dur > 0 && GetFileAttributesExW(fpath + 5, GetFileExInfoStandard, &fa)) {
    /* room for what ffprobe says up front, but never more than 64 floats per byte of the file, nor the ceiling */
    double want = (dur * rate + 4096) * chans * sizeof(float);
    double most = (double)(((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) * 64 * sizeof(float);
    if (want > most) want = most;
    if (want > (double)o.max) want = (double)o.max;
    if (want > 256.0 * 1024 * 1024) want = 256.0 * 1024 * 1024;   /* past 256 MB it grows by doubling, as it decodes */
    if (want > 0 && (o.buf = malloc((size_t)want)) != NULL) o.cap = (size_t)want;
  }
  r = run_tool(ff, cmd, &o, cancel, arg, stall);
  if (r == RUN_CANCELLED || r == RUN_STALLED) { res = stopped(r, err, cap); goto done; }
  if (r == RUN_NO_START) { snprintf(err, (size_t)cap, "FFmpeg couldn't be started"); res = SMP_FF_FAILED; goto done; }
  if (r == RUN_REFUSED) {
    if (o.no_memory) { snprintf(err, (size_t)cap, "out of memory"); res = SMP_FF_FAILED; }
    else res = SMP_FF_TOO_LONG;
    goto done;
  }
  if (o.code != 0) { ff_reason(&o, "FFmpeg couldn't decode it", err, cap); res = SMP_FF_FAILED; goto done; }
  at = o.n / (sizeof(float) * (size_t)chans);    /* whole frames */
  if (!at) { ff_reason(&o, "FFmpeg found no sound in it", err, cap); res = SMP_FF_FAILED; goto done; }
  *x = (float *)o.buf; o.buf = NULL;
  *floats = at * (size_t)chans; *ch = (unsigned)chans; *sr = (unsigned)rate;
  res = SMP_FF_OK;
done:
  free(po.buf); free(o.buf); free(fpath); free(cmd);
  return res;
}
