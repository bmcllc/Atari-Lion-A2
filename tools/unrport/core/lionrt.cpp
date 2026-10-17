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

// Segregated-fit allocator: 16-byte header, 16-byte steps up to 256 bytes, then four size classes per
// octave (at most 25% waste), per-class free lists, bump arena.
struct Hdr { u32 cls; u32 size; u32 pad[2]; };
static u32 g_bump = kHeapStart;
static Hdr* g_free[128];
static u32 g_in_use;
static const char* g_tag_name[64];
static int g_tag_bytes[64];
static u32 g_tags = 1;
static u32 size_class(u32 n, u32* rounded) {
  if (n <= 256) { *rounded = (n + 15) & ~15u; return (*rounded >> 4) - 1; }
  const u32 msb = 31 - __builtin_clz(n - 1), step = 1u << (msb - 2);
  const u32 sz = (n + step - 1) & ~(step - 1);
  *rounded = sz;
  return 16 + (msb - 8) * 4 + ((sz - (1u << msb)) / step - 1);
}
void lion_heap_stats(u32* used, u32* arena_left) { *used = g_in_use; *arena_left = kHeapEnd - g_bump; }
u32 lion_heap_capacity() { return kHeapEnd - kHeapStart; }
void* malloc(size_t n) {
  u32 sz, cls = size_class(n ? (u32)n : 1, &sz);
  if (g_free[cls]) {
    Hdr* h = g_free[cls];
    g_free[cls] = *(Hdr**)(h + 1);
    h->size = (u32)n;
    h->pad[0] = 0; g_tag_bytes[0] += (int)sz;
    g_in_use += sz;
    return h + 1;
  }
  const u32 total = sz + sizeof(Hdr);
  if (g_bump + total > kHeapEnd) {
    // Arena exhausted: take a whole block of a larger class instead of failing.
    for (u32 c = cls + 1; c < 128; ++c)
      if (g_free[c]) {
        Hdr* h = g_free[c];
        g_free[c] = *(Hdr**)(h + 1);
        h->size = (u32)n;
        h->pad[0] = 0; g_tag_bytes[0] += (int)sz;
        g_in_use += sz;
        return h + 1;
      }
    char msg[64];
    sprintf(msg, "out of memory: %u bytes, %u in use", (u32)n, g_in_use);
    lion_halt(msg);
  }
  Hdr* h = (Hdr*)g_bump;
  g_bump += total;
  h->cls = cls;
  h->size = (u32)n;
  h->pad[0] = 0; g_tag_bytes[0] += (int)sz;
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
enum : u32 { kCd = 0x1F060000, kPic = 0x1F040000, kCdChunk = 16 * 2048, kCdSlots = 24 };
struct _iobuf {
  const u8* data;  // RAM disc file, or NULL for a CD file
  long size, pos;
  u32 lba;         // first sector of a CD file
  bool used;
};
static _iobuf g_files[24];
// CD read cache shared by all files: kCdSlots chunks of kCdChunk bytes, the least recently used replaced.
static u8* g_cd_chunks;
static const _iobuf* g_slot_file[kCdSlots];
static long g_slot_at[kCdSlots];
static u32 g_slot_age[kCdSlots], g_cd_age;
static void init_file(_iobuf* f, const u8* data, long size, u32 lba) {
  f->data = data; f->size = size; f->pos = 0; f->lba = lba; f->used = true;
  for (u32 i = 0; i < kCdSlots; ++i)
    if (g_slot_file[i] == f) g_slot_file[i] = 0;
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
  if (++reads % 100 == 0) {
    char msg[96];
    u32 tb;
    asm volatile("mftb %0" : "=r"(tb));
    sprintf(msg, "cd: %u reads, %u sectors (%u KB), last lba %u, t=%u s", reads, sectors, sectors * 2, lba, tb / 25000000);
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
static bool cd_find_uncached(const char* name, u32* lba, u32* size) {
  if (!(REG(kCd) & 1)) return false;  // no disc
  static u8 sector[2048] __attribute__((aligned(32)));
  if (!cd_read(16, 1, sector)) return false;
  u32 dir_lba = sector[158] | sector[159] << 8 | sector[160] << 16 | (u32)sector[161] << 24;
  u32 dir_size = sector[166] | sector[167] << 8 | sector[168] << 16 | (u32)sector[169] << 24;
  for (u32 off = 0; off < dir_size; off += 2048) {
    if (!cd_read(dir_lba + off / 2048, 1, sector)) return false;
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
  init_file(slot, 0, (long)size, lba);
  return slot;
}
int fclose(FILE* f) {
  if (!f) return 0;
  for (u32 i = 0; i < kCdSlots; ++i)
    if (g_slot_file[i] == f) g_slot_file[i] = 0;
  f->used = false;
  return 0;
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
  u8* out = (u8*)p;
  if (!g_cd_chunks) g_cd_chunks = (u8*)malloc(kCdSlots * kCdChunk + 32);
  u8* base = (u8*)(((u32)g_cd_chunks + 31) & ~31u);
  while (total) {
    const long at = f->pos & ~(long)(kCdChunk - 1);
    int slot = -1;
    for (u32 i = 0; i < kCdSlots; ++i) if (g_slot_file[i] == f && g_slot_at[i] == at) { slot = (int)i; break; }
    if (slot < 0) {  // evict the least recently used chunk
      slot = 0;
      for (u32 i = 1; i < kCdSlots; ++i)
        if (!g_slot_file[i] || (g_slot_file[slot] && g_slot_age[i] < g_slot_age[slot])) slot = (int)i;
      const long left = f->size - at;
      const u32 sectors = (u32)((left < (long)kCdChunk ? left : (long)kCdChunk) + 2047) / 2048;
      g_slot_file[slot] = 0;
      if (!cd_read(f->lba + (u32)(at / 2048), sectors, base + slot * kCdChunk)) return 0;
      g_slot_file[slot] = f;
      g_slot_at[slot] = at;
    }
    g_slot_age[slot] = ++g_cd_age;
    size_t take = (size_t)(at + (long)kCdChunk - f->pos);
    if (take > total) take = total;
    memcpy(out, base + slot * kCdChunk + (f->pos - at), take);
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

// Runs the static constructors (the Unreal class registry is built from them).
typedef void (*Ctor)();
extern Ctor __init_array_start[], __init_array_end[];
void lion_run_ctors() {
  for (Ctor* c = __init_array_start; c < __init_array_end; ++c) (*c)();
}

}  // extern "C"
