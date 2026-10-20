/* psxlion/rt/gpu_spike.c — PS1 GPU rasterization on the Spike.
 *
 * Same entry points as the shared software rasterizer (gpu_sw_renderer.c, sw_*), which gpu.c calls; gpu_spike.h maps
 * them here unless the build asks for PSX_GPU_SW. The Tuffy only decodes GP0 and feeds the Spike; the pixels are the
 * Spike's.
 *
 * Two copies of the PS1 VRAM:
 *   - g_vram (gpu.c), BGR555 in SDRAM: what CPU uploads, fills and copies write, and where texels and CLUTs are read.
 *   - SG_VRAM, RGB565 in SGRAM (1024x512, stride 2048): the Spike's render target and the VI's scanout.
 * Uploads write both. Draws touch only SGRAM and mark the 64x256 pages they cover "newer in SGRAM"; anything that reads
 * VRAM back (GP0 C0, VRAM copies, textures sourced from a drawn page) first converts those pages back into g_vram.
 *
 * Textures: the Spike samples RGB565, power-of-two textures. A PS1 texture (page + depth + CLUT + texture window) is
 * converted once into a 256x256 RGB565 slot of the SGRAM cache and reused while the VRAM it came from is unchanged
 * (per-page versions). Pure black (0x0000) stays 0 and is dropped by the Spike's color key, as on the PS1.
 *
 * Approximations (the Spike has no equivalent): semi-transparency mode 2 (B-F) darkens by alpha instead of
 * subtracting; no dithering, no mask bit. Textured semi-transparent primitives are drawn in two passes so that only
 * texels with the STP bit blend, which is exact.
 */
#include <stdint.h>
#include <string.h>

extern uint16_t g_vram[];                   /* gpu.c: 1024x512 BGR555 */
extern void lion_logf(const char *, uint32_t, uint32_t);

#define SPIKE_CMD   (*(volatile uint32_t *)0x1F010000u)
#define SPIKE_SYNC  (*(volatile uint32_t *)0x1F010008u)
enum { OP_SET_TARGET = 1, OP_CLEAR = 2, OP_SET_MODE = 3, OP_SET_TEXTURE = 4, OP_TRI = 5, OP_SYNC = 6, OP_QUAD = 7 };
enum { M_GOURAUD = 4, M_TEXTURE = 8, M_BLEND = 64, M_BLENDADD = 1024, M_COLORKEY = 2048 };

#define VW 1024
#define VH 512
#define SG_VRAM   0x10000000u               /* RGB565 copy of the VRAM, stride 2048 */
#define SG_STRIDE 2048u
#define SG_TEX    0x10200000u               /* texture cache: 6 MB up to the end of SGRAM, in slots of three sizes (tex_init) */
#define SG_TEX_LO 0x10150000u               /* after gpu.c's display buffer (0x10100000, up to 480 lines of stride 640): 22 more 128x128 slots */
#define TEX_LO_SLOTS 22
#define TEX_SLOTS (16 + 64 + TEX_LO_SLOTS + 256)  /* 256x256, 128x128 and 64x64 texels: 2 MB each class, + the 128s below SG_TEX */

static inline uint16_t *sg_px(int x, int y) { return (uint16_t *)(uintptr_t)(SG_VRAM + (uint32_t)y * SG_STRIDE + (uint32_t)x * 2u); }
static inline uint16_t to565(uint16_t c) {   /* BGR555 -> RGB565 (STP dropped) */
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (uint16_t)((r << 11) | (((g << 1) | (g >> 4)) << 5) | b);
}
static inline uint16_t to555(uint16_t c) {   /* RGB565 -> BGR555 */
    return (uint16_t)((c >> 11) | (((c >> 6) & 31) << 5) | ((c & 31) << 10));
}
static inline uint32_t c555_888(uint32_t c) {  /* BGR555 -> 0x00RRGGBB */
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}
static inline uint32_t fbits(float f) { union { float f; uint32_t u; } x; x.f = f; return x.u; }

/* ---- Spike command stream --------------------------------------------------------------------------------- */
static uint32_t s_token;
static int s_busy;                          /* commands pushed since the Spike was last seen idle */
static inline void put(uint32_t v) { SPIKE_CMD = v; }
static void wait_idle(void) {
    if (!s_busy) return;
    put(OP_SYNC << 24 | 1); put(++s_token);
    while (SPIKE_SYNC != s_token) { }
    s_busy = 0;
}

/* ---- state set by gpu.c ------------------------------------------------------------------------------------ */
static int s_cx1, s_cy1, s_cx2 = VW - 1, s_cy2 = VH - 1;   /* drawing area, inclusive */
static int s_target_dirty = 1, s_area_empty;
static int s_semi, s_semi_mode;
static int s_mr = 128, s_mg = 128, s_mb = 128, s_raw;
static uint32_t s_tw;                       /* texture window, GP0(E2) raw */
static uint32_t s_mode = ~0u;
static uint32_t s_bound = ~0u;                  /* texture slot the Spike has bound */

static void tex_init(void);
void sk_renderer_init(uint16_t *vram) {
    (void)vram;
    s_busy = 1; wait_idle();                                            /* whatever the boot ROM left in the FIFO */
    for (uint32_t *p = (uint32_t *)(uintptr_t)SG_VRAM; p < (uint32_t *)(uintptr_t)(SG_VRAM + VH * SG_STRIDE); p++) *p = 0;
    s_target_dirty = 1; s_mode = ~0u; s_bound = ~0u;
    tex_init();
}
void sk_set_draw_area(int x1, int y1, int x2, int y2) {
    if (x1 > VW - 1) x1 = VW - 1; if (y1 > VH - 1) y1 = VH - 1; if (x2 > VW - 1) x2 = VW - 1; if (y2 > VH - 1) y2 = VH - 1;
    if (x1 != s_cx1 || y1 != s_cy1 || x2 != s_cx2 || y2 != s_cy2) { s_cx1 = x1; s_cy1 = y1; s_cx2 = x2; s_cy2 = y2; s_target_dirty = 1; }
}
void sk_set_draw_offset(int x, int y) { (void)x; (void)y; }     /* gpu.c adds the offset to every vertex */
void sk_set_texture_window(uint32_t raw) { s_tw = raw & 0xFFFFFu; }
void sk_set_semi_transparency(int enabled, int mode) { s_semi = enabled; s_semi_mode = mode & 3; }
void sk_set_mask_bits(int set_bit, int check_bit) { (void)set_bit; (void)check_bit; }
void sk_set_color_modulation(int r, int g, int b, int raw) { s_mr = r; s_mg = g; s_mb = b; s_raw = raw; }

static void bind_target(void) {
    if (!s_target_dirty) return;
    s_target_dirty = 0;
    s_area_empty = s_cx2 < s_cx1 || s_cy2 < s_cy1;
    if (s_area_empty) return;
    put(OP_SET_TARGET << 24 | 5);
    put(SG_VRAM + (uint32_t)s_cy1 * SG_STRIDE + (uint32_t)s_cx1 * 2u); put(SG_STRIDE);
    put((uint32_t)(s_cx2 - s_cx1 + 1)); put((uint32_t)(s_cy2 - s_cy1 + 1)); put(0);
    s_busy = 1;
}
static void set_mode(uint32_t m) {
    if (m == s_mode) return;
    s_mode = m; put(OP_SET_MODE << 24 | 1); put(m); s_busy = 1;
}

static uint32_t st_bind, st_new, st_tiles, st_sync, st_evict[3], st_newc[3], st_bindc[3];   /* bring-up: why textures get converted (sk_stats) */
static int st_samples;
void sk_stats(void) {
    lion_logf("tex: binds / tiles converted", st_bind, st_tiles);
    lion_logf("tex: misses / sync_back pages", st_new, st_sync);
    lion_logf("tex: new misses 256 / 128 blocks", st_newc[0], st_newc[1]);
    lion_logf("tex: new misses 64 / evictions 64", st_newc[2], st_evict[2]);
    lion_logf("tex: evictions 256 / 128", st_evict[0], st_evict[1]);
    lion_logf("tex: binds 256 / 128", st_bindc[0], st_bindc[1]);
    lion_logf("tex: binds 64", st_bindc[2], 0);
    st_bind = st_new = st_tiles = st_sync = 0;
    for (int i = 0; i < 3; i++) st_evict[i] = st_newc[i] = st_bindc[i] = 0;
}

/* ---- page bookkeeping: 16 x 2 pages of 64x256 ------------------------------------------------------------- */
static uint32_t s_sg_newer;                 /* bit p: SGRAM holds pixels g_vram does not have, in the bands s_band_newer[p] */
static uint16_t s_band_newer[32];
/* Versions of g_vram in bands of 64x16 (16 per page), bumped when the band's contents change: a converted texture depends only on
 * the bands its block reads, so a palette rewritten every frame in the corner of a texture page (MK4's CLUTs share page 15
 * with its most used textures) does not throw away every texture of the page. */
static uint32_t s_bver[32 * 16];
static inline int page_of(int x, int y) { return ((x & (VW - 1)) >> 6) | (((y & (VH - 1)) >> 8) << 4); }
static void mark_bands(int x0, int y0, int x1, int y1) {             /* inclusive, already clipped to the VRAM */
    for (int py = y0 >> 8; py <= y1 >> 8; py++) {
        int b0 = py == y0 >> 8 ? (y0 >> 4) & 15 : 0, b1 = py == y1 >> 8 ? (y1 >> 4) & 15 : 15;
        uint16_t m = (uint16_t)(((2u << b1) - 1u) & ~((1u << b0) - 1u));
        for (int px = x0 >> 6; px <= x1 >> 6; px++) { int p = px | py << 4; s_band_newer[p] |= m; s_sg_newer |= 1u << p; }
    }
}
static void bump_rect(int x0, int y0, int w, int h) {                  /* every band [x0, x0+w) x [y0, y0+h) touches (wrapping) */
    for (int r = 0; r < h; r += 16 - ((y0 + r) & 15)) {
        int y = (y0 + r) & (VH - 1);
        for (int c = 0; c < w; c += 64 - ((x0 + c) & 63)) s_bver[page_of(x0 + c, y) * 16 + ((y >> 4) & 15)]++;
    }
}

/* Brings g_vram up to date with what the Spike drew in the given pages. Only pixels whose SGRAM value differs from g_vram's are
 * taken: RGB565 has no bit 15, and the parts of a page no draw reached often hold 4/8-bit texture or CLUT words, where bit 15 is
 * index data -- copying the whole page back would clear it (every 4th texel of a 4-bit texture). */
static void sync_bands(uint32_t pages, uint32_t bands) {             /* bands: mask of the 16-row bands wanted in each page */
    pages &= s_sg_newer;
    if (!pages) return;
    int waited = 0;
    for (int p = 0; p < 32; p++) {
        if (!(pages >> p & 1u)) continue;
        uint32_t bm = s_band_newer[p] & bands;
        if (!bm) continue;
        if (!waited) { wait_idle(); waited = 1; }
        int x0 = (p & 15) << 6, y0 = (p >> 4) << 8;
        for (int b = 0; b < 16; b++) {
            if (!(bm >> b & 1u)) continue;
            st_sync++;
            int changed = 0;
            for (int y = y0 + b * 16; y < y0 + b * 16 + 16; y++) {
                const uint16_t *s = sg_px(x0, y);
                uint16_t *d = &g_vram[y * VW + x0];
                for (int x = 0; x < 64; x++) if (to565(d[x]) != s[x]) { d[x] = to555(s[x]); changed = 1; }
            }
            if (changed) s_bver[p * 16 + b]++;
        }
        s_band_newer[p] &= (uint16_t)~bm;
        if (!s_band_newer[p]) s_sg_newer &= ~(1u << p);
    }
}
static void sync_back(uint32_t pages) { sync_bands(pages, 0xFFFFu); }

/* A draw touched [x0..x1] x [y0..y1] (screen coordinates): clip to the drawing area and mark its pages. */
static void mark_drawn(int x0, int y0, int x1, int y1) {
    if (x0 < s_cx1) x0 = s_cx1; if (y0 < s_cy1) y0 = s_cy1; if (x1 > s_cx2) x1 = s_cx2; if (y1 > s_cy2) y1 = s_cy2;
    if (x1 < x0 || y1 < y0) return;
    mark_bands(x0, y0, x1, y1);
}

/* ---- texture cache ------------------------------------------------------------------------------------------ */
/* A slot holds one square block of a texture -- the whole 256x256 page, or a 128x128 / 64x64 block at a 64-texel grid
 * position (ox, oy) -- converted for one (texpage, CLUT, window, variant). A primitive takes the smallest block that holds its UV
 * box: most of a PS1 model samples a few dozen texels of a page under many CLUTs, and a frame of MK4 uses ~70 page+CLUT pairs
 * where whole-page slots only fit 48. done: 16x16 tiles of the block already converted. */
typedef struct { uint32_t key, tw, vsum, chk, hash, addr, fence; uint32_t done[8]; uint16_t size, ox, oy; int16_t next; uint8_t variant, used, ref, log2; } TexSlot;
static TexSlot s_tex[TEX_SLOTS];
#define TEX_HASH 1024                                   /* chains of slots by hash of their whole identity (tex_hash) */
static int16_t s_hhead[TEX_HASH];
static int s_hand[3];                                   /* per-class CLOCK hand: replacement is second-chance, not a scan */
static const int s_cls_first[3] = { 0, 16, 16 + 64 + TEX_LO_SLOTS }, s_cls_n[3] = { 16, 64 + TEX_LO_SLOTS, 256 }, s_cls_size[3] = { 256, 128, 64 };
static void tex_init(void) {
    uint32_t a = SG_TEX, a_hi = 0;
    for (int i = 0; i < TEX_HASH; i++) s_hhead[i] = -1;
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < s_cls_n[c]; i++) {
            TexSlot *t = &s_tex[s_cls_first[c] + i];
            if (c == 1 && i == 64) a = SG_TEX_LO;
            t->addr = a; t->size = (uint16_t)s_cls_size[c]; t->log2 = (uint8_t)(c == 0 ? 8 : c == 1 ? 7 : 6); t->used = 0; t->next = -1;
            a += (uint32_t)s_cls_size[c] * (uint32_t)s_cls_size[c] * 2u;
            if (c == 1 && i == 63) a_hi = a;
        }
        if (c == 1) a = a_hi;
    }
}
static float s_box_ox, s_box_oy, s_box_scale;           /* the bound block: u' = (u * 256 - ox) / size */
static int s_mru = -1;

/* VRAM pages a texture's texels come from (64/128/256 halfwords wide by depth). The CLUT is not part of it: CLUTs often sit
 * in a page the game also draws into every frame (beside or under a framebuffer), and a page version would then throw the
 * converted texture away each frame. The CLUT is checked by content instead (clut_sum). */
static uint32_t tex_pages(uint32_t tp) {
    int depth = (tp >> 7) & 3, tx = (tp & 15) << 6, ty = ((tp >> 4) & 1) << 8, wide = depth == 0 ? 64 : depth == 1 ? 128 : 256;
    uint32_t m = 0;
    for (int x = tx; x < tx + wide; x += 64) m |= 1u << page_of(x, ty);
    return m;
}
/* Brings the n CLUT entries at (cx, cy) up to date in g_vram (if their page was drawn) and returns a checksum of them. */
static uint32_t clut_sum(uint32_t cx, uint32_t cy, int n) {
    int y = (int)cy & (VH - 1);
    uint16_t *d = &g_vram[y * VW];
    uint32_t pages = 0;
    for (int i = 0; i < n; i += 16) pages |= 1u << page_of((int)cx + i, y);
    int band = (y >> 4) & 15, dirty = 0;
    for (int p = 0; p < 32; p++) if ((pages & s_sg_newer) >> p & 1u) dirty |= s_band_newer[p] >> band & 1;
    if (dirty) {
        wait_idle();
        for (int i = 0; i < n; i++) { int x = ((int)cx + i) & (VW - 1); uint16_t sg = *sg_px(x, y); if (to565(d[x]) != sg) d[x] = to555(sg); }
    }
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < n; i++) h = (h ^ d[((int)cx + i) & (VW - 1)]) * 0x01000193u;
    return h;
}
/* Version of the bands a block reads: rows [r0, r1] of the texture's pages (all 16 bands under a texture window). */
static uint32_t vsum_of(uint32_t pages, int r0, int r1) {
    uint32_t s = 0;
    for (int p = 0; p < 32; p++)
        if (pages >> p & 1u) for (int b = r0 >> 4; b <= r1 >> 4; b++) s += s_bver[p * 16 + b] * (2u * (uint32_t)(p * 16 + b) + 1u);
    return s;
}

/* variant 0: every texel; 1: opaque texels only (STP clear); 2: semi-transparent texels only (STP set). */
static inline uint16_t tex565(uint32_t c, int variant) {
    if (c == 0) return 0;                                               /* transparent */
    if (variant == 1 && (c & 0x8000u)) return 0;
    if (variant == 2 && !(c & 0x8000u)) return 0;
    uint16_t o = to565((uint16_t)c);
    return o ? o : 0x0020;                                              /* opaque black must survive the color key */
}
/* Without a texture window (the usual case): the CLUT converted once, then whole VRAM words -- 4 or 2 texels -- at a time.
 * u0 is a tile edge (multiple of 16), so 4/8-bit rows start on a word. */
static void convert_fast(uint16_t *dst, int size, int bx, int by, int depth, int tx, int ty, const uint16_t *clut, int variant,
                         int u0, int v0, int u1, int v1) {
    static uint16_t pal[256];
    if (depth < 2) for (int i = 0; i < (depth == 0 ? 16 : 256); i++) pal[i] = tex565(clut[i], variant);
    for (int v = v0; v <= v1; v++) {
        const uint16_t *row = &g_vram[((ty + v) & (VH - 1)) * VW];
        uint16_t *d = &dst[(v - by) * size + (u0 - bx)];
        if (depth == 0) {
            for (int w = u0 >> 2; w <= u1 >> 2; w++) {
                uint32_t x = row[(tx + w) & (VW - 1)];
                d[0] = pal[x & 15]; d[1] = pal[(x >> 4) & 15]; d[2] = pal[(x >> 8) & 15]; d[3] = pal[x >> 12]; d += 4;
            }
        } else if (depth == 1) {
            for (int w = u0 >> 1; w <= u1 >> 1; w++) {
                uint32_t x = row[(tx + w) & (VW - 1)];
                d[0] = pal[x & 255]; d[1] = pal[x >> 8]; d += 2;
            }
        } else {
            for (int u = u0; u <= u1; u++) *d++ = tex565(row[(tx + u) & (VW - 1)], variant);
        }
    }
}
static void convert(uint32_t addr, int size, int bx, int by, uint32_t tp, uint32_t cx, uint32_t cy, uint32_t tw, int variant, int u0, int v0, int u1, int v1) {
    int depth = (tp >> 7) & 3, tx = (tp & 15) << 6, ty = ((tp >> 4) & 1) << 8;
    if (!(tw & 0x3FF)) {                                                /* no window mask: texel (u, v) is VRAM word (u, v) */
        convert_fast((uint16_t *)(uintptr_t)addr, size, bx, by, depth, tx, ty, &g_vram[(cy & (VH - 1)) * VW + (cx & (VW - 1))], variant, u0, v0, u1, v1);
        return;
    }
    uint32_t mx = (tw & 31) * 8, my = ((tw >> 5) & 31) * 8, ox = ((tw >> 10) & 31) * 8, oy = ((tw >> 15) & 31) * 8;
    uint8_t umap[256], vmap[256];
    for (uint32_t i = 0; i < 256; i++) { umap[i] = (uint8_t)((i & ~mx) | (ox & mx)); vmap[i] = (uint8_t)((i & ~my) | (oy & my)); }
    const uint16_t *clut = &g_vram[(cy & (VH - 1)) * VW + (cx & (VW - 1))];
    uint16_t *dst = (uint16_t *)(uintptr_t)addr;
    for (int v = v0; v <= v1; v++) {
        const uint16_t *row = &g_vram[((ty + vmap[v]) & (VH - 1)) * VW];
        for (int u = u0; u <= u1; u++) {
            uint32_t uu = umap[u], c;
            if (depth == 0) c = clut[(row[(tx + (int)(uu >> 2)) & (VW - 1)] >> ((uu & 3) * 4)) & 15];
            else if (depth == 1) c = clut[(row[(tx + (int)(uu >> 1)) & (VW - 1)] >> ((uu & 1) * 8)) & 255];
            else c = row[(tx + (int)uu) & (VW - 1)];
            uint16_t o;
            if (c == 0) o = 0;                                          /* transparent */
            else if (variant == 1 && (c & 0x8000u)) o = 0;
            else if (variant == 2 && !(c & 0x8000u)) o = 0;
            else { o = to565((uint16_t)c); if (!o) o = 0x0020; }       /* opaque black must survive the color key */
            dst[(v - by) * size + (u - bx)] = o;
        }
    }
}

/* Converts the 16x16 tiles of slot `t` that [u0..u1] x [v0..v1] needs and has not got yet. Tiles nothing asked for stay
 * unconverted: a primitive samples only its UV box, and a PS1 texture page is 256x256 texels of which a draw uses a few. */
static void need_tiles(int slot, uint32_t tp, uint32_t cx, uint32_t cy, int u0, int v0, int u1, int v1) {
    TexSlot *t = &s_tex[slot];
    int per = t->size >> 4;                                              /* tiles per row of the block */
    int tx0 = (u0 - t->ox) >> 4, ty0 = (v0 - t->oy) >> 4, tx1 = (u1 - t->ox) >> 4, ty1 = (v1 - t->oy) >> 4, mx0 = 16, my0 = 16, mx1 = -1, my1 = -1;
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1; tx++) {
            int bit = ty * per + tx;
            if (t->done[bit >> 5] >> (bit & 31) & 1u) continue;
            t->done[bit >> 5] |= 1u << (bit & 31);
            if (tx < mx0) mx0 = tx; if (tx > mx1) mx1 = tx; if (ty < my0) my0 = ty; if (ty > my1) my1 = ty;
        }
    if (mx1 < 0) return;
    st_tiles += (uint32_t)((mx1 - mx0 + 1) * (my1 - my0 + 1));
    /* Rewriting an already converted tile inside the box writes the same texels, so queued draws may keep sampling it. */
    convert(t->addr, t->size, t->ox, t->oy, tp, cx, cy, t->tw, t->variant,
            t->ox + mx0 * 16, t->oy + my0 * 16, t->ox + mx1 * 16 + 15, t->oy + my1 * 16 + 15);
}

/* The smallest block of the 64-texel grid holding [u0..u1] x [v0..v1]: class 2 (64), 1 (128) or 0 (the page). */
static int block_for(int u0, int v0, int u1, int v1, int *ox, int *oy) {
    if ((u0 >> 6) == (u1 >> 6) && (v0 >> 6) == (v1 >> 6)) { *ox = u0 & ~63; *oy = v0 & ~63; return 2; }
    if ((u1 >> 6) - (u0 >> 6) <= 1 && (v1 >> 6) - (v0 >> 6) <= 1) {
        *ox = u0 & ~63; *oy = v0 & ~63;
        if (*ox > 128) *ox = 128;
        if (*oy > 128) *oy = 128;
        return 1;
    }
    *ox = *oy = 0;
    return 0;
}

/* Binds the texture for (texpage, CLUT, window, variant), converting what the UV box [u0..u1] x [v0..v1] needs. */
static void bind_texture(uint32_t tp, uint32_t cx, uint32_t cy, int variant, int u0, int v0, int u1, int v1) {
    int depth = (tp >> 7) & 3;
    uint32_t key = tp & 0x19Fu, clut = depth < 2 ? (cx | cy << 16) : 0, tw = s_tw;
    uint32_t pages = tex_pages(tp);
    int bx, by, cls = block_for(u0, v0, u1, v1, &bx, &by);
    if (pages & s_sg_newer)                                             /* render to texture: read what was drawn */
        sync_bands(pages, tw ? 0xFFFFu : ((2u << ((by + s_cls_size[cls] - 1) >> 4)) - 1u) & ~((1u << (by >> 4)) - 1u));
    uint32_t vs = tw ? vsum_of(pages, 0, 255) : vsum_of(pages, by, by + s_cls_size[cls] - 1);
    uint32_t chk = depth < 2 ? clut_sum(cx, cy, depth == 0 ? 16 : 256) : 0;
    int first = s_cls_first[cls], n = s_cls_n[cls];
    /* The CLUT is identified by its contents (chk), not its position: games re-upload palettes to other spots of VRAM. */
    uint32_t h = (((key * 0x9E3779B1u ^ tw) * 0x85EBCA6Bu ^ vs) * 0xC2B2AE35u ^ chk ^ (uint32_t)variant << 28
                  ^ (uint32_t)cls << 24 ^ (uint32_t)bx << 8 ^ (uint32_t)by) * 0x27D4EB2Fu;
#define SLOT_OK(t) ((t)->used && (t)->hash == h && (t)->key == key && (t)->tw == tw && (t)->variant == variant && (t)->vsum == vs \
                    && (t)->chk == chk && (t)->ox == bx && (t)->oy == by && (t)->size == s_cls_size[cls])
    int hit = -1;
    if (s_mru >= 0 && SLOT_OK(&s_tex[s_mru])) hit = s_mru;
    for (int i = s_hhead[h >> 22]; hit < 0 && i >= 0; i = s_tex[i].next) if (SLOT_OK(&s_tex[i])) hit = i;
#undef SLOT_OK
    st_bind++; st_bindc[cls]++;
    if (hit < 0) {
        st_new++; st_newc[cls]++;
        if (st_samples < 48 && *(volatile uint32_t *)0x1F030010u > 9400u) {      /* bring-up: what the misses are */
            st_samples++;
            lion_logf("tex miss: tp|variant<<16|cls<<20 / clut", key | (uint32_t)variant << 16 | (uint32_t)cls << 20, clut);
            lion_logf("tex miss: ox<<8|oy / chk", (uint32_t)bx << 8 | (uint32_t)by, chk);
        }
        int v = -1;                                                     /* CLOCK: the first slot not used since the hand passed */
        for (int k = 0; k < 2 * n && v < 0; k++) {
            int i = first + s_hand[cls];
            s_hand[cls] = s_hand[cls] + 1 == n ? 0 : s_hand[cls] + 1;
            if (!s_tex[i].used || !s_tex[i].ref) v = i; else s_tex[i].ref = 0;
        }
        TexSlot *t = &s_tex[v];
        if (t->used) {                                                  /* unlink it from its old chain */
            st_evict[cls]++;
            int16_t *pp = &s_hhead[t->hash >> 22];
            while (*pp != v) pp = &s_tex[*pp].next;
            *pp = t->next;
        }
        /* Queued draws may still sample the old contents: wait for the token sent when the slot was last unbound (bind change). */
        if (s_bound == (uint32_t)v) wait_idle();
        else if (s_busy) while ((int32_t)(SPIKE_SYNC - t->fence) < 0) { }
        t->key = key; t->tw = tw; t->variant = (uint8_t)variant; t->vsum = vs; t->chk = chk; t->used = 1; t->hash = h;
        t->ox = (uint16_t)bx; t->oy = (uint16_t)by;
        t->next = s_hhead[h >> 22]; s_hhead[h >> 22] = (int16_t)v;
        for (int i = 0; i < 8; i++) t->done[i] = 0;
        hit = v;
    }
    need_tiles(hit, tp, cx, cy, u0, v0, u1, v1);
    s_tex[hit].ref = 1; s_mru = hit;
    if (s_bound != (uint32_t)hit) {
        if (s_bound < TEX_SLOTS) { put(OP_SYNC << 24 | 1); put(++s_token); s_tex[s_bound].fence = s_token; }   /* its draws end here */
        s_bound = (uint32_t)hit;
        put(OP_SET_TEXTURE << 24 | 3); put(s_tex[hit].addr); put(s_tex[hit].log2); put(s_tex[hit].log2); s_busy = 1;
    }
    s_box_ox = (float)s_tex[hit].ox; s_box_oy = (float)s_tex[hit].oy; s_box_scale = 1.0f / (float)s_tex[hit].size;
}
static inline float bu(float u) { return (u * 256.0f - s_box_ox) * s_box_scale; }   /* page UV (fraction of 256) -> bound block */
static inline float bv(float v) { return (v * 256.0f - s_box_oy) * s_box_scale; }

/* ---- primitives ------------------------------------------------------------------------------------------- */
typedef struct { int x, y; float u, v; uint32_t argb; } Vtx;

/* Blend mode and alpha for the current semi-transparency setting; *rgb_scale = 0 turns the color black (mode 2). */
static uint32_t blend_mode(uint32_t *alpha, int *black) {
    *black = 0;
    if (!s_semi) { *alpha = 255; return 0; }
    switch (s_semi_mode) {
    case 0: *alpha = 128; return M_BLEND;                               /* B/2 + F/2 */
    case 1: *alpha = 255; return M_BLENDADD;                            /* B + F */
    case 2: *alpha = 128; *black = 1; return M_BLEND;                   /* B - F: darken instead */
    default: *alpha = 64; return M_BLENDADD;                            /* B + F/4 */
    }
}
static inline uint32_t mod8(int c) { c *= 2; return (uint32_t)(c > 255 ? 255 : c); }    /* PS1: texel * c / 128 */
static uint32_t tex_argb(uint32_t c24, uint32_t a) {
    if (s_raw) return a << 24 | 0xFFFFFFu;
    return a << 24 | mod8((int)(c24 & 0xFF)) << 16 | mod8((int)((c24 >> 8) & 0xFF)) << 8 | mod8((int)((c24 >> 16) & 0xFF));
}
static inline uint32_t rgb_argb(uint32_t rgb888_bgr, uint32_t a) {     /* PS1 0xBBGGRR -> Spike ARGB */
    return a << 24 | (rgb888_bgr & 0xFF) << 16 | (rgb888_bgr & 0xFF00) | ((rgb888_bgr >> 16) & 0xFF);
}
static int too_big(const Vtx *v) {                                      /* the PS1 skips polygons this large */
    int minx = v[0].x, maxx = v[0].x, miny = v[0].y, maxy = v[0].y;
    for (int i = 1; i < 3; i++) {
        if (v[i].x < minx) minx = v[i].x; if (v[i].x > maxx) maxx = v[i].x;
        if (v[i].y < miny) miny = v[i].y; if (v[i].y > maxy) maxy = v[i].y;
    }
    if (maxx - minx > 1023 || maxy - miny > 511) return 1;
    mark_drawn(minx, miny, maxx, maxy);
    return 0;
}
static void emit_tri(const Vtx *v, uint32_t mode) {
    set_mode(mode);
    put(OP_TRI << 24 | 21);
    for (int i = 0; i < 3; i++) {
        put(fbits((float)(v[i].x - s_cx1))); put(fbits((float)(v[i].y - s_cy1))); put(0); put(0x3F800000u);
        put(v[i].argb); put(fbits(v[i].u)); put(fbits(v[i].v));
    }
    s_busy = 1;
}
static void emit_tri_tex(const Vtx *v, uint32_t mode) {               /* UVs rebased to the bound block */
    Vtx r[3];
    for (int i = 0; i < 3; i++) { r[i] = v[i]; r[i].u = bu(v[i].u); r[i].v = bv(v[i].v); }
    emit_tri(r, mode);
}
/* A textured triangle, in one pass or (semi-transparent) two: opaque texels unblended, STP texels blended. */
static void tri_box(const Vtx *v, int *u0, int *v0, int *u1, int *v1) {
    float fu0 = v[0].u, fu1 = v[0].u, fv0 = v[0].v, fv1 = v[0].v;
    for (int i = 1; i < 3; i++) {
        if (v[i].u < fu0) fu0 = v[i].u; if (v[i].u > fu1) fu1 = v[i].u;
        if (v[i].v < fv0) fv0 = v[i].v; if (v[i].v > fv1) fv1 = v[i].v;
    }
    *u0 = (int)(fu0 * 256.0f); *u1 = (int)(fu1 * 256.0f); *v0 = (int)(fv0 * 256.0f); *v1 = (int)(fv1 * 256.0f);   /* UVs are whole texels */
    if (*u0 < 0) *u0 = 0; if (*v0 < 0) *v0 = 0; if (*u1 > 255) *u1 = 255; if (*v1 > 255) *v1 = 255;
}
static void tex_tri(Vtx *v, uint32_t tp, uint32_t cx, uint32_t cy, const uint32_t *c24, int gouraud) {
    bind_target(); if (s_area_empty || too_big(v)) return;
    int bu0, bv0, bu1, bv1; tri_box(v, &bu0, &bv0, &bu1, &bv1);
    uint32_t a; int black; uint32_t bm = blend_mode(&a, &black);
    uint32_t base = M_TEXTURE | M_COLORKEY | (gouraud ? M_GOURAUD : 0);
    if (!s_semi) {
        for (int i = 0; i < 3; i++) v[i].argb = tex_argb(c24[gouraud ? i : 0], 255);
        bind_texture(tp, cx, cy, 0, bu0, bv0, bu1, bv1); emit_tri_tex(v, base);
        return;
    }
    for (int i = 0; i < 3; i++) v[i].argb = tex_argb(c24[gouraud ? i : 0], 255);
    bind_texture(tp, cx, cy, 1, bu0, bv0, bu1, bv1); emit_tri_tex(v, base);
    for (int i = 0; i < 3; i++) v[i].argb = black ? a << 24 : tex_argb(c24[gouraud ? i : 0], a);
    bind_texture(tp, cx, cy, 2, bu0, bv0, bu1, bv1); emit_tri_tex(v, base | bm);
}
static void color_tri(Vtx *v, int gouraud) {
    bind_target(); if (s_area_empty || too_big(v)) return;
    uint32_t a; int black; uint32_t bm = blend_mode(&a, &black);
    for (int i = 0; i < 3; i++) v[i].argb = black ? (a << 24) : ((v[i].argb & 0xFFFFFFu) | a << 24);
    emit_tri(v, (gouraud ? M_GOURAUD : 0) | bm);
}

void sk_draw_flat_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) {
    uint32_t c = c555_888(color);
    Vtx v[3] = { { x0, y0, 0, 0, c }, { x1, y1, 0, 0, c }, { x2, y2, 0, 0, c } };
    color_tri(v, 0);
}
void sk_draw_gouraud_triangle(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1, int x2, int y2, uint16_t c2) {
    Vtx v[3] = { { x0, y0, 0, 0, c555_888(c0) }, { x1, y1, 0, 0, c555_888(c1) }, { x2, y2, 0, 0, c555_888(c2) } };
    color_tri(v, 1);
}
#define UVN(t) ((float)(t) * (1.0f / 256.0f))
void sk_draw_textured_triangle(int x0, int y0, int u0, int v0, int x1, int y1, int u1, int v1, int x2, int y2, int u2, int v2,
                               uint16_t clut_x, uint16_t clut_y, uint16_t texpage) {
    Vtx v[3] = { { x0, y0, UVN(u0), UVN(v0), 0 }, { x1, y1, UVN(u1), UVN(v1), 0 }, { x2, y2, UVN(u2), UVN(v2), 0 } };
    uint32_t c24 = (uint32_t)s_mr | (uint32_t)s_mg << 8 | (uint32_t)s_mb << 16;
    tex_tri(v, texpage, clut_x, clut_y, &c24, 0);
}
void sk_draw_shaded_textured_triangle(int x0, int y0, int u0, int v0, uint32_t color0, int x1, int y1, int u1, int v1, uint32_t color1,
                                      int x2, int y2, int u2, int v2, uint32_t color2, uint16_t clut_x, uint16_t clut_y,
                                      uint16_t texpage, int raw) {
    Vtx v[3] = { { x0, y0, UVN(u0), UVN(v0), 0 }, { x1, y1, UVN(u1), UVN(v1), 0 }, { x2, y2, UVN(u2), UVN(v2), 0 } };
    uint32_t c24[3] = { color0, color1, color2 };
    int saved = s_raw; s_raw = raw;
    tex_tri(v, texpage, clut_x, clut_y, c24, !raw);
    s_raw = saved;
}

/* Rectangles: one QUAD (x, y, u, v per corner; shared z, 1/w and color). */
static void emit_quad(int x, int y, int w, int h, float u0, float v0, float u1, float v1, uint32_t argb, uint32_t mode) {
    set_mode(mode);
    float fx0 = (float)(x - s_cx1), fy0 = (float)(y - s_cy1), fx1 = fx0 + (float)w, fy1 = fy0 + (float)h;
    put(OP_QUAD << 24 | 19);
    put(fbits(fx0)); put(fbits(fy0)); put(fbits(u0)); put(fbits(v0));
    put(fbits(fx1)); put(fbits(fy0)); put(fbits(u1)); put(fbits(v0));
    put(fbits(fx1)); put(fbits(fy1)); put(fbits(u1)); put(fbits(v1));
    put(fbits(fx0)); put(fbits(fy1)); put(fbits(u0)); put(fbits(v1));
    put(0); put(0x3F800000u); put(argb);
    s_busy = 1;
}
void sk_draw_flat_rect(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    bind_target(); if (s_area_empty) return;
    mark_drawn(x, y, x + w - 1, y + h - 1);
    uint32_t a; int black; uint32_t bm = blend_mode(&a, &black);
    emit_quad(x, y, w, h, 0, 0, 0, 0, black ? a << 24 : (c555_888(color) | a << 24), bm);
}
void sk_draw_textured_rect(int x, int y, int w, int h, int u, int v, uint16_t clut_x, uint16_t clut_y, uint16_t texpage) {
    if (w <= 0 || h <= 0) return;
    bind_target(); if (s_area_empty) return;
    mark_drawn(x, y, x + w - 1, y + h - 1);
    uint32_t a; int black; uint32_t bm = blend_mode(&a, &black);
    uint32_t c24 = (uint32_t)s_mr | (uint32_t)s_mg << 8 | (uint32_t)s_mb << 16;
    float u0 = UVN(u), v0 = UVN(v), u1 = UVN(u + w), v1 = UVN(v + h);
    int bu0 = u, bv0 = v, bu1 = u + w, bv1 = v + h;                     /* past 255 the rectangle wraps: take the whole width */
    if (bu1 > 255) { bu0 = 0; bu1 = 255; }
    if (bv1 > 255) { bv0 = 0; bv1 = 255; }
    bind_texture(texpage, clut_x, clut_y, s_semi ? 1 : 0, bu0, bv0, bu1, bv1);
    emit_quad(x, y, w, h, bu(u0), bv(v0), bu(u1), bv(v1), tex_argb(c24, 255), M_TEXTURE | M_COLORKEY);
    if (!s_semi) return;
    bind_texture(texpage, clut_x, clut_y, 2, bu0, bv0, bu1, bv1);
    emit_quad(x, y, w, h, bu(u0), bv(v0), bu(u1), bv(v1), black ? a << 24 : tex_argb(c24, a), M_TEXTURE | M_COLORKEY | bm);
}

/* Lines: a one-pixel-wide parallelogram (two triangles) along the major axis, both endpoint pixels included. */
static void line(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, int gouraud) {
    int dx = x1 - x0, dy = y1 - y0;
    if (dx > 1023 || -dx > 1023 || dy > 511 || -dy > 511) return;
    bind_target(); if (s_area_empty) return;
    mark_drawn(x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, x0 > x1 ? x0 : x1, y0 > y1 ? y0 : y1);
    int xa = x0, ya = y0, xb = x1, yb = y1, ox = 0, oy = 0;
    if ((dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy)) { oy = 1; if (dx >= 0) xb += 1; else xa += 1; }   /* x-major */
    else { ox = 1; if (dy >= 0) yb += 1; else ya += 1; }                                               /* y-major */
    uint32_t al; int black; uint32_t bm = blend_mode(&al, &black);
    if (black) c0 = c1 = 0;
    c0 |= al << 24; c1 |= al << 24;
    Vtx a[3] = { { xa, ya, 0, 0, c0 }, { xb, yb, 0, 0, c1 }, { xb + ox, yb + oy, 0, 0, c1 } };
    Vtx b[3] = { { xa, ya, 0, 0, c0 }, { xb + ox, yb + oy, 0, 0, c1 }, { xa + ox, ya + oy, 0, 0, c0 } };
    emit_tri(a, (gouraud ? M_GOURAUD : 0) | bm); emit_tri(b, (gouraud ? M_GOURAUD : 0) | bm);
}
void sk_draw_line(int x0, int y0, int x1, int y1, uint16_t color) { uint32_t c = c555_888(color); line(x0, y0, c, x1, y1, c, 0); }
void sk_draw_shaded_line(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1) { line(x0, y0, c555_888(c0), x1, y1, c555_888(c1), 1); }

/* ---- VRAM transfers ----------------------------------------------------------------------------------------- */
/* GP0(02): ignores the drawing area; the Spike clears the rectangle (set as a temporary target). */
void sk_fill_rect(int x, int y, int w, int h, uint16_t color) {
    x &= VW - 1; y &= VH - 1;
    if (x + w > VW) w = VW - x; if (y + h > VH) h = VH - y;
    if (w <= 0 || h <= 0) return;
    put(OP_SET_TARGET << 24 | 5);
    put(SG_VRAM + (uint32_t)y * SG_STRIDE + (uint32_t)x * 2u); put(SG_STRIDE); put((uint32_t)w); put((uint32_t)h); put(0);
    put(OP_CLEAR << 24 | 3); put(1); put(to565(color)); put(0);
    s_busy = 1; s_target_dirty = 1;
    mark_bands(x, y, x + w - 1, y + h - 1);
}

/* One row of a CPU->VRAM upload: both copies. */
void sk_vram_transfer_in(int x, int y, int w, int h, const uint16_t *data) {
    if (!data || w <= 0 || h <= 0) return;
    wait_idle();                                                        /* a pending draw could land over the upload */
    for (int r = 0; r < h; r++) {
        int py = (y + r) & (VH - 1);
        uint16_t *d = &g_vram[py * VW];
        for (int c = 0; c < w; c++) {
            int px = (x + c) & (VW - 1);
            uint16_t p = data[r * w + c];
            d[px] = p; *sg_px(px, py) = to565(p);
        }
    }
    bump_rect(x, y, w, h);
}

uint16_t sk_vram_read(int x, int y) {
    x &= VW - 1; y &= VH - 1;
    uint32_t p = 1u << page_of(x, y);
    if (s_sg_newer & p) sync_bands(p, 1u << ((y >> 4) & 15));
    return g_vram[y * VW + x];
}

/* GP0(80): VRAM to VRAM, through g_vram (the source is brought up to date first), then the rows go to SGRAM. */
void sk_copy_rect(int sx, int sy, int dx, int dy, int w, int h) {
    static uint16_t row[VW];
    uint32_t src = 0;
    for (int r = 0; r < h; r += 256) for (int c = 0; c < w; c += 64) src |= 1u << page_of(sx + c, sy + r);
    for (int r = 0; r < h; r += 256) src |= 1u << page_of(sx + w - 1, sy + r);
    for (int c = 0; c < w; c += 64) src |= 1u << page_of(sx + c, sy + h - 1);
    src |= 1u << page_of(sx + w - 1, sy + h - 1);
    sync_back(src);                                                     /* the source must hold what was drawn */
    wait_idle();
    for (int r = 0; r < h; r++) {
        int ys = (sy + r) & (VH - 1), yd = (dy + r) & (VH - 1);
        for (int c = 0; c < w; c++) row[c] = g_vram[ys * VW + ((sx + c) & (VW - 1))];
        for (int c = 0; c < w; c++) { int xd = (dx + c) & (VW - 1); g_vram[yd * VW + xd] = row[c]; *sg_px(xd, yd) = to565(row[c]); }
    }
    bump_rect(dx, dy, w, h);
}

/* ---- scanout ---------------------------------------------------------------------------------------------------- */
/* A 320-wide, 240-line, 15-bit display is the VRAM itself: the VI reads it straight from SGRAM. Returns 0 when gpu.c
 * must present (24-bit FMV from g_vram, or a width that needs resampling: then SGRAM is copied into `fb`). */
int sk_present(uint32_t disp_x, uint32_t disp_y, const uint16_t *xmap, int vres, int depth24, uint32_t fb, uint32_t fb_stride) {
    if (depth24) return 0;
    volatile uint32_t *vi = (volatile uint32_t *)0x1F030000u;
    int identity = !vres;
    for (int i = 0; identity && i < 320; i++) identity = xmap[i] == i;
    if (identity && disp_x + 320 <= VW && disp_y + 240 <= VH) {
        vi[1] = SG_VRAM + disp_y * SG_STRIDE + disp_x * 2u; vi[2] = SG_STRIDE;
        return 1;
    }
    for (int oy = 0; oy < 240; oy++) {
        int sy = (int)(disp_y + (vres ? oy * 2 : oy)) & (VH - 1);
        const uint16_t *src = sg_px(0, sy);
        uint32_t *dst = (uint32_t *)(uintptr_t)(fb + (uint32_t)oy * fb_stride);
        for (int ox = 0; ox < 320; ox += 2)
            dst[ox >> 1] = (uint32_t)src[(disp_x + xmap[ox]) & (VW - 1)] << 16 | src[(disp_x + xmap[ox + 1]) & (VW - 1)];
    }
    vi[1] = fb; vi[2] = fb_stride;
    return 1;
}
