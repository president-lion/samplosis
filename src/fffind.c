/* fffind.c: where FFmpeg's ffmpeg.exe is. In this order: the path in Settings, when that file is there; ffmpeg.exe
   in a folder on PATH; the WinGet package folder, %LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg*\*\bin.
   FFmpeg is only ever run, never shipped. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "decode.h"

static int is_file(const wchar_t *p) {
  DWORD a = GetFileAttributesW(p);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* folder\name into out when that file is there */
static int try_in(const wchar_t *folder, size_t flen, const wchar_t *name, wchar_t *out, int cap) {
  wchar_t p[1200];
  while (flen && (folder[flen - 1] == L'\\' || folder[flen - 1] == L'/')) flen--;
  if (!flen || flen + wcslen(name) + 2 > 1200) return 0;
  swprintf(p, 1200, L"%.*s\\%s", (int)flen, folder, name);
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
      e = wcschr(s, L';');
      if (!e) e = s + wcslen(s);
      if (*s == L'"' && e > s + 1 && e[-1] == L'"') found = try_in(s + 1, (size_t)(e - s - 2), L"ffmpeg.exe", out, cap);
      else found = try_in(s, (size_t)(e - s), L"ffmpeg.exe", out, cap);
    }
  }
  free(path);
  return found;
}

static int in_winget(wchar_t *out, int cap) {
  wchar_t base[700], pat[800], pkg[1000], ver[1100];
  WIN32_FIND_DATAW a, b; HANDLE fa, fb; int found = 0;
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, 700);
  if (!n || n >= 700) return 0;
  swprintf(pat, 800, L"%s\\Microsoft\\WinGet\\Packages\\Gyan.FFmpeg*", base);
  fa = FindFirstFileW(pat, &a);
  if (fa == INVALID_HANDLE_VALUE) return 0;
  do {
    if (!(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    swprintf(pkg, 1000, L"%s\\Microsoft\\WinGet\\Packages\\%s\\*", base, a.cFileName);
    fb = FindFirstFileW(pkg, &b);
    if (fb == INVALID_HANDLE_VALUE) continue;
    do {
      if (!(b.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || b.cFileName[0] == L'.') continue;
      swprintf(ver, 1100, L"%s\\Microsoft\\WinGet\\Packages\\%s\\%s\\bin", base, a.cFileName, b.cFileName);
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

int smp_ffmpeg_find(char *out, int cap) {
  wchar_t w[1100];
  if (cap > 0) out[0] = 0;
  if (!smp_ffmpeg_find_w(w, 1100)) return 0;
  return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL) > 0;
}
