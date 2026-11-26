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
   of the other kind gives that player's error). A dropped file goes by its extension: an instrument (.sf2 .sf3 .sf4
   .sf2pack .sbk .dls .gig .sfz .dspreset .dslibrary .exs) loads into the SoundFont player, an audio file into the
   sampler, and the tab follows.
   The Sampler's samples. Browse and Add samples choose several files at once (Browse replaces the samples, Add samples
   adds them, a drop of several replaces them); the File combo box replaces the selected sample. The Samples list
   selects one (Delete removes it), and Start key, End key and Velocity ("15,80") are that sample's.
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
#include "sfknob.h"
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
       ID_ZLIST, ID_ZADD, ID_ZREMOVE, ID_ZLO, ID_ZHI, ID_ZVEL,
       ID_EDIT = 300,                        /* + slider number: the edit box that stands in for it */
       ID_CHAN = 400,                         /* + channel - 1: its preset combo box */
       ID_OUT = 420,                          /* + channel - 1: its output combo box */
       ID_KNOB = 440 };                       /* + knob: its slider */
enum { PG_SAMPLER = 0, PG_SETTINGS = 1, PG_FONT = 2, NPAGES = 3 };
#define PAGE_CTLS 128
#define PAGE_STOPS 64
#define SL_LIMIT (-1)                         /* the slider that isn't a parameter: the cache size limit */
#define SL_KNOB0 (-2)                         /* SL_KNOB0 - i: the sliders of a Decent Sampler preset's knobs */
#define SL_ZLO (-100)                         /* the selected sample's Start key and End key */
#define SL_ZHI (-101)
#define IS_KNOB(w) ((w) <= SL_KNOB0 && (w) > SL_KNOB0 - NKNOB)
#define IS_ZONE(w) ((w) == SL_ZLO || (w) == SL_ZHI)
#define NKNOB 16                              /* knob sliders on the SoundFont page */
enum { T_REFRESH = 1, T_OPEN = 2 };
#define WM_DROPPED (WM_APP + 1)

typedef struct { IDropTarget t; struct Gui *g; int ok; } Drop;

/* a value's slider and the edit box that stands in for it while typing */
typedef struct { HWND tb, ed; int what; char said[96]; bool editing; const struct Gui *gui; } Slider;

typedef struct Gui {
  Plug *P;
  HWND win; HFONT font;
  int page;                                   /* PG_SAMPLER, PG_SETTINGS or PG_FONT */
  HWND tabs;                                  /* the Player tab control: first on both main pages */
  bool setting_tab;                           /* the window moves the tab itself: not the user's choice */
  HWND sample, browse, status, settings;
  HWND val[SMP_NALLPARAMS];                   /* each parameter's slider or combo box */
  Slider sl[SMP_NALLPARAMS + 1 + NKNOB + 2]; int nsl;
  Slider *sl_of[SMP_NALLPARAMS];              /* a parameter's slider, or NULL (a choice) */
  IAccPropServices *aps;                      /* the words a screen reader reads for a slider; NULL: numbers */
  HWND stretcher, keeptone, dir, dirbrowse, used, clear, back;
  Slider *limit;
  char dir_shown[1024], used_text[700];
  HWND rawrate, rawch, rawfmt, rawend, ffmpeg, ffbrowse;  /* how raw files read; where FFmpeg is */
  char ff_text[1100];                         /* FFmpeg's path as found, or "Not found" */
  char ff_for[1024]; int ff_searched;         /* the Settings path ff_text was found for (searched once per path) */
  /* the SoundFont page */
  HWND sffile, sfbrowse, sfmode, sfpreset, sfchannel, chan[NCH], out[NCH], sfstatus, sfsettings;
  HWND mode_one[4], mode_all[4 * NCH];        /* each mode's own controls with their labels: shown in that mode only */
  int nmode_one, nmode_all;
  bool sf_one;                                /* the mode the page shows: One preset */
  /* a Decent Sampler preset's knobs: NKNOB sliders made once, the first nknobs shown (with the preset's labels),
     after SoundFont transpose; Reverb and chorus, Status and Settings move down below them */
  Slider *knob[NKNOB]; HWND knob_lab[NKNOB]; int nknobs; char knob_key[2200];
  HWND fx_lab, sfstatus_lab; int transpose_y;
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
  wchar_t *dropped;                           /* the files a drop noted (each ending in a zero, the list in two),
                                                 loaded on WM_DROPPED; NULL when none */
  /* the Sampler's samples: the list, Add samples, Remove sample, the selected one's Start key, End key and
     Velocity ("15,80") */
  HWND zlist, zadd, zremove, zvel;
  Slider *zlo, *zhi;
  uint64_t zlist_key;                         /* what the list shows now (its lines and count), to rebuild on change */
  char file_base[1100];                       /* the selected sample's file when the Sampler's File box got the focus:
                                                 Enter puts it back and adds the file shown */
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
static void zones_show(Gui *g);
static int zone_targets(Gui *g, int *out);
static void refresh(Gui *g);

/* ---- sliders ---- */
/* a knob slider's knob (as the player has it now); 0 when there is none */
static int knob_of(const Slider *s, SmpKnobInfo *k) {
  const Gui *g = s->gui;
  memset(k, 0, sizeof *k);
  return g && g->P && smp_plug_knob_info(g->P, SL_KNOB0 - s->what, k);
}
static bool knob_whole(const SmpKnobInfo *k) { return k->vtype != SF_KV_FLOAT; }

static void sl_range(const Slider *s, double *lo, double *hi, double *step) {
  const SmpParamInfo *pi;
  if (s->what == SL_LIMIT) { *lo = LIMIT_LO; *hi = LIMIT_HI; *step = 1; return; }
  if (IS_ZONE(s->what)) { *lo = 0; *hi = 127; *step = 1; return; }
  if (IS_KNOB(s->what)) {                       /* a knob: whole numbers step by 1, the rest by a hundredth */
    SmpKnobInfo k;
    if (!knob_of(s, &k) || !(k.hi > k.lo)) { *lo = 0; *hi = 1; *step = 0.01; return; }
    *lo = k.lo; *hi = k.hi; *step = knob_whole(&k) ? 1 : (k.hi - k.lo) / 100;
    return;
  }
  pi = smp_param_info(s->what);
  *lo = pi->lo; *hi = pi->hi; *step = pi->kind == SMP_K_MS ? 10 : 1;
}
static double sl_get(Gui *g, const Slider *s) {
  SmpSettings st;
  if (IS_KNOB(s->what)) return smp_plug_knob_get(g->P, SL_KNOB0 - s->what);
  if (IS_ZONE(s->what)) {
    int a, b, c, d;
    smp_plug_zone_range(g->P, smp_plug_zone_selected(g->P), &a, &b, &c, &d);
    return s->what == SL_ZLO ? a : b;
  }
  if (s->what != SL_LIMIT) return smp_plug_get_param(g->P, s->what);
  smp_settings_get(&st);
  return st.cache_limit_gb;
}
static void sl_text(const Slider *s, double v, char *out, int cap) {
  if (IS_KNOB(s->what)) { SmpKnobInfo k; if (knob_of(s, &k)) smp_knob_text(&k, v, out, cap); else if (cap > 0) out[0] = 0; }
  else if (IS_ZONE(s->what)) {                 /* a note in every octave reads "every C" on both */
    int pc = smp_plug_zone_key_class(s->gui->P, smp_plug_zone_selected(s->gui->P));
    if (pc) { char n[80]; smp_note_class_name(pc, n, sizeof n); snprintf(out, (size_t)cap, "every %s", n); }
    else smp_note_name((int)floor(v + 0.5), out, cap);
  }
  else if (s->what == SL_LIMIT) gb_text(v, out, cap); else param_text(s->what, v, out, cap);
}
static int sl_parse(const Slider *s, const char *t, double *v) {
  char *e;
  if (IS_KNOB(s->what)) { SmpKnobInfo k; return knob_of(s, &k) && smp_knob_parse(&k, t, v); }
  if (IS_ZONE(s->what)) return smp_value_parse(SMP_P_NOTE_LO, t, v);
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
  if (IS_KNOB(s->what)) smp_plug_knob_set(g->P, SL_KNOB0 - s->what, v);
  else if (IS_ZONE(s->what)) {                   /* every picked sample; a key past the other end takes it along */
    int t[SMP_MULTI_MAX], nt = zone_targets(g, t), j, a, b, c, d, k = (int)floor(v + 0.5);
    if (!nt) { beep(); return; }                 /* no sample to set it for */
    for (j = 0; j < nt; j++) {
      smp_plug_zone_range(g->P, t[j], &a, &b, &c, &d);
      if (s->what == SL_ZLO) { a = k; if (b < a) b = a; } else { b = k; if (a > b) a = b; }
      smp_plug_zone_set_range(g->P, t[j], a, b, c, d);
    }
    sl_show(g, g->zlo, true); sl_show(g, g->zhi, true);   /* "every C" on the other one ends too */
    zones_show(g);
  }
  else if (s->what == SL_LIMIT) set_limit(g, v); else smp_plug_set_param(g->P, s->what, v);
  sl_show(g, s, true);
}
/* Up and Down: a semitone, a cent, a dB, 10 ms, 1 %, 1 GB (by: 1, 10 or 0.1, and the sign); whole-number values
   never step less than 1 */
static void sl_nudge(Gui *g, Slider *s, double by) {
  double lo, hi, step, v;
  int kind = s->what <= SL_LIMIT ? -1 : smp_param_info(s->what)->kind;
  bool whole = kind == SMP_K_NOTE || kind == SMP_K_SEMI || kind == SMP_K_CENT || kind == SMP_K_COUNT || kind == SMP_K_PAN;
  if (IS_KNOB(s->what)) { SmpKnobInfo k; whole = knob_of(s, &k) && knob_whole(&k); }
  if (IS_ZONE(s->what)) whole = true;
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
  v = s->what == SL_LIMIT ? floor(v * 10 + 0.5) / 10 : lo + floor((v - lo) / step + 0.5) * step;
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
      int pc = IS_ZONE(s->what) ? smp_note_class_parse(t) : 0;
      if (pc) {                                  /* a key box: notes without octaves ("D F") are those in every octave */
        int t[SMP_MULTI_MAX], nt = zone_targets(g, t), j;
        if (!nt) beep();
        for (j = 0; j < nt; j++) smp_plug_zone_set_key_class(g->P, t[j], pc);
        sl_show(g, g->zlo, true); sl_show(g, g->zhi, true);
        zones_show(g);
      }
      else if (sl_parse(s, t, &v)) sl_set(g, s, v); else beep();
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
/* FFmpeg's place for the Settings page. PATH and the WinGet folders are searched the first time the page opens,
   after Browse for FFmpeg (force), and when the saved path has changed (another instance's Settings); a page switch
   otherwise shows what was found before, so it never walks the disk. */
static void find_ffmpeg(Gui *g, int force) {
  SmpSettings s;
  smp_settings_get(&s);
  if (!force && g->ff_searched && !strcmp(g->ff_for, s.ffmpeg_path)) return;
  if (!smp_ffmpeg_find(g->ff_text, sizeof g->ff_text)) snprintf(g->ff_text, sizeof g->ff_text, "Not found");
  snprintf(g->ff_for, sizeof g->ff_for, "%s", s.ffmpeg_path);
  g->ff_searched = 1;
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
static int test_dialog; static char test_path[4400]; static const clap_plugin_t *test_plugin;
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
/* The files a multiple-choice dialog returned (the folder, then each name, each ending in a zero; or one full path),
   or the stub's ("a|b"), as full paths: how many, in *out (each and the list freed by the caller) */
static int split_files(const wchar_t *buf, bool stub, wchar_t ***out) {
  wchar_t **list = NULL; int n = 0, cap = 0;
  const wchar_t *dir = buf, *q;
  *out = NULL;
  if (stub) {                                  /* "a|b|c" */
    const wchar_t *a = buf;
    while (*a) {
      const wchar_t *e = wcschr(a, L'|'); size_t len = e ? (size_t)(e - a) : wcslen(a);
      if (n == cap) { wchar_t **nl = realloc(list, sizeof *list * (size_t)(cap = cap ? cap * 2 : 8)); if (!nl) break; list = nl; }
      if ((list[n] = malloc((len + 1) * sizeof(wchar_t))) != NULL) { memcpy(list[n], a, len * sizeof(wchar_t)); list[n][len] = 0; n++; }
      a = e ? e + 1 : a + len;
    }
    *out = list;
    return n;
  }
  q = dir + wcslen(dir) + 1;
  if (!*q) {                                   /* one file: its full path */
    if ((list = malloc(sizeof *list)) && (list[0] = _wcsdup(dir))) { *out = list; return 1; }
    free(list); return 0;
  }
  for (; *q; q += wcslen(q) + 1) {
    size_t len = wcslen(dir) + wcslen(q) + 2;
    if (n == cap) { wchar_t **nl = realloc(list, sizeof *list * (size_t)(cap = cap ? cap * 2 : 8)); if (!nl) break; list = nl; }
    if ((list[n] = malloc(len * sizeof(wchar_t))) != NULL) {
      swprintf(list[n], len, L"%s%s%s", dir, dir[wcslen(dir) - 1] == L'\\' ? L"" : L"\\", q);
      n++;
    }
  }
  *out = list;
  return n;
}
static void free_files(wchar_t **list, int n) { int i; for (i = 0; i < n; i++) free(list[i]); free(list); }

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
      find_ffmpeg(g, 1);
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

/* The Sampler's samples: the list (one line each, the selected one selected; "No samples loaded" when there are
   none), rebuilt only when its lines change and never while it is being moved through; the Velocity box, unless it
   is being typed in */
static uint64_t fnv(uint64_t h, const char *t) { for (; *t; t++) { h ^= (unsigned char)*t; h *= 1099511628211ull; } return h; }
static void zones_show(Gui *g) {
  int n = smp_plug_zone_count(g->P), sel = smp_plug_zone_selected(g->P), i;
  char line[1500], v[32]; uint64_t key = 1469598103934665603ull;
  if (!g->zlist) return;
  for (i = 0; i < n; i++) { smp_plug_zone_line(g->P, i, line, sizeof line); key = fnv(fnv(key, line), "\n"); }
  key ^= (uint64_t)n;
  if (key != g->zlist_key) {
    g->zlist_key = key;
    SendMessageW(g->zlist, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g->zlist, LB_RESETCONTENT, 0, 0);
    if (!n) SendMessageW(g->zlist, LB_ADDSTRING, 0, (LPARAM)L"No samples loaded");
    for (i = 0; i < n; i++) {
      wchar_t w[1500];
      smp_plug_zone_line(g->P, i, line, sizeof line);
      MultiByteToWideChar(CP_UTF8, 0, line, -1, w, 1500);
      SendMessageW(g->zlist, LB_ADDSTRING, 0, (LPARAM)w);
    }
    SendMessageW(g->zlist, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g->zlist, NULL, TRUE);
  }
  for (i = 0; i < (n ? n : 1); i++) {      /* the picked ones, and the caret on the selected one */
    int want = n ? smp_plug_zone_picked(g->P, i) : 1;
    if ((SendMessageW(g->zlist, LB_GETSEL, (WPARAM)i, 0) > 0) != want) SendMessageW(g->zlist, LB_SETSEL, (WPARAM)want, (LPARAM)i);
  }
  if (SendMessageW(g->zlist, LB_GETCARETINDEX, 0, 0) != (n ? sel : 0)) SendMessageW(g->zlist, LB_SETCARETINDEX, (WPARAM)(n ? sel : 0), FALSE);
  if (!g->zlo->editing) sl_show(g, g->zlo, false);
  if (!g->zhi->editing) sl_show(g, g->zhi, false);
  if (GetFocus() != g->zvel) {
    int a, b, c, d;
    smp_plug_zone_range(g->P, sel, &a, &b, &c, &d);
    snprintf(v, sizeof v, "%d,%d", c, d);
    set_text(g->zvel, v);
  }
}
/* "15,80", "15 80", "15-80" or "15 to 80" (either way round), or one number for both: 1 to 127 */
static int vel_parse(const char *t, int *lo, int *hi) {
  long a, b; char *e;
  while (*t == ' ') t++;
  a = strtol(t, &e, 10);
  if (e == t) return 0;
  t = e;
  while (*t == ' ') t++;
  if (!*t) b = a;
  else {
    if (*t == ',' || *t == '-') t++;
    else if (!_strnicmp(t, "to", 2)) t += 2;
    else if (t[-1] != ' ') return 0;
    while (*t == ' ') t++;
    b = strtol(t, &e, 10);
    if (e == t) return 0;
    t = e;
    while (*t == ' ') t++;
    if (*t) return 0;
  }
  if (a > b) { long x = a; a = b; b = x; }
  if (a < 1 || b > 127) return 0;
  *lo = (int)a; *hi = (int)b;
  return 1;
}
/* the Velocity box was left or Enter pressed: what it says becomes the range; one that doesn't read beeps and the box
   says the range again */
static void vel_apply(Gui *g) {
  char t[64], v[32]; int i = smp_plug_zone_selected(g->P), a, b, c, d, lo, hi;
  get_text(g->zvel, t, sizeof t);
  smp_plug_zone_range(g->P, i, &a, &b, &c, &d);
  snprintf(v, sizeof v, "%d,%d", c, d);
  if (!strcmp(t, v)) return;
  if (i < 0 || !vel_parse(t, &lo, &hi)) { beep(); set_text(g->zvel, v); SendMessageW(g->zvel, EM_SETSEL, 0, -1); return; }
  {                                            /* every picked sample, its keys as they are (every C stays) */
    int t[SMP_MULTI_MAX], nt = zone_targets(g, t), j;
    for (j = 0; j < nt; j++) {
      int pc = smp_plug_zone_key_class(g->P, t[j]);
      smp_plug_zone_range(g->P, t[j], &a, &b, &c, &d);
      smp_plug_zone_set_range(g->P, t[j], a, b, lo, hi);
      if (pc) smp_plug_zone_set_key_class(g->P, t[j], pc);
    }
  }
  snprintf(v, sizeof v, "%d,%d", lo, hi);
  set_text(g->zvel, v);
  zones_show(g);
}
static void zone_remove(Gui *g) {
  if (smp_plug_zone_selected(g->P) < 0) { beep(); return; }
  smp_plug_zone_remove_picked(g->P);
  refresh(g);                                  /* the list, the Sample box, Root note */
}
/* what the list shows picked becomes the samples picked; the item with the caret becomes the selected one */
static void zones_taken(Gui *g) {
  int n = smp_plug_zone_count(g->P), i; LRESULT k = SendMessageW(g->zlist, LB_GETCARETINDEX, 0, 0);
  for (i = 0; i < n; i++) smp_plug_zone_pick(g->P, i, SendMessageW(g->zlist, LB_GETSEL, (WPARAM)i, 0) > 0);
  if (k >= 0 && k < n) smp_plug_zone_select(g->P, (int)k);
  refresh(g);
}
/* the list's own keys beyond a list box's: Ctrl+A picks every sample, Ctrl+Up and Ctrl+Down move without picking,
   Ctrl+Space picks or unpicks the one moved to. 1 when the key was one of these. */
static bool zones_key(Gui *g, WPARAM w) {
  int n = smp_plug_zone_count(g->P), k = (int)SendMessageW(g->zlist, LB_GETCARETINDEX, 0, 0);
  if (GetKeyState(VK_CONTROL) >= 0 || !n) return false;
  if (w == 'A') SendMessageW(g->zlist, LB_SETSEL, TRUE, -1);
  else if (w == VK_UP || w == VK_DOWN) {
    k += w == VK_UP ? -1 : 1;
    if (k < 0) k = 0;
    if (k >= n) k = n - 1;
    SendMessageW(g->zlist, LB_SETCARETINDEX, (WPARAM)k, FALSE);
  } else if (w == VK_SPACE) SendMessageW(g->zlist, LB_SETSEL, SendMessageW(g->zlist, LB_GETSEL, (WPARAM)k, 0) <= 0, k);
  else return false;
  zones_taken(g);
  return true;
}
/* the samples a change in the window goes to: the picked ones, or the selected one; how many, in out */
static int zone_targets(Gui *g, int *out) {
  int n = smp_plug_zone_count(g->P), i, k = 0;
  for (i = 0; i < n; i++) if (smp_plug_zone_picked(g->P, i)) out[k++] = i;
  if (!k && smp_plug_zone_selected(g->P) >= 0) out[k++] = smp_plug_zone_selected(g->P);
  return k;
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
  int i;
  for (i = g->nknobs; i < NKNOB; i++) if (g->knob[i] && (h == g->knob[i]->tb || h == g->knob_lab[i])) return true;
  return g->sf_one ? in_list(g->mode_all, g->nmode_all, h) : in_list(g->mode_one, g->nmode_one, h);
}
/* the SoundFont page shows its mode's controls; the focus never stays on one that hides */
static void sf_layout(Gui *g) {
  HWND f = GetFocus(); int i;
  if (g->page != PG_FONT) return;
  if (f && mode_hides(g, f)) focus(g->sfmode);
  for (i = 0; i < g->nmode_one; i++) ShowWindow(g->mode_one[i], g->sf_one ? SW_SHOW : SW_HIDE);
  for (i = 0; i < g->nmode_all; i++) ShowWindow(g->mode_all[i], g->sf_one ? SW_HIDE : SW_SHOW);
  for (i = 0; i < NKNOB; i++) if (g->knob[i]) {
    int sh = i < g->nknobs ? SW_SHOW : SW_HIDE;
    ShowWindow(g->knob_lab[i], sh);
    if (!g->knob[i]->editing) ShowWindow(g->knob[i]->tb, sh);
  }
}

/* a label at x, y and its control (and a slider's edit box) after it, their sizes kept */
static void move_labelled(HWND lab, HWND c, HWND ed, int x, int y) {
  RECT r; int lw;
  GetWindowRect(lab, &r); lw = r.right - r.left;
  SetWindowPos(lab, NULL, x, y + 4, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  SetWindowPos(c, NULL, x + lw + 4, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  if (ed) SetWindowPos(ed, NULL, x + lw + 4, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The knobs take the places after SoundFont transpose, two to a row: the first beside it, then the next rows, left
   and right; Reverb and chorus takes the place after the last (beside SoundFont transpose when there is none, as
   before), and Status and Settings the rows below. */
static void knob_place(int slot, int ty, int *x, int *y) {
  *x = slot % 2 == 0 ? 384 : 8;
  *y = ty + ((slot + 1) / 2) * ROW;
}
static void knob_layout(Gui *g) {
  int i, x, y;
  for (i = 0; i < g->nknobs; i++) {
    knob_place(i, g->transpose_y, &x, &y);
    move_labelled(g->knob_lab[i], g->knob[i]->tb, g->knob[i]->ed, x, y);
  }
  knob_place(g->nknobs, g->transpose_y, &x, &y);
  move_labelled(g->fx_lab, g->val[SMP_P_SF_EFFECTS], NULL, x, y);
  y += ROW;
  move_labelled(g->sfstatus_lab, g->sfstatus, NULL, 8, y);
  SetWindowPos(g->sfsettings, NULL, 132, y + ROW, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  InvalidateRect(g->win, NULL, TRUE);
}

/* the knobs the player has now: labels, ranges and places when they change (a new font, mode or preset), then each
   one's value (the timer's: a CC moves a knob) */
static void sf_knobs(Gui *g) {
  char key[2200]; int n = smp_plug_knob_count(g->P), i, at;
  SmpKnobInfo k[NKNOB];
  if (n > NKNOB) n = NKNOB;
  at = snprintf(key, sizeof key, "%p|%d", (void *)sf_font(g), n);
  for (i = 0; i < n; i++) {
    if (!smp_plug_knob_info(g->P, i, &k[i])) memset(&k[i], 0, sizeof k[i]);
    if (at < (int)sizeof key) at += snprintf(key + at, sizeof key - (size_t)at, "|%s %g %g %d", k[i].label, k[i].lo, k[i].hi, k[i].vtype);
  }
  if (strcmp(key, g->knob_key)) {
    HWND f = GetFocus(); bool lost = false;
    snprintf(g->knob_key, sizeof g->knob_key, "%s", key);
    for (i = n; i < NKNOB; i++) {
      if (g->knob[i]->editing) edit_end(g, g->knob[i], false, false);
      if (f && (f == g->knob[i]->tb || f == g->knob[i]->ed)) lost = true;
    }
    g->nknobs = n;
    for (i = 0; i < n; i++) {
      Slider *s = g->knob[i];
      double steps = knob_whole(&k[i]) ? k[i].hi - k[i].lo : 100;
      wchar_t w[96];
      MultiByteToWideChar(CP_UTF8, 0, k[i].label, -1, w, 96);
      SetWindowTextW(g->knob_lab[i], w);
      annotate(g, s->tb, PROPID_ACC_NAME, k[i].label);
      annotate(g, s->ed, PROPID_ACC_NAME, k[i].label);
      SendMessageW(s->tb, TBM_SETRANGEMAX, TRUE, (LPARAM)(steps > 1000 ? 1000 : steps < 1 ? 1 : (LONG)floor(steps + 0.5)));
      s->said[0] = 0;
    }
    knob_layout(g);
    if (g->page == PG_FONT) sf_layout(g);
    if (lost) focus(g->val[SMP_P_SF_TRANSPOSE]);   /* its knob went: the focus to the slider before the knobs */
  }
  for (i = 0; i < n; i++) if (!g->knob[i]->editing) sl_show(g, g->knob[i], false);
}

static void sf_show(Gui *g) {
  char t[1500], name[200], lacks[200]; SfCfg c; SmpFont *f; int i, b, p, n, k;
  smp_plug_font_text(g->P, t, sizeof t); set_text(g->sffile, t);
  sf_lists(g);
  sf_cfg(g, &c);
  f = sf_font(g);
  n = f ? smp_sf_preset_count(f) : 0;
  if ((bool)c.one != g->sf_one) { g->sf_one = c.one != 0; sf_layout(g); }
  sf_knobs(g);
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
  if (g->P->e) smp_multi_collect(g->P->e, !g->P->active);   /* replaced samples (all, when nothing processes) */
  if (g->page != PG_SETTINGS && g->page != (smp_plug_plays(g->P) ? PG_FONT : PG_SAMPLER)) {
    show_page(g, smp_plug_plays(g->P) ? PG_FONT : PG_SAMPLER, NULL);   /* the tab follows what plays (it refreshes) */
    return;
  }
  show_sample(g);
  zones_show(g);
  if (g->page != PG_SETTINGS) file_list(g, g->page == PG_FONT, false);
  for (id = 0; id < SMP_NALLPARAMS; id++) show_param(g, id, false);
  show_status(g);
  if (g->page == PG_FONT) sf_show(g);
  if (g->page == PG_SETTINGS) show_settings(g);
}

/* the SoundFont page's kinds: every instrument format the SoundFont player opens, for the File combo box and drops
   (Browse's filter below lists the same) */
static const wchar_t *const font_exts[] = {L".sf2", L".sf3", L".sf4", L".sf2pack", L".sbk", L".dls", L".gig", L".sfz",
                                           L".dspreset", L".dslibrary", L".exs"};
static bool font_name(const wchar_t *n) {
  const wchar_t *dot = wcsrchr(n, L'.'); size_t i;
  if (!dot) return false;
  for (i = 0; i < sizeof font_exts / sizeof font_exts[0]; i++) if (!_wcsicmp(dot, font_exts[i])) return true;
  return false;
}
/* a file to load: LOAD_SAMPLE or LOAD_FONT (as the page's Browse or File combo box asks: a file of the
   other kind gives that player's error), or LOAD_DROPPED: by its extension, an instrument as the SoundFont
   (which then plays), anything else as the sample (which then plays) */
enum { LOAD_SAMPLE, LOAD_FONT, LOAD_DROPPED };
/* A sample loads in the background (the status says "Loading x.wav" or "Decoding x.dsf with FFmpeg" meanwhile, and
   a failure is told when it comes, in refresh); so does a SoundFont ("Loading x.sfz": an SFZ reads every sample
   file's header), which then plays. */
static void load_path_w(Gui *g, const wchar_t *w, int how) {
  char u[1100], err[700], msg[1900]; bool sf = how == LOAD_DROPPED ? font_name(w) : how == LOAD_FONT;
  int ok, sel = smp_plug_zone_selected(g->P);
  const char *one = u;
  if (WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL) <= 0) { beep(); return; }
  /* the Sampler's File combo box: the file replaces the selected sample (its ranges stay) */
  ok = sf ? smp_plug_load_font_start(g->P, u, err, sizeof err)
          : smp_plug_load_many(g->P, &one, 1, sel >= 0 ? SMP_LOAD_ONE : SMP_LOAD_ALL, sel, how == LOAD_DROPPED, err, sizeof err);
  if (!ok) {                                  /* refused at once: "Can't load x: why. Still playing y." */
    smp_plug_cant_load(g->P, u, err, msg, sizeof msg);   /* one full stop between, as the background failure */
    say_note(g, msg); beep();
  } else g->note[0] = 0;
  refresh(g);
}

/* several sample files at once (Browse, Add samples, a drop): how is SMP_LOAD_ALL or SMP_LOAD_ADD */
static void load_paths_w(Gui *g, wchar_t **w, int n, int how, bool dropped) {
  char **u = calloc((size_t)(n > 0 ? n : 1), sizeof *u), err[700], msg[1900]; int i, ok = 0, k = 0;
  if (!u) { beep(); return; }
  for (i = 0; i < n; i++) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
    if (len <= 0 || !(u[k] = malloc((size_t)len))) continue;
    WideCharToMultiByte(CP_UTF8, 0, w[i], -1, u[k], len, NULL, NULL);
    k++;
  }
  if (k) ok = smp_plug_load_many(g->P, (const char *const *)u, k, how, -1, dropped, err, sizeof err);
  else snprintf(err, sizeof err, "no file chosen");
  if (!ok) {
    smp_plug_cant_load(g->P, k ? u[0] : "", err, msg, sizeof msg);
    say_note(g, msg); beep();
  } else g->note[0] = 0;
  for (i = 0; i < k; i++) free(u[i]);
  free(u);
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
  {smp_plug_font_path, font_name, L"SoundFonts and instruments\0*.sf2;*.sf3;*.sf4;*.sf2pack;*.sbk;*.dls;*.gig;*.sfz;*.dspreset;*.dslibrary;*.exs\0All files\0*.*\0",
   L"Load a SoundFont"},
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

/* Browse (how SMP_LOAD_ALL) and Add samples (SMP_LOAD_ADD) on the Sampler page: several files may be chosen;
   the SoundFont page's Browse chooses one */
static void browse_file_as(Gui *g, int kind, int how);
static void browse_file(Gui *g, int kind) { browse_file_as(g, kind, SMP_LOAD_ALL); }
/* Enter on the Sampler's File combo box: the file shown joins the samples, and the selected sample goes back to the
   file it had (trying files there changed it) */
static void file_keep(Gui *g) {
  HWND c = g->file[0]; wchar_t t[600], full[1800]; char u[1900], err[700], msg[1900];
  int k = (int)SendMessageW(c, CB_GETCURSEL, 0, 0);
  if (SendMessageW(c, CB_GETDROPPEDSTATE, 0, 0)) SendMessageW(c, CB_SHOWDROPDOWN, FALSE, 0);
  if (k < 0 || !g->file_live[0] || SendMessageW(c, CB_GETLBTEXTLEN, (WPARAM)k, 0) >= 600) { beep(); return; }
  SendMessageW(c, CB_GETLBTEXT, (WPARAM)k, (LPARAM)t);
  swprintf(full, 1800, L"%s\\%s", g->file_dir[0], t);
  if (WideCharToMultiByte(CP_UTF8, 0, full, -1, u, sizeof u, NULL, NULL) <= 0) { beep(); return; }
  if (!smp_plug_zone_keep(g->P, g->file_base, u, err, sizeof err)) {
    smp_plug_cant_load(g->P, u, err, msg, sizeof msg);
    say_note(g, msg); beep();
  } else g->note[0] = 0;
  if (smp_plug_sample_path(g->P, g->file_base, sizeof g->file_base) != 1) g->file_base[0] = 0;
  refresh(g);
  file_list(g, 0, true);                       /* on the selected sample's file again */
}
static void browse_file_as(Gui *g, int kind, int how) {
  const FileKind *K = &kinds[kind];
  enum { BIG = 65536 };
  wchar_t *file = calloc(BIG, sizeof(wchar_t)), dirw[1100] = L""; char path[1100]; OPENFILENAMEW of; int ok, stub = test_dialog;
  if (!file) { beep(); return; }
  if (K->path(g->P, path, sizeof path) && MultiByteToWideChar(CP_UTF8, 0, path, -1, dirw, 1100)) {
    wchar_t *s = wcsrchr(dirw, L'\\'), *s2 = wcsrchr(dirw, L'/');
    if (s2 > s) s = s2;
    if (s) *s = 0; else dirw[0] = 0;
  }
  memset(&of, 0, sizeof of);
  of.lStructSize = sizeof of;
  of.hwndOwner = GetAncestor(g->win, GA_ROOT);
  of.lpstrFilter = K->filter ? K->filter : audio_filter();
  of.lpstrFile = file; of.nMaxFile = kind ? 1100 : BIG;
  of.lpstrInitialDir = dirw[0] ? dirw : NULL;
  of.lpstrTitle = kind ? K->title : how == SMP_LOAD_ADD ? L"Add samples" : L"Load samples";
  of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | (kind ? 0 : OFN_ALLOWMULTISELECT);
  modal_begin(g);
  ok = stub ? stub_dialog(g, file, BIG) : GetOpenFileNameW(&of);
  if (modal_end(g)) { free(file); return; }   /* the window (and maybe the plugin) went while the dialog was open */
  if (ok && kind) load_path_w(g, file, LOAD_FONT);   /* this page's player, whatever the file */
  else if (ok) {
    wchar_t **list; int n = split_files(file, stub != 0, &list);
    load_paths_w(g, list, n, how, false);
    free_files(list, n);
  }
  free(file);
  focus(kind ? g->sfbrowse : how == SMP_LOAD_ADD ? g->zadd : g->browse);
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
/* Dropped files: an instrument first loads that instrument (the SoundFont player plays); otherwise every file loads
   as the Sampler's samples, in place of those it has (and the Sampler plays) */
static void drop_files(Gui *g, wchar_t **list, int n) {
  if (n <= 0) return;
  if (font_name(list[0])) load_path_w(g, list[0], LOAD_DROPPED);
  else load_paths_w(g, list, n, SMP_LOAD_ALL, true);
}
/* the files of a drop, as full paths (freed with free_files) */
static int hdrop_files(HDROP h, wchar_t ***out) {
  UINT n = DragQueryFileW(h, 0xFFFFFFFF, NULL, 0), i; int k = 0;
  wchar_t **list = n ? calloc(n, sizeof *list) : NULL;
  *out = list;
  for (i = 0; list && i < n && i < SMP_MULTI_MAX; i++) {
    UINT len = DragQueryFileW(h, i, NULL, 0);
    if (!len || !(list[k] = malloc((len + 1) * sizeof(wchar_t)))) continue;
    if (DragQueryFileW(h, i, list[k], len + 1)) k++; else free(list[k]);
  }
  return k;
}
static void drop_hdrop(Gui *g, HDROP h) {
  wchar_t **list; int n = hdrop_files(h, &list);
  drop_files(g, list, n);
  free_files(list, n);
}
/* The drop only notes the file and posts WM_DROPPED: the load (maybe a decode) runs after the drag has returned, so
   Explorer or the Media Explorer isn't kept waiting, and nothing loads inside OLE's drag loop. */
static HRESULT STDMETHODCALLTYPE d_drop(IDropTarget *t, IDataObject *o, DWORD k, POINTL p, DWORD *eff) {
  Drop *d = (Drop *)t; STGMEDIUM m; (void)k; (void)p;
  *eff = DROPEFFECT_NONE;
  if (o && d->g && !d->g->closing && SUCCEEDED(o->lpVtbl->GetData(o, &hdrop_fmt, &m))) {
    HDROP h = (HDROP)GlobalLock(m.hGlobal);
    if (h) {
      wchar_t **list; int n = hdrop_files(h, &list), i; size_t len = 1;
      for (i = 0; i < n; i++) len += wcslen(list[i]) + 1;
      if (n && d->g->win && !d->g->dropped && (d->g->dropped = calloc(len, sizeof(wchar_t))) != NULL) {
        wchar_t *at = d->g->dropped;
        for (i = 0; i < n; i++) { wcscpy(at, list[i]); at += wcslen(list[i]) + 1; }
        PostMessageW(d->g->win, WM_DROPPED, 0, 0);
        *eff = DROPEFFECT_COPY;
      }
      free_files(list, n);
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
  if (page == PG_SETTINGS) { rescan_used(g); find_ffmpeg(g, 0); show_settings(g); }
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
    case ID_ZADD: browse_file_as(g, 0, SMP_LOAD_ADD); break;
    case ID_ZREMOVE: zone_remove(g); break;
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
  if (m == WM_SETFOCUS && id == ID_FILE && smp_plug_sample_path(g->P, g->file_base, sizeof g->file_base) != 1) g->file_base[0] = 0;
  if (m == WM_KEYDOWN && w == VK_RETURN && id == ID_FILE) { file_keep(g); return 0; }
  if (m == WM_CHAR && (w == '\r' || w == '\n') && id == ID_FILE) return 0;
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
  if (m == WM_KEYDOWN && w == VK_DELETE && id == ID_ZLIST) { zone_remove(g); return 0; }
  if (m == WM_KEYDOWN && id == ID_ZLIST && zones_key(g, w)) return 0;
  if (m == WM_CHAR && id == ID_ZLIST && (w == 1 || (w == ' ' && GetKeyState(VK_CONTROL) < 0))) return 0;   /* Ctrl+A, Ctrl+Space */
  if (m == WM_KEYDOWN && w == VK_ESCAPE && id == ID_ZVEL) {   /* as it was */
    char v[32]; int a, b, c, d;
    smp_plug_zone_range(g->P, smp_plug_zone_selected(g->P), &a, &b, &c, &d);
    snprintf(v, sizeof v, "%d,%d", c, d);
    set_text(h, v); SendMessageW(h, EM_SETSEL, 0, -1);
    return 0;
  }
  if (m == WM_KEYDOWN && w == VK_RETURN) {
    if (id == ID_DIR) { apply_dir(g); SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    if (id == ID_ZVEL) { vel_apply(g); SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    if (is_class(h, L"Button")) { SendMessageW(h, BM_CLICK, 0, 0); return 0; }
    if (is_class(h, L"Edit")) return 0;
  }
  if (m == WM_KILLFOCUS && id == ID_DIR) apply_dir(g);
  if (m == WM_KILLFOCUS && id == ID_ZVEL) vel_apply(g);
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
      } else if (code == LBN_SELCHANGE && id == ID_ZLIST) {   /* samples picked in the list */
        zones_taken(g);
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
    case WM_DROPPED:
      if (g->dropped) {
        wchar_t *buf = g->dropped, *at, *list[SMP_MULTI_MAX]; int n = 0;
        g->dropped = NULL;
        for (at = buf; *at && n < SMP_MULTI_MAX; at += wcslen(at) + 1) list[n++] = at;
        drop_files(g, list, n);
        free(buf);
      }
      return 0;
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
  s->what = what; s->gui = g;
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
#define W_HEIGHT_S (8 + (8 + REST_ROWS + 2) * ROW + 8)   /* the Sampler page: the tabs, 3 rows, the samples (4
                                                             rows), the rest in two columns, Status and Settings */
#define W_HEIGHT_F (8 + 23 * ROW + 26 + 8)   /* the SoundFont page with every knob: Settings 23 rows down */
#define W_HEIGHT (W_HEIGHT_S > W_HEIGHT_F ? W_HEIGHT_S : W_HEIGHT_F)

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
  /* the samples: the list, three rows high, with Add samples and Remove sample beside it, Velocity under them;
     Start key and End key below (Tab: the list, the buttons, Start key, End key, Velocity) */
  g->zlist = labelled(g, PG_SAMPLER, "Samples", L"LISTBOX", T | LBS_NOTIFY | LBS_EXTENDEDSEL | WS_VSCROLL | WS_BORDER | LBS_NOINTEGRALHEIGHT,
                      8, y, 120, 400, 3 * ROW - 6, ID_ZLIST);
  g->zadd = ctl(g, PG_SAMPLER, L"BUTTON", L"Add samples", T | BS_PUSHBUTTON, 548, y, 160, 26, ID_ZADD);
  g->zremove = ctl(g, PG_SAMPLER, L"BUTTON", L"Remove sample", T | BS_PUSHBUTTON, 548, y + ROW, 160, 26, ID_ZREMOVE);
  g->zlo = slider(g, PG_SAMPLER, "Start key", SL_ZLO, 8, y + 3 * ROW, 120, 236, ID_ZLO);
  g->zhi = slider(g, PG_SAMPLER, "End key", SL_ZHI, 384, y + 3 * ROW, 160, 200, ID_ZHI);
  g->zvel = labelled(g, PG_SAMPLER, "Velocity", L"EDIT", T | ES_AUTOHSCROLL, 548, y + 2 * ROW, 60, 96, 24, ID_ZVEL);
  SendMessageW(g->zvel, EM_SETLIMITTEXT, 30, 0);
  y += 4 * ROW;
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
  g->transpose_y = y;
  for (id = 0; id < NKNOB; id++) {             /* the knobs, in Tab order after SoundFont transpose (sf_knobs) */
    char t[32]; int x, yy;
    snprintf(t, sizeof t, "Knob %d", id + 1);
    knob_place(id, y, &x, &yy);
    g->knob[id] = slider(g, PG_FONT, t, SL_KNOB0 - id, x, yy, 120, 236, ID_KNOB + id);
    g->knob_lab[id] = g->all[PG_FONT][g->nall[PG_FONT] - 2];
  }
  param_box(g, PG_FONT, SMP_P_SF_EFFECTS, 384, y, 160, 200);
  g->fx_lab = g->all[PG_FONT][g->nall[PG_FONT] - 2];
  y += ROW;
  g->sfstatus = labelled(g, PG_FONT, "Status", L"EDIT", T | ES_AUTOHSCROLL | ES_READONLY, 8, y, 120, W_WIDTH - 140, 24, ID_SFSTATUS);
  g->sfstatus_lab = g->all[PG_FONT][g->nall[PG_FONT] - 2];
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
  free(g->dropped);
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
