#pragma once

#include <string>
#include <vector>

#include "bus.h"
#include "cdrom.h"
#include "loader.h"
#include "pad.h"
#include "pic.h"
#include "quacker/m88k.h"
#include "quacker/qctrl.h"
#include "quacker/qve.h"
#include "spike/spike.h"
#include "tuffy/ppc.h"
#include "tyke/tyke.h"
#include "vi.h"

namespace lion {

// The whole Atari Lion A2 board and its scheduler.
class Lion {
 public:
  // Boot MSR: machine check, FP and AltiVec enabled; vectors in low memory.
  static constexpr u32 kBootMsr = Tuffy::kMsrMe | Tuffy::kMsrFp | Tuffy::kMsrVec;
  static constexpr u32 kBootStack = map::kSdramBase + map::kSdramSize - 0x100;
  // Scheduling quantum in Tuffy cycles (1 µs): bounds FIFO latency between the chips.
  static constexpr u64 kSliceTuffy = 500;

  Lion();

  // Development boot: copies an image into memory and starts it directly
  // (as a dev kit would), uploading the Tyke firmware like the boot ROM does.
  void boot(const LionImage& image);
  // Inserts a disc (optional) and powers on: the boot ROM loads BOOT.LION.
  // Returns false (with a message) if the disc has no BOOT.LION; the ROM would
  // show its error screen in that case.
  bool boot_disc(std::vector<u8> iso, std::string* error);
  void power_on();
  bool in_boot_rom() const { return tuffy.pc >= map::kRomBase; }

  // Emulates one video frame and appends its audio to `audio`.
  void run_frame();

  std::vector<s16> audio;  // interleaved stereo, 44.1 kHz; the frontend drains it

  Bus bus;
  Pic pic;
  Vi vi;
  Pads pads;
  Tuffy tuffy;
  Quacker quacker;
  QuackerCtrl qctrl;
  Qve qve;
  Spike spike;
  Tyke tyke;
  Cdrom cd;

 private:
  void run_slice(u64 tuffy_cycles);

  // Fractional cycle accumulators so long runs keep the exact clock ratios.
  u64 quacker_acc_ = 0, spike_acc_ = 0, bus_acc_ = 0, tyke_acc_ = 0, line_acc_ = 0;
};

}  // namespace lion
