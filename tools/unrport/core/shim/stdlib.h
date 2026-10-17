#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* malloc(size_t n);
void* realloc(void* p, size_t n);
void* calloc(size_t n, size_t m);
void free(void* p);
int atoi(const char* s);
double atof(const char* s);
long atol(const char* s);
unsigned long strtoul(const char* s, char** end, int base);
int rand(void);
void srand(unsigned seed);
void qsort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*));
void abort(void);
void exit(int code);
#define RAND_MAX 0x7FFF
#ifdef __cplusplus
}
#endif
