/* Settings file: "name value" per line. Reloaded when the file's last-write time changes. */
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

static BOOL CALLBACK init_cs(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&cs); return TRUE; }

static void defaults(SmpSettings *s) {
  memset(s, 0, sizeof *s);
  strcpy(s->cache_dir, "D:\\samplosis\\cache");
  s->cache_limit_gb = 10;
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
      if (!strcmp(line, "cache_dir") && *sp && strlen(sp) < sizeof s->cache_dir) strcpy(s->cache_dir, sp);
      else if (!strcmp(line, "cache_limit_gb")) { double v = atof(sp); if (v > 0) s->cache_limit_gb = v; }
    }
    line = next;
    while (line && (*line == '\r' || *line == '\n')) line++;
  }
}

static void reload(void) {
  wchar_t p[1100];
  SmpSettings s;
  defaults(&s);
  if (!path_w(p, 1100)) { cur = s; loaded = 1; cur_stamp = 0; return; }
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
  char text[1300];
  int rc = -1;
  InitOnceExecuteOnce(&once, init_cs, NULL, NULL);
  if (!path_w(p, 1100)) return -1;
  int n = snprintf(text, sizeof text, "cache_dir %s\r\ncache_limit_gb %.10g\r\n", s->cache_dir, s->cache_limit_gb);
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
