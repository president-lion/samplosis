/* plugin.c: Samplosis as a CLAP instrument. One note input (CLAP and MIDI), one stereo output, every engine
   parameter automatable, and the instrument as text (script.c) for its state.

   Threads. The engine is made in init (48 kHz) so state loaded before activate has somewhere to go; activate at
   another rate makes a new engine from the old one's text. State load, activate, deactivate, on_main_thread and
   destroy are on the main thread; the sample is loaded there, synchronously (from the cache, or decoded once).
   Process runs on the audio thread: it splits the block at each event, so parameter changes and notes land on
   their exact frame, and touches nothing but the engine. Replaced samples are freed by smp_engine_collect on the
   main thread: after a state load the audio thread asks the host for a few main-thread callbacks (request_callback
   is thread-safe) and each one collects.

   A sample that can't be loaded (the file and its cache copy both gone) leaves the instrument silent with the
   status "Missing: <path>"; the text it came in is kept, so saving writes the path back rather than losing it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <clap/clap.h>
#include "engine_int.h"

#define DEFAULT_RATE 48000.0
#define COLLECT_ROUNDS 8

typedef struct {
  clap_plugin_t plugin;
  const clap_host_t *host;
  SmpEngine *e;
  double rate;
  char *text;                        /* the state as last accepted */
  int missing;                       /* its sample couldn't be loaded: silent, and saving writes text's sample line */
  char status[1400];
  float *scratch; uint32_t maxf;     /* the right channel when a host gives a mono output */
  volatile LONG collect_left;        /* main-thread callbacks still wanted to free replaced samples */
  uint32_t since;                    /* audio thread: frames since it last asked for one */
} Plug;

static const char *const features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SAMPLER,
                                       CLAP_PLUGIN_FEATURE_STEREO, NULL};
static const clap_plugin_descriptor_t desc = {
  CLAP_VERSION_INIT, "com.audioskit.samplosis", "Samplosis", "audioskit", "", "", "", "0.1.0",
  "A sampler you can use with a screen reader: one sample across the keyboard", features};

static Plug *plug_of(const clap_plugin_t *p) { return p && p->desc == &desc ? (Plug *)p->plugin_data : NULL; }

/* ---- the instrument as text ---- */
static char *engine_text(const SmpEngine *e) {
  int n = smp_script_write(e, NULL, 0);
  char *t = malloc((size_t)n + 1);
  if (t) smp_script_write(e, t, n + 1);
  return t;
}

/* The first `sample` line of text (without its line end) into line, and its path into path. 0 if there is none,
   1 if it isn't well formed, 2 if it is (a quoted path, then nothing or key= and 16 hex digits). */
static int sample_line(const char *text, char *line, int lcap, char *path, int pcap) {
  const char *p = text;
  while (p && *p) {
    const char *eol = strchr(p, '\n'), *s = p, *end;
    size_t len;
    end = eol ? eol : p + strlen(p);
    while (s < end && (*s == ' ' || *s == '\t')) s++;
    if (end - s > 6 && !strncmp(s, "sample", 6) && (s[6] == ' ' || s[6] == '\t')) {
      const char *q, *qe;
      while (end > s && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
      len = (size_t)(end - s);
      snprintf(line, (size_t)lcap, "%.*s", (int)len, s);
      q = strchr(s, '"');
      qe = q && q < end ? memchr(q + 1, '"', (size_t)(end - q - 1)) : NULL;
      if (qe) snprintf(path, (size_t)pcap, "%.*s", (int)(qe - q - 1), q + 1);
      else snprintf(path, (size_t)pcap, "%s", "");
      if (!qe || qe == q + 1) return 1;                    /* not well formed: no path */
      qe++;
      while (qe < end && (*qe == ' ' || *qe == '\t')) qe++;
      if (qe == end) return 2;
      if (end - qe != 20 || strncmp(qe, "key=", 4)) return 1;
      for (q = qe + 4; q < end; q++) if (!isxdigit((unsigned char)*q)) return 1;
      return 2;
    }
    p = eol ? eol + 1 : NULL;
  }
  return 0;
}

/* text without its sample lines */
static char *without_sample(const char *text) {
  char *out = malloc(strlen(text) + 1), *o = out;
  const char *p = text;
  if (!out) return NULL;
  while (*p) {
    const char *eol = strchr(p, '\n'), *s = p, *end = eol ? eol + 1 : p + strlen(p);
    while (*s == ' ' || *s == '\t') s++;
    if (!(!strncmp(s, "sample", 6) && (s[6] == ' ' || s[6] == '\t' || s[6] == '"'))) {
      memcpy(o, p, (size_t)(end - p)); o += end - p;
    }
    p = end;
  }
  *o = 0;
  return out;
}

static void keep_text(Plug *P, const char *text) {
  char *t = _strdup(text);
  if (!t) return;
  free(P->text); P->text = t;
}

static void ask_collect(Plug *P) {
  InterlockedExchange(&P->collect_left, COLLECT_ROUNDS);
  if (P->host && P->host->request_callback) P->host->request_callback(P->host);
}

static int file_exists(const char *utf8) {
  wchar_t w[1100];
  if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 1100)) return 0;
  return GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
}

/* Applies a state text. 1 when it's taken (the sample may still be missing), 0 when it doesn't read (nothing
   changes). Main thread. */
static int apply_text(Plug *P, const char *text) {
  char err[700] = "", line[1200], path[1100];
  if (!P->e) { snprintf(P->status, sizeof P->status, "Not working: the sound engine couldn't start"); return 0; }
  if (!smp_script_apply(P->e, text, err, sizeof err)) {
    const SmpSample *s;
    if (!sample_line(text, line, sizeof line, path, sizeof path)) smp_engine_set_sample(P->e, NULL);   /* an empty
                                    instrument: script_apply keeps the current sample, a state without one doesn't */
    s = smp_engine_sample(P->e);
    P->missing = 0;
    keep_text(P, text);
    if (s) snprintf(P->status, sizeof P->status, "Ready: %s", s->name);
    else snprintf(P->status, sizeof P->status, "No sample loaded");
    ask_collect(P);
    return 1;
  }
  if (sample_line(text, line, sizeof line, path, sizeof path) == 2) {
    /* a well-formed sample line that didn't load; if everything else reads, take it without the sample */
    char *rest = without_sample(text), err2[700];
    if (rest && !smp_script_apply(P->e, rest, err2, sizeof err2)) {
      free(rest);
      smp_engine_set_sample(P->e, NULL);
      P->missing = 1;
      keep_text(P, text);
      if (!file_exists(path)) snprintf(P->status, sizeof P->status, "Missing: %s", path);
      else snprintf(P->status, sizeof P->status, "Can't load %s: %s", path, err);
      ask_collect(P);
      return 1;
    }
    free(rest);
  }
  snprintf(P->status, sizeof P->status, "The saved instrument doesn't read, so nothing changed: %s", err);
  return 0;
}

/* The state to save: the engine's text, or with the sample missing, its text with the original sample line. */
static char *state_text(Plug *P) {
  char *t, *out, line[1200], path[1100];
  if (!P->e) return _strdup(P->text ? P->text : "# Samplosis 0.1.0\n");
  t = engine_text(P->e);
  if (!t || !P->missing || !P->text || !sample_line(P->text, line, sizeof line, path, sizeof path)) return t;
  {
    char *nl = strchr(t, '\n');
    size_t head = nl ? (size_t)(nl - t) + 1 : strlen(t), ll = strlen(line);
    out = malloc(strlen(t) + ll + 2);
    if (!out) return t;
    memcpy(out, t, head);
    memcpy(out + head, line, ll);
    out[head + ll] = '\n';
    strcpy(out + head + ll + 1, t + head);
    free(t);
    return out;
  }
}

/* ---- life cycle ---- */
static bool p_init(const clap_plugin_t *p) {
  Plug *P = p->plugin_data;
  P->rate = DEFAULT_RATE;
  P->e = smp_engine_new(P->rate, 4096);
  snprintf(P->status, sizeof P->status, P->e ? "No sample loaded" : "Not working: the sound engine couldn't start");
  return P->e != NULL;
}

static void p_deactivate(const clap_plugin_t *p) {
  Plug *P = p->plugin_data;
  if (P->e) smp_engine_collect(P->e);
}

static void p_destroy(const clap_plugin_t *p) {
  Plug *P = p->plugin_data;
  smp_engine_free(P->e);
  free(P->text); free(P->scratch);
  free(P);
}

static bool p_activate(const clap_plugin_t *p, double rate, uint32_t minf, uint32_t maxf) {
  Plug *P = p->plugin_data;
  (void)minf;
  if (rate <= 0) return false;
  free(P->scratch);
  P->maxf = maxf ? maxf : 4096;
  P->scratch = malloc(sizeof(float) * P->maxf);
  if (!P->scratch) return false;
  if (!P->e || rate != P->rate) {        /* a new engine at this rate, with the same instrument */
    char *t = state_text(P);
    smp_engine_free(P->e);
    P->rate = rate;
    P->e = smp_engine_new(rate, (int)P->maxf);
    if (!P->e) { free(t); snprintf(P->status, sizeof P->status, "Not working: the sound engine couldn't start"); return false; }
    if (t) apply_text(P, t);             /* text the engine wrote (or was given) always reads */
    free(t);
  }
  else smp_engine_reset(P->e, 1);         /* nothing processes now: voices from before deactivate end at once */
  P->since = 0;
  return true;
}

static bool p_start(const clap_plugin_t *p) { (void)p; return true; }
static void p_stop(const clap_plugin_t *p) { (void)p; }
static void p_reset(const clap_plugin_t *p) {   /* audio thread: every voice ends (a 5 ms fade), bend to center */
  Plug *P = p->plugin_data;
  if (P->e) smp_engine_reset(P->e, 0);
}

/* ---- events ---- */
static int vel127(double v) { int k = (int)floor(v * 127.0 + 0.5); return k < 0 ? 0 : k > 127 ? 127 : k; }

static void all_off(SmpEngine *e, int at) { int k; for (k = 0; k < 128; k++) smp_engine_note_off(e, k, at); }

/* one event at offset `at` in the sub-block that follows; parameter values apply at once */
static void event(Plug *P, const clap_event_header_t *h, int at) {
  SmpEngine *e = P->e;
  if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
  switch (h->type) {
    case CLAP_EVENT_NOTE_ON: {
      const clap_event_note_t *n = (const clap_event_note_t *)h;
      if (n->key >= 0 && n->key < 128) smp_engine_note_on(e, n->key, vel127(n->velocity), at);
      break;
    }
    case CLAP_EVENT_NOTE_OFF: case CLAP_EVENT_NOTE_CHOKE: {
      const clap_event_note_t *n = (const clap_event_note_t *)h;
      if (n->key < 0) all_off(e, at);
      else if (n->key < 128) smp_engine_note_off(e, n->key, at);
      break;
    }
    case CLAP_EVENT_MIDI: {
      const uint8_t *d = ((const clap_event_midi_t *)h)->data;
      int st = d[0] & 0xF0;
      if (st == 0x90 && d[2]) smp_engine_note_on(e, d[1] & 0x7F, d[2] & 0x7F, at);
      else if (st == 0x80 || st == 0x90) smp_engine_note_off(e, d[1] & 0x7F, at);
      else if (st == 0xE0) smp_engine_bend(e, (((d[2] & 0x7F) << 7 | (d[1] & 0x7F)) - 8192) / 8192.0, at);
      else if (st == 0xB0 && (d[1] == 123 || d[1] == 120)) all_off(e, at);   /* all notes off, all sound off */
      break;
    }
    case CLAP_EVENT_PARAM_VALUE: {
      const clap_event_param_value_t *v = (const clap_event_param_value_t *)h;
      if (v->param_id < SMP_NPARAMS) smp_engine_set_param(e, (int)v->param_id, v->value);
      break;
    }
  }
}

static clap_process_status p_process(const clap_plugin_t *p, const clap_process_t *pr) {
  Plug *P = p->plugin_data;
  uint32_t n = pr->frames_count, ne = pr->in_events ? pr->in_events->size(pr->in_events) : 0, i, pos = 0;
  const clap_audio_buffer_t *ob;
  float *l, *r;
  if (!pr->audio_outputs_count) return CLAP_PROCESS_CONTINUE;
  ob = &pr->audio_outputs[0];
  if (!ob->data32 || !ob->channel_count) return CLAP_PROCESS_ERROR;
  l = ob->data32[0];
  r = ob->channel_count > 1 ? ob->data32[1] : P->scratch;
  if (!P->e || (ob->channel_count < 2 && n > P->maxf)) {
    for (i = 0; i < ob->channel_count; i++) memset(ob->data32[i], 0, sizeof(float) * n);
    return CLAP_PROCESS_CONTINUE;
  }
  /* the block runs in pieces between event times, so every event lands on its frame */
  for (i = 0; i < ne; i++) {
    const clap_event_header_t *h = pr->in_events->get(pr->in_events, i);
    uint32_t t = h->time >= n ? (n ? n - 1 : 0) : h->time;   /* a time past the block: its last frame */
    if (t < pos) t = pos;
    if (t > pos) { smp_engine_process(P->e, l + pos, r + pos, (int)(t - pos)); pos = t; }
    event(P, h, 0);
  }
  if (n > pos) smp_engine_process(P->e, l + pos, r + pos, (int)(n - pos));
  else smp_engine_process(P->e, l, r, 0);              /* clears events queued at the very end */
  if (ob->channel_count < 2) for (i = 0; i < n; i++) l[i] = 0.5f * (l[i] + r[i]);
  for (i = 2; i < ob->channel_count; i++) memset(ob->data32[i], 0, sizeof(float) * n);
  if (P->collect_left > 0) {
    P->since += n;
    if (P->since >= (uint32_t)(P->rate / 4)) { P->since = 0; if (P->host->request_callback) P->host->request_callback(P->host); }
  }
  return CLAP_PROCESS_CONTINUE;
}

static void p_main(const clap_plugin_t *p) {
  Plug *P = p->plugin_data;
  if (P->e) smp_engine_collect(P->e);
  if (P->collect_left > 0) InterlockedDecrement(&P->collect_left);
}

/* ---- note ports ---- */
static uint32_t np_count(const clap_plugin_t *p, bool in) { (void)p; return in ? 1 : 0; }
static bool np_get(const clap_plugin_t *p, uint32_t i, bool in, clap_note_port_info_t *info) {
  (void)p;
  if (!in || i) return false;
  info->id = 0;
  info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
  info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
  snprintf(info->name, sizeof info->name, "Notes");
  return true;
}
static const clap_plugin_note_ports_t note_ports = {np_count, np_get};

/* ---- audio ports ---- */
static uint32_t ap_count(const clap_plugin_t *p, bool in) { (void)p; return in ? 0 : 1; }
static bool ap_get(const clap_plugin_t *p, uint32_t i, bool in, clap_audio_port_info_t *info) {
  (void)p;
  if (in || i) return false;
  info->id = 0;
  snprintf(info->name, sizeof info->name, "Output");
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
static const clap_plugin_audio_ports_t audio_ports = {ap_count, ap_get};

/* ---- parameters (ids are the engine's) ---- */
static const char *const pretty[SMP_NPARAMS] = {
  "Root note", "Transpose", "Fine tune", "Volume", "Pan", "Attack", "Decay", "Sustain", "Release",
  "Velocity sensitivity", "Minimum volume", "Mode", "Lowest note", "Highest note", "Pitch at lowest note",
  "Lowest velocity", "Highest velocity", "Obey note off", "Loop", "Loop start", "Start", "End", "Bend range",
  "Pitch at highest note", "Reverse", "Keep length", "Voices", "Probability", "Speed"};

static uint32_t pa_count(const clap_plugin_t *p) { (void)p; return SMP_NPARAMS; }
static bool pa_info(const clap_plugin_t *p, uint32_t i, clap_param_info_t *info) {
  const SmpParamInfo *pi = smp_param_info((int)i);
  (void)p;
  if (!pi) return false;
  memset(info, 0, sizeof *info);
  info->id = i;
  info->flags = CLAP_PARAM_IS_AUTOMATABLE;
  switch (pi->kind) {                    /* whole steps: notes, semitones, cents, choices, switches, counts */
    case SMP_K_NOTE: case SMP_K_SEMI: case SMP_K_CENT: case SMP_K_CHOICE: case SMP_K_BOOL: case SMP_K_COUNT:
      info->flags |= CLAP_PARAM_IS_STEPPED;
  }
  if (pi->kind == SMP_K_CHOICE || pi->kind == SMP_K_BOOL) info->flags |= CLAP_PARAM_IS_ENUM;
  snprintf(info->name, sizeof info->name, "%s", pretty[i]);
  info->min_value = pi->lo; info->max_value = pi->hi; info->default_value = pi->def;
  return true;
}
static bool pa_value(const clap_plugin_t *p, clap_id id, double *v) {
  Plug *P = p->plugin_data;
  if (id >= SMP_NPARAMS) return false;
  *v = P->e ? smp_engine_get_param(P->e, (int)id) : smp_param_info((int)id)->def;
  return true;
}
static bool pa_text(const clap_plugin_t *p, clap_id id, double v, char *out, uint32_t cap) {
  (void)p;
  if (id >= SMP_NPARAMS || !cap) return false;
  smp_value_text((int)id, v, out, (int)cap);
  return true;
}
static bool pa_parse(const clap_plugin_t *p, clap_id id, const char *s, double *v) {
  (void)p;
  return id < SMP_NPARAMS && smp_value_parse((int)id, s, v);
}
static void pa_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) {
  Plug *P = p->plugin_data; uint32_t i, n = in ? in->size(in) : 0;
  (void)out;
  if (!P->e) return;
  for (i = 0; i < n; i++) {
    const clap_event_header_t *h = in->get(in, i);
    if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) event(P, h, 0);
  }
}
static const clap_plugin_params_t params = {pa_count, pa_info, pa_value, pa_text, pa_parse, pa_flush};

/* ---- state: the instrument as text ---- */
static bool st_save(const clap_plugin_t *p, const clap_ostream_t *s) {
  Plug *P = p->plugin_data;
  char *t = state_text(P);
  int64_t done = 0, k = t ? (int64_t)strlen(t) : 0;
  if (!t) return false;
  while (done < k) {
    int64_t m = s->write(s, t + done, (uint64_t)(k - done));
    if (m <= 0) { free(t); return false; }
    done += m;
  }
  free(t);
  return true;
}
static bool st_load(const clap_plugin_t *p, const clap_istream_t *s) {
  Plug *P = p->plugin_data;
  size_t cap = 8192, got = 0; int64_t k; char *buf = malloc(cap), *nb; int ok;
  if (!buf) return false;
  for (;;) {
    if (got + 1 >= cap) {
      if (cap >= (1u << 24)) { free(buf); return false; }   /* 16 MB: not an instrument */
      nb = realloc(buf, cap * 2);
      if (!nb) { free(buf); return false; }
      buf = nb; cap *= 2;
    }
    k = s->read(s, buf + got, (uint64_t)(cap - 1 - got));
    if (k < 0) { free(buf); return false; }
    if (k == 0) break;
    got += (size_t)k;
  }
  buf[got] = 0;
  ok = apply_text(P, buf);
  free(buf);
  return ok != 0;
}
static const clap_plugin_state_t state = {st_save, st_load};

static uint32_t lat_get(const clap_plugin_t *p) { (void)p; return 0; }
static const clap_plugin_latency_t latency = {lat_get};

static const void *p_ext(const clap_plugin_t *p, const char *id) {
  (void)p;
  if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &note_ports;
  if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audio_ports;
  if (!strcmp(id, CLAP_EXT_PARAMS)) return &params;
  if (!strcmp(id, CLAP_EXT_STATE)) return &state;
  if (!strcmp(id, CLAP_EXT_LATENCY)) return &latency;
  return NULL;
}

/* ---- the status, for the window (and the test host) ---- */
SMP_API int smp_plugin_status(const clap_plugin_t *p, char *out, int cap) {
  Plug *P = plug_of(p);
  if (!P || !out || cap <= 0) return 0;
  snprintf(out, (size_t)cap, "%s", P->status);
  return 1;
}

/* ---- test hooks (tests\host.c): make a real-time plugin render exactly like smp_engine_render_test ---- */
SMP_API int smp_test_plugin_wait(const clap_plugin_t *p, int timeout_ms) {   /* keep-length copies and start heads */
  Plug *P = plug_of(p);
  if (!P || !P->e) return 1;
  if (smp_engine_wait_renders(P->e, timeout_ms)) return 1;
  smp_worker_wait_idle(P->e);
  return 0;
}
SMP_API void smp_test_plugin_offline(const clap_plugin_t *p, int on) {   /* note-ons past the head wait for the streamer */
  Plug *P = plug_of(p);
  if (P && P->e) P->e->offline = on;
}
SMP_API int smp_test_plugin_voices(const clap_plugin_t *p) {   /* sounding voices */
  Plug *P = plug_of(p);
  return P && P->e ? smp_engine_active_voices(P->e) : 0;
}
SMP_API void smp_test_plugin_sync(const clap_plugin_t *p) {   /* a whole streamer pass; call between process calls */
  Plug *P = plug_of(p);
  if (P && P->e) smp_stream_wait_idle(P->e);
}

/* ---- factory and entry ---- */
static const clap_plugin_t *f_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
  Plug *P;
  (void)f;
  if (!clap_version_is_compatible(host->clap_version) || strcmp(id, desc.id)) return NULL;
  P = calloc(1, sizeof *P);
  if (!P) return NULL;
  P->host = host;
  P->plugin.desc = &desc;
  P->plugin.plugin_data = P;
  P->plugin.init = p_init;
  P->plugin.destroy = p_destroy;
  P->plugin.activate = p_activate;
  P->plugin.deactivate = p_deactivate;
  P->plugin.start_processing = p_start;
  P->plugin.stop_processing = p_stop;
  P->plugin.reset = p_reset;
  P->plugin.process = p_process;
  P->plugin.get_extension = p_ext;
  P->plugin.on_main_thread = p_main;
  return &P->plugin;
}
static uint32_t f_count(const clap_plugin_factory_t *f) { (void)f; return 1; }
static const clap_plugin_descriptor_t *f_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i ? NULL : &desc; }
static const clap_plugin_factory_t factory = {f_count, f_desc, f_create};

static bool e_init(const char *path) { (void)path; return true; }
static void e_deinit(void) {}
static const void *e_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? NULL : &factory; }
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, e_init, e_deinit, e_factory};
