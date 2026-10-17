#include "vi.h"

#include <algorithm>

namespace lion {

void Vi::set_line(int line) {
  line_ = u32(line);
  if (line == visible_lines()) {
    ++frame_;
    pic_.raise(Pic::kVblank);
  }
}

bool Vi::scanout(std::vector<u32>& out) const {
  if (mode_ == kOff) return false;
  const int w = width(), h = height();
  out.resize(size_t(w) * h);
  const u32 stride = stride_ ? stride_ : u32(w * 2);
  for (int y = 0; y < h; ++y) {
    const u8* row = bus_.host_range(fb_addr_ + u32(y) * stride, u32(w * 2));
    u32* dst = &out[size_t(y) * w];
    if (!row) {
      std::fill(dst, dst + w, 0u);
      continue;
    }
    for (int x = 0; x < w; ++x) {
      u16 c = load_be16(row + x * 2);
      u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
      dst[x] = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
    }
  }
  return true;
}

u32 Vi::read32(u32 offset, Master, bool*) {
  switch (offset) {
    case 0x00: return mode_;
    case 0x04: return fb_addr_;
    case 0x08: return stride_;
    case 0x0C: return line_;
    case 0x10: return frame_;
  }
  return 0;
}

void Vi::write32(u32 offset, u32 value, Master, bool*) {
  switch (offset) {
    case 0x00: mode_ = value & 3; break;
    case 0x04: fb_addr_ = value; break;
    case 0x08: stride_ = value; break;
  }
}

}  // namespace lion
