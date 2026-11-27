/* plugin.c: Samplosis as a CLAP instrument. Two players: the sampler (the engine) and the SoundFont player (the
   synth, sfsynth.c); `plays` says which one hears the notes, and both keep their settings. One note input (MIDI
   preferred, CLAP notes too: they reach the SoundFont player as MIDI on their channel), 16 stereo outputs (the
   sampler writes the first; the SoundFont player routes channels with sf_output), every parameter of both players
   automatable, and the state as text: `plays sampler|soundfont`, then the sampler's text (script.c), then the
   SoundFont player's lines.

   Threads. The engine is made in init (48 kHz) so state loaded before activate has somewhere to go; activate at
   another rate makes a new engine from the old one's text. State load, activate, deactivate, on_main_thread and
   destroy are on the main thread; a state's sample is loaded there, synchronously (from the cache, or decoded once),
   so a project opens with its sample. The window's loads (Browse, the File combo box, a drop) decode on a worker
   thread instead and are swapped in on the main thread when ready (see "the window's sample loads" below).
   Process runs on the audio thread: it splits the block at each event, so parameter changes and notes land on
   their exact frame, and touches nothing but the players. Replaced samples are freed by smp_multi_collect on the
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
#include "decode.h"

#define DEFAULT_RATE 48000.0
#define COLLECT_ROUNDS 8
#define NOUTS 16                 /* stereo output ports */
#define SF_BLOCK 4096            /* the SoundFont player's piece size; it splits longer blocks itself */
#define SWITCH_MS 15.0           /* after a switch of players, the old one plays out its fade this long (voices fade
                                    over 5 ms, the SoundFont player's effect tails over 10 ms) */

/* Plug (plugin_int.h) is shared with the window (gui.c). */

static const char *const features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SAMPLER,
                                       CLAP_PLUGIN_FEATURE_STEREO, NULL};
static const clap_plugin_descriptor_t desc = {
  CLAP_VERSION_INIT, "com.audioskit.samplosis", "Samplosis", "audioskit", "", "", "", SMP_VERSION,
  "A sampler you can use with a screen reader: samples across the keyboard, and SoundFonts", features};

static Plug *plug_of(const clap_plugin_t *p) { return p && p->desc == &desc ? (Plug *)p->plugin_data : NULL; }
static void loader_cancel(Plug *P);      /* the window's background loads (below) */
static int take_font(Plug *P, SmpFont *f, const char *path, char *err, int cap);
static void loader_stop(Plug *P);

/* ---- the instrument as text ---- */
static void keep_text(Plug *P, const char *text);
static char *engine_text(const SmpMulti *m) {
  int n = smp_multi_script_write(m, NULL, 0);
  char *t = malloc((size_t)n + 1);
  if (t) smp_multi_script_write(m, t, n + 1);
  return t;
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

/* "Ready: x.wav", and what decoded it when that wasn't built in: "Ready: x.m4a (decoded by Windows)"; with several
   samples "Ready: 3 samples" */
static void ready_status(Plug *P) {
  int n = smp_multi_count(P->e), sel = smp_multi_selected(P->e);
  const SmpSample *s = smp_multi_sample(P->e, sel);
  if (n > 1) snprintf(P->status, sizeof P->status, "Ready: %d samples", n);
  else if (s) {
    const char *by = s->by == SMP_DEC_WINDOWS ? " (decoded by Windows)" : s->by == SMP_DEC_FFMPEG ? " (decoded by FFmpeg)" : "";
    snprintf(P->status, sizeof P->status, "Ready: %s%s", s->name, by);
  } else snprintf(P->status, sizeof P->status, "No sample loaded");
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

/* the status for samples that didn't load: "Missing: <path>" or "Can't load <path>: why", and how many more */
static void missing_status(Plug *P, int nmiss) {
  int i, n = smp_multi_count(P->e), at;
  char path[1100];
  for (i = 0; i < n; i++) if (smp_multi_path(P->e, i, path, sizeof path) == 2) break;
  if (i == n) return;
  if (!file_exists(path)) at = snprintf(P->status, sizeof P->status, "Missing: %s", path);
  else at = snprintf(P->status, sizeof P->status, "Can't load %s: %s", path, smp_multi_why(P->e, i));
  if (nmiss > 1 && at > 0 && at < (int)sizeof P->status)
    snprintf(P->status + at, sizeof P->status - (size_t)at, " (and %d more)", nmiss - 1);
}

/* Applies a state text to the sampler. 1 when it's taken (samples may be missing: they stay in the list, silent, and
   are written back), 0 when it doesn't read (nothing changes). Main thread. */
static int apply_sampler(Plug *P, const char *text) {
  char err[700] = ""; int nmiss = 0;
  if (!P->e) { snprintf(P->status, sizeof P->status, "Not working: the sound engine couldn't start"); return 0; }
  if (smp_multi_script_apply(P->e, text, &nmiss, err, sizeof err)) {
    snprintf(P->status, sizeof P->status, "The saved instrument doesn't read, so nothing changed: %s", err);
    return 0;
  }
  P->missing = nmiss;
  keep_text(P, text);
  if (nmiss) missing_status(P, nmiss); else ready_status(P);
  if (!P->active) smp_multi_collect(P->e, 1);
  ask_collect(P);
  return 1;
}

/* The sampler's part of the state: its samples (missing ones as they came) and parameters */
static char *sampler_text(Plug *P) {
  if (!P->e) return _strdup(P->text ? P->text : "# Samplosis 0.1.0\n");
  return engine_text(P->e);
}

/* every line of text that starts with word (and a space), joined by newlines; NULL when none */
static char *lines_of(const char *text, const char *word) {
  size_t k = strlen(word), n = 0;
  const char *q;
  char *out = NULL;
  for (q = text; q && *q; q = strchr(q, '\n') ? strchr(q, '\n') + 1 : NULL) {
    const char *e = strchr(q, '\n'), *a = q;
    size_t L;
    while (*a == ' ' || *a == '\t') a++;
    if (strncmp(a, word, k) || (a[k] != ' ' && a[k] != '\t')) continue;
    L = e ? (size_t)(e - a) : strlen(a);
    while (L && (a[L - 1] == '\r' || a[L - 1] == ' ')) L--;
    {
      char *m = realloc(out, n + L + 2);
      if (!m) { free(out); return NULL; }
      out = m;
      if (n) out[n++] = '\n';
      memcpy(out + n, a, L); n += L; out[n] = 0;
    }
  }
  return out;
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
  if (P->sf_knobs) {                       /* the sf_knob lines go with sf_used, where the player writes them */
    size_t kl = strlen(P->sf_knobs);
    char *both = malloc(ul + kl + 2);
    if (!both) return t;
    if (ul) { memcpy(both, P->sf_used, ul); both[ul] = '\n'; memcpy(both + ul + 1, P->sf_knobs, kl + 1); ul += kl + 1; }
    else { memcpy(both, P->sf_knobs, kl + 1); ul = kl; }
    out = malloc((size_t)n + fl + ul + 3);
    if (!out) { free(both); return t; }
    for (at = t; at; at = strchr(at, '\n') ? strchr(at, '\n') + 1 : NULL)
      if (!strncmp(at, first, k) && at[k] == ' ') break;
    head = at ? (size_t)(at - t) : (size_t)n;
    memcpy(out, P->sf_line, fl); out[fl] = '\n';
    memcpy(out + fl + 1, t, head);
    memcpy(out + fl + 1 + head, both, ul); out[fl + 1 + head + ul] = '\n';
    strcpy(out + fl + 1 + head + ul + 1, t + head);
    free(both); free(t);
    return out;
  }
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
  P->told_until = 0;                       /* a whole new state: an old window load's failure no longer applies */
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
  free(P->sf_line); free(P->sf_used); free(P->sf_knobs); P->sf_line = P->sf_used = P->sf_knobs = NULL; P->sf_missing = 0;
  if (!smp_synth_script_apply(P->s, text, err, sizeof err)) {
    if (!fk) smp_synth_unload(P->s);      /* a state without a font has none, as one without a sample */
  } else {                                /* the font didn't load: silence, and everything else */
    smp_synth_unload(P->s);               /* first, so sf_used doesn't ask for the old font's presets */
    smp_synth_script_apply(P->s, rest, NULL, 0);
    P->sf_missing = 1;
    P->sf_line = _strdup(line);
    if (word_line(text, "sf_used", used, sizeof used, NULL, 0, NULL)) P->sf_used = _strdup(used);
    P->sf_knobs = lines_of(text, "sf_knob");
    if (!file_exists(path)) snprintf(P->sf_err, sizeof P->sf_err, "Missing: %s", path);
    else snprintf(P->sf_err, sizeof P->sf_err, "Can't load the SoundFont: %s", err);
  }
  free(rest);
  set_plays(P, plays);
  P->bad = 0;
  if (!P->active) smp_synth_collect_now(P->s);   /* nothing processes: replaced fonts close now */
  ask_collect(P);
  return 1;
}

/* ---- life cycle ---- */
static bool p_init(const clap_plugin_t *p) {
  Plug *P = p->plugin_data;
  P->rate = DEFAULT_RATE;
  if (P->host && P->host->get_extension) {
    P->host_params = P->host->get_extension(P->host, CLAP_EXT_PARAMS);
    P->host_state = P->host->get_extension(P->host, CLAP_EXT_STATE);
  }
  P->e = smp_multi_new(P->rate, 4096);
  P->s = smp_synth_new(P->rate, SF_BLOCK);
  snprintf(P->status, sizeof P->status, P->e && P->s ? "No sample loaded" : "Not working: the sound engine couldn't start");
  return P->e != NULL && P->s != NULL;
}

static void p_deactivate(const clap_plugin_t *p) {
  Plug *P = p->plugin_data;
  P->active = 0;
  if (P->e) smp_multi_collect(P->e, 1);      /* nothing processes now */
  if (P->s) smp_synth_collect_now(P->s);  /* nothing processes now: every replaced font can close */
}

static void p_destroy(const clap_plugin_t *p) {
  Plug *P = p->plugin_data;
  smp_gui_close(P);                      /* a host that didn't destroy the window first */
  loader_stop(P);                        /* a window load still decoding is cancelled */
  smp_multi_free(P->e);
  smp_synth_free(P->s);                  /* here, on the main thread, never at DLL unload (it may join the loader) */
  free(P->text); free(P->scratch); free(P->fade_buf); free(P->sf_line); free(P->sf_used); free(P->sf_knobs);
  free(P);
}

static bool p_activate(const clap_plugin_t *p, double rate, uint32_t minf, uint32_t maxf) {
  Plug *P = p->plugin_data;
  (void)minf;
  if (rate <= 0) return false;
  free(P->scratch); free(P->fade_buf);
  P->maxf = maxf ? maxf : 4096;
  P->scratch = malloc(sizeof(float) * 2 * P->maxf);   /* also a stereo pair when the host's main output isn't one */
  P->fade_buf = malloc(sizeof(float) * 2 * P->maxf);
  if (!P->scratch || !P->fade_buf) { free(P->scratch); free(P->fade_buf); P->scratch = P->fade_buf = NULL; return false; }
  P->switch_left = 0;
  if (!P->e || !P->s || rate != P->rate) {   /* new players at this rate, with the same instrument */
    char *t = state_text(P);
    SmpSynth *old = P->s, *s = smp_synth_new(rate, SF_BLOCK);
    if (!s) { free(t); snprintf(P->status, sizeof P->status, "Not working: the sound engine couldn't start"); return false; }
    smp_multi_free(P->e);
    P->rate = rate;
    P->e = smp_multi_new(rate, (int)P->maxf);
    P->s = s;
    if (!P->e) { free(t); smp_synth_free(old); snprintf(P->status, sizeof P->status, "Not working: the sound engine couldn't start"); return false; }
    if (t) apply_text(P, t);             /* text the players wrote (or were given) always reads */
    free(t);
    smp_synth_free(old);                 /* after the new one opened the same font, so it isn't loaded twice */
    smp_synth_set_offline(P->s, P->offline);
  } else {                               /* nothing processes now: voices from before deactivate end at once */
    smp_multi_reset(P->e, 1);
    smp_synth_flush(P->s, 1);            /* and no reverb tail; the state's sf_used presets stay loaded */
  }
  P->since = 0;
  P->active = 1;
  return true;
}

static bool p_start(const clap_plugin_t *p) { (void)p; return true; }
static void p_stop(const clap_plugin_t *p) { (void)p; }
static void p_reset(const clap_plugin_t *p) {   /* audio thread: every voice ends (a 5 ms fade), bend to center */
  Plug *P = p->plugin_data;
  if (P->e) smp_multi_reset(P->e, 0);
  /* REAPER resets on stop, seek and loop: every SoundFont voice and effect tail ends and the controllers go back
     to their defaults, but the state's sf_used presets stay loaded for the song's first notes */
  if (P->s) smp_synth_flush(P->s, 0);
}

/* ---- events ---- */
static int vel127(double v) { int k = (int)floor(v * 127.0 + 0.5); return k < 0 ? 0 : k > 127 ? 127 : k; }

static void all_off(SmpMulti *e, int at) { int k; for (k = 0; k < 128; k++) smp_multi_note_off(e, k, at); }

static void midi3(SmpSynth *s, int st, int d1, int d2, int at) {
  uint8_t m[3]; m[0] = (uint8_t)st; m[1] = (uint8_t)(d1 & 127); m[2] = (uint8_t)(d2 & 127);
  smp_synth_midi(s, m, 3, at);
}

/* a parameter value: the sampler's ids to the engine, 31 to 34 to the SoundFont player */
static void set_value(Plug *P, int id, double v) {
  if (id >= 0 && id < SMP_NPARAMS) smp_multi_set_param(P->e, id, v);
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
      /* a key on one channel, or with channel -1 (a wildcard) on every channel */
      if (n->key >= 0 && n->key < 128) { for (ch = lo; ch <= hi; ch++) midi3(s, 0x80 | ch, n->key, 0, at); break; }
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

/* one event at offset `at` in the sub-block that follows, for the player `plays` (the block's snapshot);
   parameter values apply at once */
static void event(Plug *P, const clap_event_header_t *h, int at, int plays) {
  SmpMulti *e = P->e;
  if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
  if (h->type == CLAP_EVENT_PARAM_VALUE) {
    const clap_event_param_value_t *v = (const clap_event_param_value_t *)h;
    if (v->param_id < SMP_NALLPARAMS && !smp_param_per_sample((int)v->param_id)) set_value(P, (int)v->param_id, v->value);
    return;
  }
  if (plays) { synth_event(P, h, at); return; }
  switch (h->type) {
    case CLAP_EVENT_NOTE_ON: {
      const clap_event_note_t *n = (const clap_event_note_t *)h;
      if (n->key >= 0 && n->key < 128) smp_multi_note_on(e, n->key, vel127(n->velocity), at);
      break;
    }
    case CLAP_EVENT_NOTE_OFF: case CLAP_EVENT_NOTE_CHOKE: {
      const clap_event_note_t *n = (const clap_event_note_t *)h;
      if (n->key < 0) all_off(e, at);
      else if (n->key < 128) smp_multi_note_off(e, n->key, at);
      break;
    }
    case CLAP_EVENT_MIDI: {
      const uint8_t *d = ((const clap_event_midi_t *)h)->data;
      int st = d[0] & 0xF0;
      if (st == 0x90 && d[2]) smp_multi_note_on(e, d[1] & 0x7F, d[2] & 0x7F, at);
      else if (st == 0x80 || st == 0x90) smp_multi_note_off(e, d[1] & 0x7F, at);
      else if (st == 0xE0) smp_multi_bend(e, (((d[2] & 0x7F) << 7 | (d[1] & 0x7F)) - 8192) / 8192.0, at);
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
    if (!InterlockedExchange(&P->gui_pending[id], 0) || smp_param_per_sample(id)) continue;
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

/* a block that can't be played (no buffers to play into): its parameter values still apply, its notes are lost,
   and the SoundFont player still takes its font */
static void block_lost(Plug *P, const clap_process_t *pr) {
  uint32_t i, ne = pr->in_events ? pr->in_events->size(pr->in_events) : 0;
  for (i = 0; i < ne; i++) {
    const clap_event_header_t *h = pr->in_events->get(pr->in_events, i);
    if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) event(P, h, 0, 0);
  }
  if (P->s) smp_synth_skip(P->s, (int)pr->frames_count);
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
    if (n > P->maxf) { block_lost(P, pr); return; }   /* a host past its own block limit */
    outs[0] = P->scratch + P->maxf; outs[1] = P->scratch;
    if (b && b->data32 && b->channel_count == 1) mono = b->data32[0];
  }
  if (pr->transport && (pr->transport->flags & CLAP_TRANSPORT_HAS_TEMPO))
    smp_synth_set_tempo(P->s, pr->transport->tempo);   /* Decent Sampler's tempo-synced delays */
  for (i = 0; i < ne; i++) {
    const clap_event_header_t *h = pr->in_events->get(pr->in_events, i);
    uint32_t t = h->time >= n ? (n ? n - 1 : 0) : h->time;   /* a time past the block: its last frame */
    if (t < pos) t = pos;
    if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE && t > pos) {
      synth_run(P, outs, pos, t); pos = t;
    }
    event(P, h, (int)(t - pos), 1);
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
  int plays = P->plays, fading;
  if (P->e && P->s && InterlockedExchange(&P->silence, 0)) {   /* plays changed: the other player fades out */
    if (plays) smp_multi_reset(P->e, 0); else smp_synth_flush(P->s, 0);
    P->switch_left = (uint32_t)(SWITCH_MS * 0.001 * P->rate);
  }
  fading = P->switch_left > 0 && n <= P->maxf;      /* the old player's fade is heard on output 1 */
  P->switch_left = P->switch_left > n ? P->switch_left - n : 0;
  if (plays && P->e && P->s) {
    /* the sampler runs on silently, so its fades end and its replaced samples can be freed; just after a switch
       its fade is added to output 1 (when the host gave a stereo pair there) */
    const clap_audio_buffer_t *b0 = pr->audio_outputs_count ? &pr->audio_outputs[0] : NULL;
    if (n <= P->maxf) smp_multi_process(P->e, P->fade_buf, P->fade_buf + P->maxf, (int)n);
    process_synth(P, pr);
    if (fading && b0 && b0->data32 && b0->channel_count >= 2 && b0->data32[0] && b0->data32[1])
      for (i = 0; i < n; i++) { b0->data32[0][i] += P->fade_buf[i]; b0->data32[1][i] += P->fade_buf[P->maxf + i]; }
    goto done;
  }
  if (!pr->audio_outputs_count) { block_lost(P, pr); return CLAP_PROCESS_CONTINUE; }
  ob = &pr->audio_outputs[0];
  if (!ob->data32 || !ob->channel_count) { block_lost(P, pr); return CLAP_PROCESS_ERROR; }
  /* a channel the host gave as NULL is played into scratch: a missing left is thrown away, a missing right is
     folded into the left as for a mono output */
  l = ob->data32[0] ? ob->data32[0] : P->scratch + P->maxf;
  r = ob->channel_count > 1 && ob->data32[1] ? ob->data32[1] : P->scratch;
  if (!P->e || ((l != ob->data32[0] || r == P->scratch) && n > P->maxf)) {
    zero_ports(pr, 0, n);
    block_lost(P, pr);
    return CLAP_PROCESS_CONTINUE;
  }
  /* the block runs in pieces between event times, so every event lands on its frame */
  for (i = 0; i < ne; i++) {
    const clap_event_header_t *h = pr->in_events->get(pr->in_events, i);
    uint32_t t = h->time >= n ? (n ? n - 1 : 0) : h->time;   /* a time past the block: its last frame */
    if (t < pos) t = pos;
    if (t > pos) { smp_multi_process(P->e, l + pos, r + pos, (int)(t - pos)); pos = t; }
    event(P, h, 0, 0);
  }
  if (n > pos) smp_multi_process(P->e, l + pos, r + pos, (int)(n - pos));
  else smp_multi_process(P->e, l, r, 0);              /* clears events queued at the very end */
  if (r == P->scratch && l == ob->data32[0]) for (i = 0; i < n; i++) l[i] = 0.5f * (l[i] + r[i]);
  for (i = 2; i < ob->channel_count; i++) if (ob->data32[i]) memset(ob->data32[i], 0, sizeof(float) * n);
  zero_ports(pr, 1, n);                  /* the sampler writes output 1 only */
  if (P->s && fading) {                  /* just after a switch: the SoundFont player's fade, into output 1 */
    float *o[2 * NOUTS];
    memset(o, 0, sizeof o);
    o[0] = P->fade_buf; o[1] = P->fade_buf + P->maxf;
    smp_synth_process(P->s, o, (int)n);
    if (r == P->scratch)                 /* a mono output (folded above): the fade folded too */
      for (i = 0; i < n; i++) l[i] += 0.5f * (P->fade_buf[i] + P->fade_buf[P->maxf + i]);
    else for (i = 0; i < n; i++) { l[i] += P->fade_buf[i]; r[i] += P->fade_buf[P->maxf + i]; }
  } else if (P->s) smp_synth_skip(P->s, (int)n);   /* it still takes its font, so replaced ones close */
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
  if (smp_plug_poll_load(P) && P->gui) smp_gui_loaded(P);   /* a window load finished: swapped in here */
  if (P->e) smp_multi_collect(P->e, !P->active);
  if (P->collect_left > 0) InterlockedDecrement(&P->collect_left);
  /* a replaced font closes once the audio thread has let go of it: keep asking until it has */
  if (P->s && !P->active) smp_synth_collect_now(P->s);   /* nothing processes: close them now */
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
  "Velocity sensitivity", "Minimum volume", "Mode", "Lowest note", "Highest note", "Pitch at start key",
  "Lowest velocity", "Highest velocity", "Obey note off", "Loop", "Loop start", "Start", "End", "Bend range",
  "Pitch at end key", "Reverse", "Keep length", "Voices", "Probability", "Speed", "Glide", "Glide mode",
  "SoundFont volume", "SoundFont voices", "Reverb and chorus", "SoundFont transpose"};

/* the host's list: every parameter but each sample's own (smp_param_per_sample), by id; ids keep their numbers */
static uint32_t pa_count(const clap_plugin_t *p) { (void)p; return SMP_NALLPARAMS - SMP_NPERSAMPLE; }
static bool pa_info(const clap_plugin_t *p, uint32_t at, clap_param_info_t *info) {
  const SmpParamInfo *pi; uint32_t i, k = 0;
  (void)p;
  for (i = 0; i < SMP_NALLPARAMS; i++) if (!smp_param_per_sample((int)i) && k++ == at) break;
  pi = smp_param_info((int)i);
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
  if (id >= SMP_NALLPARAMS || smp_param_per_sample((int)id)) return false;
  *v = smp_plug_get_param(P, (int)id);
  return true;
}
static bool pa_text(const clap_plugin_t *p, clap_id id, double v, char *out, uint32_t cap) {
  (void)p;
  if (id >= SMP_NALLPARAMS || smp_param_per_sample((int)id) || !cap) return false;
  smp_value_text((int)id, v, out, (int)cap);
  return true;
}
static bool pa_parse(const clap_plugin_t *p, clap_id id, const char *s, double *v) {
  (void)p;
  return id < SMP_NALLPARAMS && !smp_param_per_sample((int)id) && smp_value_parse((int)id, s, v);
}
static void pa_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) {
  Plug *P = p->plugin_data; uint32_t i, n = in ? in->size(in) : 0;
  if (!P->e || !P->s) return;
  for (i = 0; i < n; i++) {
    const clap_event_header_t *h = in->get(in, i);
    if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) event(P, h, 0, P->plays);
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
  loader_cancel(P);                      /* the project's state is newer than a window load still under way */
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
static int told_now(Plug *P) {            /* a failed or stopped window load is still being told */
  if (!P->told_until) return 0;
  if (GetTickCount64() < P->told_until) return 1;
  P->told_until = 0;
  return 0;
}

static int loading_text(Plug *P, char *out, int cap);

SMP_API int smp_plugin_status(const clap_plugin_t *p, char *out, int cap) {
  Plug *P = plug_of(p);
  if (!P || !out || cap <= 0) return 0;
  if (loading_text(P, out, cap)) {}
  else if (told_now(P)) snprintf(out, (size_t)cap, "%s", P->told);
  else if (P->plays && !P->bad && P->e && P->s) sf_status(P, out, cap);
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
  if (id < SMP_NPARAMS) return P->e ? smp_multi_get_param(P->e, id) : smp_param_info(id)->def;
  return P->s ? smp_synth_get_param(P->s, id) : smp_param_info(id)->def;
}

/* the samples a window load made become the sampler's (main thread): how is SMP_LOAD_* (plugin_int.h); index the
   sample a replace is for. Returns how many were taken. */
static int take_samples(Plug *P, SmpSample **ss, int n, int how, int index) {
  char *t; int i, took = 0, first = -1;
  if (how == SMP_LOAD_ONE && index >= 0 && index < smp_multi_count(P->e) && n > 0) {
    if (smp_multi_replace(P->e, index, ss[0])) { took = 1; first = index; }
    ss[0] = NULL;
    for (i = 1; i < n; i++) { smp_sample_free(ss[i]); ss[i] = NULL; }
  } else {
    if (how == SMP_LOAD_ALL) smp_multi_clear(P->e);
    for (i = 0; i < n; i++) {
      int k = smp_multi_add(P->e, ss[i]);  /* takes it (freed when the list is full) */
      ss[i] = NULL;
      if (k >= 0) { took++; if (first < 0) first = k; }
    }
  }
  if (first >= 0) {                      /* the new ones are picked: all that Add samples added, else the first */
    int k;
    for (k = 0; k < smp_multi_count(P->e); k++)
      smp_multi_pick(P->e, k, how == SMP_LOAD_ADD ? k >= first && k < first + took : k == first);
    smp_multi_select(P->e, first);
  }
  P->missing = 0;
  for (i = 0; i < smp_multi_count(P->e); i++) { char path[8]; if (smp_multi_path(P->e, i, path, sizeof path) == 2) P->missing++; }
  t = engine_text(P->e);
  if (t) { free(P->text); P->text = t; }
  if (P->missing) missing_status(P, P->missing); else ready_status(P);
  P->bad = 0;
  tell_host(P, SMP_P_ROOT);
  mark_dirty(P);
  if (!P->active) smp_multi_collect(P->e, 1);
  ask_collect(P);
  return took;
}

/* ---- the window's sample loads, in the background ----
   One worker thread per instance, made at the first load. The window hands it a job and returns at once; a newer
   job cancels the one running (the cancel reaches Windows' and FFmpeg's waits) and replaces one not yet started. The
   worker decodes into the cache (smp_sample_load_ex), leaves the result in `done` and asks the host for a
   main-thread callback; the sample is swapped into the engine there, or by the window's timer, never on the worker.
   The Loader is shared by the plugin and the worker, each holding a reference, and the worker holds one on this DLL
   (dropped by FreeLibraryAndExitThread), so destroy can give up waiting for a worker stuck on a dead disk: the
   worker then finds `quit`, frees its result and the Loader, and touches nothing of the plugin's. host is used only
   under the lock while quit isn't set (destroy sets it before it returns). */
typedef struct Loader Loader;
typedef struct Job {
  Loader *L;
  volatile LONG cancel;                  /* a newer load, a state load or destroy: stop */
  long serial;
  int dropped;
  int font;                              /* a SoundFont: parsed (not yet shared) into f */
  int how, index;                        /* samples: SMP_LOAD_* and the sample a replace is for */
  char path[1100];                       /* the first file (the font's, or the first sample's) */
  int n; char (*paths)[1100];            /* samples: every file, in order */
  SmpSample **ss;                        /* and what loaded (NULL for those that didn't) */
  int nfail; char fail_path[1100];       /* how many didn't, and the first of them (its reason in err) */
  SmpFont *f;
  char err[700];
} Job;
struct Loader {
  SRWLOCK lock; CONDITION_VARIABLE cv;
  int quit;
  Job *want, *busy, *done;               /* the next job, the one decoding, a finished one not yet taken */
  long serial;                           /* the newest job's (main thread writes it, under the lock) */
  volatile LONG ffmpeg;                  /* the serial of a job that got to FFmpeg */
  volatile LONG prog[3];                 /* an SFZ's sample files read so far, of how many (negative: a bundle's
                                            files unpacking), and for which serial */
  const clap_host_t *host;
  volatile LONG refs;
  HMODULE self;
  HANDLE thread;
};

/* a background load in progress: what it's doing ("Loading x.sfz", "Decoding x.dsf with FFmpeg"); 0 when none.
   Both statuses say this first, so they agree during a load. */
static int loading_text(Plug *P, char *out, int cap) {
  int ff;
  if (!P->loading || !P->loader) return 0;
  ff = !P->loading_font && InterlockedCompareExchange(&P->loader->ffmpeg, 0, 0) == P->loader->serial;
  if (!ff && P->loading_font && P->loader->prog[2] == P->loader->serial && P->loader->prog[1] > 0) {
    /* an SFZ (Decent Sampler, EXS) reading its sample files: how far, rounded to a tenth so the status changes
       about ten times rather than thousands */
    long n = P->loader->prog[1], d = P->loader->prog[0], step = n >= 100 ? n / 10 : 1;
    snprintf(out, (size_t)cap, "Loading %s: %ld of %ld samples", P->loading_name, d >= n ? n : d / step * step, n);
    return 1;
  }
  if (!ff && P->loading_font && P->loader->prog[2] == P->loader->serial && P->loader->prog[1] < 0) {
    /* a Decent Sampler bundle unpacking into the cache: files, the total given negative (sfdslib.c) */
    long n = -P->loader->prog[1], d = P->loader->prog[0], step = n >= 100 ? n / 10 : 1;
    snprintf(out, (size_t)cap, "Unpacking %s: %ld of %ld files", P->loading_name, d >= n ? n : d / step * step, n);
    return 1;
  }
  if (!P->loading_font && P->loading_n > 1) {
    long d = P->loader->prog[2] == P->loader->serial ? P->loader->prog[0] : 0;
    snprintf(out, (size_t)cap, ff ? "Loading %ld of %d samples: decoding %s with FFmpeg" : "Loading %ld of %d samples",
             d + 1 > P->loading_n ? (long)P->loading_n : d + 1, P->loading_n, P->loading_name);
    return 1;
  }
  snprintf(out, (size_t)cap, ff ? "Decoding %s with FFmpeg" : "Loading %s", P->loading_name);
  return 1;
}

static void job_free(Job *j) {
  int i;
  if (!j) return;
  for (i = 0; j->ss && i < j->n; i++) smp_sample_free(j->ss[i]);
  free(j->ss); free(j->paths);
  smp_sf_discard(j->f);
  free(j);
}
static void loader_release(Loader *L) { if (!InterlockedDecrement(&L->refs)) free(L); }
static int job_cancelled(void *a) { return InterlockedCompareExchange(&((Job *)a)->cancel, 0, 0) != 0; }
static void job_at_ffmpeg(void *a) { Job *j = a; InterlockedExchange(&j->L->ffmpeg, j->serial); }

static DWORD WINAPI load_worker(void *arg) {
  Loader *L = arg; HMODULE self = L->self;
  AcquireSRWLockExclusive(&L->lock);
  for (;;) {
    Job *j;
    while (!L->quit && !L->want) SleepConditionVariableSRW(&L->cv, &L->lock, INFINITE, 0);
    if (L->quit) break;
    j = L->want; L->want = NULL; L->busy = j;
    ReleaseSRWLockExclusive(&L->lock);
    if (j->font) {
      InterlockedExchange(&L->prog[1], 0); InterlockedExchange(&L->prog[0], 0); InterlockedExchange(&L->prog[2], j->serial);
      j->f = smp_sf_parse_detached_ex(j->path, job_cancelled, j, L->prog, j->err, sizeof j->err);
    }
    else {
      int i;
      for (i = 0; i < j->n && !j->cancel; i++) {   /* "Loading 2 of 5 samples": how far */
        char why[700] = "";
        InterlockedExchange(&L->prog[0], i); InterlockedExchange(&L->prog[1], j->n); InterlockedExchange(&L->prog[2], j->serial);
        j->ss[i] = smp_sample_load_ex(j->paths[i], job_cancelled, job_at_ffmpeg, j, why, sizeof why);
        if (!j->ss[i] && !j->cancel && !j->nfail++) {
          snprintf(j->fail_path, sizeof j->fail_path, "%s", j->paths[i]);
          snprintf(j->err, sizeof j->err, "%s", why);
        }
      }
    }
    AcquireSRWLockExclusive(&L->lock);
    L->busy = NULL;
    if (j->cancel || L->quit) job_free(j);   /* nobody wants it now */
    else {
      job_free(L->done);
      L->done = j;
      if (L->host && L->host->request_callback) L->host->request_callback(L->host);
    }
  }
  ReleaseSRWLockExclusive(&L->lock);
  loader_release(L);
  FreeLibraryAndExitThread(self, 0);       /* drops the worker's hold on the DLL; never returns */
}

static Loader *loader_of(Plug *P) {
  Loader *L;
  if (P->loader) return P->loader;
  if (!(L = calloc(1, sizeof *L))) return NULL;
  InitializeSRWLock(&L->lock); InitializeConditionVariable(&L->cv);
  L->host = P->host;
  L->refs = 2;                             /* the plugin's and the worker's */
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)(void *)load_worker, &L->self))
    L->thread = CreateThread(NULL, 0, load_worker, L, 0, NULL);
  if (!L->thread) {
    if (L->self) FreeLibrary(L->self);
    free(L);
    return NULL;
  }
  SetThreadPriority(L->thread, THREAD_PRIORITY_BELOW_NORMAL);
  return P->loader = L;
}

/* every job dropped: the one waiting and a finished one freed, the one decoding told to stop (main thread) */
static void loader_cancel(Plug *P) {
  Loader *L = P->loader;
  P->loading = 0;
  if (!L) return;
  AcquireSRWLockExclusive(&L->lock);
  job_free(L->want); L->want = NULL;
  job_free(L->done); L->done = NULL;
  if (L->busy) InterlockedExchange(&L->busy->cancel, 1);
  ReleaseSRWLockExclusive(&L->lock);
}

/* destroy: cancel, tell the worker to end, wait a little for it (it ends on its own if it can't now) */
static void loader_stop(Plug *P) {
  Loader *L = P->loader;
  if (!L) return;
  loader_cancel(P);
  P->loader = NULL;
  AcquireSRWLockExclusive(&L->lock);
  L->quit = 1; L->host = NULL;
  ReleaseSRWLockExclusive(&L->lock);
  WakeAllConditionVariable(&L->cv);
  WaitForSingleObject(L->thread, 2000);
  CloseHandle(L->thread);
  loader_release(L);
}

static const char *base_name(const char *path) {
  const char *b = path, *q;
  for (q = path; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
  return b;
}

static int load_job(Plug *P, const char *const *paths, int n, int how, int index, int dropped, int font, char *err, int cap);

/* a window load that failed or was stopped: told in the status for as long as the window's own note lasts (6 s) */
#define TELL_MS 6000
static void tell(Plug *P, const char *msg) {
  snprintf(P->told, sizeof P->told, "%s", msg);
  P->told_until = GetTickCount64() + TELL_MS;
}

void smp_plug_still_playing(Plug *P, char *out, int cap) {
  const char *name = NULL;
  if (cap <= 0) return;
  out[0] = 0;
  if (P->plays) {
    SmpFont *f = P->s ? smp_synth_font(P->s) : NULL;
    if (f && f->path) name = base_name(f->path);
  } else if (P->e && smp_multi_count(P->e) > 1) {
    snprintf(out, (size_t)cap, " Still playing %d samples.", smp_multi_count(P->e));
    return;
  } else {
    const SmpSample *s = P->e ? smp_multi_sample(P->e, smp_multi_selected(P->e)) : NULL;
    if (s) name = s->name;
  }
  if (name && name[0]) snprintf(out, (size_t)cap, " Still playing %s.", name);
}

/* "Can't load x: why." and, when something still plays, " Still playing y." (a why that ends in a full stop
   doesn't get a second one). The window's own refusals use it too. */
void smp_plug_cant_load(Plug *P, const char *path, const char *why, char *out, int cap) {
  char still[400]; size_t n = strlen(why);
  smp_plug_still_playing(P, still, sizeof still);
  snprintf(out, (size_t)cap, "Can't load %s: %s%s%s", base_name(path), why,
           still[0] && n && why[n - 1] != '.' ? "." : "", still);
}

static void load_failed(Plug *P, const char *path, const char *why) {
  smp_plug_cant_load(P, path, why, P->load_err, sizeof P->load_err);
  tell(P, P->load_err);
  P->load_failed++;
}

int smp_plug_load_start(Plug *P, const char *path, int dropped, char *err, int cap) {
  return load_job(P, &path, 1, SMP_LOAD_ALL, -1, dropped, 0, err, cap);
}

int smp_plug_load_many(Plug *P, const char *const *paths, int n, int how, int index, int dropped, char *err, int cap) {
  return load_job(P, paths, n, how, index, dropped, 0, err, cap);
}

int smp_plug_load_font_start(Plug *P, const char *path, char *err, int cap) {
  if (!P->s) { snprintf(err, (size_t)cap, "the sound engine isn't working"); return 0; }
  return load_job(P, &path, 1, 0, -1, 0, 1, err, cap);
}

static int load_job(Plug *P, const char *const *paths, int n, int how, int index, int dropped, int font, char *err, int cap) {
  Loader *L; Job *j; int i;
  const char *path = n > 0 ? paths[0] : "";
  if (!font && !P->e) { snprintf(err, (size_t)cap, "the sound engine isn't working"); return 0; }
  if (n <= 0) { snprintf(err, (size_t)cap, "no file chosen"); return 0; }
  if (!font && how == SMP_LOAD_ADD && smp_multi_count(P->e) + n > SMP_MULTI_MAX) {
    snprintf(err, (size_t)cap, "an instrument holds at most %d samples", SMP_MULTI_MAX); return 0;
  }
  if (!font && n > SMP_MULTI_MAX) { snprintf(err, (size_t)cap, "an instrument holds at most %d samples", SMP_MULTI_MAX); return 0; }
  for (i = 0; i < n; i++) if (strlen(paths[i]) >= sizeof j->path) { snprintf(err, (size_t)cap, "its path is too long"); return 0; }
  if (!(L = loader_of(P)) || !(j = calloc(1, sizeof *j))) { snprintf(err, (size_t)cap, "out of memory"); return 0; }
  if (!font) {
    j->paths = calloc((size_t)n, sizeof *j->paths); j->ss = calloc((size_t)n, sizeof *j->ss);
    if (!j->paths || !j->ss) { job_free(j); snprintf(err, (size_t)cap, "out of memory"); return 0; }
    for (i = 0; i < n; i++) strcpy(j->paths[i], paths[i]);
    j->n = n; j->how = how; j->index = index;
  }
  if (P->loading && P->loading_font != font) {
    /* the one worker serves both pages: a load of the other kind still running is stopped, and that is said (a
       newer load of the same kind replacing an older one is what was asked, so it isn't) */
    snprintf(P->load_note, sizeof P->load_note, "Stopped loading %s to load %s", P->loading_name, base_name(path));
    tell(P, P->load_note);
    P->load_notes++;
  }
  j->L = L; j->dropped = dropped; j->font = font;
  strcpy(j->path, path);
  AcquireSRWLockExclusive(&L->lock);
  job_free(L->want);
  job_free(L->done); L->done = NULL;
  if (L->busy) InterlockedExchange(&L->busy->cancel, 1);
  j->serial = ++L->serial;
  L->want = j;
  ReleaseSRWLockExclusive(&L->lock);
  WakeConditionVariable(&L->cv);
  P->loading = 1;
  P->loading_font = font;
  P->loading_n = font ? 1 : n;
  snprintf(P->loading_path, sizeof P->loading_path, "%s", path);
  snprintf(P->loading_name, sizeof P->loading_name, "%s", base_name(path));
  return 1;
}

int smp_plug_poll_load(Plug *P) {
  Loader *L = P->loader; Job *j; int r;
  if (!L) return 0;
  AcquireSRWLockExclusive(&L->lock);
  j = L->done; L->done = NULL;
  ReleaseSRWLockExclusive(&L->lock);
  if (!j) return 0;
  P->loading = 0;
  if (j->font) {
    char err[700] = "";
    SmpFont *f = j->f ? smp_sf_adopt(j->f, err, sizeof err) : NULL;
    j->f = NULL;                         /* adopted (or freed by adopt) */
    if (f && take_font(P, f, j->path, err, sizeof err)) { P->told_until = 0; r = 1; }
    else {
      load_failed(P, j->path, err[0] ? err : j->err);   /* told by the window; the status says it for a while */
      r = -1;
    }
  } else if (P->e && j->nfail < j->n) {   /* some (or all) loaded: they are taken, and any that didn't are told */
    take_samples(P, j->ss, j->n, j->how, j->index);
    if (j->dropped) smp_plug_set_plays(P, 0);   /* a dropped sample: the sampler plays */
    P->told_until = 0;
    r = 1;
    if (j->nfail) {
      char why[800];
      snprintf(why, sizeof why, j->nfail > 1 ? "%s (and %d more didn't load)" : "%s", j->err, j->nfail - 1);
      load_failed(P, j->fail_path, why);
      r = -1;
    }
  } else {
    load_failed(P, j->nfail ? j->fail_path : j->path, P->e ? j->err : "the sound engine isn't working");
    r = -1;
  }
  job_free(j);
  return r;
}

SMP_API int smp_test_plugin_loading(const clap_plugin_t *p) {   /* tests: a window load is under way */
  Plug *P = plug_of(p);
  return P && P->loading;
}

int smp_plug_sample_path(Plug *P, char *path, int cap) {
  if (P->loading && !P->loading_font) { snprintf(path, (size_t)cap, "%s", P->loading_path); return 1; }
  return P->e ? smp_multi_path(P->e, smp_multi_selected(P->e), path, cap) : (cap > 0 ? (path[0] = 0) : 0);
}

void smp_plug_sample_text(Plug *P, char *out, int cap) {
  static const char *const from[] = {"from the file name", "detected", "by default"};
  const SmpSample *s = P->e ? smp_multi_sample(P->e, smp_multi_selected(P->e)) : NULL;
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
  if (loading_text(P, out, cap)) return;  /* a background load: what it's doing */
  if (told_now(P)) { snprintf(out, (size_t)cap, "%s", P->told); return; }
  if (P->plays && !P->bad && P->e && P->s) { sf_status(P, out, cap); return; }
  if (!P->e || strncmp(P->status, "Ready", 5)) { snprintf(out, (size_t)cap, "%s", P->status); return; }
  if (smp_multi_get_param(P->e, SMP_P_KEEP_LENGTH) >= 0.5 && !smp_multi_render_status(P->e, &ready, &total, why, sizeof why)) {
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
  P->bad = 0;                            /* the window acted: an old refusal no longer says how things are */
  if (P->plays == which) return;
  set_plays(P, which);
  mark_dirty(P);
}

SmpSynth *smp_plug_synth(Plug *P) { return P->s; }

/* a Decent Sampler preset's knobs, for the window (sfsynth.c's smp_synth_knob_*); a move marks the project changed */
int smp_plug_knob_count(Plug *P) { return P->s ? smp_synth_knob_count(P->s) : 0; }
int smp_plug_knob_info(Plug *P, int i, SmpKnobInfo *k) { return P->s ? smp_synth_knob_info(P->s, i, k) : 0; }
double smp_plug_knob_get(Plug *P, int i) { return P->s ? smp_synth_knob_get(P->s, i) : 0; }
void smp_plug_knob_set(Plug *P, int i, double v) { if (P->s) { smp_synth_knob_set(P->s, i, v); mark_dirty(P); } }

/* the font's format in words, or NULL for SoundFont 2 and 3 (their text is as it was before other formats came) */
static const char *font_kind(const SmpFont *f) {
  if (f->is_gig) return "GigaStudio";
  if (f->is_ds) return "Decent Sampler";
  if (f->is_exs) return "EXS";
  if (f->is_sfz) return "SFZ";
  if (f->is_dls) return "DLS";
  if (f->is_pack) return "SF2Pack";
  if (f->version == 1) return "SoundFont 1";
  if (f->version >= 4) return "SF4";
  return NULL;
}

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
    const char *kind = font_kind(f);
    for (q = f->path; *q; q++) if (*q == '\\' || *q == '/') name = q + 1;
    snprintf(out, (size_t)cap, "%s%s%s, %d %s", name, kind ? ", " : "", kind ? kind : "", n, n == 1 ? "preset" : "presets");
  }
}

/* "Ready: x" or "Loading samples for N presets", then what the font couldn't do, each said once in plain words:
   its problem ("Missing 3 samples (first: a.wav)", an SF2Pack that can't be unpacked, the GigaStudio restart
   message) and the settings it ignored ("Ignored 12 SFZ settings it doesn't support (first: lfo01_freq)"). Both
   change only when the font does (the problem may appear once, while samples load), so the status stays still. */
static void sf_status(Plug *P, char *out, int cap) {
  char t[1200], first[200] = ""; int n, k; size_t L; SmpFont *f; const char *prob, *kind;
  if (P->sf_missing) { snprintf(out, (size_t)cap, "%s", P->sf_err); return; }
  if (!(f = smp_synth_font(P->s))) { snprintf(out, (size_t)cap, "No SoundFont loaded"); return; }
  n = smp_synth_loading(P->s);
  if (n > 0) {
    int total = 0, ready = smp_sf_load_progress(f, &total), step = total >= 100 ? total / 10 : 1;
    if (total > 0 && ready < total)
      snprintf(out, (size_t)cap, "Loading samples: %d of %d", ready / step * step, total);
    else snprintf(out, (size_t)cap, "Loading samples for %d %s", n, n == 1 ? "preset" : "presets");
  }
  else { smp_plug_font_text(P, t, sizeof t); snprintf(out, (size_t)cap, "Ready: %s", t); }
  prob = smp_sf_problem(f);
  if (prob && prob[0] && (L = strlen(out)) + 4 < (size_t)cap) {
    snprintf(out + L, (size_t)cap - L, ". %c%s", toupper((unsigned char)prob[0]), prob + 1);
    L = strlen(out);
    if (out[L - 1] == '.') out[L - 1] = 0;  /* statuses end without a full stop; ". " goes between parts */
  }
  k = smp_sf_ignored(f, first, sizeof first);
  kind = f->is_gig ? "GigaStudio" : f->is_ds ? "Decent Sampler" : f->is_exs ? "EXS" : "SFZ";
  if (k > 0 && (L = strlen(out)) + 4 < (size_t)cap)
    snprintf(out + L, (size_t)cap - L, first[0] ? ". Ignored %d %s %s it doesn't support (first: %s)" :
             ". Ignored %d %s %s it doesn't support%s", k, kind, k == 1 ? "setting" : "settings", first);
}

int smp_plug_load_font(Plug *P, const char *path, char *err, int cap) {
  SmpFont *f;
  if (!P->s) { snprintf(err, (size_t)cap, "the sound engine isn't working"); return 0; }
  if (!(f = smp_sf_open(path, err, cap))) return 0;
  return take_font(P, f, path, err, cap);
}

/* a font opened (or adopted from a background load) goes into the player, which then plays (main thread) */
static int take_font(Plug *P, SmpFont *f, const char *path, char *err, int cap) {
  if (!P->s) { smp_sf_close(f); snprintf(err, (size_t)cap, "the sound engine isn't working"); return 0; }
  if (!smp_synth_load_font(P->s, f, path, err, cap)) return 0;
  free(P->sf_line); free(P->sf_used); free(P->sf_knobs); P->sf_line = P->sf_used = P->sf_knobs = NULL; P->sf_missing = 0;
  set_plays(P, 1);
  P->bad = 0;
  if (!P->active) smp_synth_collect_now(P->s);   /* nothing processes: the old font closes now */
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

/* 1 when a key from sf_key names a configuration line (the lines smp_synth_script_cfg reads) */
static int cfg_key(const char *key) {
  char w[64] = "";
  sscanf(key, "%63s", w);
  return !strcmp(w, "sf_mode") || !strcmp(w, "sf_preset") || !strcmp(w, "sf_channel") ||
         !strcmp(w, "sf_channel_preset") || !strcmp(w, "sf_output");
}

int smp_plug_sf_set(Plug *P, const char *line) {
  char key[140], k2[140], err[700];
  char *cur, *out, *o; const char *p;
  int n, drop, ok;
  if (!P->s || !line) return 0;
  drop = sf_key(line, key, sizeof key);
  if (!cfg_key(key)) return 0;
  n = smp_synth_script_write(P->s, NULL, 0);
  cur = malloc((size_t)n + 1);
  out = malloc((size_t)n + strlen(line) + 2);
  if (!cur || !out) { free(cur); free(out); return 0; }
  smp_synth_script_write(P->s, cur, n + 1);
  /* the player's configuration lines with this one in place of the line it replaces. Only those are applied
     (smp_synth_script_cfg): the parameters aren't sent round through text, which would round them and could
     overwrite a value automation set meanwhile, and the font and sf_used stay as they are */
  for (p = cur, o = out; *p; ) {
    const char *eol = strchr(p, '\n'), *end = eol ? eol + 1 : p + strlen(p);
    char one[1200];
    snprintf(one, sizeof one, "%.*s", (int)(end - p), p);
    sf_key(one, k2, sizeof k2);
    if (cfg_key(k2) && strcmp(k2, key)) { memcpy(o, p, (size_t)(end - p)); o += end - p; }
    p = end;
  }
  if (!drop) { strcpy(o, line); o += strlen(line); *o++ = '\n'; }
  *o = 0;
  ok = !smp_synth_script_cfg(P->s, out, err, sizeof err);
  if (ok) mark_dirty(P);
  free(cur); free(out);
  return ok;
}

int smp_plug_font_path(Plug *P, char *path, int cap) {
  SmpFont *f = P->s ? smp_synth_font(P->s) : NULL;
  char line[1200];
  if (P->loading && P->loading_font) { snprintf(path, (size_t)cap, "%s", P->loading_path); return 1; }
  if (P->sf_missing && P->sf_line && font_line(P->sf_line, line, sizeof line, path, cap) == 2) return 2;
  if (f) { snprintf(path, (size_t)cap, "%s", f->path); return 1; }
  if (cap > 0) path[0] = 0;
  return 0;
}

/* the Sampler's samples, for the window (plugin_int.h) */
int smp_plug_zone_count(Plug *P) { return P->e ? smp_multi_count(P->e) : 0; }
int smp_plug_zone_selected(Plug *P) { return P->e ? smp_multi_selected(P->e) : -1; }
void smp_plug_zone_select(Plug *P, int i) {
  if (!P->e || i == smp_multi_selected(P->e)) return;
  smp_multi_select(P->e, i);
  tell_host(P, SMP_P_ROOT);              /* Root note is the selected sample's */
}
void smp_plug_zone_line(Plug *P, int i, char *out, int cap) { if (P->e) smp_multi_line(P->e, i, out, cap); else if (cap > 0) out[0] = 0; }
void smp_plug_zone_range(Plug *P, int i, int *klo, int *khi, int *vlo, int *vhi) {
  if (P->e) smp_multi_range(P->e, i, klo, khi, vlo, vhi); else { *klo = 0; *khi = 127; *vlo = 1; *vhi = 127; }
}
int smp_plug_zone_set_range(Plug *P, int i, int klo, int khi, int vlo, int vhi) {
  int a, b, c, d;
  if (!P->e) return 0;
  smp_multi_range(P->e, i, &a, &b, &c, &d);
  if (a == klo && b == khi && c == vlo && d == vhi) return 1;
  if (!smp_multi_set_range(P->e, i, klo, khi, vlo, vhi)) return 0;
  mark_dirty(P);
  return 1;
}
int smp_plug_zone_key_class(Plug *P, int i) { return P->e ? smp_multi_key_class(P->e, i) : -1; }
int smp_plug_zone_set_key_class(Plug *P, int i, int pc) {
  if (!P->e) return 0;
  if (smp_multi_key_class(P->e, i) == pc) return 1;
  if (!smp_multi_set_key_class(P->e, i, pc)) return 0;
  mark_dirty(P);
  return 1;
}
int smp_plug_zone_keep(Plug *P, const char *restore, const char *add, char *err, int cap) {
  int sel, k, i; char cur[1100]; SmpSample *s; char *t;
  if (!P->e || (sel = smp_multi_selected(P->e)) < 0) { snprintf(err, (size_t)cap, "no sample is selected"); return 0; }
  if (P->loading && !P->loading_font) loader_cancel(P);   /* the file shown is loaded here instead */
  if (smp_multi_count(P->e) >= SMP_MULTI_MAX) { snprintf(err, (size_t)cap, "an instrument holds at most %d samples", SMP_MULTI_MAX); return 0; }
  if (!(s = smp_sample_load(add, err, cap))) return 0;
  smp_multi_path(P->e, sel, cur, sizeof cur);
  if (restore[0] && strcmp(cur, restore)) {     /* the sample tried in its place goes back as it was */
    char why[600];
    SmpSample *r = smp_sample_load(restore, why, sizeof why);
    if (r) smp_multi_replace(P->e, sel, r);
  }
  if ((k = smp_multi_add(P->e, s)) < 0) { snprintf(err, (size_t)cap, "out of memory"); return 0; }
  for (i = 0; i < smp_multi_count(P->e); i++) smp_multi_pick(P->e, i, i == sel);
  P->missing = 0;
  for (i = 0; i < smp_multi_count(P->e); i++) { char path[8]; if (smp_multi_path(P->e, i, path, sizeof path) == 2) P->missing++; }
  t = engine_text(P->e);
  if (t) { free(P->text); P->text = t; }
  if (P->missing) missing_status(P, P->missing); else ready_status(P);
  P->bad = 0; P->told_until = 0;
  tell_host(P, SMP_P_ROOT);
  mark_dirty(P);
  if (!P->active) smp_multi_collect(P->e, 1);
  ask_collect(P);
  return 1;
}
int smp_plug_zone_picked(Plug *P, int i) { return P->e ? smp_multi_picked(P->e, i) : 0; }
void smp_plug_zone_pick(Plug *P, int i, int on) { if (P->e) smp_multi_pick(P->e, i, on); }
void smp_plug_zone_remove_picked(Plug *P) {
  int i, any = 0;
  if (!P->e) return;
  for (i = 0; i < smp_multi_count(P->e); i++) any |= smp_multi_picked(P->e, i);
  if (!any) { smp_plug_zone_remove(P, smp_multi_selected(P->e)); return; }
  {                                      /* which, first: removing one may pick another (none left picked) */
    int list[SMP_MULTI_MAX], n = 0;
    for (i = 0; i < smp_multi_count(P->e); i++) if (smp_multi_picked(P->e, i)) list[n++] = i;
    while (n--) {
      int sel = smp_multi_selected(P->e);
      smp_multi_remove(P->e, list[n]);
      if (sel > list[n]) smp_multi_select(P->e, sel - 1);   /* the selected one stays the same sample */
    }
  }
  smp_plug_zone_remove(P, -1);           /* the status, the host and the project told (removes nothing) */
}
void smp_plug_zone_remove(Plug *P, int i) {
  if (!P->e || i >= smp_multi_count(P->e)) return;
  if (i >= 0) smp_multi_remove(P->e, i);
  P->missing = 0;
  { int k; char path[8]; for (k = 0; k < smp_multi_count(P->e); k++) if (smp_multi_path(P->e, k, path, sizeof path) == 2) P->missing++; }
  if (P->missing) missing_status(P, P->missing); else ready_status(P);
  tell_host(P, SMP_P_ROOT);
  mark_dirty(P);
  if (!P->active) smp_multi_collect(P->e, 1);
  ask_collect(P);
}

int smp_plug_sf_channel_now(Plug *P, int ch, int *bank, int *program) {
  return P->s ? smp_synth_channel_preset(P->s, ch, bank, program) : 0;
}

/* ---- test hooks (tests\host.c): make a real-time plugin render exactly like smp_engine_render_test ---- */
SMP_API int smp_test_plugin_wait(const clap_plugin_t *p, int timeout_ms) {   /* keep-length copies and start heads */
  Plug *P = plug_of(p);
  if (!P || !P->e) return 1;
  if (smp_multi_wait_renders(P->e, timeout_ms)) return 1;   /* and each engine's worker */
  if (P->s && smp_synth_wait_loaded(P->s, timeout_ms)) return 1;   /* the state's sf_used presets */
  return 0;
}
SMP_API void smp_test_plugin_offline(const clap_plugin_t *p, int on) {   /* note-ons past the head wait for the streamer */
  Plug *P = plug_of(p);
  if (P && P->e) smp_multi_test_offline(P->e, on);
}
SMP_API int smp_test_plugin_voices(const clap_plugin_t *p) {   /* sounding voices */
  Plug *P = plug_of(p);
  if (P && P->plays && P->s) return smp_synth_voices(P->s, NULL);
  return P && P->e ? smp_multi_active_voices(P->e) : 0;
}
SMP_API void smp_test_plugin_sync(const clap_plugin_t *p) {   /* a whole streamer pass; call between process calls */
  Plug *P = plug_of(p);
  if (P && P->e) smp_multi_test_sync(P->e);
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

static bool e_init(const char *path) { (void)path; smp_update_cleanup(NULL); return true; }   /* an update's .old */
static void e_deinit(void) {}
static const void *e_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? NULL : &factory; }
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, e_init, e_deinit, e_factory};
