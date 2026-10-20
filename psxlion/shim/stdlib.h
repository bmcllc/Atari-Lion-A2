#ifndef PSXLION_STDLIB_H
#define PSXLION_STDLIB_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int atoi(const char *);
long atol(const char *);
unsigned long strtoul(const char *, char **, int);
void *calloc(size_t, size_t);
void *malloc(size_t);
void *realloc(void *, size_t);
void free(void *);
void exit(int);
char *getenv(const char *);
static inline int abs(int x) { return x < 0 ? -x : x; }
#ifdef __cplusplus
}
#endif
#endif
