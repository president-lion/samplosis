/* A stand-in CLAP host for the tests: loads samplosis.clap through clap_entry and prints plain lines.
     host.exe samplosis.clap state <state.txt>                     load the state, save it, print it
     host.exe samplosis.clap play <state.txt> <events> <out.wav> [rate [all]]
   Events, one per line: "on key vel frame", "off key frame [channel]" (CLAP notes on channel 0, or a note-off on
   that channel, -1 any), "midi status d1 d2 frame", "bend fraction frame" (a MIDI pitch bend), "param id value
   frame", "length frames"; between blocks: "reset frame",
   "cycle frame" (deactivate and activate again), "state <file> frame" (load another state), "render offline|realtime
   frame" (the render extension's set); and "rawon key vel frame time" (a note-on in the block holding frame, with its
   time field set to time as is: a sloppy host). "sleep ms frame" lets wall time pass between
   blocks; "rename <from> <to> frame" renames a file between blocks (paths without spaces). With
   SAMPLOSIS_TEST_PAUSE_LOADER set, the SoundFont loader is paused; SAMPLOSIS_TEST_GRACE_MS sets how soon it frees
   samples no preset in use needs. Blocks of 256 frames. The host gives one stereo output, or with
   `all` every output port the plugin has, each written to the wav as 2 channels in port order.
   play makes the real-time plugin render exactly, like smp_engine_render_test, through its smp_test_* hooks.
     host.exe samplosis.clap gui <state.txt> <actions.txt>
   Opens the plugin's window on a desktop of its own (never the one in use) between a "before" and an "after" button, as
   REAPER's FX window has controls around it, and runs actions, one per line. Keys go to the focused control by
   message (never SendInput: someone is using the desktop). A line "focus: <class> | <name> | <value>" tells what a
   screen reader would find focused: a button's name is its text; another control's name is its accessible name
   (MSAA, as a screen reader asks for it), else the label just before it. A slider (a trackbar) shows as class
   "Slider" with its accessible value; a tab control as class "Tab" with its accessible name and, as its value, the
   name of the tab MSAA says is selected.
     key <tab|up|down|enter|space|escape|left|right|pageup|pagedown|home|end> [shift] [ctrl]   a key, then the focus line
     char <c>                a character typed (WM_CHAR), then the focus line
     say                     the focus line
     focus <name>            focus the plugin's control with that name (silent; "note: no control" if none shows)
     window                  focus the plugin's window itself, as REAPER does coming back to it (silent)
     type <text>             set the focused control's text (silent)
     read <name>             "read: <name> | <value>"
     dump                    "control: <class> | <name> | <value>" for every control that shows
     param <id>              "param <id>: <value>"
     flush                   the host's params flush: "out: gesture_begin|value|gesture_end <id> [value]"
     state [word]            the saved state's sample line: "state: sample ..."; with a word, every saved line
                             starting with it ("state: sf_preset 0 1"), or "state: (none)"
     midi <status> <d1> <d2> one block of 256 frames through the plugin with that MIDI event at frame 0 (the plugin
                             is activated at 48 kHz first, and deactivated at the end)
     drop <path>             drop the file on the plugin's window (WM_DROPFILES)
     dropdown <open|close>   open or close the focused combo box's list (CB_SHOWDROPDOWN)
     load <state.txt>      the host loads another state while the window is open ("load: ok")
     list <name>             a combo box's entries: "list: <name> | <entry> | <entry> ..."
     counts                  "counts: beeps N | request_flush N | mark_dirty N"
     pump <ms>               let the window's timers run
     dialog <pick|closegui|destroy> <path>   the next file or folder dialog returns path at once; closegui first
                             destroys the window, destroy the whole plugin (as REAPER can while a dialog is open)
     alive                   "alive: plugin" or "alive: plugin destroyed" (the host got this far)
     async <on|off>          on: keys and drops don't wait for the background load they start (see settle)
     wait                    wait for the background load to be swapped in (up to 20 s)
     clock                   "clock: <n> ms" since the last clock
     status                  "status: <text>", the plugin's own status (smp_plugin_status)
     pids <file>             "pids: <running> of <all> running" for the process ids listed in the file
     ffsearches              "ffsearches: N", how many times the plugin has searched for FFmpeg (PATH, WinGet)
   SAMPLOSIS_TEST_DECODE_FLAGS passes its number to smp_test_only_decode_flags. */
#include <windows.h>
#include <oleacc.h>
#include <shlobj.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>

#define BLOCK 256

typedef int (*StatusFn)(const clap_plugin_t *, char *, int);
typedef int (*WaitFn)(const clap_plugin_t *, int);
typedef void (*OfflineFn)(const clap_plugin_t *, int);
typedef void (*SyncFn)(const clap_plugin_t *);
typedef int (*VoicesFn)(const clap_plugin_t *);

static volatile LONG want_callback;
static int flushes, dirties;
static void hp_rescan(const clap_host_t *h, clap_param_rescan_flags f) { (void)h; (void)f; }
static void hp_clear(const clap_host_t *h, clap_id id, clap_param_clear_flags f) { (void)h; (void)id; (void)f; }
static void hp_flush(const clap_host_t *h) { (void)h; flushes++; }
static const clap_host_params_t host_params = {hp_rescan, hp_clear, hp_flush};
static void hs_dirty(const clap_host_t *h) { (void)h; dirties++; }
static const clap_host_state_t host_state = {hs_dirty};
static const void *h_ext(const clap_host_t *h, const char *id) {
  (void)h;
  if (!strcmp(id, CLAP_EXT_PARAMS)) return &host_params;
  if (!strcmp(id, CLAP_EXT_STATE)) return &host_state;
  return NULL;
}
static void h_restart(const clap_host_t *h) { (void)h; }
static void h_process(const clap_host_t *h) { (void)h; }
static void h_callback(const clap_host_t *h) { (void)h; InterlockedExchange(&want_callback, 1); }
static clap_host_t host = {CLAP_VERSION_INIT, NULL, "samplosis test host", "audioskit", "", "1", h_ext, h_restart,
                           h_process, h_callback};

static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb"); char *b; long n;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
  b = malloc((size_t)n + 1);
  if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
  fclose(f);
  if (b) { b[n] = 0; if (len) *len = (size_t)n; }
  return b;
}

/* streams over a buffer; reads come in small pieces, as a host may give them */
typedef struct { char *buf; size_t len, pos, cap; } Mem;
static int64_t is_read(const clap_istream_t *s, void *out, uint64_t n) {
  Mem *m = s->ctx; size_t k = m->len - m->pos;
  if (n > 1000) n = 1000;
  if (k > n) k = (size_t)n;
  memcpy(out, m->buf + m->pos, k); m->pos += k;
  return (int64_t)k;
}
static int64_t os_write(const clap_ostream_t *s, const void *in, uint64_t n) {
  Mem *m = s->ctx;
  if (m->len + n + 1 > m->cap) {
    size_t c = (m->len + (size_t)n + 1) * 2; char *b = realloc(m->buf, c);
    if (!b) return -1;
    m->buf = b; m->cap = c;
  }
  memcpy(m->buf + m->len, in, (size_t)n); m->len += (size_t)n; m->buf[m->len] = 0;
  return (int64_t)n;
}

static HMODULE dll;
static void status_line(const clap_plugin_t *p) {
  char st[2000] = "";
  StatusFn f = (StatusFn)GetProcAddress(dll, "smp_plugin_status");
  if (f) f(p, st, sizeof st);
  printf("status: %s\n", st);
}

static void main_thread(const clap_plugin_t *p) {
  if (InterlockedExchange(&want_callback, 0)) p->on_main_thread(p);
}

static int load_state(const clap_plugin_t *p, const char *path) {
  const clap_plugin_state_t *st = p->get_extension(p, CLAP_EXT_STATE);
  Mem m = {0}; clap_istream_t is = {&m, is_read}; int ok;
  m.buf = read_file(path, &m.len);
  if (!m.buf) { printf("error: can't read %s\n", path); return -1; }
  ok = st->load(p, &is);
  free(m.buf);
  printf("load: %s\n", ok ? "ok" : "refused");
  return ok;
}

static int cmd_state(const clap_plugin_t *p, const char *path) {
  const clap_plugin_descriptor_t *d = p->desc;
  const clap_plugin_params_t *pa = p->get_extension(p, CLAP_EXT_PARAMS);
  const clap_plugin_note_ports_t *np = p->get_extension(p, CLAP_EXT_NOTE_PORTS);
  const clap_plugin_audio_ports_t *ap = p->get_extension(p, CLAP_EXT_AUDIO_PORTS);
  const clap_plugin_latency_t *lat = p->get_extension(p, CLAP_EXT_LATENCY);
  const clap_plugin_state_t *st = p->get_extension(p, CLAP_EXT_STATE);
  uint32_t i, n;
  printf("name: %s\nvendor: %s\nversion: %s\nid: %s\nfeatures:", d->name, d->vendor, d->version, d->id);
  for (i = 0; d->features[i]; i++) printf(" %s", d->features[i]);
  printf("\n");
  if (!pa || !np || !ap || !lat || !st) { printf("error: an extension is missing\n"); return 1; }
  {
    clap_note_port_info_t ni; memset(&ni, 0, sizeof ni);
    np->get(p, 0, true, &ni);
    printf("note ports: %u in,%s%s, prefers %s\n", np->count(p, true), ni.supported_dialects & CLAP_NOTE_DIALECT_CLAP ? " clap" : "",
           ni.supported_dialects & CLAP_NOTE_DIALECT_MIDI ? " midi" : "",
           ni.preferred_dialect == CLAP_NOTE_DIALECT_MIDI ? "midi" : ni.preferred_dialect == CLAP_NOTE_DIALECT_CLAP ? "clap" : "other");
  }
  {
    clap_audio_port_info_t ai; memset(&ai, 0, sizeof ai);
    ap->get(p, 0, false, &ai);
    printf("audio ports: %u out, %u channels\n", ap->count(p, false), ai.channel_count);
    printf("audio port names: ");
    for (i = 0; i < ap->count(p, false); i++) {
      memset(&ai, 0, sizeof ai);
      ap->get(p, i, false, &ai);
      printf("%s%s%s", i ? ", " : "", ai.name, ai.flags & CLAP_AUDIO_PORT_IS_MAIN ? " (main)" : "");
    }
    printf("\n");
  }
  printf("latency: %u\n", lat->get(p));
  n = pa->count(p);
  printf("params: %u\n", n);
  for (i = 0; i < n; i++) {
    clap_param_info_t pi; char t[128];
    pa->get_info(p, i, &pi);
    pa->value_to_text(p, pi.id, pi.default_value, t, sizeof t);
    printf("param %u %s %g %g %g%s%s text %s\n", pi.id, pi.name, pi.min_value, pi.max_value, pi.default_value,
           pi.flags & CLAP_PARAM_IS_AUTOMATABLE ? " automatable" : "", pi.flags & CLAP_PARAM_IS_STEPPED ? " stepped" : "", t);
  }
  { double v = 0; if (pa->text_to_value(p, 3, "-6 dB", &v)) printf("parse -6 dB = %g\n", v); }
  if (load_state(p, path) < 0) return 1;
  main_thread(p);
  status_line(p);
  {
    Mem m = {0}; clap_ostream_t os = {&m, os_write};
    if (!st->save(p, &os)) { printf("error: save failed\n"); return 1; }
    printf("--- state\n%s", m.buf ? m.buf : "");
    free(m.buf);
  }
  return 0;
}

typedef struct { long frame; clap_event_param_value_t pv; clap_event_note_t note; clap_event_midi_t midi; int kind;
                 int action; char path[600], path2[600]; long rawtime; } Ev;   /* action: 1 reset, 2 deactivate and
                                                                     activate, 3 load state, 4 render offline,
                                                                     5 render realtime, 6 sleep (rawtime ms),
                                                                     7 rename path to path2 */
static Ev *evs; static int nev;
typedef struct { const Ev *const *list; uint32_t n; } EvList;
static const clap_event_header_t *ev_header(const Ev *e) {
  return e->kind == 0 ? &e->note.header : e->kind == 1 ? &e->midi.header : &e->pv.header;
}
static uint32_t el_size(const clap_input_events_t *l) { return ((const EvList *)l->ctx)->n; }
static const clap_event_header_t *el_get(const clap_input_events_t *l, uint32_t i) {
  const EvList *x = l->ctx; return i < x->n ? ev_header(x->list[i]) : NULL;
}
static bool oe_push(const clap_output_events_t *l, const clap_event_header_t *h) { (void)l; (void)h; return true; }
static bool oe_print(const clap_output_events_t *l, const clap_event_header_t *h) {   /* what the plugin tells the host */
  (void)l;
  if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return true;
  if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN) printf("out: gesture_begin %u\n", ((const clap_event_param_gesture_t *)h)->param_id);
  else if (h->type == CLAP_EVENT_PARAM_GESTURE_END) printf("out: gesture_end %u\n", ((const clap_event_param_gesture_t *)h)->param_id);
  else if (h->type == CLAP_EVENT_PARAM_VALUE)
    printf("out: value %u %g\n", ((const clap_event_param_value_t *)h)->param_id, ((const clap_event_param_value_t *)h)->value);
  return true;
}

static int parse_events(const char *text, long *length) {
  const char *p = text; int cap = 0;
  *length = -1;
  while (p && *p) {
    char w[16]; int used = 0, ok = 0, nch = 0; Ev e; const char *nl = strchr(p, '\n');
    memset(&e, 0, sizeof e);
    if (sscanf(p, "%15s%n", w, &used) == 1) {
      int a, b, c; double x;
      if (!strcmp(w, "length")) { if (sscanf(p + used, "%ld", length) != 1) return 0; p = nl ? nl + 1 : NULL; continue; }
      if (!strcmp(w, "reset") && sscanf(p + used, "%ld", &e.frame) == 1) { e.action = 1; ok = 1; }
      else if (!strcmp(w, "cycle") && sscanf(p + used, "%ld", &e.frame) == 1) { e.action = 2; ok = 1; }
      else if (!strcmp(w, "state") && sscanf(p + used, "%599s %ld", e.path, &e.frame) == 2) { e.action = 3; ok = 1; }
      else if (!strcmp(w, "render") && sscanf(p + used, "%599s %ld", e.path, &e.frame) == 2 &&
               (!strcmp(e.path, "offline") || !strcmp(e.path, "realtime"))) { e.action = strcmp(e.path, "offline") ? 5 : 4; ok = 1; }
      else if (!strcmp(w, "sleep") && sscanf(p + used, "%ld %ld", &e.rawtime, &e.frame) == 2) { e.action = 6; ok = 1; }
      else if (!strcmp(w, "rename") && sscanf(p + used, "%599s %599s %ld", e.path, e.path2, &e.frame) == 3) { e.action = 7; ok = 1; }
      else if (!strcmp(w, "rawon") && sscanf(p + used, "%d %d %ld %ld", &a, &b, &e.frame, &e.rawtime) == 4) {
        e.kind = 0; e.note.header.type = CLAP_EVENT_NOTE_ON; e.note.key = (int16_t)a; e.note.velocity = b / 127.0; e.rawtime++; ok = 1;
      } else if (!strcmp(w, "on") && sscanf(p + used, "%d %d %ld", &a, &b, &e.frame) == 3) {
        e.kind = 0; e.note.header.type = CLAP_EVENT_NOTE_ON; e.note.key = (int16_t)a; e.note.velocity = b / 127.0; ok = 1;
      } else if (!strcmp(w, "off") && sscanf(p + used, "%d %ld", &a, &e.frame) == 2) {
        e.kind = 0; e.note.header.type = CLAP_EVENT_NOTE_OFF; e.note.key = (int16_t)a; ok = 1;
        if (sscanf(p + used, "%d %ld %d", &a, &e.frame, &c) == 3) nch = c;               /* a channel: set below */
      } else if (!strcmp(w, "midi") && sscanf(p + used, "%d %d %d %ld", &a, &b, &c, &e.frame) == 4) {
        e.kind = 1; e.midi.header.type = CLAP_EVENT_MIDI;
        e.midi.data[0] = (uint8_t)a; e.midi.data[1] = (uint8_t)b; e.midi.data[2] = (uint8_t)c; ok = 1;
      } else if (!strcmp(w, "bend") && sscanf(p + used, "%lf %ld", &x, &e.frame) == 2) {
        int v = (int)(x * 8192.0 + (x < 0 ? -0.5 : 0.5)) + 8192;
        if (v < 0) v = 0;
        if (v > 16383) v = 16383;
        e.kind = 1; e.midi.header.type = CLAP_EVENT_MIDI;
        e.midi.data[0] = 0xE0; e.midi.data[1] = (uint8_t)(v & 0x7F); e.midi.data[2] = (uint8_t)(v >> 7); ok = 1;
      } else if (!strcmp(w, "param") && sscanf(p + used, "%d %lf %ld", &a, &x, &e.frame) == 3) {
        e.kind = 2; e.pv.header.type = CLAP_EVENT_PARAM_VALUE; e.pv.param_id = (clap_id)a; e.pv.value = x;
        e.pv.note_id = -1; e.pv.port_index = -1; e.pv.channel = -1; e.pv.key = -1; ok = 1;
      }
      if (!ok) return 0;
      {
        clap_event_header_t *h = (clap_event_header_t *)ev_header(&e);
        h->size = e.kind == 0 ? sizeof e.note : e.kind == 1 ? sizeof e.midi : sizeof e.pv;
        h->space_id = CLAP_CORE_EVENT_SPACE_ID;
        if (e.kind == 0) { e.note.note_id = -1; e.note.port_index = 0; e.note.channel = 0; }
        if (e.kind == 0) e.note.channel = (int16_t)nch;
      }
      if (nev == cap) { cap = cap ? cap * 2 : 64; evs = realloc(evs, sizeof *evs * (size_t)cap); if (!evs) return 0; }
      evs[nev++] = e;
    }
    p = nl ? nl + 1 : NULL;
  }
  { int i, j; for (i = 1; i < nev; i++) { Ev x = evs[i]; for (j = i - 1; j >= 0 && evs[j].frame > x.frame; j--) evs[j + 1] = evs[j]; evs[j + 1] = x; } }
  return 1;
}

static void wav_header(FILE *f, uint32_t frames, uint32_t sr, uint16_t ch) {
  uint32_t data = frames * 4 * ch, riff = 36 + data, br = sr * 4 * ch, fmt = 16;
  uint16_t tag = 3, align = (uint16_t)(4 * ch), bits = 32;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
  fwrite(&tag, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
  fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

#define MAXPORTS 16
static int cmd_play(const clap_plugin_t *p, const char *state, const char *events, const char *out, double rate, int all) {
  WaitFn wait = (WaitFn)GetProcAddress(dll, "smp_test_plugin_wait");
  OfflineFn offline = (OfflineFn)GetProcAddress(dll, "smp_test_plugin_offline");
  SyncFn sync = (SyncFn)GetProcAddress(dll, "smp_test_plugin_sync");
  VoicesFn voices = (VoicesFn)GetProcAddress(dll, "smp_test_plugin_voices");
  char *text = read_file(events, NULL); long length, b; int k = 0;
  static float bufs[2 * MAXPORTS][BLOCK], lr[2 * MAXPORTS * BLOCK]; float *chans[2 * MAXPORTS];
  const Ev *list[1024]; EvList el; FILE *f;
  clap_input_events_t in = {&el, el_size, el_get};
  clap_output_events_t oe = {NULL, oe_push};
  const clap_plugin_audio_ports_t *ap = p->get_extension(p, CLAP_EXT_AUDIO_PORTS);
  const clap_plugin_render_t *rn = p->get_extension(p, CLAP_EXT_RENDER);
  clap_audio_buffer_t ob[MAXPORTS]; clap_process_t pr; uint32_t nports = 1, c, j;
  if (!wait || !offline || !sync) { printf("error: the test hooks are missing\n"); return 1; }
  if (!text || !parse_events(text, &length)) { printf("error: can't read the events in %s\n", events); return 1; }
  free(text);
  if (length < 0) length = (nev ? evs[nev - 1].frame : 0) + (long)rate;
  if (load_state(p, state) < 0) return 1;
  main_thread(p);
  if (!p->activate(p, rate, 1, BLOCK)) { printf("error: activate failed\n"); return 1; }
  status_line(p);
  if (wait(p, 120000)) { printf("error: renders didn't finish\n"); return 1; }
  offline(p, 1);
  p->start_processing(p);
  if (!(f = fopen(out, "wb"))) { printf("error: can't write %s\n", out); return 1; }
  if (all) { nports = ap ? ap->count(p, false) : 1; if (nports > MAXPORTS) nports = MAXPORTS; }
  wav_header(f, (uint32_t)length, (uint32_t)(rate + 0.5), (uint16_t)(2 * nports));
  memset(ob, 0, sizeof ob);
  for (c = 0; c < 2 * MAXPORTS; c++) chans[c] = bufs[c];
  for (c = 0; c < nports; c++) { ob[c].data32 = chans + 2 * c; ob[c].channel_count = 2; }
  memset(&pr, 0, sizeof pr);
  pr.steady_time = 0; pr.audio_outputs = ob; pr.audio_outputs_count = nports; pr.in_events = &in; pr.out_events = &oe;
  for (b = 0; b < length; b += BLOCK) {
    uint32_t n = (uint32_t)(length - b < BLOCK ? length - b : BLOCK), i;
    el.list = list; el.n = 0;
    for (; k < nev && evs[k].frame < b + (long)n && el.n < 1024; k++) {
      clap_event_header_t *h = (clap_event_header_t *)ev_header(&evs[k]);
      if (evs[k].action == 1) { p->reset(p); continue; }                     /* between blocks, as a host does */
      if (evs[k].action == 2) {
        p->stop_processing(p); p->deactivate(p); main_thread(p);
        if (!p->activate(p, rate, 1, BLOCK)) { printf("error: activate failed\n"); return 1; }
        offline(p, 1); p->start_processing(p); continue;
      }
      if (evs[k].action == 3) { if (load_state(p, evs[k].path) < 0) return 1; main_thread(p); continue; }
      if (evs[k].action == 4 || evs[k].action == 5) {
        if (!rn || !rn->set(p, evs[k].action == 4 ? CLAP_RENDER_OFFLINE : CLAP_RENDER_REALTIME)) { printf("error: render set failed\n"); return 1; }
        continue;
      }
      if (evs[k].action == 6) { Sleep((DWORD)evs[k].rawtime); main_thread(p); continue; }   /* wall time passes */
      if (evs[k].action == 7) {                                              /* a file goes away (or comes back) */
        if (!MoveFileA(evs[k].path, evs[k].path2)) { printf("error: can't rename %s\n", evs[k].path); return 1; }
        continue;
      }
      h->time = (uint32_t)(evs[k].rawtime ? evs[k].rawtime - 1 : evs[k].frame < b ? 0 : evs[k].frame - b);
      list[el.n++] = &evs[k];
    }
    pr.frames_count = n; pr.steady_time = b;
    p->process(p, &pr);
    for (i = 0; i < n; i++) for (j = 0; j < 2 * nports; j++) lr[2 * nports * i + j] = bufs[j][i];
    fwrite(lr, 8 * nports, n, f);
    sync(p);
    main_thread(p);
  }
  fclose(f);
  if (voices) printf("voices: %d\n", voices(p));
  { char st[2000] = ""; StatusFn sf = (StatusFn)GetProcAddress(dll, "smp_plugin_status"); if (sf) sf(p, st, sizeof st); printf("status after: %s\n", st); }
  p->stop_processing(p);
  offline(p, 0);
  p->deactivate(p);
  main_thread(p);
  printf("frames: %ld\n", length);
  return 0;
}

/* ---- the window ---- */
typedef long (*BeepsFn)(void);
typedef void (*QuietFn)(int);
typedef void (*DialogFn)(const clap_plugin_t *, int, const char *);
static HWND plugwin;
static int plugin_gone;                     /* a "dialog destroy" freed the plugin: don't touch it again */

static int cls_is(HWND h, const wchar_t *c) { wchar_t n[64] = L""; GetClassNameW(h, n, 64); return !_wcsicmp(n, c); }
static void utf8(const wchar_t *w, char *out, int cap) { WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL); }

/* the accessible name and value (MSAA), as a screen reader asks for them; 0 when there is no accessible object */
static int msaa(HWND h, char *name, char *value) {
  IAccessible *a = NULL; VARIANT self; BSTR b = NULL;
  if (FAILED(AccessibleObjectFromWindow(h, (DWORD)OBJID_CLIENT, &IID_IAccessible, (void **)&a)) || !a) return 0;
  VariantInit(&self); self.vt = VT_I4; self.lVal = CHILDID_SELF;
  if (SUCCEEDED(a->lpVtbl->get_accName(a, self, &b)) && b) { utf8(b, name, 300); SysFreeString(b); b = NULL; }
  if (value && SUCCEEDED(a->lpVtbl->get_accValue(a, self, &b)) && b) { utf8(b, value, 1100); SysFreeString(b); }
  a->lpVtbl->Release(a);
  return 1;
}

static void selected_tab(HWND h, char *value);
/* class, name and value as a screen reader finds them */
static void describe(HWND h, char *cls, char *name, char *value) {
  wchar_t c[64] = L"", t[1100] = L"";
  cls[0] = name[0] = value[0] = 0;
  if (!h) { strcpy(cls, "nothing"); return; }
  GetClassNameW(h, c, 64); utf8(c, cls, 64);
  GetWindowTextW(h, t, 1100);
  if (cls_is(h, L"Button")) { utf8(t, name, 300); return; }
  if (cls_is(h, L"msctls_trackbar32")) { strcpy(cls, "Slider"); msaa(h, name, value); return; }
  if (cls_is(h, L"SysTabControl32")) { strcpy(cls, "Tab"); msaa(h, name, NULL); selected_tab(h, value); return; }
  msaa(h, name, NULL);
  if (!name[0]) {
    HWND prev = GetWindow(h, GW_HWNDPREV);
    if (prev && cls_is(prev, L"Static")) { wchar_t lt[300]; GetWindowTextW(prev, lt, 300); utf8(lt, name, 300); }
  }
  if (cls_is(h, L"ComboBox")) {
    LRESULT i = SendMessageW(h, CB_GETCURSEL, 0, 0);
    t[0] = 0;
    if (i >= 0) SendMessageW(h, CB_GETLBTEXT, (WPARAM)i, (LPARAM)t);
  }
  if (cls_is(h, L"ListBox")) {
    LRESULT i = SendMessageW(h, LB_GETCURSEL, 0, 0);
    t[0] = 0;
    if (i >= 0 && SendMessageW(h, LB_GETTEXTLEN, (WPARAM)i, 0) < 1100) SendMessageW(h, LB_GETTEXT, (WPARAM)i, (LPARAM)t);
  }
  utf8(t, value, 1100);
}
/* a tab control: the name of the tab MSAA reports as selected */
static void selected_tab(HWND h, char *value) {
  IAccessible *a = NULL; long n = 0, i;
  if (FAILED(AccessibleObjectFromWindow(h, (DWORD)OBJID_CLIENT, &IID_IAccessible, (void **)&a)) || !a) return;
  a->lpVtbl->get_accChildCount(a, &n);
  for (i = 1; i <= n; i++) {
    VARIANT c, st; BSTR b = NULL;
    VariantInit(&c); VariantInit(&st); c.vt = VT_I4; c.lVal = i;
    if (FAILED(a->lpVtbl->get_accState(a, c, &st)) || st.vt != VT_I4 || !(st.lVal & STATE_SYSTEM_SELECTED)) continue;
    if (SUCCEEDED(a->lpVtbl->get_accName(a, c, &b)) && b) { utf8(b, value, 1100); SysFreeString(b); }
    break;
  }
  a->lpVtbl->Release(a);
}
static void say(void) {
  char c[64], n[300], v[1100];
  describe(GetFocus(), c, n, v);
  printf("focus: %s | %s | %s\n", c, n, v);
}
static HWND find_named(const char *want) {
  HWND w;
  for (w = GetWindow(plugwin, GW_CHILD); w; w = GetWindow(w, GW_HWNDNEXT)) {
    char c[64], n[300], v[1100];
    if (!IsWindowVisible(w) || !(GetWindowLongW(w, GWL_STYLE) & WS_TABSTOP)) continue;
    describe(w, c, n, v);
    if (!strcmp(n, want)) return w;
  }
  return NULL;
}
static void pump(int ms) {
  DWORD end = GetTickCount() + (DWORD)ms; MSG m;
  for (;;) {
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    if ((int)(end - GetTickCount()) <= 0) break;
    Sleep(5);
  }
}
/* keys straight to the focused control, as REAPER's FX window leaves them */
static void key(WPARAM vk, int shift, int ctrl) {
  BYTE ks[256];
  GetKeyboardState(ks);
  ks[VK_SHIFT] = shift ? 0x80 : 0; ks[VK_CONTROL] = ctrl ? 0x80 : 0; SetKeyboardState(ks);
  SendMessageW(GetFocus(), WM_KEYDOWN, vk, 1);
  if (vk == VK_SPACE) SendMessageW(GetFocus(), WM_CHAR, ' ', 1);
  if (vk == VK_RETURN) SendMessageW(GetFocus(), WM_CHAR, 13, 1);
  if (vk == VK_TAB) SendMessageW(GetFocus(), WM_CHAR, 9, 1);
  SendMessageW(GetFocus(), WM_KEYUP, vk, 0xC0000001);
  ks[VK_SHIFT] = 0; ks[VK_CONTROL] = 0; SetKeyboardState(ks);
}
/* The window's loads run in the background. Unless "async on", every key, character or drop then waits (up to 20 s,
   with the window's messages and the host's main-thread callbacks running) until the load it started is swapped in,
   so the other tests read what they always read. */
typedef int (*LoadingFn)(const clap_plugin_t *);
static int async_loads;
static void settle(const clap_plugin_t *p, int always) {
  LoadingFn loading = (LoadingFn)GetProcAddress(dll, "smp_test_plugin_loading");
  DWORD end = GetTickCount() + 20000;
  if ((async_loads && !always) || !loading || plugin_gone) return;
  while (loading(p) && (int)(end - GetTickCount()) > 0) { main_thread(p); pump(10); }
  main_thread(p);
}
static DWORD WINAPI watchdog(LPVOID arg) { (void)arg; Sleep(90000); printf("HUNG\n"); ExitProcess(3); return 0; }

static int cmd_gui(const clap_plugin_t *p, const char *state, const char *actions) {
  const clap_plugin_gui_t *gui = p->get_extension(p, CLAP_EXT_GUI);
  const clap_plugin_params_t *pa = p->get_extension(p, CLAP_EXT_PARAMS);
  BeepsFn beeps = (BeepsFn)GetProcAddress(dll, "smp_test_gui_beeps");
  QuietFn quiet = (QuietFn)GetProcAddress(dll, "smp_test_gui_quiet");
  DialogFn dialog = (DialogFn)GetProcAddress(dll, "smp_test_gui_dialog");
  char *text = read_file(actions, NULL), *line, *next;
  HWND top, holder; clap_window_t cw; uint32_t w = 0, h = 0;
  HDESK desk; int active = 0;
  if (!gui || !beeps || !quiet) { printf("error: no gui extension or test hooks\n"); return 1; }
  if (!text) { printf("error: can't read %s\n", actions); return 1; }
  /* The window lives on a desktop of its own, before this thread has any window: it can never become the
     foreground window, so it can't take the keyboard from whoever is using the real desktop (and a screen reader
     there never sees it) */
  desk = CreateDesktopW(L"samplosis_test", NULL, NULL, 0, GENERIC_ALL, NULL);
  if (!desk || !SetThreadDesktop(desk)) { printf("error: no desktop of its own (%lu)\n", GetLastError()); return 1; }
  CoInitialize(NULL);                        /* for the accessible objects (msaa) */
  CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
  quiet(1);
  if (load_state(p, state) < 0) return 1;
  main_thread(p);
  if (!gui->is_api_supported(p, CLAP_WINDOW_API_WIN32, false) || !gui->create(p, CLAP_WINDOW_API_WIN32, false)) {
    printf("error: gui create failed\n"); return 1;
  }
  gui->get_size(p, &w, &h);
  top = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW, L"#32770", L"FX: samplosis test", WS_POPUP, -3000, -3000,
                        (int)w + 120, (int)h + 20, NULL, NULL, NULL, NULL);
  CreateWindowExW(0, L"BUTTON", L"before", WS_CHILD | WS_VISIBLE | WS_TABSTOP, (int)w + 10, 10, 80, 26, top, (HMENU)1, NULL, NULL);
  holder = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, (int)w, (int)h, top, NULL, NULL, NULL);
  CreateWindowExW(0, L"BUTTON", L"after", WS_CHILD | WS_VISIBLE | WS_TABSTOP, (int)w + 10, 50, 80, 26, top, (HMENU)2, NULL, NULL);
  memset(&cw, 0, sizeof cw); cw.api = CLAP_WINDOW_API_WIN32; cw.win32 = holder;
  if (!gui->set_parent(p, &cw)) { printf("error: set_parent failed\n"); return 1; }
  gui->show(p);
  plugwin = FindWindowExW(holder, NULL, L"SamplosisWindow", NULL);
  if (!plugwin) { printf("error: no SamplosisWindow\n"); return 1; }
  ShowWindow(top, SW_SHOWNOACTIVATE);
  SetFocus(top);
  pump(400);
  for (line = text; line && *line; line = next) {
    char a[1100] = "", *arg;
    size_t n;
    next = strchr(line, '\n');
    if (next) *next++ = 0;
    snprintf(a, sizeof a, "%s", line);
    n = strlen(a);
    while (n && (a[n - 1] == '\r' || a[n - 1] == ' ')) a[--n] = 0;
    if (!a[0]) continue;
    arg = strchr(a, ' ');
    if (arg) *arg++ = 0; else arg = a + strlen(a);
    if (!strcmp(a, "key")) {
      static const struct { const char *n; WPARAM vk; } keys[] = {{"tab", VK_TAB}, {"up", VK_UP}, {"down", VK_DOWN},
        {"enter", VK_RETURN}, {"space", VK_SPACE}, {"escape", VK_ESCAPE}, {"left", VK_LEFT}, {"right", VK_RIGHT},
        {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"home", VK_HOME}, {"end", VK_END}, {"delete", VK_DELETE}};
      char k[32] = ""; int i, found = 0;
      sscanf(arg, "%31s", k);
      for (i = 0; i < (int)(sizeof keys / sizeof keys[0]); i++)
        if (!strcmp(k, keys[i].n)) { key(keys[i].vk, strstr(arg, "shift") != NULL, strstr(arg, "ctrl") != NULL); found = 1; }
      if (!found) { printf("error: no key %s\n", k); return 1; }
      pump(60); settle(p, 0); say();
    } else if (!strcmp(a, "char")) {
      SendMessageW(GetFocus(), WM_CHAR, (WPARAM)(unsigned char)arg[0], 1);
      pump(60); settle(p, 0); say();
    } else if (!strcmp(a, "say")) say();
    else if (!strcmp(a, "focus")) {
      HWND c = find_named(arg);
      if (c) { SetFocus(c); pump(30); } else printf("note: no control %s\n", arg);
    } else if (!strcmp(a, "window")) { SetFocus(plugwin); pump(60); }
    else if (!strcmp(a, "type")) {
      wchar_t wt[1100]; MultiByteToWideChar(CP_UTF8, 0, arg, -1, wt, 1100);
      SendMessageW(GetFocus(), WM_SETTEXT, 0, (LPARAM)wt); pump(30);
    } else if (!strcmp(a, "read")) {
      HWND c = find_named(arg); char cl[64], nm[300], v[1100];
      describe(c, cl, nm, v);
      printf("read: %s | %s\n", arg, c ? v : "(none)");
    } else if (!strcmp(a, "dump")) {
      HWND c;
      for (c = GetWindow(plugwin, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        char cl[64], nm[300], v[1100];
        if (!IsWindowVisible(c) || !(GetWindowLongW(c, GWL_STYLE) & WS_TABSTOP)) continue;
        describe(c, cl, nm, v);
        printf("control: %s | %s | %s\n", cl, nm, v);
      }
    } else if (!strcmp(a, "param")) {
      double v = 0; pa->get_value(p, (clap_id)atoi(arg), &v);
      printf("param %d: %g\n", atoi(arg), v);
    } else if (!strcmp(a, "flush")) {
      clap_input_events_t in; clap_output_events_t out; EvList none = {NULL, 0};
      in.ctx = &none; in.size = el_size; in.get = el_get;
      out.ctx = NULL; out.try_push = oe_print;
      pa->flush(p, &in, &out);
    } else if (!strcmp(a, "state")) {
      const clap_plugin_state_t *st = p->get_extension(p, CLAP_EXT_STATE);
      Mem m = {0}; clap_ostream_t os = {&m, os_write}; char *sl;
      st->save(p, &os);
      if (arg[0]) {                          /* every line starting with the word */
        size_t k = strlen(arg); int any = 0; char *q = m.buf;
        while (q && *q) {
          char *e = strchr(q, '\n');
          if (e) *e = 0;
          if (!strncmp(q, arg, k) && (q[k] == ' ' || !q[k])) { printf("state: %s\n", q); any = 1; }
          q = e ? e + 1 : NULL;
        }
        if (!any) printf("state: (none)\n");
      } else {
        sl = m.buf ? strstr(m.buf, "\nsample ") : NULL;
        if (sl) { char *e = strchr(sl + 1, '\n'); if (e) *e = 0; printf("state: %s\n", sl + 1); }
        else printf("state: no sample\n");
      }
      free(m.buf);
    } else if (!strcmp(a, "midi")) {        /* one block with this MIDI event at its start */
      static float l[BLOCK], r[BLOCK]; float *ch[2] = {l, r};
      clap_audio_buffer_t ob; clap_process_t pr; Ev e; const Ev *one[1]; EvList el = {one, 1};
      clap_input_events_t in = {&el, el_size, el_get}; clap_output_events_t oe = {NULL, oe_push};
      int st_ = 0, d1 = 0, d2 = 0;
      sscanf(arg, "%d %d %d", &st_, &d1, &d2);
      if (!active) {
        if (!p->activate(p, 48000.0, 1, BLOCK)) { printf("error: activate failed\n"); return 1; }
        p->start_processing(p); active = 1;
      }
      memset(&e, 0, sizeof e);
      e.kind = 1; e.midi.header.type = CLAP_EVENT_MIDI; e.midi.header.size = sizeof e.midi;
      e.midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      e.midi.data[0] = (uint8_t)st_; e.midi.data[1] = (uint8_t)d1; e.midi.data[2] = (uint8_t)d2;
      one[0] = &e;
      memset(&ob, 0, sizeof ob); ob.data32 = ch; ob.channel_count = 2;
      memset(&pr, 0, sizeof pr);
      pr.frames_count = BLOCK; pr.audio_outputs = &ob; pr.audio_outputs_count = 1; pr.in_events = &in; pr.out_events = &oe;
      p->process(p, &pr);
      main_thread(p);
    } else if (!strcmp(a, "dropdown")) {    /* open or close the focused combo box's list (silent) */
      SendMessageW(GetFocus(), CB_SHOWDROPDOWN, !strcmp(arg, "open"), 0); pump(60);
    } else if (!strcmp(a, "load")) {
      if (load_state(p, arg) < 0) return 1;
      main_thread(p); pump(30);
    } else if (!strcmp(a, "list")) {
      HWND c = find_named(arg); int lb = c && cls_is(c, L"ListBox");
      LRESULT i, n = c ? SendMessageW(c, lb ? LB_GETCOUNT : CB_GETCOUNT, 0, 0) : 0;
      printf("list: %s", arg);
      for (i = 0; i < n; i++) {
        wchar_t t[1100] = L""; char u[1100];
        if (lb) { if (SendMessageW(c, LB_GETTEXTLEN, (WPARAM)i, 0) < 1100) SendMessageW(c, LB_GETTEXT, (WPARAM)i, (LPARAM)t); }
        else if (SendMessageW(c, CB_GETLBTEXTLEN, (WPARAM)i, 0) < 1100) SendMessageW(c, CB_GETLBTEXT, (WPARAM)i, (LPARAM)t);
        utf8(t, u, sizeof u); printf(" | %s", u);
      }
      printf("\n");
    } else if (!strcmp(a, "drop")) {        /* files dropped on the window (several: a|b), as Explorer's WM_DROPFILES */
      wchar_t wp[4400]; size_t k, j; HGLOBAL hg; DROPFILES *df;
      MultiByteToWideChar(CP_UTF8, 0, arg, -1, wp, 4400);
      k = wcslen(wp);
      for (j = 0; j < k; j++) if (wp[j] == L'|') wp[j] = 0;   /* each name ends in a zero, the list in two */
      hg = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + (k + 2) * sizeof(wchar_t));
      if (!hg || !(df = GlobalLock(hg))) { printf("error: drop\n"); return 1; }
      df->pFiles = sizeof(DROPFILES); df->fWide = TRUE;
      memcpy((char *)df + sizeof(DROPFILES), wp, (k + 1) * sizeof(wchar_t));
      GlobalUnlock(hg);
      SendMessageW(plugwin, WM_DROPFILES, (WPARAM)hg, 0);   /* the window calls DragFinish, which frees it */
      pump(100); settle(p, 0);
    } else if (!strcmp(a, "counts")) printf("counts: beeps %ld | request_flush %d | mark_dirty %d\n", beeps(), flushes, dirties);
    else if (!strcmp(a, "pump")) pump(atoi(arg));
    else if (!strcmp(a, "dialog")) {     /* the next file or folder dialog: returns the path at once */
      char mode[32] = ""; int used = 0, act;
      sscanf(arg, "%31s %n", mode, &used);
      act = !strcmp(mode, "pick") ? 1 : !strcmp(mode, "closegui") ? 2 : !strcmp(mode, "destroy") ? 3 : 0;
      if (!act || !dialog) { printf("error: dialog %s\n", mode); return 1; }
      dialog(p, act, arg + used);
      if (act == 3) plugin_gone = 1;
    } else if (!strcmp(a, "alive")) printf("alive: %s\n", plugin_gone ? "plugin destroyed" : "plugin");
    else if (!strcmp(a, "async")) async_loads = !strcmp(arg, "on");
    else if (!strcmp(a, "wait")) settle(p, 1);
    else if (!strcmp(a, "status")) status_line(p);   /* the plugin's own status (what a window opened later shows) */
    else if (!strcmp(a, "ffsearches")) {
      typedef long (*CountFn)(void);
      CountFn fc = (CountFn)GetProcAddress(dll, "smp_test_ffmpeg_searches");
      printf("ffsearches: %ld\n", fc ? fc() : -1L);
    }
    else if (!strcmp(a, "clock")) {           /* milliseconds since the last clock */
      static DWORD last; DWORD now = GetTickCount();
      printf("clock: %lu ms\n", last ? now - last : 0); last = now;
    } else if (!strcmp(a, "pids")) {          /* of the process ids listed in a file, how many still run */
      FILE *pf = fopen(arg, "r"); unsigned long pid; int all = 0, running = 0;
      while (pf && fscanf(pf, "%lu", &pid) == 1) {
        HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, pid);
        all++;
        if (ph) { running += WaitForSingleObject(ph, 0) == WAIT_TIMEOUT; CloseHandle(ph); }
      }
      if (pf) fclose(pf);
      printf("pids: %d of %d running\n", running, all);
    }
    else { printf("error: unknown action %s\n", a); return 1; }
  }
  free(text);
  if (active && !plugin_gone) { p->stop_processing(p); p->deactivate(p); main_thread(p); }
  if (!plugin_gone) gui->destroy(p);
  DestroyWindow(top);
  if (!plugin_gone) main_thread(p);
  pump(300);                                 /* timers or posted messages of a window gone must not run into freed memory */
  printf("closed\n");
  return 0;
}

int main(int argc, char **argv) {
  const clap_plugin_entry_t *entry; const clap_plugin_factory_t *fac; const clap_plugin_t *p; int r;
  _setmode(_fileno(stdout), _O_BINARY);
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 4) { printf("usage: host.exe samplosis.clap state <state.txt> | play <state.txt> <events> <out.wav> [rate [all]] | gui <state.txt> <actions.txt>\n"); return 2; }
  dll = LoadLibraryA(argv[1]);
  if (!dll) { printf("error: can't load %s\n", argv[1]); return 1; }
  if (getenv("SAMPLOSIS_TEST_PAUSE_LOADER")) {   /* tests: the SoundFont loader loads nothing (a status while loading) */
    void (*pause)(int) = (void (*)(int))GetProcAddress(dll, "smp_sf_test_pause");
    if (pause) pause(1);
  }
  if (getenv("SAMPLOSIS_TEST_GRACE_MS")) {       /* tests: how soon an unneeded SoundFont sample is freed */
    void (*grace)(int) = (void (*)(int))GetProcAddress(dll, "smp_sf_test_grace");
    if (grace) grace(atoi(getenv("SAMPLOSIS_TEST_GRACE_MS")));
  }
  if (getenv("SAMPLOSIS_TEST_DECODE_FLAGS")) {   /* tests: smp_test_only_decode_flags (FFmpeg held back, ...) */
    void (*flags)(long) = (void (*)(long))GetProcAddress(dll, "smp_test_only_decode_flags");
    if (flags) flags(atol(getenv("SAMPLOSIS_TEST_DECODE_FLAGS")));
  }
  entry = (const clap_plugin_entry_t *)GetProcAddress(dll, "clap_entry");
  if (!entry || !entry->init(argv[1])) { printf("error: no clap_entry\n"); return 1; }
  fac = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
  if (!fac || fac->get_plugin_count(fac) != 1) { printf("error: no factory\n"); return 1; }
  p = fac->create_plugin(fac, &host, fac->get_plugin_descriptor(fac, 0)->id);
  if (!p || !p->init(p)) { printf("error: the plugin didn't start\n"); return 1; }
  if (!strcmp(argv[2], "state")) r = cmd_state(p, argv[3]);
  else if (!strcmp(argv[2], "gui") && argc >= 5) r = cmd_gui(p, argv[3], argv[4]);
  else if (!strcmp(argv[2], "play") && argc >= 6)
    r = cmd_play(p, argv[3], argv[4], argv[5], argc > 6 ? atof(argv[6]) : 48000.0, argc > 7 && !strcmp(argv[7], "all"));
  else { printf("unknown command %s\n", argv[2]); r = 2; }
  if (!plugin_gone) p->destroy(p);
  entry->deinit();
  free(evs);
  return r;
}
