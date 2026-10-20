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
typedef unsigned short u16;
typedef unsigned char u8;

#define REG(a) (*(volatile u32 *)(a))
enum : u32 { kHeapStart = 0x00240000, kHeapEnd = 0x01780000, kRamDisc = 0x00200000, kRamDiscMagic = 0x4B534452 };

extern "C" {

// ---- debug log ring (read with the emulator's --dump-ram) -------------------------------------
enum : u32 { kLogAddr = 0x001F0000, kLogSize = 0x10000 };
void lion_log(const char* text) {
  volatile u32* len = (volatile u32*)kLogAddr;
  volatile char* buf = (volatile char*)(kLogAddr + 4);
  for (; *text; ++text)
    if (*len < kLogSize - 8) buf[(*len)++] = *text;
  if (*len < kLogSize - 8) buf[(*len)++] = '\n';
}

// ---- halt / panic ---------------------------------------------------------------------------
const char* g_panic = 0;
void lion_halt(const char* why) {
  g_panic = why;
  lion_log("HALT:");
  lion_log(why);
  {  // where from: return addresses up the stack (SysV back chain)
    u32* sp = (u32*)__builtin_frame_address(0);
    char msg[160];
    int n = sprintf(msg, "stack:");
    for (int d = 0; d < 14 && sp; ++d) {
      u32* prev = (u32*)sp[0];
      if (!prev || (u32)prev < 0x100000 || (u32)prev > 0x01800000) break;
      n += sprintf(msg + n, " %x", prev[1]);
      sp = prev;
    }
    lion_log(msg);
  }
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
  if ((((u32)a | (u32)b) & 3) == 0) {  // word copies when both sides are aligned
    u32* wa = (u32*)a; const u32* wb = (const u32*)b;
    for (; n >= 16; n -= 16) { wa[0] = wb[0]; wa[1] = wb[1]; wa[2] = wb[2]; wa[3] = wb[3]; wa += 4; wb += 4; }
    for (; n >= 4; n -= 4) *wa++ = *wb++;
    a = (u8*)wa; b = (const u8*)wb;
  }
  while (n--) *a++ = *b++;
  return d;
}
void* memmove(void* d, const void* s, size_t n) {
  u8* a = (u8*)d; const u8* b = (const u8*)s;
  if (a < b || a >= b + n) return memcpy(d, s, n);
  a += n; b += n;
  while (n--) *--a = *--b;
  return d;
}
void* memset(void* d, int c, size_t n) {
  u8* a = (u8*)d;
  while (n && ((u32)a & 3)) { *a++ = (u8)c; --n; }
  const u32 w = (u8)c * 0x01010101u;
  u32* wa = (u32*)a;
  for (; n >= 16; n -= 16) { wa[0] = w; wa[1] = w; wa[2] = w; wa[3] = w; wa += 4; }
  for (; n >= 4; n -= 4) *wa++ = w;
  a = (u8*)wa;
  while (n--) *a++ = (u8)c;
  return d;
}
int memcmp(const void* x, const void* y, size_t n) {
  const u8 *a = (const u8*)x, *b = (const u8*)y;
  for (; n; --n, ++a, ++b) if (*a != *b) return *a - *b;
  return 0;
}

// Segregated-fit allocator: 16-byte header, 16-byte steps up to 256 bytes, then eight size classes per
// octave (at most 12.5% waste), per-class free lists, bump arena.
enum : u32 { kClasses = 160 };
struct Hdr { u32 cls; u32 size; u32 pad[2]; };
static u32 g_bump = kHeapStart;
static Hdr* g_free[kClasses];
static u32 g_in_use;
void lion_class_report();
void lion_tag_report();
static const char* g_tag_name[64];
static int g_tag_bytes[64];
static u32 g_tags = 1;
static int g_cls_live[kClasses];  // live blocks per size class (memory report)
static u32 size_class(u32 n, u32* rounded) {
  if (n <= 256) { *rounded = (n + 15) & ~15u; return (*rounded >> 4) - 1; }
  const u32 msb = 31 - __builtin_clz(n - 1), step = 1u << (msb - 3);
  const u32 sz = (n + step - 1) & ~(step - 1);
  *rounded = sz;
  return 16 + (msb - 8) * 8 + ((sz - (1u << msb)) / step - 1);
}
static u32 class_size(u32 c) {
  if (c < 16) return (c + 1) * 16;
  const u32 k = c - 16, base = 1u << (k / 8 + 8);
  return base + (k % 8 + 1) * (base / 8);
}
void lion_heap_stats(u32* used, u32* arena_left) { *used = g_in_use; *arena_left = kHeapEnd - g_bump; }
u32 lion_heap_capacity() { return kHeapEnd - kHeapStart; }
void* malloc(size_t n) {
  u32 sz, cls = size_class(n ? (u32)n : 1, &sz);
  if (g_free[cls]) {
    Hdr* h = g_free[cls];
    g_free[cls] = *(Hdr**)(h + 1);
    h->size = (u32)n;
    h->pad[0] = 0; g_tag_bytes[0] += (int)sz; ++g_cls_live[h->cls];
    g_in_use += sz;
    return h + 1;
  }
  const u32 total = sz + sizeof(Hdr);
  if (g_bump + total > kHeapEnd) {
    // Arena exhausted: take a whole block of a larger class instead of failing.
    for (u32 c = cls + 1; c < kClasses; ++c)
      if (g_free[c]) {
        Hdr* h = g_free[c];
        g_free[c] = *(Hdr**)(h + 1);
        // Split: the rest of the block goes back to the free lists (a small request must not eat a 2 MB block).
        const u32 csz = class_size(c);
        if (csz >= sz + sizeof(Hdr) + 32) {
          const u32 rest = csz - sz - sizeof(Hdr);
          u32 rc = c;
          while (rc > 0 && class_size(rc) > rest) --rc;
          Hdr* r = (Hdr*)((u8*)(h + 1) + sz);
          r->cls = rc;
          r->size = class_size(rc);
          r->pad[0] = 0;
          *(Hdr**)(r + 1) = g_free[rc];
          g_free[rc] = r;
          h->cls = cls;
        }
        h->size = (u32)n;
        h->pad[0] = 0; g_tag_bytes[0] += (int)sz; ++g_cls_live[h->cls];
        g_in_use += sz;
        return h + 1;
      }
    char msg[64];
    sprintf(msg, "out of memory: %u bytes, %u in use", (u32)n, g_in_use);
    lion_class_report();
    lion_tag_report();
    lion_halt(msg);
  }
  Hdr* h = (Hdr*)g_bump;
  g_bump += total;
  h->cls = cls;
  h->size = (u32)n;
  h->pad[0] = 0; g_tag_bytes[0] += (int)sz; ++g_cls_live[h->cls];
  g_in_use += sz;
  return h + 1;
}
void free(void* p) {
  if (!p) return;
  Hdr* h = (Hdr*)p - 1;
  u32 sz;
  size_class(h->size ? h->size : 1, &sz);
  g_in_use -= sz;
  g_tag_bytes[h->pad[0]] -= (int)sz;
  --g_cls_live[h->cls];
  *(Hdr**)p = g_free[h->cls];
  g_free[h->cls] = h;
}
void* realloc(void* p, size_t n) {
  if (!p) return malloc(n);
  Hdr* h = (Hdr*)p - 1;
  u32 cap, want;
  size_class(h->size ? h->size : 1, &cap);
  size_class(n ? (u32)n : 1, &want);
  if (want == cap) { h->size = (u32)n; return p; }
  void* q = malloc(n);
  memcpy(q, p, h->size < n ? h->size : n);
  free(p);
  return q;
}
// Allocations carry the engine's tag string (appMalloc) for the memory report.
static void retag(void* p, const char* tag) {
  if (!p || !tag) return;
  u32 t = 1;
  for (; t < g_tags; ++t) if (g_tag_name[t] == tag) break;
  if (t == g_tags) { if (g_tags < 64) g_tag_name[g_tags++] = tag; else t = 63; }
  Hdr* h = (Hdr*)p - 1;
  u32 sz;
  size_class(h->size ? h->size : 1, &sz);
  if (h->pad[0] == t) return;
  g_tag_bytes[h->pad[0]] -= (int)sz;
  g_tag_bytes[t] += (int)sz;
  h->pad[0] = t;
}
void* lion_malloc_tag(size_t n, const char* tag) { void* p = malloc(n); retag(p, tag); return p; }
void* lion_realloc_tag(void* p, size_t n, const char* tag) { void* q = realloc(p, n); retag(q, tag); return q; }
// The size classes holding the most live memory.
void lion_class_report() {
  char m[96];
  static int done[kClasses];
  for (int c = 0; c < (int)kClasses; ++c) done[c] = 0;
  for (int r = 0; r < 12; ++r) {
    int best = -1;
    u32 best_bytes = 0;
    for (int c = 0; c < (int)kClasses; ++c) {
      // The size of class c: invert size_class() by probing its first member size.
      u32 sz = c < 16 ? (u32)(c + 1) * 16 : 0;
      if (!sz) { const int k = c - 16, o = k / 8, st = k % 8; const u32 base = 1u << (o + 8); sz = base + (u32)(st + 1) * (base / 8); }
      const u32 bytes = g_cls_live[c] > 0 ? (u32)g_cls_live[c] * sz : 0;
      if (!done[c] && bytes > best_bytes) { best_bytes = bytes; best = c; }
    }
    if (best < 0 || best_bytes < 100000) break;
    done[best] = 1;
    sprintf(m, "heap class %d: %d blocks, %u KB", best, g_cls_live[best], best_bytes >> 10);
    lion_log(m);
  }
}
void lion_tag_report() {
  char m[96];
  for (u32 t = 0; t < g_tags; ++t)
    if (g_tag_bytes[t] > 100000) {
      sprintf(m, "heap tag %s: %d KB", t ? g_tag_name[t] : "(untagged)", g_tag_bytes[t] >> 10);
      lion_log(m);
    }
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
// Newton's iteration from a linear guess on [0.5, 2) after taking the exponent out (even powers of two only). Four
// steps reach double precision; the software divide is the cost, so the iteration count matters.
double sqrt(double x) {
  if (x <= 0) return 0;
  if (x != x || x > 1.7976931348623157e308) return x;
  if (x < 2.2250738585072014e-308) return sqrt(x * 18014398509481984.0) * (1.0 / 134217728.0);  // subnormal: 2^54 up
  union { double d; unsigned long long u; } v;
  v.d = x;
  int e = (int)((v.u >> 52) & 0x7FF) - 1022;  // x = m * 2^e with m in [0.5, 1)
  v.u = (v.u & 0x000FFFFFFFFFFFFFull) | (1022ull << 52);
  double m = v.d;
  if (e & 1) { m *= 2; --e; }                  // now e is even and m is in [0.5, 2)
  double g = 0.4714045 + 0.4714045 * m;
  g = 0.5 * (g + m / g);
  g = 0.5 * (g + m / g);
  g = 0.5 * (g + m / g);
  g = 0.5 * (g + m / g);
  v.u = (unsigned long long)(1023 + e / 2) << 52;  // 2^(e/2)
  return g * v.d;
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
  if (x != x) return x;
  if (x > 700) return 1e300;
  if (x < -700) return 0;
  int k = (int)floor(x / 0.693147180559945);
  double r = x - k * 0.693147180559945, term = 1, s = 1;
  for (int i = 1; i < 20; ++i) { term *= r / i; s += term; }
  while (k > 0) { s *= 2; --k; }
  while (k < 0) { s *= 0.5; ++k; }
  return s;
}
double log(double x) {
  if (x != x) return x;
  if (x <= 0) return -1e300;
  if (x > 1e300) return 690.0;
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

// ---- stdio: the RAM disc first, then ISO9660 files streamed from the CD ----------------------------
enum : u32 { kLeanGroup = 32, kCd = 0x1F060000, kPic = 0x1F040000, kCdChunk = 2 * 2048, kCdSlots = 128, kCdGroup = 128, kStageGroups = 2, kTraceMax = 4096 };
struct _iobuf {
  const u8* data;  // RAM disc file, or NULL for a CD file
  long size, pos;
  u32 lba;         // first sector of a CD file
  u32 id;          // hash of the file name (stable between builds, unlike the sector)
  bool used;
};
static _iobuf g_files[96];  // every loaded package keeps its file open (retail levels touch ~40 packages)
// CD read cache shared by all files: kCdSlots chunks of kCdChunk bytes, the least recently used replaced. A chunk is
// identified by the hash of its file name and its index, so the cache survives fclose/fopen.
static u8* g_cd_chunks;
static u32 g_slot_lba[kCdSlots];   // 0 = empty
static u32 g_slot_chunk[kCdSlots];
static u32 g_slot_age[kCdSlots], g_cd_age;
// Chunks that came in a prefetched run wait in a small staging area (two runs, the older one replaced) and move to
// the per-chunk cache the moment the game touches them, so hot chunks stay and the rest of a run does not.
static u8* g_stage;
static u32 g_stage_lba[kStageGroups * kCdGroup], g_stage_chunk[kStageGroups * kCdGroup], g_stage_next;
// Chunks in the order the game first touched them (file name hash, chunk index): build.sh records this once, and
// unrport lays the chunks out in that order in PRELOAD.BIN (with PRELOAD.MAP naming them), so that loading reads the
// disc in long sequential runs instead of one seek per chunk. 'extern' for the host: build.sh finds the symbol in the linked image.
u32 g_cd_trace[2 + 4 * kTraceMax];  // count, (file, chunk) pairs, then the same for the chunks that were read again
static bool g_pre_tried;
static u32 g_pre_lba, g_pre_n;
static u32* g_pre_ent;  // pairs: file id, chunk
static u32 g_pre_hot;   // the first g_pre_hot entries are kept in memory (g_pin) all the time
static u8* g_pin;
static u8* g_pin_raw;
static bool g_lean;  // the preload was released: small staging area, no groups
static u32 name_hash(const char* s) {  // FNV-1a over the lower-case name
  u32 h = 2166136261u;
  for (; *s; ++s) { u32 c = (unsigned char)*s; if (c >= 'A' && c <= 'Z') c += 32; h = (h ^ c) * 16777619u; }
  return h ? h : 1;
}
static void init_file(_iobuf* f, const u8* data, long size, u32 lba, u32 id = 0) {
  f->data = data; f->size = size; f->pos = 0; f->lba = lba; f->id = id; f->used = true;
}

// Waits for the drive with the CPU dozing; the vblank/CD interrupt handler at 0x500 only acknowledges.
static bool cd_wait() {
  u32 msr;
  asm volatile("mfmsr %0" : "=r"(msr));
  const u32 doze = msr | 0x8000 | 0x40000;  // EE | POW
  REG(kPic + 4) = 0x20;                      // PIC: CD interrupt
  while (!(REG(kCd) & 8)) asm volatile("mtmsr %0; isync" : : "r"(doze));
  asm volatile("mtmsr %0; isync" : : "r"(msr));
  REG(kPic + 4) = 0;
  const bool ok = !(REG(kCd) & 4);
  REG(kCd + 4) = 3;  // acknowledge
  return ok;
}
static u32 g_n_group, g_n_seen, g_n_unmapped;
static bool cd_read(u32 lba, u32 count, void* dst) {
  static u32 reads, sectors;
  sectors += count;
  if (reads < 6) {  // who is reading? walk the stack (SysV: back chain at 0(sp), LR at 4(frame))
    u32* sp = (u32*)__builtin_frame_address(0);
    char msg[160];
    int n = sprintf(msg, "cd read %u: lba %u count %u from", reads, lba, count);
    for (int d = 0; d < 9 && sp; ++d) {
      u32* prev = (u32*)sp[0];
      if (!prev || (u32)prev < 0x100000 || (u32)prev > 0x01800000) break;
      n += sprintf(msg + n, " %x", prev[1]);
      sp = prev;
    }
    lion_log(msg);
  }
  {  // loading bar on the front buffer: the disc is ~28000 sectors of assets
    volatile unsigned short* fb = (volatile unsigned short*)0x10000000;
    const u32 w = sectors * 300 / 28000 > 300 ? 300 : sectors * 300 / 28000;
    for (u32 y = 224; y < 232; ++y)
      for (u32 x = 10; x < 10 + w; ++x) fb[y * 320 + x] = 0xF800;  // red like the Lion
  }
  static u32 next_lba, seq, near_, far_, prev_count;
  if (lba == next_lba) ++seq; else if ((lba > next_lba ? lba - next_lba : next_lba - lba) < 2000) ++near_; else ++far_;
  next_lba = lba + count;
  if (++reads % 100 == 0) {
    char msg[128];
    u32 tb;
    asm volatile("mftb %0" : "=r"(tb));
    sprintf(msg, "cd: %u reads, %u sectors (%u KB), last lba %u, t=%u s, sequential %u near %u far %u (groups %u, seen %u, unmapped %u)", reads, sectors, sectors * 2, lba, tb / 25000000, seq, near_, far_, g_n_group, g_n_seen, g_n_unmapped);
    lion_log(msg);
  }
  REG(kCd + 8) = lba;
  REG(kCd + 12) = count;
  REG(kCd + 16) = (u32)dst;
  REG(kCd + 4) = 1;
  return cd_wait();
}

// Looks a file up in the ISO9660 root directory (names compared case-insensitively, ";1" ignored).
struct CdEntry { char name[40]; u32 lba, size; bool found; };
static CdEntry g_cd_cache[24];
static u32 g_cd_cached;
static bool cd_find_uncached(const char* name, u32* lba, u32* size);
static bool cd_find(const char* name, u32* lba, u32* size) {
  for (u32 i = 0; i < g_cd_cached; ++i)
    if (!stricmp(g_cd_cache[i].name, name)) { *lba = g_cd_cache[i].lba; *size = g_cd_cache[i].size; return g_cd_cache[i].found; }
  const bool found = cd_find_uncached(name, lba, size);
  if (g_cd_cached < 24 && strlen(name) < 40) {
    CdEntry& e = g_cd_cache[g_cd_cached++];
    strcpy(e.name, name);
    e.found = found;
    e.lba = found ? *lba : 0;
    e.size = found ? *size : 0;
  }
  return found;
}
// The root directory is read once, in one command: every lookup used to seek to each of its sectors in turn.
static u8* g_root_dir;
static u32 g_root_dir_size;
static bool cd_read_root_dir() {
  if (g_root_dir) return true;
  static u8 sector[2048] __attribute__((aligned(32)));
  if (!cd_read(16, 1, sector)) return false;
  const u32 dir_lba = sector[158] | sector[159] << 8 | sector[160] << 16 | (u32)sector[161] << 24;
  const u32 dir_size = sector[166] | sector[167] << 8 | sector[168] << 16 | (u32)sector[169] << 24;
  const u32 sectors = (dir_size + 2047) / 2048;
  u8* buf = (u8*)malloc(sectors * 2048 + 32);
  u8* dir = (u8*)(((u32)buf + 31) & ~31u);
  if (!cd_read(dir_lba, sectors, dir)) { free(buf); return false; }
  g_root_dir = dir;
  g_root_dir_size = dir_size;
  return true;
}
static bool cd_find_uncached(const char* name, u32* lba, u32* size) {
  if (!(REG(kCd) & 1)) return false;  // no disc
  if (!cd_read_root_dir()) return false;
  for (u32 off = 0; off < g_root_dir_size; off += 2048) {
    const u8* sector = g_root_dir + off;
    for (u32 p = 0; p < 2048 && sector[p];) {
      const u8 len = sector[p], nlen = sector[p + 32];
      char entry[40];
      u32 n = nlen < 39 ? nlen : 39, i = 0;
      for (; i < n && sector[p + 33 + i] != ';'; ++i) entry[i] = (char)sector[p + 33 + i];
      entry[i] = 0;
      if (!stricmp(entry, name)) {
        *lba = sector[p + 2] | sector[p + 3] << 8 | sector[p + 4] << 16 | (u32)sector[p + 5] << 24;
        *size = sector[p + 10] | sector[p + 11] << 8 | sector[p + 12] << 16 | (u32)sector[p + 13] << 24;
        return true;
      }
      p += len;
    }
  }
  return false;
}

FILE* fopen(const char* path, const char* mode) {
  if (mode[0] != 'r') return 0;  // the disc is read-only
  const char* base = path;
  for (const char* p = path; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
  _iobuf* slot = 0;
  for (_iobuf& f : g_files) if (!f.used) { slot = &f; break; }
  if (!slot) return 0;
  const u32* rd = (const u32*)kRamDisc;
  if (rd[0] == kRamDiscMagic)
    for (u32 i = 0; i < rd[1]; ++i) {
      const char* name = (const char*)(rd + 2 + i * 14);  // 48-byte name, offset, size
      if (stricmp(name, base) != 0) continue;
      init_file(slot, (const u8*)kRamDisc + rd[2 + i * 14 + 12], (long)rd[2 + i * 14 + 13], 0);
      return slot;
    }
  u32 lba, size;
  if (!cd_find(base, &lba, &size)) return 0;
  init_file(slot, 0, (long)size, lba, name_hash(base));
  return slot;
}
int fclose(FILE* f) {
  if (!f) return 0;
  f->used = false;
  return 0;
}
// The preload map (see g_cd_trace), read from the disc the first time a chunk is missing.
static u8* g_pre_touch;   // per entry: the chunk was fetched already in this run
static u32* g_pre_hash;
enum : u32 { kPreHash = 8192, kPreHashMax = 4096 };
static u32 pre_hash(u32 id, u32 chunk) { return ((id ^ chunk * 2654435761u) * 2246822519u) >> 19; }
// Entry index of (file, chunk) in the preload map, or -1.
static int pre_index(u32 id, u32 chunk) {
  if (!g_pre_hash) {
    for (u32 i = 0; i < g_pre_n; ++i) if (g_pre_ent[2 * i] == id && g_pre_ent[2 * i + 1] == chunk) return (int)i;
    return -1;
  }
  for (u32 h = pre_hash(id, chunk) & (kPreHash - 1);; h = (h + 1) & (kPreHash - 1)) {
    const u32 e = g_pre_hash[h];
    if (!e) return -1;
    if (g_pre_ent[2 * (e - 1)] == id && g_pre_ent[2 * (e - 1) + 1] == chunk) return (int)(e - 1);
  }
}
static void cd_load_preload() {
  g_pre_tried = true;
  u32 mlba, msize, blba, bsize;
  if (!cd_find("PRELOAD.MAP", &mlba, &msize) || !cd_find("PRELOAD.BIN", &blba, &bsize)) return;
  u8* raw = (u8*)malloc(((msize + 2047) & ~2047u) + 32);
  u8* buf = (u8*)(((u32)raw + 31) & ~31u);
  if (!cd_read(mlba, (msize + 2047) / 2048, buf) || ((u32*)buf)[0] != 0x50524C44 /* PRLD */) { free(raw); return; }
  g_pre_n = ((u32*)buf)[1];
  g_pre_hot = ((u32*)buf)[2];
  g_pre_ent = (u32*)malloc(g_pre_n * 8);
  memcpy(g_pre_ent, buf + 12, g_pre_n * 8);
  g_pre_touch = (u8*)calloc(g_pre_n + 1, 1);
  if (g_pre_n <= kPreHashMax) {  // (file, chunk) -> entry index + 1, open addressing
    g_pre_hash = (u32*)calloc(kPreHash, 4);
    for (u32 i = 0; i < g_pre_n; ++i) {
      u32 h = pre_hash(g_pre_ent[2 * i], g_pre_ent[2 * i + 1]);
      while (g_pre_hash[h]) h = (h + 1) & (kPreHash - 1);
      g_pre_hash[h] = i + 1;
    }
  }
  g_pre_lba = blba;
  free(raw);
  if (g_pre_hot) {  // chunks the recorded load read again and again: read once, keep
    u8* praw = (u8*)malloc(g_pre_hot * kCdChunk + 32);
    g_pin_raw = praw;
    g_pin = praw;
    u8* pbase = (u8*)(((u32)praw + 31) & ~31u);
    if (!cd_read(g_pre_lba, g_pre_hot * (kCdChunk / 2048u), pbase)) g_pre_hot = 0;
    g_pin = pbase;
  }
  char msg[64];
  sprintf(msg, "preload: %u chunks, %u kept", g_pre_n, g_pre_hot);
  lion_log(msg);
}

// Brings a chunk of a CD file into the cache and returns its slot. A chunk named by the preload map is read
// together with the chunks that follow it on the disc (one read into the staging area): the order is the one of
// the recorded load, so those are the next chunks the game asks for.
static int cd_fetch(_iobuf* f, u32 chunk, u8* base, u32 want) {
  if (!g_pre_tried) cd_load_preload();
  bool seen = false;  // touched before: a chunk the cache let go, which is read alone
  if (g_pre_n) {      // with a preload, the touch bit of the chunk's entry tells the same (a lookup, not a scan)
    const int pi = pre_index(f->id, chunk);
    if (pi >= 0) { seen = g_pre_touch[pi] != 0; g_pre_touch[pi] = 1; }
  } else {  // record the first touch of each chunk (the recording run has no preload)
    u32 n = g_cd_trace[0];
    for (u32 i = 0; i < n && !seen; ++i) seen = g_cd_trace[1 + 2 * i] == f->id && g_cd_trace[2 + 2 * i] == chunk;
    if (!seen && n < kTraceMax) { g_cd_trace[1 + 2 * n] = f->id; g_cd_trace[2 + 2 * n] = chunk; g_cd_trace[0] = n + 1; }
    if (seen) {  // read again: a candidate for staying in memory
      u32 h = g_cd_trace[1 + 2 * kTraceMax];
      bool known = false;
      for (u32 i = 0; i < h && !known; ++i) known = g_cd_trace[2 + 2 * kTraceMax + 2 * i] == f->id && g_cd_trace[3 + 2 * kTraceMax + 2 * i] == chunk;
      if (!known && h < kTraceMax) { g_cd_trace[2 + 2 * kTraceMax + 2 * h] = f->id; g_cd_trace[3 + 2 * kTraceMax + 2 * h] = chunk; g_cd_trace[1 + 2 * kTraceMax] = h + 1; }
    }
  }
  // The slot that will hold it: least recently used.
  int slot = 0;
  for (u32 i = 1; i < kCdSlots; ++i)
    if (!g_slot_lba[i] || (g_slot_lba[slot] && g_slot_age[i] < g_slot_age[slot])) slot = (int)i;
  g_slot_lba[slot] = 0;
  u8* dst = base + slot * kCdChunk;
  int from = -1;  // staged copy
  for (u32 i = 0; i < kStageGroups * kCdGroup && from < 0; ++i)
    if (g_stage_lba[i] == f->id && g_stage_chunk[i] == chunk) from = (int)i;
  if (from < 0) {
    // Read a run into the staging area: the chunks that follow this one on the disc when the preload map names it,
    // otherwise the chunks the caller wants (a sound, an object) and a few more, straight from the file.
    int p = -1;
    if (!seen) {
      const int pi = g_pre_n ? pre_index(f->id, chunk) : -1;
      if (pi >= (int)g_pre_hot) p = pi;
    }
    if (!g_stage) g_stage = (u8*)malloc((g_lean ? kLeanGroup : kStageGroups * kCdGroup) * kCdChunk + 32);
    u8* sbase = (u8*)(((u32)g_stage + 31) & ~31u);
    const u32 g = g_lean ? 0 : g_stage_next++ % kStageGroups;
    u32 cnt, lba;
    if (p >= 0) {
      cnt = g_pre_n - (u32)p < kCdGroup ? g_pre_n - (u32)p : kCdGroup;
      lba = g_pre_lba + (u32)p * (kCdChunk / 2048u);
      ++g_n_group;
    } else {
      const u32 in_file = (u32)((f->size - (long)chunk * kCdChunk + kCdChunk - 1) / kCdChunk);
      cnt = want < 4 ? 4 : want;
      if (cnt > (g_lean ? kLeanGroup : kCdGroup)) cnt = g_lean ? kLeanGroup : kCdGroup;
      if (cnt > in_file) cnt = in_file;
      lba = f->lba + chunk * (kCdChunk / 2048u);
      if (seen) ++g_n_seen; else ++g_n_unmapped;
    }
    // The last chunk of a file may be short: read only the sectors it has.
    u32 sectors = cnt * (kCdChunk / 2048u);
    if (p < 0) {
      const long end = (long)(chunk + cnt) * kCdChunk;
      if (end > f->size) sectors -= (u32)((end - f->size) / 2048);
    }
    for (u32 j = 0; j < kCdGroup; ++j) g_stage_lba[g * kCdGroup + j] = 0;
    if (!cd_read(lba, sectors, sbase + g * kCdGroup * kCdChunk)) return -1;
    for (u32 j = 0; j < cnt; ++j) {
      g_stage_lba[g * kCdGroup + j] = p >= 0 ? g_pre_ent[2 * (p + j)] : f->id;
      g_stage_chunk[g * kCdGroup + j] = p >= 0 ? g_pre_ent[2 * (p + j) + 1] : chunk + j;
    }
    from = (int)(g * kCdGroup);
  }
  memcpy(dst, (u8*)(((u32)g_stage + 31) & ~31u) + from * kCdChunk, kCdChunk);
  g_slot_lba[slot] = f->id;
  g_slot_chunk[slot] = chunk;
  return slot;
}

// The chunk the last read used (most reads are a few bytes out of it), its LRU slot (-1 when kept in g_pin) and a
// hint table from (file, chunk) to the slot, checked on use.
static u32 g_last_id, g_last_chunk;
static const u8* g_last_src;
static int g_last_slot = -1;
static u16 g_tab[1024];

// The level is up: the pinned chunks and the staging area (3+ MB) go back to the heap. Later reads (sounds, music,
// a next level) use the plain per-chunk cache with small read-ahead.
extern "C" void lion_cd_release_preload() {
  if (g_lean) return;
  g_lean = true;
  g_pre_hot = 0;
  g_pre_n = 0;  // no more preload groups
  free(g_pin_raw);
  g_pin_raw = g_pin = 0;
  free(g_stage);
  g_stage = 0;
  for (u32 i = 0; i < kStageGroups * kCdGroup; ++i) g_stage_lba[i] = 0;
  for (u32 i = 0; i < 1024; ++i) if (g_tab[i] & 0x8000) g_tab[i] = 0;
  g_last_src = 0;
  g_last_slot = -1;
  lion_log("preload released");
}

size_t fread(void* p, size_t size, size_t n, FILE* f) {
  if (!f || !size) return 0;
  size_t have = (size_t)(f->size - f->pos) / size;
  if (n > have) n = have;
  size_t total = n * size;
  if (f->data) {
    memcpy(p, f->data + f->pos, total);
    f->pos += (long)total;
    return n;
  }
  {  // fast path: the whole read is in the chunk the previous read used
    const long at = f->pos & ~(long)(kCdChunk - 1);
    if (g_last_src && g_last_id == f->id && g_last_chunk == (u32)(at / kCdChunk) && (size_t)(f->pos - at) + total <= kCdChunk) {
      const u8* s = g_last_src + (f->pos - at);
      u8* d = (u8*)p;
      if (total <= 16) for (size_t i = 0; i < total; ++i) d[i] = s[i];
      else memcpy(d, s, total);
      if (g_last_slot >= 0) g_slot_age[g_last_slot] = ++g_cd_age;
      f->pos += (long)total;
      return n;
    }
  }
  u8* out = (u8*)p;
  if (!g_cd_chunks) g_cd_chunks = (u8*)malloc(kCdSlots * kCdChunk + 32);
  u8* base = (u8*)(((u32)g_cd_chunks + 31) & ~31u);
  while (total) {
    const long at = f->pos & ~(long)(kCdChunk - 1);
    const u32 chunk = (u32)(at / kCdChunk);
    const u32 h = ((f->id ^ chunk * 2654435761u) * 2246822519u) >> 22;
    const u8* src = (g_last_src && g_last_id == f->id && g_last_chunk == chunk) ? g_last_src : 0;
    int slot_used = src ? g_last_slot : -1;
    if (!src && g_tab[h]) {
      const u32 e = g_tab[h];
      if (e & 0x8000) {
        const u32 i = e & 0x7FFF;
        if (i < g_pre_hot && g_pre_ent[2 * i] == f->id && g_pre_ent[2 * i + 1] == chunk) src = g_pin + i * kCdChunk;
      } else if (g_slot_lba[e - 1] == f->id && g_slot_chunk[e - 1] == chunk) {
        src = base + (e - 1) * kCdChunk;
        slot_used = (int)e - 1;
      }
    }
    for (u32 i = 0; !src && i < kCdSlots; ++i)
      if (g_slot_lba[i] == f->id && g_slot_chunk[i] == chunk) { src = base + i * kCdChunk; slot_used = (int)i; g_tab[h] = (u16)(i + 1); }
    if (!src && g_pre_tried)  // kept chunks
      for (u32 i = 0; !src && i < g_pre_hot; ++i)
        if (g_pre_ent[2 * i] == f->id && g_pre_ent[2 * i + 1] == chunk) { src = g_pin + i * kCdChunk; g_tab[h] = (u16)(0x8000 | i); }
    if (!src) {
      const u32 want = (u32)((total + (size_t)(f->pos - at) + kCdChunk - 1) / kCdChunk);  // chunks this read still needs
      const int slot = cd_fetch(f, chunk, base, want);
      if (slot < 0) return 0;
      src = base + slot * kCdChunk;
      slot_used = slot;
      g_tab[h] = (u16)(slot + 1);
    }
    if (slot_used >= 0) g_slot_age[slot_used] = ++g_cd_age;
    g_last_id = f->id; g_last_chunk = chunk; g_last_src = src; g_last_slot = slot_used;
    size_t take = (size_t)(at + (long)kCdChunk - f->pos);
    if (take > total) take = total;
    memcpy(out, src + (f->pos - at), take);
    out += take;
    f->pos += (long)take;
    total -= take;
  }
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
void __cxa_throw(void* obj, void*, void (*)(void*)) {
  lion_log("THROW:");
  lion_log(*(const char**)obj);  // appThrowf() throws a char*
  lion_halt("uncaught exception");
}
void __cxa_call_unexpected(void*) { lion_halt("unexpected"); }
void __cxa_begin_catch() { lion_halt("catch"); }
void __cxa_end_catch() {}
void _Unwind_Resume(void*) { lion_halt("unwind"); }
int __gxx_personality_v0(...) { return 0; }
void __cxa_pure_virtual() { lion_halt("pure virtual"); }


// 64-bit integer division and conversions (the compiler's runtime library is not linked).
unsigned long long __udivmoddi4(unsigned long long n, unsigned long long d, unsigned long long* rem) {
  unsigned long long q = 0, r = 0;
  if (d == 0) { if (rem) *rem = 0; return 0; }
  for (int i = 63; i >= 0; --i) {
    r = (r << 1) | ((n >> i) & 1);
    if (r >= d) { r -= d; q |= 1ull << i; }
  }
  if (rem) *rem = r;
  return q;
}
unsigned long long __udivdi3(unsigned long long n, unsigned long long d) { return __udivmoddi4(n, d, 0); }
unsigned long long __umoddi3(unsigned long long n, unsigned long long d) { unsigned long long r; __udivmoddi4(n, d, &r); return r; }
long long __divdi3(long long n, long long d) {
  const bool neg = (n < 0) != (d < 0);
  const unsigned long long q = __udivmoddi4(n < 0 ? -(unsigned long long)n : n, d < 0 ? -(unsigned long long)d : d, 0);
  return neg ? -(long long)q : (long long)q;
}
long long __moddi3(long long n, long long d) {
  unsigned long long r;
  __udivmoddi4(n < 0 ? -(unsigned long long)n : n, d < 0 ? -(unsigned long long)d : d, &r);
  return n < 0 ? -(long long)r : (long long)r;
}
double __floatundidf(unsigned long long v) { return (double)(unsigned)(v >> 32) * 4294967296.0 + (double)(unsigned)v; }
double __floatdidf(long long v) { return v < 0 ? -__floatundidf(-(unsigned long long)v) : __floatundidf((unsigned long long)v); }
unsigned long long __fixunsdfdi(double d) {
  if (d <= 0) return 0;
  const unsigned hi = (unsigned)(d / 4294967296.0);
  return ((unsigned long long)hi << 32) | (unsigned)(d - (double)hi * 4294967296.0);
}
long long __fixdfdi(double d) { return d < 0 ? -(long long)__fixunsdfdi(-d) : (long long)__fixunsdfdi(d); }

// Runs the static constructors (the Unreal class registry is built from them).
typedef void (*Ctor)();
extern Ctor __init_array_start[], __init_array_end[];
void lion_run_ctors() {
  for (Ctor* c = __init_array_start; c < __init_array_end; ++c) (*c)();
}

}  // extern "C"
