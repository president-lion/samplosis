/* gui.c: Samplosis's window, made for a screen reader and the keyboard. Plain Win32 controls, each with its label
   just before it (which is how a screen reader names it).

   Tab. REAPER doesn't pass Tab on into a plugin's window, so every control takes every key (WM_GETDLGCODE) and moves
   the focus itself: Tab and Shift+Tab go through the page's controls in order and past either end out to REAPER's
   FX window around it (tab_out, from cosmos_clap/gui.c).
   Focus is never lost: when a page switches the focus goes to a control on the new page, and when REAPER focuses the
   window itself (coming back to it) the focus goes on to the control last used.
   Values are sliders (trackbars) that a screen reader reads in words: the words are set as the slider's accessible
   name and value (Dynamic Annotation, IAccPropServices). Up or Right and Down or Left step a value (Shift ten times as
   far, Control a tenth), Page Up and Page Down ten steps, Home and End go to the limits, and it stops there. Enter,
   or typing a character, swaps the slider for an edit box in its place holding the value; Enter there reads what was
   typed (a bad entry beeps and the old value stays), Escape leaves it as it was, and either goes back to the slider.
   Tab or leaving the box reads it too. Choices are combo boxes. Every change goes to the engine at once and the host
   is told (plugin.c).
   A 250 ms timer brings the controls up to date (automation, renders) and frees replaced samples; a value is set
   only when it changes, so the screen reader isn't interrupted.
   Two players, two main pages. The first control of both is one tab control, "Player" (its accessible name, by
   annotation), with the tabs Sampler and SoundFont: the tab chosen is what the instance plays, and its page shows.
   Left and Right on it change the tab, and Ctrl+Tab or Ctrl+Shift+Tab do from any control on either main page; the
   focus is then on the tab control. The window opens on the tab that plays, and follows it when something else
   (a state, a dropped file) changes it. Settings is the third page; Back goes to the main page that plays.
   On the SoundFont page, the controls of the mode not chosen (One preset: Preset and Channel; All channels: the 16
   channels and their outputs) are hidden, and Tab skips hidden controls. The preset lists are built once per font.
   Each channel's first entry, "From the song (now <preset>)", follows what the song plays, but not while its list is
   open; a chosen preset the font lacks shows as "Bank b program p (not in this font)".
   Files. Each main page has Browse and a File combo box: the files of its kind in the loaded file's folder, the
   loaded one selected; choosing one (Up and Down too) loads it at once. Both load only their own page's kind (a file
   of the other kind gives that player's error). A dropped file goes by its extension: a .sf2 or .sf3 loads as the
   SoundFont, an audio file into the sampler, and the tab follows.
   Everything here runs on the main thread (the window's). */
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <ole2.h>
#include <initguid.h>
#include <oleacc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include "plugin_int.h"
#include "decode.h"

#define W_WIDTH 760
#define ROW 30
#define CLASS_NAME L"SamplosisWindow"
#define LIMIT_LO 0.1
#define LIMIT_HI 10000.0
#define NOTE_MS 6000

#define NCH 16                                /* MIDI channels */
enum { ID_VAL = 100,                          /* + parameter id: its slider or combo box */
       ID_SAMPLE = 200, ID_BROWSE, ID_FILE, ID_STATUS, ID_SETTINGS,
       ID_STRETCHER, ID_KEEPTONE, ID_DIR, ID_DIRBROWSE, ID_LIMIT, ID_USED, ID_CLEAR, ID_BACK,
       ID_TABS, ID_FONT, ID_SFBROWSE, ID_SFFILE, ID_SFMODE, ID_SFPRESET, ID_SFCHANNEL, ID_SFSTATUS,
       ID_SFSETTINGS, ID_RAWRATE, ID_RAWCH, ID_RAWFMT, ID_RAWEND, ID_FFMPEG, ID_FFBROWSE,
       ID_EDIT = 300,                        /* + slider number: the edit box that stands in for it */
       ID_CHAN = 400,                         /* + channel - 1: its preset combo box */
       ID_OUT = 420 };                        /* + channel - 1: its output combo box */
enum { PG_SAMPLER = 0, PG_SETTINGS = 1, PG_FONT = 2, NPAGES = 3 };
#define PAGE_CTLS 128
#define PAGE_STOPS 64
#define SL_LIMIT (-1)                         /* the slider that isn't a parameter: the cache size limit */
enum { T_REFRESH = 1, T_OPEN = 2 };
#define WM_DROPPED (WM_APP + 1)

typedef struct { IDropTarget t; struct Gui *g; int ok; } Drop;

/* a value's slider and the edit box that stands in for it while typing */
typedef struct { HWND tb, ed; int what; char said[64]; bool editing; } Slider;

typedef struct Gui {
  Plug *P;
  HWND win; HFONT font;
  int page;                                   /* PG_SAMPLER, PG_SETTINGS or PG_FONT */
  HWND tabs;                                  /* the Player tab control: first on both main pages */
  bool setting_tab;                           /* the window moves the tab itself: not the user's choice */
  HWND sample, browse, status, settings;
  HWND val[SMP_NALLPARAMS];                   /* each parameter's slider or combo box */
  Slider sl[SMP_NALLPARAMS + 1]; int nsl;
  Slider *sl_of[SMP_NALLPARAMS];              /* a parameter's slider, or NULL (a choice) */
  IAccPropServices *aps;                      /* the words a screen reader reads for a slider; NULL: numbers */
  HWND stretcher, keeptone, dir, dirbrowse, used, clear, back;
  Slider *limit;
  char dir_shown[1024], used_text[700];
  HWND rawrate, rawch, rawfmt, rawend, ffmpeg, ffbrowse;  /* how raw files read; where FFmpeg is */
  char ff_text[1100];                         /* FFmpeg's path as found, or "Not found" */
  /* the SoundFont page */
  HWND sffile, sfbrowse, sfmode, sfpreset, sfchannel, chan[NCH], out[NCH], sfstatus, sfsettings;
  HWND mode_one[4], mode_all[4 * NCH];        /* each mode's own controls with their labels: shown in that mode only */
  int nmode_one, nmode_all;
  bool sf_one;                                /* the mode the page shows: One preset */
  const void *list_font; char list_key[1200]; /* the font the preset lists were built for (and its text) */
  char from_song[NCH][300];                   /* each channel's first entry as it reads now */
  HWND file[2];                               /* each main page's File combo box: the sampler's, the SoundFont's */
  char file_key[2][1110];                     /* the file it was listed for (unchanged: nothing to do) */
  wchar_t file_dir[2][1100];                  /* its folder, when its entries are files that can load */
  bool file_live[2];
  char lacks_preset[200], lacks_chan[NCH][200];   /* the last entry saying a chosen preset isn't in the font, or "" */
  HWND all[NPAGES][PAGE_CTLS]; int nall[NPAGES];      /* every window on each page (labels too), to show and hide */
  HWND order[NPAGES][PAGE_STOPS]; int norder[NPAGES]; /* the Tab order of each page */
  HWND last; bool had_focus, closing;
  char note[800]; ULONGLONG note_until;       /* a message the status box shows for a while (a file that won't load) */
  Drop drop; int ole_init, drop_registered;
  wchar_t dropped[1100];                      /* the file a drop noted, loaded on WM_DROPPED */
  int modal;                                  /* file or folder dialogs open (see modal_begin) */
  long fails_seen;                            /* the plugin's failed background loads the window has told of */
  long notes_seen;                            /* and its loads stopped by a load of the other kind */
} Gui;

/* ---- tests (tests\host.c): beeps are counted, and can be kept quiet ---- */
static volatile LONG beeps; static int quiet;
SMP_API long smp_test_gui_beeps(void) { return beeps; }
SMP_API void smp_test_gui_quiet(int q) { quiet = q; }
static void beep(void) { InterlockedIncrement(&beeps); if (!quiet) MessageBeep(MB_ICONWARNING); }

static HINSTANCE module(void) {
  HMODULE me = NULL;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCWSTR)&module, &me);
  return me;
}

static void set_text(HWND h, const char *s) {          /* only when it changed, so a screen reader isn't told twice */
  wchar_t w[1100], now[1100];
  MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 1100);
  GetWindowTextW(h, now, 1100);
  if (wcscmp(w, now)) SetWindowTextW(h, w);
}
static void get_text(HWND h, char *s, int cap) {
  wchar_t w[1100]; GetWindowTextW(h, w, 1100);
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s, cap, NULL, NULL);
}
static bool is_class(HWND h, const wchar_t *c) { wchar_t n[32]; GetClassNameW(h, n, 32); return !_wcsicmp(n, c); }
static void focus(HWND h) {
  SetFocus(h);
  if (is_class(h, L"Edit")) SendMessageW(h, EM_SETSEL, 0, -1);
}
static bool is_choice(int id) { int k = smp_param_info(id)->kind; return k == SMP_K_CHOICE || k == SMP_K_BOOL; }

/* Out of a plugin's window to the control before or after it in REAPER's window around it (the FX list, the Add
   button): the next one in Tab order (the order Windows lists the windows in) that isn't the plugin's own.
   GetNextDlgTabItem can't be trusted for this: it stops at containers that aren't marked as holding controls.
   (From cosmos_clap/gui.c.) */
typedef struct { HWND list[1024]; int n; } Walk;
static BOOL CALLBACK walk_one(HWND h, LPARAM l) { Walk *w = (Walk *)l; if (w->n < 1024) w->list[w->n++] = h; return TRUE; }
static bool tab_out(HWND win, bool back) {
  HWND root = GetAncestor(win, GA_ROOT);
  Walk *w = root ? malloc(sizeof *w) : NULL;
  int at = -1, i, k; bool moved = false;
  if (!w) return false;
  w->n = 0; EnumChildWindows(root, walk_one, (LPARAM)w);
  for (i = 0; i < w->n && at < 0; i++) if (w->list[i] == win) at = i;
  for (k = 1; at >= 0 && k < w->n && !moved; k++) {
    HWND h = w->list[((at + (back ? -k : k)) % w->n + w->n) % w->n];
    if (h == win || IsChild(win, h) || !(GetWindowLongW(h, GWL_STYLE) & WS_TABSTOP)) continue;
    if (!IsWindowVisible(h) || !IsWindowEnabled(h)) continue;
    SetFocus(h); moved = true;
  }
  free(w);
  return moved;
}

/* ---- values ---- */
static void param_text(int id, double v, char *out, int cap) {
  smp_value_text(id, v, out, cap);
  if (is_choice(id) && out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');   /* "Off", "Note" */
}
static void gb_text(double gb, char *out, int cap) {
  if (fabs(gb - floor(gb + 0.5)) < 1e-9) snprintf(out, (size_t)cap, "%.0f GB", gb);
  else snprintf(out, (size_t)cap, "%.1f GB", gb);
}
static void set_limit(Gui *g, double gb);
static void say_note(Gui *g, const char *msg);

/* ---- sliders ---- */
static void sl_range(const Slider *s, double *lo, double *hi, double *step) {
  const SmpParamInfo *pi;
  if (s->what == SL_LIMIT) { *lo = LIMIT_LO; *hi = LIMIT_HI; *step = 1; return; }
  pi = smp_param_info(s->what);
  *lo = pi->lo; *hi = pi->hi; *step = pi->kind == SMP_K_MS ? 10 : 1;
}
static double sl_get(Gui *g, const Slider *s) {
  SmpSettings st;
  if (s->what != SL_LIMIT) return smp_plug_get_param(g->P, s->what);
  smp_settings_get(&st);
  return st.cache_limit_gb;
}
static void sl_text(const Slider *s, double v, char *out, int cap) {
  if (s->what == SL_LIMIT) gb_text(v, out, cap); else param_text(s->what, v, out, cap);
}
static int sl_parse(const Slider *s, const char *t, double *v) {
  char *e;
  if (s->what != SL_LIMIT) return smp_value_parse(s->what, t, v);
  *v = strtod(t, &e);
  while (*e == ' ') e++;
  return e != t && (!*e || !_stricmp(e, "GB")) && *v > 0;
}
static void annotate(Gui *g, HWND h, MSAAPROPID prop, const char *text) {
  wchar_t w[128];
  if (!g->aps || !MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 128)) return;
  g->aps->lpVtbl->SetHwndPropStr(g->aps, h, (DWORD)OBJID_CLIENT, CHILDID_SELF, prop, w);
}
/* the slider shows the value: its words (said) as the screen reader's value, and the thumb where the value is */
static void sl_show(Gui *g, Slider *s, bool force) {
  char t[64]; double v = sl_get(g, s), lo, hi, step; LRESULT n, pos;
  sl_text(s, v, t, sizeof t);
  if (!force && !strcmp(t, s->said)) return;
  strcpy(s->said, t);
  annotate(g, s->tb, PROPID_ACC_VALUE, t);
  sl_range(s, &lo, &hi, &step);
  n = SendMessageW(s->tb, TBM_GETRANGEMAX, 0, 0);
  pos = hi > lo ? (LRESULT)floor((v - lo) / (hi - lo) * (double)n + 0.5) : 0;
  if (SendMessageW(s->tb, TBM_GETPOS, 0, 0) != pos) SendMessageW(s->tb, TBM_SETPOS, TRUE, pos);
  NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, s->tb, OBJID_CLIENT, CHILDID_SELF);
}
static void sl_set(Gui *g, Slider *s, double v) {
  double lo, hi, step;
  sl_range(s, &lo, &hi, &step);
  if (v < lo) v = lo;                            /* past either end it stays at the end */
  if (v > hi) v = hi;
  if (s->what == SL_LIMIT) set_limit(g, v); else smp_plug_set_param(g->P, s->what, v);
  sl_show(g, s, true);
}
/* Up and Down: a semitone, a cent, a dB, 10 ms, 1 %, 1 GB (by: 1, 10 or 0.1, and the sign); whole-number values
   never step less than 1 */
static void sl_nudge(Gui *g, Slider *s, double by) {
  double lo, hi, step, v;
  int kind = s->what == SL_LIMIT ? -1 : smp_param_info(s->what)->kind;
  bool whole = kind == SMP_K_NOTE || kind == SMP_K_SEMI || kind == SMP_K_CENT || kind == SMP_K_COUNT || kind == SMP_K_PAN;
  sl_range(s, &lo, &hi, &step);
  step *= fabs(by);
  if (whole && step < 1) step = 1;
  v = sl_get(g, s) + (by < 0 ? -step : step);
  v = whole ? floor(v + 0.5) : s->what == SL_LIMIT ? floor(v * 10 + 0.5) / 10 : floor(v * 1000 + 0.5) / 1000;
  sl_set(g, s, v);
}
/* the mouse moved the thumb */
static void sl_dragged(Gui *g, Slider *s) {
  double lo, hi, step, n = (double)SendMessageW(s->tb, TBM_GETRANGEMAX, 0, 0), v;
  sl_range(s, &lo, &hi, &step);
  v = lo + (double)SendMessageW(s->tb, TBM_GETPOS, 0, 0) / (n > 0 ? n : 1) * (hi - lo);
  v = s->what == SL_LIMIT ? floor(v * 10 + 0.5) / 10 : floor(v / step + 0.5) * step;
  sl_set(g, s, v);
}

/* Enter (or a character typed) on a slider: the edit box takes its place, holding the value (or the character) */
static void edit_begin(Gui *g, Slider *s, wchar_t first) {
  RECT r; wchar_t w[64];
  if (first) { w[0] = first; w[1] = 0; } else MultiByteToWideChar(CP_UTF8, 0, s->said, -1, w, 64);
  SetWindowTextW(s->ed, w);
  GetWindowRect(s->tb, &r); MapWindowPoints(NULL, g->win, (POINT *)&r, 2);
  SetWindowPos(s->ed, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
  s->editing = true;
  ShowWindow(s->ed, SW_SHOW);
  SetFocus(s->ed);
  if (first) SendMessageW(s->ed, EM_SETSEL, 1, 1); else SendMessageW(s->ed, EM_SETSEL, 0, -1);
  ShowWindow(s->tb, SW_HIDE);
}
/* Back to the slider: apply reads what was typed (a bad entry beeps and the value stays); to_slider puts the focus
   on the slider (Enter, Escape), else the focus is going somewhere else already (Tab, a click) */
static void edit_end(Gui *g, Slider *s, bool apply, bool to_slider) {
  if (!s->editing) return;
  s->editing = false;                            /* first: the focus leaving the box comes back here */
  if (apply) {
    char t[128]; double v;
    get_text(s->ed, t, sizeof t);
    if (strcmp(t, s->said)) {
      if (sl_parse(s, t, &v)) sl_set(g, s, v); else beep();
    }
  }
  ShowWindow(s->tb, SW_SHOW);
  g->last = s->tb;
  if (to_slider) focus(s->tb);
  ShowWindow(s->ed, SW_HIDE);
}

static void show_param(Gui *g, int id, bool force) {
  double v = smp_plug_get_param(g->P, id);
  if (is_choice(id)) {
    int k = (int)floor(v + 0.5);
    if (SendMessageW(g->val[id], CB_GETCURSEL, 0, 0) != k) SendMessageW(g->val[id], CB_SETCURSEL, (WPARAM)k, 0);
    return;
  }
  if (!g->sl_of[id]->editing) sl_show(g, g->sl_of[id], force);
}

/* ---- the Settings page ---- */
static void rescan_used(Gui *g) {             /* scans the folder: on opening the page and after a change */
  SmpSettings s; char why[600], lim[32]; uint64_t b;
  smp_settings_get(&s);
  if (!smp_cache_ok(why, sizeof why)) { snprintf(g->used_text, sizeof g->used_text, "Not working: %s", why); return; }
  b = smp_cache_used_bytes();
  gb_text(s.cache_limit_gb, lim, sizeof lim);
  if (b < (1ull << 30)) snprintf(g->used_text, sizeof g->used_text, "%.0f MB of %s", b / 1048576.0, lim);
  else snprintf(g->used_text, sizeof g->used_text, "%.1f GB of %s", b / 1073741824.0, lim);
}
static void find_ffmpeg(Gui *g) {            /* searches: on opening the page and after Browse for FFmpeg */
  if (!smp_ffmpeg_find(g->ff_text, sizeof g->ff_text)) snprintf(g->ff_text, sizeof g->ff_text, "Not found");
}
static int rate_index(int rate) {             /* the Raw sample rate entry for a rate */
  const int *r; int n = smp_raw_rates(&r), i;
  for (i = 0; i < n; i++) if (r[i] == rate) return i;
  return 0;
}
static void sel(HWND c, int k) { if (SendMessageW(c, CB_GETCURSEL, 0, 0) != k) SendMessageW(c, CB_SETCURSEL, (WPARAM)k, 0); }
static void show_settings(Gui *g) {
  SmpSettings s; HWND foc = GetFocus();
  smp_settings_get(&s);
  if (foc != g->dir) { set_text(g->dir, s.cache_dir); snprintf(g->dir_shown, sizeof g->dir_shown, "%s", s.cache_dir); }
  if (!g->limit->editing) sl_show(g, g->limit, false);
  set_text(g->used, g->used_text);
  sel(g->stretcher, s.stretcher);
  sel(g->keeptone, s.keep_tone);
  sel(g->rawrate, rate_index(s.raw_rate));
  sel(g->rawch, s.raw_channels == 1 ? 0 : 1);
  sel(g->rawfmt, s.raw_format);
  sel(g->rawend, s.raw_endian);
  set_text(g->ffmpeg, g->ff_text);
}
static void save_settings(Gui *g, const SmpSettings *s) {
  if (smp_settings_set(s)) beep();
  rescan_used(g);
}
static int apply_dir(Gui *g) {
  char t[1024]; SmpSettings s;
  get_text(g->dir, t, sizeof t);
  if (!strcmp(t, g->dir_shown)) return 1;
  smp_settings_get(&s);
  if (!t[0] || strlen(t) >= sizeof s.cache_dir) { beep(); set_text(g->dir, g->dir_shown); return 0; }
  strcpy(s.cache_dir, t);
  save_settings(g, &s);
  snprintf(g->dir_shown, sizeof g->dir_shown, "%s", t);
  set_text(g->used, g->used_text);
  return 1;
}
static void set_limit(Gui *g, double gb) {     /* the slider has clamped it */
  SmpSettings s;
  smp_settings_get(&s);
  s.cache_limit_gb = floor(gb * 10 + 0.5) / 10;
  save_settings(g, &s);
  set_text(g->used, g->used_text);
}
/* the Stretcher and Keep the tone combo boxes: every engine makes its copies again with the new choice (old ones
   stay in the cache, so changing back is instant). The raw combo boxes: how the next .raw or .pcm file loaded reads
   (a raw file's cache key holds them, so loading it again reads it the new way). */
static void choose(Gui *g, int id, int k) {
  SmpSettings s; const int *r; int n = smp_raw_rates(&r);
  smp_settings_get(&s);
  if (id == ID_STRETCHER) s.stretcher = k;
  else if (id == ID_KEEPTONE) s.keep_tone = k;
  else if (id == ID_RAWRATE) s.raw_rate = r[k < n ? k : n - 1];
  else if (id == ID_RAWCH) s.raw_channels = k ? 2 : 1;
  else if (id == ID_RAWFMT) s.raw_format = k;
  else if (id == ID_RAWEND) s.raw_endian = k;
  if (smp_settings_set(&s)) beep();             /* not save_settings: the cache folder needs no rescan */
  show_settings(g);
  if (id == ID_RAWRATE || id == ID_RAWCH || id == ID_RAWFMT || id == ID_RAWEND) {
    char path[1100];                            /* the loaded raw sample stays as it was read: Status says so */
    if (smp_plug_sample_path(g->P, path, sizeof path) == 1 && smp_is_raw_name(path))
      say_note(g, "Raw settings apply the next time a raw file loads");
  }
}
static void clear_cache(Gui *g) {             /* every file not in use */
  smp_cache_trim(0);
  rescan_used(g);
  set_text(g->used, g->used_text);
}

/* ---- modal dialogs ----
   A file or folder dialog runs its own message loop and only disables REAPER's FX window, so while it is open the
   user can close the FX chain, delete the track or close the project: the window is destroyed (smp_gui_close) and
   the plugin may be freed. So g->modal counts dialogs open; smp_gui_close then destroys the window and cuts g off
   from the plugin (g->P = NULL) but leaves freeing g to modal_end, and the code after a dialog does nothing more. */
static void gui_free(Gui *g);
static void modal_begin(Gui *g) { g->modal++; }
static bool modal_end(Gui *g) {          /* true when the window closed meanwhile: g is gone, touch nothing */
  g->modal--;
  if (!g->closing) return false;
  if (!g->modal) gui_free(g);
  return true;
}

/* tests: a dialog that returns a path at once, after closing the window (2) or destroying the plugin (3) first */
static int test_dialog; static char test_path[1100]; static const clap_plugin_t *test_plugin;
SMP_API void smp_test_gui_dialog(const clap_plugin_t *p, int action, const char *path) {
  test_plugin = p; test_dialog = action;
  snprintf(test_path, sizeof test_path, "%s", path ? path : "");
}
static int stub_dialog(Gui *g, wchar_t *out, int cap) {
  int a = test_dialog;
  test_dialog = 0;
  if (a == 2 && g->P) smp_gui_close(g->P);
  else if (a == 3 && test_plugin) test_plugin->destroy(test_plugin);
  return MultiByteToWideChar(CP_UTF8, 0, test_path, -1, out, cap) > 0;
}

static int pick_folder(HWND owner, wchar_t *out, int cap) {
  IFileOpenDialog *d = NULL; IShellItem *it = NULL; PWSTR path = NULL; DWORD opt = 0; int ok = 0;
  if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d))) return -1;
  d->lpVtbl->GetOptions(d, &opt);
  d->lpVtbl->SetOptions(d, opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  d->lpVtbl->SetTitle(d, L"Cache folder");
  if (SUCCEEDED(d->lpVtbl->Show(d, owner)) && SUCCEEDED(d->lpVtbl->GetResult(d, &it)) &&
      SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &path))) {
    ok = wcslen(path) < (size_t)cap;
    if (ok) wcscpy(out, path);
    CoTaskMemFree(path);
  }
  if (it) it->lpVtbl->Release(it);
  d->lpVtbl->Release(d);
  return ok;
}

static void browse_folder(Gui *g) {
  wchar_t w[1100]; int ok;
  modal_begin(g);
  ok = test_dialog ? stub_dialog(g, w, 1100) : pick_folder(GetAncestor(g->win, GA_ROOT), w, 1100);
  if (modal_end(g)) return;
  if (ok > 0) {
    SmpSettings s; char u[1024];
    smp_settings_get(&s);
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL) > 0) {
      strcpy(s.cache_dir, u);
      save_settings(g, &s);
      show_settings(g);
    } else beep();
  } else if (ok < 0) beep();
  focus(g->dirbrowse);
}

/* Browse for FFmpeg: an .exe chosen goes in Settings, and the FFmpeg box shows what is found now */
static void browse_ffmpeg(Gui *g) {
  wchar_t file[1100] = L"", dirw[1100] = L"", *slash; OPENFILENAMEW of; int ok;
  if (smp_ffmpeg_find_w(dirw, 1100) && (slash = wcsrchr(dirw, L'\\')) != NULL) *slash = 0; else dirw[0] = 0;
  memset(&of, 0, sizeof of);
  of.lStructSize = sizeof of;
  of.hwndOwner = GetAncestor(g->win, GA_ROOT);
  of.lpstrFilter = L"Programs\0*.exe\0All files\0*.*\0";
  of.lpstrFile = file; of.nMaxFile = 1100;
  of.lpstrInitialDir = dirw[0] ? dirw : NULL;
  of.lpstrTitle = L"Find ffmpeg.exe";
  of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
  modal_begin(g);
  ok = test_dialog ? stub_dialog(g, file, 1100) : GetOpenFileNameW(&of);
  if (modal_end(g)) return;               /* the window (and maybe the plugin) went while the dialog was open */
  if (ok) {
    SmpSettings s; char u[1024];
    smp_settings_get(&s);
    if (WideCharToMultiByte(CP_UTF8, 0, file, -1, u, sizeof u, NULL, NULL) > 0) {
      strcpy(s.ffmpeg_path, u);
      if (smp_settings_set(&s)) beep();
      find_ffmpeg(g);
      show_settings(g);
    } else beep();
  }
  focus(g->ffbrowse);
}

/* ---- the sample and the SoundFont ---- */
static void say_note(Gui *g, const char *msg) {
  snprintf(g->note, sizeof g->note, "%s", msg);
  g->note_until = GetTickCount64() + NOTE_MS;
}
static void show_status(Gui *g) {
  char t[1500];
  if (g->note[0] && GetTickCount64() < g->note_until) snprintf(t, sizeof t, "%s", g->note);
  else { g->note[0] = 0; smp_plug_status_text(g->P, t, sizeof t); }
  set_text(g->status, t);
  set_text(g->sfstatus, t);
}
static void show_sample(Gui *g) {
  char t[1500]; smp_plug_sample_text(g->P, t, sizeof t); set_text(g->sample, t);
}

/* the SoundFont player's settings, read from its state lines */
typedef struct { int one, bank, program, channel, pin[NCH], out[NCH]; } SfCfg;   /* pin: bank * 128 + program, or -1 */
static void sf_cfg(Gui *g, SfCfg *c) {
  SmpSynth *s = smp_plug_synth(g->P);
  int n = s ? smp_synth_script_write(s, NULL, 0) : 0, i;
  char *t = n > 0 ? malloc((size_t)n + 1) : NULL, *p;
  memset(c, 0, sizeof *c);
  for (i = 0; i < NCH; i++) c->pin[i] = -1;
  if (!t) return;
  smp_synth_script_write(s, t, n + 1);
  for (p = t; p && *p; ) {
    char *e = strchr(p, '\n'), w[32] = "", a[32] = ""; int x, y, z;
    if (e) *e = 0;
    if (sscanf(p, "%31s %31s", w, a) >= 1) {
      if (!strcmp(w, "sf_mode")) c->one = !strcmp(a, "one");
      else if (!strcmp(w, "sf_preset") && sscanf(p + 9, "%d %d", &x, &y) == 2) { c->bank = x; c->program = y; }
      else if (!strcmp(w, "sf_channel")) c->channel = strcmp(a, "all") ? atoi(a) : 0;
      else if (!strcmp(w, "sf_channel_preset") && sscanf(p + 17, "%d %d %d", &x, &y, &z) == 3 && x >= 1 && x <= NCH)
        c->pin[x - 1] = y * 128 + z;
      else if (!strcmp(w, "sf_output") && sscanf(p + 9, "%d %d", &x, &y) == 2 && x >= 1 && x <= NCH) c->out[x - 1] = y - 1;
    }
    p = e ? e + 1 : NULL;
  }
  free(t);
}
static SmpFont *sf_font(Gui *g) { SmpSynth *s = smp_plug_synth(g->P); return s ? smp_synth_font(s) : NULL; }
static void cb_add(HWND c, const char *s) {
  wchar_t w[400]; MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 400); SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)w);
}
static void cb_select(HWND c, int k) { if (SendMessageW(c, CB_GETCURSEL, 0, 0) != k) SendMessageW(c, CB_SETCURSEL, (WPARAM)k, 0); }

/* the preset lists: Preset's, and each channel's after its first entry; built again only when the font changes */
static void sf_lists(Gui *g) {
  SmpFont *f = sf_font(g);
  char key[1200], name[200], t[300]; int n = f ? smp_sf_preset_count(f) : 0, i, c, b, p;
  smp_plug_font_text(g->P, key, sizeof key);
  if (g->list_key[0] && (const void *)f == g->list_font && !strcmp(key, g->list_key)) return;
  g->list_font = f; snprintf(g->list_key, sizeof g->list_key, "%s", key);  /* an open list closes first (its entries are about to go), and nothing redraws until all 17 are built */
  for (c = 0; c <= NCH; c++) {
    HWND h = c < NCH ? g->chan[c] : g->sfpreset;
    if (SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0)) SendMessageW(h, CB_SHOWDROPDOWN, FALSE, 0);
    SendMessageW(h, WM_SETREDRAW, FALSE, 0);
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    SendMessageW(h, CB_INITSTORAGE, (WPARAM)(n + 2), (LPARAM)((n + 2) * 64 * sizeof(wchar_t)));
  }
  g->lacks_preset[0] = 0;
  for (c = 0; c < NCH; c++) {
    cb_add(g->chan[c], "From the song");
    g->from_song[c][0] = 0;                    /* the timer writes it */
    g->lacks_chan[c][0] = 0;
  }
  for (i = 0; i < n; i++) {
    if (!smp_sf_preset_info(f, i, &b, &p, name, sizeof name)) name[0] = 0, b = p = 0;
    snprintf(t, sizeof t, "%s, bank %d program %d", name, b, p + 1);
    cb_add(g->sfpreset, t);
    for (c = 0; c < NCH; c++) cb_add(g->chan[c], t);
  }
  for (c = 0; c <= NCH; c++) {
    HWND h = c < NCH ? g->chan[c] : g->sfpreset;
    SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(h, NULL, TRUE);
  }}
/* A chosen preset the font doesn't have: an entry after the presets (at base) saying so, selected, so a screen
   reader says what's wrong instead of nothing. want NULL: no such entry. Returns its index, or -1. */
static int lacks_entry(HWND c, char *have, int cap, int base, const char *want) {
  if (strcmp(have, want ? want : "")) {
    if (have[0]) SendMessageW(c, CB_DELETESTRING, (WPARAM)base, 0);
    if (want) cb_add(c, want);
    snprintf(have, (size_t)cap, "%s", want ? want : "");
  }
  return want ? base : -1;
}
static int sf_index(Gui *g, int bank, int program) {   /* the preset's place in the lists, or -1 */
  SmpFont *f = sf_font(g);
  return f ? smp_sf_find_preset(f, bank, program) : -1;
}

static bool in_list(HWND *list, int n, HWND h) { int i; for (i = 0; i < n; i++) if (list[i] == h) return true; return false; }
/* a window of the SoundFont page that its mode hides */
static bool mode_hides(Gui *g, HWND h) {
  return g->sf_one ? in_list(g->mode_all, g->nmode_all, h) : in_list(g->mode_one, g->nmode_one, h);
}
/* the SoundFont page shows its mode's controls; the focus never stays on one that hides */
static void sf_layout(Gui *g) {
  HWND f = GetFocus(); int i;
  if (g->page != PG_FONT) return;
  if (f && mode_hides(g, f)) focus(g->sfmode);
  for (i = 0; i < g->nmode_one; i++) ShowWindow(g->mode_one[i], g->sf_one ? SW_SHOW : SW_HIDE);
  for (i = 0; i < g->nmode_all; i++) ShowWindow(g->mode_all[i], g->sf_one ? SW_HIDE : SW_SHOW);
}

static void sf_show(Gui *g) {
  char t[1500], name[200], lacks[200]; SfCfg c; SmpFont *f; int i, b, p, n, k;
  smp_plug_font_text(g->P, t, sizeof t); set_text(g->sffile, t);
  sf_lists(g);
  sf_cfg(g, &c);
  f = sf_font(g);
  n = f ? smp_sf_preset_count(f) : 0;
  if ((bool)c.one != g->sf_one) { g->sf_one = c.one != 0; sf_layout(g); }
  cb_select(g->sfmode, c.one ? 1 : 0);
  k = sf_index(g, c.bank, c.program);
  snprintf(lacks, sizeof lacks, "Bank %d program %d (not in this font)", c.bank, c.program + 1);
  if (k < 0) k = lacks_entry(g->sfpreset, g->lacks_preset, sizeof g->lacks_preset, n, f ? lacks : NULL);
  else lacks_entry(g->sfpreset, g->lacks_preset, sizeof g->lacks_preset, n, NULL);
  cb_select(g->sfpreset, k);
  cb_select(g->sfchannel, c.channel);
  for (i = 0; i < NCH; i++) {
    k = c.pin[i] >= 0 ? sf_index(g, c.pin[i] / 128, c.pin[i] % 128) : -1;
    /* the first entry says what the song plays there now; never while the list is open (it would jump) */
    if (f && smp_plug_sf_channel_now(g->P, i + 1, &b, &p)) {
      int at = smp_sf_find_preset(f, b, p);
      if (at < 0 || !smp_sf_preset_info(f, at, NULL, NULL, name, sizeof name)) snprintf(name, sizeof name, "bank %d program %d", b, p + 1);
      snprintf(t, sizeof t, "From the song (now %s)", name);
    } else snprintf(t, sizeof t, "From the song");
    if (strcmp(t, g->from_song[i]) && !SendMessageW(g->chan[i], CB_GETDROPPEDSTATE, 0, 0)) {
      wchar_t w[400]; LRESULT was = SendMessageW(g->chan[i], CB_GETCURSEL, 0, 0);
      snprintf(g->from_song[i], sizeof g->from_song[i], "%s", t);
      MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 400);
      SendMessageW(g->chan[i], CB_DELETESTRING, 0, 0);
      SendMessageW(g->chan[i], CB_INSERTSTRING, 0, (LPARAM)w);
      if (was == 0) SendMessageW(g->chan[i], CB_SETCURSEL, 0, 0);     /* showing it: the new words show */
    }
    if (c.pin[i] >= 0 && k < 0 && f) {           /* pinned to a preset the font lacks */
      snprintf(lacks, sizeof lacks, "Bank %d program %d (not in this font)", c.pin[i] / 128, c.pin[i] % 128 + 1);
      k = lacks_entry(g->chan[i], g->lacks_chan[i], sizeof g->lacks_chan[i], n + 1, lacks) - 1;
    } else lacks_entry(g->chan[i], g->lacks_chan[i], sizeof g->lacks_chan[i], n + 1, NULL);
    cb_select(g->chan[i], c.pin[i] >= 0 ? (k >= 0 ? k + 1 : -1) : 0);
    cb_select(g->out[i], c.out[i]);
  }
}

/* a SoundFont combo box chose entry k: one state line to the player (one it won't take beeps) */
static void sf_choose(Gui *g, int id, int k) {
  char line[200] = ""; int b = 0, p = 0; SmpFont *f = sf_font(g); int n = f ? smp_sf_preset_count(f) : 0;
  if (k < 0) return;
  /* the entry saying the chosen preset isn't in the font: choosing it changes nothing */
  if ((id == ID_SFPRESET && k >= n) || (id >= ID_CHAN && id < ID_CHAN + NCH && k > n)) { sf_show(g); return; }
  if (id == ID_SFMODE) snprintf(line, sizeof line, "sf_mode %s", k ? "one" : "all");
  else if (id == ID_SFPRESET) { if (f && smp_sf_preset_info(f, k, &b, &p, NULL, 0)) snprintf(line, sizeof line, "sf_preset %d %d", b, p); }
  else if (id == ID_SFCHANNEL) { if (k) snprintf(line, sizeof line, "sf_channel %d", k); else snprintf(line, sizeof line, "sf_channel all"); }
  else if (id >= ID_CHAN && id < ID_CHAN + NCH) {
    if (!k) snprintf(line, sizeof line, "sf_channel_preset %d song", id - ID_CHAN + 1);
    else if (f && smp_sf_preset_info(f, k - 1, &b, &p, NULL, 0)) snprintf(line, sizeof line, "sf_channel_preset %d %d %d", id - ID_CHAN + 1, b, p);
  } else if (id >= ID_OUT && id < ID_OUT + NCH) snprintf(line, sizeof line, "sf_output %d %d", id - ID_OUT + 1, k + 1);
  if (!line[0] || !smp_plug_sf_set(g->P, line)) beep();
  sf_show(g);
}

static void show_page(Gui *g, int page, HWND to);
static void file_list(Gui *g, int kind, bool rescan);
static void refresh(Gui *g) {
  int id;
  smp_plug_poll_load(g->P);                   /* a background load that finished is swapped in */
  if (g->P->load_failed != g->fails_seen) {   /* one that failed: the reason in Status for a while, and a beep */
    g->fails_seen = g->P->load_failed;
    say_note(g, g->P->load_err); beep();
  }
  if (g->P->load_notes != g->notes_seen) {    /* a load stopped for one of the other kind: said in Status, no beep */
    g->notes_seen = g->P->load_notes;
    say_note(g, g->P->load_note);
  }
  if (g->P->e) smp_engine_collect(g->P->e);   /* replaced samples, when nothing is processing */
  if (g->page != PG_SETTINGS && g->page != (smp_plug_plays(g->P) ? PG_FONT : PG_SAMPLER)) {
    show_page(g, smp_plug_plays(g->P) ? PG_FONT : PG_SAMPLER, NULL);   /* the tab follows what plays (it refreshes) */
    return;
  }
  show_sample(g);
  if (g->page != PG_SETTINGS) file_list(g, g->page == PG_FONT, false);
  for (id = 0; id < SMP_NALLPARAMS; id++) show_param(g, id, false);
  show_status(g);
  if (g->page == PG_FONT) sf_show(g);
  if (g->page == PG_SETTINGS) show_settings(g);
}

static bool font_name(const wchar_t *n) {
  const wchar_t *dot = wcsrchr(n, L'.');
  return dot && (!_wcsicmp(dot, L".sf2") || !_wcsicmp(dot, L".sf3") || !_wcsicmp(dot, L".sfz"));
}
/* a file to load: LOAD_SAMPLE or LOAD_FONT (as the page's Browse or File combo box asks: a file of the
   other kind gives that player's error), or LOAD_DROPPED: by its extension, a .sf2 or .sf3 as the SoundFont
   (which then plays), anything else as the sample (which then plays) */
enum { LOAD_SAMPLE, LOAD_FONT, LOAD_DROPPED };
/* A sample loads in the background (the status says "Loading x.wav" or "Decoding x.dsf with FFmpeg" meanwhile, and
   a failure is told when it comes, in refresh); so does a SoundFont ("Loading x.sfz": an SFZ reads every sample
   file's header), which then plays. */
static void load_path_w(Gui *g, const wchar_t *w, int how) {
  char u[1100], err[700], msg[1900]; bool sf = how == LOAD_DROPPED ? font_name(w) : how == LOAD_FONT, dropped = how == LOAD_DROPPED;
  if (WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL) <= 0) { beep(); return; }
  if (!sf) {
    if (!smp_plug_load_start(g->P, u, dropped, err, sizeof err)) {
      const char *base = strrchr(u, '\\'), *b2 = strrchr(u, '/');
      if (b2 > base) base = b2;
      snprintf(msg, sizeof msg, "Can't load %s: %s", base ? base + 1 : u, err);
      say_note(g, msg); beep();
    } else g->note[0] = 0;
    refresh(g);
    return;
  }
  if (!smp_plug_load_font_start(g->P, u, err, sizeof err)) {
    const char *base = strrchr(u, '\\'), *b2 = strrchr(u, '/');
    if (b2 > base) base = b2;
    snprintf(msg, sizeof msg, "Can't load %s: %s", base ? base + 1 : u, err);
    say_note(g, msg); beep();
  } else g->note[0] = 0;
  refresh(g);
}

static bool audio_name(const wchar_t *n) {     /* the decode layer's list of the sampler's extensions */
  const wchar_t **ext; int k = smp_audio_extensions(&ext), i;
  const wchar_t *dot = wcsrchr(n, L'.');
  if (!dot) return false;
  for (i = 0; i < k; i++) if (!_wcsicmp(dot, ext[i])) return true;
  return false;
}
/* Browse's filter for samples, from the same list: "Audio files" then "All files" (made once) */
static const wchar_t *audio_filter(void) {
  static wchar_t f[1024]; static int made;
  if (!made) {
    const wchar_t **ext; int k = smp_audio_extensions(&ext), i; size_t at;
    wcscpy(f, L"Audio files");
    at = wcslen(f) + 1;
    for (i = 0; i < k && at + wcslen(ext[i]) + 40 < 1024; i++)
      at += (size_t)swprintf(f + at, 1024 - at, L"%s*%s", i ? L";" : L"", ext[i]);
    at++;                                     /* past the patterns' terminating zero */
    memcpy(f + at, L"All files\0*.*\0", sizeof L"All files\0*.*\0");
    made = 1;
  }
  return f;
}
static int by_name(const void *a, const void *b) { return _wcsicmp(*(const wchar_t *const *)a, *(const wchar_t *const *)b); }

/* what Browse and the File combo box work on: the sample (0) or the SoundFont (1) */
typedef struct {
  int (*path)(Plug *, char *, int);
  bool (*ours)(const wchar_t *);
  const wchar_t *filter, *title;
} FileKind;
static const FileKind kinds[2] = {
  {smp_plug_sample_path, audio_name, NULL, L"Load a sample"},             /* filter: audio_filter() */
  {smp_plug_font_path, font_name, L"SoundFonts\0*.sf2;*.sf3;*.sfz\0All files\0*.*\0", L"Load a SoundFont"},
};

/* The File combo box: the files of its kind in the loaded file's folder, by name (alphabetical, ignoring case),
   the loaded one selected (it is listed even if its kind's extensions don't match it). With nothing loaded, one
   entry: "No file loaded", or "<name> (missing)". Its entries change only when the listing does, and never while its
   list is open; rescan: list the folder again even if the loaded file is the same (the combo box just got the
   focus: files added in Explorer show up). */
static void file_list(Gui *g, int kind, bool rescan) {
  const FileKind *K = &kinds[kind];
  HWND c = g->file[kind];
  char path[1100], key[1110]; wchar_t w[1100], pat[1200], *slash, *name = NULL, **names = NULL, one[1200];
  WIN32_FIND_DATAW fd; HANDLE f; int n = 0, cap = 0, i, at = -1, rc, same;
  if (!c) return;
  if (SendMessageW(c, CB_GETDROPPEDSTATE, 0, 0)) { g->file_key[kind][0] = 0; return; }   /* again once it closes */
  rc = K->path(g->P, path, sizeof path);
  snprintf(key, sizeof key, "%d%s", rc, path);
  if (!rescan && !strcmp(key, g->file_key[kind])) return;
  snprintf(g->file_key[kind], sizeof g->file_key[kind], "%s", key);
  g->file_dir[kind][0] = 0;
  if (rc && MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1100)) {
    for (slash = w; *slash; slash++) if (*slash == L'/') *slash = L'\\';
    slash = wcsrchr(w, L'\\');
    name = slash ? slash + 1 : w;
    if (slash) *slash = 0;
    else if (rc == 1) rc = 3;                 /* a name without a folder: just that entry, no folder to list */
  } else rc = 0;
  if (rc == 1) {                              /* the folder's files of this kind, and the loaded one */
    wcscpy(g->file_dir[kind], w);
    swprintf(pat, 1200, L"%s\\*", w);
    f = FindFirstFileW(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
      do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (!K->ours(fd.cFileName) && _wcsicmp(fd.cFileName, name))) continue;
        if (n == cap) {
          wchar_t **nn = realloc(names, sizeof *names * (size_t)(cap = cap ? cap * 2 : 64));
          if (!nn) break;
          names = nn;
        }
        names[n] = _wcsdup(fd.cFileName);
        if (names[n]) n++;
      } while (FindNextFileW(f, &fd));
      FindClose(f);
    }
    qsort(names, (size_t)n, sizeof *names, by_name);
    for (i = 0; i < n; i++) if (!_wcsicmp(names[i], name)) at = i;
  }
  if (rc != 1 || at < 0) {                    /* nothing loaded, missing, or the folder can't be read: one entry */
    for (i = 0; i < n; i++) free(names[i]);
    n = 0; at = 0;
    if (rc) swprintf(one, 1200, rc == 2 ? L"%s (missing)" : L"%s", name); else wcscpy(one, L"No file loaded");
    if (!names) names = malloc(sizeof *names);
    if (names && (names[0] = _wcsdup(one)) != NULL) n = 1;
  }
  /* the same entries as now: only the selection moves (also safe inside the combo box's own notification) */
  same = (int)SendMessageW(c, CB_GETCOUNT, 0, 0) == n;
  for (i = 0; same && i < n; i++) {
    wchar_t t[1200];
    if (SendMessageW(c, CB_GETLBTEXTLEN, (WPARAM)i, 0) >= 1200) { same = 0; break; }
    SendMessageW(c, CB_GETLBTEXT, (WPARAM)i, (LPARAM)t);
    same = !wcscmp(t, names[i]);
  }
  if (!same) {
    SendMessageW(c, WM_SETREDRAW, FALSE, 0);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)names[i]);
    SendMessageW(c, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(c, NULL, TRUE);
  }
  g->file_live[kind] = rc == 1 && n > 0 && at >= 0 && g->file_dir[kind][0];
  cb_select(c, n ? at : -1);
  for (i = 0; i < n; i++) free(names[i]);
  free(names);
}

/* the File combo box chose entry k: that file loads at once through the page's own loader; if it won't, the
   reason shows in Status (with a beep) and the combo box goes back to the loaded file */
static void file_chosen(Gui *g, int kind, int k) {
  HWND c = g->file[kind]; wchar_t t[600], out[1800];
  if (k < 0 || !g->file_live[kind] || SendMessageW(c, CB_GETLBTEXTLEN, (WPARAM)k, 0) >= 600) { file_list(g, kind, true); return; }
  SendMessageW(c, CB_GETLBTEXT, (WPARAM)k, (LPARAM)t);
  swprintf(out, 1800, L"%s\\%s", g->file_dir[kind], t);
  load_path_w(g, out, kind ? LOAD_FONT : LOAD_SAMPLE);
  file_list(g, kind, true);                   /* the loaded file selected (again, when the load failed) */
}

static void browse_file(Gui *g, int kind) {
  const FileKind *K = &kinds[kind];
  wchar_t file[1100] = L"", dirw[1100] = L""; char path[1100]; OPENFILENAMEW of; int ok;
  if (K->path(g->P, path, sizeof path) && MultiByteToWideChar(CP_UTF8, 0, path, -1, dirw, 1100)) {
    wchar_t *s = wcsrchr(dirw, L'\\'), *s2 = wcsrchr(dirw, L'/');
    if (s2 > s) s = s2;
    if (s) *s = 0; else dirw[0] = 0;
  }
  memset(&of, 0, sizeof of);
  of.lStructSize = sizeof of;
  of.hwndOwner = GetAncestor(g->win, GA_ROOT);
  of.lpstrFilter = K->filter ? K->filter : audio_filter();
  of.lpstrFile = file; of.nMaxFile = 1100;
  of.lpstrInitialDir = dirw[0] ? dirw : NULL;
  of.lpstrTitle = K->title;
  of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
  modal_begin(g);
  ok = test_dialog ? stub_dialog(g, file, 1100) : GetOpenFileNameW(&of);
  if (modal_end(g)) return;               /* the window (and maybe the plugin) went while the dialog was open */
  if (ok) load_path_w(g, file, kind ? LOAD_FONT : LOAD_SAMPLE);   /* this page's player, whatever the file */
  focus(kind ? g->sfbrowse : g->browse);
}

/* ---- drag and drop: a file dropped anywhere on the window loads ---- */
static FORMATETC hdrop_fmt = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
static HRESULT STDMETHODCALLTYPE d_qi(IDropTarget *t, REFIID r, void **o) {
  if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDropTarget)) { *o = t; return S_OK; }
  *o = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE d_ref(IDropTarget *t) { (void)t; return 1; }   /* lives as long as the window */
static HRESULT STDMETHODCALLTYPE d_enter(IDropTarget *t, IDataObject *o, DWORD k, POINTL p, DWORD *eff) {
  Drop *d = (Drop *)t; (void)k; (void)p;
  d->ok = o && o->lpVtbl->QueryGetData(o, &hdrop_fmt) == S_OK;
  *eff = d->ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE d_over(IDropTarget *t, DWORD k, POINTL p, DWORD *eff) {
  (void)k; (void)p; *eff = ((Drop *)t)->ok ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK;
}
static HRESULT STDMETHODCALLTYPE d_leave(IDropTarget *t) { ((Drop *)t)->ok = 0; return S_OK; }
static void drop_hdrop(Gui *g, HDROP h) {
  wchar_t w[1100];
  if (DragQueryFileW(h, 0, w, 1100)) load_path_w(g, w, LOAD_DROPPED);
}
/* The drop only notes the file and posts WM_DROPPED: the load (maybe a decode) runs after the drag has returned, so
   Explorer or the Media Explorer isn't kept waiting, and nothing loads inside OLE's drag loop. */
static HRESULT STDMETHODCALLTYPE d_drop(IDropTarget *t, IDataObject *o, DWORD k, POINTL p, DWORD *eff) {
  Drop *d = (Drop *)t; STGMEDIUM m; (void)k; (void)p;
  *eff = DROPEFFECT_NONE;
  if (o && d->g && !d->g->closing && SUCCEEDED(o->lpVtbl->GetData(o, &hdrop_fmt, &m))) {
    HDROP h = (HDROP)GlobalLock(m.hGlobal);
    if (h) {
      if (DragQueryFileW(h, 0, d->g->dropped, 1100) && d->g->win) {
        PostMessageW(d->g->win, WM_DROPPED, 0, 0);
        *eff = DROPEFFECT_COPY;
      }
      GlobalUnlock(m.hGlobal);
    }
    ReleaseStgMedium(&m);
  }
  d->ok = 0;
  return S_OK;
}
static IDropTargetVtbl drop_vtbl = {d_qi, d_ref, d_ref, d_enter, d_over, d_leave, d_drop};

/* REAPER has OLE going on its main thread already (S_FALSE: one more reference, released on close). If this thread
   can't have it (RPC_E_CHANGED_MODE) or registering fails, WM_DROPFILES does the same job. */
static void drop_start(Gui *g) {
  HRESULT hr = OleInitialize(NULL);
  g->ole_init = SUCCEEDED(hr);
  g->drop.t.lpVtbl = &drop_vtbl; g->drop.g = g;
  g->drop_registered = g->ole_init && RegisterDragDrop(g->win, &g->drop.t) == S_OK;
  if (!g->drop_registered) DragAcceptFiles(g->win, TRUE);
}

/* ---- Tab and focus ---- */
/* the next control in the page's Tab order that shows (the SoundFont page hides one mode's controls); past either
   end, out of the plugin */
static void tab_key(Gui *g, HWND h, bool back) {
  HWND *o = g->order[g->page]; int n = g->norder[g->page], at = -1, i, d = back ? -1 : 1;
  for (i = 0; i < n; i++) if (o[i] == h) at = i;
  if (at >= 0) for (i = at + d; i >= 0 && i < n; i += d) if (IsWindowVisible(o[i])) { focus(o[i]); return; }
  if (tab_out(g->win, back)) return;
  for (i = back ? n - 1 : 0; i >= 0 && i < n; i += d) if (IsWindowVisible(o[i])) { focus(o[i]); return; }
}

/* If the focus was ours and is now on nothing that shows (its page hidden), or on the window itself (REAPER focuses
   it when one comes back to it), it goes on to the control last used, or the page's first. */
static void keep_focus(Gui *g) {
  HWND h = GetFocus(), to;
  if (!g->win || g->closing || (h && h != g->win && !(IsChild(g->win, h) && !IsWindowVisible(h)))) return;
  if (!h && !g->had_focus) return;
  to = g->last && IsChild(g->win, g->last) && IsWindowVisible(g->last) ? g->last : g->order[g->page][0];
  focus(to);
}

/* a window that shows on the page: one of its own, and not one its SoundFont mode hides */
static bool shows_on(Gui *g, int page, HWND h) {
  return in_list(g->all[page], g->nall[page], h) && !(page == PG_FONT && mode_hides(g, h));
}
/* Shows a page. The focus goes to `to`, or, when it was on one of ours that won't show, to the page's first control
   (the tab control on a main page); focus outside the plugin stays where it is. The new page shows before the old
   one hides, so the focus never falls to nothing. */
static void show_page(Gui *g, int page, HWND to) {
  HWND f = GetFocus(); int i, q;
  if (page == PG_SETTINGS) { rescan_used(g); find_ffmpeg(g); show_settings(g); }
  if (page == PG_FONT) { SfCfg c; sf_cfg(g, &c); g->sf_one = c.one != 0; }
  g->page = page;
  if (page != PG_SETTINGS) {                  /* the tab control shows the page's tab */
    int k = page == PG_FONT ? 1 : 0;
    g->setting_tab = true;
    if (SendMessageW(g->tabs, TCM_GETCURFOCUS, 0, 0) != k) SendMessageW(g->tabs, TCM_SETCURFOCUS, (WPARAM)k, 0);
    if (SendMessageW(g->tabs, TCM_GETCURSEL, 0, 0) != k) SendMessageW(g->tabs, TCM_SETCURSEL, (WPARAM)k, 0);
    g->setting_tab = false;
  }
  for (i = 0; i < g->nall[page]; i++) if (shows_on(g, page, g->all[page][i])) ShowWindow(g->all[page][i], SW_SHOW);
  if (to) focus(to);
  else if (f && (f == g->win || IsChild(g->win, f)) && !shows_on(g, page, f)) focus(g->order[page][0]);
  for (q = 0; q < NPAGES; q++)
    for (i = 0; i < g->nall[q]; i++) if (!shows_on(g, page, g->all[q][i])) ShowWindow(g->all[q][i], SW_HIDE);
  if (page != PG_SETTINGS) refresh(g);
}

/* the user chose a tab: that player plays, its page shows, and the focus is on the tab control */
static void set_tab(Gui *g, int which) {
  smp_plug_set_plays(g->P, which);
  show_page(g, which ? PG_FONT : PG_SAMPLER, g->tabs);
}

static void click(Gui *g, int id) {
  int m;
  switch (id) {
    case ID_BROWSE: browse_file(g, 0); break;
    case ID_SFBROWSE: browse_file(g, 1); break;
    case ID_SETTINGS: case ID_SFSETTINGS: show_page(g, PG_SETTINGS, NULL); break;
    case ID_BACK:                             /* to the main page that plays, on its Settings button */
      m = smp_plug_plays(g->P) ? PG_FONT : PG_SAMPLER;
      show_page(g, m, m == PG_FONT ? g->sfsettings : g->settings);
      break;
    case ID_DIRBROWSE: browse_folder(g); break;
    case ID_FFBROWSE: browse_ffmpeg(g); break;
    case ID_CLEAR: clear_cache(g); break;
  }
}

static Slider *slider_of(Gui *g, HWND h) {
  int i;
  for (i = 0; i < g->nsl; i++) if (g->sl[i].tb == h || g->sl[i].ed == h) return &g->sl[i];
  return NULL;
}

static LRESULT CALLBACK ctl_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
  Gui *g = (Gui *)data;
  bool shift, ctrl;
  Slider *s;
  /* every key comes to the control (Tab, Enter and the arrows included), not to the window or REAPER around it */
  if (m == WM_GETDLGCODE) return DefSubclassProc(h, m, w, l) | DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTTAB | DLGC_WANTCHARS;
  if (g->closing) return DefSubclassProc(h, m, w, l);
  if (m == WM_SETFOCUS) { g->last = h; g->had_focus = true; }
  if (m == WM_SETFOCUS && (id == ID_FILE || id == ID_SFFILE)) file_list(g, id == ID_SFFILE, true);   /* new files show */
  if (m == WM_KILLFOCUS && !(w && IsChild(g->win, (HWND)w))) g->had_focus = false;   /* gone out of the window */
  shift = GetKeyState(VK_SHIFT) < 0; ctrl = GetKeyState(VK_CONTROL) < 0;
  s = slider_of(g, h);
  if (m == WM_KEYDOWN && w == VK_TAB && ctrl) {           /* Ctrl+Tab, Ctrl+Shift+Tab: the next or previous tab */
    if (g->page != PG_SETTINGS) {
      int n = (int)SendMessageW(g->tabs, TCM_GETITEMCOUNT, 0, 0), k = (int)SendMessageW(g->tabs, TCM_GETCURSEL, 0, 0);
      if (s && h == s->ed) edit_end(g, s, true, false);  /* a value being typed is read first */
      if (n > 0) set_tab(g, ((k < 0 ? 0 : k) + (shift ? n - 1 : 1)) % n);
    }
    return 0;
  }
  if (s && h == s->ed) {                                  /* typing a value in place of its slider */
    if (m == WM_KEYDOWN && w == VK_TAB) { tab_key(g, s->tb, shift); return 0; }   /* leaving the box reads it */
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) { edit_end(g, s, w == VK_RETURN, true); return 0; }
    if (m == WM_CHAR && (w == 9 || w == '\r' || w == '\n' || w == 27)) return 0;
    if (m == WM_KILLFOCUS) edit_end(g, s, true, false);
    return DefSubclassProc(h, m, w, l);
  }
  if (m == WM_KEYDOWN && w == VK_TAB) { tab_key(g, h, shift); return 0; }
  if (m == WM_CHAR && w == 9) return 0;                                   /* the Tab's character */
  if (m == WM_CHAR && (w == '\r' || w == '\n') && !is_class(h, L"ComboBox")) return 0;
  if (s) {                                                /* a slider: the keys are ours, not the trackbar's */
    double by = shift ? 10 : ctrl ? 0.1 : 1, lo, hi, step;
    if (m == WM_KEYDOWN) {
      switch (w) {
        case VK_RETURN: edit_begin(g, s, 0); return 0;
        case VK_UP: case VK_RIGHT: sl_nudge(g, s, by); return 0;
        case VK_DOWN: case VK_LEFT: sl_nudge(g, s, -by); return 0;
        case VK_PRIOR: sl_nudge(g, s, 10); return 0;
        case VK_NEXT: sl_nudge(g, s, -10); return 0;
        case VK_HOME: case VK_END: sl_range(s, &lo, &hi, &step); sl_set(g, s, w == VK_HOME ? lo : hi); return 0;
      }
    }
    if (m == WM_CHAR && w > ' ' && w != 127) { edit_begin(g, s, (wchar_t)w); return 0; }   /* typing starts the box */
    if (m == WM_CHAR) return 0;
  }
  if (m == WM_KEYDOWN && w == VK_RETURN) {
    if (id == ID_DIR) { apply_dir(g); SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    if (is_class(h, L"Button")) { SendMessageW(h, BM_CLICK, 0, 0); return 0; }
    if (is_class(h, L"Edit")) return 0;
  }
  if (m == WM_KILLFOCUS && id == ID_DIR) apply_dir(g);
  return DefSubclassProc(h, m, w, l);
}

static LRESULT CALLBACK win_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  Gui *g = (Gui *)GetWindowLongPtrW(h, GWLP_USERDATA);
  if (!g || g->closing) return DefWindowProcW(h, m, w, l);
  switch (m) {
    case WM_TIMER:
      if (w == T_OPEN) { KillTimer(h, T_OPEN); if (!GetFocus() || GetFocus() == h || GetAncestor(GetFocus(), GA_ROOT) == GetAncestor(h, GA_ROOT)) focus(g->order[g->page][0]); }
      else refresh(g);
      return 0;
    case WM_COMMAND: {
      int id = LOWORD(w), code = HIWORD(w);
      if (code == CBN_SELCHANGE && id >= ID_VAL && id < ID_VAL + SMP_NALLPARAMS) {
        LRESULT k = SendMessageW((HWND)l, CB_GETCURSEL, 0, 0);
        if (k >= 0) smp_plug_set_param(g->P, id - ID_VAL, (double)k);
      } else if (code == CBN_SELCHANGE && (id == ID_STRETCHER || id == ID_KEEPTONE || id == ID_RAWRATE ||
                                           id == ID_RAWCH || id == ID_RAWFMT || id == ID_RAWEND)) {
        LRESULT k = SendMessageW((HWND)l, CB_GETCURSEL, 0, 0);
        if (k >= 0) choose(g, id, (int)k);
      } else if (code == CBN_SELCHANGE && (id == ID_FILE || id == ID_SFFILE)) {
        file_chosen(g, id == ID_SFFILE, (int)SendMessageW((HWND)l, CB_GETCURSEL, 0, 0));
      } else if (code == CBN_CLOSEUP && (id == ID_FILE || id == ID_SFFILE)) {
        file_list(g, id == ID_SFFILE, true);   /* its list closed: back on the loaded file (a choice that failed) */
      } else if (code == CBN_SELCHANGE && (id == ID_SFMODE || id == ID_SFPRESET || id == ID_SFCHANNEL ||
                                           (id >= ID_CHAN && id < ID_CHAN + NCH) || (id >= ID_OUT && id < ID_OUT + NCH))) {
        sf_choose(g, id, (int)SendMessageW((HWND)l, CB_GETCURSEL, 0, 0));
      } else if (code == BN_CLICKED) click(g, id);
      return 0;
    }
    case WM_HSCROLL: {                         /* the mouse on a slider (its keys never reach the trackbar) */
      Slider *s = l ? slider_of(g, (HWND)l) : NULL;
      int code = LOWORD(w);
      if (s && (code == TB_THUMBTRACK || code == TB_THUMBPOSITION || code == TB_PAGEUP || code == TB_PAGEDOWN)) sl_dragged(g, s);
      return 0;
    }
    case WM_NOTIFY: {                          /* the tab control's own keys (Left, Right) or the mouse chose a tab */
      const NMHDR *n = (const NMHDR *)l;
      if (n && n->hwndFrom == g->tabs && n->code == TCN_SELCHANGE && !g->setting_tab)
        set_tab(g, (int)SendMessageW(g->tabs, TCM_GETCURSEL, 0, 0));
      else break;                              /* not ours: the default */
      return 0;
    }
    case WM_SETFOCUS: keep_focus(g); return 0;
    case WM_DROPFILES: drop_hdrop(g, (HDROP)w); DragFinish((HDROP)w); return 0;
    case WM_DROPPED: if (g->dropped[0]) { wchar_t f[1100]; wcscpy(f, g->dropped); g->dropped[0] = 0; load_path_w(g, f, LOAD_DROPPED); } return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

/* ---- making the window ---- */
static HWND ctl(Gui *g, int page, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
  HWND c = CreateWindowExW(!wcscmp(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | style,
                           x, y, w, h, g->win, (HMENU)(INT_PTR)id, module(), NULL);
  if (!c) return NULL;
  SendMessageW(c, WM_SETFONT, (WPARAM)g->font, FALSE);
  if (g->nall[page] < PAGE_CTLS) g->all[page][g->nall[page]++] = c;
  if (style & WS_TABSTOP) {
    SetWindowSubclass(c, ctl_proc, (UINT_PTR)id, (DWORD_PTR)g);
    if (g->norder[page] < PAGE_STOPS) g->order[page][g->norder[page]++] = c;
  }
  return c;                                   /* hidden: the page shows it (show_page) */
}
/* a label, then its control: the order a screen reader names it by */
static HWND labelled(Gui *g, int page, const char *label, const wchar_t *cls, DWORD style, int x, int y, int lw, int cw, int ch, int id) {
  wchar_t lab[96];
  MultiByteToWideChar(CP_UTF8, 0, label, -1, lab, 96);
  ctl(g, page, L"STATIC", lab, SS_LEFT, x, y + 4, lw, 20, -1);
  return ctl(g, page, cls, L"", style, x + lw + 4, y, cw, ch, id);
}

/* a label, then the edit box that stands in for the slider (hidden; outside the Tab order), then the slider */
static Slider *slider(Gui *g, int page, const char *label, int what, int x, int y, int lw, int cw, int id) {
  Slider *s = &g->sl[g->nsl]; wchar_t lab[96]; double lo, hi, step, n;
  int i = g->nsl++;
  MultiByteToWideChar(CP_UTF8, 0, label, -1, lab, 96);
  s->what = what;
  ctl(g, page, L"STATIC", lab, SS_LEFT, x, y + 4, lw, 20, -1);
  s->ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, x + lw + 4, y, cw, 24, g->win,
                          (HMENU)(INT_PTR)(ID_EDIT + i), module(), NULL);
  SendMessageW(s->ed, WM_SETFONT, (WPARAM)g->font, FALSE);
  SendMessageW(s->ed, EM_SETLIMITTEXT, 60, 0);
  SetWindowSubclass(s->ed, ctl_proc, (UINT_PTR)(ID_EDIT + i), (DWORD_PTR)g);
  s->tb = ctl(g, page, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH, x + lw + 4, y, cw, 24, id);
  sl_range(s, &lo, &hi, &step);
  n = floor((hi - lo) / step + 0.5);
  SendMessageW(s->tb, TBM_SETRANGEMIN, FALSE, 0);
  SendMessageW(s->tb, TBM_SETRANGEMAX, FALSE, (LPARAM)(n > 1000 ? 1000 : n < 1 ? 1 : n));
  SendMessageW(s->tb, TBM_SETPAGESIZE, 0, 10);
  annotate(g, s->tb, PROPID_ACC_NAME, label);
  annotate(g, s->ed, PROPID_ACC_NAME, label);
  sl_show(g, s, true);
  return s;
}

static HWND param_box(Gui *g, int page, int id, int x, int y, int lw, int cw) {
  const DWORD T = WS_TABSTOP;
  HWND c;
  if (is_choice(id)) {
    int n = (int)(smp_param_info(id)->hi + 0.5), k;
    c = labelled(g, page, smp_param_label[id], L"COMBOBOX", T | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, lw, cw, 200, ID_VAL + id);
    for (k = 0; k <= n; k++) {
      char t[64]; wchar_t wt[64];
      param_text(id, k, t, sizeof t);
      MultiByteToWideChar(CP_UTF8, 0, t, -1, wt, 64);
      SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)wt);
    }
  } else {
    g->sl_of[id] = slider(g, page, smp_param_label[id], id, x, y, lw, cw, ID_VAL + id);
    c = g->sl_of[id]->tb;
  }
  g->val[id] = c;
  return c;
}

#define N_REST (SMP_NPARAMS - 2)
#define REST_ROWS ((N_REST + 1) / 2)
#define W_HEIGHT (8 + (4 + REST_ROWS + 2) * ROW + 8)   /* the Sampler page, the taller: the tabs, 3 rows, the
                                                           rest in two columns, Status and Settings */

static bool build(Gui *g, HWND parent) {
  HINSTANCE hi = module();
  WNDCLASSW wc; NONCLIENTMETRICSW nm; const DWORD T = WS_TABSTOP; int id, k, y;
  INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES};
  InitCommonControlsEx(&icc);
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = win_proc; wc.hInstance = hi; wc.lpszClassName = CLASS_NAME;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  RegisterClassW(&wc);                         /* fails harmlessly once it's registered */
  /* WS_EX_CONTROLPARENT: Tab moves from REAPER's FX window into these controls */
  g->win = CreateWindowExW(WS_EX_CONTROLPARENT, CLASS_NAME, L"Samplosis", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                           0, 0, W_WIDTH, W_HEIGHT, parent, NULL, hi, NULL);
  if (!g->win) return false;
  SetWindowLongPtrW(g->win, GWLP_USERDATA, (LONG_PTR)g);
  memset(&nm, 0, sizeof nm); nm.cbSize = sizeof nm;
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0);
  g->font = CreateFontIndirectW(&nm.lfMessageFont);
  drop_start(g);                               /* OLE first: the sliders' words need COM */
  if (FAILED(CoCreateInstance(&CLSID_AccPropServices, NULL, CLSCTX_INPROC_SERVER, &IID_IAccPropServices, (void **)&g->aps))) g->aps = NULL;

  /* the tab control: the first control of both main pages */
  y = 8;
  g->tabs = ctl(g, PG_SAMPLER, WC_TABCONTROLW, L"", T | WS_CLIPSIBLINGS, 8, y, 300, 26, ID_TABS);
  {
    TCITEMW it; wchar_t t0[] = L"Sampler", t1[] = L"SoundFont";
    memset(&it, 0, sizeof it); it.mask = TCIF_TEXT;
    it.pszText = t0; SendMessageW(g->tabs, TCM_INSERTITEMW, 0, (LPARAM)&it);
    it.pszText = t1; SendMessageW(g->tabs, TCM_INSERTITEMW, 1, (LPARAM)&it);
  }
  annotate(g, g->tabs, PROPID_ACC_NAME, "Player");
  g->all[PG_FONT][g->nall[PG_FONT]++] = g->tabs;
  g->order[PG_FONT][g->norder[PG_FONT]++] = g->tabs;

  /* the Sampler page, in Tab order */
  y += ROW;
  g->sample = labelled(g, 0, "Sample", L"EDIT", T | ES_AUTOHSCROLL | ES_READONLY, 8, y, 120, W_WIDTH - 140, 24, ID_SAMPLE);
  y += ROW;
  g->browse = ctl(g, 0, L"BUTTON", L"Browse", T | BS_PUSHBUTTON, 132, y, 100, 26, ID_BROWSE);
  g->file[0] = labelled(g, PG_SAMPLER, "File", L"COMBOBOX", T | CBS_DROPDOWNLIST | WS_VSCROLL, 240, y, 40, 470, 320, ID_FILE);
  y += ROW;
  param_box(g, PG_SAMPLER, SMP_P_KEEP_LENGTH, 8, y, 120, 130);
  param_box(g, PG_SAMPLER, SMP_P_MODE, 384, y, 160, 130);
  y += ROW;
  for (id = 0, k = 0; id < SMP_NPARAMS; id++) {
    if (id == SMP_P_KEEP_LENGTH || id == SMP_P_MODE) continue;
    param_box(g, PG_SAMPLER, id, k % 2 ? 384 : 8, y + (k / 2) * ROW, k % 2 ? 160 : 120, k % 2 ? 200 : 236);
    k++;
  }
  y += REST_ROWS * ROW;
  g->status = labelled(g, 0, "Status", L"EDIT", T | ES_AUTOHSCROLL | ES_READONLY, 8, y, 120, W_WIDTH - 140, 24, ID_STATUS);
  y += ROW;
  g->settings = ctl(g, 0, L"BUTTON", L"Settings", T | BS_PUSHBUTTON, 132, y, 100, 26, ID_SETTINGS);

  /* the SoundFont page, in Tab order (after the tab control) */
  y = 8 + ROW;
  g->sffile = labelled(g, PG_FONT, "SoundFont", L"EDIT", T | ES_AUTOHSCROLL | ES_READONLY, 8, y, 120, W_WIDTH - 140, 24, ID_FONT);
  y += ROW;
  g->sfbrowse = ctl(g, PG_FONT, L"BUTTON", L"Browse", T | BS_PUSHBUTTON, 132, y, 100, 26, ID_SFBROWSE);
  g->file[1] = labelled(g, PG_FONT, "File", L"COMBOBOX", T | CBS_DROPDOWNLIST | WS_VSCROLL, 240, y, 40, 470, 320, ID_SFFILE);
  y += ROW;
  g->sfmode = labelled(g, PG_FONT, "Mode", L"COMBOBOX", T | CBS_DROPDOWNLIST | WS_VSCROLL, 8, y, 120, 200, 200, ID_SFMODE);
  cb_add(g->sfmode, "All channels");
  cb_add(g->sfmode, "One preset");
  y += ROW;
  k = g->nall[PG_FONT];                        /* One preset: in the place of the channels */
  g->sfpreset = labelled(g, PG_FONT, "Preset", L"COMBOBOX", T | CBS_DROPDOWNLIST | WS_VSCROLL, 8, y, 120, 400, 320, ID_SFPRESET);
  g->sfchannel = labelled(g, PG_FONT, "Channel", L"COMBOBOX", T | CBS_DROPDOWNLIST | WS_VSCROLL, 8, y + ROW, 120, 200, 320, ID_SFCHANNEL);
  cb_add(g->sfchannel, "All channels");
  for (id = 1; id <= NCH; id++) { char t[32]; snprintf(t, sizeof t, "Channel %d", id); cb_add(g->sfchannel, t); }
  for (; k < g->nall[PG_FONT] && g->nmode_one < 4; k++) g->mode_one[g->nmode_one++] = g->all[PG_FONT][k];
  k = g->nall[PG_FONT];                        /* All channels: two columns of 8, a channel and its output a row */
  for (id = 0; id < NCH; id++) {
    char t[48]; int x = id < 8 ? 8 : 384, yy = y + (id % 8) * ROW, o;
    snprintf(t, sizeof t, "Channel %d", id + 1);
    g->chan[id] = labelled(g, PG_FONT, t, L"COMBOBOX", T | CBS_DROPDOWNLIST | WS_VSCROLL, x, yy, 66, 120, 320, ID_CHAN + id);
    SendMessageW(g->chan[id], CB_SETDROPPEDWIDTH, 320, 0);
    snprintf(t, sizeof t, "Channel %d output", id + 1);
    g->out[id] = labelled(g, PG_FONT, t, L"COMBOBOX", T | CBS_DROPDOWNLIST | WS_VSCROLL, x + 194, yy, 96, 76, 320, ID_OUT + id);
    for (o = 1; o <= NCH; o++) { snprintf(t, sizeof t, "Output %d", o); cb_add(g->out[id], t); }
  }
  for (; k < g->nall[PG_FONT] && g->nmode_all < 4 * NCH; k++) g->mode_all[g->nmode_all++] = g->all[PG_FONT][k];
  y += 8 * ROW;
  param_box(g, PG_FONT, SMP_P_SF_VOLUME, 8, y, 120, 236);
  param_box(g, PG_FONT, SMP_P_SF_VOICES, 384, y, 160, 200);
  y += ROW;
  param_box(g, PG_FONT, SMP_P_SF_TRANSPOSE, 8, y, 120, 236);
  param_box(g, PG_FONT, SMP_P_SF_EFFECTS, 384, y, 160, 200);
  y += ROW;
  g->sfstatus = labelled(g, PG_FONT, "Status", L"EDIT", T | ES_AUTOHSCROLL | ES_READONLY, 8, y, 120, W_WIDTH - 140, 24, ID_SFSTATUS);
  y += ROW;
  g->sfsettings = ctl(g, PG_FONT, L"BUTTON", L"Settings", T | BS_PUSHBUTTON, 132, y, 100, 26, ID_SFSETTINGS);

  /* Settings */
  y = 8;
  g->stretcher = labelled(g, 1, "Stretcher", L"COMBOBOX", T | CBS_DROPDOWNLIST | WS_VSCROLL, 8, y, 140, 260, 200, ID_STRETCHER);
  SendMessageW(g->stretcher, CB_ADDSTRING, 0, (LPARAM)L"Signalsmith Stretch, fast");
  SendMessageW(g->stretcher, CB_ADDSTRING, 0, (LPARAM)L"Rubber Band, slower");
  y += ROW;
  g->keeptone = labelled(g, 1, "Keep the tone", L"COMBOBOX", T | CBS_DROPDOWNLIST | WS_VSCROLL, 8, y, 140, 260, 200, ID_KEEPTONE);
  SendMessageW(g->keeptone, CB_ADDSTRING, 0, (LPARAM)L"Off");
  SendMessageW(g->keeptone, CB_ADDSTRING, 0, (LPARAM)L"On");
  y += ROW;
  g->dir = labelled(g, 1, "Cache folder", L"EDIT", T | ES_AUTOHSCROLL, 8, y, 140, 470, 24, ID_DIR);
  SendMessageW(g->dir, EM_SETLIMITTEXT, 1000, 0);
  g->dirbrowse = ctl(g, 1, L"BUTTON", L"Browse folder", T | BS_PUSHBUTTON, 630, y, 120, 26, ID_DIRBROWSE);
  y += ROW;
  g->limit = slider(g, 1, "Cache size limit", SL_LIMIT, 8, y, 140, 160, ID_LIMIT);
  y += ROW;
  g->rawrate = labelled(g, 1, "Raw sample rate", L"COMBOBOX", T | CBS_DROPDOWNLIST | WS_VSCROLL, 8, y, 140, 260, 320, ID_RAWRATE);
  {
    const int *r; int n = smp_raw_rates(&r), i;
    for (i = 0; i < n; i++) { char t[32]; snprintf(t, sizeof t, "%d Hz", r[i]); cb_add(g->rawrate, t); }
  }
  y += ROW;
  g->rawch = labelled(g, 1, "Raw channels", L"COMBOBOX", T | CBS_DROPDOWNLIST | WS_VSCROLL, 8, y, 140, 260, 200, ID_RAWCH);
  cb_add(g->rawch, "Mono");
  cb_add(g->rawch, "Stereo");
  y += ROW;
  g->rawfmt = labelled(g, 1, "Raw sample format", L"COMBOBOX", T | CBS_DROPDOWNLIST | WS_VSCROLL, 8, y, 140, 260, 200, ID_RAWFMT);
  cb_add(g->rawfmt, "16-bit integer");                 /* in SMP_RAW_* order */
  cb_add(g->rawfmt, "24-bit integer");
  cb_add(g->rawfmt, "32-bit integer");
  cb_add(g->rawfmt, "32-bit float");
  cb_add(g->rawfmt, "8-bit unsigned");
  y += ROW;
  g->rawend = labelled(g, 1, "Raw byte order", L"COMBOBOX", T | CBS_DROPDOWNLIST | WS_VSCROLL, 8, y, 140, 260, 200, ID_RAWEND);
  cb_add(g->rawend, "Little endian");
  cb_add(g->rawend, "Big endian");
  y += ROW;
  g->ffmpeg = labelled(g, 1, "FFmpeg", L"EDIT", T | ES_AUTOHSCROLL | ES_READONLY, 8, y, 140, 470, 24, ID_FFMPEG);
  g->ffbrowse = ctl(g, 1, L"BUTTON", L"Browse for FFmpeg", T | BS_PUSHBUTTON, 630, y, 120, 26, ID_FFBROWSE);
  y += ROW;
  g->used = labelled(g, 1, "Cache in use", L"EDIT", T | ES_AUTOHSCROLL | ES_READONLY, 8, y, 140, 470, 24, ID_USED);
  y += ROW;
  g->clear = ctl(g, 1, L"BUTTON", L"Clear cache", T | BS_PUSHBUTTON, 152, y, 120, 26, ID_CLEAR);
  y += ROW;
  g->back = ctl(g, 1, L"BUTTON", L"Back", T | BS_PUSHBUTTON, 152, y, 120, 26, ID_BACK);

  for (id = 0; id < SMP_NALLPARAMS; id++) show_param(g, id, true);
  show_page(g, smp_plug_plays(g->P) ? PG_FONT : PG_SAMPLER, NULL);   /* opens on the tab that plays */
  SetTimer(g->win, T_REFRESH, 250, NULL);
  SetTimer(g->win, T_OPEN, 150, NULL);          /* just opened: start on the first control */
  return true;
}

void smp_gui_loaded(Plug *P) {
  Gui *g = P ? P->gui : NULL;
  if (g && g->win && !g->closing && !g->modal) refresh(g);   /* during a dialog, the timer does it afterwards */
}

void smp_gui_close(Plug *P) {
  Gui *g = P ? P->gui : NULL;
  if (!g) return;
  g->closing = true;
  P->gui = NULL; g->P = NULL;                 /* the plugin may be freed right after this */
  if (g->win) {
    KillTimer(g->win, T_REFRESH); KillTimer(g->win, T_OPEN);
    if (g->aps) {
      MSAAPROPID props[2] = {PROPID_ACC_NAME, PROPID_ACC_VALUE}; int i;
      for (i = 0; i < g->nsl; i++) {
        g->aps->lpVtbl->ClearHwndProps(g->aps, g->sl[i].tb, (DWORD)OBJID_CLIENT, CHILDID_SELF, props, 2);
        g->aps->lpVtbl->ClearHwndProps(g->aps, g->sl[i].ed, (DWORD)OBJID_CLIENT, CHILDID_SELF, props, 1);
      }
      g->aps->lpVtbl->ClearHwndProps(g->aps, g->tabs, (DWORD)OBJID_CLIENT, CHILDID_SELF, props, 1);
    }
    if (g->drop_registered) RevokeDragDrop(g->win);
    DestroyWindow(g->win); g->win = NULL;
  }
  if (!g->modal) gui_free(g);                 /* else the dialog's caller frees it when the dialog returns */
}

static void gui_free(Gui *g) {
  if (g->aps) g->aps->lpVtbl->Release(g->aps);
  if (g->ole_init) OleUninitialize();
  if (g->font) DeleteObject(g->font);
  memset(g, 0xDD, sizeof *g);                 /* a use after free shows up as garbage, not as old values */
  free(g);
}

/* ---- the CLAP gui extension: embedded in the host's window, Win32 only ---- */
static Plug *plug(const clap_plugin_t *p) { return (Plug *)p->plugin_data; }
static bool g_supported(const clap_plugin_t *p, const char *api, bool floating) { (void)p; return !strcmp(api, CLAP_WINDOW_API_WIN32) && !floating; }
static bool g_preferred(const clap_plugin_t *p, const char **api, bool *floating) { (void)p; *api = CLAP_WINDOW_API_WIN32; *floating = false; return true; }
static bool g_create(const clap_plugin_t *p, const char *api, bool floating) {
  Plug *P = plug(p);
  if (!g_supported(p, api, floating)) return false;
  if (P->gui) return true;
  P->gui = calloc(1, sizeof(Gui));
  if (!P->gui) return false;
  P->gui->P = P;
  P->gui->fails_seen = P->load_failed;        /* failures from before the window opened aren't told again */
  P->gui->notes_seen = P->load_notes;
  return true;
}
static void g_destroy(const clap_plugin_t *p) { smp_gui_close(plug(p)); }
static bool g_scale(const clap_plugin_t *p, double s) { (void)p; (void)s; return false; }
static bool g_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h) { (void)p; *w = W_WIDTH; *h = W_HEIGHT; return true; }
static bool g_can_resize(const clap_plugin_t *p) { (void)p; return false; }
static bool g_hints(const clap_plugin_t *p, clap_gui_resize_hints_t *h) { (void)p; (void)h; return false; }
static bool g_adjust(const clap_plugin_t *p, uint32_t *w, uint32_t *h) { return g_size(p, w, h); }
static bool g_set_size(const clap_plugin_t *p, uint32_t w, uint32_t h) { (void)p; (void)w; (void)h; return true; }
static bool g_parent(const clap_plugin_t *p, const clap_window_t *win) {
  Plug *P = plug(p); Gui *g = P->gui;
  if (!g || g->win) return false;
  if (!build(g, (HWND)win->win32)) { smp_gui_close(P); return false; }
  return true;
}
static bool g_transient(const clap_plugin_t *p, const clap_window_t *w) { (void)p; (void)w; return false; }
static void g_title(const clap_plugin_t *p, const char *t) { (void)p; (void)t; }
static bool g_show(const clap_plugin_t *p) { Gui *g = plug(p)->gui; if (g && g->win) ShowWindow(g->win, SW_SHOW); return true; }
static bool g_hide(const clap_plugin_t *p) { Gui *g = plug(p)->gui; if (g && g->win) ShowWindow(g->win, SW_HIDE); return true; }

const clap_plugin_gui_t smp_gui_ext = {g_supported, g_preferred, g_create, g_destroy, g_scale, g_size, g_can_resize,
                                       g_hints, g_adjust, g_set_size, g_parent, g_transient, g_title, g_show, g_hide};
