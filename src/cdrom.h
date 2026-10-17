#pragma once

#include <string>
#include <vector>

#include "bus.h"
#include "pic.h"

namespace lion {

// 12x CD-ROM drive with a DMA engine (see docs/hardware.md).
// Images are plain 2048-byte-sector ISO files.
class Cdrom : public MmioDevice {
 public:
  static constexpr u32 kSectorSize = 2048;
  static constexpr u64 kBytesPerSecond = 12 * 153'600;  // 12x, 150 KiB/s per speed step
  static constexpr double kSeekBaseSeconds = 0.080;
  static constexpr double kSeekFullSeconds = 0.150;  // extra time to cross a full 74-minute disc
  static constexpr u32 kFullDiscSectors = 333'000;

  enum Status : u32 { kPresent = 1u << 0, kBusy = 1u << 1, kError = 1u << 2, kDone = 1u << 3 };
  enum Command : u32 { kRead = 1, kStop = 2, kAck = 3 };

  Cdrom(Bus& bus, Pic& pic) : bus_(bus), pic_(pic) {}

  void insert(std::vector<u8> image);
  void eject();
  bool present() const { return !image_.empty(); }
  u32 capacity() const { return u32(image_.size() / kSectorSize); }

  // Advances the drive by `cycles` bus cycles (100 MHz).
  void tick(u64 cycles);

  // ISO9660 lookup ("DIR/FILE.EXT", case-insensitive, version suffix optional).
  bool find_file(const std::string& path, u32* lba, u32* size) const;
  bool read_file(const std::string& path, std::vector<u8>* out) const;

  u32 read32(u32 offset, Master who, bool* stall) override;
  void write32(u32 offset, u32 value, Master who, bool* stall) override;

 private:
  void start_read();
  void finish(u32 status_bit);
  const u8* sector(u32 lba) const { return &image_[size_t(lba) * kSectorSize]; }

  Bus& bus_;
  Pic& pic_;
  std::vector<u8> image_;
  u32 status_ = 0;
  u32 lba_ = 0, count_ = 0, dma_ = 0, done_ = 0;
  u32 head_ = 0;          // sector under the laser
  u64 wait_ = 0;          // bus cycles until the next event
  bool seeking_ = false;
  u64 clock_ = 0;  // bus cycles since power-on
};

}  // namespace lion
