#pragma once

#include <algorithm>
#include <array>
#include <vector>

#include "common.h"

namespace lion {

// Physical memory map (see docs/hardware.md).
namespace map {
constexpr u32 kSdramBase = 0x0000'0000, kSdramSize = 24u << 20;
constexpr u32 kSgramBase = 0x1000'0000, kSgramSize = 8u << 20;
constexpr u32 kQuackerCtrl = 0x1F00'0000;
constexpr u32 kQve = 0x1F00'1000;
constexpr u32 kSpike = 0x1F01'0000;
constexpr u32 kTyke = 0x1F02'0000;
constexpr u32 kVi = 0x1F03'0000;
constexpr u32 kPic = 0x1F04'0000;
constexpr u32 kPad = 0x1F05'0000;
constexpr u32 kCd = 0x1F06'0000;
constexpr u32 kSramBase = 0x1F10'0000, kSramSize = 16u << 10;
// 8 MB boot ROM ending at the top of the address space. The PowerPC vectors
// (MSR[IP] = 1) sit at 0xFFF0_0000, in its last megabyte; below them, audio data.
constexpr u32 kRomBase = 0xFF80'0000, kRomSize = 8u << 20;
constexpr u32 kRomVectors = 0xFFF0'0000;
}  // namespace map

enum class Master { Tuffy, Quacker, Dma };

class MmioDevice {
 public:
  virtual ~MmioDevice() = default;
  // Set *stall to make the requesting CPU retry the access later (e.g. FIFO empty/full).
  virtual u32 read32(u32 offset, Master who, bool* stall) = 0;
  virtual void write32(u32 offset, u32 value, Master who, bool* stall) = 0;
};

class Bus {
 public:
  Bus();

  // Register a device for the 4 KB-aligned window [base, base + size).
  void map_device(u32 base, u32 size, MmioDevice* dev);

  u8 read8(u32 addr, Master who);
  u16 read16(u32 addr, Master who);
  u32 read32(u32 addr, Master who);
  u64 read64(u32 addr, Master who);
  void write8(u32 addr, u8 v, Master who);
  void write16(u32 addr, u16 v, Master who);
  void write32(u32 addr, u32 v, Master who);
  void write64(u32 addr, u64 v, Master who);

  // Host pointer for direct RAM access, or nullptr for MMIO / unmapped.
  // The span is guaranteed contiguous up to the end of the 64 KB page.
  u8* host_ptr(u32 addr) {
    u8* page = rpages_[addr >> 16];
    return page ? page + (addr & 0xFFFF) : nullptr;
  }
  // Pointer valid for `len` bytes, or nullptr.
  u8* host_range(u32 addr, u32 len);

  // Set by an MMIO access that asked the CPU to wait; the CPU clears it.
  bool stall = false;

  // ---- SDRAM arbitration (timing only) ----
  // The 64-bit/100 MHz SDRAM serves one transfer at a time. A master asks for
  // `duration` bus cycles at time `t` (bus cycles since power-on, from its own
  // clock) and gets back how long it had to wait for the bus.
  static constexpr u32 kLineFillCycles = 8;  // 32-byte line: access latency + 4 beats
  static constexpr u32 kSingleCycles = 3;    // single uncached beat
  u32 acquire(Master who, u64 t, u32 duration) {
    const u64 start = std::max(t, busy_until_);
    busy_until_ = start + duration;
    const u32 wait = u32(start - t);
    contention[size_t(who)] += wait;
    busy_cycles += duration;
    return wait;
  }
  // Real-time masters (Tyke) jump the queue: they wait at most for the transfer
  // in progress, and everyone queued behind is pushed back instead.
  u32 acquire_realtime(Master who, u64 t, u32 duration) {
    const u64 start = busy_until_ > t ? t + std::min<u64>(busy_until_ - t, kLineFillCycles) : t;
    busy_until_ = std::max(busy_until_, start) + duration;
    const u32 wait = u32(start - t);
    contention[size_t(who)] += wait;
    busy_cycles += duration;
    return wait;
  }
  u64 contention[3] = {};  // bus cycles each master spent waiting (Tuffy, Quacker, DMA)
  u64 busy_cycles = 0;

  std::vector<u8>& sdram() { return sdram_; }
  std::vector<u8>& sgram() { return sgram_; }
  std::vector<u8>& sram() { return sram_; }
  std::vector<u8>& rom() { return rom_; }

 private:
  struct Window {
    u32 base, size;
    MmioDevice* dev;
  };
  MmioDevice* find(u32 addr, u32* offset);
  u32 mmio_read(u32 addr, Master who);
  void mmio_write(u32 addr, u32 v, Master who);
  void map_ram(u32 base, std::vector<u8>& mem, bool writable);

  std::vector<u8> sdram_, sgram_, sram_, rom_;
  // 64 KB pages: host pointers for reads and for writes (ROM is read-only).
  std::array<u8*, 65536> rpages_{};
  std::array<u8*, 65536> wpages_{};
  std::vector<Window> windows_;
  u32 unmapped_warnings_ = 0;
  u64 busy_until_ = 0;
};

}  // namespace lion
