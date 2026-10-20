/* psxlion/rt/stubs.c — features of the PC runtime that the Lion build turns off: PGXP (sub-pixel GTE precision),
 * widescreen hooks, and the 64-bit division helpers the compiler expects from compiler-rt. */
#include <stdint.h>

void pgxp_set_enabled(int e) { (void)e; }
void pgxp_invalidate_all(void) { }
void pgxp_suppress_begin(void) { }
void pgxp_suppress_end(void) { }
void pgxp_gte_push_sxy(int32_t x, int32_t y, uint16_t sz, uint32_t packed) { (void)x; (void)y; (void)sz; (void)packed; }
int  pgxp_get_gte_sxy_checked(uint32_t i, uint32_t expect, int check, int32_t *x, int32_t *y) { (void)i; (void)expect; (void)check; (void)x; (void)y; return 0; }
void pgxp_gte_reg_written(int reg, uint32_t v) { (void)reg; (void)v; }
void pgxp_store_gte_reg(uint32_t addr, uint8_t reg) { (void)addr; (void)reg; }
int  pgxp_load_precise_word(uint32_t a, uint32_t p, int32_t *x, int32_t *y, uint16_t *z) { (void)a; (void)p; (void)x; (void)y; (void)z; return 0; }
void gpu_pgxp_rederive_enable(void) { }
int  gpu_ws_precise_nclip_enabled(void) { return 0; }
int  gpu_ws_present_native_43(void) { return 1; }       /* the Lion shows 4:3 */
void psx_ws_note_gte_project(int n) { (void)n; }

static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem) {
    uint64_t q = 0, r = 0;
    if (!d) { if (rem) *rem = 0; return ~0ull; }
    for (int i = 63; i >= 0; i--) { r = (r << 1) | ((n >> i) & 1); if (r >= d) { r -= d; q |= 1ull << i; } }
    if (rem) *rem = r;
    return q;
}
uint64_t __udivdi3(uint64_t n, uint64_t d) { return udivmod64(n, d, 0); }
uint64_t __umoddi3(uint64_t n, uint64_t d) { uint64_t r; udivmod64(n, d, &r); return r; }
int64_t __divdi3(int64_t n, int64_t d) {
    int neg = (n < 0) ^ (d < 0);
    uint64_t q = udivmod64(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, 0);
    return neg ? -(int64_t)q : (int64_t)q;
}
int64_t __moddi3(int64_t n, int64_t d) {
    uint64_t r; udivmod64(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}
void debug_server_log_probe(uint32_t pc, struct CPUState *cpu) { (void)pc; (void)cpu; }     /* referenced by the retail BIOS image */
