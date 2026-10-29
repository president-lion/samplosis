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
  SmpEngine *e;
  SmpSynth *s;                       /* the SoundFont player; made with the engine, freed in destroy */
  volatile LONG plays;               /* 0 the sampler, 1 the SoundFont player (any thread reads it) */
  volatile LONG silence;             /* the audio thread resets the player not playing at its next block */
  volatile LONG offline;             /* the host renders offline (clap.render) */
  int bad;                           /* the last state was refused: the status says why */
  int active;                        /* between activate and deactivate (main thread): process may run */
  int sf_missing;                    /* the state's SoundFont couldn't be loaded: silent; its lines are kept */
  char *sf_line, *sf_used;           /* then its soundfont and sf_used lines, written back on save */
  char sf_err[1400];                 /* then "Missing: <path>" or why it didn't load */
  double rate;
  char *text;                        /* the state as last accepted */
  int missing;                       /* its sample couldn't be loaded: silent, and saving writes text's sample line */
  char status[1400];
  float *scratch; uint32_t maxf;     /* the right channel when a host gives a mono output */
  volatile LONG collect_left;        /* main-thread callbacks still wanted to free replaced samples */
  uint32_t since;                    /* audio thread: frames since it last asked for one */
  volatile LONG gui_any;             /* the window changed parameters: tell the host in the next flush or process */
  volatile LONG gui_pending[SMP_NALLPARAMS];
  struct Gui *gui;
} Plug;

extern const char *const smp_param_label[SMP_NALLPARAMS];    /* "Root note", "Volume", ... "SoundFont volume" */

void smp_plug_set_param(Plug *P, int id, double v);   /* from the window: the engine (ids below SMP_NPARAMS) or the
                                                         SoundFont player (31 to 34) at once, then the host is told */
double smp_plug_get_param(Plug *P, int id);
int smp_plug_load(Plug *P, const char *path, char *err, int cap);   /* 1 loaded; 0 and a plain reason */
int smp_plug_sample_path(Plug *P, char *path, int cap);  /* 1 a sample, 2 a missing one, 0 none */
void smp_plug_sample_text(Plug *P, char *out, int cap);  /* "Guitar C3.wav, 2.40 s, root C3 from the file name" */
void smp_plug_status_text(Plug *P, char *out, int cap);  /* "Keep length: 40 of 128 copies ready", or a reason; with
                                    the SoundFont player playing, "Ready: GM.sf2, 412 presets", "Loading samples for
                                    3 presets", "No SoundFont loaded" or "Missing: <path>" */
/* the SoundFont player */
int smp_plug_plays(Plug *P);                       /* 0 the sampler, 1 the SoundFont player */
void smp_plug_set_plays(Plug *P, int which);       /* marks the project dirty and silences the other player */
int smp_plug_load_font(Plug *P, const char *path, char *err, int cap);  /* 1 loaded (and plays it); 0 and a reason */
SmpSynth *smp_plug_synth(Plug *P);                 /* ask each time: activate at a new rate replaces the player,
                                                      so a pointer kept from before may have been freed */
void smp_plug_font_text(Plug *P, char *out, int cap);   /* "GM.sf2, 412 presets", "No SoundFont loaded" or
                                                          "Missing: <path>" */
int smp_plug_sf_set(Plug *P, const char *line);    /* 1 taken; 0 the line didn't read (nothing changed: beep).
                                    One state line, like "sf_mode one", "sf_preset 0 25",
                                    "sf_channel_preset 3 0 25", "sf_channel_preset 3 song" (the song chooses again)
                                    or "sf_output 3 2"; the other settings stay. Marks the project dirty */
int smp_plug_sf_channel_now(Plug *P, int ch, int *bank, int *program);  /* the preset channel ch (1 to 16) plays
                                    now (all mode); 1 found, 0 none */

extern const clap_plugin_gui_t smp_gui_ext;
void smp_gui_close(Plug *P);
#endif
