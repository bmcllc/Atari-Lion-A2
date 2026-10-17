#pragma once

#include <array>

#include "bus.h"

namespace lion {

// Quacker Vector Engine: two FP32 MAC units driven by memory-mapped commands.
// Operations complete functionally at once; BUSY stays set for the modelled
// duration (8 Quacker cycles per vector) and a new OP while busy stalls.
class Qve : public MmioDevice {
 public:
  enum Op : u32 { kXform = 1, kXformProject = 2, kLight = 3 };
  static constexpr u32 kCyclesPerVector = 8;

  explicit Qve(Bus& bus) : bus_(bus) {}

  void tick(u64 quacker_cycles) { busy_ = busy_ > quacker_cycles ? busy_ - quacker_cycles : 0; }
  bool busy() const { return busy_ != 0; }
  u64 vectors_processed() const { return vectors_; }

  u32 read32(u32 offset, Master who, bool* stall) override;
  void write32(u32 offset, u32 value, Master who, bool* stall) override;

 private:
  void start(u32 op);

  Bus& bus_;
  std::array<u32, 0x80 / 4> regs_{};
  u64 busy_ = 0;
  u64 vectors_ = 0;
};

}  // namespace lion
