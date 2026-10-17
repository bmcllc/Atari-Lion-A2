#pragma once

#include "bus.h"
#include "fifo.h"
#include "pic.h"
#include "quacker/m88k.h"

namespace lion {

// Quacker control block and the Tuffy -> Quacker Command FIFO.
class QuackerCtrl : public MmioDevice {
 public:
  static constexpr size_t kFifoWords = 1024;

  QuackerCtrl(Quacker& cpu, Pic& pic) : cpu_(cpu), pic_(pic), fifo_(kFifoWords) {}

  WordFifo& fifo() { return fifo_; }
  u32 mailbox() const { return mailbox_; }

  u32 read32(u32 offset, Master who, bool* stall) override;
  void write32(u32 offset, u32 value, Master who, bool* stall) override;

 private:
  Quacker& cpu_;
  Pic& pic_;
  WordFifo fifo_;
  u32 start_pc_ = 0;
  u32 mailbox_ = 0;
};

}  // namespace lion
