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
#ifdef __cplusplus
}
#endif
#endif
