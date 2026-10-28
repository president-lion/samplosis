#ifndef SAMPLOSIS_H
#define SAMPLOSIS_H
#include <stdint.h>
#ifdef _WIN32
#define SMP_API __declspec(dllexport)
#else
#define SMP_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
SMP_API const char *smp_version(void);

/* parameter ids (also the REAPER/CLAP parameter ids) */
enum {
  SMP_P_ROOT = 0, SMP_P_TRANSPOSE = 1, SMP_P_FINE = 2, SMP_P_VOLUME = 3, SMP_P_PAN = 4,
  SMP_P_ATTACK = 5, SMP_P_DECAY = 6, SMP_P_SUSTAIN = 7, SMP_P_RELEASE = 8,
  SMP_P_VELOCITY_SENS = 9, SMP_P_MIN_VOLUME = 10, SMP_P_MODE = 11,
  SMP_P_NOTE_LO = 12, SMP_P_NOTE_HI = 13, SMP_P_PITCH_LO = 14, SMP_P_VEL_LO = 15, SMP_P_VEL_HI = 16,
  SMP_P_OBEY_NOTE_OFF = 17, SMP_P_LOOP = 18, SMP_P_LOOP_START = 19, SMP_P_START = 20, SMP_P_END = 21,
  SMP_P_BEND_RANGE = 22, SMP_P_PITCH_HI = 23, SMP_P_REVERSE = 24, SMP_P_KEEP_LENGTH = 25,
  SMP_P_VOICES = 26, SMP_P_PROBABILITY = 27, SMP_P_SPEED = 28
};
#define SMP_NPARAMS 29

/* how a parameter reads in words */
enum { SMP_K_NOTE, SMP_K_SEMI, SMP_K_CENT, SMP_K_DB, SMP_K_PAN, SMP_K_MS, SMP_K_PERCENT,
       SMP_K_CHOICE, SMP_K_BOOL, SMP_K_COUNT, SMP_K_POSITION };

typedef struct { const char *name; double lo, hi, def; int kind; } SmpParamInfo;

SMP_API const SmpParamInfo *smp_param_info(int id); /* NULL when id is out of range */

SMP_API int smp_note_parse(const char *s);                  /* C-1 = 0, C4 = 60; -1 if not a note */
SMP_API void smp_note_name(int n, char *out, int cap);      /* sharps: 61 = "C#4" */
SMP_API int smp_note_from_filename(const char *path);       /* last note-like token of the base name, or -1 */
SMP_API double smp_detect_pitch(const float *mono, long n, int rate); /* fractional MIDI note, or -1 */
SMP_API int smp_value_text(int param, double v, char *out, int cap);  /* returns length written */
SMP_API int smp_value_parse(int param, const char *s, double *v);    /* 1 ok (clamped to range), 0 fail */
#ifdef __cplusplus
}
#endif
#endif
