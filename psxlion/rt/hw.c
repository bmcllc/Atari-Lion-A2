/* psxlion/rt/hw.c — memory-mapped PS1 hardware and the Lion log ring.
 *
 * Skeleton: I_STAT/I_MASK are real; GPU registers stop the machine at the first access (bring-up milestone);
 * every other register reads back what was written (memory control, timers, DMA, ...) and is logged once.
 */
#include <stdint.h>

#define LOG_ADDR 0x001F0000u
#define LOG_SIZE 0x20000u                  /* 128 KB, still inside --until-log's 0x1F0000..0x21FFFF window */
#define LOG_CAP  (LOG_SIZE - 4u)
/* A ring: the first word counts every byte ever written, the text wraps at LOG_CAP (logcat.py puts it back in order), so a
 * long run keeps its latest lines instead of its first ones. */
static void log_put(char c) {
    volatile uint32_t *len = (volatile uint32_t *)LOG_ADDR;
    volatile char *buf = (volatile char *)(LOG_ADDR + 4);
    uint32_t n = *len;
    buf[n % LOG_CAP] = c;
    *len = n + 1;
}
void lion_log(const char *s) {
    for (; *s; ++s) log_put(*s);
    log_put('\n');
}
int g_ww_log_enabled;
void lion_log_reset(void) {
    *(volatile uint32_t *)LOG_ADDR = 0;
    g_ww_log_enabled = 1;
}
static void hex(char *p, uint32_t v, int digits) { for (int i = digits - 1; i >= 0; i--) { *p++ = "0123456789ABCDEF"[(v >> (i * 4)) & 15]; } }
void lion_logf(const char *what, uint32_t a, uint32_t b) {
    char line[96]; int n = 0;
    while (what[n] && n < 60) { line[n] = what[n]; n++; }
    line[n++] = ' '; hex(line + n, a, 8); n += 8; line[n++] = ' '; hex(line + n, b, 8); n += 8; line[n] = 0;
    lion_log(line);
}

extern volatile uint32_t g_psx_last_fn_entry;
extern uint32_t g_debug_last_store_pc;
uint32_t i_stat, i_mask;                       /* 0x1F801070 / 0x1F801074 */
static uint8_t s_io[0x2000];                   /* shadow of 0x1F801000..0x1F802FFF */
extern uint32_t cd_read(uint32_t reg);
extern void cd_write(uint32_t reg, uint32_t v);
extern void cd_dma(uint8_t *dst, uint32_t bytes);
extern uint32_t sio_read(uint32_t reg, int size);
extern void sio_write(uint32_t reg, uint32_t v, int size);
/* GPU and MDEC live in their own translation units. */
extern void gpu_gp0_word(uint32_t w);
extern uint32_t gpu_read_gp0(void);
extern void gpu_write_gp1(uint32_t v);
extern uint32_t gpu_gpustat(void);
extern uint32_t mdec_read(uint32_t addr);
extern void mdec_write(uint32_t addr, uint32_t v);
extern void mdec_dma_write_word(uint32_t v);
extern uint32_t mdec_dma_read_word(void);
extern uint32_t spu_read(uint32_t phys, int size);                 /* spu.c */
extern void spu_write(uint32_t phys, uint32_t val, int size);
extern void spu_dma_write(uint32_t w);
extern uint32_t spu_dma_read(void);
static int s_logged;
#ifdef PSX_TRACE_EVT
static int s_evt;
extern int fntrace_is_game_started(void);
#define EVT(w, a, b) do { if (s_evt < 1500 && fntrace_is_game_started()) { s_evt++; lion_logf(w, a, b); } } while (0)
#else
#define EVT(w, a, b) do { } while (0)
#endif
static void gpu_note_draw(uint32_t cmd, uint32_t how);

/* ---- root counters (0x1F801100/10/20: value, mode, target), driven by the Tuffy time base (25 MHz) scaled to the PS1's
 * 33.8688 MHz. Sources: 0 = system clock (dot clock ~ /6 and hblank ~ /2148 for timers 0 and 1; timer 2 can be /8). ------ */
/* Until the game starts the clock runs PSX_FAST_BIOS times faster: the BIOS's boot waits are timed on the root counters
 * (~5 s of PS1 clock after the logo's CD Pause) and nothing it does depends on the real rate. 1 disables it. */
#ifndef PSX_FAST_BIOS
#define PSX_FAST_BIOS 16
#endif
extern int fntrace_is_game_started(void);
static uint64_t s_tb0, s_clk_base;
static int s_clk_fast = PSX_FAST_BIOS > 1;
static uint64_t ps1_clk(void) {
    uint32_t hi, lo, hi2;
    do { __asm__ volatile("mftbu %0" : "=r"(hi)); __asm__ volatile("mftb %0" : "=r"(lo)); __asm__ volatile("mftbu %0" : "=r"(hi2)); } while (hi != hi2);
    uint64_t tb = ((uint64_t)hi << 32) | lo;
    if (!s_tb0) s_tb0 = tb;
    uint64_t mult = s_clk_fast ? PSX_FAST_BIOS : 1;
    uint64_t clk = s_clk_base + (((tb - s_tb0) * 11097u * mult) >> 13);   /* x 1.35468 (exact: 1.354752) */
    if (s_clk_fast && fntrace_is_game_started()) { s_clk_fast = 0; s_clk_base = clk; s_tb0 = tb; }   /* continuous at the switch */
    return clk;
}
uint64_t psx_clock(void) { return ps1_clk(); }                 /* sio.c paces its bytes with it */
typedef struct { uint32_t start_val, mode, target; uint64_t start_clk; } Timer;
static Timer s_tm[3];
static uint32_t tm_div(int n) {
    uint32_t src = (s_tm[n].mode >> 8) & 3;
    if (n == 0) return (src & 1) ? 6 : 1;
    if (n == 1) return (src & 1) ? 2148 : 1;
    return (src & 2) ? 8 : 1;
}
static uint32_t tm_value(int n) {
    Timer *t = &s_tm[n];
    uint64_t ticks = (ps1_clk() - t->start_clk) / tm_div(n);
    uint32_t period = (t->mode & 8) ? (t->target & 0xFFFF) + 1 : 0x10000;
    if (!period) period = 0x10000;
    return (uint32_t)((t->start_val + ticks) % period);
}
static void tm_set(int n, uint32_t val) { s_tm[n].start_val = val & 0xFFFF; s_tm[n].start_clk = ps1_clk(); }

/* ---- DMA (0x1F801080..0x1F8010F7): transfers complete at once. Channels: 0 MDEC in, 1 MDEC out, 2 GPU, 3 CD, 4 SPU,
 * 5 PIO, 6 OTC. Only OTC has an effect on memory so far; the others consume their data (GPU/SPU/CD/MDEC modules
 * hook in here). -------------------------------------------------------------------------------------------- */
extern uint8_t *g_psx_ram;
static uint32_t s_dpcr = 0x07654321, s_dicr;
static int s_dma_delay[7]; static uint32_t s_dma_busy;
static void io32(uint32_t phys, uint32_t v) { uint8_t *p = s_io + (phys - 0x1F801000u); p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t io32r(uint32_t phys) { const uint8_t *p = s_io + (phys - 0x1F801000u); return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void dma_irq_update(void) {
    uint32_t en = (s_dicr >> 16) & 0x7F, fl = (s_dicr >> 24) & 0x7F;
    uint32_t master = (s_dicr & 0x8000u) || ((s_dicr & 0x800000u) && (en & fl));
    s_dicr = (s_dicr & 0x7FFFFFFFu) | (master ? 0x80000000u : 0);
    if (master) i_stat |= 1u << 3;
}
static uint32_t ram_rd(uint32_t a) {
    const uint8_t *p = g_psx_ram + (a & 0x1FFFFCu);
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
void ww_after(uint32_t phys, uint32_t len, uint32_t kind);      /* watch.c */
void ww_buf_write(uint32_t pc, uint32_t phys, uint32_t val, uint32_t len, uint32_t kind);
static void ram_wr(uint32_t a, uint32_t v) {
    uint8_t *p = g_psx_ram + (a & 0x1FFFFCu);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    if ((a & 0x1FFFFCu) < 0x500u) ww_after(a & 0x1FFFFCu, 4, 4);
    else ww_buf_write(0xD0000000u, a & 0x1FFFFCu, v, 4, 4);
}
static void dma_finish(int ch, uint32_t madr, uint32_t bcr);
extern int mdec_dma_write_ready(void);
extern int mdec_dma_read_ready(void);

/* MDEC channels move data only while the MDEC asks for it (DREQ): input while it accepts words, output while a decoded block
 * waits. libpress arms channel 1 (out) before channel 0 (in), so an output transfer started on an idle MDEC must wait for the
 * decode instead of reading nothing -- otherwise every slice of a picture lands one transfer late. A waiting channel keeps its
 * address and remaining words, and mdec_pump() moves whatever both sides allow whenever either one may have changed. */
static uint32_t s_md_madr[2], s_md_left[2], s_md_bcr[2], s_dma_busy_md; static int32_t s_md_step[2];
static void mdec_pump(void) {
    for (int moved = 1; moved;) {
        moved = 0;
        for (int ch = 0; ch < 2; ch++) {
            if (!(s_dma_busy_md & (1u << ch))) continue;
            while (s_md_left[ch] && (ch == 0 ? mdec_dma_write_ready() : mdec_dma_read_ready())) {
                if (ch == 0) mdec_dma_write_word(ram_rd(s_md_madr[0]));
                else ram_wr(s_md_madr[1], mdec_dma_read_word());
                s_md_madr[ch] += s_md_step[ch]; s_md_left[ch]--; moved = 1;
            }
            if (!s_md_left[ch]) { s_dma_busy_md &= ~(1u << ch); dma_finish(ch, s_md_madr[ch], s_md_bcr[ch]); }
        }
    }
}
void hw_mdec_poke(void) { if (s_dma_busy_md) mdec_pump(); }        /* CPU access to the MDEC registers (mdec_read/write callers) */

static void dma_run(int ch) {
    uint32_t base = 0x1F801080u + 0x10u * ch, madr = io32r(base) & 0x1FFFFCu, bcr = io32r(base + 4), chcr = io32r(base + 8);
    if (!(s_dpcr & (8u << (ch * 4)))) { EVT("evt DMA start with channel disabled; ch / DPCR", ch, s_dpcr); return; }   /* channel disabled in DPCR */
    uint32_t dir = chcr & 1u, sync = (chcr >> 9) & 3u;
    int32_t step = (chcr & 2u) ? -4 : 4;
    uint32_t words = (sync == 0) ? (bcr & 0xFFFFu) : (bcr & 0xFFFFu) * (bcr >> 16);
    if (sync != 2 && !words) words = 0x10000;

    switch (ch) {
    case 0: case 1:                                                  /* MDEC in (RAM -> MDEC) / out (MDEC -> RAM): paced by DREQ */
        if (dir != (ch == 0 ? 1u : 0u)) break;
        s_md_madr[ch] = madr; s_md_left[ch] = words; s_md_step[ch] = step; s_md_bcr[ch] = bcr;
        s_dma_busy_md |= 1u << ch;
        mdec_pump();
        return;                                                      /* dma_finish() runs when the last word moved */
    case 2:                                                          /* GPU */
        if (dir && sync == 2) {                                      /* linked list of {count, next, words...} nodes */
            uint32_t a = madr;
            for (int k = 0; k < 65536 && a != 0xFFFFFFu; k++) {
                uint32_t hdr = ram_rd(a), n = hdr >> 24, next = hdr & 0xFFFFFFu;
                for (uint32_t i = 0; i < n; i++) gpu_gp0_word(ram_rd((a + 4 + i * 4) & 0x1FFFFCu));
                if (next == 0xFFFFFFu) { a = next; break; }
                if ((next & 0x1FFFFCu) == (a & 0x1FFFFCu)) break;
                a = next & 0x1FFFFCu;
            }
            madr = a;
        } else if (dir) {
            for (uint32_t i = 0; i < words; i++, madr += step) gpu_gp0_word(ram_rd(madr));
        } else {
            for (uint32_t i = 0; i < words; i++, madr += step) ram_wr(madr, gpu_read_gp0());
        }
        break;
    case 3: {                                                        /* CD -> RAM */
        uint32_t n = (sync == 0) ? (bcr & 0xFFFFu) : (bcr & 0xFFFFu) * (bcr >> 16);
        if (!n) n = 0x10000;
        EVT("evt cd dma madr / words", madr, n);
        cd_dma(g_psx_ram + (madr & 0x1FFFFCu), n * 4);
        { extern void dirty_ram_mark_executable_range(uint32_t, uint32_t); dirty_ram_mark_executable_range(madr & 0x1FFFFCu, n * 4); }
        break;
    }
    case 6: {                                                        /* OTC: a reversed linked list of empty nodes */
        uint32_t n = bcr & 0xFFFFu; if (!n) n = 0x10000;
        for (uint32_t i = 0; i < n; i++, madr -= 4) {
            uint32_t v = (i == n - 1) ? 0x00FFFFFFu : ((madr - 4) & 0x1FFFFFu);
            ram_wr(madr, v);
        }
        break;
    }
    case 4:                                                          /* SPU sound RAM, at the SPU's transfer address */
        if (dir) for (uint32_t i = 0; i < words; i++, madr += step) spu_dma_write(ram_rd(madr));
        else for (uint32_t i = 0; i < words; i++, madr += step) ram_wr(madr, spu_dma_read());
        break;
    default: break;                                                  /* PIO: completion only */
    }
    dma_finish(ch, madr, bcr);
}
static void dma_finish(int ch, uint32_t madr, uint32_t bcr) {
    uint32_t base = 0x1F801080u + 0x10u * ch, chcr = io32r(base + 8);
    io32(base + 0, madr & 0x1FFFFCu);
    /* The data moved already; the channel stays busy (CHCR bit 24) for as long as the transfer would take, and only then does
     * the DMA interrupt fire -- software starts a transfer and keeps working before it checks for completion. */
    io32(base + 8, chcr & ~0x10000000u);
    s_dma_delay[ch] = 24 + (ch == 3 ? 128 : 16) + (bcr & 0xFFFF) / 4;
    s_dma_busy |= 1u << ch;
}
void hw_tick(void) {
    if (!s_dma_busy) return;
    for (int ch = 0; ch < 7; ch++) {
        if (!(s_dma_busy & (1u << ch)) || --s_dma_delay[ch] > 0) continue;
        s_dma_busy &= ~(1u << ch);
        uint32_t base = 0x1F801080u + 0x10u * ch;
        io32(base + 8, io32r(base + 8) & ~0x01000000u);               /* done: clear busy */
        if ((s_dicr >> (16 + ch)) & 1u) s_dicr |= 1u << (24 + ch);
        dma_irq_update();
        { extern int g_ww_log_enabled; static int k;
          if (ch == 3 && g_ww_log_enabled && k < 300) { k++; lion_logf("DMA3 done dicr / i_stat", s_dicr, i_stat); } }
    }
}
int g_gpu_touched;

static void gpu_note_draw(uint32_t cmd, uint32_t how) {         /* bring-up milestone: the first drawing command */
    static int once; extern int fntrace_is_game_started(void);
    if (once || !fntrace_is_game_started()) return;          /* the BIOS draws too (a black clear at power-up): wait for the game */
    once = 1;
    lion_logf("psxlion: first GPU draw command / source", cmd, how);
    lion_logf("psxlion: first GPU draw; last function", g_psx_last_fn_entry, 0);
}
static void stop_at_gpu(const char *kind, uint32_t phys, uint32_t val) {
    if (phys == 0x1F801810u && (val >> 29) >= 1 && (val >> 29) <= 3) gpu_note_draw(val, 1);   /* polygons, lines, rectangles */
#ifdef PSX_GPU_PASSTHROUGH      /* bring-up aid: log the first accesses, then let the BIOS go on with a "ready" GPU */
    static int n; if (n < 24) { n++; lion_logf(kind, phys, val); }
    return;
#endif
    lion_logf(kind, phys, val);
    lion_logf("psxlion: first GPU access; last function / count =", g_psx_last_fn_entry, 0);
    g_gpu_touched = 1;
    for (;;) { }
}

#ifdef PSX_TRACE_MMIO
static int s_trace_on, s_trace_n; static uint32_t s_trace_last_a, s_trace_last_v, s_trace_rep;
void hw_trace_arm(void) { s_trace_on = 1; }
static void tr(char rw, uint32_t a, uint32_t v) {
    if (!s_trace_on || s_trace_n >= 120) return;
    if (a == s_trace_last_a && v == s_trace_last_v) { s_trace_rep++; return; }
    s_trace_last_a = a; s_trace_last_v = v; s_trace_n++;
    lion_logf(rw == 'r' ? "mmio R addr / val" : "mmio W addr / val", a, v);
}
#else
#define tr(rw, a, v) do { } while (0)
#endif

static uint32_t mmio_read_inner(uint32_t phys, int size);
/* GPUSTAT: ready bits (26-28), bit 13 = interlace field, bit 31 = even/odd line; both follow the Lion's VI frame counter. */
#ifndef PSX_GPUSTAT_MODE
#define PSX_GPUSTAT_MODE 3
#endif
static uint32_t gpustat(void) {
    uint32_t f = *(volatile uint32_t *)0x1F030010u & 1u, v = 0x1C000000u;
    switch (PSX_GPUSTAT_MODE) {
    case 0: v |= f << 13; break;                                  /* A: field only */
    case 1: v |= 1u << 13; v |= f << 31; break;                   /* B: interlaced, line parity toggles */
    case 2: v |= (f << 13) | (f << 31); break;                    /* C: both follow the frame */
    case 3: v |= (f << 13) | ((f ^ 1u) << 31); break;             /* D: opposite phases */
    }
    return v;
}
uint32_t psx_mmio_read(uint32_t phys, int size) { uint32_t v = mmio_read_inner(phys, size); tr('r', phys, v); return v; }
static uint32_t mmio_read_inner(uint32_t phys, int size) {
    /* Sub-word reads of the interrupt and DMA registers see the same 32-bit register (libcd reads DICR's enable byte with
     * lbu 0x1F8010F6). */
    if (size < 4 && ((phys >= 0x1F801070u && phys < 0x1F801078u) || (phys >= 0x1F801080u && phys < 0x1F801100u)))
        return mmio_read_inner(phys & ~3u, 4) >> ((phys & 3u) * 8) & (size == 1 ? 0xFFu : 0xFFFFu);
    if (phys >= 0x1F801040u && phys < 0x1F801050u) return sio_read(phys - 0x1F801040u, size);
    if (phys >= 0x1F801800u && phys <= 0x1F801803u) return cd_read(phys - 0x1F801800u);
    if (phys >= 0x1F801100u && phys < 0x1F801130u && (phys & 0xF) < 0xC) {            /* root counters */
        int n = (phys - 0x1F801100u) >> 4;
        switch (phys & 0xF) {
        case 0: return tm_value(n);
        case 4: return (s_tm[n].mode & 0x3FFu) | 0x400u;                                /* bit 10: no IRQ request */
        case 8: return s_tm[n].target;
        }
    }
    if (phys == 0x1F801070u) { extern void sio_poll(void); sio_poll(); return i_stat; }   /* drivers spin here for the pad's ACK */
    if (phys == 0x1F801074u) return i_mask;
    if (phys == 0x1F8010F0u) return s_dpcr;
    if (phys == 0x1F8010F4u) return s_dicr;
    if (phys >= 0x1F801C00u && phys < 0x1F802000u) return spu_read(phys, size);
    if (phys == 0x1F801810u) return gpu_read_gp0();
    if (phys == 0x1F801814u) return gpu_gpustat();
    if (phys >= 0x1F801820u && phys <= 0x1F801827u) return mdec_read(phys);
    if (phys >= 0x1F801000u && phys < 0x1F803000u) {
        const uint8_t *p = s_io + (phys - 0x1F801000u);
        uint32_t v = 0;
        for (int i = size - 1; i >= 0; i--) v = v << 8 | p[i];
        return v;
    }
    if (s_logged < 40) { s_logged++; lion_logf("mmio read (unmapped)", phys, size); }
    return 0;
}

static void mmio_write_inner(uint32_t phys, uint32_t val, int size);
void psx_mmio_write(uint32_t phys, uint32_t val, int size) { tr('w', phys, val); mmio_write_inner(phys, val, size); }
static void mmio_write_inner(uint32_t phys, uint32_t val, int size) {
    /* Sub-word writes to the interrupt and DMA registers merge into the 32-bit register (libcd turns a channel's DMA
     * interrupt on and off with sb 0x1F8010F6). DICR flags (bits 24-30) are write-1-to-clear: a write that does not cover
     * the top byte must not acknowledge them; I_STAT is write-0-to-clear, so its untouched bytes are written as ones. */
    if (size < 4 && ((phys >= 0x1F801070u && phys < 0x1F801078u) || (phys >= 0x1F801080u && phys < 0x1F801100u))) {
        uint32_t w = phys & ~3u, sh = (phys & 3u) * 8, m = (size == 1 ? 0xFFu : 0xFFFFu) << sh;
        uint32_t cur = w == 0x1F801070u ? 0xFFFFFFFFu : w == 0x1F8010F4u ? (s_dicr & 0x00FFFFFFu) : mmio_read_inner(w, 4);
        mmio_write_inner(w, (cur & ~m) | ((val << sh) & m), 4);
        return;
    }
    if (phys >= 0x1F801040u && phys < 0x1F801050u) { sio_write(phys - 0x1F801040u, val, size); return; }
    if (phys >= 0x1F801800u && phys <= 0x1F801803u) { cd_write(phys - 0x1F801800u, val); return; }
    if (phys >= 0x1F801100u && phys < 0x1F801130u && (phys & 0xF) < 0xC) {
        int n = (phys - 0x1F801100u) >> 4;
        switch (phys & 0xF) {
        case 0: tm_set(n, val); break;
        case 4: s_tm[n].mode = val; tm_set(n, 0); break;                                /* writing the mode resets the counter */
        case 8: s_tm[n].target = val & 0xFFFF; break;
        }
        return;
    }
    if (phys == 0x1F801070u) { i_stat &= val; return; }     /* acknowledge: write 0 clears */
    if (phys == 0x1F801074u) { i_mask = val & 0x7FF; return; }
    if (phys == 0x1F8010F0u) { s_dpcr = val; return; }
    if (phys == 0x1F8010F4u) { extern int g_ww_log_enabled; static int k; if (g_ww_log_enabled && k < 200) { k++; lion_logf("DICR write val / old", val, s_dicr); }
                               s_dicr = (s_dicr & 0xFF000000u & ~(val & 0x7F000000u)) | (val & 0x00FF803Fu) | (s_dicr & 0x80000000u); dma_irq_update(); return; }
    if (phys >= 0x1F801088u && phys <= 0x1F8010E8u && (phys & 0xF) == 8 && size == 4) {          /* CHCR of channel n */
        io32(phys, val);
        if (val & 0x01000000u) dma_run((phys - 0x1F801080u) >> 4);
        else if (phys <= 0x1F801098u) s_dma_busy_md &= ~(1u << ((phys - 0x1F801080u) >> 4));    /* MDEC channel stopped */
        return;
    }
    if (phys >= 0x1F801C00u && phys < 0x1F802000u) { spu_write(phys, val, size); return; }
    if (phys == 0x1F801810u) { gpu_gp0_word(val); return; }
    if (phys == 0x1F801814u) { gpu_write_gp1(val); return; }
    if (phys >= 0x1F801820u && phys <= 0x1F801827u) { mdec_write(phys, val); hw_mdec_poke(); return; }
    if (phys == 0x1F802080u) {                      /* the BIOS's TTY (DUART B): collect a line, send it to the log */
        static char line[100]; static int n;
        if (val == '\n' || n >= 98) { line[n] = 0; if (n) { lion_log("tty: "); lion_log(line); } n = 0; }
        else line[n++] = (char)val;
        return;
    }
    if (phys >= 0x1F801000u && phys < 0x1F803000u) {
        uint8_t *p = s_io + (phys - 0x1F801000u);
        for (int i = 0; i < size; i++) p[i] = val >> (i * 8);
        if (s_logged < 40) { s_logged++; lion_logf("mmio write", phys, val); }
        return;
    }
    if (s_logged < 40) { s_logged++; lion_logf("mmio write (unmapped)", phys, val); }
}
