/* Settings file: "name value" per line (cache_dir, cache_limit_gb, stretcher signalsmith|rubberband,
   keep_tone on|off, declick on|off, ffmpeg_path, raw_rate, raw_channels 1|2, raw_format s16|s24|s32|f32|u8, raw_endian little|big).
   Reloaded when the file's last-write time changes. Words read in any case; a value that doesn't read keeps the
   default. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "samplosis.h"

static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION cs;
static SmpSettings cur;
static ULONGLONG cur_stamp; /* last-write time of the loaded file; 0 = file missing */
static int loaded;
static volatile LONG declick = 1;   /* cur's declick, for the audio thread */
int smp_declick_on(void) { return declick; }

static BOOL CALLBACK init_cs(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&cs); return TRUE; }

static void defaults(SmpSettings *s) {
  memset(s, 0, sizeof *s);
  strcpy(s->cache_dir, "D:\\samplosis\\cache");
  s->cache_limit_gb = 10;
  s->raw_rate = 44100; s->raw_channels = 2; s->raw_format = SMP_RAW_S16; s->raw_endian = SMP_RAW_LITTLE;
}

/* the sample rates a raw file can be read at: the Settings page's list */
static const int raw_rates[] = {8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000};
#define N_RAW_RATES ((int)(sizeof raw_rates / sizeof raw_rates[0]))
int smp_raw_rates(const int **list) { *list = raw_rates; return N_RAW_RATES; }
static int rate_ok(int r) { int i; for (i = 0; i < N_RAW_RATES; i++) if (raw_rates[i] == r) return 1; return 0; }
static const char *const raw_formats[] = {"s16", "s24", "s32", "f32", "u8"};

/* a struct filled by hand (or zeroed) saves as the defaults where its raw values don't make sense */
static void tidy(SmpSettings *s) {
  if (!rate_ok(s->raw_rate)) s->raw_rate = 44100;
  if (s->raw_channels != 1 && s->raw_channels != 2) s->raw_channels = 2;
  if (s->raw_format < 0 || s->raw_format > SMP_RAW_U8) s->raw_format = SMP_RAW_S16;
  if (s->raw_endian != SMP_RAW_BIG) s->raw_endian = SMP_RAW_LITTLE;
  if (s->no_declick) s->no_declick = 1;
}

static int path_w(wchar_t *out, int cap) {
  DWORD n = GetEnvironmentVariableW(L"SAMPLOSIS_SETTINGS", out, cap);
  if (n > 0 && n < (DWORD)cap) return 1;
  n = GetEnvironmentVariableW(L"APPDATA", out, cap - 32);
  if (n == 0 || n >= (DWORD)cap - 32) return 0;
  wcscat(out, L"\\Samplosis\\settings.txt");
  return 1;
}

static ULONGLONG stamp_of(const wchar_t *p) {
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExW(p, GetFileExInfoStandard, &d)) return 0;
  return ((ULONGLONG)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
}

static void parse(char *text, SmpSettings *s) {
  char *line = text;
  while (line && *line) {
    char *nl = strpbrk(line, "\r\n");
    char *next = NULL;
    if (nl) { next = nl + 1; *nl = 0; }
    char *sp = strchr(line, ' ');
    if (sp) {
      *sp++ = 0;
      while (*sp == ' ') sp++;
      { size_t n = strlen(sp); while (n && (sp[n - 1] == ' ' || sp[n - 1] == '	')) sp[--n] = 0; }   /* hand edits */
      if (!strcmp(line, "cache_dir") && *sp && strlen(sp) < sizeof s->cache_dir) strcpy(s->cache_dir, sp);
      else if (!strcmp(line, "cache_limit_gb")) { double v = atof(sp); if (v > 0) s->cache_limit_gb = v; }
      else if (!strcmp(line, "stretcher")) {
        if (!_stricmp(sp, "signalsmith")) s->stretcher = SMP_STRETCH_SIGNALSMITH;
        else if (!_stricmp(sp, "rubberband")) s->stretcher = SMP_STRETCH_RUBBERBAND;
      } else if (!strcmp(line, "keep_tone")) {
        if (!_stricmp(sp, "on")) s->keep_tone = 1;
        else if (!_stricmp(sp, "off")) s->keep_tone = 0;
      } else if (!strcmp(line, "declick")) {
        if (!_stricmp(sp, "on")) s->no_declick = 0;
        else if (!_stricmp(sp, "off")) s->no_declick = 1;
      } else if (!strcmp(line, "ffmpeg_path")) {
        if (strlen(sp) < sizeof s->ffmpeg_path) strcpy(s->ffmpeg_path, sp);
      } else if (!strcmp(line, "raw_rate")) {
        char *e; long v = strtol(sp, &e, 10);
        if (e != sp && !*e && rate_ok((int)v)) s->raw_rate = (int)v;
      } else if (!strcmp(line, "raw_channels")) {
        if (!strcmp(sp, "1") || !strcmp(sp, "2")) s->raw_channels = sp[0] - '0';
      } else if (!strcmp(line, "raw_format")) {
        int i;
        for (i = 0; i <= SMP_RAW_U8; i++) if (!_stricmp(sp, raw_formats[i])) s->raw_format = i;
      } else if (!strcmp(line, "raw_endian")) {
        if (!_stricmp(sp, "little")) s->raw_endian = SMP_RAW_LITTLE;
        else if (!_stricmp(sp, "big")) s->raw_endian = SMP_RAW_BIG;
      }
    }
    line = next;
    while (line && (*line == '\r' || *line == '\n')) line++;
  }
}

static void reload(void) {
  wchar_t p[1100];
  SmpSettings s;
  defaults(&s);
  if (!path_w(p, 1100)) { cur = s; loaded = 1; cur_stamp = 0; InterlockedExchange(&declick, 1); return; }
  ULONGLONG st = stamp_of(p);
  if (loaded && st == cur_stamp) return;
  if (st) {
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
      char buf[8192]; DWORD n = 0;
      if (ReadFile(h, buf, sizeof buf - 1, &n, NULL)) { buf[n] = 0; parse(buf, &s); }
      CloseHandle(h);
    }
  }
  cur = s; cur_stamp = st; loaded = 1;
  InterlockedExchange(&declick, !s.no_declick);
}

void smp_settings_get(SmpSettings *out) {
  InitOnceExecuteOnce(&once, init_cs, NULL, NULL);
  EnterCriticalSection(&cs);
  reload();
  *out = cur;
  LeaveCriticalSection(&cs);
}

static void make_dirs(wchar_t *p) { /* creates every folder above the file name */
  for (wchar_t *c = p + 1; *c; c++)
    if (*c == L'\\' || *c == L'/') { wchar_t t = *c; *c = 0; CreateDirectoryW(p, NULL); *c = t; }
}

int smp_settings_set(const SmpSettings *s) {
  wchar_t p[1100];
  char text[2600];
  int rc = -1;
  SmpSettings t = *s;
  InitOnceExecuteOnce(&once, init_cs, NULL, NULL);
  if (!path_w(p, 1100)) return -1;
  tidy(&t);
  t.cache_dir[sizeof t.cache_dir - 1] = 0; t.ffmpeg_path[sizeof t.ffmpeg_path - 1] = 0;
  int n = snprintf(text, sizeof text, "cache_dir %s\r\ncache_limit_gb %.10g\r\nstretcher %s\r\nkeep_tone %s\r\ndeclick %s\r\n"
                   "ffmpeg_path %s\r\nraw_rate %d\r\nraw_channels %d\r\nraw_format %s\r\nraw_endian %s\r\n",
                   t.cache_dir, t.cache_limit_gb, t.stretcher == SMP_STRETCH_RUBBERBAND ? "rubberband" : "signalsmith",
                   t.keep_tone ? "on" : "off", t.no_declick ? "off" : "on", t.ffmpeg_path, t.raw_rate, t.raw_channels, raw_formats[t.raw_format],
                   t.raw_endian == SMP_RAW_BIG ? "big" : "little");
  if (n <= 0 || n >= (int)sizeof text) return -1;
  EnterCriticalSection(&cs);
  make_dirs(p);
  HANDLE h = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD w = 0;
    if (WriteFile(h, text, n, &w, NULL) && (int)w == n) rc = 0;
    CloseHandle(h);
  }
  if (rc == 0) { loaded = 0; reload(); }
  LeaveCriticalSection(&cs);
  return rc;
}
