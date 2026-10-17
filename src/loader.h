#pragma once

#include <string>
#include <vector>

#include "common.h"

namespace lion {

// `.lion` executable image (see docs/hardware.md).
struct LionImage {
  struct Segment {
    u32 addr;
    std::vector<u8> data;
  };
  u32 entry = 0;
  std::vector<Segment> segments;

  static constexpr u32 kMagic = 0x4C49'4F4E;  // "LION"
  static constexpr u32 kVersion = 1;

  bool parse(const std::vector<u8>& file, std::string* error);
  std::vector<u8> serialize() const;
};

bool read_file(const std::string& path, std::vector<u8>* out);

}  // namespace lion
