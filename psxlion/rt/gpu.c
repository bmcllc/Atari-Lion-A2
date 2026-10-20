/* psxlion/rt/gpu.c — PS1 GPU command processor on top of the shared software rasterizer.
 *
 * The command decoding is ours (only the pieces MK4 needs, grown as the game trips over them), but the rasterization
 * itself is runtime/src/gpu_sw_renderer.c from psxrecomp: 15-bit VRAM, CLUTs, semi-transparency, mask bits, texture
 * windows. Display is copied from the PS1 VRAM into the Lion's SGRAM framebuffer at every VI frame, converting BGR555
 * to RGB565.
 *
 * The GPU is synchronous: a command completes inside the write that supplies its last word, and the DMA controller feeds
 * GP0 words straight from RAM. That is plenty for this port (the PS1 GPU has no timing the game can observe besides the
 * status bits, which stay "ready").
 */
#include <stdint.h>
#include "gpu_sw_renderer.h"
#include "gpu_spike.h"                      /* rasterize on the Spike unless PSX_GPU_SW */

extern void lion_log(const char *);
extern void lion_logf(const char *, uint32_t, uint32_t);

#define VRAM_W 1024
#define VRAM_H 512
#ifdef PSX_GPU_SW
#define FB_ADDR 0x10000000u                 /* SGRAM: 320x240 RGB565, stride 640 */
#else
#define FB_ADDR 0x10100000u                 /* SGRAM: 320x240 RGB565, stride 640 (the VRAM copy is at 0x10000000) */
#endif
#define FB_STRIDE 640u
#define VI_MODE   (*(volatile uint32_t *)0x1F030000u)
#define VI_FB     (*(volatile uint32_t *)0x1F030004u)
#define VI_STRIDE (*(volatile uint32_t *)0x1F030008u)
#define VI_FRAME  (*(volatile uint32_t *)0x1F030010u)

uint16_t g_vram[VRAM_W * VRAM_H] __attribute__((aligned(32)));
static uint16_t s_rgb565[32768] __attribute__((aligned(32)));
static uint16_t s_xmap[320];

/* ---- stubs for the shared rasterizer's optional facilities ------------------------------------------------- */
int g_psx_vram_dirty_tracking;
int g_ws_bd_stretch_on, g_ws_bd_stretch_pct;
void gpu_vram_dirty_mark_row_impl(uint32_t y) { (void)y; }
void gpu_vram_dirty_mark_rows(uint32_t y0, uint32_t h) { (void)y0; (void)h; }
void gpu_vram_dirty_mark_rect(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void gpu_vram_dirty_mark_all(void) { }
void gpu_vram_dirty_clear(void) { }
int gpu_vram_dirty_any(void) { return 0; }
uint32_t gpu_vram_dirty_row_count(void) { return 0; }
const uint64_t *gpu_vram_dirty_mask(void) { static const uint64_t z[8]; return z; }
int gpu_vram_dirty_verify_enabled(void) { return 0; }
void gpu_vram_dirty_set_tracking(int on) { g_psx_vram_dirty_tracking = on; }
int gpu_vram_dirty_tracking(void) { return g_psx_vram_dirty_tracking; }
int psx_ws_prim_in_backdrop(void) { return 0; }

/* ---- GPU state ------------------------------------------------------------------------------------------- */
static uint32_t s_cmd[16];
static int s_have, s_need;                  /* words in the current command / words it needs */
static uint32_t s_texpage;                  /* GP0(E1) bits 0-10 (x/y base, semi mode, depth, dither, ...) */
static uint32_t s_tw;                       /* texture window raw */
static int s_draw_x1, s_draw_y1, s_draw_x2 = 1023, s_draw_y2 = 511;
static int s_off_x, s_off_y;
static int s_mask_set, s_mask_check;
static uint32_t s_gpuread;                  /* GP1(10h) info latch */
static int s_disp_disabled = 1;
static uint32_t s_disp_x, s_disp_y, s_hres = 1, s_vres, s_depth, s_interlace;
static uint32_t s_hx1 = 0x200, s_hx2 = 0xC00;   /* GP1(06h) horizontal display range (video clocks) */
static int s_dma_dir;
static uint32_t s_flips;                    /* display-area changes: the game's own frame rate (gpu_frame_tick logs it) */

/* CPU -> VRAM stream (GP0 0xA0) */
static uint32_t s_img_x, s_img_y, s_img_w, s_img_h, s_img_row, s_img_col, s_img_left;
static uint16_t s_img_line[1024];
/* VRAM -> CPU stream (GP0 0xC0) */
static uint32_t s_rd_x, s_rd_y, s_rd_w, s_rd_h, s_rd_row, s_rd_col, s_rd_left;
/* polylines (0x48/0x58): consumed until the 0x55555555 terminator */
static int s_polyline, s_polyline_shaded, s_polyline_semi, s_polyline_phase, s_polyline_started;
static int s_pl_x, s_pl_y; static uint16_t s_pl_c;
static int s_bad_cmd_logged;

static inline int sx11(uint32_t v) { return (int)(v << 21) >> 21; }
static inline uint16_t c555(uint32_t c) {
    return (uint16_t)(((c & 0xFF) >> 3) | (((c >> 8) & 0xFF) >> 3) << 5 | (((c >> 16) & 0xFF) >> 3) << 10);
}
static inline uint32_t c565(uint32_t c) {        /* BGR555 -> RGB565 */
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (r << 11) | (((g << 1) | (g >> 4)) << 5) | b;
}
/* Visible display width from GP1(06h) and GP1(08h), as psx-spx/DuckStation compute it. The Lion VI is fixed at
 * 320x240, so s_xmap samples this width down to 320 (FMVs often use a 512-wide CRTC). */
static uint32_t display_width(void) {
    static const uint32_t cyc_lut[4] = { 10, 8, 5, 4 }, mode_lut[4] = { 256, 320, 512, 640 };
    uint32_t cycles = cyc_lut[s_hres & 3], w = mode_lut[s_hres & 3];
    uint32_t dots = (s_hx2 > s_hx1) ? (s_hx2 - s_hx1) : 0;
    if (dots) { w = ((dots / cycles) + 2u) & ~3u; if (!w) w = 4; }
    if (w > 1024) w = 1024;
    return w;
}
static void update_xmap(void) {
    uint32_t w = display_width();
    for (int i = 0; i < 320; i++) s_xmap[i] = (uint16_t)((uint32_t)i * w / 320u);
}

void gpu_init(void) {
    sw_renderer_init(g_vram);
    sw_set_draw_area(0, 0, 1023, 511);
    sw_set_draw_offset(0, 0);
    sw_set_texture_window(0);
    sw_set_semi_transparency(0, 0);
    sw_set_mask_bits(0, 0);
    sw_set_color_modulation(128, 128, 128, 0);
    for (uint32_t i = 0; i < 32768; i++) s_rgb565[i] = (uint16_t)c565(i);
    for (int i = 0; i < 320; i++) s_xmap[i] = (uint16_t)i;
    VI_MODE = 1;                                 /* 240p, 320x240 */
    VI_FB = FB_ADDR;
    VI_STRIDE = FB_STRIDE;
    uint32_t *fb = (uint32_t *)FB_ADDR;
    for (int i = 0; i < 320 * 240 / 2; i++) fb[i] = 0;
    s_disp_disabled = 1;
}

/* ---- small helpers --------------------------------------------------------------------------------------- */
static void set_draw_area(void) {
    sw_set_draw_area(s_draw_x1, s_draw_y1, s_draw_x2, s_draw_y2);
}
static uint16_t poly_texpage(uint32_t word) {
    uint16_t tp = (uint16_t)(word >> 16) & 0x1FF;
    s_texpage = (s_texpage & ~0x1FFu) | tp;     /* the poly's word latches bits 0-8, like the hardware */
    return tp;
}
/* Both take the full UV word (CLUT in bits 16-31), like the hardware. */
static inline uint16_t clut_x(uint32_t uv) { return (uint16_t)(((uv >> 16) & 0x3F) * 16); }
static inline uint16_t clut_y(uint32_t uv) { return (uint16_t)(((uv >> 16) >> 6) & 0x1FF); }
static inline void tvtx(uint32_t w, int *x, int *y) { *x = sx11(w & 0x7FF) + s_off_x; *y = sx11((w >> 16) & 0x7FF) + s_off_y; }
static void set_tex_state(uint32_t color24, int semi, int raw) {
    sw_set_color_modulation((int)(color24 & 0xFF), (int)((color24 >> 8) & 0xFF), (int)((color24 >> 16) & 0xFF), raw);
    sw_set_semi_transparency(semi, (int)((s_texpage >> 5) & 3));
}

/* ---- polygons and lines ---------------------------------------------------------------------------------- */
static void exec_poly(uint8_t op) {
    /* Opcode bits (command byte): 0x04 textured, 0x08 quad, 0x10 Gouraud, 0x02 semi, 0x01 raw. */
    int textured = (op & 4) != 0, gouraud = (op & 0x10) != 0, quad = (op & 8) != 0;
    int semi = (op >> 1) & 1, raw = op & 1;
    int n = quad ? 4 : 3;
    int x[4], y[4], u[4], v[4];
    uint32_t col[4], clutw = 0, tp = 0;
    if (gouraud) {
        if (textured) {
            /* words per vertex: color (vertex 0's shares word 0 with the command), xy, uv (+ CLUT / texpage on vertices 0 / 1) */
            for (int i = 0; i < n; i++) {
                col[i] = s_cmd[i * 3] & 0xFFFFFFu;
                tvtx(s_cmd[1 + i * 3], &x[i], &y[i]);
                u[i] = s_cmd[2 + i * 3] & 0xFF; v[i] = (s_cmd[2 + i * 3] >> 8) & 0xFF;
            }
            clutw = s_cmd[2];
            tp = poly_texpage(s_cmd[5]);          /* word 5 = vertex 1's uv/tpage */
        } else {
            for (int i = 0; i < n; i++) {
                col[i] = c555(s_cmd[i * 2]);
                tvtx(s_cmd[1 + i * 2], &x[i], &y[i]);
            }
        }
    } else {
        if (textured) {
            uint32_t color24 = s_cmd[0] & 0xFFFFFFu;
            col[0] = color24;
            for (int i = 0; i < n; i++) {
                tvtx(s_cmd[1 + i * 2], &x[i], &y[i]);
                u[i] = s_cmd[2 + i * 2] & 0xFF; v[i] = (s_cmd[2 + i * 2] >> 8) & 0xFF;
            }
            clutw = s_cmd[2];
            tp = poly_texpage(s_cmd[4]);          /* word 4 = vertex 1's uv/tpage */
        } else {
            uint16_t color = c555(s_cmd[0]);
            for (int i = 0; i < 4; i++) col[i] = color;
            for (int i = 0; i < n; i++) tvtx(s_cmd[1 + i], &x[i], &y[i]);
        }
    }

    if (textured) set_tex_state(s_cmd[0] & 0xFFFFFFu, semi, raw);
    else sw_set_semi_transparency(semi, (int)((s_texpage >> 5) & 3));

    if (gouraud && textured) {
        if (!quad) sw_draw_shaded_textured_triangle(x[0],y[0],u[0],v[0],col[0], x[1],y[1],u[1],v[1],col[1],
                                                    x[2],y[2],u[2],v[2],col[2], clut_x(clutw),clut_y(clutw),tp,raw);
        else {
            sw_draw_shaded_textured_triangle(x[0],y[0],u[0],v[0],col[0], x[1],y[1],u[1],v[1],col[1],
                                             x[2],y[2],u[2],v[2],col[2], clut_x(clutw),clut_y(clutw),tp,raw);
            sw_draw_shaded_textured_triangle(x[2],y[2],u[2],v[2],col[2], x[1],y[1],u[1],v[1],col[1],
                                             x[3],y[3],u[3],v[3],col[3], clut_x(clutw),clut_y(clutw),tp,raw);
        }
    } else if (textured) {
        if (!quad) sw_draw_textured_triangle(x[0],y[0],u[0],v[0], x[1],y[1],u[1],v[1], x[2],y[2],u[2],v[2],
                                             clut_x(clutw),clut_y(clutw),tp);
        else {
            sw_draw_textured_triangle(x[0],y[0],u[0],v[0], x[1],y[1],u[1],v[1], x[2],y[2],u[2],v[2],
                                      clut_x(clutw),clut_y(clutw),tp);
            sw_draw_textured_triangle(x[2],y[2],u[2],v[2], x[1],y[1],u[1],v[1], x[3],y[3],u[3],v[3],
                                      clut_x(clutw),clut_y(clutw),tp);
        }
    } else if (gouraud) {
        if (!quad) sw_draw_gouraud_triangle(x[0],y[0],(uint16_t)col[0], x[1],y[1],(uint16_t)col[1], x[2],y[2],(uint16_t)col[2]);
        else {
            sw_draw_gouraud_triangle(x[0],y[0],(uint16_t)col[0], x[1],y[1],(uint16_t)col[1], x[2],y[2],(uint16_t)col[2]);
            sw_draw_gouraud_triangle(x[2],y[2],(uint16_t)col[2], x[1],y[1],(uint16_t)col[1], x[3],y[3],(uint16_t)col[3]);
        }
    } else {
        uint16_t color = (uint16_t)col[0];
        if (!quad) sw_draw_flat_triangle(x[0],y[0],x[1],y[1],x[2],y[2],color);
        else {
            sw_draw_flat_triangle(x[0],y[0],x[1],y[1],x[2],y[2],color);
            sw_draw_flat_triangle(x[2],y[2],x[1],y[1],x[3],y[3],color);
        }
    }
}

static void exec_line(uint8_t op) {
    int semi = (op >> 1) & 1;
    int x0,y0,x1,y1;
    sw_set_semi_transparency(semi, (int)((s_texpage >> 5) & 3));
    if ((op & 0x10) == 0) {                       /* 0x40-0x47: mono */
        tvtx(s_cmd[1], &x0, &y0); tvtx(s_cmd[2], &x1, &y1);
        sw_draw_line(x0, y0, x1, y1, c555(s_cmd[0]));
    } else {                                      /* 0x50-0x57: Gouraud */
        tvtx(s_cmd[1], &x0, &y0); tvtx(s_cmd[3], &x1, &y1);
        sw_draw_shaded_line(x0, y0, c555(s_cmd[0]), x1, y1, c555(s_cmd[2]));
    }
}

static void exec_polyline_word(uint32_t w) {
    if ((w & 0xF000F000u) == 0x50005000u) { s_polyline = 0; return; }
    int x, y;
    if (s_polyline_shaded && s_polyline_phase == 0) {     /* color word */
        s_pl_c = c555(w);
        s_polyline_phase = 1;
        return;
    }
    tvtx(w, &x, &y);
    if (!s_polyline_started) {
        s_pl_x = x; s_pl_y = y; s_polyline_started = 1;
    } else {
        sw_draw_line(s_pl_x, s_pl_y, x, y, s_pl_c);
        s_pl_x = x; s_pl_y = y;
    }
    if (s_polyline_shaded) s_polyline_phase = 0;
}

/* ---- rectangles ------------------------------------------------------------------------------------------ */
static void exec_rect(uint8_t op) {
    int semi = (op >> 1) & 1, raw = op & 1;
    int x, y;
    tvtx(s_cmd[1], &x, &y);
    sw_set_semi_transparency(semi, (int)((s_texpage >> 5) & 3));
    if (op < 0x64) {                                    /* 0x60-0x63: variable, flat */
        int w = s_cmd[2] & 0xFFFF, h = (s_cmd[2] >> 16) & 0xFFFF;
        sw_set_color_modulation(128, 128, 128, 0);
        sw_draw_flat_rect(x, y, w, h, c555(s_cmd[0]));
    } else if (op < 0x68) {                             /* 0x64-0x67: variable, textured */
        int w = s_cmd[3] & 0xFFFF, h = (s_cmd[3] >> 16) & 0xFFFF;
        uint32_t uv = s_cmd[2];
        set_tex_state(s_cmd[0] & 0xFFFFFFu, semi, raw);
        sw_draw_textured_rect(x, y, w, h, uv & 0xFF, (uv >> 8) & 0xFF,
                              clut_x(uv), clut_y(uv), (uint16_t)(s_texpage & 0x1FF));
    } else if (op < 0x6C) {                             /* 0x68-0x6B: 1x1 dot */
        sw_draw_flat_rect(x, y, 1, 1, c555(s_cmd[0]));
    } else if (op < 0x70) {                             /* 0x6C-0x6F: 1x1 textured */
        uint32_t uv = s_cmd[2];
        set_tex_state(s_cmd[0] & 0xFFFFFFu, semi, raw);
        sw_draw_textured_rect(x, y, 1, 1, uv & 0xFF, (uv >> 8) & 0xFF,
                              clut_x(uv), clut_y(uv), (uint16_t)(s_texpage & 0x1FF));
    } else if (op < 0x74) {                             /* 0x70-0x73: 8x8 flat */
        sw_draw_flat_rect(x, y, 8, 8, c555(s_cmd[0]));
    } else if (op < 0x78) {                             /* 0x74-0x77: 8x8 textured */
        uint32_t uv = s_cmd[2];
        set_tex_state(s_cmd[0] & 0xFFFFFFu, semi, raw);
        sw_draw_textured_rect(x, y, 8, 8, uv & 0xFF, (uv >> 8) & 0xFF,
                              clut_x(uv), clut_y(uv), (uint16_t)(s_texpage & 0x1FF));
    } else if (op < 0x7C) {                             /* 0x78-0x7B: 16x16 flat */
        sw_draw_flat_rect(x, y, 16, 16, c555(s_cmd[0]));
    } else {                                            /* 0x7C-0x7F: 16x16 textured */
        uint32_t uv = s_cmd[2];
        set_tex_state(s_cmd[0] & 0xFFFFFFu, semi, raw);
        sw_draw_textured_rect(x, y, 16, 16, uv & 0xFF, (uv >> 8) & 0xFF,
                              clut_x(uv), clut_y(uv), (uint16_t)(s_texpage & 0x1FF));
    }
}

/* ---- command execution ----------------------------------------------------------------------------------- */
static void note_first_draw(uint32_t cmd) {
    static int once;
    extern int fntrace_is_game_started(void);
    if (once || !fntrace_is_game_started()) return;
    once = 1;
    lion_logf("psxlion: first GPU draw command / GP0", cmd, 0);
}

/* Bring-up: -DPSX_GPU_TRACE_FRAME=N records every GP0 command (and GP1 write) of VI frames N and N+1 into a 1 MB buffer at
 * 0x1400000 (u32 word count, then records { n, words... }; GP1 writes as { 0xF1, value }); gputrace.py decodes a --dump-ram.
 * The VRAM at the first record is copied to 0x1500000 (gpuref.py renders the trace with it). */
#ifdef PSX_GPU_TRACE_FRAME
#define GT_BUF ((volatile uint32_t *)0x01400000u)
static void gt_rec(const uint32_t *w, uint32_t n) {
    uint32_t f = VI_FRAME;
    if (f < PSX_GPU_TRACE_FRAME) return;                    /* from frame N until the buffer is full */
    uint32_t at = GT_BUF[0];
    if (at == 0)                                            /* the VRAM as the trace starts, at 0x1500000 (native u16) */
        for (uint32_t i = 0; i < VRAM_W * VRAM_H; i++) ((volatile uint16_t *)0x01500000u)[i] = g_vram[i];
    if (at + n + 2 >= 0x40000u) return;
    GT_BUF[1 + at] = n;
    for (uint32_t i = 0; i < n; i++) GT_BUF[2 + at + i] = w[i];
    GT_BUF[0] = at + n + 1;
}
#else
#define gt_rec(w, n) do { } while (0)
#endif

static int cmd_words(uint8_t op);
static void gp0_execute(void) {
    uint8_t op = (uint8_t)(s_cmd[0] >> 24);
    { int n = cmd_words(op); gt_rec(s_cmd, (uint32_t)(n > 0 ? n : 1)); (void)n; }
    if (op >= 0x20 && op <= 0x7F) note_first_draw(s_cmd[0]);
    switch (op) {
    case 0x00: case 0x01: case 0x1F: break;
    case 0x02: {
        uint32_t c = s_cmd[0] & 0xFFFFFFu;
        int x = s_cmd[1] & 0x3F0, y = (s_cmd[1] >> 16) & 0x1FF;
        int w = ((s_cmd[2] & 0x3FF) + 15) & ~15, h = (s_cmd[2] >> 16) & 0x1FF;
        sw_fill_rect(x, y, w, h, c555(c));
        break;
    }
    case 0xE1:
        s_texpage = s_cmd[0] & 0x7FFu;
        break;
    case 0xE2:
        s_tw = s_cmd[0] & 0xFFFFFu;
        sw_set_texture_window(s_tw);
        break;
    case 0xE3:
        s_draw_x1 = s_cmd[0] & 0x3FF; s_draw_y1 = (s_cmd[0] >> 10) & 0x1FF; set_draw_area(); break;
    case 0xE4:
        s_draw_x2 = s_cmd[0] & 0x3FF; s_draw_y2 = (s_cmd[0] >> 10) & 0x1FF; set_draw_area(); break;
    case 0xE5:
        s_off_x = (int)(s_cmd[0] << 21) >> 21; s_off_y = (int)((s_cmd[0] >> 11) << 21) >> 21;
        sw_set_draw_offset(s_off_x, s_off_y);
        break;
    case 0xE6:
        s_mask_set = s_cmd[0] & 1; s_mask_check = (s_cmd[0] >> 1) & 1;
        sw_set_mask_bits(s_mask_set, s_mask_check);
        break;

    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
        int sx = s_cmd[1] & 0x3FF, sy = (s_cmd[1] >> 16) & 0x1FF;
        int dx = s_cmd[2] & 0x3FF, dy = (s_cmd[2] >> 16) & 0x1FF;
        int w = s_cmd[3] & 0x3FF, h = (s_cmd[3] >> 16) & 0x1FF;
        if (!w) w = 0x400; if (!h) h = 0x200;
        sw_copy_rect(sx, sy, dx, dy, w, h);
        break;
    }
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        s_img_x = s_cmd[1] & 0x3FF; s_img_y = (s_cmd[1] >> 16) & 0x1FF;
        s_img_w = s_cmd[2] & 0x3FF; if (!s_img_w) s_img_w = 0x400;
        s_img_h = (s_cmd[2] >> 16) & 0x1FF; if (!s_img_h) s_img_h = 0x200;
        s_img_row = s_img_col = 0;
        s_img_left = (s_img_w * s_img_h + 1) / 2;
        break;
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: case 0xC6: case 0xC7:
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD7:
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        s_rd_x = s_cmd[1] & 0x3FF; s_rd_y = (s_cmd[1] >> 16) & 0x1FF;
        s_rd_w = s_cmd[2] & 0x3FF; if (!s_rd_w) s_rd_w = 0x400;
        s_rd_h = (s_cmd[2] >> 16) & 0x1FF; if (!s_rd_h) s_rd_h = 0x200;
        s_rd_row = s_rd_col = 0;
        s_rd_left = (s_rd_w * s_rd_h + 1) / 2;
        break;
    default:
        if (op >= 0x20 && op <= 0x3F) exec_poly(op);
        else if (op >= 0x40 && op <= 0x5F) exec_line(op);
        else if (op >= 0x60 && op <= 0x7F) exec_rect(op);
        else if (!s_bad_cmd_logged) { s_bad_cmd_logged = 1; lion_logf("gpu: unhandled GP0 / last fn", s_cmd[0], 0); }
        break;
    }
}

static int cmd_words(uint8_t op) {
    if (op >= 0x20 && op <= 0x3F) {
        int quad = (op & 8) != 0, gouraud = (op & 0x10) != 0, tex = (op & 4) != 0;
        if (tex) return gouraud ? (quad ? 12 : 9) : (quad ? 9 : 7);
        return gouraud ? (quad ? 8 : 6) : (quad ? 5 : 4);
    }
    if (op >= 0x40 && op <= 0x47) return 3;
    if (op >= 0x48 && op <= 0x4F) return -1;
    if (op >= 0x50 && op <= 0x57) return 4;
    if (op >= 0x58 && op <= 0x5F) return -1;
    if (op >= 0x60 && op <= 0x63) return 3;
    if (op >= 0x64 && op <= 0x67) return 4;
    if (op >= 0x68 && op <= 0x6B) return 2;
    if (op >= 0x6C && op <= 0x6F) return 3;
    if (op >= 0x70 && op <= 0x73) return 2;
    if (op >= 0x74 && op <= 0x77) return 3;
    if (op >= 0x78 && op <= 0x7B) return 2;
    if (op >= 0x7C && op <= 0x7F) return 3;
    if (op >= 0x80 && op <= 0x9F) return 4;
    if (op >= 0xA0 && op <= 0xBF) return 3;
    if (op >= 0xC0 && op <= 0xDF) return 3;
    if (op == 0x02) return 3;
    return 1;
}

void gpu_gp0_word(uint32_t w) {
    if (s_img_left) {                             /* pixel data of a CPU->VRAM transfer */
        uint16_t p0 = (uint16_t)w, p1 = (uint16_t)(w >> 16);
        s_img_line[s_img_col++] = p0;
        if (s_img_col == s_img_w) { sw_vram_transfer_in(s_img_x, s_img_y + s_img_row, s_img_w, 1, s_img_line); s_img_col = 0; s_img_row++; }
        if (s_img_left > 1 || !((s_img_w * s_img_h) & 1)) {     /* the last word's high half is padding only for an odd count */
            s_img_line[s_img_col++] = p1;
            if (s_img_col == s_img_w) { sw_vram_transfer_in(s_img_x, s_img_y + s_img_row, s_img_w, 1, s_img_line); s_img_col = 0; s_img_row++; }
        }
        s_img_left--;
        if (!s_img_left && s_img_col) { sw_vram_transfer_in(s_img_x, s_img_y + s_img_row, s_img_col, 1, s_img_line); s_img_col = 0; }
        return;
    }
    if (s_polyline) { exec_polyline_word(w); return; }
    if (s_need) {
        s_cmd[s_have++] = w;
        if (s_have >= s_need) { s_need = 0; s_have = 0; gp0_execute(); }
        return;
    }
    uint8_t op = (uint8_t)(w >> 24);
    int n = cmd_words(op);
    s_cmd[0] = w; s_have = 1;
    if (n == 1) { s_have = 0; gp0_execute(); return; }
    if (n < 0) {                                  /* polyline: seed and consume until terminator */
        s_polyline = 1; s_polyline_shaded = (op & 0x10) != 0; s_polyline_semi = (op >> 1) & 1;
        s_polyline_started = 0;
        s_polyline_phase = 1;                     /* next word is vertex0 (shaded: header already gave colour0) */
        s_pl_c = c555(w); s_have = 0;
        sw_set_semi_transparency(s_polyline_semi, (int)((s_texpage >> 5) & 3));
        return;
    }
    s_need = n;
}

uint32_t gpu_read_gp0(void) {
    if (s_rd_left) {
        uint16_t p0 = sw_vram_read(s_rd_x + s_rd_col, s_rd_y + s_rd_row);
        s_rd_col++;
        if (s_rd_col >= s_rd_w) { s_rd_col = 0; s_rd_row++; }
        uint32_t v = p0;
        if (s_rd_left > 1 || !((s_rd_w * s_rd_h) & 1)) {
            uint16_t p1 = sw_vram_read(s_rd_x + s_rd_col, s_rd_y + s_rd_row);
            s_rd_col++;
            if (s_rd_col >= s_rd_w) { s_rd_col = 0; s_rd_row++; }
            v |= (uint32_t)p1 << 16;
        }
        s_rd_left--;
        return v;
    }
    return s_gpuread;
}

void gpu_write_gp1(uint32_t v) {
    uint32_t cmd = (v >> 24) & 0x3F;
    { uint32_t r[2] = { 0xF1000000u, v }; gt_rec(r, 2); (void)r; }
    switch (cmd) {
    case 0x00:
        s_texpage = 0; s_tw = 0; s_off_x = s_off_y = 0;
        s_draw_x1 = s_draw_y1 = 0; s_draw_x2 = 1023; s_draw_y2 = 511;
        s_mask_set = s_mask_check = 0; s_disp_disabled = 1;
        s_hres = 1; s_vres = 0; s_depth = 0; s_interlace = 0;
        s_img_left = 0; s_rd_left = 0; s_need = 0; s_polyline = 0;
        sw_set_draw_area(0, 0, 1023, 511); sw_set_draw_offset(0, 0);
        sw_set_texture_window(0); sw_set_mask_bits(0, 0);
        break;
    case 0x01: s_need = 0; s_have = 0; s_img_left = 0; s_rd_left = 0; s_polyline = 0; break;
    case 0x02: break;
    case 0x03: s_disp_disabled = (int)(v & 1); break;
    case 0x04: s_dma_dir = (int)(v & 3); break;
    case 0x05: {
        uint32_t x = v & 0x3FF, y = (v >> 10) & 0x1FF;
        if (x != s_disp_x || y != s_disp_y) s_flips++;                  /* the game shows a new frame */
        s_disp_x = x; s_disp_y = y; break;
    }
    case 0x06:
        s_hx1 = v & 0xFFF; s_hx2 = (v >> 12) & 0xFFF;
        update_xmap();
        break;
    case 0x07: break;
    case 0x08:
        s_hres = v & 3; s_vres = (v >> 2) & 1; s_depth = (v >> 4) & 1; s_interlace = (v >> 5) & 1;
        update_xmap();
        break;
    default:
        if (cmd >= 0x10 && cmd <= 0x1F) {
            uint32_t which = v & 0x0F;
            switch (which) {
            case 2: s_gpuread = s_tw; break;
            case 3: s_gpuread = s_draw_x1 | (s_draw_y1 << 10); break;
            case 4: s_gpuread = s_draw_x2 | (s_draw_y2 << 10); break;
            case 5: s_gpuread = (s_off_x & 0x7FF) | ((s_off_y & 0x7FF) << 11); break;
            case 7: s_gpuread = 2; break;
            default: s_gpuread = 0; break;
            }
        }
        break;
    }
}

uint32_t gpu_gpustat(void) {
    uint32_t f = VI_FRAME & 1u;
    uint32_t v = (1u << 26) | (1u << 28);           /* ready for GP0 / DMA block */
    v |= s_texpage & 0x7FFu;
    if (s_rd_left) v |= 1u << 27;                   /* VRAM->CPU read in progress */
    v |= (uint32_t)(s_mask_set & 1) << 11;
    v |= (uint32_t)(s_mask_check & 1) << 12;
    v |= f << 13;
    v |= (uint32_t)s_hres << 17;
    v |= (uint32_t)s_vres << 19;
    v |= (uint32_t)s_depth << 21;
    v |= (uint32_t)s_interlace << 22;
    v |= (uint32_t)(s_disp_disabled & 1) << 23;
    if (s_dma_dir) v |= 1u << 25;                   /* DMA request */
    v |= ((uint32_t)s_dma_dir & 3u) << 29;
    v |= (f ^ 1u) << 31;
    return v;
}

/* ---- presentation ---------------------------------------------------------------------------------------- */
static inline uint32_t rgb24_px(const uint16_t *row, uint32_t bx) {
    if (bx + 2u >= 2048u) return 0;
    uint16_t w0 = row[bx >> 1], w1 = row[(bx + 1u) >> 1], w2 = row[(bx + 2u) >> 1];
    uint32_t r = (bx & 1u) ? (w0 >> 8) : (w0 & 0xFFu);
    uint32_t g = ((bx + 1u) & 1u) ? (w1 >> 8) : (w1 & 0xFFu);
    uint32_t b = ((bx + 2u) & 1u) ? (w2 >> 8) : (w2 & 0xFFu);
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}
extern uint32_t mdec_get_decode_count(void);
void gpu_frame_tick(void) {
    static uint32_t last, last_mdec;
    static int mdec_logged, present_logged;
    uint32_t f = VI_FRAME;
    if (f == last) return;
    last = f;
    static uint32_t mark;
    if ((f >> 8) != mark) {                         /* guest frame rate: flips per 256 VI frames (60 Hz) */
        static uint32_t last_flips;
        mark = f >> 8;
        lion_logf("gpu: flips per 256 frames / VI frame", s_flips - last_flips, f);
        last_flips = s_flips;
        { extern void sio_stats(void); sio_stats(); }
        { extern void sk_stats(void); sk_stats(); }
        { extern uint32_t g_polls; static uint32_t last_polls; lion_logf("irq: polls per 256 frames / VI frame", g_polls - last_polls, f); last_polls = g_polls; }
    }
    uint32_t md = mdec_get_decode_count();
    if (md != last_mdec) {
        if (!mdec_logged) { mdec_logged = 1; lion_logf("mdec: first decode / count", md, 0); }
        last_mdec = md;
    }
    if (s_disp_disabled) return;
    if (!present_logged) {
        present_logged = 1;
        lion_logf("gpu: first present; disp xy / mode", s_disp_x | (s_disp_y << 16), s_hres | (s_depth << 8));
    }
    uint32_t w = (s_hres == 0) ? 256 : (s_hres == 1) ? 320 : (s_hres == 2) ? 512 : 640;
    (void)w;
#ifndef PSX_GPU_SW
    if (sk_present(s_disp_x, s_disp_y, s_xmap, (int)s_vres, (int)s_depth, FB_ADDR, FB_STRIDE)) return;
    VI_FB = FB_ADDR; VI_STRIDE = FB_STRIDE;           /* 24-bit: converted from g_vram below */
#endif
    uint32_t *fb = (uint32_t *)FB_ADDR;
    if (s_depth) {                                  /* 24-bit FMV: packed RGB888 bytes across the VRAM row */
        for (int oy = 0; oy < 240; oy++) {
            int sy = (int)(s_disp_y + (s_vres ? oy * 2 : oy)) & (VRAM_H - 1);
            const uint16_t *row = &g_vram[sy * VRAM_W];
            uint32_t base = (s_disp_x & 1023u) * 2u;
            uint32_t *dst = (uint32_t *)((uint8_t *)fb + oy * FB_STRIDE);
            for (int ox = 0; ox < 320; ox += 2) {
                uint32_t p0 = rgb24_px(row, base + (uint32_t)s_xmap[ox] * 3u);
                uint32_t p1 = rgb24_px(row, base + (uint32_t)s_xmap[ox + 1] * 3u);
                dst[ox >> 1] = (p0 << 16) | p1;
            }
        }
        return;
    }
    for (int oy = 0; oy < 240; oy++) {
        int sy = (int)(s_disp_y + (s_vres ? oy * 2 : oy)) & (VRAM_H - 1);
        const uint16_t *src = &g_vram[sy * VRAM_W];
        uint32_t *dst = (uint32_t *)((uint8_t *)fb + oy * FB_STRIDE);
        for (int ox = 0; ox < 320; ox += 2) {
            uint16_t p0 = src[(s_disp_x + s_xmap[ox]) & (VRAM_W - 1)];
            uint16_t p1 = src[(s_disp_x + s_xmap[ox + 1]) & (VRAM_W - 1)];
            dst[ox >> 1] = ((uint32_t)s_rgb565[p0] << 16) | s_rgb565[p1];
        }
    }
}
