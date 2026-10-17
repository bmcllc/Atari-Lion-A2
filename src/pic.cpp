#include "pic.h"

namespace lion {

u32 Pic::read32(u32 offset, Master, bool*) {
  switch (offset) {
    case 0x00: return pending_;
    case 0x04: return mask_;
  }
  return 0;
}

void Pic::write32(u32 offset, u32 value, Master, bool*) {
  switch (offset) {
    case 0x00: pending_ &= ~value; break;
    case 0x04: mask_ = value; break;
  }
}

}  // namespace lion
