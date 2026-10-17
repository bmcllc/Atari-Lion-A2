// Freestanding runtime for Unreal's Core on the Lion: libc subset, heap, RAM-disc files,
// exception-ABI stubs (a throw halts) and static-constructor support.
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <direct.h>

typedef unsigned int u32;
typedef unsigned char u8;

#define REG(a) (*(volatile u32 *)(a))
enum : u32 { kHeapStart = 0x00A00000, kHeapEnd = 0x01600000, kRamDisc = 0x00400000, kRamDiscMagic = 0x4B534452 };

extern "C" {

// ---- halt / panic ---------------------------------------------------------------------------
const char* g_panic = 0;
void lion_halt(const char* why) {
  g_panic = why;
  // Panic screen: red 320x240 frame on the SGRAM so a stuck boot is visible.
  volatile unsigned short* fb = (volatile unsigned short*)0x10000000;
  for (int i = 0; i < 320 * 240; ++i) fb[i] = 0xF800;
  REG(0x1F030000) = 1, REG(0x1F030004) = 0x10000000, REG(0x1F030008) = 640;
  for (;;) {}
}
void abort(void) { lion_halt("abort"); }
void exit(int) { lion_halt("exit"); }

// ---- memory ---------------------------------------------------------------------------------
void* memcpy(void* d, const void* s, size_t n) {
  u8* a = (u8*)d; const u8* b = (const u8*)s;
  while (n--) *a++ = *b++;
  return d;
}
void* memmove(void* d, const void* s, size_t n) {
  u8* a = (u8*)d; const u8* b = (const u8*)s;
  if (a < b) { while (n--) *a++ = *b++; }
  else { a += n; b += n; while (n--) *--a = *--b; }
  return d;
}
void* memset(void* d, int c, size_t n) {
  u8* a = (u8*)d;
  while (n--) *a++ = (u8)c;
  return d;
}
int memcmp(const void* x, const void* y, size_t n) {
  const u8 *a = (const u8*)x, *b = (const u8*)y;
  for (; n; --n, ++a, ++b) if (*a != *b) return *a - *b;
  return 0;
}

// Size-class allocator: 16-byte header, power-of-two classes with free lists, bump arena.
struct Hdr { u32 cls; u32 size; u32 pad[2]; };
static u32 g_bump = kHeapStart;
static Hdr* g_free[32];
void* malloc(size_t n) {
  u32 cls = 4;
  while ((1u << cls) < n) ++cls;
  if (g_free[cls]) {
    Hdr* h = g_free[cls];
    g_free[cls] = *(Hdr**)(h + 1);
    h->size = (u32)n;
    return h + 1;
  }
  u32 total = (1u << cls) + sizeof(Hdr);
  if (g_bump + total > kHeapEnd) lion_halt("out of memory");
  Hdr* h = (Hdr*)g_bump;
  g_bump += total;
  h->cls = cls;
  h->size = (u32)n;
  return h + 1;
}
void free(void* p) {
  if (!p) return;
  Hdr* h = (Hdr*)p - 1;
  *(Hdr**)p = g_free[h->cls];
  g_free[h->cls] = h;
}
void* realloc(void* p, size_t n) {
  if (!p) return malloc(n);
  Hdr* h = (Hdr*)p - 1;
  if (n <= (1u << h->cls)) { h->size = (u32)n; return p; }
  void* q = malloc(n);
  memcpy(q, p, h->size);
  free(p);
  return q;
}
void* calloc(size_t n, size_t m) {
  void* p = malloc(n * m);
  memset(p, 0, n * m);
  return p;
}

// ---- strings --------------------------------------------------------------------------------
size_t strlen(const char* s) { const char* p = s; while (*p) ++p; return (size_t)(p - s); }
char* strcpy(char* d, const char* s) { char* r = d; while ((*d++ = *s++)) {} return r; }
char* strncpy(char* d, const char* s, size_t n) {
  size_t i = 0;
  for (; i < n && s[i]; ++i) d[i] = s[i];
  for (; i < n; ++i) d[i] = 0;
  return d;
}
char* strcat(char* d, const char* s) { strcpy(d + strlen(d), s); return d; }
int strcmp(const char* a, const char* b) {
  while (*a && *a == *b) ++a, ++b;
  return (u8)*a - (u8)*b;
}
int strncmp(const char* a, const char* b, size_t n) {
  for (; n; --n, ++a, ++b) {
    if (*a != *b) return (u8)*a - (u8)*b;
    if (!*a) return 0;
  }
  return 0;
}
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int stricmp(const char* a, const char* b) {
  while (*a && lower((u8)*a) == lower((u8)*b)) ++a, ++b;
  return lower((u8)*a) - lower((u8)*b);
}
int strnicmp(const char* a, const char* b, size_t n) {
  for (; n; --n, ++a, ++b) {
    if (lower((u8)*a) != lower((u8)*b)) return lower((u8)*a) - lower((u8)*b);
    if (!*a) return 0;
  }
  return 0;
}
char* strchr(const char* s, int c) {
  for (;; ++s) {
    if (*s == (char)c) return (char*)s;
    if (!*s) return 0;
  }
}
char* strrchr(const char* s, int c) {
  const char* r = 0;
  for (;; ++s) {
    if (*s == (char)c) r = s;
    if (!*s) return (char*)r;
  }
}
char* strstr(const char* h, const char* n) {
  size_t m = strlen(n);
  for (; *h; ++h) if (!strncmp(h, n, m)) return (char*)h;
  return m ? 0 : (char*)h;
}
char* strupr(char* s) {
  for (char* p = s; *p; ++p) if (*p >= 'a' && *p <= 'z') *p -= 32;
  return s;
}
unsigned long strtoul(const char* s, char** end, int base) {
  unsigned long v = 0;
  if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  for (;; ++s) {
    int d = *s >= '0' && *s <= '9' ? *s - '0' : lower(*s) >= 'a' && lower(*s) <= 'f' ? lower(*s) - 'a' + 10 : 99;
    if (d >= base) break;
    v = v * (unsigned long)base + (unsigned long)d;
  }
  if (end) *end = (char*)s;
  return v;
}
long atol(const char* s) {
  while (*s == ' ') ++s;
  bool neg = *s == '-';
  if (neg || *s == '+') ++s;
  long v = (long)strtoul(s, 0, 10);
  return neg ? -v : v;
}
int atoi(const char* s) { return (int)atol(s); }
double atof(const char* s) {
  while (*s == ' ') ++s;
  bool neg = *s == '-';
  if (neg || *s == '+') ++s;
  double v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  if (*s == '.') {
    double f = 0.1;
    for (++s; *s >= '0' && *s <= '9'; ++s, f *= 0.1) v += (*s - '0') * f;
  }
  return neg ? -v : v;
}
static u32 g_rand = 12345;
int rand(void) { g_rand = g_rand * 1103515245u + 12345u; return (int)((g_rand >> 16) & 0x7FFF); }
void srand(unsigned s) { g_rand = s; }
void qsort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*)) {
  u8* a = (u8*)base;  // insertion sort: only used on tiny arrays
  u8 tmp[64];
  for (size_t i = 1; i < n && size <= sizeof(tmp); ++i) {
    memcpy(tmp, a + i * size, size);
    size_t j = i;
    for (; j > 0 && cmp(a + (j - 1) * size, tmp) > 0; --j) memcpy(a + j * size, a + (j - 1) * size, size);
    memcpy(a + j * size, tmp, size);
  }
}

// ---- math -----------------------------------------------------------------------------------
double fabs(double x) { return x < 0 ? -x : x; }
double floor(double x) {
  if (x > 2147483647.0 || x < -2147483647.0) return x;
  double t = (double)(int)x;
  return t > x ? t - 1 : t;
}
double ceil(double x) { double f = floor(x); return f < x ? f + 1 : f; }
double fmod(double x, double y) { return y == 0 ? 0 : x - floor(x / y) * y; }
double sqrt(double x) {
  if (x <= 0) return 0;
  double g = x > 1 ? x * 0.5 : 1.0;
  for (int i = 0; i < 40; ++i) g = 0.5 * (g + x / g);
  return g;
}
double sin(double x) {
  const double pi = 3.14159265358979, two_pi = 6.28318530717959;
  x -= floor(x / two_pi) * two_pi;
  if (x > pi) x -= two_pi;
  double r = x, term = x, x2 = x * x;
  for (int i = 1; i < 12; ++i) { term *= -x2 / ((2 * i) * (2 * i + 1)); r += term; }
  return r;
}
double cos(double x) { return sin(x + 1.57079632679490); }
double tan(double x) { return sin(x) / cos(x); }
double atan(double x) {
  bool neg = x < 0, inv = fabs(x) > 1;
  double a = fabs(x);
  if (inv) a = 1 / a;
  double a2 = a * a, r = a, term = a;
  for (int i = 1; i < 40; ++i) { term *= -a2; r += term / (2 * i + 1); }
  if (inv) r = 1.57079632679490 - r;
  return neg ? -r : r;
}
double atan2(double y, double x) {
  const double pi = 3.14159265358979;
  if (x > 0) return atan(y / x);
  if (x < 0) return y >= 0 ? atan(y / x) + pi : atan(y / x) - pi;
  return y > 0 ? pi / 2 : y < 0 ? -pi / 2 : 0;
}
double exp(double x) {
  int k = (int)floor(x / 0.693147180559945);
  double r = x - k * 0.693147180559945, term = 1, s = 1;
  for (int i = 1; i < 20; ++i) { term *= r / i; s += term; }
  while (k > 0) { s *= 2; --k; }
  while (k < 0) { s *= 0.5; ++k; }
  return s;
}
double log(double x) {
  if (x <= 0) return -1e300;
  int k = 0;
  while (x > 1.5) { x *= 0.5; ++k; }
  while (x < 0.75) { x *= 2; --k; }
  double y = (x - 1) / (x + 1), y2 = y * y, r = 0, t = y;
  for (int i = 0; i < 30; ++i, t *= y2) r += t / (2 * i + 1);
  return 2 * r + k * 0.693147180559945;
}
double pow(double x, double y) {
  if (y == 0) return 1;
  if (x == 0) return 0;
  if (y == floor(y) && fabs(y) < 64) {
    double r = 1; int n = (int)fabs(y);
    for (int i = 0; i < n; ++i) r *= x;
    return y < 0 ? 1 / r : r;
  }
  return exp(y * log(x));
}

// ---- time -----------------------------------------------------------------------------------
long clock(void) { u32 tb; asm volatile("mftb %0" : "=r"(tb)); return (long)tb; }
long time(long* t) { long v = clock() / 25000000; if (t) *t = v; return v; }

// ---- stdio over the RAM disc ----------------------------------------------------------------
struct _iobuf { const u8* data; long size, pos; };
static _iobuf g_files[16];

FILE* fopen(const char* path, const char* mode) {
  if (mode[0] != 'r') return 0;  // the disc is read-only
  const u32* rd = (const u32*)kRamDisc;
  if (rd[0] != kRamDiscMagic) return 0;
  const char* base = path;
  for (const char* p = path; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
  for (u32 i = 0; i < rd[1]; ++i) {
    const char* name = (const char*)(rd + 2 + i * 14);  // 48-byte name, offset, size
    if (stricmp(name, base) != 0) continue;
    for (_iobuf& f : g_files)
      if (!f.data) {
        f.data = (const u8*)kRamDisc + rd[2 + i * 14 + 12];
        f.size = (long)rd[2 + i * 14 + 13];
        f.pos = 0;
        return &f;
      }
  }
  return 0;
}
int fclose(FILE* f) { if (f) f->data = 0; return 0; }
size_t fread(void* p, size_t size, size_t n, FILE* f) {
  if (!f || !size) return 0;
  size_t have = (size_t)(f->size - f->pos) / size;
  if (n > have) n = have;
  memcpy(p, f->data + f->pos, n * size);
  f->pos += (long)(n * size);
  return n;
}
size_t fwrite(const void*, size_t, size_t, FILE*) { return 0; }
int fseek(FILE* f, long off, int whence) {
  long p = whence == SEEK_SET ? off : whence == SEEK_CUR ? f->pos + off : f->size + off;
  if (p < 0 || p > f->size) return -1;
  f->pos = p;
  return 0;
}
long ftell(FILE* f) { return f->pos; }
int fflush(FILE*) { return 0; }
int ferror(FILE*) { return 0; }
int unlink(const char*) { return -1; }
int mkdir(const char*) { return -1; }
char* getcwd(char* b, int) { b[0] = 0; return b; }
int chdir(const char*) { return -1; }
long _findfirst(const char*, struct _finddata_t*) { return -1; }
int _findnext(long, struct _finddata_t*) { return -1; }

// ---- vsprintf (subset: flags - 0, width, precision, l, d i u x X c s p f g %) ----------------
static char* put_uint(char* end, unsigned long v, unsigned base, bool upper) {
  do { unsigned d = (unsigned)(v % base); *--end = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10); v /= base; } while (v);
  return end;
}
int vsnprintf(char* out, size_t cap, const char* fmt, va_list ap) {
  size_t n = 0;
  auto emit = [&](char c) { if (n + 1 < cap) out[n] = c; ++n; };
  for (; *fmt; ++fmt) {
    if (*fmt != '%') { emit(*fmt); continue; }
    ++fmt;
    bool left = false, zero = false, plus = false;
    for (;; ++fmt) {
      if (*fmt == '-') left = true; else if (*fmt == '0') zero = true; else if (*fmt == '+') plus = true; else if (*fmt != ' ') break;
    }
    int width = 0, prec = -1;
    if (*fmt == '*') { width = va_arg(ap, int); ++fmt; } else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
    if (*fmt == '.') {
      ++fmt; prec = 0;
      if (*fmt == '*') { prec = va_arg(ap, int); ++fmt; } else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
    }
    while (*fmt == 'l' || *fmt == 'h') ++fmt;
    char tmp[48], *s = tmp + sizeof(tmp), sign = 0;
    const char* str = 0;
    int len = 0;
    switch (*fmt) {
      case 'd': case 'i': {
        long v = va_arg(ap, int);
        if (v < 0) { sign = '-'; v = -v; } else if (plus) sign = '+';
        s = put_uint(s, (unsigned long)v, 10, false);
        str = s; len = (int)(tmp + sizeof(tmp) - s); break;
      }
      case 'u': s = put_uint(s, va_arg(ap, unsigned), 10, false); str = s; len = (int)(tmp + sizeof(tmp) - s); break;
      case 'x': case 'X': s = put_uint(s, va_arg(ap, unsigned), 16, *fmt == 'X'); str = s; len = (int)(tmp + sizeof(tmp) - s); break;
      case 'p': s = put_uint(s, (unsigned long)va_arg(ap, void*), 16, false); str = s; len = (int)(tmp + sizeof(tmp) - s); break;
      case 'c': tmp[0] = (char)va_arg(ap, int); str = tmp; len = 1; break;
      case 's': str = va_arg(ap, const char*); if (!str) str = "(null)"; len = (int)strlen(str); if (prec >= 0 && len > prec) len = prec; break;
      case 'f': case 'g': case 'e': {
        double v = va_arg(ap, double);
        if (v < 0) { sign = '-'; v = -v; } else if (plus) sign = '+';
        int p = prec < 0 ? (*fmt == 'f' ? 6 : 4) : prec;
        double scale = 1; for (int i = 0; i < p; ++i) scale *= 10;
        unsigned long whole = (unsigned long)v, frac = (unsigned long)((v - (double)whole) * scale + 0.5);
        if (frac >= (unsigned long)scale) { ++whole; frac = 0; }
        char* e = tmp + sizeof(tmp);
        if (p) { for (int i = 0; i < p; ++i) { *--e = (char)('0' + frac % 10); frac /= 10; } *--e = '.'; }
        e = put_uint(e, whole, 10, false);
        str = e; len = (int)(tmp + sizeof(tmp) - e); break;
      }
      case '%': emit('%'); continue;
      default: if (!*fmt) --fmt; continue;
    }
    int total = len + (sign ? 1 : 0);
    if (!left && !zero) for (int i = total; i < width; ++i) emit(' ');
    if (sign) emit(sign);
    if (!left && zero) for (int i = total; i < width; ++i) emit('0');
    for (int i = 0; i < len; ++i) emit(str[i]);
    if (left) for (int i = total; i < width; ++i) emit(' ');
  }
  if (cap) out[n < cap ? n : cap - 1] = 0;
  return (int)n;
}
int vsprintf(char* out, const char* fmt, va_list ap) { return vsnprintf(out, 0x7FFFFFFF, fmt, ap); }
int sprintf(char* out, const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int r = vsprintf(out, fmt, ap);
  va_end(ap);
  return r;
}

// ---- C++ ABI stubs: a throw halts; nothing is ever unwound ------------------------------------
int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
void* __dso_handle = 0;
void* __cxa_allocate_exception(size_t n) { return malloc(n); }
void __cxa_throw(void*, void*, void (*)(void*)) { lion_halt("uncaught exception"); }
void __cxa_call_unexpected(void*) { lion_halt("unexpected"); }
void __cxa_begin_catch() { lion_halt("catch"); }
void __cxa_end_catch() {}
void _Unwind_Resume(void*) { lion_halt("unwind"); }
int __gxx_personality_v0(...) { return 0; }
void __cxa_pure_virtual() { lion_halt("pure virtual"); }

// Runs the static constructors (the Unreal class registry is built from them).
typedef void (*Ctor)();
extern Ctor __init_array_start[], __init_array_end[];
void lion_run_ctors() {
  for (Ctor* c = __init_array_start; c < __init_array_end; ++c) (*c)();
}

}  // extern "C"
