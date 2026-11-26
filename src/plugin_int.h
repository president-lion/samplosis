/* Shared by plugin.c and gui.c: the plugin instance and what the window may do with it. Not part of the API.
   Everything here is main thread only, except where it says. */
#ifndef SMP_PLUGIN_INT_H
#define SMP_PLUGIN_INT_H
#include <windows.h>
#include <clap/clap.h>
#include "samplosis.h"

struct Gui;
typedef struct Plug {
  clap_plugin_t plugin;
  const clap_host_t *host;
  const clap_host_params_t *host_params;   /* NULL if the host has none */
  const clap_host_state_t *host_state;
  SmpMulti *e;                       /* the Sampler: its samples (multi.c) */
  SmpSynth *s;                       /* the SoundFont player; made with the engine, freed in destroy */
  volatile LONG plays;               /* 0 the sampler, 1 the SoundFont player (any thread reads it) */
  volatile LONG silence;             /* the audio thread resets the player not playing at its next block */
  volatile LONG offline;             /* the host renders offline (clap.render) */
  int bad;                           /* the last state was refused: the status says why */
  int active;                        /* between activate and deactivate (main thread): process may run */
  int sf_missing;                    /* the state's SoundFont couldn't be loaded: silent; its lines are kept */
  char *sf_line, *sf_used;           /* then its soundfont and sf_used lines, written back on save */
  char *sf_knobs;                    /* and its sf_knob lines (one or more, joined by newlines), or NULL */
  char sf_err[1400];                 /* then "Missing: <path>" or why it didn't load */
  double rate;
  char *text;                        /* the state as last accepted */
  int missing;                       /* how many of its samples couldn't be loaded (silent; written back as they came) */
  char status[1400];
  float *scratch; uint32_t maxf;     /* the right channel when a host gives a mono output */
  float *fade_buf;                   /* audio thread: a stereo pair (2 * maxf) the player switched away from plays
                                        its fade into */
  uint32_t switch_left;              /* audio thread: frames the old player still plays out after a switch */
  volatile LONG collect_left;        /* main-thread callbacks still wanted to free replaced samples */
  uint32_t since;                    /* audio thread: frames since it last asked for one */
  volatile LONG gui_any;             /* the window changed parameters: tell the host in the next flush or process */
  volatile LONG gui_pending[SMP_NALLPARAMS];
  struct Gui *gui;
  struct Loader *loader;             /* the window's sample loads, on a worker thread (made at the first) */
  int loading;                       /* one of them is under way: its file is loading_path */
  int loading_font;                  /* and it is a SoundFont (the SoundFont page's), not a sample */
  int loading_n;                     /* how many files it loads (several samples at once) */
  char loading_path[1100], loading_name[256];
  long load_failed;                  /* how many have failed; the last one's "Can't load x: why" is load_err */
  long load_notes;                   /* how many were stopped by a load of the other kind; the last one's "Stopped
                                        loading x to load y" is load_note */
  char load_note[1200];
  char load_err[1900];
  /* the last failed or stopped window load is told (in the status, for a window open or opened soon) until
     told_until (GetTickCount64), or until the next load succeeds or a state is applied; after that the status says
     how things are again (keep-length progress, say) rather than hiding it behind an old failure */
  char told[1900];
  ULONGLONG told_until;
} Plug;

extern const char *const smp_param_label[SMP_NALLPARAMS];    /* "Root note", "Volume", ... "SoundFont volume" */

void smp_plug_set_param(Plug *P, int id, double v);   /* from the window: the engine (ids below SMP_NPARAMS) or the
                                                         SoundFont player (31 to 34) at once, then the host is told */
double smp_plug_get_param(Plug *P, int id);
/* A sample loads in the background: 1 started (a load still under way is cancelled), 0 and a plain reason. The
   window's call returns at once; the status says "Loading x.wav", or "Decoding x.dsf with FFmpeg". dropped: when it
   loads, the sampler plays. */
int smp_plug_load_start(Plug *P, const char *path, int dropped, char *err, int cap);
/* Several samples at once, the same way: SMP_LOAD_ALL replaces every sample with these, SMP_LOAD_ADD adds them to
   the list, SMP_LOAD_ONE replaces sample index with the first (its ranges stay). Those that load are taken; the
   first that didn't is told as a failed load ("(and 2 more didn't load)"). The first new one is selected. */
enum { SMP_LOAD_ALL = 0, SMP_LOAD_ADD = 1, SMP_LOAD_ONE = 2 };
int smp_plug_load_many(Plug *P, const char *const *paths, int n, int how, int index, int dropped, char *err, int cap);
/* the Sampler's samples, for the window (main thread). Selecting one makes Root note its root (the host is told);
   a range set, or a sample removed, marks the project changed */
int smp_plug_zone_count(Plug *P);
int smp_plug_zone_selected(Plug *P);               /* -1 when there are none */
void smp_plug_zone_select(Plug *P, int i);
void smp_plug_zone_line(Plug *P, int i, char *out, int cap);   /* "Guitar C3.wav, root C3, keys C-1 to G9, ..." */
void smp_plug_zone_range(Plug *P, int i, int *key_lo, int *key_hi, int *vel_lo, int *vel_hi);
int smp_plug_zone_set_range(Plug *P, int i, int key_lo, int key_hi, int vel_lo, int vel_hi);   /* 0: not a range */
void smp_plug_zone_remove(Plug *P, int i);
int smp_plug_zone_picked(Plug *P, int i);          /* picked in the list: what range changes and Remove act on */
void smp_plug_zone_pick(Plug *P, int i, int on);
void smp_plug_zone_remove_picked(Plug *P);
/* Enter on the Sampler's File combo box: the selected sample goes back to restore (the file it had when the box got
   the focus, if trying files there changed it) and add joins the list as a new sample, selected and picked
   (selection stays on the restored one). Loads now, on this thread (they are in the cache: both were loaded just
   before); a load still under way is dropped. 1 done, 0 and a reason. */
int smp_plug_zone_keep(Plug *P, const char *restore, const char *add, char *err, int cap);         /* every picked sample (or the selected one when none is) */
int smp_plug_zone_key_class(Plug *P, int i);       /* notes in every octave: a bit per note (C is 1), or 0 (a range) */
int smp_plug_zone_set_key_class(Plug *P, int i, int pc);
/* A finished background load swapped in (main thread): 1 loaded, -1 it failed (load_failed counts up, load_err says
   why), 0 nothing finished. on_main_thread asks too, and tells the window. */
int smp_plug_poll_load(Plug *P);
int smp_plug_sample_path(Plug *P, char *path, int cap);  /* 1 a sample (or the one loading), 2 a missing one, 0 none */
void smp_plug_sample_text(Plug *P, char *out, int cap);  /* "Guitar C3.wav, 2.40 s, root C3 from the file name" */
void smp_plug_status_text(Plug *P, char *out, int cap);  /* "Keep length: 40 of 128 copies ready", or a reason; with
                                    the SoundFont player playing, "Ready: GM.sf2, 412 presets", "Loading samples for
                                    3 presets", "No SoundFont loaded" or "Missing: <path>" */
/* the SoundFont player */
int smp_plug_plays(Plug *P);                       /* 0 the sampler, 1 the SoundFont player */
void smp_plug_set_plays(Plug *P, int which);       /* marks the project dirty and silences the other player */
int smp_plug_load_font(Plug *P, const char *path, char *err, int cap);  /* 1 loaded (and plays it); 0 and a reason */
/* The SoundFont page's loads: in the background like a sample's (an SFZ reads every sample file's header). 1
   started, 0 and a plain reason; the status says "Loading x.sfz" until smp_plug_poll_load swaps it in on the main
   thread (then it plays). A newer load, or a project's state, cancels it. */
int smp_plug_load_font_start(Plug *P, const char *path, char *err, int cap);
int smp_plug_knob_count(Plug *P);                  /* a Decent Sampler preset's knobs (main thread): as
                                                      smp_synth_knob_*; a set marks the project changed */
int smp_plug_knob_info(Plug *P, int i, SmpKnobInfo *k);
double smp_plug_knob_get(Plug *P, int i);
void smp_plug_knob_set(Plug *P, int i, double v);
SmpSynth *smp_plug_synth(Plug *P);                 /* ask each time: activate at a new rate replaces the player,
                                                      so a pointer kept from before may have been freed */
void smp_plug_font_text(Plug *P, char *out, int cap);   /* "GM.sf2, 412 presets", "Piano.sfz, SFZ, 1 preset",
                                    "GM.dls, DLS, 235 presets", "No SoundFont loaded" or "Missing: <path>" (SF2 and SF3
                                    have no format word, as before; every other format names itself) */
void smp_plug_cant_load(Plug *P, const char *path, const char *why, char *out, int cap);   /* "Can't load x: why."
                                    plus " Still playing y." when something plays (one full stop between) */
void smp_plug_still_playing(Plug *P, char *out, int cap);   /* " Still playing y." (with its leading space) after a
                                    load that failed, when something plays (the font or sample that plays); else "" */
int smp_plug_sf_set(Plug *P, const char *line);    /* 1 taken; 0 the line didn't read (nothing changed: beep).
                                    One state line, like "sf_mode one", "sf_preset 0 25",
                                    "sf_channel_preset 3 0 25", "sf_channel_preset 3 song" (the song chooses again)
                                    or "sf_output 3 2"; the other settings stay. Marks the project dirty */
int smp_plug_font_path(Plug *P, char *path, int cap);   /* 1 a font, 2 a missing one, 0 none */
int smp_plug_sf_channel_now(Plug *P, int ch, int *bank, int *program);  /* the preset channel ch (1 to 16) plays
                                    now (all mode); 1 found, 0 none */

extern const clap_plugin_gui_t smp_gui_ext;
void smp_gui_close(Plug *P);
void smp_gui_loaded(Plug *P);                      /* a background load finished: the window shows it */
#endif
