/* psxlion/rt/watch.c — bring-up audit of every write into the low 0x500 bytes of guest RAM.
 *
 * The stall analysis found RAM 0x0..0x43F zero (vector stubs 0x0..0xF0, __globals 0x100, the head of the
 * A0 table 0x200..0x43F) while RAM 0x440..0x4FF still held the tail of the table OpenBIOS had copied
 * there. Boot itself writes this window (the _boot crt0 zeroes 0x60's neighbour stores, copyA0table()
 * memcpy's 0x300 bytes to 0x200, installKernelHandlers() writes 0x80/0xA0/0xB0/0xC0), so *somebody*
 * zeroed it afterwards. This module latches the culprit:
 *
 *   - every write that lands in [0, 0x500) is compared against a shadow copy; a byte that was nonzero
 *     and became zero logs one ZERO block with the exact store pc, last function entry, sp, ra and SR,
 *   - the first nonzero write into each 4-byte slot 0x0..0x100 and 0x200..0x220 logs a FIRST block
 *     (did the vector install / A0 table copy actually land?),
 *   - stores dropped because SR.IsC (cache isolation) was set log a SUPP block (they never reach RAM),
 *   - all-zero DMA fills into the window log a D block with size and context,
 *   - ww_checkpoint() (game start, first weird syscall, every 250 VI frames) logs a digest of the
 *     window so the zeroing can be placed in time even if the individual writes are never caught.
 *
 * Writers covered: psx_write_word/half/byte (core.c), the CD sector DMA loop (cd.c) and the DMA
 * channel writes (hw.c ram_wr). Compile-time always on; the cost is one compare per RAM store.
 */
#include "psx_runtime.h"

extern void lion_log(const char *);
extern void lion_logf(const char *, uint32_t, uint32_t);
extern uint8_t *g_psx_ram;
extern CPUState g_cpu;
extern uint32_t g_debug_last_store_pc;
extern volatile uint32_t g_psx_last_fn_entry;

#define WW_HI       0x500u
#define WW_ZERO_HI  0x440u

/* The store that zeroed the window: `sh t1,0(a1)` at 0x8009CB5C in the XA-ADPCM decoder 0x8009C8F4.
 * Its a1 should be v1 + v0*0xA0000 + 0x50008 with v1 = *(gp+36), v0 = *(v1) (caller 0x80083C0C..0x80083C34),
 * so the first stores log the live registers and the pointer chain to expose what the arithmetic really is. */
#define WW_DEC_PC   0x8009CB5Cu
#define WW_DEC_HI   64                           /* stores logged with full context */
#define WW_ENT1     0x8009C99Cu                  /* XA decoder prologue stores: a1 = the caller's destination */
#define WW_ENT2     0x8009C9A0u
#define WW_T1CONST  0x9C8C0u                     /* the .word 0xFFFFFF the decoder uses for its ring stride */
#define WW_PTRDESC  0x10D3A0u                    /* gp+36: the allocated decode buffer pointer */
#define WW_STATE_LO 0x9CC34u                     /* decoder save/restore block: a0 a1 v0 v1 t4 t5 t7 t8 t9 */
#define WW_STATE_HI 0x9CC58u

static uint8_t s_shadow[WW_HI];
static uint8_t s_seen[WW_HI >> 2];              /* one flag per 4-byte slot */
static int s_zero_n, s_first_n, s_supp_n, s_dma_n, s_ck_n, s_dec_n, s_dp_n;
static int s_ent_n, s_near_n, s_desc_n, s_t1_n, s_st_n, s_jmp_n, s_back_n;
static uint32_t s_last_a1; static int s_last_ok;

/* ---------------------------------------------------------------------------------------------------------
 * WINDOW MODE (bring-up probe): a raw binary trace of every decoder store whose destination lands in
 * [0x192400, 0x194600] — the stretch where the native reference exits its first decode call on the
 * end-marker (a1 = 0x193454) while Lion keeps walking. The buffer sits at Lion 0x220000 (link.ld
 * .wintrace, NOLOAD, not part of the image) so one --dump-ram 1F0000,50000 captures ring + trace.
 * Record: u32 pc, a1, val, v0, v1, t5 (24 bytes) after a u32 count. */
#define WW_WINDOW_MODE 1
#define WW_WIN_LO 0x192400u
#define WW_WIN_HI 0x194600u
#define WW_REC_SZ 28u                            /* u32 pc, a, val, v0, v1, t5, a0 */
__attribute__((section(".wintrace"), used, aligned(8))) uint8_t g_wintrace[0x20000];
static uint32_t s_win_n;
static void win_log_raw(uint32_t pc, uint32_t a, uint32_t val) {
    if (s_win_n >= (0x20000u / WW_REC_SZ) - 1u) return;
    uint32_t *e = (uint32_t *)(g_wintrace + 4u + s_win_n * WW_REC_SZ);
    e[0] = pc; e[1] = a; e[2] = val;
    e[3] = g_cpu.gpr[2]; e[4] = g_cpu.gpr[3]; e[5] = g_cpu.gpr[13];
    e[6] = g_cpu.gpr[4];                     /* a0 = the decoder's input pointer */
    *(volatile uint32_t *)g_wintrace = ++s_win_n;
}
static void win_log(uint32_t pc, uint32_t a1, uint32_t val) {
    if (1) { (void)pc; (void)a1; (void)val; return; }   /* decoder trace retired: answer captured (see notes) */
    if (a1 < WW_WIN_LO || a1 > WW_WIN_HI) return;
    win_log_raw(pc, a1, val);
}
/* Refill-load probe: the generated lhu sites inside the decoder are patched to call this so the
 * loaded value and the input pointer land in the same stream as the stores. pc is tagged into the
 * high nibble so load records are distinguishable from store records. */
extern uint16_t psx_read_half(uint32_t a);
uint16_t ww_lhu(CPUState *cpu, uint32_t addr, int rt, uint32_t pc) {
    uint16_t v = psx_read_half(addr);
    (void)cpu; (void)rt; (void)addr; (void)pc;
    return v;
}

/* ---- STREAM-FILL TRACE (bring-up probe 2) ---------------------------------------------------------------
 * The decoder reads the XA bitstream from 0x11224C + k*0x4EC0; the first window's data ends early on Lion
 * (0x112A20) but not on the native reference (0x112D88), so the difference is in whatever fills the window.
 * Every store that lands in [0x112240, 0x112E40) is recorded with its pc, address, value, length and kind
 * (0 w 1 h 2 b 3 CD-DMA 4 DMA) into the same .wintrace buffer. CD controller activity is interleaved with
 * pc tags 0xCD00xxxx (command), 0xCD10xxxx (sector delivered), 0xCD20xxxx (CD DMA to RAM). */
#define WW_FILL_LO 0x112240u
#define WW_FILL_HI 0x112E40u
static void ww_fill_rec(uint32_t pc, uint32_t a, uint32_t val, uint32_t len, uint32_t kind) {
    if (s_win_n >= (0x20000u / WW_REC_SZ) - 1u) return;
    uint32_t *e = (uint32_t *)(g_wintrace + 4u + s_win_n * WW_REC_SZ);
    e[0] = pc; e[1] = a; e[2] = val; e[3] = len | (kind << 8); e[4] = 0; e[5] = 0; e[6] = 0;
    *(volatile uint32_t *)g_wintrace = ++s_win_n;
}
void ww_buf_write(uint32_t pc, uint32_t phys, uint32_t val, uint32_t len, uint32_t kind) {
    if (phys + len <= WW_FILL_LO || phys >= WW_FILL_HI) return;
    ww_fill_rec(pc, phys, val, len, kind);
}
void ww_cd_event(uint32_t tag, uint32_t a, uint32_t b) { ww_fill_rec(tag, a, b, 0, 0); }
/* CD channel-3 DMA into RAM: pc tag 0xCD200000, a = dest phys, val = byte count, len field = FIFO
 * bytes remaining at entry (avail), kind 3. Records only transfers that touch the stream window. */
void ww_cd_dma(uint32_t dst, uint32_t bytes, uint32_t avail) {
    if (dst + bytes <= WW_FILL_LO || dst >= WW_FILL_HI) return;
    ww_fill_rec(0xCD200000u, dst, bytes, avail, 3);
}

/* First store of the run at each XA-driver store pc, with the a1 it used: a compact timeline of the
 * destination pointer across decoder calls. */
static const uint32_t s_dec_pcs[9] = { 0x8009C99Cu, 0x8009C9A0u, 0x8009CA74u, 0x8009CAB4u, 0x8009CB5Cu,
                                       0x8009CB7Cu, 0x8009CB9Cu, 0x8009CBACu, 0x8009CBD4u };
static uint8_t s_dec_seen[9];

/* Called only for stores inside the XA driver (pc 0x8009C800..0x8009D000 — gated in core.c) or the two
 * special data addresses. Answers: where a1 starts, where it is saved/restored, and how it jumps. */
void ww_pc_probe(uint32_t pc, uint32_t a, uint32_t kind) {
    win_log(pc, a, kind);
#if !WW_WINDOW_MODE
    uint32_t p = a & 0x1FFFFFFFu;
    if (p == WW_T1CONST) {
        if (s_t1_n < 8) { s_t1_n++; lion_logf("ww T1 pc/val", pc, kind); }
        return;
    }
    if (p == WW_PTRDESC) {
        if (s_desc_n < 8) { s_desc_n++; lion_logf("ww DESC pc/val", pc, kind); lion_logf("ww DESC gp/sp", g_cpu.gpr[28], g_cpu.gpr[29]); }
        return;
    }
    if (p >= WW_STATE_LO && p <= WW_STATE_HI) {                     /* decoder save (or restore data) */
        if (s_st_n < 40) { s_st_n++; lion_logf("ww ST pc/val", pc, kind); }
        return;
    }
    if (pc == WW_ENT1 || pc == WW_ENT2) {
        if (s_ent_n < 12) {
            s_ent_n++;
            lion_logf("ww ENT pc/a1", pc, a);
            lion_logf("ww ENT a0/gp", g_cpu.gpr[4], g_cpu.gpr[28]);
            lion_logf("ww ENT ra/sp", g_cpu.gpr[31], g_cpu.gpr[29]);
        }
        return;
    }
    for (int i = 0; i < 9; i++) {
        if (pc == s_dec_pcs[i] && !s_dec_seen[i]) {
            s_dec_seen[i] = 1;
            lion_logf("ww PC first pc/a1", pc, a);
            lion_logf("ww PC a0/gp", g_cpu.gpr[4], g_cpu.gpr[28]);
            return;
        }
    }
    /* Continuity of the destination walk: anything other than +0/+2/+4 forward means a rebase/rewind. */
    if (s_last_ok) {
        int32_t d = (int32_t)(a - s_last_a1);
        if (d > 4 && s_jmp_n < 40) {
            s_jmp_n++;
            lion_logf("ww JUMP pc/a1", pc, a);
            lion_logf("ww JUMP last/a0", s_last_a1, g_cpu.gpr[4]);
            lion_logf("ww JUMP gp/ra", g_cpu.gpr[28], g_cpu.gpr[31]);
        } else if (d < 0 && s_back_n < 40) {
            s_back_n++;
            lion_logf("ww BACK pc/a1", pc, a);
            lion_logf("ww BACK last/a0", s_last_a1, g_cpu.gpr[4]);
            lion_logf("ww BACK gp/ra", g_cpu.gpr[28], g_cpu.gpr[31]);
        }
    }
    s_last_a1 = a; s_last_ok = 1;
    if (s_near_n < 120 && p >= 0x1FF000u && p < 0x200800u) {
        s_near_n++;
        lion_logf("ww NEAR pc/a1", pc, a);
        lion_logf("ww NEAR a0/dst", g_cpu.gpr[4], p);
    }
#endif /* !WW_WINDOW_MODE */
}

static uint32_t ww_ram32(uint32_t a) {
    const uint8_t *p = g_psx_ram + a;
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t ww_old32(uint32_t a) {
    a &= ~3u;
    return s_shadow[a] | (uint32_t)s_shadow[a + 1] << 8 | (uint32_t)s_shadow[a + 2] << 16 | (uint32_t)s_shadow[a + 3] << 24;
}
/* Called after the store actually happened (kind: 0 sw, 1 sh, 2 sb, 3 CD DMA, 4 DMA channel). */
void ww_after(uint32_t phys, uint32_t len, uint32_t kind) {
    if (phys >= WW_HI || !len) return;
    if (len > WW_HI - phys) len = WW_HI - phys;
#if WW_WINDOW_MODE
    (void)kind; return;                 /* window mode: keep the ring free for the walk trace */
#endif

    /* Register context for the decoder's stores: a1 is the destination pointer the game computed. */
    if (g_debug_last_store_pc == WW_DEC_PC && s_dec_n < WW_DEC_HI) {
        s_dec_n++;
        uint32_t a1 = g_cpu.gpr[5], a0 = g_cpu.gpr[4];
        lion_logf("ww DEC a1raw/a0", a1, a0);
        lion_logf("ww DEC a2/a3", g_cpu.gpr[6], g_cpu.gpr[7]);
        lion_logf("ww DEC gp/ra", g_cpu.gpr[28], g_cpu.gpr[31]);
        /* Reconstruct the caller's arithmetic: v1 = *(gp+36), v0 = *(v1), expected a1 = v1 + v0*0xA0000 + 0x50008.
         * Logged once for the first store and then for the first few stores that land low (the anomaly). */
        if (s_dp_n < 6 && (s_dec_n == 1 || (a1 & 0x1FFFFFu) < WW_HI)) {
            s_dp_n++;
            uint32_t v1 = ww_ram32(((g_cpu.gpr[28] + 36) & 0x1FFFFFu));
            uint32_t p1 = v1 & 0x1FFFFFFFu;
            uint32_t v0 = (p1 < 0x800000u) ? ww_ram32(v1 & 0x1FFFFFu) : 0x5E5E5E5Eu;  /* 5E.. = v1 not RAM-backed */
            lion_logf("ww DP gp/at36", g_cpu.gpr[28], v1);
            lion_logf("ww DP v0/exp-a1", v0, v1 + v0 * 0xA0000u + 0x50008u);
            lion_logf("ww DP a1/dst", a1, a1 & 0x1FFFFFu);
            lion_logf("ww DP dlt/v0q", a1 - v1 - 0x50008u, (a1 - v1 - 0x50008u) / 0xA0000u);
        }
    }

    if (kind >= 3 && len >= 32) {
        uint32_t nz = 0;
        for (uint32_t i = 0; i < len; i++) if (g_psx_ram[phys + i]) { nz = 1; break; }
        if (!nz && s_dma_n < 30) {
            s_dma_n++;
            lion_logf("ww D zero-fill a/len", phys & ~3u, len);
            lion_logf("ww D kind/pc", kind, g_debug_last_store_pc);
            lion_logf("ww D fn/sp", g_psx_last_fn_entry, g_cpu.gpr[29]);
            lion_logf("ww D ra/sr", g_cpu.gpr[31], g_cpu.cop0[12]);
        }
    }

    for (uint32_t i = 0; i < len; i++) {
        uint32_t a = phys + i;
        uint8_t nv = g_psx_ram[a], ov = s_shadow[a];
        if (nv == 0 && ov != 0 && a < WW_ZERO_HI && s_zero_n < 40) {
            s_zero_n++;
            lion_logf("ww ZERO a/kind", a & ~3u, kind);
            lion_logf("ww   old/pc", ww_old32(a), g_debug_last_store_pc);
            lion_logf("ww   fn/sp", g_psx_last_fn_entry, g_cpu.gpr[29]);
            lion_logf("ww   ra/sr", g_cpu.gpr[31], g_cpu.cop0[12]);
            lion_logf("ww   a1/a0", g_cpu.gpr[5], g_cpu.gpr[4]);
        }
        if (nv != 0 && a < WW_HI) {
            uint32_t slot = a & ~3u;
            if ((slot < 0x100u || (slot >= 0x200u && slot < 0x220u)) && !s_seen[slot >> 2]) {
                s_seen[slot >> 2] = 1;
                if (s_first_n < 48) {
                    s_first_n++;
                    lion_logf("ww FIRST a/val", slot, ww_ram32(slot));
                    lion_logf("ww   pc/fn", g_debug_last_store_pc, g_psx_last_fn_entry);
                }
            }
        }
        s_shadow[a] = nv;
    }
}

/* A store that would have landed in the window but was dropped by SR.IsC. */
void ww_suppressed(uint32_t phys, uint32_t val, uint32_t size) {
    if (phys >= WW_HI || s_supp_n >= 40) return;
    s_supp_n++;
    lion_logf("ww SUPP a/val", phys & ~3u, val);
    lion_logf("ww   size/pc", size, g_debug_last_store_pc);
    lion_logf("ww   fn/sp", g_psx_last_fn_entry, g_cpu.gpr[29]);
}

/* Digest of the window: called at landmark points (tag: 1 game start, 2 first weird syscall) and by
 * ww_checkpoint_frame() every 250 VI frames. Reads the RAM itself, not the shadow. */
void ww_checkpoint(uint32_t tag) {
    uint32_t nz = 0, h = 0;
    for (uint32_t a = 0; a < WW_HI; a++) { uint8_t b = g_psx_ram[a]; if (b) nz++; h = h * 33u + b; }
    if (tag == 1) lion_log("ww CKPT game-start");
    else if (tag == 2) lion_log("ww CKPT weird-syscall");
    else lion_log("ww CKPT");
    lion_logf("ww C tag/nonzero", tag, nz);
    lion_logf("ww C w80/wA0", ww_ram32(0x80), ww_ram32(0xA0));
    lion_logf("ww C at200/at440", ww_ram32(0x200), ww_ram32(0x440));
    lion_logf("ww C g100/w60", ww_ram32(0x100), ww_ram32(0x60));
    lion_logf("ww C hash/pc", h, g_debug_last_store_pc);
}
void ww_checkpoint_frame(uint32_t frame) {
    if (!frame || (frame % 250u) != 0 || s_ck_n >= 16) return;
    s_ck_n++;
    ww_checkpoint(0x10000u | frame);
}
