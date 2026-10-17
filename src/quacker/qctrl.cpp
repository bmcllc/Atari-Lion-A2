#include "quacker/qctrl.h"

namespace lion {

u32 QuackerCtrl::read32(u32 offset, Master who, bool* stall) {
  switch (offset) {
    case 0x00: return cpu_.running ? 1 : 0;
    case 0x04: return start_pc_;
    case 0x08: {
      if (who != Master::Quacker) return 0;
      u32 v;
      if (!fifo_.pop(&v)) {
        *stall = true;  // Quacker waits for the next command
        return 0;
      }
      return v;
    }
    case 0x0C: return u32(fifo_.size());
    case 0x10: return mailbox_;
  }
  return 0;
}

void QuackerCtrl::write32(u32 offset, u32 value, Master who, bool* stall) {
  switch (offset) {
    case 0x00: {
      const bool run = value & 1;
      if (run && !cpu_.running) cpu_.reset(start_pc_);
      cpu_.running = run;
      break;
    }
    case 0x04: start_pc_ = value; break;
    case 0x08:
      if (!fifo_.push(value)) *stall = true;  // full: the writer waits
      break;
    case 0x10: mailbox_ = value; break;
    case 0x14: pic_.raise(Pic::kQuacker); break;
  }
}

}  // namespace lion
