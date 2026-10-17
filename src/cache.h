#pragma once

#include <vector>

#include "common.h"

namespace lion {

// Timing-only set-associative cache with LRU replacement. Data always lives in
// the bus memory; this tracks which lines would be resident to charge latency.
class CacheModel {
 public:
  CacheModel(u32 size_bytes, u32 ways, u32 line_bytes)
      : ways_(ways), line_shift_(u32(std::countr_zero(line_bytes))), sets_(size_bytes / line_bytes / ways),
        tags_(size_t(sets_) * ways, kInvalid), stamps_(size_t(sets_) * ways, 0) {}

  // Looks up the line holding `pa`; on a miss it is allocated (if `allocate`).
  bool access(u32 pa, bool allocate = true) {
    const u32 line = pa >> line_shift_;
    const u32 set = line % sets_;
    u32* tags = &tags_[size_t(set) * ways_];
    u64* stamps = &stamps_[size_t(set) * ways_];
    ++clock_;
    for (u32 w = 0; w < ways_; ++w) {
      if (tags[w] == line) {
        stamps[w] = clock_;
        ++hits;
        return true;
      }
    }
    ++misses;
    if (allocate) {
      u32 victim = 0;
      for (u32 w = 1; w < ways_; ++w)
        if (tags[w] == kInvalid || (tags[victim] != kInvalid && stamps[w] < stamps[victim])) victim = w;
      tags[victim] = line;
      stamps[victim] = clock_;
    }
    return false;
  }

  bool contains(u32 pa) const {
    const u32 line = pa >> line_shift_;
    const u32* tags = &tags_[size_t(line % sets_) * ways_];
    for (u32 w = 0; w < ways_; ++w)
      if (tags[w] == line) return true;
    return false;
  }

  void invalidate(u32 pa) {
    const u32 line = pa >> line_shift_;
    u32* tags = &tags_[size_t(line % sets_) * ways_];
    for (u32 w = 0; w < ways_; ++w)
      if (tags[w] == line) tags[w] = kInvalid;
  }

  void invalidate_all() { std::fill(tags_.begin(), tags_.end(), kInvalid); }
  u32 line_bytes() const { return 1u << line_shift_; }

  u64 hits = 0, misses = 0;

 private:
  static constexpr u32 kInvalid = ~0u;
  u32 ways_, line_shift_, sets_;
  std::vector<u32> tags_;
  std::vector<u64> stamps_;
  u64 clock_ = 0;
};

}  // namespace lion
