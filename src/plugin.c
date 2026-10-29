/* plugin.c: Samplosis as a CLAP instrument. Two players: the sampler (the engine) and the SoundFont player (the
   synth, sfsynth.c); `plays` says which one hears the notes, and both keep their settings. One note input (MIDI
   preferred, CLAP notes too: they reach the SoundFont player as MIDI on their channel), 16 stereo outputs (the
   sampler writes the first; the SoundFont player routes channels with sf_output), every parameter of both players
   automatable, and the state as text: `plays sampler|soundfont`, then the sampler's text (script.c), then the
   SoundFont player's lines.

   Threads. The engine is made in init (48 kHz) so state loaded before activate has somewhere to go; activate at
   another rate makes a new engine from the old one's text. State load, activate, deactivate, on_main_thread and
   destroy are on the main thread; the sample is loaded there, synchronously (from the cache, or decoded once).
   Process runs on the audio thread: it splits the block at each event, so parameter changes and notes land on
   their exact frame, and touches nothing but the engine. Replaced samples are freed by smp_engine_collect on the
   main thread: after a state load the audio thread asks the host for a few main-thread callbacks (request_callback
   is thread-safe) and each one collects.

   A sample that can't be loaded (the file and its cache copy both gone) leaves the instrument silent with the
   status "Missing: <path>"; the text it came in is kept, so saving writes the path back rather than losing it.
   A SoundFont that can't be loaded is the same: silence, "Missing: <path>", and its soundfont and sf_used lines
   are written back as they came.

   The SoundFont player is made with the engine, made again at a new rate (from the same text), and freed only in
   destroy: smp_synth_free closes fonts, which may join the loader thread, so never at DLL unload. Replaced fonts
   close in smp_synth_collect on the main thread, as replaced samples do. The host's offline render (clap.render)
   lets the player wait for its samples on the audio thread, so a render to file is exact. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <clap/clap.h>
#include "engine_int.h"
#include "plugin_int.h"
#include "sf2.h"

#define DEFAULT_RATE 48000.0
#define COLLECT_ROUNDS 8
#define NOUTS 16                 /* stereo output ports */
#define SF_BLOCK 4096            /* the SoundFont player's piece size; it splits longer blocks itself */

/* Plug (plugin_int.h) is shared with the window (gui.c). */

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

/* The last line whose first word is word: 1, with the line (trimmed) in line, the rest after the word in rest and
   its number in *lineno; 0 if there is none. Any out pointer may be NULL. */
static int word_line(const char *text, const char *word, char *line, int lcap, char *rest, int rcap, int *lineno) {
  const char *p = text; int n = 0, found = 0; size_t wl = strlen(word);
  while (p && *p) {
    const char *eol = strchr(p, '\n'), *s = p, *end = eol ? eol : p + strlen(p);
    n++;
    while (s < end && (*s == ' ' || *s == '\t')) s++;
    if ((size_t)(end - s) >= wl && !strncmp(s, word, wl) && (s + wl == end || s[wl] == ' ' || s[wl] == '\t' ||
                                                             s[wl] == '\r' || s[wl] == '"')) {
      const char *r = s + wl;
      while (end > s && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
      while (r < end && (*r == ' ' || *r == '\t')) r++;
      if (line) snprintf(line, (size_t)lcap, "%.*s", (int)(end - s), s);
      if (rest) snprintf(rest, (size_t)rcap, "%.*s", (int)(end > r ? end - r : 0), r);
      if (lineno) *lineno = n;
      found = 1;
    }
    p = eol ? eol + 1 : NULL;
  }
  return found;
}

/* text without the lines whose first word is word */
static char *without_word(const char *text, const char *word) {
  char *out = malloc(strlen(text) + 1), *o = out;
  const char *p = text; size_t wl = strlen(word);
  if (!out) return NULL;
  while (*p) {
    const char *eol = strchr(p, '\n'), *s = p, *end = eol ? eol + 1 : p + strlen(p);
    while (*s == ' ' || *s == '\t') s++;
    if (!(!strncmp(s, word, wl) && (s[wl] == ' ' || s[wl] == '\t' || s[wl] == '"' || s[wl] == '\r' ||
                                    s[wl] == '\n' || !s[wl]))) {
      memcpy(o, p, (size_t)(end - p)); o += end - p;
    }
    p = end;
  }
  *o = 0;
  return out;
}

/* the plays line: 0 the sampler (also when there is none), 1 the SoundFont player, -1 and err when it doesn't read */
static int plays_of(const char *text, char *err, int cap) {
  char rest[200]; int ln = 0;
  if (!word_line(text, "plays", NULL, 0, rest, sizeof rest, &ln) || !strcmp(rest, "sampler")) return 0;
  if (!strcmp(rest, "soundfont")) return 1;
  snprintf(err, (size_t)cap, "line %d: plays is sampler or soundfont, not '%s'", ln, rest);
  return -1;
}

/* the soundfont line: 0 none, 1 not well formed, 2 well formed (soundfont "path" and nothing after), with the
   line and its path */
static int font_line(const char *text, char *line, int lcap, char *path, int pcap) {
  char rest[1200]; size_t n;
  line[0] = path[0] = 0;
  if (!word_line(text, "soundfont", line, lcap, rest, sizeof rest, NULL)) return 0;
  n = strlen(rest);
  if (n < 3 || rest[0] != '"' || rest[n - 1] != '"' || memchr(rest + 1, '"', n - 2)) return 1;
  snprintf(path, (size_t)pcap, "%.*s", (int)(n - 2), rest + 1);
  return 2;
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

/* Applies a state text to the sampler. 1 when it's taken (the sample may still be missing), 0 when it doesn't
   read (nothing changes). Main thread. */
static int apply_sampler(Plug *P, const char *text) {
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

/* The sampler's part of the state: the engine's text, or with the sample missing, its text with the original
   sample line. */
static char *sampler_text(Plug *P) {
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

/* The SoundFont player's part: its lines, or with the font missing, its lines with the original soundfont line
   first and the original sf_used line just before the parameters, where the player writes them. */
static char *synth_text(Plug *P) {
  int n = smp_synth_script_write(P->s, NULL, 0);
  char *t = malloc((size_t)n + 1), *out, *at;
  const char *first = smp_param_info(SMP_NPARAMS)->name;
  size_t head, fl, ul, k = strlen(first);
  if (!t) return NULL;
  smp_synth_script_write(P->s, t, n + 1);
  if (!P->sf_missing || !P->sf_line) return t;
  fl = strlen(P->sf_line); ul = P->sf_used ? strlen(P->sf_used) : 0;
  out = malloc((size_t)n + fl + ul + 3);
  if (!out) return t;
  for (at = t; at; at = strchr(at, '\n') ? strchr(at, '\n') + 1 : NULL)
    if (!strncmp(at, first, k) && at[k] == ' ') break;
  head = at ? (size_t)(at - t) : (size_t)n;
  memcpy(out, P->sf_line, fl); out[fl] = '\n';
  memcpy(out + fl + 1, t, head);
  if (ul) { memcpy(out + fl + 1 + head, P->sf_used, ul); out[fl + 1 + head + ul] = '\n'; ul++; }
  strcpy(out + fl + 1 + head + ul, t + head);
  free(t);
  return out;
}

/* The whole state: plays, the sampler's text, the SoundFont player's lines. */
static char *state_text(Plug *P) {
  char *a = sampler_text(P), *b = P->s ? synth_text(P) : _strdup(""), *out = NULL;
  if (a && b && (out = malloc(strlen(a) + strlen(b) + 32)))
    sprintf(out, "plays %s\n%s%s", P->plays ? "soundfont" : "sampler", a, b);
  free(a); free(b);
  return out;
}

static void refuse(Plug *P, const char *why) {
  snprintf(P->status, sizeof P->status, "The saved instrument doesn't read, so nothing changed: %s", why);
  P->bad = 1;
}

static void set_plays(Plug *P, int which) {
  if (P->plays == which) return;
  InterlockedExchange(&P->plays, which);
  InterlockedExchange(&P->silence, 1);    /* the other player's voices end at the next block */
}

/* Applies a whole state: plays, the sampler's text and the SoundFont player's lines. 1 when it's taken (the sample
   or the font may be missing), 0 when it doesn't read (nothing changes). Main thread. */
static int apply_text(Plug *P, const char *text) {
  char err[1400] = "", line[1200], path[1100], used[1200];
  char *rest; int plays, fk, r;
  SmpSynth *chk;
  if (!P->e || !P->s) { snprintf(P->status, sizeof P->status, "Not working: the sound engine couldn't start"); return 0; }
  plays = plays_of(text, err, sizeof err);
  if (plays < 0) { refuse(P, err); return 0; }
  /* The player's lines are checked first, on a player of its own without the font, so a bad line refuses the
     whole state before the sampler changes. After that only the font can fail. */
  fk = font_line(text, line, sizeof line, path, sizeof path);
  rest = fk == 2 ? without_word(text, "soundfont") : _strdup(text);
  if (!rest) { refuse(P, "out of memory"); return 0; }
  chk = smp_synth_new(P->rate, 64);
  r = chk ? smp_synth_script_apply(chk, rest, err, sizeof err) : -1;
  smp_synth_free(chk);
  if (r) { free(rest); refuse(P, r < 0 ? "out of memory" : err); return 0; }
  if (!apply_sampler(P, text)) { free(rest); P->bad = 1; return 0; }
  free(P->sf_line); free(P->sf_used); P->sf_line = P->sf_used = NULL; P->sf_missing = 0;
  if (!smp_synth_script_apply(P->s, text, err, sizeof err)) {
    if (!fk) smp_synth_unload(P->s);      /* a state without a font has none, as one without a sample */
  } else {                                /* the font didn't load: everything else, and silence */
    smp_synth_script_apply(P->s, rest, NULL, 0);
    smp_synth_unload(P->s);
    P->sf_missing = 1;
    P->sf_line = _strdup(line);
    if (word_line(text, "sf_used", used, sizeof used, NULL, 0, NULL)) P->sf_used = _strdup(used);
    if (!file_exists(path)) snprintf(P->sf_err, sizeof P->sf_err, "Missing: %s", path);
    else snprintf(P->sf_err, sizeof P->sf_err, "Can't load the SoundFont: %s", err);
  }
  free(rest);
  set_plays(P, plays);
  P->bad = 0;
  ask_collect(P);
  return 1;
}

/* ---- life cycle ---- */
static void midi3(SmpSynth *s, int st, int d1, int d2, int at);

static bool p_init(const clap_plugin_t *p) {
  Plug *P = p->plugin_data;
  P->rate = DEFAULT_RATE;
  if (P->host && P->host->get_extension) {
    P->host_params = P->host->get_extension(P->host, CLAP_EXT_PARAMS);
    P->host_state = P->host->get_extension(P->host, CLAP_EXT_STATE);
  }
  P->e = smp_engine_new(P->rate, 4096);
  P->s = smp_synth_new(P->rate, SF_BLOCK);
  snprintf(P->status, sizeof P->status, P->e && P->s ? "No sample loaded" : "Not working: the sound engine couldn't start");
  return P->e != NULL && P->s != NULL;
}

static void p_deactivate(const clap_plugin_t *p) {
  Plug *P = p->plugin_data;
  if (P->e) smp_engine_collect(P->e);
  if (P->s) smp_synth_collect(P->s);
}

static void p_destroy(const clap_plugin_t *p) {
  Plug *P = p->plugin_data;
  smp_gui_close(P);                      /* a host that didn't destroy the window first */
  smp_engine_free(P->e);
  smp_synth_free(P->s);                  /* here, on the main thread, never at DLL unload (it may join the loader) */
  free(P->text); free(P->scratch); free(P->sf_line); free(P->sf_used);
  free(P);
}

static bool p_activate(const clap_plugin_t *p, double rate, uint32_t minf, uint32_t maxf) {
  Plug *P = p->plugin_data;
  (void)minf;
  if (rate <= 0) return false;
  free(P->scratch);
  P->maxf = maxf ? maxf : 4096;
  P->scratch = malloc(sizeof(float) * 2 * P->maxf);   /* also a stereo pair when the host's main output isn't one */
  if (!P->scratch) return false;
  if (!P->e || !P->s || rate != P->rate) {   /* new players at this rate, with the same instrument */
    char *t = state_text(P);
    SmpSynth *old = P->s, *s = smp_synth_new(rate, SF_BLOCK);
    if (!s) { free(t); snprintf(P->status, sizeof P->status, "Not working: the sound engine couldn't start"); return false; }
    smp_engine_free(P->e);
    P->rate = rate;
    P->e = smp_engine_new(rate, (int)P->maxf);
    P->s = s;
    if (!P->e) { free(t); smp_synth_free(old); snprintf(P->status, sizeof P->status, "Not working: the sound engine couldn't start"); return false; }
    if (t) apply_text(P, t);             /* text the players wrote (or were given) always reads */
    free(t);
    smp_synth_free(old);                 /* after the new one opened the same font, so it isn't loaded twice */
    smp_synth_set_offline(P->s, P->offline);
  } else {                               /* nothing processes now: voices from before deactivate end */
    int ch;
    smp_engine_reset(P->e, 1);
    /* All Sound Off and Reset All Controllers, queued for the first block. Not smp_synth_reset: that also lets
       the state's sf_used presets go, which a song about to start still wants loaded. */
    for (ch = 0; ch < 16; ch++) { midi3(P->s, 0xB0 | ch, 120, 0, 0); midi3(P->s, 0xB0 | ch, 121, 0, 0); }
  }
  P->since = 0;
  return true;
}

static bool p_start(const clap_plugin_t *p) { (void)p; return true; }
static void p_stop(const clap_plugin_t *p) { (void)p; }
static void p_reset(const clap_plugin_t *p) {   /* audio thread: every voice ends (a 5 ms fade), bend to center */
  Plug *P = p->plugin_data;
  if (P->e) smp_engine_reset(P->e, 0);
  if (P->s) smp_synth_reset(P->s);       /* and the SoundFont player's controllers go back to their defaults */
}

/* ---- events ---- */
static int vel127(double v) { int k = (int)floor(v * 127.0 + 0.5); return k < 0 ? 0 : k > 127 ? 127 : k; }

static void all_off(SmpEngine *e, int at) { int k; for (k = 0; k < 128; k++) smp_engine_note_off(e, k, at); }

static void midi3(SmpSynth *s, int st, int d1, int d2, int at) {
  uint8_t m[3]; m[0] = (uint8_t)st; m[1] = (uint8_t)(d1 & 127); m[2] = (uint8_t)(d2 & 127);
  smp_synth_midi(s, m, 3, at);
}

/* a parameter value: the sampler's ids to the engine, 31 to 34 to the SoundFont player */
static void set_value(Plug *P, int id, double v) {
  if (id >= 0 && id < SMP_NPARAMS) smp_engine_set_param(P->e, id, v);
  else if (id >= SMP_NPARAMS && id < SMP_NALLPARAMS) smp_synth_set_param(P->s, id, v);
}

/* one event for the SoundFont player at offset `at` in its next block: CLAP notes become MIDI on their channel */
static void synth_event(Plug *P, const clap_event_header_t *h, int at) {
  SmpSynth *s = P->s;
  switch (h->type) {
    case CLAP_EVENT_NOTE_ON: {
      const clap_event_note_t *n = (const clap_event_note_t *)h;
      int ch = n->channel >= 0 && n->channel < 16 ? n->channel : 0, v = vel127(n->velocity);
      if (n->key >= 0 && n->key < 128) midi3(s, 0x90 | ch, n->key, v ? v : 1, at);   /* velocity 0 would be an off */
      break;
    }
    case CLAP_EVENT_NOTE_OFF: case CLAP_EVENT_NOTE_CHOKE: {
      const clap_event_note_t *n = (const clap_event_note_t *)h;
      int one = n->channel >= 0 && n->channel < 16, lo = one ? n->channel : 0, hi = one ? n->channel : 15, ch;
      if (n->key >= 0 && n->key < 128) { midi3(s, 0x80 | lo, n->key, 0, at); break; }
      /* every key: All Notes Off, or All Sound Off for a choke */
      for (ch = lo; ch <= hi; ch++) midi3(s, 0xB0 | ch, h->type == CLAP_EVENT_NOTE_CHOKE ? 120 : 123, 0, at);
      break;
    }
    case CLAP_EVENT_MIDI:
      smp_synth_midi(s, ((const clap_event_midi_t *)h)->data, 3, at);
      break;
    case CLAP_EVENT_MIDI_SYSEX: {
      const clap_event_midi_sysex_t *x = (const clap_event_midi_sysex_t *)h;
      if (x->buffer && x->size) smp_synth_midi(s, x->buffer, (int)x->size, at);
      break;
    }
  }
}

/* one event at offset `at` in the sub-block that follows; parameter values apply at once */
static void event(Plug *P, const clap_event_header_t *h, int at) {
  SmpEngine *e = P->e;
  if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
  if (h->type == CLAP_EVENT_PARAM_VALUE) {
    const clap_event_param_value_t *v = (const clap_event_param_value_t *)h;
    set_value(P, (int)v->param_id, v->value);
    return;
  }
  if (P->plays) { synth_event(P, h, at); return; }
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
  }
}

/* Parameters the window changed, to the host as a gesture each (begin, the value, end), so REAPER records
   automation and its own controls follow. Audio thread (process) or main thread (flush); no allocation. */
static void emit_gui_params(Plug *P, const clap_output_events_t *out) {
  int id;
  if (!out || !P->e || !P->s || !InterlockedExchange(&P->gui_any, 0)) return;
  for (id = 0; id < SMP_NALLPARAMS; id++) {
    clap_event_param_gesture_t g; clap_event_param_value_t v;
    if (!InterlockedExchange(&P->gui_pending[id], 0)) continue;
    memset(&g, 0, sizeof g); memset(&v, 0, sizeof v);
    g.header.size = sizeof g; g.header.space_id = CLAP_CORE_EVENT_SPACE_ID; g.header.type = CLAP_EVENT_PARAM_GESTURE_BEGIN;
    g.param_id = (clap_id)id;
    out->try_push(out, &g.header);
    v.header.size = sizeof v; v.header.space_id = CLAP_CORE_EVENT_SPACE_ID; v.header.type = CLAP_EVENT_PARAM_VALUE;
    v.param_id = (clap_id)id; v.note_id = -1; v.port_index = -1; v.channel = -1; v.key = -1;
    v.value = smp_plug_get_param(P, id);
    out->try_push(out, &v.header);
    g.header.type = CLAP_EVENT_PARAM_GESTURE_END;
    out->try_push(out, &g.header);
  }
}

/* zeroes every output port from `from` on, as far as the host gave channels */
static void zero_ports(const clap_process_t *pr, uint32_t from, uint32_t n) {
  uint32_t i, c;
  for (i = from; i < pr->audio_outputs_count; i++) {
    const clap_audio_buffer_t *b = &pr->audio_outputs[i];
    if (!b->data32) continue;
    for (c = 0; c < b->channel_count; c++) if (b->data32[c]) memset(b->data32[c], 0, sizeof(float) * n);
  }
}

static void synth_run(Plug *P, float *const *outs, uint32_t a, uint32_t b) {
  float *o[2 * NOUTS]; int i;
  for (i = 0; i < 2 * NOUTS; i++) o[i] = outs[i] ? outs[i] + a : NULL;
  smp_synth_process(P->s, o, (int)(b - a));
}

/* The SoundFont player's block. Each stereo port the host gave is an output. One it didn't give (fewer ports, a
   mono port, NULL buffers) plays into output 1, and when output 1 itself isn't a stereo pair the player writes a
   scratch pair, folded to mono for a mono main port. MIDI is queued on its frame; the block is split only at
   parameter changes. */
static void process_synth(Plug *P, const clap_process_t *pr) {
  uint32_t n = pr->frames_count, ne = pr->in_events ? pr->in_events->size(pr->in_events) : 0, i, pos = 0;
  float *outs[2 * NOUTS]; float *mono = NULL;
  memset(outs, 0, sizeof outs);
  zero_ports(pr, 0, n);
  for (i = 0; i < pr->audio_outputs_count && i < NOUTS; i++) {
    const clap_audio_buffer_t *b = &pr->audio_outputs[i];
    if (b->data32 && b->channel_count >= 2 && b->data32[0] && b->data32[1]) { outs[2 * i] = b->data32[0]; outs[2 * i + 1] = b->data32[1]; }
  }
  if (!outs[0]) {
    const clap_audio_buffer_t *b = pr->audio_outputs_count ? &pr->audio_outputs[0] : NULL;
    if (n > P->maxf) { smp_synth_skip(P->s, (int)n); return; }   /* a host past its own block limit */
    outs[0] = P->scratch + P->maxf; outs[1] = P->scratch;
    if (b && b->data32 && b->channel_count == 1) mono = b->data32[0];
  }
  for (i = 0; i < ne; i++) {
    const clap_event_header_t *h = pr->in_events->get(pr->in_events, i);
    uint32_t t = h->time >= n ? (n ? n - 1 : 0) : h->time;   /* a time past the block: its last frame */
    if (t < pos) t = pos;
    if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE && t > pos) {
      synth_run(P, outs, pos, t); pos = t;
    }
    event(P, h, (int)(t - pos));
  }
  if (n > pos) synth_run(P, outs, pos, n);
  else smp_synth_process(P->s, outs, 0);              /* clears events queued at the very end */
  if (mono) for (i = 0; i < n; i++) mono[i] = 0.5f * (outs[0][i] + outs[1][i]);
}

static clap_process_status p_process(const clap_plugin_t *p, const clap_process_t *pr) {
  Plug *P = p->plugin_data;
  uint32_t n = pr->frames_count, ne = pr->in_events ? pr->in_events->size(pr->in_events) : 0, i, pos = 0;
  const clap_audio_buffer_t *ob;
  float *l, *r;
  int plays = P->plays;
  if (P->e && P->s && InterlockedExchange(&P->silence, 0)) {   /* plays changed: the other player goes quiet */
    if (plays) smp_engine_reset(P->e, 0); else smp_synth_reset(P->s);
  }
  if (plays && P->e && P->s) {
    /* the sampler runs on silently, so its fades end and its replaced samples can be freed */
    if (n <= P->maxf) smp_engine_process(P->e, P->scratch, P->scratch + P->maxf, (int)n);
    process_synth(P, pr);
    goto done;
  }
  if (!pr->audio_outputs_count) return CLAP_PROCESS_CONTINUE;
  ob = &pr->audio_outputs[0];
  if (!ob->data32 || !ob->channel_count) return CLAP_PROCESS_ERROR;
  l = ob->data32[0];
  r = ob->channel_count > 1 ? ob->data32[1] : P->scratch;
  if (!P->e || (ob->channel_count < 2 && n > P->maxf)) {
    for (i = 0; i < ob->channel_count; i++) if (ob->data32[i]) memset(ob->data32[i], 0, sizeof(float) * n);
    zero_ports(pr, 1, n);
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
  for (i = 2; i < ob->channel_count; i++) if (ob->data32[i]) memset(ob->data32[i], 0, sizeof(float) * n);
  zero_ports(pr, 1, n);                  /* the sampler writes output 1 only */
  if (P->s) smp_synth_skip(P->s, (int)n);   /* the SoundFont player still takes its font, so replaced ones close */
done:
  emit_gui_params(P, pr->out_events);
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
  /* a replaced font closes once the audio thread has let go of it: keep asking until it has */
  if (P->s && smp_synth_collect(P->s) > 0 && P->collect_left <= 0) InterlockedExchange(&P->collect_left, COLLECT_ROUNDS);
}

/* ---- note ports ---- */
static uint32_t np_count(const clap_plugin_t *p, bool in) { (void)p; return in ? 1 : 0; }
static bool np_get(const clap_plugin_t *p, uint32_t i, bool in, clap_note_port_info_t *info) {
  (void)p;
  if (!in || i) return false;
  info->id = 0;
  info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
  info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;   /* the SoundFont player wants programs and controllers */
  snprintf(info->name, sizeof info->name, "Notes");
  return true;
}
static const clap_plugin_note_ports_t note_ports = {np_count, np_get};

/* ---- audio ports ---- */
static uint32_t ap_count(const clap_plugin_t *p, bool in) { (void)p; return in ? 0 : NOUTS; }
static bool ap_get(const clap_plugin_t *p, uint32_t i, bool in, clap_audio_port_info_t *info) {
  (void)p;
  if (in || i >= NOUTS) return false;
  info->id = i;
  snprintf(info->name, sizeof info->name, "Output %u", i + 1);
  info->flags = i ? 0 : CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
static const clap_plugin_audio_ports_t audio_ports = {ap_count, ap_get};

/* ---- parameters (ids are the engine's) ---- */
const char *const smp_param_label[SMP_NALLPARAMS] = {
  "Root note", "Transpose", "Fine tune", "Volume", "Pan", "Attack", "Decay", "Sustain", "Release",
  "Velocity sensitivity", "Minimum volume", "Mode", "Lowest note", "Highest note", "Pitch at lowest note",
  "Lowest velocity", "Highest velocity", "Obey note off", "Loop", "Loop start", "Start", "End", "Bend range",
  "Pitch at highest note", "Reverse", "Keep length", "Voices", "Probability", "Speed", "Glide", "Glide mode",
  "SoundFont volume", "SoundFont voices", "Reverb and chorus", "SoundFont transpose"};

static uint32_t pa_count(const clap_plugin_t *p) { (void)p; return SMP_NALLPARAMS; }
static bool pa_info(const clap_plugin_t *p, uint32_t i, clap_param_info_t *info) {
  const SmpParamInfo *pi = smp_param_info((int)i);
  (void)p;
  if (!pi || i >= SMP_NALLPARAMS) return false;
  memset(info, 0, sizeof *info);
  info->id = i;
  info->flags = CLAP_PARAM_IS_AUTOMATABLE;
  switch (pi->kind) {                    /* whole steps: notes, semitones, cents, choices, switches, counts */
    case SMP_K_NOTE: case SMP_K_SEMI: case SMP_K_CENT: case SMP_K_CHOICE: case SMP_K_BOOL: case SMP_K_COUNT:
      info->flags |= CLAP_PARAM_IS_STEPPED;
  }
  if (pi->kind == SMP_K_CHOICE || pi->kind == SMP_K_BOOL) info->flags |= CLAP_PARAM_IS_ENUM;
  snprintf(info->name, sizeof info->name, "%s", smp_param_label[i]);
  info->min_value = pi->lo; info->max_value = pi->hi; info->default_value = pi->def;
  return true;
}
static bool pa_value(const clap_plugin_t *p, clap_id id, double *v) {
  Plug *P = p->plugin_data;
  if (id >= SMP_NALLPARAMS) return false;
  *v = smp_plug_get_param(P, (int)id);
  return true;
}
static bool pa_text(const clap_plugin_t *p, clap_id id, double v, char *out, uint32_t cap) {
  (void)p;
  if (id >= SMP_NALLPARAMS || !cap) return false;
  smp_value_text((int)id, v, out, (int)cap);
  return true;
}
static bool pa_parse(const clap_plugin_t *p, clap_id id, const char *s, double *v) {
  (void)p;
  return id < SMP_NALLPARAMS && smp_value_parse((int)id, s, v);
}
static void pa_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) {
  Plug *P = p->plugin_data; uint32_t i, n = in ? in->size(in) : 0;
  if (!P->e || !P->s) return;
  for (i = 0; i < n; i++) {
    const clap_event_header_t *h = in->get(in, i);
    if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) event(P, h, 0);
  }
  emit_gui_params(P, out);
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

/* ---- render: offline, the SoundFont player waits for its samples, so a render to file is exact ---- */
static bool rn_hard(const clap_plugin_t *p) { (void)p; return false; }
static bool rn_set(const clap_plugin_t *p, clap_plugin_render_mode mode) {
  Plug *P = p->plugin_data;
  InterlockedExchange(&P->offline, mode == CLAP_RENDER_OFFLINE);
  smp_synth_set_offline(P->s, mode == CLAP_RENDER_OFFLINE);
  return true;
}
static const clap_plugin_render_t render = {rn_hard, rn_set};

static const void *p_ext(const clap_plugin_t *p, const char *id) {
  (void)p;
  if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &note_ports;
  if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audio_ports;
  if (!strcmp(id, CLAP_EXT_PARAMS)) return &params;
  if (!strcmp(id, CLAP_EXT_STATE)) return &state;
  if (!strcmp(id, CLAP_EXT_LATENCY)) return &latency;
  if (!strcmp(id, CLAP_EXT_RENDER)) return &render;
  if (!strcmp(id, CLAP_EXT_GUI)) return &smp_gui_ext;
  return NULL;
}

/* ---- the status, for the window (and the test host) ---- */
static void sf_status(Plug *P, char *out, int cap);

SMP_API int smp_plugin_status(const clap_plugin_t *p, char *out, int cap) {
  Plug *P = plug_of(p);
  if (!P || !out || cap <= 0) return 0;
  if (P->plays && !P->bad && P->e && P->s) sf_status(P, out, cap);
  else snprintf(out, (size_t)cap, "%s", P->status);
  return 1;
}

/* ---- what the window does (plugin_int.h); main thread ---- */
static void mark_dirty(Plug *P) { if (P->host_state && P->host_state->mark_dirty) P->host_state->mark_dirty(P->host); }

static void tell_host(Plug *P, int id) {
  InterlockedExchange(&P->gui_pending[id], 1);
  InterlockedExchange(&P->gui_any, 1);
  if (P->host_params && P->host_params->request_flush) P->host_params->request_flush(P->host);
}

void smp_plug_set_param(Plug *P, int id, double v) {
  if (!P->e || !P->s || id < 0 || id >= SMP_NALLPARAMS) return;
  set_value(P, id, v);
  tell_host(P, id);
  mark_dirty(P);
}

double smp_plug_get_param(Plug *P, int id) {
  if (id < 0 || id >= SMP_NALLPARAMS) return 0;
  if (id < SMP_NPARAMS) return P->e ? smp_engine_get_param(P->e, id) : smp_param_info(id)->def;
  return P->s ? smp_synth_get_param(P->s, id) : smp_param_info(id)->def;
}

int smp_plug_load(Plug *P, const char *path, char *err, int cap) {
  SmpSample *s; char *t;
  if (!P->e) { snprintf(err, (size_t)cap, "the sound engine isn't working"); return 0; }
  s = smp_sample_load(path, err, cap);
  if (!s) return 0;
  smp_engine_set_sample(P->e, s);        /* the root follows the sample's */
  P->missing = 0;
  t = engine_text(P->e);
  if (t) { free(P->text); P->text = t; }
  snprintf(P->status, sizeof P->status, "Ready: %s", s->name);
  tell_host(P, SMP_P_ROOT);
  mark_dirty(P);
  ask_collect(P);
  return 1;
}

int smp_plug_sample_path(Plug *P, char *path, int cap) {
  const SmpSample *s = P->e ? smp_engine_sample(P->e) : NULL;
  char line[1200];
  if (s) { snprintf(path, (size_t)cap, "%s", s->path); return 1; }
  if (P->missing && P->text && sample_line(P->text, line, sizeof line, path, cap) == 2) return 2;
  if (cap > 0) path[0] = 0;
  return 0;
}

void smp_plug_sample_text(Plug *P, char *out, int cap) {
  static const char *const from[] = {"from the file name", "detected", "by default"};
  const SmpSample *s = P->e ? smp_engine_sample(P->e) : NULL;
  char note[16], path[1100];
  if (s) {
    smp_note_name(s->root, note, sizeof note);
    snprintf(out, (size_t)cap, "%s, %.2f s, root %s %s", s->name, s->rate > 0 ? (double)s->frames / s->rate : 0.0,
             note, from[s->root_from < 0 || s->root_from > 2 ? 2 : s->root_from]);
  } else if (smp_plug_sample_path(P, path, sizeof path) == 2) snprintf(out, (size_t)cap, "Missing: %s", path);
  else snprintf(out, (size_t)cap, "No sample loaded");
}

void smp_plug_status_text(Plug *P, char *out, int cap) {
  int ready = 0, total = 0; char why[600] = "";
  if (P->plays && !P->bad && P->e && P->s) { sf_status(P, out, cap); return; }
  if (!P->e || strncmp(P->status, "Ready", 5)) { snprintf(out, (size_t)cap, "%s", P->status); return; }
  if (smp_engine_get_param(P->e, SMP_P_KEEP_LENGTH) >= 0.5 && !smp_engine_render_status(P->e, &ready, &total, why, sizeof why)) {
    if (why[0]) snprintf(out, (size_t)cap, "%s", why);
    else snprintf(out, (size_t)cap, "Keep length: %d of %d copies ready", ready, total);
    return;
  }
  snprintf(out, (size_t)cap, "%s", P->status);
}

/* ---- the SoundFont player, for the window (plugin_int.h); main thread ---- */
int smp_plug_plays(Plug *P) { return P->plays ? 1 : 0; }

void smp_plug_set_plays(Plug *P, int which) {
  which = which ? 1 : 0;
  if (P->plays == which) return;
  set_plays(P, which);
  mark_dirty(P);
}

SmpSynth *smp_plug_synth(Plug *P) { return P->s; }

void smp_plug_font_text(Plug *P, char *out, int cap) {
  SmpFont *f = P->s ? smp_synth_font(P->s) : NULL;
  if (P->sf_missing) {
    char path[1100] = "", line[1200];
    if (P->sf_line) font_line(P->sf_line, line, sizeof line, path, sizeof path);
    snprintf(out, (size_t)cap, "Missing: %s", path);
  } else if (!f) snprintf(out, (size_t)cap, "No SoundFont loaded");
  else {
    const char *name = f->path, *q;
    int n = smp_sf_preset_count(f);
    for (q = f->path; *q; q++) if (*q == '\\' || *q == '/') name = q + 1;
    snprintf(out, (size_t)cap, "%s, %d %s", name, n, n == 1 ? "preset" : "presets");
  }
}

static void sf_status(Plug *P, char *out, int cap) {
  char t[1200]; int n;
  if (P->sf_missing) { snprintf(out, (size_t)cap, "%s", P->sf_err); return; }
  if (!smp_synth_font(P->s)) { snprintf(out, (size_t)cap, "No SoundFont loaded"); return; }
  n = smp_synth_loading(P->s);
  if (n > 0) { snprintf(out, (size_t)cap, "Loading samples for %d %s", n, n == 1 ? "preset" : "presets"); return; }
  smp_plug_font_text(P, t, sizeof t);
  snprintf(out, (size_t)cap, "Ready: %s", t);
}

int smp_plug_load_font(Plug *P, const char *path, char *err, int cap) {
  if (!P->s) { snprintf(err, (size_t)cap, "the sound engine isn't working"); return 0; }
  if (!smp_synth_load(P->s, path, err, cap)) return 0;
  free(P->sf_line); free(P->sf_used); P->sf_line = P->sf_used = NULL; P->sf_missing = 0;
  set_plays(P, 1);
  mark_dirty(P);
  ask_collect(P);
  return 1;
}

/* the first word of a state line, or the first two for the per-channel settings: the line it replaces has the
   same. 1 when it is "sf_channel_preset <ch> song": the pin goes, and the song chooses again. */
static int sf_key(const char *line, char *key, int cap) {
  char a[64] = "", b[64] = "", c[64] = "";
  int n = sscanf(line, " %63s %63s %63s", a, b, c), two = !strcmp(a, "sf_channel_preset") || !strcmp(a, "sf_output");
  if (two && n >= 2) snprintf(key, (size_t)cap, "%s %s", a, b);
  else snprintf(key, (size_t)cap, "%s", a);
  return two && n == 3 && !strcmp(a, "sf_channel_preset") && !strcmp(c, "song");
}

void smp_plug_sf_set(Plug *P, const char *line) {
  char key[140], k2[140], err[700];
  char *cur, *out, *o; const char *p;
  int n, drop;
  if (!P->s || !line) return;
  drop = sf_key(line, key, sizeof key);
  n = smp_synth_script_write(P->s, NULL, 0);
  cur = malloc((size_t)n + 1);
  out = malloc((size_t)n + strlen(line) + 2);
  if (!cur || !out) { free(cur); free(out); return; }
  smp_synth_script_write(P->s, cur, n + 1);
  /* the player's own lines with this one in place of the line it replaces; everything else stays */
  for (p = cur, o = out; *p; ) {
    const char *eol = strchr(p, '\n'), *end = eol ? eol + 1 : p + strlen(p);
    char one[1200];
    snprintf(one, sizeof one, "%.*s", (int)(end - p), p);
    sf_key(one, k2, sizeof k2);
    if (strcmp(k2, key)) { memcpy(o, p, (size_t)(end - p)); o += end - p; }
    p = end;
  }
  if (!drop) { strcpy(o, line); o += strlen(line); *o++ = '\n'; }
  *o = 0;
  if (!smp_synth_script_apply(P->s, out, err, sizeof err)) mark_dirty(P);
  free(cur); free(out);
}

int smp_plug_sf_channel_now(Plug *P, int ch, int *bank, int *program) {
  return P->s ? smp_synth_channel_preset(P->s, ch, bank, program) : 0;
}

/* ---- test hooks (tests\host.c): make a real-time plugin render exactly like smp_engine_render_test ---- */
SMP_API int smp_test_plugin_wait(const clap_plugin_t *p, int timeout_ms) {   /* keep-length copies and start heads */
  Plug *P = plug_of(p);
  if (!P || !P->e) return 1;
  if (smp_engine_wait_renders(P->e, timeout_ms)) return 1;
  smp_worker_wait_idle(P->e);
  if (P->s && smp_synth_wait_loaded(P->s, timeout_ms)) return 1;   /* the state's sf_used presets */
  return 0;
}
SMP_API void smp_test_plugin_offline(const clap_plugin_t *p, int on) {   /* note-ons past the head wait for the streamer */
  Plug *P = plug_of(p);
  if (P && P->e) P->e->offline = on;
}
SMP_API int smp_test_plugin_voices(const clap_plugin_t *p) {   /* sounding voices */
  Plug *P = plug_of(p);
  if (P && P->plays && P->s) return smp_synth_voices(P->s, NULL);
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
