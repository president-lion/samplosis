/* Update: the Settings page's Update Samplosis button. It asks GitHub for the latest release of
   president-lion/samplosis; when that release's version is newer than this one, it downloads the release's
   samplosis.clap beside the loaded one and swaps them. Windows lets a loaded DLL be renamed but not overwritten, so
   the loaded file becomes samplosis.clap.old and the download takes its name; the host uses it the next time it
   loads the plugin (restarting REAPER), and the next load deletes the .old file. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "samplosis.h"

#define LATEST_URL "https://api.github.com/repos/president-lion/samplosis/releases/latest"
#define ASSET "samplosis.clap"
#define MAX_JSON (4u << 20)
#define MAX_CLAP (256u << 20)
#define MIN_CLAP (64u << 10)

/* 1 when tag ("v0.2.0" or "0.2.0") names a newer version than mine, 0 when not, -1 when tag isn't a version */
SMP_API int smp_version_newer(const char *tag, const char *mine) {
  int a[3] = {0, 0, 0}, b[3] = {0, 0, 0}, i, n;
  if (!tag || !mine) return -1;
  if (*tag == 'v' || *tag == 'V') tag++;
  if (*mine == 'v' || *mine == 'V') mine++;
  n = sscanf(tag, "%d.%d.%d", &a[0], &a[1], &a[2]);
  if (n < 1 || a[0] < 0 || a[1] < 0 || a[2] < 0) return -1;
  sscanf(mine, "%d.%d.%d", &b[0], &b[1], &b[2]);
  for (i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
  return 0;
}

static wchar_t *widen(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = n > 0 ? malloc(n * sizeof *w) : NULL;
  if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}

/* GETs url (http or https, following redirects) into a new buffer of at most max bytes. 0 when it came back 200;
   otherwise a message in msg */
static int http_get(const char *url, char **out, size_t *len, size_t max, char *msg, int n) {
  URL_COMPONENTS u; wchar_t host[256], path[2048], extra[2048], *w = widen(url);
  HINTERNET s = NULL, c = NULL, r = NULL; DWORD code = 0, sz = sizeof code, got; int res = -1;
  char *buf = NULL; size_t used = 0, cap = 0;
  *out = NULL; *len = 0;
  memset(&u, 0, sizeof u); u.dwStructSize = sizeof u;
  u.lpszHostName = host; u.dwHostNameLength = 256; u.lpszUrlPath = path; u.dwUrlPathLength = 2048;
  u.lpszExtraInfo = extra; u.dwExtraInfoLength = 2048;
  if (!w || !WinHttpCrackUrl(w, 0, 0, &u)) { snprintf(msg, n, "Update failed: a bad address"); goto done; }
  if (u.dwExtraInfoLength && wcslen(path) + u.dwExtraInfoLength < 2048)   /* the query goes back after the path */
    wcsncat(path, u.lpszExtraInfo, u.dwExtraInfoLength);
  s = WinHttpOpen(L"Samplosis-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) s = WinHttpOpen(L"Samplosis-updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);         /* before Windows 8.1 */
  if (!s) { snprintf(msg, n, "Update failed: Windows' web access didn't start"); goto done; }
  WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);
  c = WinHttpConnect(s, host, u.nPort, 0);
  r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
  if (!r || !WinHttpAddRequestHeaders(r, L"Accept: application/vnd.github+json, */*", (DWORD)-1L,
                                      WINHTTP_ADDREQ_FLAG_ADD) ||
      !WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(r, NULL)) {
    snprintf(msg, n, "Update failed: couldn't reach %ls (no internet connection?)", host); goto done;
  }
  WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &code, &sz, WINHTTP_NO_HEADER_INDEX);
  if (code != 200) { snprintf(msg, n, "Update failed: %ls answered %lu", host, (unsigned long)code); goto done; }
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(r, &avail)) { snprintf(msg, n, "Update failed: the download broke off"); goto done; }
    if (!avail) break;
    if (used + avail > max) { snprintf(msg, n, "Update failed: the download is too big"); goto done; }
    if (used + avail + 1 > cap) {
      size_t nc = cap ? cap : 65536; char *nb;
      while (nc < used + avail + 1) nc *= 2;
      if (!(nb = realloc(buf, nc))) { snprintf(msg, n, "Update failed: out of memory"); goto done; }
      buf = nb; cap = nc;
    }
    if (!WinHttpReadData(r, buf + used, avail, &got)) { snprintf(msg, n, "Update failed: the download broke off"); goto done; }
    used += got;
  }
  if (!buf && !(buf = malloc(1))) goto done;
  buf[used] = 0;
  *out = buf; *len = used; buf = NULL; res = 0;
done:
  free(buf); free(w);
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  if (s) WinHttpCloseHandle(s);
  return res;
}

/* the string value after "key": in json, unescaped into out (n bytes); start: where to look from. Returns the
   position after it, or NULL */
static const char *json_str(const char *json, const char *key, char *out, int n) {
  char k[64]; const char *p, *q; int i = 0;
  snprintf(k, sizeof k, "\"%s\"", key);
  if (!(p = strstr(json, k))) return NULL;
  p += strlen(k);
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p++ != ':') return NULL;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p++ != '"') return NULL;
  for (q = p; *q && *q != '"'; q++) {
    char ch = *q;
    if (ch == '\\' && q[1]) { ch = *++q; if (ch == 'u') return NULL; }   /* \uXXXX: not in a tag or URL we use */
    if (i < n - 1) out[i++] = ch;
  }
  out[i] = 0;
  return *q ? q + 1 : NULL;
}

/* the release's samplosis.clap download address, into url */
static int find_asset(const char *json, char *url, int n) {
  const char *p = json; char v[2048];
  while ((p = json_str(p, "browser_download_url", v, sizeof v)) != NULL) {
    size_t l = strlen(v), a = strlen(ASSET);
    if (l > a && v[l - a - 1] == '/' && !_stricmp(v + l - a, ASSET)) { snprintf(url, n, "%s", v); return 1; }
  }
  return 0;
}

/* this DLL's own path */
static int self_path(wchar_t *p, DWORD n) {
  HMODULE me = NULL;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCWSTR)(void *)smp_version_newer, &me)) return 0;
  DWORD l = GetModuleFileNameW(me, p, n);
  return l > 0 && l < n;
}

/* writes data to target: through target.new, then the old file (loaded or not) moved aside to target.old */
static int install(const wchar_t *target, const char *data, size_t len, char *msg, int n) {
  wchar_t tmp[MAX_PATH + 16], old[MAX_PATH + 32]; HANDLE f; DWORD put = 0; int i;
  if (wcslen(target) >= MAX_PATH) { snprintf(msg, n, "Update failed: the plugin's path is too long"); return -1; }
  swprintf(tmp, MAX_PATH + 16, L"%ls.new", target);
  f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) {
    snprintf(msg, n, "Update failed: can't write beside %ls (no permission?)", target); return -1;
  }
  if (!WriteFile(f, data, (DWORD)len, &put, NULL) || put != len || !FlushFileBuffers(f)) {
    CloseHandle(f); DeleteFileW(tmp); snprintf(msg, n, "Update failed: couldn't write the new file (disk full?)");
    return -1;
  }
  CloseHandle(f);
  if (GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES) {
    for (i = 0; i < 100; i++) {                /* .old may still be loaded from an update before: the next name */
      if (i) swprintf(old, MAX_PATH + 32, L"%ls.old%d", target, i);
      else swprintf(old, MAX_PATH + 32, L"%ls.old", target);
      DeleteFileW(old);
      if (MoveFileExW(target, old, MOVEFILE_REPLACE_EXISTING)) break;
    }
    if (i == 100) { DeleteFileW(tmp); snprintf(msg, n, "Update failed: couldn't move the old samplosis.clap aside"); return -1; }
    if (!MoveFileExW(tmp, target, 0)) {
      MoveFileExW(old, target, 0);
      DeleteFileW(tmp); snprintf(msg, n, "Update failed: couldn't put the new samplosis.clap in place"); return -1;
    }
  } else if (!MoveFileExW(tmp, target, MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp); snprintf(msg, n, "Update failed: couldn't put the new samplosis.clap in place"); return -1;
  }
  return 0;
}

/* checks api_url (NULL: GitHub's latest release) and installs a newer samplosis.clap at target (UTF-8; NULL: this
   DLL's own file). 1: installed, 0: already the newest, -1: failed. msg says which, for the window */
SMP_API int smp_update_run(const char *api_url, const char *target, char *msg, int n) {
  char *json = NULL, *clap = NULL, tag[64], url[2048]; size_t jl, cl; int res = -1;
  wchar_t self[MAX_PATH + 1], *t = NULL;
  if (!msg || n < 1) return -1;
  msg[0] = 0;
  if (http_get(api_url ? api_url : LATEST_URL, &json, &jl, MAX_JSON, msg, n)) goto done;
  if (!json_str(json, "tag_name", tag, sizeof tag) || smp_version_newer(tag, smp_version()) < 0) {
    snprintf(msg, n, "Update failed: the latest release has no version"); goto done;
  }
  if (!smp_version_newer(tag, smp_version())) {
    snprintf(msg, n, "Samplosis %s is the newest", smp_version()); res = 0; goto done;
  }
  if (!find_asset(json, url, sizeof url)) { snprintf(msg, n, "Update failed: release %s has no samplosis.clap", tag); goto done; }
  if (http_get(url, &clap, &cl, MAX_CLAP, msg, n)) goto done;
  if (cl < MIN_CLAP || clap[0] != 'M' || clap[1] != 'Z') {
    snprintf(msg, n, "Update failed: the download isn't a plugin"); goto done;
  }
  if (target) t = widen(target);
  else if (self_path(self, MAX_PATH + 1)) t = _wcsdup(self);
  if (!t) { snprintf(msg, n, "Update failed: couldn't find where samplosis.clap is"); goto done; }
  if (install(t, clap, cl, msg, n)) goto done;
  snprintf(msg, n, "Installed Samplosis %s. Restart REAPER to use it", tag[0] == 'v' ? tag + 1 : tag);
  res = 1;
done:
  free(json); free(clap); free(t);
  return res;
}

/* deletes the samplosis.clap.old files an update left beside path (UTF-8; NULL: this DLL's). Those still loaded
   somewhere stay */
SMP_API void smp_update_cleanup(const char *path) {
  wchar_t self[MAX_PATH + 1], pat[MAX_PATH + 16], dir[MAX_PATH + 1], f[MAX_PATH * 2], *p = NULL, *sl;
  WIN32_FIND_DATAW d; HANDLE h;
  if (path) p = widen(path);
  else if (self_path(self, MAX_PATH + 1)) p = _wcsdup(self);
  if (!p || wcslen(p) >= MAX_PATH) { free(p); return; }
  swprintf(pat, MAX_PATH + 16, L"%ls.old*", p);
  wcscpy(dir, p);
  sl = wcsrchr(dir, L'\\'); if (!sl) sl = wcsrchr(dir, L'/');
  if (sl) sl[1] = 0; else dir[0] = 0;
  h = FindFirstFileW(pat, &d);
  if (h != INVALID_HANDLE_VALUE) {
    do { swprintf(f, MAX_PATH * 2, L"%ls%ls", dir, d.cFileName); DeleteFileW(f); } while (FindNextFileW(h, &d));
    FindClose(h);
  }
  free(p);
}

/* the window's side: one update at a time on a thread of its own */
static volatile LONG busy;                     /* 1 while a check runs */
static volatile LONG seq;                      /* goes up when one finishes */
static char result[600];
static CRITICAL_SECTION lock; static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_lock(PINIT_ONCE o, PVOID a, PVOID *c) { (void)o; (void)a; (void)c; InitializeCriticalSection(&lock); return TRUE; }

static DWORD WINAPI update_thread(void *arg) {
  HMODULE self = arg; char m[600], *url = NULL, *tgt = NULL; DWORD l;
  char ev[2048];
  l = GetEnvironmentVariableA("SAMPLOSIS_UPDATE_URL", ev, sizeof ev);    /* the tests' stand-in server */
  if (l && l < sizeof ev) url = _strdup(ev);
  l = GetEnvironmentVariableA("SAMPLOSIS_UPDATE_TARGET", ev, sizeof ev); /* and the file they let it replace */
  if (l && l < sizeof ev) tgt = _strdup(ev);
  smp_update_run(url, tgt, m, sizeof m);
  free(url); free(tgt);
  EnterCriticalSection(&lock); snprintf(result, sizeof result, "%s", m); LeaveCriticalSection(&lock);
  InterlockedIncrement(&seq);
  InterlockedExchange(&busy, 0);
  FreeLibraryAndExitThread(self, 0);           /* the DLL stays loaded until this thread is done with it */
}

/* starts a check unless one is running. 1 when started */
SMP_API int smp_update_start(void) {
  HMODULE self = NULL; HANDLE t;
  InitOnceExecuteOnce(&once, init_lock, NULL, NULL);
  if (InterlockedCompareExchange(&busy, 1, 0)) return 0;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)(void *)update_thread, &self)) {
    InterlockedExchange(&busy, 0); return 0;
  }
  t = CreateThread(NULL, 0, update_thread, self, 0, NULL);
  if (!t) { FreeLibrary(self); InterlockedExchange(&busy, 0); return 0; }
  CloseHandle(t);
  return 1;
}

/* what the window shows: 1 while checking; otherwise the last result (or "", none yet) into msg. *done goes up
   by one each time a check finishes */
SMP_API int smp_update_status(char *msg, int n, long *done) {
  InitOnceExecuteOnce(&once, init_lock, NULL, NULL);
  if (done) *done = seq;
  if (busy) return 1;
  EnterCriticalSection(&lock); snprintf(msg, n, "%s", result); LeaveCriticalSection(&lock);
  return 0;
}
