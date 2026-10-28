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
  double rate;
  char *text;                        /* the state as last accepted */
  int missing;                       /* its sample couldn't be loaded: silent, and saving writes text's sample line */
  char status[1400];
  float *scratch; uint32_t maxf;     /* the right channel when a host gives a mono output */
  volatile LONG collect_left;        /* main-thread callbacks still wanted to free replaced samples */
  uint32_t since;                    /* audio thread: frames since it last asked for one */
  volatile LONG gui_any;             /* the window changed parameters: tell the host in the next flush or process */
  volatile LONG gui_pending[SMP_NPARAMS];
  struct Gui *gui;
} Plug;

extern const char *const smp_param_label[SMP_NPARAMS];    /* "Root note", "Volume", ... */

void smp_plug_set_param(Plug *P, int id, double v);   /* from the window: the engine at once, then the host is told */
double smp_plug_get_param(Plug *P, int id);
int smp_plug_load(Plug *P, const char *path, char *err, int cap);   /* 1 loaded; 0 and a plain reason */
int smp_plug_sample_path(Plug *P, char *path, int cap);  /* 1 a sample, 2 a missing one, 0 none */
void smp_plug_sample_text(Plug *P, char *out, int cap);  /* "Guitar C3.wav, 2.40 s, root C3 from the file name" */
void smp_plug_status_text(Plug *P, char *out, int cap);  /* "Keep length: 40 of 128 copies ready", or a reason */

extern const clap_plugin_gui_t smp_gui_ext;
void smp_gui_close(Plug *P);
#endif
