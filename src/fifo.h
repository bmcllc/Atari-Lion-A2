#pragma once

#include <vector>

#include "common.h"

namespace lion {

// Fixed-capacity word FIFO used between Tuffy, Quacker and Spike.
class WordFifo {
 public:
  explicit WordFifo(size_t capacity) : buf_(capacity) {}

  bool empty() const { return count_ == 0; }
  bool full() const { return count_ == buf_.size(); }
  size_t size() const { return count_; }
  size_t capacity() const { return buf_.size(); }

  bool push(u32 v) {
    if (full()) return false;
    buf_[(head_ + count_) % buf_.size()] = v;
    ++count_;
    return true;
  }
  bool pop(u32* v) {
    if (empty()) return false;
    *v = buf_[head_];
    head_ = (head_ + 1) % buf_.size();
    --count_;
    return true;
  }
  u32 peek(size_t i) const { return buf_[(head_ + i) % buf_.size()]; }
  void clear() { head_ = count_ = 0; }

 private:
  std::vector<u32> buf_;
  size_t head_ = 0, count_ = 0;
};

}  // namespace lion
