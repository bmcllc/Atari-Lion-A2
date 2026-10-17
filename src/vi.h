#pragma once

#include <vector>

#include "bus.h"
#include "pic.h"

namespace lion {

// Video interface: scans an RGB565 framebuffer out at 60 Hz.
class Vi : public MmioDevice {
 public:
  enum Mode : u32 { kOff = 0, k240p = 1, k480i = 2, k480p = 3 };

  Vi(Bus& bus, Pic& pic) : bus_(bus), pic_(pic) {}

  int lines_per_frame() const { return mode_ == k240p ? 262 : 525; }
  int visible_lines() const { return mode_ == k240p ? 240 : 480; }
  int width() const { return mode_ == k240p ? 320 : 640; }
  int height() const { return visible_lines(); }
  Mode mode() const { return Mode(mode_); }
  u32 frame() const { return frame_; }

  // Called by the scheduler at the start of each line.
  void set_line(int line);

  // Converts the current framebuffer to XRGB8888 (width() × height()).
  // Returns false when video is off.
  bool scanout(std::vector<u32>& out) const;

  u32 read32(u32 offset, Master who, bool* stall) override;
  void write32(u32 offset, u32 value, Master who, bool* stall) override;

 private:
  Bus& bus_;
  Pic& pic_;
  u32 mode_ = kOff;
  u32 fb_addr_ = 0;
  u32 stride_ = 0;
  u32 line_ = 0;
  u32 frame_ = 0;
};

}  // namespace lion
