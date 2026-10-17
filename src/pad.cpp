#include "pad.h"

namespace lion {

u32 Pads::read32(u32 offset, Master, bool*) {
  const u32 n = offset >> 4;
  if (n >= port.size()) return 0;
  const State& s = port[n];
  switch (offset & 0xF) {
    case 0x0: return s.buttons;
    case 0x4: return u32(s.lx) << 24 | u32(s.ly) << 16 | u32(s.rx) << 8 | s.ry;
    case 0x8: return s.connected ? 1 : 0;
  }
  return 0;
}

}  // namespace lion
