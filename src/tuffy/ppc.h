#pragma once

#include <array>

#include "bus.h"
#include "cache.h"
#include "dispatch.h"
#include "pic.h"

namespace lion {

// Atari Tuffy: custom PowerPC 7400 (G4) interpreter.
// MMU with BATs and hashed page tables (software TLB), AltiVec, and a
// cycle model with L1/L2 caches and per-instruction latencies.
class Tuffy {
 public:
  // MSR bits (32-bit numbering, value masks).
  static constexpr u32 kMsrVec = 1u << 25;
  static constexpr u32 kMsrPow = 1u << 18;
  static constexpr u32 kMsrEe = 1u << 15;
  static constexpr u32 kMsrPr = 1u << 14;
  static constexpr u32 kMsrFp = 1u << 13;
  static constexpr u32 kMsrMe = 1u << 12;
  static constexpr u32 kMsrIp = 1u << 6;
  static constexpr u32 kMsrIr = 1u << 5;
  static constexpr u32 kMsrDr = 1u << 4;
  static constexpr u32 kMsrRi = 1u << 1;

  // Exception vectors.
  static constexpr u32 kVecReset = 0x100, kVecDsi = 0x300, kVecIsi = 0x400, kVecExternal = 0x500,
                       kVecAlignment = 0x600, kVecProgram = 0x700, kVecFpUnavailable = 0x800,
                       kVecDecrementer = 0x900, kVecSyscall = 0xC00, kVecTrace = 0xD00,
                       kVecVecUnavailable = 0xF20;

  // PVR of the MPC7400 family; the minor revision marks the Atari part.
  static constexpr u32 kPvr = 0x000C'0A01;

  // Timing model (core cycles at 500 MHz).
  static constexpr u32 kL2Latency = 10;        // backside L2 at 250 MHz
  static constexpr u32 kMemLatency = 60;       // SDRAM line fill over the 100 MHz bus
  static constexpr u32 kUncachedLatency = 30;  // single-beat uncached / MMIO access
  static constexpr u32 kMispredictPenalty = 4; // refetch after a wrong branch guess
  // HID0 / L2CR enable bits.
  static constexpr u32 kHid0Ice = 1u << 15, kHid0Dce = 1u << 14, kL2crEnable = 1u << 31;
  static constexpr u32 kSprHid0 = 1008, kSprL2cr = 1017;

  Tuffy(Bus& bus, Pic& pic)
      : l1i(32 << 10, 8, 32), l1d(32 << 10, 8, 32), l2(1 << 20, 2, 32), bus_(bus), pic_(pic) {}

  CacheModel l1i, l1d, l2;
  BranchPredictor<512> bht;  // 512-entry branch history table
  DualIssue dispatch;

  void reset(u32 pc, u32 msr);

  // Executes up to `cycles` core cycles and returns how many were consumed.
  // Returns early (consuming the budget) while dozing or stalled on MMIO.
  u64 run(u64 cycles);

  bool dozing() const { return (msr & kMsrPow) != 0; }

  // Architectural state.
  std::array<u32, 32> gpr{};
  std::array<u64, 32> fpr{};  // raw IEEE double bits
  struct Vr {
    u32 w[4];
  };
  std::array<Vr, 32> vr{};
  u32 pc = 0, lr = 0, ctr = 0, xer = 0, cr = 0, msr = 0, fpscr = 0, vscr = 0x10000;
  u32 srr0 = 0, srr1 = 0, dar = 0, dsisr = 0;
  u32 dec = 0xFFFF'FFFF;
  u64 tb = 0;
  std::array<u32, 1024> spr{};  // everything not held in a named field (BATs, SDR1, HIDs...)
  std::array<u32, 16> sr{};     // segment registers

  u64 total_cycles = 0;
  u64 instructions = 0;
  u64 stall_cycles = 0;  // cycles spent on cache misses and long-latency ops
  bool trace = false;
  // Behave exactly like a commercial MPC7400: fsqrt/fsqrts are illegal.
  bool strict_7400 = false;
  // Count of illegal/unimplemented opcodes hit (for diagnostics).
  u32 illegal_count = 0;
  u32 first_illegal_pc = 0, first_illegal_lr = 0, first_illegal_sp = 0;  // debugging aid

 private:
  void execute(u32 op);
  void op19(u32 op);
  void op31(u32 op);
  void op59(u32 op);
  void op63(u32 op);
  void op4(u32 op);  // AltiVec
  void altivec_ldst(u32 op, u32 xo);

  void exception(u32 vector, u32 srr1_bits = 0, u32 return_pc = 0xFFFF'FFFF);
  void program(u32 reason) { exception(kVecProgram, reason, pc); }
  void illegal(u32 op);
  bool check_fp();
  bool check_vec();
  void tick_timebase(u64 cycles);

  u32 read_spr(u32 n);
  void write_spr(u32 n, u32 v);

  void set_cr0(u32 r);
  void set_cr_field(int field, u32 bits4) { cr = (cr & ~(0xF0000000u >> (field * 4))) | (bits4 << (28 - field * 4)); }
  bool cr_bit(int b) const { return (cr >> (31 - b)) & 1; }
  bool ca() const { return (xer >> 29) & 1; }
  void set_ca(bool c) { xer = c ? xer | (1u << 29) : xer & ~(1u << 29); }
  void set_ov(bool o) { xer = o ? xer | (3u << 30) : xer & ~(1u << 30); }

  // Effective-address memory accessors (translated when MSR[DR] is set).
  // On a fault they raise DSI and set fault_; on an MMIO wait the bus sets stall.
  u8 rd8(u32 a) { return u8(load(a, 1)); }
  u16 rd16(u32 a) { return u16(load(a, 2)); }
  u32 rd32(u32 a) { return u32(load(a, 4)); }
  u64 rd64(u32 a) { return load(a, 8); }
  void wr8(u32 a, u8 v) { store(a, v, 1); }
  void wr16(u32 a, u16 v) { store(a, v, 2); }
  void wr32(u32 a, u32 v) { store(a, v, 4); }
  void wr64(u32 a, u64 v) { store(a, v, 8); }
  u64 load(u32 ea, u32 size);
  void store(u32 ea, u64 v, u32 size);
  void zero_block(u32 ea);  // dcbz

 public:
  // MMU. Exposed for tests and debuggers.
  enum class Access { Read, Write, Exec };
  struct Translation {
    u32 pa;
    bool inhibited;  // WIMG.I: bypasses the caches
  };
  // Translates without side effects on CPU state other than PTE R/C bits.
  // Returns false and sets *dsisr_out (DSISR/SRR1 style bits) on a fault.
  bool translate(u32 ea, Access acc, Translation* out, u32* fault_bits);
  void flush_tlb();

 private:
  bool translate_bat(u32 ea, Access acc, Translation* out, u32* fault_bits, bool* hit);
  bool translate_page(u32 ea, Access acc, Translation* out, u32* fault_bits);
  bool fetch(u32 ea, u32* op);
  void data_fault(u32 ea, u32 bits, bool write);
  void update_mode(u32 old_msr);

  struct TlbEntry {
    u32 tag = ~0u;  // ea page | 1 when valid
    u32 pa_page = 0;
    bool inhibited = false;
  };
  static constexpr size_t kTlbSize = 1024;
  std::array<TlbEntry, kTlbSize> tlb_read_{}, tlb_write_{}, tlb_exec_{};
  bool fault_ = false;

  // Timing.
  void account_fetch(u32 pa, bool inhibited);
  void account_data(u32 pa, u32 size, bool write, bool inhibited);
  void account_zero(u32 pa, bool inhibited);
  u32 miss_penalty(u32 pa);
  u32 bus_wait(u32 duration) {
    return bus_.acquire(Master::Tuffy, (clock_ + slice_done_ + extra_) / (kTuffyHz / kBusHz), duration) *
           u32(kTuffyHz / kBusHz);
  }
  void penalty(u32 cycles) {
    extra_ += cycles;
    stall_cycles += cycles;
  }
  IssueInfo classify(u32 op) const;
  static bool pairable(u8 a, u8 b);
  void predict_branch(bool conditional, bool taken) {
    if (conditional && !bht.predict(pc, taken)) penalty(kMispredictPenalty);
    if (taken) dispatch.flush();
  }
  u32 prev_extra_ = 0;
  u32 extra_ = 0;      // extra cycles charged by the current instruction
  u64 slice_done_ = 0; // cycles into the current run() call (for bus arbitration)
  u64 clock_ = 0;      // core cycles since power-on (never reset)
  u64 debt_ = 0;       // cycles overrun past the previous budget
  u32 last_fetch_line_ = ~0u;

  u32 ea_d(u32 op) const;     // (rA|0) + d
  u32 ea_x(u32 op) const;     // (rA|0) + rB
  u32 ea_du(u32 op) const;    // rA + d
  u32 ea_xu(u32 op) const;    // rA + rB

  Bus& bus_;
  Pic& pic_;
  u32 next_pc_ = 0;
  bool dec_pending_ = false;
  bool reserve_ = false;
  u32 reserve_addr_ = 0;
  u64 tb_frac_ = 0;
};

}  // namespace lion
