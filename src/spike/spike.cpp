#include "spike/spike.h"

#include <algorithm>
#include <cmath>

namespace lion {

namespace {

inline u32 rgb565_to_xrgb(u16 c) {
  u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}
inline u16 xrgb_to_rgb565(u32 r, u32 g, u32 b) { return u16((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }

// Fast helpers for the per-pixel path (the rasterizer does not need libm precision).
inline int ifloor(float x) {
  const int i = int(x);
  return i - (float(i) > x);
}
// log2 with ~0.01 error: exponent plus a quadratic fit of the mantissa.
inline float fast_log2(float x) {
  const u32 b = f32_to_bits(x);
  const float m = bits_to_f32((b & 0x007F'FFFF) | 0x3F80'0000);  // [1, 2)
  return float(int((b >> 23) & 0xFF) - 128) + m * (2.0f - 0.33582878f * m) - 0.65871759f;
}

inline float edge(float ax, float ay, float bx, float by, float px, float py) {
  return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

}  // namespace

u32 Spike::read32(u32 offset, Master, bool*) {
  switch (offset) {
    case 0x04: return idle() ? 0 : 1;
    case 0x08: return sync_;
    case 0x0C: return u32(pixels_);
  }
  return 0;
}

void Spike::write32(u32 offset, u32 value, Master, bool* stall) {
  if (offset == 0x00 && !fifo_.push(value)) *stall = true;
}

void Spike::tick(u64 cycles) {
  u64 budget = cycles;
  if (debt_) {
    u64 pay = std::min(debt_, budget);
    debt_ -= pay;
    budget -= pay;
  }
  while (budget && !debt_) {
    u64 cost = 0;
    if (!execute_next(&cost)) break;
    u64 pay = std::min(cost, budget);
    budget -= pay;
    debt_ = cost - pay;
  }
}

// Pops and executes one complete command. Returns false if none is ready.
bool Spike::execute_next(u64* cost) {
  if (fifo_.empty()) return false;
  const u32 header = fifo_.peek(0);
  const u32 op = header >> 24, len = header & 0xFFFF;
  if (fifo_.size() < 1 + size_t(len)) return false;  // wait for the rest of the command
  u32 w[32] = {};
  u32 tmp;
  fifo_.pop(&tmp);
  for (u32 i = 0; i < len; ++i) {
    fifo_.pop(&tmp);
    if (i < 32) w[i] = tmp;
  }
  *cost = 1 + len;

  switch (op) {
    case kNop: break;
    case kSetTarget:
      color_addr_ = w[0];
      stride_ = w[1];
      width_ = std::min(w[2], 2048u);
      height_ = std::min(w[3], 2048u);
      z_addr_ = w[4];
      break;
    case kClear: {
      u64 before = pixels_;
      clear(w[0], w[1], w[2]);
      *cost += (pixels_ - before) / kPixelsPerCycle;
      pixels_ = before;  // clears are not counted as drawn pixels
      break;
    }
    case kSetMode: mode_ = w[0]; break;
    case kSetTexture:
      tex_addr_ = w[0];
      tex_w_log2_ = std::min(w[1], 10u);
      tex_h_log2_ = std::min(w[2], 10u);
      // Optional level count; the chain stops at 1x1.
      tex_levels_ = std::clamp(len >= 4 ? w[3] : 1u, 1u, std::max(tex_w_log2_, tex_h_log2_) + 1);
      break;
    case kTri: {
      if (len < 3 * kVertexWords) break;
      Vertex v[3];
      for (int i = 0; i < 3; ++i) {
        const u32* p = &w[i * kVertexWords];
        v[i] = {bits_to_f32(p[0]), bits_to_f32(p[1]), bits_to_f32(p[2]), bits_to_f32(p[3]),
                p[4],              bits_to_f32(p[5]), bits_to_f32(p[6])};
      }
      u64 before = pixels_;
      draw_triangle(v);
      *cost += kTriSetupCycles + (pixels_ - before) / kPixelsPerCycle;
      break;
    }
    case kSync:
      sync_ = w[0];
      pic_.raise(Pic::kSpike);
      break;
    default: LION_WARN("Spike: unknown command %02X", op); break;
  }
  return true;
}

void Spike::clear(u32 flags, u32 color, u32 z) {
  if (!width_ || !height_) return;
  // One row is built, then copied: the host does this with wide stores.
  if (flags & 1) {
    u8 pattern[2048 * 2];
    for (u32 x = 0; x < width_; ++x) store_be16(pattern + x * 2, u16(color));
    for (u32 y = 0; y < height_; ++y)
      if (u8* row = bus_.host_range(color_addr_ + y * stride_, width_ * 2)) std::memcpy(row, pattern, width_ * 2);
    pixels_ += u64(width_) * height_;
  }
  if (flags & 2) {
    if (u8* zb = bus_.host_range(z_addr_, width_ * height_ * 4)) {
      for (u32 x = 0; x < width_; ++x) store_be32(zb + x * 4, z & 0xFFFFFF);
      for (u32 y = 1; y < height_; ++y) std::memcpy(zb + size_t(y) * width_ * 4, zb, width_ * 4);
    }
    pixels_ += u64(width_) * height_;
  }
}

// Mip levels are stored one after another, each half the size of the previous one.
void Spike::resolve_levels() {
  u32 addr = tex_addr_;
  for (u32 l = 0; l < tex_levels_; ++l) {
    const u32 lw = std::max(1u, (1u << tex_w_log2_) >> l), lh = std::max(1u, (1u << tex_h_log2_) >> l);
    levels_[l] = {bus_.host_range(addr, lw * lh * 2), lw, lh};
    addr += lw * lh * 2;
  }
}

u32 Spike::sample(u32 level, float u, float v) const {
  const u8* tex = levels_[level].ptr;
  const u32 tw = levels_[level].w, th = levels_[level].h;
  if (!tex) return 0xFFFFFF;
  auto texel = [&](int x, int y) {
    x &= int(tw - 1);
    y &= int(th - 1);
    return rgb565_to_xrgb(load_be16(tex + (size_t(y) * tw + size_t(x)) * 2));
  };
  // Wrap first so huge coordinates keep their fractional precision.
  if (!(std::fabs(u) < 1e9f) || !(std::fabs(v) < 1e9f)) u = v = 0.0f;  // NaN / overflow guard
  u -= float(ifloor(u));
  v -= float(ifloor(v));
  float fx = u * float(tw), fy = v * float(th);
  if (!(mode_ & kBilinear)) return texel(ifloor(fx), ifloor(fy));
  fx -= 0.5f;
  fy -= 0.5f;
  const int x0 = ifloor(fx), y0 = ifloor(fy);
  // 8-bit fixed-point weights, as the hardware filter would use.
  const u32 ax = u32((fx - float(x0)) * 256.0f), ay = u32((fy - float(y0)) * 256.0f);
  const u32 c00 = texel(x0, y0), c10 = texel(x0 + 1, y0), c01 = texel(x0, y0 + 1), c11 = texel(x0 + 1, y0 + 1);
  // Two channels at a time: red|blue in one word, green in another.
  auto lerp = [](u32 a, u32 b, u32 t) { return (a * (256 - t) + b * t) >> 8; };
  const u32 rb = lerp(lerp(c00 & 0xFF00FF, c10 & 0xFF00FF, ax) & 0xFF00FF, lerp(c01 & 0xFF00FF, c11 & 0xFF00FF, ax) & 0xFF00FF, ay);
  const u32 g = lerp(lerp(c00 & 0xFF00, c10 & 0xFF00, ax) & 0xFF00, lerp(c01 & 0xFF00, c11 & 0xFF00, ax) & 0xFF00, ay);
  return (rb & 0xFF00FF) | (g & 0xFF00);
}

u32 Spike::sample_lod(float u, float v, float lod) const {
  if (!(mode_ & kMipmap) || tex_levels_ <= 1 || !(lod > 0.0f)) return sample(0, u, v);
  const float max_level = float(tex_levels_ - 1);
  lod = std::min(lod, max_level);
  if (!(mode_ & kTrilinear)) return sample(u32(lod + 0.5f), u, v);
  const u32 l0 = u32(lod);
  const u32 l1 = std::min(l0 + 1, tex_levels_ - 1);
  const u32 t = u32((lod - float(l0)) * 256.0f);
  const u32 a = sample(l0, u, v), b = sample(l1, u, v);
  const u32 rb = (((a & 0xFF00FF) * (256 - t) + (b & 0xFF00FF) * t) >> 8) & 0xFF00FF;
  const u32 g = (((a & 0xFF00) * (256 - t) + (b & 0xFF00) * t) >> 8) & 0xFF00;
  return rb | g;
}

void Spike::draw_triangle(const Vertex in[3]) {
  ++triangles_;
  if (!width_ || !height_) return;
  const Vertex* v[3] = {&in[0], &in[1], &in[2]};
  float area = edge(v[0]->x, v[0]->y, v[1]->x, v[1]->y, v[2]->x, v[2]->y);
  if (area == 0.0f || !std::isfinite(area)) return;
  // Front faces are counter-clockwise as seen on screen, which gives a negative
  // area in Y-down coordinates. Normalise to positive for the edge tests.
  if (area > 0.0f) {
    if (mode_ & kCullBack) return;
  } else {
    std::swap(v[1], v[2]);
    area = -area;
  }

  // Everything the pixel loop needs is copied into locals: the framebuffer is
  // written through byte pointers, which would otherwise force reloads.
  const u32 mode = mode_, width = width_, height = height_, stride = stride_;
  float X[3], Y[3], Z[3], IW[3], U[3], V[3], CR[3], CG[3], CB[3], CA[3];
  for (int i = 0; i < 3; ++i) {
    X[i] = v[i]->x, Y[i] = v[i]->y, Z[i] = v[i]->z, IW[i] = v[i]->inv_w, U[i] = v[i]->u, V[i] = v[i]->v;
    CR[i] = float((v[i]->argb >> 16) & 0xFF);
    CG[i] = float((v[i]->argb >> 8) & 0xFF);
    CB[i] = float(v[i]->argb & 0xFF);
    CA[i] = float(v[i]->argb >> 24);
  }

  const int minx = std::max(0, ifloor(std::min({X[0], X[1], X[2]})));
  const int maxx = std::min(int(width) - 1, int(std::ceil(std::max({X[0], X[1], X[2]}))));
  const int miny = std::max(0, ifloor(std::min({Y[0], Y[1], Y[2]})));
  const int maxy = std::min(int(height) - 1, int(std::ceil(std::max({Y[0], Y[1], Y[2]}))));
  if (minx > maxx || miny > maxy) return;

  u8* const color = bus_.host_range(color_addr_, stride * height);
  u8* const zbuf = (mode & (kZTest | kZWrite)) ? bus_.host_range(z_addr_, width * height * 4) : nullptr;
  if (!color) return;

  const bool persp = mode & kPerspective, gouraud = mode & kGouraud, blend = mode & (kBlend | kBlendAdd);
  const bool additive = mode & kBlendAdd;
  const bool ztest = zbuf && (mode & kZTest), zwrite = zbuf && (mode & kZWrite);
  const bool textured = mode & kTexture;
  const bool mip = textured && (mode & kMipmap) && tex_levels_ > 1;
  if (textured) resolve_levels();
  const float tex_w = float(1u << tex_w_log2_), tex_h = float(1u << tex_h_log2_);

  // Normalised barycentric weight k at (x, y) = E[k] + DX[k] * x + DY[k] * y.
  const float inv_area = 1.0f / area;
  Setup su;
  for (int k = 0; k < 3; ++k) {
    const int a = (k + 1) % 3, b = (k + 2) % 3;
    su.DX[k] = -(Y[b] - Y[a]) * inv_area;
    su.DY[k] = (X[b] - X[a]) * inv_area;
    su.E[k] = ((X[b] - X[a]) * (-Y[a]) - (Y[b] - Y[a]) * (-X[a])) * inv_area;
  }
  // Attributes as planes A(x, y) = c + dx * x + dy * y (exact for the affine case).
  auto plane = [&](const float* attr) {
    Plane pl{0, 0, 0};
    for (int k = 0; k < 3; ++k) {
      pl.c += su.E[k] * attr[k];
      pl.dx += su.DX[k] * attr[k];
      pl.dy += su.DY[k] * attr[k];
    }
    return pl;
  };
  const float* const attrs[kAttrs] = {Z, CR, CG, CB, CA, U, V};
  for (int i = 0; i < kAttrs; ++i) {
    su.P[i] = plane(attrs[i]);
    for (int k = 0; k < 3; ++k) su.A[i][k] = attrs[i][k];
  }
  for (int k = 0; k < 3; ++k) su.IW[k] = IW[k];
  su.color = color;
  su.zbuf = zbuf;
  su.stride = stride;
  su.width = width;
  su.minx = minx;
  su.maxx = maxx;
  su.ztest = ztest;
  su.zwrite = zwrite;
  su.additive = additive;
  su.mip = mip;
  su.tex_w = tex_w;
  su.tex_h = tex_h;
  // Without perspective the texel footprint, and so the level of detail, is constant.
  su.lod = 0.0f;
  if (mip && !persp) {
    const float dux = su.P[5].dx * tex_w, dvx = su.P[6].dx * tex_h, duy = su.P[5].dy * tex_w, dvy = su.P[6].dy * tex_h;
    const float m = std::max(dux * dux + dvx * dvx, duy * duy + dvy * dvy);
    su.lod = m > 0.0f ? 0.5f * fast_log2(m) : 0.0f;
  }

  const int key = (persp ? 16 : 0) | (gouraud ? 8 : 0) | (textured ? 4 : 0) | (blend ? 2 : 0) | (zbuf ? 1 : 0);
  auto rasterize = [&](int y0, int y1) -> u64 { return kRasterizers[key](*this, su, y0, y1); };

  // Rows are independent: large triangles are split into bands across threads.
  const int rows = maxy - miny + 1;
  const u64 bbox = u64(rows) * u64(maxx - minx + 1);
  if (pool_ && bbox >= kParallelArea && rows >= 16) {
    const unsigned bands = std::min<unsigned>(unsigned(rows) / 8, (pool_->size() + 1) * 2);
    std::array<u64, 64> counts{};
    pool_->run(bands, [&](unsigned i) {
      const int y0 = miny + int(u64(rows) * i / bands), y1 = miny + int(u64(rows) * (i + 1) / bands);
      counts[i] = rasterize(y0, y1);
    });
    for (unsigned i = 0; i < bands; ++i) pixels_ += counts[i];
  } else {
    pixels_ += rasterize(miny, maxy + 1);
  }
}

// One pixel loop per feature combination. Everything it touches is a local copy:
// the framebuffer is written through byte pointers, which would otherwise force
// every value to be reloaded per pixel.
template <bool kPersp, bool kGouraud, bool kTex, bool kBlend, bool kZ>
u64 Spike::raster(const Spike& sp, const Setup& in, int y_begin, int y_end) {
  const Setup su = in;
  u64 drawn = 0;
  constexpr float k255 = 1.0f / 255.0f;
  for (int y = y_begin; y < y_end; ++y) {
    const float py = float(y) + 0.5f;
    u8* const crow = su.color + size_t(y) * su.stride;
    // Span of this row inside all three edges (one pixel of margin; the per-pixel
    // test below stays exact). Thin diagonal triangles have huge bounding boxes.
    float lo = float(su.minx), hi = float(su.maxx);
    for (int k = 0; k < 3; ++k) {
      const float at = su.E[k] + su.DY[k] * py + su.DX[k] * 0.5f;  // weight at x = 0
      if (su.DX[k] > 0) lo = std::max(lo, -at / su.DX[k] - 1.0f);
      else if (su.DX[k] < 0) hi = std::min(hi, at / -su.DX[k] + 1.0f);
      else if (at < 0) hi = lo - 1.0f;
    }
    if (!(lo <= hi)) continue;
    const int x0 = std::max(su.minx, int(lo)), x1 = std::min(su.maxx, int(hi) + 1);
    const float px = float(x0) + 0.5f;
    float w0 = su.E[0] + su.DY[0] * py + su.DX[0] * px;
    float w1 = su.E[1] + su.DY[1] * py + su.DX[1] * px;
    float w2 = su.E[2] + su.DY[2] * py + su.DX[2] * px;
    // Affine attributes walk their planes; with perspective they come from the weights.
    float at[kAttrs];
    for (int i = 0; i < kAttrs; ++i) at[i] = su.P[i].c + su.P[i].dy * py + su.P[i].dx * px;
    for (int x = x0; x <= x1;
         ++x, w0 += su.DX[0], w1 += su.DX[1], w2 += su.DX[2], at[0] += su.P[0].dx, at[1] += su.P[1].dx,
         at[2] += su.P[2].dx, at[3] += su.P[3].dx, at[4] += su.P[4].dx, at[5] += su.P[5].dx, at[6] += su.P[6].dx) {
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;

      u32 z24 = 0;
      u8* zp = nullptr;
      if constexpr (kZ) {
        z24 = u32(int(std::clamp(at[0], 0.0f, 1.0f) * float(0xFFFFFF)));
        zp = su.zbuf + (size_t(y) * su.width + size_t(x)) * 4;
        if (su.ztest && z24 >= (load_be32(zp) & 0xFFFFFF)) continue;
      }

      float q[3] = {w0, w1, w2};
      if constexpr (kPersp) {
        q[0] *= su.IW[0], q[1] *= su.IW[1], q[2] *= su.IW[2];
        const float s = q[0] + q[1] + q[2];
        if (s != 0.0f) {
          const float inv = 1.0f / s;
          q[0] *= inv, q[1] *= inv, q[2] *= inv;
        }
      }
      auto attr = [&](int i) {
        if constexpr (kPersp) return q[0] * su.A[i][0] + q[1] * su.A[i][1] + q[2] * su.A[i][2];
        else return at[i];
      };

      float r, g, b, a;
      if constexpr (kGouraud) {
        r = attr(1), g = attr(2), b = attr(3), a = attr(4);
      } else {
        r = su.A[1][0], g = su.A[2][0], b = su.A[3][0], a = su.A[4][0];
      }
      if constexpr (kTex) {
        const float u = attr(5), tv = attr(6);
        float lod = su.lod;
        if (kPersp && su.mip) {
          // Level of detail from the texel footprint of one pixel step in x and y.
          auto weights = [&](float b0, float b1, float b2, float* o) {
            o[0] = b0 * su.IW[0], o[1] = b1 * su.IW[1], o[2] = b2 * su.IW[2];
            const float s = o[0] + o[1] + o[2];
            if (s != 0.0f) {
              const float inv = 1.0f / s;
              o[0] *= inv, o[1] *= inv, o[2] *= inv;
            }
          };
          float qx[3], qy[3];
          weights(w0 + su.DX[0], w1 + su.DX[1], w2 + su.DX[2], qx);
          weights(w0 + su.DY[0], w1 + su.DY[1], w2 + su.DY[2], qy);
          auto dot = [&](const float* w, int i) { return w[0] * su.A[i][0] + w[1] * su.A[i][1] + w[2] * su.A[i][2]; };
          const float dux = (dot(qx, 5) - u) * su.tex_w, dvx = (dot(qx, 6) - tv) * su.tex_h;
          const float duy = (dot(qy, 5) - u) * su.tex_w, dvy = (dot(qy, 6) - tv) * su.tex_h;
          const float m = std::max(dux * dux + dvx * dvx, duy * duy + dvy * dvy);
          lod = m > 0.0f ? 0.5f * fast_log2(m) : 0.0f;  // log2 of the larger footprint length
        }
        const u32 t = sp.sample_lod(u, tv, lod);
        r *= float((t >> 16) & 0xFF) * k255;
        g *= float((t >> 8) & 0xFF) * k255;
        b *= float(t & 0xFF) * k255;
      }
      u8* const cp = crow + size_t(x) * 2;
      if constexpr (kBlend) {
        const float al = a * k255;
        const u32 dst = rgb565_to_xrgb(load_be16(cp));
        const float keep = su.additive ? 1.0f : 1.0f - al;
        r = r * al + float((dst >> 16) & 0xFF) * keep;
        g = g * al + float((dst >> 8) & 0xFF) * keep;
        b = b * al + float(dst & 0xFF) * keep;
      }
      store_be16(cp, xrgb_to_rgb565(u32(int(std::clamp(r, 0.0f, 255.0f))), u32(int(std::clamp(g, 0.0f, 255.0f))),
                                    u32(int(std::clamp(b, 0.0f, 255.0f)))));
      if (kZ && su.zwrite) store_be32(zp, (load_be32(zp) & 0xFF00'0000u) | z24);
      ++drawn;
    }
  }
  return drawn;
}

#define LION_RASTER(n) &Spike::raster<bool((n) & 16), bool((n) & 8), bool((n) & 4), bool((n) & 2), bool((n) & 1)>
const Spike::RasterFn Spike::kRasterizers[32] = {
    LION_RASTER(0),  LION_RASTER(1),  LION_RASTER(2),  LION_RASTER(3),  LION_RASTER(4),  LION_RASTER(5),  LION_RASTER(6),
    LION_RASTER(7),  LION_RASTER(8),  LION_RASTER(9),  LION_RASTER(10), LION_RASTER(11), LION_RASTER(12), LION_RASTER(13),
    LION_RASTER(14), LION_RASTER(15), LION_RASTER(16), LION_RASTER(17), LION_RASTER(18), LION_RASTER(19), LION_RASTER(20),
    LION_RASTER(21), LION_RASTER(22), LION_RASTER(23), LION_RASTER(24), LION_RASTER(25), LION_RASTER(26), LION_RASTER(27),
    LION_RASTER(28), LION_RASTER(29), LION_RASTER(30), LION_RASTER(31)};
#undef LION_RASTER

}  // namespace lion
