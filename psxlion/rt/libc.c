/* Freestanding memory and string functions for the Lion (clang may emit calls to these even with -ffreestanding). */
#include <stddef.h>
#include <stdint.h>
/* Word at a time when both pointers share their alignment (GTE marshaling and MDEC blocks call these per operation). */
__attribute__((no_builtin)) void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *a = d; const uint8_t *b = s;
    if ((((uintptr_t)a ^ (uintptr_t)b) & 3) == 0) {
        while (n && ((uintptr_t)a & 3)) { *a++ = *b++; n--; }
        uint32_t *wa = (uint32_t *)a; const uint32_t *wb = (const uint32_t *)b;
        for (; n >= 16; n -= 16) { uint32_t x0 = wb[0], x1 = wb[1], x2 = wb[2], x3 = wb[3]; wa[0] = x0; wa[1] = x1; wa[2] = x2; wa[3] = x3; wa += 4; wb += 4; }
        for (; n >= 4; n -= 4) *wa++ = *wb++;
        a = (uint8_t *)wa; b = (const uint8_t *)wb;
    }
    while (n--) *a++ = *b++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    uint8_t *a = d; const uint8_t *b = s;
    if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
__attribute__((no_builtin)) void *memset(void *d, int c, size_t n) {
    uint8_t *a = d;
    while (n && ((uintptr_t)a & 3)) { *a++ = (uint8_t)c; n--; }
    uint32_t w = (uint8_t)c * 0x01010101u, *wa = (uint32_t *)a;
    for (; n >= 16; n -= 16) { wa[0] = w; wa[1] = w; wa[2] = w; wa[3] = w; wa += 4; }
    for (; n >= 4; n -= 4) *wa++ = w;
    a = (uint8_t *)wa;
    while (n--) *a++ = (uint8_t)c;
    return d;
}
int memcmp(const void *x, const void *y, size_t n) {
    const uint8_t *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

/* Minimal pieces gte.cpp pulls in for its debug/config paths. */
int atoi(const char *s) { int n = 0, neg = 0; if (*s == '-') { neg = 1; s++; } while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0'); return neg ? -n : n; }
char *getenv(const char *n) { (void)n; return 0; }
void exit(int c) { (void)c; for (;;) { } }
struct lion_file; struct lion_file *stderr;
int fprintf(struct lion_file *f, const char *fmt, ...) { (void)f; (void)fmt; return 0; }
int snprintf(char *b, size_t n, const char *fmt, ...) { (void)fmt; if (n) b[0] = 0; return 0; }
/* Small first-fit heap: the MDEC decoder grows its input/output FIFOs with realloc, so free() must really return
 * blocks. 640 KB lives in .bss. */
typedef struct Block { size_t size; struct Block *next; uint32_t free, pad; } Block;
static uint8_t s_heap[640 * 1024] __attribute__((aligned(16)));
static Block *s_free_list;
static void heap_init(void) {
    s_free_list = (Block *)s_heap;
    s_free_list->size = sizeof(s_heap) - sizeof(Block);
    s_free_list->next = 0;
    s_free_list->free = 1;
}
void *malloc(size_t n) {
    if (!s_free_list) heap_init();
    if (!n) n = 1;
    n = (n + 15) & ~(size_t)15;
    for (Block *b = s_free_list; b; b = b->next) {
        if (!b->free || b->size < n) continue;
        if (b->size >= n + sizeof(Block) + 16) {
            Block *nb = (Block *)((uint8_t *)(b + 1) + n);
            nb->size = b->size - n - sizeof(Block);
            nb->next = b->next;
            nb->free = 1;
            b->next = nb;
            b->size = n;
        }
        b->free = 0;
        return b + 1;
    }
    return 0;
}
void free(void *p) {
    if (!p) return;
    Block *b = (Block *)p - 1;
    b->free = 1;
    for (Block *q = s_free_list; q; q = q->next) {
        if (q->free && q->next && q->next->free && (uint8_t *)(q + 1) + q->size == (uint8_t *)q->next) {
            q->size += sizeof(Block) + q->next->size;
            q->next = q->next->next;
        }
    }
}
void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    Block *b = (Block *)p - 1;
    if (n <= b->size) return p;
    void *q = malloc(n);
    if (!q) return 0;
    memcpy(q, p, b->size);
    free(p);
    return q;
}
void *calloc(size_t a, size_t b) {
    size_t n = a * b;
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}
long atol(const char *s) { return atoi(s); }
unsigned long strtoul(const char *s, char **end, int base) {
    unsigned long v = 0; if (!base) base = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (;; s++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 32) >= 'a' && (*s | 32) <= 'f' ? (*s | 32) - 'a' + 10 : 99;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)s;
    return v;
}
