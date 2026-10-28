/* A stand-in CLAP host for the tests: loads samplosis.clap through clap_entry and prints plain lines.
     host.exe samplosis.clap state <state.txt>                     load the state, save it, print it
     host.exe samplosis.clap play <state.txt> <events> <out.wav> [rate]
   Events, one per line: "on key vel frame", "off key frame" (CLAP notes), "midi status d1 d2 frame",
   "bend fraction frame" (a MIDI pitch bend), "param id value frame", "length frames". Blocks of 256 frames.
   play makes the real-time plugin render exactly, like smp_engine_render_test, through its smp_test_* hooks. */
#include <windows.h>
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

static volatile LONG want_callback;
static const void *h_ext(const clap_host_t *h, const char *id) { (void)h; (void)id; return NULL; }
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
    printf("note ports: %u in,%s%s\n", np->count(p, true), ni.supported_dialects & CLAP_NOTE_DIALECT_CLAP ? " clap" : "",
           ni.supported_dialects & CLAP_NOTE_DIALECT_MIDI ? " midi" : "");
  }
  {
    clap_audio_port_info_t ai; memset(&ai, 0, sizeof ai);
    ap->get(p, 0, false, &ai);
    printf("audio ports: %u out, %u channels\n", ap->count(p, false), ai.channel_count);
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

typedef struct { long frame; clap_event_param_value_t pv; clap_event_note_t note; clap_event_midi_t midi; int kind; } Ev;
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

static int parse_events(const char *text, long *length) {
  const char *p = text; int cap = 0;
  *length = -1;
  while (p && *p) {
    char w[16]; int used = 0, ok = 0; Ev e; const char *nl = strchr(p, '\n');
    memset(&e, 0, sizeof e);
    if (sscanf(p, "%15s%n", w, &used) == 1) {
      int a, b, c; double x;
      if (!strcmp(w, "length")) { if (sscanf(p + used, "%ld", length) != 1) return 0; p = nl ? nl + 1 : NULL; continue; }
      if (!strcmp(w, "on") && sscanf(p + used, "%d %d %ld", &a, &b, &e.frame) == 3) {
        e.kind = 0; e.note.header.type = CLAP_EVENT_NOTE_ON; e.note.key = (int16_t)a; e.note.velocity = b / 127.0; ok = 1;
      } else if (!strcmp(w, "off") && sscanf(p + used, "%d %ld", &a, &e.frame) == 2) {
        e.kind = 0; e.note.header.type = CLAP_EVENT_NOTE_OFF; e.note.key = (int16_t)a; ok = 1;
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
      }
      if (nev == cap) { cap = cap ? cap * 2 : 64; evs = realloc(evs, sizeof *evs * (size_t)cap); if (!evs) return 0; }
      evs[nev++] = e;
    }
    p = nl ? nl + 1 : NULL;
  }
  { int i, j; for (i = 1; i < nev; i++) { Ev x = evs[i]; for (j = i - 1; j >= 0 && evs[j].frame > x.frame; j--) evs[j + 1] = evs[j]; evs[j + 1] = x; } }
  return 1;
}

static void wav_header(FILE *f, uint32_t frames, uint32_t sr) {
  uint32_t data = frames * 8, riff = 36 + data, br = sr * 8, fmt = 16;
  uint16_t tag = 3, ch = 2, align = 8, bits = 32;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
  fwrite(&tag, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
  fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

static int cmd_play(const clap_plugin_t *p, const char *state, const char *events, const char *out, double rate) {
  WaitFn wait = (WaitFn)GetProcAddress(dll, "smp_test_plugin_wait");
  OfflineFn offline = (OfflineFn)GetProcAddress(dll, "smp_test_plugin_offline");
  SyncFn sync = (SyncFn)GetProcAddress(dll, "smp_test_plugin_sync");
  char *text = read_file(events, NULL); long length, b; int k = 0;
  float l[BLOCK], r[BLOCK], lr[2 * BLOCK]; float *chans[2] = {l, r};
  const Ev *list[1024]; EvList el; FILE *f;
  clap_input_events_t in = {&el, el_size, el_get};
  clap_output_events_t oe = {NULL, oe_push};
  clap_audio_buffer_t ob; clap_process_t pr;
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
  wav_header(f, (uint32_t)length, (uint32_t)(rate + 0.5));
  memset(&ob, 0, sizeof ob); ob.data32 = chans; ob.channel_count = 2;
  memset(&pr, 0, sizeof pr);
  pr.steady_time = 0; pr.audio_outputs = &ob; pr.audio_outputs_count = 1; pr.in_events = &in; pr.out_events = &oe;
  for (b = 0; b < length; b += BLOCK) {
    uint32_t n = (uint32_t)(length - b < BLOCK ? length - b : BLOCK), i;
    el.list = list; el.n = 0;
    for (; k < nev && evs[k].frame < b + (long)n && el.n < 1024; k++) {
      clap_event_header_t *h = (clap_event_header_t *)ev_header(&evs[k]);
      h->time = (uint32_t)(evs[k].frame < b ? 0 : evs[k].frame - b);
      list[el.n++] = &evs[k];
    }
    pr.frames_count = n; pr.steady_time = b;
    p->process(p, &pr);
    for (i = 0; i < n; i++) { lr[2 * i] = l[i]; lr[2 * i + 1] = r[i]; }
    fwrite(lr, 8, n, f);
    sync(p);
    main_thread(p);
  }
  fclose(f);
  p->stop_processing(p);
  offline(p, 0);
  p->deactivate(p);
  main_thread(p);
  printf("frames: %ld\n", length);
  return 0;
}

int main(int argc, char **argv) {
  const clap_plugin_entry_t *entry; const clap_plugin_factory_t *fac; const clap_plugin_t *p; int r;
  _setmode(_fileno(stdout), _O_BINARY);
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 4) { printf("usage: host.exe samplosis.clap state <state.txt> | play <state.txt> <events> <out.wav> [rate]\n"); return 2; }
  dll = LoadLibraryA(argv[1]);
  if (!dll) { printf("error: can't load %s\n", argv[1]); return 1; }
  entry = (const clap_plugin_entry_t *)GetProcAddress(dll, "clap_entry");
  if (!entry || !entry->init(argv[1])) { printf("error: no clap_entry\n"); return 1; }
  fac = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
  if (!fac || fac->get_plugin_count(fac) != 1) { printf("error: no factory\n"); return 1; }
  p = fac->create_plugin(fac, &host, fac->get_plugin_descriptor(fac, 0)->id);
  if (!p || !p->init(p)) { printf("error: the plugin didn't start\n"); return 1; }
  if (!strcmp(argv[2], "state")) r = cmd_state(p, argv[3]);
  else if (!strcmp(argv[2], "play") && argc >= 6) r = cmd_play(p, argv[3], argv[4], argv[5], argc > 6 ? atof(argv[6]) : 48000.0);
  else { printf("unknown command %s\n", argv[2]); r = 2; }
  p->destroy(p);
  entry->deinit();
  free(evs);
  return r;
}
