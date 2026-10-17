#include "quacker/qve.h"

#include <algorithm>

namespace lion {

namespace {
constexpr u32 kMatrix = 0x00 / 4, kViewport = 0x40 / 4, kLight = 0x50 / 4;
constexpr u32 kSrc = 0x60 / 4, kDst = 0x64 / 4, kCount = 0x68 / 4, kSrcStride = 0x6C / 4,
              kDstStride = 0x70 / 4, kOp = 0x74 / 4, kStatus = 0x78 / 4;
}  // namespace

u32 Qve::read32(u32 offset, Master, bool*) {
  if (offset / 4 == kStatus) return busy_ ? 1 : 0;
  if (offset < 0x80) return regs_[offset / 4];
  return 0;
}

void Qve::write32(u32 offset, u32 value, Master, bool* stall) {
  if (offset >= 0x80) return;
  const u32 idx = offset / 4;
  if (idx == kOp) {
    if (busy_) {
      *stall = true;
      return;
    }
    start(value);
    return;
  }
  if (idx != kStatus) regs_[idx] = value;
}

void Qve::start(u32 op) {
  const u32 count = regs_[kCount];
  const u32 src_stride = regs_[kSrcStride] ? regs_[kSrcStride] : 16;
  const u32 dst_stride = regs_[kDstStride] ? regs_[kDstStride] : 16;
  float m[16], vp[4], light[4];
  for (int i = 0; i < 16; ++i) m[i] = bits_to_f32(regs_[kMatrix + i]);
  for (int i = 0; i < 4; ++i) vp[i] = bits_to_f32(regs_[kViewport + i]);
  for (int i = 0; i < 4; ++i) light[i] = bits_to_f32(regs_[kLight + i]);

  u32 src = regs_[kSrc], dst = regs_[kDst];
  for (u32 n = 0; n < count; ++n, src += src_stride, dst += dst_stride) {
    float v[4], o[4];
    for (int i = 0; i < 4; ++i) v[i] = bits_to_f32(bus_.read32(src + 4 * i, Master::Dma));
    switch (op) {
      case kXform:
      case kXformProject:
        for (int row = 0; row < 4; ++row)
          o[row] = m[row * 4 + 0] * v[0] + m[row * 4 + 1] * v[1] + m[row * 4 + 2] * v[2] + m[row * 4 + 3] * v[3];
        if (op == kXformProject) {
          const float inv_w = o[3] != 0.0f ? 1.0f / o[3] : 0.0f;
          o[0] = o[0] * inv_w * vp[0] + vp[2];
          o[1] = o[1] * inv_w * vp[1] + vp[3];
          o[2] = o[2] * inv_w;
          o[3] = inv_w;
        }
        break;
      case kLight: {
        float ndl = v[0] * light[0] + v[1] * light[1] + v[2] * light[2];
        o[0] = std::min(std::max(ndl, 0.0f) + light[3], 1.0f);
        o[1] = v[1];
        o[2] = v[2];
        o[3] = v[3];
        break;
      }
      default:
        LION_WARN("QVE: unknown op %u", op);
        return;
    }
    for (int i = 0; i < 4; ++i) bus_.write32(dst + 4 * i, f32_to_bits(o[i]), Master::Dma);
  }
  vectors_ += count;
  busy_ = u64(count) * kCyclesPerVector;
}

}  // namespace lion
