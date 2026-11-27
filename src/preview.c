/* The file browser's preview: the sound under the cursor plays through the plugin's main output, at its own pitch,
   while the host processes (REAPER does with the transport stopped). A worker thread decodes it (a newer request
   cancels a decode under way); the decoded sound goes to the audio thread through pv_req, and the audio thread
   hands back the one it let go through pv_old, which the main thread frees. Nothing is freed on the audio thread. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "engine_int.h"
#include "plugin_int.h"
#include "decode.h"

typedef struct Preview { float *d; int ch; long frames; double pos, step; } Preview;
#define PV_STOP ((Preview *)(INT_PTR)1)       /* in pv_req: silence */

struct PvWorker {
  HANDLE thread, wake; CRITICAL_SECTION lock;
  char *path;                                 /* the next file to decode, or NULL */
  volatile LONG gen;                          /* goes up with each request: a decode for an older one stops */
  volatile LONG quit;
};

static void pv_free(Preview *v) { if (v && v != PV_STOP) { free(v->d); free(v); } }

/* the next preview (or PV_STOP) for the audio thread; one it never took is freed */
static void post(Plug *P, Preview *v) { pv_free(InterlockedExchangePointer((PVOID volatile *)&P->pv_req, v)); }

typedef struct { struct PvWorker *w; LONG gen; } Job;
static int cancelled(void *a) { const Job *j = a; return j->w->gen != j->gen || j->w->quit; }

static DWORD WINAPI worker(void *arg) {
  Plug *P = arg; struct PvWorker *w = P->pv;
  for (;;) {
    char *path, err[300]; Job j; float *d; int ch = 0, rate = 0; long frames = 0;
    WaitForSingleObject(w->wake, INFINITE);
    if (w->quit) break;
    EnterCriticalSection(&w->lock);
    path = w->path; w->path = NULL; j.w = w; j.gen = w->gen;
    LeaveCriticalSection(&w->lock);
    if (!path) continue;
    d = decode_audio_ex(path, &ch, &frames, &rate, NULL, cancelled, &j, err, sizeof err);
    free(path);
    if (d && frames > 1 && ch > 0 && rate > 0 && !cancelled(&j) && P->rate > 0) {
      Preview *v = calloc(1, sizeof *v);
      if (v) {
        v->d = d; v->ch = ch; v->frames = frames; v->step = rate / P->rate; d = NULL;
        EnterCriticalSection(&w->lock);       /* not past a newer request's stop */
        if (j.gen == w->gen) post(P, v); else pv_free(v);
        LeaveCriticalSection(&w->lock);
      }
    }
    free(d);
  }
  return 0;
}

/* main thread: preview path (UTF-8), or stop with NULL. 0 when the worker couldn't start */
int smp_plug_preview(Plug *P, const char *path) {
  struct PvWorker *w = P->pv;
  smp_plug_preview_collect(P);                /* the audio thread takes the next one only once this is free */
  if (!w && path) {
    if (!(w = calloc(1, sizeof *w))) return 0;
    InitializeCriticalSection(&w->lock);
    w->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    P->pv = w;
    w->thread = w->wake ? CreateThread(NULL, 0, worker, P, 0, NULL) : NULL;
    if (!w->thread) { smp_plug_preview_free(P); return 0; }
  }
  if (!w) { post(P, PV_STOP); return 1; }
  EnterCriticalSection(&w->lock);
  InterlockedIncrement(&w->gen);
  free(w->path); w->path = path ? _strdup(path) : NULL;
  post(P, PV_STOP);                           /* the one playing stops now, not when the next is decoded */
  LeaveCriticalSection(&w->lock);
  if (path) SetEvent(w->wake);
  return 1;
}

/* main thread: frees what the audio thread let go; with nothing processing (deactivated), everything */
void smp_plug_preview_collect(Plug *P) {
  pv_free(InterlockedExchangePointer((PVOID volatile *)&P->pv_old, NULL));
  if (!P->active) {
    pv_free(InterlockedExchangePointer((PVOID volatile *)&P->pv_req, NULL));
    pv_free(P->pv_cur); P->pv_cur = NULL;
  }
}

/* main thread, plugin destroyed: the worker stops (its decode is cancelled) and everything is freed */
void smp_plug_preview_free(Plug *P) {
  struct PvWorker *w = P->pv;
  if (w) {
    InterlockedExchange(&w->quit, 1); InterlockedIncrement(&w->gen);
    if (w->thread) { SetEvent(w->wake); WaitForSingleObject(w->thread, INFINITE); CloseHandle(w->thread); }
    if (w->wake) CloseHandle(w->wake);
    DeleteCriticalSection(&w->lock);
    free(w->path); free(w);
    P->pv = NULL;
  }
  pv_free(InterlockedExchangePointer((PVOID volatile *)&P->pv_old, NULL));
  pv_free(InterlockedExchangePointer((PVOID volatile *)&P->pv_req, NULL));
  pv_free(P->pv_cur); P->pv_cur = NULL;
}

/* audio thread: takes up a new request (once the main thread has freed the last one let go) and adds the preview
   into l and r (r may be NULL: a mono output) */
void smp_plug_preview_mix(Plug *P, float *l, float *r, uint32_t n) {
  Preview *v; uint32_t i;
  if (!P->pv_old && P->pv_req) {
    Preview *q = InterlockedExchangePointer((PVOID volatile *)&P->pv_req, NULL);
    if (q) {
      if (P->pv_cur) InterlockedExchangePointer((PVOID volatile *)&P->pv_old, P->pv_cur);
      P->pv_cur = q == PV_STOP ? NULL : q;
    }
  }
  if (!(v = P->pv_cur) || !l) return;
  for (i = 0; i < n; i++) {
    long k = (long)v->pos; float f, a, b;
    if (k + 1 >= v->frames) break;
    f = (float)(v->pos - k);
    a = v->d[k] + f * (v->d[k + 1] - v->d[k]);
    if (v->ch > 1) { const float *c = v->d + v->frames; b = c[k] + f * (c[k + 1] - c[k]); } else b = a;
    if (r) { l[i] += a; r[i] += b; } else l[i] += 0.5f * (a + b);
    v->pos += v->step;
  }
}

/* 1 while a preview is playing or about to (the tests) */
int smp_plug_previewing(Plug *P) {
  const Preview *v = P->pv_cur;
  return (P->pv_req && P->pv_req != PV_STOP) || (v && (long)v->pos + 1 < v->frames);
}
