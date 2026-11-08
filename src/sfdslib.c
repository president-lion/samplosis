/* sfdslib.c: Decent Sampler bundles (.dslibrary): a zip of one or more .dspreset files and the samples they name.

   Opening one unpacks it once into the cache folder, then reads its presets with sfds.c (sf_parse_ds_many), one font
   with a preset per .dspreset, in the order of their names in the zip (ignoring case). The font's path stays the
   bundle's, so the registry, the state and the window name the bundle.

   The folder: <cache>\dslib\<016llx key>\, the key smp_key("dslib1|<full path, lower case>|<size>|<mtime>"), so a
   changed bundle unpacks into a new folder (the old one is trimmed in time). It is written as <key>.part and renamed
   when every entry is in, so a folder that exists is whole, and opening it again unpacks nothing (its files keep
   their times, so the SFZ probe cache still knows them); the .part name is the process's and thread's own, so two
   opens at once never write into one. Each open touches the folder's time, and the cache trims
   unpacked folders oldest first like its other files (cache.c), never one a font holds open (the key is pinned
   from here until sf_free).

   What is unpacked: the .dspreset files and the audio files (by extension); everything else is skipped (images,
   __MACOSX\ and ._ resource forks, peak files, Decent Sampler's own caches). An entry whose name climbs out of the
   folder (..), is absolute or names a drive refuses the whole bundle. Entries are stored or deflated (miniz, in
   src\codecs\miniz; Zip64 too); an encrypted entry, or one of another method, refuses the bundle. While unpacking,
   the open's progress counts the entries: [0] of minus [1] (negative while unpacking, so the status says
   "Unpacking" rather than "Loading"), and cancel is asked between entries and every
   4 MB. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#define MINIZ_NO_STDIO
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#include <miniz.h>
#include "sf2.h"

#define DL_NAME 1024                     /* an entry's name's bytes kept (longer ones refuse the bundle) */

int sf_is_dslib(const char *path, const uint8_t *head, size_t n) {
  const char *dot = path ? strrchr(path, '.') : NULL;
  if (dot && !_stricmp(dot, ".dslibrary")) return 1;
  return n >= 4 && head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4;
}

/* ---- reading the zip through a Windows handle (wide paths) ---- */

static size_t zread(void *op, mz_uint64 ofs, void *buf, size_t n) {
  HANDLE h = op;
  OVERLAPPED o;
  DWORD got = 0;
  if (n > 0x40000000) n = 0x40000000;
  memset(&o, 0, sizeof o);
  o.Offset = (DWORD)ofs; o.OffsetHigh = (DWORD)(ofs >> 32);
  if (!ReadFile(h, buf, (DWORD)n, &got, &o)) return 0;
  return got;
}

typedef struct {
  HANDLE h;
  int (*cancel)(void *); void *carg;
  uint64_t since;                        /* bytes written since cancel was last asked */
  int stopped;
} Out;

static size_t zwrite(void *op, mz_uint64 ofs, const void *buf, size_t n) {
  Out *o = op;
  DWORD w = 0;
  (void)ofs;
  if ((o->since += n) >= (4u << 20)) { o->since = 0; if (o->cancel && o->cancel(o->carg)) { o->stopped = 1; return 0; } }
  if (!WriteFile(o->h, buf, (DWORD)n, &w, NULL) || w != n) return 0;
  return n;
}

/* ---- names ---- */

static int ends_with(const char *s, const char *ext) {
  size_t a = strlen(s), b = strlen(ext);
  return a >= b && !_stricmp(s + a - b, ext);
}

static const char *base_of(const char *s) {
  const char *b = s, *c;
  for (c = s; *c; c++) if (*c == '/' || *c == '\\') b = c + 1;
  return b;
}

/* 1 when the entry is unpacked: a .dspreset or an audio file, not in __MACOSX and not a ._ fork */
static int wanted(const char *name) {
  static const char *const audio[] = {".wav", ".wave", ".aif", ".aiff", ".aifc", ".flac", ".ogg", ".oga", ".opus",
                                      ".mp3", ".wv", ".ape", ".m4a", ".mp4", ".caf", ".w64", ".rf64", ".wma"};
  const char *b = base_of(name);
  size_t i;
  if (!_strnicmp(name, "__MACOSX/", 9) || !_strnicmp(name, "__MACOSX\\", 9) || strstr(name, "/__MACOSX/") ||
      (b[0] == '.' && b[1] == '_'))
    return 0;
  if (ends_with(name, ".dspreset")) return 1;
  for (i = 0; i < sizeof audio / sizeof *audio; i++) if (ends_with(name, audio[i])) return 1;
  return 0;
}

/* 1 when the name stays inside the folder: not absolute, no drive, no .. part */
static int safe(const char *name) {
  const char *s = name;
  if (!name[0] || name[0] == '/' || name[0] == '\\' || strchr(name, ':')) return 0;
  while (*s) {
    const char *e = s;
    while (*e && *e != '/' && *e != '\\') e++;
    if (e - s == 2 && s[0] == '.' && s[1] == '.') return 0;
    s = *e ? e + 1 : e;
  }
  return 1;
}

/* the entry's name as a wide path below dir (slashes turned round); 0 when it doesn't fit */
static int entry_path(const wchar_t *dir, const char *name, wchar_t *out, int cap) {
  int n = (int)wcslen(dir), k, i;
  if (n + 2 >= cap) return 0;
  memcpy(out, dir, (size_t)n * sizeof *out);
  out[n++] = L'\\';
  k = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, out + n, cap - n);
  if (k <= 0) k = MultiByteToWideChar(CP_OEMCP, 0, name, -1, out + n, cap - n);   /* an old zip's DOS names */
  if (k <= 0) return 0;
  for (i = n; out[i]; i++) if (out[i] == L'/') out[i] = L'\\';
  return 1;
}

/* the folders above file p */
static void make_parents(wchar_t *p) {
  wchar_t *c;
  for (c = p + 3; *c; c++)
    if (*c == L'\\') { *c = 0; CreateDirectoryW(p, NULL); *c = L'\\'; }
}

typedef struct { mz_uint index; char name[DL_NAME]; } Ent;

static int by_name(const void *a, const void *b) { return _stricmp(((const Ent *)a)->name, ((const Ent *)b)->name); }

/* unpacks the wanted entries of the open zip into dir: 1, or 0 with why */
static int unpack(mz_zip_archive *z, const wchar_t *dir, Ent *ents, int nents, int (*cancel)(void *), void *carg,
                  volatile LONG *prog, char *why, int wcap) {
  int k;
  if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
    snprintf(why, (size_t)wcap, "the cache folder can't be written");
    return 0;
  }
  for (k = 0; k < nents; k++) {
    wchar_t fn[1400];
    mz_zip_archive_file_stat st;
    Out o;
    FILETIME ft;
    int ok;
    if (cancel && cancel(carg)) { snprintf(why, (size_t)wcap, "cancelled"); return 0; }
    if (!mz_zip_reader_file_stat(z, ents[k].index, &st) || !entry_path(dir, ents[k].name, fn, 1400)) {
      snprintf(why, (size_t)wcap, "its zip is damaged");
      return 0;
    }
    make_parents(fn);
    memset(&o, 0, sizeof o);
    o.cancel = cancel; o.carg = carg;
    o.h = CreateFileW(fn, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (o.h == INVALID_HANDLE_VALUE) { snprintf(why, (size_t)wcap, "the cache folder can't be written"); return 0; }
    ok = mz_zip_reader_extract_to_callback(z, ents[k].index, zwrite, &o, 0);
    if (ok) {                              /* the entry's own time, so the probe cache recognises it next time */
      ULONGLONG t = ((ULONGLONG)st.m_time + 11644473600ULL) * 10000000ULL;
      ft.dwLowDateTime = (DWORD)t; ft.dwHighDateTime = (DWORD)(t >> 32);
      SetFileTime(o.h, NULL, NULL, &ft);
    }
    CloseHandle(o.h);
    if (!ok) {
      if (o.stopped) snprintf(why, (size_t)wcap, "cancelled");
      else if (mz_zip_get_last_error(z) == MZ_ZIP_FILE_WRITE_FAILED) snprintf(why, (size_t)wcap, "the cache folder can't be written");
      else snprintf(why, (size_t)wcap, "its zip is damaged (%s)", base_of(ents[k].name));
      return 0;
    }
    if (prog) InterlockedIncrement(&prog[0]);
  }
  return 1;
}

int sf_parse_dslib(SmpFont *f, const char *path, int (*cancel)(void *), void *carg, char *why, int wcap) {
  wchar_t *full = sf_full_path(path), dir[1200], part[1300];
  HANDLE h = INVALID_HANDLE_VALUE;
  mz_zip_archive z;
  int zinit = 0, ok = 0, nents = 0, npre = 0, k;
  Ent *ents = NULL;
  char **pre = NULL;
  volatile LONG *prog = sf_get_open_progress();
  LARGE_INTEGER sz;
  uint64_t key;
  memset(&z, 0, sizeof z);
  if (!full) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
  /* the key: the bundle's full path (lower case), size and time */
  {
    char u[3 * 1100], desc[3 * 1100 + 64];
    wchar_t lw[1100];
    wcsncpy(lw, full, 1099); lw[1099] = 0;
    CharLowerW(lw);
    if (WideCharToMultiByte(CP_UTF8, 0, lw, -1, u, sizeof u, NULL, NULL) <= 0) { snprintf(why, (size_t)wcap, "its path is too long"); goto done; }
    snprintf(desc, sizeof desc, "dslib1|%s|%llu|%llu", u, (unsigned long long)f->size, (unsigned long long)f->mtime);
    key = smp_key(desc);
  }
  if (!smp_cache_ok(NULL, 0) || !smp_cache_dslib_path(key, dir, 1200)) {
    snprintf(why, (size_t)wcap, "the cache folder can't be written");
    goto done;
  }
  smp_cache_pin(key);                      /* from now until sf_free: never trimmed while this font may use it */
  f->dslib_key = key; f->dslib_pinned = 1;

  h = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, NULL);   /* synchronous: each read gives its own offset */
  if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &sz)) { snprintf(why, (size_t)wcap, "it can't be read"); goto done; }
  {
    uint8_t head[4] = {0};
    if (zread(h, 0, head, 4) != 4 || head[0] != 'P' || head[1] != 'K' || (head[2] != 3 && head[2] != 5)) {
      snprintf(why, (size_t)wcap, "it isn't a zip file");
      goto done;
    }
  }
  z.m_pRead = zread; z.m_pIO_opaque = h;
  if (!mz_zip_reader_init(&z, (mz_uint64)sz.QuadPart, 0)) { snprintf(why, (size_t)wcap, "its zip is damaged"); goto done; }
  zinit = 1;

  /* the entries to unpack, checked before anything is written */
  {
    mz_uint i, n = mz_zip_reader_get_num_files(&z);
    if (!(ents = calloc(n ? n : 1, sizeof *ents))) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
    for (i = 0; i < n; i++) {
      char nm[DL_NAME];
      mz_uint L = mz_zip_reader_get_filename(&z, i, nm, sizeof nm);
      if (mz_zip_reader_is_file_a_directory(&z, i)) continue;
      if (!L || L >= sizeof nm - 1) { snprintf(why, (size_t)wcap, "its zip is damaged"); goto done; }
      if (!wanted(nm)) continue;
      if (!safe(nm)) { snprintf(why, (size_t)wcap, "it holds an unsafe path (%.80s)", nm); goto done; }
      if (mz_zip_reader_is_file_encrypted(&z, i) || !mz_zip_reader_is_file_supported(&z, i)) {
        snprintf(why, (size_t)wcap, "it uses zip features this can't read (encryption or an unknown compression)");
        goto done;
      }
      ents[nents].index = i;
      memcpy(ents[nents].name, nm, L + 1);
      if (ends_with(nm, ".dspreset")) npre++;
      nents++;
    }
  }
  if (!npre) { snprintf(why, (size_t)wcap, "it has no .dspreset in it"); goto done; }
  qsort(ents, (size_t)nents, sizeof *ents, by_name);

  if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) {
    /* not unpacked yet: into <key>.part, then renamed (a .part left by a crash or a cancel is started again) */
    /* a .part of this thread's own: two opens of the bundle at once (two instances, two hosts) never share one */
    swprintf(part, 1300, L"%s.%lu.%lu.part", dir, GetCurrentProcessId(), GetCurrentThreadId());
    {
      wchar_t parent[1200], *bs;
      wcscpy(parent, dir);
      if ((bs = wcsrchr(parent, L'\\')) != NULL) { *bs = 0; CreateDirectoryW(parent, NULL); }
    }
    smp_cache_delete_tree(part);
    if (prog) { InterlockedExchange(&prog[0], 0); InterlockedExchange(&prog[1], -nents); }
    if (!unpack(&z, part, ents, nents, cancel, carg, prog, why, wcap)) { smp_cache_delete_tree(part); goto done; }
    if (!MoveFileExW(part, dir, 0)) {
      smp_cache_delete_tree(part);         /* another instance got there first: its folder is as good */
      if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) { snprintf(why, (size_t)wcap, "the cache folder can't be written"); goto done; }
    }
  } else {                                 /* there already: the folder's time says it was used now */
    HANDLE dh = CreateFileW(dir, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (dh != INVALID_HANDLE_VALUE) {
      FILETIME now; GetSystemTimeAsFileTime(&now);
      SetFileTime(dh, NULL, NULL, &now);
      CloseHandle(dh);
    }
  }
  if (prog) { InterlockedExchange(&prog[1], 0); InterlockedExchange(&prog[0], 0); }
  mz_zip_reader_end(&z); zinit = 0;
  CloseHandle(h); h = INVALID_HANDLE_VALUE;

  /* the presets, by their unpacked paths (UTF-8) */
  if (!(pre = calloc((size_t)npre, sizeof *pre))) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
  for (k = 0, npre = 0; k < nents; k++) {
    wchar_t fn[1400];
    char u[4200];
    if (!ends_with(ents[k].name, ".dspreset")) continue;
    if (!entry_path(dir, ents[k].name, fn, 1400) || WideCharToMultiByte(CP_UTF8, 0, fn, -1, u, sizeof u, NULL, NULL) <= 0 ||
        !(pre[npre] = _strdup(u))) {
      snprintf(why, (size_t)wcap, "out of memory");
      goto done;
    }
    npre++;
  }
  ok = sf_parse_ds_many(f, path, (const char *const *)pre, npre, cancel, carg, why, wcap);
done:
  if (prog && prog[1] < 0) { InterlockedExchange(&prog[1], 0); InterlockedExchange(&prog[0], 0); }
  if (zinit) mz_zip_reader_end(&z);
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  if (pre) { for (k = 0; k < npre; k++) free(pre[k]); free(pre); }
  free(ents); free(full);
  return ok;
}
