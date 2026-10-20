/* Freestanding stdio.h: only what the psxrecomp sources mention. Output is dropped or goes to the Lion log ring. */
#ifndef PSXLION_STDIO_H
#define PSXLION_STDIO_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct lion_file FILE;
extern FILE *stderr;
int snprintf(char *, size_t, const char *, ...);
int fprintf(FILE *, const char *, ...);
#ifdef __cplusplus
}
#endif
#endif
