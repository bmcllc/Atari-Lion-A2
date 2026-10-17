// Unreal-level viewer for the Atari Lion A2 (Tuffy, freestanding C).
// Reads the level blob written by unrport (see unrport.cpp) from kLevel, transforms it
// on the FPU and rasterises it on the Spike (Z-buffered, Gouraud).
typedef unsigned int u32;
typedef int i32;

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
#define LEVEL_ADDR 0x00200000u
#define LEVEL_MAGIC 0x554C564Cu  // "ULVL"

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

int main(void) {
  const u32 *lv = (const u32 *)LEVEL_ADDR;
  u32 npoly = lv[0] == LEVEL_MAGIC ? lv[1] : 0;
  u32 ntex = npoly ? lv[2] : 0;
  const float *start = (const float *)(lv + 3);  // PlayerStart: x, y, z, yaw, present
  const u32 *textab = lv + 8, *first = lv + 8 + 4 * ntex;  // per texture: addr, log2 w, log2 h, levels

  // Bounds of the level keep the camera inside it.
  float lo[3] = {-128, -128, -128}, hi[3] = {128, 128, 128};
  if (npoly) {
    const u32 *p = first;  // per polygon: nv, colour, texture, normal[3], then nv x (x, y, z, s, t)
    for (int k = 0; k < 3; ++k) lo[k] = 1e9f, hi[k] = -1e9f;
    for (u32 i = 0; i < npoly; ++i) {
      u32 nv = p[0];
      const float *f = (const float *)(p + 6);
      for (u32 j = 0; j < nv; ++j)
        for (int k = 0; k < 3; ++k) {
          if (f[5 * j + k] < lo[k]) lo[k] = f[5 * j + k];
          if (f[5 * j + k] > hi[k]) hi[k] = f[5 * j + k];
        }
      p += 6 + 5 * nv;
    }
  }

  REG(VI + 0) = 1;  // 240p
  REG(VI + 4) = BUF0;
  REG(VI + 8) = W * 2;

  float yaw = 0.6f, pitch = 0.12f;
  for (int k = 0; k < 3; ++k) cam[k] = (lo[k] + hi[k]) * 0.5f;
  if (npoly && lv[7]) {
    for (int k = 0; k < 3; ++k) cam[k] = start[k];
    yaw = start[3];
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

    u32 cur = 0xFFFFFFFFu, mode = 0;  // texture currently bound
    const u32 *p = first;
    for (u32 i = 0; i < npoly; ++i) {
      u32 nv = p[0], tex = p[2];
      if (tex != cur || !mode) {
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
      draw_poly((const float *)(p + 6), nv, p[1]);
      p += 6 + 5 * nv;
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
