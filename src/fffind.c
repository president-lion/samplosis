/* fffind.c: where FFmpeg's ffmpeg.exe is. In this order: the path in Settings, when that file is there; ffmpeg.exe
   in a folder on PATH; the WinGet package folder, %LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg*\*\bin.
   FFmpeg is only ever run, never shipped. */
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "decode.h"

#ifdef _WIN32
#define FF_EXE L"ffmpeg.exe"
#define PATH_SEP L';'
#else
#define FF_EXE L"ffmpeg"
#define PATH_SEP L':'
#endif

static int is_file(const wchar_t *p) {
  DWORD a = GetFileAttributesW(p);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* folder\name into out when that file is there */
static int try_in(const wchar_t *folder, size_t flen, const wchar_t *name, wchar_t *out, int cap) {
  wchar_t p[1200];
  while (flen && (folder[flen - 1] == L'\\' || folder[flen - 1] == L'/')) flen--;
  if (!flen || flen + wcslen(name) + 2 > 1200) return 0;
#ifdef _WIN32
  swprintf(p, 1200, L"%.*ls\\%ls", (int)flen, folder, name);
#else
  swprintf(p, 1200, L"%.*ls/%ls", (int)flen, folder, name);
#endif
  if (!is_file(p) || wcslen(p) >= (size_t)cap) return 0;
  wcscpy(out, p);
  return 1;
}

static int on_path(wchar_t *out, int cap) {
  DWORD n = GetEnvironmentVariableW(L"PATH", NULL, 0);
  wchar_t *path, *s, *e; int found = 0;
  if (!n) return 0;
  path = malloc(sizeof(wchar_t) * n);
  if (!path) return 0;
  if (GetEnvironmentVariableW(L"PATH", path, n) < n) {
    for (s = path; *s && !found; s = *e ? e + 1 : e) {
      e = wcschr(s, PATH_SEP);
      if (!e) e = s + wcslen(s);
      if (*s == L'"' && e > s + 1 && e[-1] == L'"') found = try_in(s + 1, (size_t)(e - s - 2), FF_EXE, out, cap);
      else found = try_in(s, (size_t)(e - s), FF_EXE, out, cap);
    }
  }
  free(path);
  return found;
}

#ifdef _WIN32
static int in_winget(wchar_t *out, int cap) {
  wchar_t base[700], pat[800], pkg[1000], ver[1100];
  WIN32_FIND_DATAW a, b; HANDLE fa, fb; int found = 0;
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, 700);
  if (!n || n >= 700) return 0;
  swprintf(pat, 800, L"%ls\\Microsoft\\WinGet\\Packages\\Gyan.FFmpeg*", base);
  fa = FindFirstFileW(pat, &a);
  if (fa == INVALID_HANDLE_VALUE) return 0;
  do {
    if (!(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    swprintf(pkg, 1000, L"%ls\\Microsoft\\WinGet\\Packages\\%ls\\*", base, a.cFileName);
    fb = FindFirstFileW(pkg, &b);
    if (fb == INVALID_HANDLE_VALUE) continue;
    do {
      if (!(b.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || b.cFileName[0] == L'.') continue;
      swprintf(ver, 1100, L"%ls\\Microsoft\\WinGet\\Packages\\%ls\\%ls\\bin", base, a.cFileName, b.cFileName);
      found = try_in(ver, wcslen(ver), L"ffmpeg.exe", out, cap);
    } while (!found && FindNextFileW(fb, &b));
    FindClose(fb);
  } while (!found && FindNextFileW(fa, &a));
  FindClose(fa);
  return found;
}

int smp_ffmpeg_find_w(wchar_t *out, int cap) {
  SmpSettings s; wchar_t w[1100];
  smp_settings_get(&s);
  if (s.ffmpeg_path[0] && MultiByteToWideChar(CP_UTF8, 0, s.ffmpeg_path, -1, w, 1100) && is_file(w) &&
      wcslen(w) < (size_t)cap) {
    wcscpy(out, w);
    return 1;
  }
  return on_path(out, cap) || in_winget(out, cap);
}
#else
/* A plugin started by a host from the Dock doesn't get the shell's PATH, so the package managers' folders are
   looked in too: Homebrew (Apple silicon, then Intel) and MacPorts. */
static int in_package_dirs(wchar_t *out, int cap) {
  static const wchar_t *const dirs[] = {L"/opt/homebrew/bin", L"/usr/local/bin", L"/opt/local/bin"};
  for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++)
    if (try_in(dirs[i], wcslen(dirs[i]), FF_EXE, out, cap)) return 1;
  return 0;
}

int smp_ffmpeg_find_w(wchar_t *out, int cap) {
  SmpSettings s; wchar_t w[1100];
  smp_settings_get(&s);
  if (s.ffmpeg_path[0] && MultiByteToWideChar(CP_UTF8, 0, s.ffmpeg_path, -1, w, 1100) && is_file(w) &&
      wcslen(w) < (size_t)cap) {
    wcscpy(out, w);
    return 1;
  }
  return on_path(out, cap) || in_package_dirs(out, cap);
}

#endif

static volatile LONG g_searches;           /* tests: the Settings page's searches (smp_ffmpeg_find) */
SMP_API long smp_test_ffmpeg_searches(void) { return g_searches; }

int smp_ffmpeg_find(char *out, int cap) {
  wchar_t w[1100];
  InterlockedIncrement(&g_searches);
  if (cap > 0) out[0] = 0;
  if (!smp_ffmpeg_find_w(w, 1100)) return 0;
  return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL) > 0;
}
