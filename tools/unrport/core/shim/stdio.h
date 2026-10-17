// Minimal stdio for the Lion: files come from the RAM disc (see lionrt.cpp).
#pragma once
#include <stddef.h>
#include <stdarg.h>
#ifndef _FILE_DEFINED
#define _FILE_DEFINED
typedef struct _iobuf FILE;
#endif
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#ifdef __cplusplus
extern "C" {
#endif
FILE* fopen(const char* path, const char* mode);
int fclose(FILE* f);
size_t fread(void* p, size_t size, size_t n, FILE* f);
size_t fwrite(const void* p, size_t size, size_t n, FILE* f);
int fseek(FILE* f, long off, int whence);
long ftell(FILE* f);
int fflush(FILE* f);
int ferror(FILE* f);
int vsprintf(char* out, const char* fmt, va_list ap);
int sprintf(char* out, const char* fmt, ...);
int vsnprintf(char* out, size_t n, const char* fmt, va_list ap);
#ifdef __cplusplus
}
#endif
