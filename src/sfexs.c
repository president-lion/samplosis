/* sfexs.c: Logic's EXS24 and Sampler instruments (.exs) read into the SoundFont model through sfsfz.c's builder. An
   .exs is one preset (bank 0, program 0, named after the file); each zone is a region (two for a stereo file), fed
   to the builder as SFZ opcodes, so sample files, probing, missing files, stereo pairs, start heads and the region
   conditions all work as they do for SFZ.

   The file. Apple never published it; the layout here is the one ConvertWithMoss (git-moss, LGPL; only its
   description of the format is used, no code) reads and writes. A file is a run of blocks, each an 84-byte header
   and its content:
   - header: byte 0 is 0 for big-endian numbers (older PowerPC files) and 1 for little-endian; byte 3 the type in its
     low four bits (0 instrument, 1 zone, 2 group, 3 sample, 4 parameters; others are read past), with 0x80 set by a
     big-endian variant that also sets bit 15 of the size, so the size is masked to 15 bits there; then the content
     size (4 bytes), an index, flags, a magic of "SOBT" or "SOBJ" (big-endian) or "TBOS" or "JBOS" (little), and a
     64-byte name. A block without the magic ends the reading: what came before it counts.
   - zone: options (bit 0 one-shot, bit 1 no pitch tracking, bit 2 reversed, bit 3 velocity range on, bit 6 output
     set), root key, fine tune (cents), pan (-50..50), volume (dB), volume scale, key range, velocity range, sample
     start and end and loop start and end (frames; the ends one past the last), loop crossfade (ms), loop tune, loop
     options (bit 0 on, bit 2 play to the end on release), loop direction (0 forward, 1 backward, 2 alternating),
     coarse tune (semitones), output, and the group and sample it uses, by their order in the file.
   - group: volume (dB) and pan added to each zone's, polyphony, options (bit 4 muted), an exclusive group, a
     velocity range, a random-select offset, release trigger and its decay, and in newer files a round-robin
     position (-1, 0, 1 ... for the 1st, 2nd, 3rd ...), an "enable by" kind (1 note, 2 round robin, 3 controller, 4
     pitch bend, 5 MIDI channel, 6 articulation, 7 tempo) and its values, and a key range.
   - sample: its rate, channels and length (the file's own header is trusted instead), the folder it was in on the
     Mac it was made on, and its file name (the block's name when the file name field isn't there).
   - parameters: a count, that many one-byte ids and as many 16-bit values; newer files add a count of 16-bit id and
     value pairs. The ones read: the amplitude envelope (0x16d delay, 0x52 attack, 0x58 hold, 0x54 decay, 0x51
     sustain 0..127, 0x55 release; a time t is 10 (t / 127)^4 seconds), its velocity sensitivity (0x5a, -60 is full),
     pitch bend up and down (0x03, 0x04 semitones; down -1 follows up), transpose (0x2d) and coarse tune (0x0e) in
     semitones, fine tune (0x0f, cents), random sample select (0xa3). Counted as ignored when set: the filter (0x2c),
     mono legato (0x0a), glide (0x14) and volume key scaling (0x08).
   Strings are UTF-8 when they are valid UTF-8 and Mac Roman otherwise.

   Finding the samples. An .exs names its samples by a Mac folder and a file name, which rarely exist as they are
   on Windows, so each is looked for: in the folder the previous sample was found in, beside the .exs, then by a
   search of the .exs's folder and its subfolders, then of each folder above it in turn (three at most, never a
   drive's root), stopping when every sample is found, or after 20000 folders or 3 seconds (a big drive's worth of
   folders would take minutes, every time the instrument opens). A name with a single quote also matches one with an
   underscore in its place (as copies made on other systems have), and one with no extension matches .aif, .aiff,
   .wav and .caf. Where two files share a name at the same depth, the one in a folder named like its Mac folder
   wins. A sample that isn't found is missing as the builder says ("Missing N samples (first: name)").

   Mapping. Each zone: pitch_keycenter, lokey and hikey, lovel and hivel (0..127 unless its velocity range is on),
   both narrowed to its group's ranges (a zone left with nothing is dropped); pitch_keytrack 0 for a zone without
   pitch tracking; transpose and tune from its coarse and fine tune plus the instrument's; volume and pan (SFZ
   percent, twice EXS's) with its group's added; offset and end; loop_continuous with loop_start and loop_end, or
   loop_sustain when it plays to the end on release, when its loop is on (a loop with no points loops the whole
   sample), else one_shot or no_loop (an .exs's own loop setting wins over the file's); trigger=release for a
   release-trigger group; group and off_by for an exclusive group; seq_length and seq_position for a round-robin
   group (the length is the highest position among them); lorand and hirand splitting the draw among zones with the
   same ranges when random select is on (the instrument's, or the group's offset); sw_last for a group enabled by a
   note (the switch range spans every such note, the lowest by group order being the default); loccN and hiccN for
   one enabled by a controller. Muted groups are left out. Counted as ignored: reversed zones, backward and
   alternating loops, loop crossfades and tuning, zone outputs, Flex, group polyphony, release trigger decay,
   velocity and key crossfades, the enable-by kinds the player can't follow (their groups play as if always on),
   and the parameters above. Damaged files never crash: every read is bounded by its block, the file is at most
   64 MB, and at most SF_MAX_REGIONS zones and 65536 samples and groups are read (more is counted). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "plat.h"
#include "sf2.h"

#define EXS_MAX_FILE (64u << 20)
#define EXS_MAX_LIST 65536
#define EXS_HDR 84
#define EXS_UP 3                                 /* folders above the .exs searched */
#define EXS_MAX_DIRS 20000                       /* folders a search may open, all levels together */
#define EXS_SEARCH_MS 3000                       /* and the time it may take */
#define EXS_DEPTH 16
#define WP (MAX_PATH * 4)

typedef struct {
  uint8_t opts, key, lo, hi, vlo, vhi, looptune, loopopts, loopdir, flex;
  int fine, pan, vol, coarse;
  uint32_t start, end, ls, le, xfade;
  int group, sample;
} Zone;

typedef struct {
  int vol, pan, poly, opts, excl, vmin, vmax, rnd, rt, rt_decay, vxf, kxf;
  int ext, rr_pos, by, ccnum, cclo, cchi, knlo, knhi, by_note;
} Group;

typedef struct {
  char name[256];                                /* the file name, UTF-8 */
  char hint[256];                                /* the last folder of its Mac path, UTF-8 */
  char *path;                                    /* where it was found (UTF-8), or NULL */
  int depth;                                     /* the search level it was found at (a closer one wins) */
  int hinted;                                    /* found in a folder named like hint */
} Sample;

/* one region as fed */
typedef struct {
  int z, lokey, hikey, lovel, hivel, vol, pan, transpose, tune, seq_len, seq_pos, rel, excl, sw, cc, cclo, cchi;
  float lorand, hirand;
  int random;
} Reg;

typedef struct {
  Zone *z; int nz, capz;
  Group *g; int ng, capg;
  Sample *s; int ns, caps;
  Reg *r; int nr, capr;
  int param[0x200]; uint8_t has[0x200];
  int zones_capped, lists_capped;
  int sw_lo, sw_hi, sw_def;
  /* what's ignored, as found */
  int reversed, loopdir, xfade, looptune, output, flex, poly, rtdecay, vxf, kxf, enable_other, filter, legato,
      glide, keyscale;
  int (*cancel)(void *); void *carg;
  /* the search */
  wchar_t dir[WP];                               /* the .exs's folder, with a trailing backslash */
  int left, dirs;
  ULONGLONG until;                              /* GetTickCount64 when the search stops */
  SfzBuild *b;
} X;

static uint32_t u32(const uint8_t *p, int be) {
  return be ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
            : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}
static int s16(const uint8_t *p, int be) { return (int16_t)(be ? p[0] << 8 | p[1] : p[1] << 8 | p[0]); }
static int u16(const uint8_t *p, int be) { return be ? p[0] << 8 | p[1] : p[1] << 8 | p[0]; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int grow(void **a, int *cap, int need, size_t each) {
  void *n; int c;
  if (need <= *cap) return 1;
  c = *cap ? *cap * 2 : 64;
  while (c < need) c *= 2;
  if (!(n = realloc(*a, (size_t)c * each))) return 0;
  *a = n; *cap = c;
  return 1;
}

static const uint16_t mac_roman[128] = {
  0xC4, 0xC5, 0xC7, 0xC9, 0xD1, 0xD6, 0xDC, 0xE1, 0xE0, 0xE2, 0xE4, 0xE3, 0xE5, 0xE7, 0xE9, 0xE8,
  0xEA, 0xEB, 0xED, 0xEC, 0xEE, 0xEF, 0xF1, 0xF3, 0xF2, 0xF4, 0xF6, 0xF5, 0xFA, 0xF9, 0xFB, 0xFC,
  0x2020, 0xB0, 0xA2, 0xA3, 0xA7, 0x2022, 0xB6, 0xDF, 0xAE, 0xA9, 0x2122, 0xB4, 0xA8, 0x2260, 0xC6, 0xD8,
  0x221E, 0xB1, 0x2264, 0x2265, 0xA5, 0xB5, 0x2202, 0x2211, 0x220F, 0x3C0, 0x222B, 0xAA, 0xBA, 0x3A9, 0xE6, 0xF8,
  0xBF, 0xA1, 0xAC, 0x221A, 0x192, 0x2248, 0x2206, 0xAB, 0xBB, 0x2026, 0xA0, 0xC0, 0xC3, 0xD5, 0x152, 0x153,
  0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0xF7, 0x25CA, 0xFF, 0x178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
  0x2021, 0xB7, 0x201A, 0x201E, 0x2030, 0xC2, 0xCA, 0xC1, 0xCB, 0xC8, 0xCD, 0xCE, 0xCF, 0xCC, 0xD3, 0xD4,
  0xF8FF, 0xD2, 0xDA, 0xDB, 0xD9, 0x131, 0x2C6, 0x2DC, 0xAF, 0x2D8, 0x2D9, 0x2DA, 0xB8, 0x2DD, 0x2DB, 0x2C7};

/* n bytes (to the first NUL) as UTF-8 into out: as they are when they're valid UTF-8, else read as Mac Roman.
   Control characters become spaces. */
static void text(const uint8_t *in, size_t n, char *out, size_t cap) {
  size_t L = 0, i, o = 0;
  wchar_t w[256];
  int k;
  while (L < n && in[L]) L++;
  if (L > 255) L = 255;
  k = L ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)in, (int)L, w, 255) : 0;
  if (!k) for (i = 0; i < L; i++) w[k++] = in[i] < 0x80 ? in[i] : mac_roman[in[i] - 0x80];
  for (i = 0; i < (size_t)k; i++) if (w[i] < 32) w[i] = L' ';
  o = k ? (size_t)WideCharToMultiByte(CP_UTF8, 0, w, k, out, (int)cap - 1, NULL, NULL) : 0;
  out[o < cap ? o : cap - 1] = 0;
}

/* ---- reading the blocks ---- */

static void read_zone(X *x, const uint8_t *c, uint32_t n, int be) {
  uint8_t b[96] = {0};
  Zone *z;
  if (x->nz >= SF_MAX_REGIONS) { x->zones_capped = 1; return; }
  if (!grow((void **)&x->z, &x->capz, x->nz + 1, sizeof *x->z)) { x->zones_capped = 1; return; }
  memcpy(b, c, n < sizeof b ? n : sizeof b);
  z = &x->z[x->nz];
  z->opts = b[0]; z->key = b[1]; z->fine = (int8_t)b[2]; z->pan = (int8_t)b[3]; z->vol = (int8_t)b[4];
  z->lo = b[6]; z->hi = b[7]; z->vlo = b[9]; z->vhi = b[10];
  z->start = u32(b + 12, be); z->end = u32(b + 16, be); z->ls = u32(b + 20, be); z->le = u32(b + 24, be);
  z->xfade = u32(b + 28, be); z->looptune = b[32]; z->loopopts = b[33]; z->loopdir = b[34];
  z->flex = b[77]; z->coarse = (int8_t)b[80];
  if (!(z->opts & 0x40)) b[82] = 0;
  if (b[82]) x->output = 1;
  /* a short zone has no group or sample: the sample with its own number, as ConvertWithMoss reads it */
  z->group = n >= 92 ? (int32_t)u32(b + 88, be) : -1;
  z->sample = n >= 96 ? (int32_t)u32(b + 92, be) : x->nz;
  if (z->sample < 0) z->sample = x->nz;
  x->nz++;
}

static void read_group(X *x, const uint8_t *c, uint32_t n, int be) {
  uint8_t b[92] = {0};
  Group *g;
  if (x->ng >= EXS_MAX_LIST) { x->lists_capped = 1; return; }
  if (!grow((void **)&x->g, &x->capg, x->ng + 1, sizeof *x->g)) { x->lists_capped = 1; return; }
  memcpy(b, c, n < sizeof b ? n : sizeof b);
  g = &x->g[x->ng++];
  g->vol = (int8_t)b[0]; g->pan = (int8_t)b[1]; g->poly = b[2]; g->opts = b[3]; g->excl = b[4];
  g->vmin = b[5]; g->vmax = b[6] ? b[6] : 127; g->rnd = b[7];
  g->rt_decay = (g->opts & 64) && u16(b + 16, be);
  g->vxf = b[32] && b[32] != 128; g->kxf = b[35] && b[35] != 128;
  g->rt = n >= 74 && b[73];
  g->by_note = b[75];
  g->ext = n >= 92;
  g->knlo = 0; g->knhi = 127; g->rr_pos = -1;
  if (g->ext) {
    g->rr_pos = (int32_t)u32(b + 80, be);
    g->by = b[84]; g->ccnum = b[85]; g->cclo = b[86]; g->cchi = b[87];
    g->knlo = b[88]; g->knhi = b[89] ? b[89] : 127;
  }
}

static void read_sample(X *x, const uint8_t *c, uint32_t n, const uint8_t *hdr_name) {
  Sample *s;
  char path[256];
  const char *q, *last;
  if (x->ns >= EXS_MAX_LIST) { x->lists_capped = 1; return; }
  if (!grow((void **)&x->s, &x->caps, x->ns + 1, sizeof *x->s)) { x->lists_capped = 1; return; }
  s = &x->s[x->ns++];
  memset(s, 0, sizeof *s);
  if (n > 336) text(c + 336, n - 336 < 256 ? n - 336 : 256, s->name, sizeof s->name);
  if (!s->name[0]) text(hdr_name, 64, s->name, sizeof s->name);
  /* a name with a folder in it keeps its last part */
  for (q = s->name, last = s->name; *q; q++) if (*q == '/' || *q == '\\' || *q == ':') last = q + 1;
  if (last != s->name) memmove(s->name, last, strlen(last) + 1);
  path[0] = 0;
  if (n > 80) text(c + 80, n - 80 < 256 ? n - 80 : 256, path, sizeof path);
  {
    size_t L = strlen(path);
    while (L && (path[L - 1] == '/' || path[L - 1] == ':' || path[L - 1] == '\\')) path[--L] = 0;
    for (q = path, last = path; *q; q++) if (*q == '/' || *q == '\\' || *q == ':') last = q + 1;
    snprintf(s->hint, sizeof s->hint, "%s", last);
  }
}

static void read_params(X *x, const uint8_t *c, uint32_t n, int be) {
  uint32_t cnt, i, at;
  if (n < 4) return;
  cnt = u32(c, be);
  if (cnt > (n - 4) / 3) cnt = (n - 4) / 3;
  for (i = 0; i < cnt; i++) {
    int id = c[4 + i];
    if (id) { x->param[id] = s16(c + 4 + cnt + 2 * i, be); x->has[id] = 1; }
  }
  at = 4 + cnt * 3;
  if (at + 4 <= n) {
    uint32_t c2 = u32(c + at, be);
    at += 4;
    if (c2 > (n - at) / 4) c2 = (n - at) / 4;
    for (i = 0; i < c2; i++) {
      int id = u16(c + at + 4 * i, be);
      if (id > 0 && id < 0x200) { x->param[id] = u16(c + at + 4 * i + 2, be); x->has[id] = 1; }
    }
  }
}

/* 1 when the blocks were read (some may be damaged and cut short), 0 when not even the first block is one */
static int read_blocks(X *x, const uint8_t *t, size_t n) {
  size_t at = 0;
  int blocks = 0;
  while (at + EXS_HDR <= n) {
    const uint8_t *h = t + at;
    int be = h[0] == 0, type = h[3] & 0x0F;
    uint32_t size = u32(h + 4, be);
    if (memcmp(h + 16, be ? "SOBT" : "TBOS", 4) && memcmp(h + 16, be ? "SOBJ" : "JBOS", 4)) break;
    if (h[3] & 0x80) size &= 0x7FFF;
    if (size > n - at - EXS_HDR) size = (uint32_t)(n - at - EXS_HDR);   /* cut short: what's there counts */
    switch (type) {
      case 1: read_zone(x, h + EXS_HDR, size, be); break;
      case 2: read_group(x, h + EXS_HDR, size, be); break;
      case 3: read_sample(x, h + EXS_HDR, size, h + 20); break;
      case 4: read_params(x, h + EXS_HDR, size, be); break;
      default: break;
    }
    blocks++;
    at += EXS_HDR + (size_t)size;
  }
  return blocks > 0;
}

/* ---- finding the samples ---- */

static int ieq_w(const wchar_t *a, const wchar_t *b) { return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL; }

static void to_w(const char *u, wchar_t *w, int cap) {
  if (!MultiByteToWideChar(CP_UTF8, 0, u, -1, w, cap)) w[0] = 0;
}

static int has_ext(const char *n) { const char *d = strrchr(n, '.'); return d && d[1] && !strpbrk(d, " "); }

/* the names searched for, lower case, in a small hash table: each sample's name, the same with underscores for
   quotes, and for a name with no extension the name alone (matched by a file's name less an audio extension) */
typedef struct { wchar_t *k; int s, noext, next; } Key;
typedef struct { Key *k; int nk, capk; int *slot; unsigned mask; } Keys;

static unsigned whash(const wchar_t *w) { unsigned h = 2166136261u; while (*w) { h ^= *w++; h *= 16777619u; } return h; }

static int key_add(Keys *t, const wchar_t *w, int s, int noext) {
  if (t->nk && t->k[t->nk - 1].s == s && !wcscmp(t->k[t->nk - 1].k, w)) return 1;   /* no quote: one spelling */
  if (!grow((void **)&t->k, &t->capk, t->nk + 1, sizeof *t->k)) return 0;
  if (!(t->k[t->nk].k = _wcsdup(w))) return 0;
  t->k[t->nk].s = s; t->k[t->nk].noext = noext; t->k[t->nk].next = -1;
  t->nk++;
  return 1;
}

static int keys_make(X *x, Keys *t) {
  int i;
  unsigned n = 64;
  for (i = 0; i < x->ns; i++) {
    wchar_t w[300], v[300];
    size_t L;
    const Sample *s = &x->s[i];
    if (!s->name[0]) continue;
    to_w(s->name, w, 256);
    if (!w[0]) continue;
    CharLowerBuffW(w, (DWORD)wcslen(w));
    for (L = 0; w[L]; L++) v[L] = w[L] == L'\'' ? L'_' : w[L];
    v[L] = 0;
    if (!key_add(t, w, i, !has_ext(s->name)) || !key_add(t, v, i, !has_ext(s->name))) return 0;
  }
  while (n < (unsigned)t->nk * 2u) n *= 2;
  if (!(t->slot = malloc(n * sizeof *t->slot))) return 0;
  t->mask = n - 1;
  for (i = 0; i < (int)n; i++) t->slot[i] = -1;
  for (i = t->nk - 1; i >= 0; i--) {
    unsigned b = whash(t->k[i].k) & t->mask;
    t->k[i].next = t->slot[b]; t->slot[b] = i;
  }
  return 1;
}

static void keys_free(Keys *t) {
  int i;
  for (i = 0; i < t->nk; i++) free(t->k[i].k);
  free(t->k); free(t->slot);
}

static int audio_ext(const wchar_t *d) {
  return d && (!_wcsicmp(d, L".aif") || !_wcsicmp(d, L".aiff") || !_wcsicmp(d, L".wav") || !_wcsicmp(d, L".caf"));
}

static void found(X *x, Sample *s, const wchar_t *full, const wchar_t *folder_name, int depth) {
  char u[WP];
  int hinted = 0;
  if (s->hint[0]) { wchar_t h[300]; to_w(s->hint, h, 256); hinted = h[0] && ieq_w(h, folder_name); }
  if (s->path && (s->depth < depth || s->hinted || !hinted)) return;   /* the closer, or the one in the named folder */
  if (!WideCharToMultiByte(CP_UTF8, 0, full, -1, u, sizeof u, NULL, NULL)) return;
  if (!s->path) x->left--;
  free(s->path);
  s->path = _strdup(u);
  if (!s->path) { x->left++; return; }
  s->depth = depth; s->hinted = hinted;
}

/* one folder (dir, with a trailing backslash) and its subfolders, skipping skip (a subfolder already searched) */
static void search(X *x, const Keys *t, const wchar_t *dir, const wchar_t *skip, int level, int depth) {
  WIN32_FIND_DATAW fd;
  HANDLE h;
  wchar_t pat[WP], folder[MAX_PATH];
  const wchar_t *e;
  size_t L = wcslen(dir);
  if (depth > EXS_DEPTH || x->dirs >= EXS_MAX_DIRS || L + 2 >= WP || GetTickCount64() > x->until) return;
  if (x->cancel && x->cancel(x->carg)) return;
  x->dirs++;
  /* the folder's own name, for the hint */
  folder[0] = 0;
  for (e = dir + L - 1; e > dir && e[-1] != L'\\'; e--) {}
  if (L > 1) { size_t k = (size_t)(dir + L - 1 - e); if (k < MAX_PATH) { wmemcpy(folder, e, k); folder[k] = 0; } }
  _snwprintf(pat, WP - 1, L"%ls*", dir); pat[WP - 1] = 0;
  h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
  if (h == INVALID_HANDLE_VALUE) return;
  /* the files first, so a folder's own samples count before its subfolders' */
  do {
    wchar_t lw[MAX_PATH], *dot;
    int pass;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    wcsncpy(lw, fd.cFileName, MAX_PATH - 1); lw[MAX_PATH - 1] = 0;
    CharLowerBuffW(lw, (DWORD)wcslen(lw));
    dot = wcsrchr(lw, L'.');
    for (pass = 0; pass < 2; pass++) {
      int k;
      if (pass == 1) { if (!audio_ext(dot)) break; *dot = 0; }      /* the name less its extension */
      for (k = t->slot[whash(lw) & t->mask]; k >= 0; k = t->k[k].next) {
        Sample *s = &x->s[t->k[k].s];
        if ((pass == 1 && !t->k[k].noext) || wcscmp(t->k[k].k, lw)) continue;
        if (!s->path || (s->depth == level && !s->hinted)) {
          wchar_t full[WP];
          if (_snwprintf(full, WP - 1, L"%ls%ls", dir, fd.cFileName) < 0) continue;
          full[WP - 1] = 0;
          found(x, s, full, folder, level);
        }
      }
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    wchar_t sub[WP];
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
    if (_snwprintf(sub, WP - 1, L"%ls%ls\\", dir, fd.cFileName) < 0) continue;
    sub[WP - 1] = 0;
    if (skip && ieq_w(sub, skip)) continue;
    search(x, t, sub, NULL, level, depth + 1);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

static int exists_file(const wchar_t *p) {
  DWORD a = GetFileAttributesW(p);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* C:\ or \\server\share\ (with its trailing backslash) */
static int is_root(const wchar_t *d) {
  int n = 0;
  const wchar_t *q;
  if (d[0] == L'\\' && d[1] == L'\\') { for (q = d; *q; q++) n += *q == L'\\'; return n <= 4; }
  return wcslen(d) <= 3;
}

static void find_samples(X *x) {
  wchar_t prev[WP], cur[WP], up[WP];
  int i, level;
  Keys t;
  memset(&t, 0, sizeof t);
  prev[0] = 0;
  x->left = 0;
  for (i = 0; i < x->ns; i++) if (x->s[i].name[0]) x->left++;
  /* the previous sample's folder, then beside the .exs: an open each, no search */
  for (i = 0; i < x->ns && x->left > 0; i++) {
    Sample *s = &x->s[i];
    wchar_t n[300], full[WP];
    const wchar_t *dirs[2];
    int k;
    if (!s->name[0]) continue;
    to_w(s->name, n, 256);
    if (!n[0]) continue;
    dirs[0] = prev; dirs[1] = x->dir;
    for (k = 0; k < 2 && !s->path; k++) {
      if (!dirs[k][0] || _snwprintf(full, WP - 1, L"%ls%ls", dirs[k], n) < 0) continue;
      full[WP - 1] = 0;
      if (exists_file(full)) {
        wchar_t *sl;
        found(x, s, full, L"", 0);
        s->hinted = 1;                                   /* where it sits beside its kin counts as named */
        wcscpy(prev, full);
        if ((sl = wcsrchr(prev, L'\\')) != NULL) sl[1] = 0;
      }
    }
  }
  /* then the searches, a level further up each time */
  if (x->left <= 0) return;
  if (!keys_make(x, &t)) { keys_free(&t); return; }
  x->until = GetTickCount64() + EXS_SEARCH_MS;
  wcscpy(cur, x->dir);
  for (level = 0; level <= EXS_UP && x->left > 0; level++) {
    if (level == 0) search(x, &t, cur, NULL, 0, 0);
    else {
      size_t L = wcslen(cur);
      wchar_t *e;
      wcscpy(up, cur);
      if (L < 2) break;
      up[L - 1] = 0;
      if (!(e = wcsrchr(up, L'\\'))) break;
      e[1] = 0;
      if (is_root(up)) break;                         /* never a drive's root: too much to search */
      search(x, &t, up, cur, level, 0);
      wcscpy(cur, up);
    }
    if (x->cancel && x->cancel(x->carg)) break;
  }
  keys_free(&t);
}

/* ---- the regions ---- */

static void plan(X *x) {
  int i, j, rr_len = 0, random_all = x->has[0xa3] && x->param[0xa3] > 0, sw_any = 0;
  x->sw_lo = 127; x->sw_hi = 0; x->sw_def = -1;
  for (i = 0; i < x->ng; i++) {
    Group *g = &x->g[i];
    if (g->opts & 16) continue;
    if (g->ext && g->by == 2 && g->rr_pos + 2 > rr_len) rr_len = g->rr_pos + 2;
    if (g->ext && g->by == 1) {
      sw_any = 1;
      if (g->by_note < x->sw_lo) x->sw_lo = g->by_note;
      if (g->by_note > x->sw_hi) x->sw_hi = g->by_note;
      if (x->sw_def < 0) x->sw_def = g->by_note;
    }
  }
  if (!sw_any) x->sw_def = -1;
  for (i = 0; i < x->nz; i++) {
    const Zone *z = &x->z[i];
    const Group *g = z->group >= 0 && z->group < x->ng ? &x->g[z->group] : NULL;
    Reg r;
    memset(&r, 0, sizeof r);
    r.z = i; r.sw = -1; r.cc = -1;
    r.lokey = z->lo; r.hikey = z->hi;
    if (r.lokey > 127) r.lokey = 127;
    if (r.hikey > 127) r.hikey = 127;
    r.lovel = z->opts & 8 ? z->vlo : 0; r.hivel = z->opts & 8 ? z->vhi : 127;
    if (r.lovel > 127) r.lovel = 127;
    if (r.hivel > 127) r.hivel = 127;
    if (r.hivel == 0 && r.lovel == 0) r.hivel = 127;
    r.vol = z->vol; r.pan = z->pan;
    r.transpose = z->coarse; r.tune = z->fine;
    r.random = random_all;
    if (z->opts & 4) x->reversed = 1;
    if (z->loopopts & 1) {
      if (z->loopdir) x->loopdir = 1;
      if (z->xfade) x->xfade = 1;
      if (z->looptune) x->looptune = 1;
    }
    if (z->flex & 1) x->flex = 1;
    if (g) {
      if (g->opts & 16) continue;                                    /* muted */
      if (g->vmin > r.lovel) r.lovel = g->vmin;
      if (g->vmax < r.hivel) r.hivel = g->vmax;
      if (g->knlo > r.lokey) r.lokey = g->knlo;
      if (g->knhi < r.hikey) r.hikey = g->knhi;
      r.vol += g->vol; r.pan += g->pan;
      r.rel = g->rt; r.excl = g->excl;
      if (g->rnd) r.random = 1;
      if (g->poly) x->poly = 1;
      if (g->rt && g->rt_decay) x->rtdecay = 1;
      if (g->vxf) x->vxf = 1;
      if (g->kxf) x->kxf = 1;
      if (g->ext) {
        if (g->by == 2) { r.seq_len = rr_len; r.seq_pos = clampi(g->rr_pos + 2, 1, rr_len > 0 ? rr_len : 1); r.random = 0; }
        else if (g->by == 1) r.sw = g->by_note;
        else if (g->by == 3) { r.cc = g->ccnum & 127; r.cclo = g->cclo & 127; r.cchi = g->cchi & 127; }
        else if (g->by >= 4) x->enable_other = 1;
      }
    }
    if (r.lokey > r.hikey || r.lovel > r.hivel) continue;            /* nothing left inside its group's ranges */
    if (!grow((void **)&x->r, &x->capr, x->nr + 1, sizeof *x->r)) break;
    x->r[x->nr++] = r;
  }
  /* random select: zones with the same ranges and conditions share the draw */
  for (i = 0; i < x->nr; i++) {
    Reg *a = &x->r[i];
    int n = 0, k = 0;
    if (!a->random || a->hirand > 0) continue;
    for (j = i; j < x->nr; j++) {
      Reg *b = &x->r[j];
      if (b->random && b->hirand == 0 && b->lokey == a->lokey && b->hikey == a->hikey && b->lovel == a->lovel &&
          b->hivel == a->hivel && b->rel == a->rel && b->sw == a->sw && b->cc == a->cc) n++;
    }
    for (j = i; j < x->nr; j++) {
      Reg *b = &x->r[j];
      if (b->random && b->hirand == 0 && b->lokey == a->lokey && b->hikey == a->hikey && b->lovel == a->lovel &&
          b->hivel == a->hivel && b->rel == a->rel && b->sw == a->sw && b->cc == a->cc) {
        b->lorand = (float)k / (float)n; b->hirand = (float)(k + 1) / (float)n; k++;
      }
    }
  }
  if (x->has[0x2c] && x->param[0x2c] > 0) x->filter = 1;
  if (x->has[0x0a] && x->param[0x0a] > 0) x->legato = 1;
  if (x->has[0x14] && x->param[0x14] > 0) x->glide = 1;
  if (x->has[0x08] && x->param[0x08] != 0) x->keyscale = 1;
}

static void opi(SfzBuild *b, const char *name, int v) { char t[24]; snprintf(t, sizeof t, "%d", v); sfz_opcode(b, name, t); }
static void opd(SfzBuild *b, const char *name, double v) { char t[40]; snprintf(t, sizeof t, "%.6g", v); sfz_opcode(b, name, t); }

static double env_time(X *x, int id) {
  double v;
  if (!x->has[id]) return 0;
  v = clampi(x->param[id], 0, 127) / 127.0;
  return 10.0 * v * v * v * v;
}

static int feed_exs(SfzBuild *b, void *ctx, const wchar_t *full, char *why, int wcap) {
  X *x = ctx;
  int i, transpose = 0, tune = 0;
  (void)full; (void)why; (void)wcap;
  x->b = b;
  sfz_header(b, "global");
  /* the instrument's amplitude envelope, velocity and bend; every region inherits them */
  if (x->has[0x16d]) opd(b, "ampeg_delay", env_time(x, 0x16d));
  if (x->has[0x52]) opd(b, "ampeg_attack", env_time(x, 0x52));
  if (x->has[0x58]) opd(b, "ampeg_hold", env_time(x, 0x58));
  if (x->has[0x54]) opd(b, "ampeg_decay", env_time(x, 0x54));
  if (x->has[0x51]) opd(b, "ampeg_sustain", clampi(x->param[0x51], 0, 127) * 100.0 / 127.0);
  if (x->has[0x55]) opd(b, "ampeg_release", env_time(x, 0x55));
  if (x->has[0x5a]) {
    double v = x->param[0x5a] / -60.0;
    opd(b, "amp_veltrack", (v < 0 ? 0 : v > 1 ? 1 : v) * 100.0);
  }
  {
    int up = x->has[0x03] ? clampi(x->param[0x03], 0, 48) : 2;
    int dn = x->has[0x04] ? (x->param[0x04] == -1 ? up : clampi(x->param[0x04], 0, 48)) : 2;
    opi(b, "bend_up", up * 100); opi(b, "bend_down", -dn * 100);
  }
  if (x->has[0x2d]) transpose += x->param[0x2d];
  if (x->has[0x0e]) transpose += x->param[0x0e];
  if (x->has[0x0f]) tune += x->param[0x0f];
  if (x->zones_capped) sfz_ignored(b, "too many zones (some left out)");
  if (x->lists_capped) sfz_ignored(b, "too many samples or groups (some left out)");
  if (x->reversed) sfz_ignored(b, "reversed zones");
  if (x->loopdir) sfz_ignored(b, "backward and alternating loops");
  if (x->xfade) sfz_ignored(b, "loop crossfades");
  if (x->looptune) sfz_ignored(b, "loop tuning");
  if (x->output) sfz_ignored(b, "zone outputs");
  if (x->flex) sfz_ignored(b, "Flex");
  if (x->poly) sfz_ignored(b, "group polyphony");
  if (x->rtdecay) sfz_ignored(b, "release trigger decay");
  if (x->vxf) sfz_ignored(b, "velocity crossfades");
  if (x->kxf) sfz_ignored(b, "key crossfades");
  if (x->enable_other) sfz_ignored(b, "enable by pitch bend, channel, articulation or tempo");
  if (x->filter) sfz_ignored(b, "filter");
  if (x->legato) sfz_ignored(b, "mono legato");
  if (x->glide) sfz_ignored(b, "glide");
  if (x->keyscale) sfz_ignored(b, "volume key scaling");
  for (i = 0; i < x->nr; i++) {
    const Reg *r = &x->r[i];
    const Zone *z = &x->z[r->z];
    const Sample *s;
    if ((i & 0xFFF) == 0 && sfz_cancelled(b)) return 1;
    if (z->sample >= x->ns) continue;                               /* no such sample: nothing to play */
    s = &x->s[z->sample];
    if (!s->name[0]) continue;
    sfz_header(b, "region");
    sfz_opcode(b, "sample", s->path ? s->path : s->name);            /* not found: missing, by its name */
    opi(b, "pitch_keycenter", z->key > 127 ? 60 : z->key);
    opi(b, "lokey", r->lokey); opi(b, "hikey", r->hikey);
    opi(b, "lovel", r->lovel); opi(b, "hivel", r->hivel);
    if (z->opts & 2) opi(b, "pitch_keytrack", 0);
    if (r->transpose + transpose) opi(b, "transpose", clampi(r->transpose + transpose, -127, 127));
    if (r->tune + tune) opi(b, "tune", clampi(r->tune + tune, -9600, 9600));
    if (r->vol) opi(b, "volume", clampi(r->vol, -144, 24));
    if (r->pan) opi(b, "pan", clampi(r->pan * 2, -100, 100));
    if (z->start) opi(b, "offset", (int)(z->start > 0x7FFFFFFF ? 0x7FFFFFFF : z->start));
    if (z->end > z->start + 1 && z->end <= 0x7FFFFFFF) opi(b, "end", (int)z->end - 1);
    if (z->loopopts & 1) {
      sfz_opcode(b, "loop_mode", z->loopopts & 4 ? "loop_sustain" : "loop_continuous");
      if (z->le > z->ls + 1 && z->le <= 0x7FFFFFFF) { opi(b, "loop_start", (int)z->ls); opi(b, "loop_end", (int)z->le - 1); }
      else sfz_loop_whole(b);
    } else sfz_opcode(b, "loop_mode", z->opts & 1 ? "one_shot" : "no_loop");
    if (r->rel) sfz_opcode(b, "trigger", "release");
    if (r->excl) { opi(b, "group", r->excl); opi(b, "off_by", r->excl); }
    if (r->seq_len > 1) { opi(b, "seq_length", r->seq_len); opi(b, "seq_position", r->seq_pos); }
    if (r->hirand > 0 && (r->lorand > 0 || r->hirand < 1)) { opd(b, "lorand", r->lorand); opd(b, "hirand", r->hirand); }
    if (r->sw >= 0) {
      opi(b, "sw_lokey", x->sw_lo); opi(b, "sw_hikey", x->sw_hi); opi(b, "sw_last", r->sw);
      if (x->sw_def >= 0) opi(b, "sw_default", x->sw_def);
    }
    if (r->cc >= 0) {
      char n[16];
      snprintf(n, sizeof n, "locc%d", r->cc); opi(b, n, r->cclo);
      snprintf(n, sizeof n, "hicc%d", r->cc); opi(b, n, r->cchi);
    }
  }
  return 1;
}

int sf_is_exs(const char *path, const uint8_t *head, size_t n) {
  const char *dot = path ? strrchr(path, '.') : NULL;
  if (dot && !_stricmp(dot, ".exs")) return 1;
  if (n < EXS_HDR || head[1] != 1) return 0;
  return !memcmp(head + 16, head[0] == 0 ? "SOBT" : "TBOS", 4) || !memcmp(head + 16, head[0] == 0 ? "SOBJ" : "JBOS", 4);
}

int sf_parse_exs(SmpFont *f, const char *path, int (*cancel)(void *), void *carg, char *why, int wcap) {
  X *x = calloc(1, sizeof *x);
  wchar_t *full = sf_full_path(path);
  HANDLE h = INVALID_HANDLE_VALUE;
  LARGE_INTEGER sz; DWORD got = 0;
  uint8_t *t = NULL;
  int ok = 0, i;
  if (!x || !full) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
  h = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) { snprintf(why, (size_t)wcap, "it can't be read"); goto done; }
  if (!GetFileSizeEx(h, &sz) || sz.QuadPart > EXS_MAX_FILE) { snprintf(why, (size_t)wcap, "it's too big to be an instrument"); goto done; }
  if (!(t = malloc((size_t)sz.QuadPart + 1))) { snprintf(why, (size_t)wcap, "out of memory"); goto done; }
  if (sz.QuadPart && (!ReadFile(h, t, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart)) {
    snprintf(why, (size_t)wcap, "it can't be read");
    goto done;
  }
  CloseHandle(h); h = INVALID_HANDLE_VALUE;
  if (!read_blocks(x, t, (size_t)sz.QuadPart)) { snprintf(why, (size_t)wcap, "its blocks are damaged"); goto done; }
  free(t); t = NULL;
  if (!x->nz) { snprintf(why, (size_t)wcap, "it has no zones"); goto done; }
  {
    wchar_t *sl;
    wcsncpy(x->dir, full, WP - 1); x->dir[WP - 1] = 0;
    if ((sl = wcsrchr(x->dir, L'\\')) != NULL) sl[1] = 0; else x->dir[0] = 0;
  }
  x->cancel = cancel; x->carg = carg;
  find_samples(x);
  if (cancel && cancel(carg)) { snprintf(why, (size_t)wcap, "cancelled"); goto done; }
  plan(x);
  ok = sfz_build(f, path, feed_exs, x, cancel, carg, why, wcap);
  if (ok == 0 && !strcmp(why, "it has no regions with samples")) snprintf(why, (size_t)wcap, "it has no samples to play");
done:
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  free(t); free(full);
  if (x) {
    for (i = 0; i < x->ns; i++) free(x->s[i].path);
    free(x->z); free(x->g); free(x->s); free(x->r);
    free(x);
  }
  return ok;
}
