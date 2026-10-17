#pragma once

#include <array>

#include "bus.h"
#include "cache.h"
#include "dispatch.h"

namespace lion {

// Atari Quacker: custom Motorola MC88110 interpreter.
// Integer, bit-field, branch (with .n delay slots), load/store, control
// registers, FP on the general registers and on the extended register file
// (x0-x31, 80-bit), and the SFU2 graphics unit. Encodings follow the GNU
// binutils m88k opcode table (88110 entries).
class Quacker {
 public:
  // Control register numbers.
  static constexpr int kPid = 0, kPsr = 1, kEpsr = 2, kExip = 4, kVbr = 7;
  static constexpr u32 kPsrSupervisor = 1u << 31;
  static constexpr u32 kPsrCarry = 1u << 28;
  static constexpr u32 kPsrSfu2Disable = 1u << 4, kPsrSfu1Disable = 1u << 3;
  // Exception vector numbers (MC88110 User's Manual, table 7-1).
  static constexpr u32 kVecInterrupt = 1, kVecInstructionAccess = 2, kVecDataAccess = 3, kVecMisaligned = 4,
                       kVecUnimplemented = 5, kVecPrivilege = 6, kVecBounds = 7, kVecDivZero = 8,
                       kVecOverflow = 9, kVecSfu1 = 114, kVecSfu2 = 116;
  // MC88110 identification in PID: arch 88110, Atari revision.
  static constexpr u32 kPidValue = 0x0000'0101;

  // Timing model (100 MHz core, same clock as the bus).
  static constexpr u32 kMemLatency = 10;  // cache line fill from SDRAM
  static constexpr u32 kMmioLatency = 2;  // FIFO / QVE / Spike registers
  static constexpr u32 kMulExtra = 2, kDivExtra = 37, kFdivSExtra = 12, kFdivDExtra = 22, kFsqrtExtra = 25;

  explicit Quacker(Bus& bus) : icache(8 << 10, 2, 32), dcache(8 << 10, 2, 32), bus_(bus) {}

  CacheModel icache, dcache;
  DualIssue dispatch;
  // Target instruction cache: taken branches that hit it fetch the target with no bubble.
  static constexpr u32 kTicEntries = 32, kTicMissPenalty = 1;
  u64 tic_hits = 0, tic_misses = 0;

  void reset(u32 pc);
  // Executes up to `cycles` cycles; returns the number consumed.
  u64 run(u64 cycles);

  bool running = false;
  std::array<u32, 32> r{};
  std::array<u32, 64> cr{};
  std::array<u32, 64> fcr{};
  std::array<long double, 32> x{};  // extended registers; x0 reads as +0.0
  u32 pc = 0;

  u64 total_cycles = 0;
  u64 instructions = 0;
  u64 stall_cycles = 0;
  bool trace = false;
  u32 illegal_count = 0;

 private:
  void execute(u32 op);
  IssueInfo classify(u32 op) const;
  static bool pairable(u8 a, u8 b);
  void taken_branch(bool delayed);
  std::array<u32, kTicEntries> tic_{};
  u32 prev_extra_ = 0;
  void op_triadic(u32 op);
  void op_bitfield_imm(u32 op);
  void op_control(u32 op);
  void op_fp(u32 op);
  void op_graphics(u32 op);
  void ld_x(u32 xd, u32 ea, u32 size);
  void st_x(u32 xs, u32 ea, u32 size);
  long double get_x(u32 reg, u32 type) const;
  void set_x(u32 reg, u32 type, long double v);
  u64 pair(u32 reg) const { return u64(r[reg]) << 32 | r[(reg + 1) & 31]; }
  void set_pair(u32 reg, u64 v) {
    r[reg] = u32(v >> 32);
    r[(reg + 1) & 31] = u32(v);
  }
  void trap(u32 vector);
  void illegal(u32 op);
  void branch(u32 target, bool delayed);

  bool carry() const { return cr[kPsr] & kPsrCarry; }
  void set_carry(bool c) { cr[kPsr] = c ? cr[kPsr] | kPsrCarry : cr[kPsr] & ~kPsrCarry; }

  double get_fp(u32 reg, u32 type) const;
  void set_fp(u32 reg, u32 type, double v);

  u8 rd8(u32 a) { account(a, false); return bus_.read8(a, Master::Quacker); }
  u16 rd16(u32 a) { account(a, false); return bus_.read16(a, Master::Quacker); }
  u32 rd32(u32 a) { account(a, false); return bus_.read32(a, Master::Quacker); }
  void wr8(u32 a, u8 v) { account(a, true); bus_.write8(a, v, Master::Quacker); }
  void wr16(u32 a, u16 v) { account(a, true); bus_.write16(a, v, Master::Quacker); }
  void wr32(u32 a, u32 v) { account(a, true); bus_.write32(a, v, Master::Quacker); }

  // Charges cache/MMIO latency for a data access. The local SRAM is single-cycle.
  void account(u32 a, bool write) {
    if ((a >> 16) == (map::kSramBase >> 16)) return;
    if ((a >> 24) == 0x1F) {
      penalty(kMmioLatency);
    } else if (!dcache.access(a)) {
      const u32 wait = bus_.acquire(Master::Quacker, clock_ + slice_done_ + extra_, Bus::kLineFillCycles);
      penalty((write ? kMemLatency / 2 : kMemLatency) + wait);
    }
  }
  void penalty(u32 n) {
    extra_ += n;
    stall_cycles += n;
  }
  u32 extra_ = 0;
  u64 debt_ = 0;
  u64 slice_done_ = 0;
  u64 clock_ = 0;  // cycles since power-on (advances even while held in reset)
  u32 last_fetch_line_ = ~0u;

  Bus& bus_;
  u32 next_pc_ = 0;
  bool delay_armed_ = false;
  u32 delay_target_ = 0;
};

}  // namespace lion
