#pragma once

#include <array>

#include "bus.h"

namespace lion {

// Four native controller ports, digital + analog.
class Pads : public MmioDevice {
 public:
  enum Button : u32 {
    kUp = 1u << 0,
    kDown = 1u << 1,
    kLeft = 1u << 2,
    kRight = 1u << 3,
    kA = 1u << 4,
    kB = 1u << 5,
    kC = 1u << 6,
    kX = 1u << 7,
    kY = 1u << 8,
    kZ = 1u << 9,
    kL = 1u << 10,
    kR = 1u << 11,
    kStart = 1u << 12,
    kSelect = 1u << 13,
  };

  struct State {
    u32 buttons = 0;
    u8 lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    bool connected = false;
  };

  std::array<State, 4> port;

  u32 read32(u32 offset, Master who, bool* stall) override;
  void write32(u32, u32, Master, bool*) override {}
};

}  // namespace lion
