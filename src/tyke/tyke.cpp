#include "tyke/tyke.h"

#include <algorithm>

namespace lion {

namespace {
inline u32 fD(u32 op) { return (op >> 22) & 15; }
inline u32 fA(u32 op) { return (op >> 18) & 15; }
inline u32 fB(u32 op) { return (op >> 14) & 15; }
inline s32 imm14(u32 op) { return s32(op << 18) >> 18; }
inline s32 imm18(u32 op) { return s32(op << 14) >> 14; }
inline s32 imm26(u32 op) { return s32(op << 6) >> 6; }
}  // namespace

void Tyke::reset() {
  r_.fill(0);
  acc_.fill(0);
  pc_ = 0;
  waiting_ = false;
  ticks_done_ = now_ * kAudioRate / kHz;
  extra_ = 0;
}

void Tyke::load_program(const std::vector<u32>& words) {
  std::copy_n(words.begin(), std::min(words.size(), pram_.size()), pram_.begin());
  reset();
  running_ = true;
}

u32 Tyke::read32(u32 offset, Master, bool*) {
  if (offset < kCtrlBase) return dram_[(offset / 4) % kDataWords];
  if (offset >= kProgramBase) return pram_[((offset - kProgramBase) / 4) % kProgramWords];
  switch (offset - kCtrlBase) {
    case 0x00: return running_ ? 1 : 0;
    case 0x04: return pc_;
    case 0x08: return sample_count_;
  }
  return 0;
}

void Tyke::write32(u32 offset, u32 value, Master, bool*) {
  if (offset < kCtrlBase) {
    dram_[(offset / 4) % kDataWords] = value;
  } else if (offset >= kProgramBase) {
    pram_[((offset - kProgramBase) / 4) % kProgramWords] = value;
  } else if (offset == kCtrlBase) {
    const bool run = value & 1;
    if (run && !running_) reset();
    running_ = run;
  }
}

void Tyke::run(u64 cycles) {
  // The target is absolute: cycles spent stalled on the bus are paid back from
  // the next slices instead of pushing the DSP's clock ahead of the system.
  target_ += cycles;
  const u64 end = target_;
  total_cycles += cycles;
  if (!running_) {
    now_ = std::max(now_, end);
    return;
  }
  while (now_ < end) {
    if (waiting_) {
      // WAIT releases at the next 44.1 kHz output period. A DSP more than a few
      // periods late drops them rather than bursting.
      if (tick_cycle(ticks_done_ + 4) < now_) ticks_done_ = now_ * kAudioRate / kHz;
      const u64 next = tick_cycle(ticks_done_ + 1);
      if (next > now_) {
        const u64 until = std::min(next, end);
        idle_cycles += until - now_;
        now_ = until;
        if (now_ < next) return;
      }
      ++ticks_done_;
      waiting_ = false;
    }
    const u32 op = pram_[pc_ % kProgramWords];
    pc_ = (pc_ + 1) % kProgramWords;
    extra_ = 0;
    step(op);
    now_ += 1 + extra_;
    ++instructions;
    if (!running_) return;
  }
}

void Tyke::step(u32 op) {
  const u32 d = fD(op), a = fA(op), b = fB(op);
  const u32 ra = r_[a], rb = r_[b];
  const s32 imm = imm14(op);
  const u32 here = (pc_ + kProgramWords - 1) % kProgramWords;
  auto set = [&](u32 v) {
    if (d) r_[d] = v;
  };
  auto branch = [&](bool cond) {
    if (cond) pc_ = u32(s32(here) + imm) % kProgramWords;
  };
  switch (op >> 26) {
    case 0x00: break;                                              // NOP
    case 0x01: set(u32(imm18(op))); break;                         // MOVI
    case 0x02: set((r_[d] & 0xFFFF) | (op & 0xFFFF) << 16); break; // MOVHI
    case 0x03: set(ra + rb); break;                                // ADD
    case 0x04: set(ra - rb); break;                                // SUB
    case 0x05: set(ra & rb); break;                                // AND
    case 0x06: set(ra | rb); break;                                // OR
    case 0x07: set(ra ^ rb); break;                                // XOR
    case 0x08: set(rb & 32 ? 0 : ra << (rb & 31)); break;          // SHL
    case 0x09: set(rb & 32 ? 0 : ra >> (rb & 31)); break;          // SHR
    case 0x0A: set(u32(s32(ra) >> std::min(rb, 31u))); break;      // SAR
    case 0x0B: set(u32(s32(ra) * s32(rb))); break;                 // MUL
    case 0x0C: set(ra + u32(imm)); break;                          // ADDI
    case 0x0D: set(ra << (imm & 31)); break;                       // SHLI
    case 0x0E: set(ra >> (imm & 31)); break;                       // SHRI
    case 0x0F: set(u32(s32(ra) >> (imm & 31))); break;            // SARI
    case 0x10: set(ra & (u32(imm) & 0x3FFF)); break;               // ANDI
    case 0x11: set(dram_[(ra + u32(imm)) % kDataWords]); break;    // LD
    case 0x12: dram_[(ra + u32(imm)) % kDataWords] = r_[d]; break; // ST
    case 0x13:                                                     // LDXH
      set(u32(s32(s16(bus_.read16(ra + rb * 2, Master::Dma)))));
      extra_ += kExternalLatency + bus_wait();
      break;
    case 0x14:  // LDX
      set(bus_.read32(ra + u32(imm), Master::Dma));
      extra_ += kExternalLatency + bus_wait();
      break;
    case 0x15: acc_[d & 1] = 0; break;                                                     // CLR
    case 0x16: acc_[d & 1] += s64(s32(ra)) * s32(rb); break;                               // MAC
    case 0x17:                                                                              // MACS
      acc_[0] += s64(s32(ra)) * s16(rb >> 16);
      acc_[1] += s64(s32(ra)) * s16(rb);
      break;
    case 0x18: set(u32(s32(std::clamp(acc_[a & 1] >> (imm & 63), s64(INT32_MIN), s64(INT32_MAX))))); break;  // MVA
    case 0x19: set(u32(std::clamp(s32(ra), -32768, 32767))); break;                                           // SAT16
    case 0x1A: pc_ = u32(s32(here) + imm26(op)) % kProgramWords; break;                                       // BR
    case 0x1B:                                                                                                 // CALL
      r_[15] = pc_;
      pc_ = u32(s32(here) + imm26(op)) % kProgramWords;
      break;
    case 0x1C: pc_ = r_[15] % kProgramWords; break;  // RET
    case 0x1D: branch(ra == rb); break;              // BEQ
    case 0x1E: branch(ra != rb); break;              // BNE
    case 0x1F: branch(s32(ra) < s32(rb)); break;     // BLT
    case 0x20: branch(s32(ra) >= s32(rb)); break;    // BGE
    case 0x21: branch(ra < rb); break;               // BLTU
    case 0x22: branch(ra >= rb); break;              // BGEU
    case 0x23:                                       // OUT
      switch (imm) {
        case 0: left_ = s16(std::clamp(s32(ra), -32768, 32767)); break;
        case 1:
          output_.push_back(left_);
          output_.push_back(s16(std::clamp(s32(ra), -32768, 32767)));
          ++sample_count_;
          break;
        case 2: pic_.raise(Pic::kTyke); break;
      }
      break;
    case 0x24: set(imm == 0 ? sample_count_ : 0); break;  // IN
    case 0x25: waiting_ = true; break;                    // WAIT
    case 0x26: running_ = false; break;                   // HALT
    default:
      if (illegal_count++ < 8) LION_WARN("Tyke: illegal opcode %08X @%04X", op, here);
      running_ = false;
      break;
  }
}

}  // namespace lion
