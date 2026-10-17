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
  if (flags & 1) {
    for (u32 y = 0; y < height_; ++y) {
      u8* row = bus_.host_range(color_addr_ + y * stride_, width_ * 2);
      if (!row) continue;
      for (u32 x = 0; x < width_; ++x) store_be16(row + x * 2, u16(color));
    }
    pixels_ += u64(width_) * height_;
  }
  if (flags & 2) {
    u8* zb = bus_.host_range(z_addr_, width_ * height_ * 4);
    if (zb)
      for (u32 i = 0; i < width_ * height_; ++i) store_be32(zb + i * 4, z & 0xFFFFFF);
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

  const bool persp = mode & kPerspective, gouraud = mode & kGouraud, blend = mode & kBlend;
  const bool ztest = zbuf && (mode & kZTest), zwrite = zbuf && (mode & kZWrite);
  const bool textured = mode & kTexture;
  const bool mip = textured && (mode & kMipmap) && tex_levels_ > 1;
  if (textured) resolve_levels();
  const float tex_w = float(1u << tex_w_log2_), tex_h = float(1u << tex_h_log2_);

  // Normalised barycentric weight k at (x, y) = E[k] + DX[k] * x + DY[k] * y.
  const float inv_area = 1.0f / area;
  float E[3], DX[3], DY[3];
  for (int k = 0; k < 3; ++k) {
    const int a = (k + 1) % 3, b = (k + 2) % 3;
    DX[k] = -(Y[b] - Y[a]) * inv_area;
    DY[k] = (X[b] - X[a]) * inv_area;
    E[k] = ((X[b] - X[a]) * (-Y[a]) - (Y[b] - Y[a]) * (-X[a])) * inv_area;
  }

  // Perspective-correct weights from linear barycentrics.
  auto correct = [&](float b0, float b1, float b2, float* q) {
    q[0] = b0, q[1] = b1, q[2] = b2;
    if (persp) {
      q[0] *= IW[0], q[1] *= IW[1], q[2] *= IW[2];
      const float s = q[0] + q[1] + q[2];
      if (s != 0.0f) {
        const float inv = 1.0f / s;
        q[0] *= inv, q[1] *= inv, q[2] *= inv;
      }
    }
  };

  // Rows are independent: large triangles are split into bands across threads.
  auto raster_rows = [&](int y_begin, int y_end) -> u64 {
  u64 drawn = 0;
  for (int y = y_begin; y < y_end; ++y) {
    const float py = float(y) + 0.5f;
    u8* const crow = color + size_t(y) * stride;
    float w0 = E[0] + DY[0] * py + DX[0] * (float(minx) + 0.5f);
    float w1 = E[1] + DY[1] * py + DX[1] * (float(minx) + 0.5f);
    float w2 = E[2] + DY[2] * py + DX[2] * (float(minx) + 0.5f);
    for (int x = minx; x <= maxx; ++x, w0 += DX[0], w1 += DX[1], w2 += DX[2]) {
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;

      const float z = w0 * Z[0] + w1 * Z[1] + w2 * Z[2];
      const u32 z24 = u32(std::clamp(z, 0.0f, 1.0f) * float(0xFFFFFF));
      u8* const zp = zbuf ? zbuf + (size_t(y) * width + size_t(x)) * 4 : nullptr;
      if (ztest && z24 >= (load_be32(zp) & 0xFFFFFF)) continue;

      float q[3];
      correct(w0, w1, w2, q);

      float r, g, b, a;
      if (gouraud) {
        r = q[0] * CR[0] + q[1] * CR[1] + q[2] * CR[2];
        g = q[0] * CG[0] + q[1] * CG[1] + q[2] * CG[2];
        b = q[0] * CB[0] + q[1] * CB[1] + q[2] * CB[2];
        a = q[0] * CA[0] + q[1] * CA[1] + q[2] * CA[2];
      } else {
        r = CR[0], g = CG[0], b = CB[0], a = CA[0];
      }
      if (textured) {
        const float u = q[0] * U[0] + q[1] * U[1] + q[2] * U[2];
        const float tv = q[0] * V[0] + q[1] * V[1] + q[2] * V[2];
        float lod = 0.0f;
        if (mip) {
          // Level of detail from the texel footprint of one pixel step in x and y.
          float qx[3], qy[3];
          correct(w0 + DX[0], w1 + DX[1], w2 + DX[2], qx);
          correct(w0 + DY[0], w1 + DY[1], w2 + DY[2], qy);
          const float dux = (qx[0] * U[0] + qx[1] * U[1] + qx[2] * U[2] - u) * tex_w;
          const float dvx = (qx[0] * V[0] + qx[1] * V[1] + qx[2] * V[2] - tv) * tex_h;
          const float duy = (qy[0] * U[0] + qy[1] * U[1] + qy[2] * U[2] - u) * tex_w;
          const float dvy = (qy[0] * V[0] + qy[1] * V[1] + qy[2] * V[2] - tv) * tex_h;
          const float m = std::max(dux * dux + dvx * dvx, duy * duy + dvy * dvy);
          lod = m > 0.0f ? 0.5f * fast_log2(m) : 0.0f;  // log2 of the larger footprint length
        }
        const u32 t = sample_lod(u, tv, lod);
        constexpr float k = 1.0f / 255.0f;
        r *= float((t >> 16) & 0xFF) * k;
        g *= float((t >> 8) & 0xFF) * k;
        b *= float(t & 0xFF) * k;
      }
      u8* const cp = crow + size_t(x) * 2;
      if (blend) {
        const float al = a / 255.0f;
        const u32 dst = rgb565_to_xrgb(load_be16(cp));
        r = r * al + float((dst >> 16) & 0xFF) * (1 - al);
        g = g * al + float((dst >> 8) & 0xFF) * (1 - al);
        b = b * al + float(dst & 0xFF) * (1 - al);
      }
      store_be16(cp, xrgb_to_rgb565(u32(std::clamp(r, 0.0f, 255.0f)), u32(std::clamp(g, 0.0f, 255.0f)),
                                    u32(std::clamp(b, 0.0f, 255.0f))));
      if (zwrite) store_be32(zp, (load_be32(zp) & 0xFF00'0000u) | z24);
      ++drawn;
    }
  }
  return drawn;
  };

  const int rows = maxy - miny + 1;
  const u64 bbox = u64(rows) * u64(maxx - minx + 1);
  if (pool_ && bbox >= kParallelArea && rows >= 16) {
    const unsigned bands = std::min<unsigned>(unsigned(rows) / 8, (pool_->size() + 1) * 2);
    std::vector<u64> counts(bands, 0);
    pool_->run(bands, [&](unsigned i) {
      const int y0 = miny + int(u64(rows) * i / bands), y1 = miny + int(u64(rows) * (i + 1) / bands);
      counts[i] = raster_rows(y0, y1);
    });
    for (u64 c : counts) pixels_ += c;
  } else {
    pixels_ += raster_rows(miny, maxy + 1);
  }
}

}  // namespace lion
