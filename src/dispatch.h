#pragma once

#include <array>

#include "common.h"

namespace lion {

// Dual-issue model shared by Tuffy and Quacker: an instruction issues in the
// same cycle as the previous one when both fit in different (or duplicated)
// execution units and the second does not read or overwrite a register the
// first writes. Registers are tracked as a 128-bit set.
struct RegSet {
  u64 lo = 0, hi = 0;
  void add(u32 bit) {
    if (bit < 64) lo |= 1ull << bit;
    else hi |= 1ull << (bit - 64);
  }
  bool overlaps(const RegSet& o) const { return (lo & o.lo) || (hi & o.hi); }
};

struct IssueInfo {
  u8 unit = 0;             // execution unit class (CPU specific)
  bool serialize = false;  // must issue alone (system, sync, unknown)
  bool branch = false;     // handled by the branch unit (folded on the PowerPC)
  RegSet dst, src;
};

class DualIssue {
 public:
  // `pairable(a, b)` says whether units a and b can issue together.
  // Returns the dispatch cost of `cur` (0 when it pairs with the previous one).
  template <typename Pairable>
  u32 issue(const IssueInfo& cur, bool prev_stalled, Pairable pairable) {
    const bool pair = has_prev_ && !prev_paired_ && !prev_stalled && !cur.serialize && !prev_.serialize &&
                      pairable(prev_.unit, cur.unit) && !cur.src.overlaps(prev_.dst) &&
                      !cur.dst.overlaps(prev_.dst);
    prev_ = cur;
    has_prev_ = true;
    prev_paired_ = pair;
    if (pair) ++paired;
    return pair ? 0 : 1;
  }
  void flush() { has_prev_ = false; }
  u64 paired = 0;

 private:
  IssueInfo prev_;
  bool has_prev_ = false, prev_paired_ = false;
};

// 2-bit saturating branch history table.
template <size_t N>
class BranchPredictor {
 public:
  // Records the outcome and returns whether the prediction was right.
  bool predict(u32 pc, bool taken) {
    u8& c = table_[(pc >> 2) % N];
    const bool guess = c >= 2;
    if (taken && c < 3) ++c;
    if (!taken && c > 0) --c;
    if (guess == taken) ++hits;
    else ++misses;
    return guess == taken;
  }
  void reset() { table_.fill(1); }  // weakly not-taken
  u64 hits = 0, misses = 0;

 private:
  std::array<u8, N> table_ = [] {
    std::array<u8, N> t{};
    t.fill(1);
    return t;
  }();
};

}  // namespace lion
