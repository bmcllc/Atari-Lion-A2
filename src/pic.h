#pragma once

#include "bus.h"

namespace lion {

// Interrupt controller in front of the Tuffy external interrupt line.
class Pic : public MmioDevice {
 public:
  enum Source : u32 {
    kVblank = 1u << 0,
    kSpike = 1u << 1,
    kQuacker = 1u << 2,
    kTyke = 1u << 3,
    kPad = 1u << 4,
    kCd = 1u << 5,
  };

  void raise(u32 bits) { pending_ |= bits; }
  bool asserted() const { return (pending_ & mask_) != 0; }
  u32 pending() const { return pending_; }

  // Board straps, read-only at +0x08. Bit 0: development unit (the boot ROM skips its intro).
  enum Strap : u32 { kStrapFastBoot = 1u << 0 };
  u32 straps = 0;

  u32 read32(u32 offset, Master who, bool* stall) override;
  void write32(u32 offset, u32 value, Master who, bool* stall) override;

 private:
  u32 pending_ = 0;
  u32 mask_ = 0;
};

}  // namespace lion
