/* The instrument as plain text. See samplosis.h. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "samplosis.h"

/* ---- writer: snprintf-style appends that keep counting past the end ---- */
typedef struct { char *out; int cap; int n; } Out;

static void put(Out *o, const char *s) {
  size_t l = strlen(s);
  if (o->out && o->cap > 0 && (size_t)o->n < (size_t)o->cap) {
    size_t room = (size_t)o->cap - 1 - (size_t)o->n;
    size_t c = l < room ? l : room;
    memcpy(o->out + o->n, s, c);
    o->out[o->n + (int)c] = 0;
  }
  o->n += (int)l;
}

SMP_API int smp_script_write(const SmpEngine *e, char *out, int cap) {
  Out o = {out, cap, 0};
  char line[1400];
  const SmpSample *s;
  if (out && cap > 0) out[0] = 0;
  put(&o, "# Samplosis 0.1.0\n");
  s = e ? smp_engine_sample(e) : NULL;
  if (s) {
    snprintf(line, sizeof line, "sample \"%s\" key=%016llx\n", s->path, (unsigned long long)s->src_key);
    put(&o, line);
  }
  for (int i = 0; i < SMP_NPARAMS; i++) {
    char v[64];
    smp_value_text(i, e ? smp_engine_get_param(e, i) : smp_param_info(i)->def, v, sizeof v);
    snprintf(line, sizeof line, "%s %s\n", smp_param_info(i)->name, v);
    put(&o, line);
  }
  return o.n;
}

/* ---- reader ---- */
static void fail(char *err, int cap, int line, const char *fmt, const char *a) {
  char m[600];
  int n = snprintf(m, sizeof m, "line %d: ", line);
  snprintf(m + n, sizeof m - (size_t)n, fmt, a);
  if (err && cap > 0) snprintf(err, (size_t)cap, "%s", m);
}

static int is_hex16(const char *s) {
  for (int i = 0; i < 16; i++) if (!isxdigit((unsigned char)s[i])) return 0;
  return s[16] == 0 || isspace((unsigned char)s[16]);
}

SMP_API int smp_script_apply(SmpEngine *e, const char *text, char *err, int cap) {
  double val[SMP_NPARAMS]; char have[SMP_NPARAMS] = {0};
  char path[1024] = ""; uint64_t key = 0; int has_sample = 0, has_key = 0, sample_line = 0;
  const char *p = text;
  int lineno = 0;
  if (err && cap > 0) err[0] = 0;
  if (!e || !text) { if (err && cap > 0) snprintf(err, (size_t)cap, "no instrument or no text"); return 1; }
  while (*p) {
    const char *eol = p; char buf[1100]; size_t len;
    char *s, *name, *rest;
    while (*eol && *eol != '\n') eol++;
    len = (size_t)(eol - p);
    lineno++;
    if (len >= sizeof buf) { fail(err, cap, lineno, "%s", "the line is too long"); return 1; }
    memcpy(buf, p, len); buf[len] = 0;
    p = *eol ? eol + 1 : eol;
    s = buf;
    while (isspace((unsigned char)*s)) s++;
    len = strlen(s);
    while (len && isspace((unsigned char)s[len - 1])) s[--len] = 0;
    if (!*s || *s == '#') continue;
    name = s;
    rest = s;
    while (*rest && !isspace((unsigned char)*rest)) rest++;
    if (*rest) { *rest++ = 0; while (isspace((unsigned char)*rest)) rest++; }
    if (!strcmp(name, "sample")) {
      char *q, *end;
      if (*rest != '"') { fail(err, cap, lineno, "sample needs the path in quotes, like %s", "sample \"E:\\samples\\a.wav\""); return 1; }
      q = rest + 1;
      end = strchr(q, '"');
      if (!end) { fail(err, cap, lineno, "sample: %s", "the closing quote is missing"); return 1; }
      *end = 0;
      if (!*q) { fail(err, cap, lineno, "sample: %s", "the path is empty"); return 1; }
      snprintf(path, sizeof path, "%s", q);
      rest = end + 1;
      while (isspace((unsigned char)*rest)) rest++;
      has_key = 0;
      if (*rest) {
        if (strncmp(rest, "key=", 4) || !is_hex16(rest + 4)) {
          fail(err, cap, lineno, "sample: expected key= and 16 hex digits, got '%s'", rest); return 1;
        }
        key = strtoull(rest + 4, NULL, 16); has_key = 1;
        rest += 20;
        while (isspace((unsigned char)*rest)) rest++;
        if (*rest) { fail(err, cap, lineno, "sample: unexpected '%s' after the key", rest); return 1; }
      }
      has_sample = 1; sample_line = lineno;
      continue;
    }
    {
      int id = -1;
      for (int i = 0; i < SMP_NPARAMS; i++) if (!strcmp(name, smp_param_info(i)->name)) { id = i; break; }
      if (id < 0) { fail(err, cap, lineno, "unknown setting '%s'", name); return 1; }
      if (!*rest) { fail(err, cap, lineno, "%s needs a value", name); return 1; }
      if (!smp_value_parse(id, rest, &val[id])) {
        char m[700]; snprintf(m, sizeof m, "%s: can't read '%s'", name, rest);
        fail(err, cap, lineno, "%s", m); return 1;
      }
      have[id] = 1;
    }
  }
  /* every line is good; load the sample before touching the engine */
  {
    SmpSample *ns = NULL; char msg[512] = "";
    const SmpSample *have_s = smp_engine_sample(e);
    /* the sample already loaded (same path and key): keep it, so re-applied state doesn't restart anything */
    if (has_sample && has_key && have_s && have_s->src_key == key && !strcmp(have_s->path, path)) has_sample = 0;
    if (has_sample) {
      ns = smp_sample_load(path, msg, sizeof msg);
      if (!ns && has_key) { msg[0] = 0; ns = smp_sample_load_known(path, key, have[SMP_P_ROOT] ? (int)val[SMP_P_ROOT] : 60, msg, sizeof msg); }
      if (!ns) {
        char m[700]; snprintf(m, sizeof m, "sample: %s", msg[0] ? msg : "can't load it");
        fail(err, cap, sample_line, "%s", m);
        return 1;
      }
    }
    {
      const SmpSample *cur = ns ? ns : smp_engine_sample(e);
      double root = have[SMP_P_ROOT] ? val[SMP_P_ROOT] : cur ? cur->root : smp_param_info(SMP_P_ROOT)->def;
      if (ns) smp_engine_set_sample(e, ns);       /* sets root to the sample's; fixed below */
      for (int i = 0; i < SMP_NPARAMS; i++)
        smp_engine_set_param(e, i, i == SMP_P_ROOT ? root : have[i] ? val[i] : smp_param_info(i)->def);
    }
  }
  return 0;
}
