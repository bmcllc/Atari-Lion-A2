#pragma once

#include <array>
#include <vector>

#include "bus.h"
#include "pic.h"

namespace lion {

// Tyke: the Lion's 40 MHz audio DSP (ISA in docs/hardware.md).
//   0x0000-0x7FFF  data RAM (8K words), shared with the host; the default
//                  firmware reads its 24-channel mixer registers from here
//   0x8000-0x80FF  control
//   0xC000-0xFFFF  program RAM (4K instructions)
// The DSP executes its program; WAIT paces it to the 44.1 kHz output clock.
class Tyke : public MmioDevice {
 public:
  static constexpr int kChannels = 24;
  static constexpr u32 kDataWords = 8192, kProgramWords = 4096;
  static constexpr u32 kDataBase = 0x0000, kCtrlBase = 0x8000, kProgramBase = 0xC000;
  static constexpr u32 kHz = 40'000'000;
  static constexpr u32 kExternalLatency = 2;  // extra cycles for LDX/LDXH over the system bus

  Tyke(Bus& bus, Pic& pic) : bus_(bus), pic_(pic) {}

  // Uploads a program and starts it at word 0 (what the boot ROM does).
  void load_program(const std::vector<u32>& words);
  void reset();

  // Advances the DSP by `cycles` Tyke cycles.
  void run(u64 cycles);
  // Moves produced stereo samples to `out`.
  void drain(std::vector<s16>& out) {
    out.insert(out.end(), output_.begin(), output_.end());
    output_.clear();
  }

  bool running() const { return running_; }
  u32 pc() const { return pc_; }
  u32 data(u32 word) const { return dram_[word % kDataWords]; }
  u64 total_cycles = 0, idle_cycles = 0, instructions = 0;
  u32 illegal_count = 0;

  u32 read32(u32 offset, Master who, bool* stall) override;
  void write32(u32 offset, u32 value, Master who, bool* stall) override;

 private:
  void step(u32 op);
  // Bus wait for a single SDRAM beat with real-time priority, in Tyke cycles (40 MHz vs 100 MHz bus).
  u32 bus_wait() {
    const u64 t = now_ * kBusHz / kHz;
    return u32((u64(bus_.acquire_realtime(Master::Dma, t, Bus::kSingleCycles)) * kHz + kBusHz - 1) / kBusHz);
  }

  Bus& bus_;
  Pic& pic_;
  std::array<u32, kDataWords> dram_{};
  std::array<u32, kProgramWords> pram_{};
  std::array<u32, 16> r_{};
  std::array<s64, 2> acc_{};
  u32 pc_ = 0;
  bool running_ = false;
  bool waiting_ = false;
  u64 now_ = 0;         // Tyke cycles since power-on
  u64 target_ = 0;      // where now_ should be at the end of the current run()
  u64 ticks_done_ = 0;  // 44.1 kHz output periods consumed by WAIT
  u64 tick_cycle(u64 n) const { return n * kHz / kAudioRate; }
  u32 sample_count_ = 0;
  u32 extra_ = 0;
  s16 left_ = 0;
  std::vector<s16> output_;
};

}  // namespace lion
