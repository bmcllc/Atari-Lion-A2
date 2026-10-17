// Unreal-level viewer for the Atari Lion A2 (Tuffy, freestanding C++).
// Unreal's own Core (compiled for PowerPC, see core/) opens the .unr package from the RAM disc and
// reads the BSP; this file transforms it on the FPU and rasterises it on the Spike (Z-buffered,
// Gouraud, textured from the table written by unrport).
#include "core/level.h"

typedef unsigned int u32;
typedef int i32;

extern "C" void lion_core_init();
extern "C" void lion_halt(const char* why);
extern "C" void lion_engine_test();
extern "C" void lion_engine_boot();
extern "C" void lion_engine_tick();

#define REG(a) (*(volatile u32 *)(a))
#define SPIKE 0x1F010000u
#define VI 0x1F030000u
#define PAD 0x1F050000u
#define SGRAM 0x10000000u
#define W 320
#define H 240
#define BUF0 SGRAM
#define BUF1 (SGRAM + 0x80000u)
#define ZBUF (SGRAM + 0x200000u)
#define TEXTAB_ADDR 0x00300000u  // texture table from unrport: magic, count, wall/floor/ceiling slots, entries
#define TEXTAB_MAGIC 0x554C5458u  // "ULTX"

enum { kSetTarget = 1, kClear = 2, kSetMode = 3, kSetTexture = 4, kTri = 5, kSync = 6 };
enum { kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kA = 16, kB = 32, kL = 1 << 10, kR = 1 << 11 };

static inline u32 fbits(float f) {
  union { float f; u32 u; } x;
  x.f = f;
  return x.u;
}
static inline void put(u32 v) { REG(SPIKE) = v; }

static float fsin(float x) {
  const float pi = 3.14159265f, two_pi = 6.28318531f;
  while (x > pi) x -= two_pi;
  while (x < -pi) x += two_pi;
  float y = 1.27323954f * x - 0.405284735f * x * (x < 0 ? -x : x);
  float a = y < 0 ? -y : y;
  return 0.225f * (y * a - y) + y;
}
static float fcos(float x) { return fsin(x + 1.57079633f); }

typedef struct { float r, up, f, s, t; } V3;  // view space: right, up, forward; texture coordinates

static float cam[3], cy, sy, cp, sp;

static V3 to_view(const float *p) {  // p = x, y, z, s, t
  float dx = p[0] - cam[0], dy = p[1] - cam[1], dz = p[2] - cam[2];
  float fwd = dx * cy + dy * sy, right = -dx * sy + dy * cy;
  V3 v = {right, dz * cp - fwd * sp, fwd * cp + dz * sp, p[3], p[4]};
  return v;
}

static u32 shade(u32 argb, float f) {
  float s = 1.0f - f * (1.0f / 700.0f);
  if (s < 0.2f) s = 0.2f;
  if (s > 1.0f) s = 1.0f;
  u32 r = (u32)(((argb >> 16) & 255) * s), g = (u32)(((argb >> 8) & 255) * s), b = (u32)((argb & 255) * s);
  return 0xFF000000u | r << 16 | g << 8 | b;
}

static void emit(const V3 *v, u32 argb) {
  const float zn = 8.0f, zf = 4096.0f, focal = 200.0f, zs = 1.0f / (1.0f - zn / zf);
  float zd = (1.0f - zn / v->f) * zs;
  if (zd < 0) zd = 0;
  if (zd > 1) zd = 1;
  put(fbits(W * 0.5f + focal * v->r / v->f));
  put(fbits(H * 0.5f - focal * v->up / v->f));
  put(fbits(zd));
  put(fbits(1.0f / v->f));
  put(shade(argb, v->f));
  put(fbits(v->s));
  put(fbits(v->t));
}

static void draw_poly(const float *v, u32 n, u32 argb) {
  const float near = 8.0f;
  V3 in[16], out[18];
  u32 m = 0;
  for (u32 i = 0; i < n; ++i) in[i] = to_view(v + 5 * i);
  for (u32 i = 0; i < n; ++i) {
    V3 a = in[i], b = in[(i + 1) % n];
    int ain = a.f >= near, bin = b.f >= near;
    if (ain) out[m++] = a;
    if (ain != bin) {
      float t = (near - a.f) / (b.f - a.f);
      V3 c = {a.r + (b.r - a.r) * t, a.up + (b.up - a.up) * t, near, a.s + (b.s - a.s) * t, a.t + (b.t - a.t) * t};
      out[m++] = c;
    }
  }
  for (u32 i = 1; i + 1 < m; ++i) {
    put(kTri << 24 | 21);
    emit(&out[0], argb);
    emit(&out[i], argb);
    emit(&out[i + 1], argb);
  }
}

static LvLevel g_level;
static float g_vtx[LV_MAX_POLYS][16 * 5];  // x, y, z, s, t per vertex
static u32 g_argb[LV_MAX_POLYS], g_tex[LV_MAX_POLYS];

static u32 color_for(const float *n) {
  float ax = n[0] < 0 ? -n[0] : n[0], ay = n[1] < 0 ? -n[1] : n[1], az = n[2] < 0 ? -n[2] : n[2];
  if (az >= ax && az >= ay) return n[2] > 0 ? 0xFF3A4660u : 0xFF6B5A3Cu;  // ceiling slate, floor earth
  if (ax >= ay) return n[0] > 0 ? 0xFFB05A4Au : 0xFF4F8F6Au;
  return n[1] > 0 ? 0xFF4A6FB0u : 0xFFB0A04Au;
}

extern "C" int main(void) {
  lion_core_init();
  lion_engine_test();
  lion_engine_boot();
  for (;;) lion_engine_tick();  // draws through ULionRenderDevice (liondrv/LionRenDev.cpp)
  if (!lion_level_load("level.unr", &g_level)) lion_halt("level load failed");
  const u32 npoly = g_level.count;
  const u32 *tt = (const u32 *)TEXTAB_ADDR;
  const u32 ntex = tt[0] == TEXTAB_MAGIC ? tt[1] : 0;
  const u32 *slot = tt + 2, *textab = tt + 5;  // per texture: addr, log2 w, log2 h, levels
  float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
  for (u32 i = 0; i < npoly; ++i) {
    const LvPoly &p = g_level.polys[i];
    const float *n = p.normal;
    float anz = n[2] < 0 ? -n[2] : n[2];
    u32 s = anz > 0.7f ? (n[2] > 0 ? slot[2] : slot[1]) : slot[0];
    if (ntex == 0 || s >= ntex) s = 0xFFFFFFFFu;
    g_tex[i] = s;
    g_argb[i] = s == 0xFFFFFFFFu ? color_for(n) : 0xFFFFFFFFu;
    float tw = s == 0xFFFFFFFFu ? 1.0f : (float)(1u << textab[4 * s + 1]);
    float th = s == 0xFFFFFFFFu ? 1.0f : (float)(1u << textab[4 * s + 2]);
    for (u32 j = 0; j < p.nv; ++j) {
      float d[3] = {p.v[3 * j] - p.base[0], p.v[3 * j + 1] - p.base[1], p.v[3 * j + 2] - p.base[2]};
      float *o = &g_vtx[i][5 * j];
      for (int k = 0; k < 3; ++k) {
        o[k] = p.v[3 * j + k];
        if (o[k] < lo[k]) lo[k] = o[k];
        if (o[k] > hi[k]) hi[k] = o[k];
      }
      o[3] = (d[0] * p.tex_u[0] + d[1] * p.tex_u[1] + d[2] * p.tex_u[2]) / tw;
      o[4] = (d[0] * p.tex_v[0] + d[1] * p.tex_v[1] + d[2] * p.tex_v[2]) / th;
    }
  }
  if (!npoly) for (int k = 0; k < 3; ++k) lo[k] = -128, hi[k] = 128;

  REG(VI + 0) = 1;  // 240p
  REG(VI + 4) = BUF0;
  REG(VI + 8) = W * 2;

  float yaw = 0.6f, pitch = 0.12f;
  for (int k = 0; k < 3; ++k) cam[k] = (lo[k] + hi[k]) * 0.5f;
  if (g_level.have_start) {
    for (int k = 0; k < 3; ++k) cam[k] = g_level.start[k];
    yaw = g_level.yaw;
    pitch = 0;
  }
  u32 token = 0, front = 0;
  for (;;) {
    u32 b = REG(PAD), st = REG(PAD + 4);
    float lx = (float)((i32)(st >> 24) - 128), ly = (float)((i32)((st >> 16) & 255) - 128);
    if (lx > -16 && lx < 16) lx = 0;
    if (ly > -16 && ly < 16) ly = 0;
    float turn = lx * (0.04f / 128), walk = -ly * (4.0f / 128), strafe = 0;
    if (b & kLeft) turn -= 0.04f;
    if (b & kRight) turn += 0.04f;
    if (b & kUp) walk += 4.0f;
    if (b & kDown) walk -= 4.0f;
    if (b & kL) strafe -= 4.0f;
    if (b & kR) strafe += 4.0f;
    if (b & kA) pitch += 0.02f;
    if (b & kB) pitch -= 0.02f;
    if (pitch > 1.4f) pitch = 1.4f;
    if (pitch < -1.4f) pitch = -1.4f;
    yaw += turn;
    cy = fcos(yaw), sy = fsin(yaw), cp = fcos(pitch), sp = fsin(pitch);
    cam[0] += cy * walk - sy * strafe;
    cam[1] += sy * walk + cy * strafe;
    for (int k = 0; k < 3; ++k) {
      if (cam[k] < lo[k] + 16) cam[k] = lo[k] + 16;
      if (cam[k] > hi[k] - 16) cam[k] = hi[k] - 16;
    }

    u32 back = front ^ 1;
    put(kSetTarget << 24 | 5);
    put(back ? BUF1 : BUF0);
    put(W * 2);
    put(W);
    put(H);
    put(ZBUF);
    put(kClear << 24 | 3);
    put(3);
    put(0x2D7F);  // sky blue, RGB565
    put(0xFFFFFF);

    u32 cur = 0xFFFFFFFEu, mode = 0;  // texture currently bound
    for (u32 i = 0; i < npoly; ++i) {
      u32 tex = g_tex[i];
      if (tex != cur) {
        cur = tex;
        // Z test | Z write | Gouraud, plus texture, bilinear, perspective, mipmap and trilinear.
        mode = tex < ntex ? 7 | 8 | 16 | 32 | 256 | 512 : 7;
        put(kSetMode << 24 | 1);
        put(mode);
        if (tex < ntex) {
          put(kSetTexture << 24 | 4);
          put(textab[4 * tex]);
          put(textab[4 * tex + 1]);
          put(textab[4 * tex + 2]);
          put(textab[4 * tex + 3]);
        }
      }
      draw_poly(g_vtx[i], g_level.polys[i].nv, g_argb[i]);
    }

    ++token;
    put(kSync << 24 | 1);
    put(token);
    while (REG(SPIKE + 8) != token) {}
    REG(VI + 4) = back ? BUF1 : BUF0;
    front = back;
    u32 fr = REG(VI + 0x10);
    while (REG(VI + 0x10) == fr) {}
  }
}
