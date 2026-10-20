/* psxlion/rt/gpu_spike.h — routes gpu.c's rasterizer calls to the Spike backend (gpu_spike.c). Build with
 * -DPSX_GPU_SW to keep the shared software rasterizer (gpu_sw_renderer.c) on the Tuffy instead. */
#pragma once
#ifndef PSX_GPU_SW
#include <stdint.h>
#define sw_renderer_init                 sk_renderer_init
#define sw_set_draw_area                 sk_set_draw_area
#define sw_set_draw_offset               sk_set_draw_offset
#define sw_set_texture_window            sk_set_texture_window
#define sw_set_semi_transparency         sk_set_semi_transparency
#define sw_set_mask_bits                 sk_set_mask_bits
#define sw_set_color_modulation          sk_set_color_modulation
#define sw_fill_rect                     sk_fill_rect
#define sw_copy_rect                     sk_copy_rect
#define sw_vram_transfer_in              sk_vram_transfer_in
#define sw_vram_read                     sk_vram_read
#define sw_draw_flat_triangle            sk_draw_flat_triangle
#define sw_draw_gouraud_triangle         sk_draw_gouraud_triangle
#define sw_draw_textured_triangle        sk_draw_textured_triangle
#define sw_draw_shaded_textured_triangle sk_draw_shaded_textured_triangle
#define sw_draw_flat_rect                sk_draw_flat_rect
#define sw_draw_textured_rect            sk_draw_textured_rect
#define sw_draw_line                     sk_draw_line
#define sw_draw_shaded_line              sk_draw_shaded_line
void sk_renderer_init(uint16_t *vram);
void sk_set_draw_area(int x1, int y1, int x2, int y2);
void sk_set_draw_offset(int x, int y);
void sk_set_texture_window(uint32_t raw);
void sk_set_semi_transparency(int enabled, int mode);
void sk_set_mask_bits(int set_bit, int check_bit);
void sk_set_color_modulation(int r, int g, int b, int raw);
void sk_fill_rect(int x, int y, int w, int h, uint16_t color);
void sk_copy_rect(int sx, int sy, int dx, int dy, int w, int h);
void sk_vram_transfer_in(int x, int y, int w, int h, const uint16_t *data);
uint16_t sk_vram_read(int x, int y);
void sk_draw_flat_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);
void sk_draw_gouraud_triangle(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1, int x2, int y2, uint16_t c2);
void sk_draw_textured_triangle(int x0, int y0, int u0, int v0, int x1, int y1, int u1, int v1, int x2, int y2, int u2, int v2,
                               uint16_t clut_x, uint16_t clut_y, uint16_t texpage);
void sk_draw_shaded_textured_triangle(int x0, int y0, int u0, int v0, uint32_t color0, int x1, int y1, int u1, int v1, uint32_t color1,
                                      int x2, int y2, int u2, int v2, uint32_t color2, uint16_t clut_x, uint16_t clut_y,
                                      uint16_t texpage, int raw);
void sk_draw_flat_rect(int x, int y, int w, int h, uint16_t color);
void sk_draw_textured_rect(int x, int y, int w, int h, int u, int v, uint16_t clut_x, uint16_t clut_y, uint16_t texpage);
void sk_draw_line(int x0, int y0, int x1, int y1, uint16_t color);
void sk_draw_shaded_line(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1);
int sk_present(uint32_t disp_x, uint32_t disp_y, const uint16_t *xmap, int vres, int depth24, uint32_t fb, uint32_t fb_stride);
#endif
