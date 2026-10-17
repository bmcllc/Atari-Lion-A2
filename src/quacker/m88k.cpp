#include "quacker/m88k.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace lion {

namespace {

inline u32 fD(u32 op) { return (op >> 21) & 31; }
inline u32 fS1(u32 op) { return (op >> 16) & 31; }
inline u32 fS2(u32 op) { return op & 31; }
inline u32 imm16(u32 op) { return op & 0xFFFF; }
inline s32 disp16(u32 op) { return s32(s16(op & 0xFFFF)) * 4; }
inline s32 disp26(u32 op) { return (s32(op << 6) >> 6) * 4; }

// Integer compare result bit string (MC88100 layout).
u32 cmp_bits(u32 a, u32 b) {
  const s32 sa = s32(a), sb = s32(b);
  u32 r = a == b ? 1u << 2 : 1u << 3;
  if (sa > sb) r |= 1u << 4;
  if (sa <= sb) r |= 1u << 5;
  if (sa < sb) r |= 1u << 6;
  if (sa >= sb) r |= 1u << 7;
  if (a > b) r |= 1u << 8;
  if (a <= b) r |= 1u << 9;
  if (a < b) r |= 1u << 10;
  if (a >= b) r |= 1u << 11;
  return r;
}

u32 fcmp_bits(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) return 1u << 0;  // not comparable
  u32 r = 1u << 1;                                     // comparable
  r |= a == b ? 1u << 2 : 1u << 3;
  if (a > b) r |= 1u << 4;
  if (a <= b) r |= 1u << 5;
  if (a < b) r |= 1u << 6;
  if (a >= b) r |= 1u << 7;
  return r;
}

// bcnd / tcnd condition: M5 bit 0 > 0, bit 1 == 0, bit 2 < 0 (not 0x80000000), bit 3 == 0x80000000.
bool cond_m5(u32 m5, u32 v) {
  if (v == 0) return m5 & 2;
  if (v == 0x8000'0000) return m5 & 8;
  return s32(v) > 0 ? (m5 & 1) : (m5 & 4);
}

inline u32 field_mask(u32 w) { return w == 0 || w >= 32 ? 0xFFFF'FFFF : (1u << w) - 1; }

// Bit-field operations shared by the immediate and register forms.
u32 bitfield(u32 sub, u32 s1, u32 w, u32 o) {
  const u32 m = field_mask(w);
  switch (sub) {
    case 0x20: return s1 & ~(m << o);  // clr
    case 0x22: return s1 | (m << o);   // set
    case 0x24: {                       // ext
      u32 width = w ? w : 32;
      s32 v = s32(s1) >> o;
      if (width >= 32) return u32(v);
      u32 sh = 32 - width;
      return u32(s32(u32(v) << sh) >> sh);
    }
    case 0x26: return (s1 >> o) & m;   // extu
    case 0x28: return (s1 & m) << o;   // mak
    case 0x2A: return std::rotr(s1, int(o));  // rot
  }
  return s1;
}

}  // namespace

#define GUARD() \
  if (bus_.stall) return

void Quacker::reset(u32 start_pc) {
  icache.invalidate_all();
  dcache.invalidate_all();
  extra_ = 0;
  debt_ = 0;
  stall_cycles = 0;
  last_fetch_line_ = ~0u;
  r.fill(0);
  x.fill(0.0L);
  cr.fill(0);
  fcr.fill(0);
  cr[kPid] = kPidValue;
  cr[kPsr] = kPsrSupervisor;
  pc = start_pc;
  delay_armed_ = false;
}

void Quacker::illegal(u32 op) {
  if (illegal_count < 16) LION_WARN("Quacker: illegal opcode %08X @%08X", op, pc);
  ++illegal_count;
  trap(kVecUnimplemented);
}

void Quacker::trap(u32 vector) {
  cr[kEpsr] = cr[kPsr];
  cr[kExip] = next_pc_;
  cr[kPsr] |= kPsrSupervisor;
  next_pc_ = cr[kVbr] + vector * 8;
  delay_armed_ = false;
}

void Quacker::taken_branch(bool delayed) {
  // Delayed (.n) branches hide the fetch bubble with their delay slot.
  u32& entry = tic_[(pc >> 2) % kTicEntries];
  if (entry == pc + 1) {
    ++tic_hits;
  } else {
    ++tic_misses;
    entry = pc + 1;
    if (!delayed) penalty(kTicMissPenalty);
  }
  dispatch.flush();
}

void Quacker::branch(u32 target, bool delayed) {
  taken_branch(delayed);
  if (delayed) {
    delay_armed_ = true;
    delay_target_ = target;
  } else {
    next_pc_ = target;
  }
}

u64 Quacker::run(u64 budget) {
  const u64 start = clock_;
  clock_ += budget;
  if (!running) return budget;
  if (debt_ >= budget) {
    debt_ -= budget;
    total_cycles += budget;
    return budget;
  }
  u64 done = debt_;
  debt_ = 0;
  clock_ = start;  // account() reads clock_ + slice_done_ during the loop
  while (done < budget) {
    const u8* p = bus_.host_ptr(pc);
    if (!p || (pc & 3)) {
      LION_WARN("Quacker: fetch from unmapped %08X, halting", pc);
      running = false;
      break;
    }
    extra_ = 0;
    slice_done_ = done;
    // Instruction cache: charged once per line; the local SRAM needs no cache.
    if (const u32 line = pc >> 5; line != last_fetch_line_) {
      last_fetch_line_ = line;
      if ((pc >> 16) != (map::kSramBase >> 16) && !icache.access(pc))
        penalty(kMemLatency + bus_.acquire(Master::Quacker, start + done, Bus::kLineFillCycles));
    }
    const u32 op = load_be32(p);
    if (trace) std::fprintf(stderr, "Q %08X: %08X r2=%08X r3=%08X\n", pc, op, r[2], r[3]);

    const bool in_delay = delay_armed_;
    const u32 target = delay_target_;
    delay_armed_ = false;
    next_pc_ = pc + 4;
    execute(op);
    r[0] = 0;
    if (bus_.stall) {
      bus_.stall = false;
      delay_armed_ = in_delay;
      delay_target_ = target;
      done = budget;
      break;
    }
    if (in_delay && !delay_armed_) next_pc_ = target;
    if (next_pc_ != pc + 4) last_fetch_line_ = ~0u;
    const u32 cost = dispatch.issue(classify(op), prev_extra_ != 0, pairable);
    prev_extra_ = extra_;
    pc = next_pc_;
    done += cost + extra_;
    ++instructions;
  }
  if (done > budget) debt_ = done - budget;
  total_cycles += budget;
  clock_ = start + budget;
  return budget;
}

void Quacker::execute(u32 op) {
  const u32 d = fD(op), s1 = fS1(op);
  const u32 a = r[s1];
  const u32 imm = imm16(op);
  switch (op >> 26) {
    // Loads and stores with unsigned 16-bit displacement.
    // MC88110: the 88100 xmem-immediate slots load the extended registers.
    case 0x00: ld_x(d, a + imm, 8); return;   // ld.d xD
    case 0x01: ld_x(d, a + imm, 4); return;   // ld xD
    case 0x0F: ld_x(d, a + imm, 16); return;  // ld.x xD
    case 0x0C: st_x(d, a + imm, 8); return;   // st.d xS
    case 0x0D: st_x(d, a + imm, 4); return;   // st xS
    case 0x0E: st_x(d, a + imm, 16); return;  // st.x xS
    case 0x02: { u32 v = rd16(a + imm); GUARD(); r[d] = v; return; }                       // ld.hu
    case 0x03: { u32 v = rd8(a + imm); GUARD(); r[d] = v; return; }                        // ld.bu
    case 0x04: {                                                                           // ld.d
      u32 hi = rd32(a + imm), lo = rd32(a + imm + 4);
      GUARD();
      r[d] = hi;
      r[(d + 1) & 31] = lo;
      return;
    }
    case 0x05: { u32 v = rd32(a + imm); GUARD(); r[d] = v; return; }                       // ld
    case 0x06: { u32 v = u32(s32(s16(rd16(a + imm)))); GUARD(); r[d] = v; return; }        // ld.h
    case 0x07: { u32 v = u32(s32(s8(rd8(a + imm)))); GUARD(); r[d] = v; return; }          // ld.b
    case 0x08: wr32(a + imm, r[d]); wr32(a + imm + 4, r[(d + 1) & 31]); return;           // st.d
    case 0x09: wr32(a + imm, r[d]); return;                                                // st
    case 0x0A: wr16(a + imm, u16(r[d])); return;                                           // st.h
    case 0x0B: wr8(a + imm, u8(r[d])); return;                                             // st.b

    // Logical immediates: and/and.u keep the other half of S1.
    case 0x10: r[d] = a & (0xFFFF'0000u | imm); return;  // and
    case 0x11: r[d] = a & (imm << 16 | 0xFFFF); return;  // and.u
    case 0x12: r[d] = a & imm; return;                   // mask
    case 0x13: r[d] = a & (imm << 16); return;           // mask.u
    case 0x14: r[d] = a ^ imm; return;                   // xor
    case 0x15: r[d] = a ^ (imm << 16); return;           // xor.u
    case 0x16: r[d] = a | imm; return;                   // or
    case 0x17: r[d] = a | (imm << 16); return;           // or.u

    // Arithmetic immediates (unsigned 16-bit).
    case 0x18: r[d] = a + imm; return;  // addu
    case 0x19: r[d] = a - imm; return;  // subu
    case 0x1A:                          // divu
      penalty(kDivExtra);
      if (imm == 0) { trap(kVecDivZero); return; }
      r[d] = a / imm;
      return;
    case 0x1B: r[d] = a * imm; penalty(kMulExtra); return;  // mul
    case 0x1C: {                        // add (signed overflow trap)
      s64 v = s64(s32(a)) + s64(imm);
      if (v != s32(v)) { trap(kVecOverflow); return; }
      r[d] = u32(v);
      return;
    }
    case 0x1D: {  // sub
      s64 v = s64(s32(a)) - s64(imm);
      if (v != s32(v)) { trap(kVecOverflow); return; }
      r[d] = u32(v);
      return;
    }
    case 0x1E:  // div (signed)
      penalty(kDivExtra);
      if (imm == 0) { trap(kVecDivZero); return; }
      r[d] = u32(s32(a) / s32(imm));
      return;
    case 0x1F: r[d] = cmp_bits(a, imm); return;  // cmp

    case 0x20: op_control(op); return;
    case 0x21: op_fp(op); return;
    case 0x22: op_graphics(op); return;

    // Branches. Displacements are word offsets from the branch itself.
    case 0x30: case 0x31: branch(pc + u32(disp26(op)), op >> 26 & 1); return;  // br, br.n
    case 0x32: case 0x33: {                                                    // bsr, bsr.n
      const bool n = op >> 26 & 1;
      r[1] = pc + (n ? 8 : 4);
      branch(pc + u32(disp26(op)), n);
      return;
    }
    case 0x34: case 0x35:  // bb0, bb0.n
      if (!((a >> d) & 1)) branch(pc + u32(disp16(op)), op >> 26 & 1);
      return;
    case 0x36: case 0x37:  // bb1, bb1.n
      if ((a >> d) & 1) branch(pc + u32(disp16(op)), op >> 26 & 1);
      return;
    case 0x3A: case 0x3B:  // bcnd, bcnd.n
      if (cond_m5(d, a)) branch(pc + u32(disp16(op)), op >> 26 & 1);
      return;

    case 0x3C: op_bitfield_imm(op); return;
    case 0x3D: op_triadic(op); return;
    case 0x3E:  // tbnd immediate
      if (a > imm) trap(kVecBounds);
      return;
  }
  illegal(op);
}

void Quacker::op_bitfield_imm(u32 op) {
  const u32 d = fD(op), a = r[fS1(op)];
  const u32 sub = (op >> 10) & 0x3F;
  const u32 w5 = (op >> 5) & 31, o5 = op & 31;
  // Register-indexed extended-register loads/stores share this primary opcode.
  if (sub >= 0x04 && sub <= 0x0A && sub != 0x07) {
    static constexpr u32 kXSize[3] = {8, 4, 16};
    const u32 size = kXSize[(sub & 3) % 3];
    const u32 ea = a + ((op & 0x200) ? r[op & 31] * size : r[op & 31]);
    if (sub < 0x08) ld_x(d, ea, size);
    else st_x(d, ea, size);
    return;
  }
  switch (sub) {
    case 0x20: case 0x22: case 0x24: case 0x26: case 0x28: case 0x2A:
      r[d] = bitfield(sub, a, w5, o5);
      return;
    case 0x34: case 0x36: case 0x3A: {  // tb0, tb1, tcnd
      const bool taken = sub == 0x34 ? !((a >> d) & 1) : sub == 0x36 ? ((a >> d) & 1) : cond_m5(d, a);
      if (!taken) return;
      // Vectors 0-127 are reserved for the hardware and the supervisor.
      const u32 vec = op & 0x1FF;
      trap(vec < 128 && !(cr[kPsr] & kPsrSupervisor) ? kVecPrivilege : vec);
      return;
    }
  }
  illegal(op);
}

void Quacker::op_triadic(u32 op) {
  const u32 d = fD(op), s1 = fS1(op), s2 = fS2(op);
  const u32 a = r[s1], b = r[s2];
  const u32 sub = (op >> 10) & 0x3F;

  // Register-indexed loads/stores: sub 0x00-0x0F, bit 9 scaled, bit 8 user space.
  if (sub < 0x10) {
    static constexpr u32 kSize[4] = {8, 4, 2, 1};
    const u32 size = sub == 0x00 ? 1 : sub == 0x01 ? 4 : kSize[sub & 3];
    const u32 ea = a + ((op & 0x200) ? b * size : b);
    switch (sub) {
      case 0x00: { u8 old = rd8(ea); GUARD(); wr8(ea, u8(r[d])); r[d] = old; return; }    // xmem.bu
      case 0x01: { u32 old = rd32(ea); GUARD(); wr32(ea, r[d]); r[d] = old; return; }     // xmem
      case 0x02: { u32 v = rd16(ea); GUARD(); r[d] = v; return; }                           // ld.hu
      case 0x03: { u32 v = rd8(ea); GUARD(); r[d] = v; return; }                            // ld.bu
      case 0x04: {                                                                          // ld.d
        u32 hi = rd32(ea), lo = rd32(ea + 4);
        GUARD();
        r[d] = hi;
        r[(d + 1) & 31] = lo;
        return;
      }
      case 0x05: { u32 v = rd32(ea); GUARD(); r[d] = v; return; }                           // ld
      case 0x06: { u32 v = u32(s32(s16(rd16(ea)))); GUARD(); r[d] = v; return; }            // ld.h
      case 0x07: { u32 v = u32(s32(s8(rd8(ea)))); GUARD(); r[d] = v; return; }              // ld.b
      case 0x08: wr32(ea, r[d]); wr32(ea + 4, r[(d + 1) & 31]); return;                    // st.d
      case 0x09: wr32(ea, r[d]); return;                                                    // st
      case 0x0A: wr16(ea, u16(r[d])); return;                                               // st.h
      case 0x0B: wr8(ea, u8(r[d])); return;                                                 // st.b
      case 0x0C: case 0x0D: case 0x0E: case 0x0F: r[d] = ea; return;                        // lda.*
    }
  }

  switch (sub) {
    case 0x10: r[d] = a & b; return;   // and
    case 0x11: r[d] = a & ~b; return;  // and.c
    case 0x14: r[d] = a ^ b; return;   // xor
    case 0x15: r[d] = a ^ ~b; return;  // xor.c
    case 0x16: r[d] = a | b; return;   // or
    case 0x17: r[d] = a | ~b; return;  // or.c

    case 0x18:    // addu[.ci/.co/.cio]
    case 0x19: {  // subu[...]: a + ~b + (ci ? C : 1)
      const bool ci = op & 0x200, co = op & 0x100;
      const bool is_sub = sub == 0x19;
      const u32 y = is_sub ? ~b : b;
      const u32 cin = ci ? u32(carry()) : (is_sub ? 1 : 0);
      const u64 wide = u64(a) + y + cin;
      r[d] = u32(wide);
      if (co) set_carry(wide >> 32);
      return;
    }
    case 0x1C:    // add (signed, traps on overflow)
    case 0x1D: {  // sub
      const bool ci = op & 0x200, co = op & 0x100;
      const bool is_sub = sub == 0x1D;
      const u32 y = is_sub ? ~b : b;
      const u32 cin = ci ? u32(carry()) : (is_sub ? 1 : 0);
      const u64 wide = u64(a) + y + cin;
      const u32 res = u32(wide);
      if (((a ^ res) & (y ^ res)) >> 31) { trap(kVecOverflow); return; }
      r[d] = res;
      if (co) set_carry(wide >> 32);
      return;
    }
    case 0x1A:  // divu, divu.d (64-bit dividend in the D pair)
      penalty(kDivExtra);
      if (b == 0) { trap(kVecDivZero); return; }
      if (op & 0x100) set_pair(d, pair(s1) / b);
      else r[d] = a / b;
      return;
    case 0x1B:  // mul(u), muls (bit 9), mulu.d (bit 8: 64-bit product in the D pair)
      penalty(kMulExtra);
      if (op & 0x100) set_pair(d, u64(a) * b);
      else if (op & 0x200) r[d] = u32(s32(a) * s32(b));
      else r[d] = a * b;
      return;
    case 0x1E:  // div
      penalty(kDivExtra);
      if (b == 0) { trap(kVecDivZero); return; }
      r[d] = (a == 0x8000'0000 && b == 0xFFFF'FFFF) ? a : u32(s32(a) / s32(b));
      return;
    case 0x1F: r[d] = cmp_bits(a, b); return;  // cmp

    case 0x20: case 0x22: case 0x24: case 0x26: case 0x28: case 0x2A:
      r[d] = bitfield(sub, a, (b >> 5) & 31, b & 31);
      return;

    case 0x30: case 0x31: branch(b & ~3u, sub & 1); return;  // jmp, jmp.n
    case 0x32: case 0x33: {                                  // jsr, jsr.n
      const bool n = sub & 1;
      const u32 target = b & ~3u;
      r[1] = pc + (n ? 8 : 4);
      branch(target, n);
      return;
    }
    case 0x3A: r[d] = b ? 31 - u32(std::countl_zero(b)) : 32; return;    // ff1
    case 0x3B: r[d] = ~b ? 31 - u32(std::countl_zero(~b)) : 32; return;  // ff0
    case 0x3E:                                                           // tbnd
      if (a > b) trap(kVecBounds);
      return;
    case 0x3F:  // rte
      if (!(cr[kPsr] & kPsrSupervisor)) { trap(kVecPrivilege); return; }
      cr[kPsr] = cr[kEpsr];
      next_pc_ = cr[kExip] & ~3u;
      return;
  }
  illegal(op);
}

void Quacker::op_control(u32 op) {
  const u32 d = fD(op), s1 = fS1(op);
  const u32 crn = (op >> 5) & 63;
  const u32 sub = (op >> 11) & 0x1F;
  if (!(cr[kPsr] & kPsrSupervisor) && !(sub & 1)) { trap(kVecPrivilege); return; }
  switch (sub) {
    case 0x08: r[d] = cr[crn]; return;  // ldcr
    case 0x10:                          // stcr
      if (crn != kPid) cr[crn] = r[s1];
      return;
    case 0x18: {  // xcr
      u32 old = cr[crn];
      if (crn != kPid) cr[crn] = r[s1];
      r[d] = old;
      return;
    }
    case 0x09: r[d] = fcr[crn]; return;      // fldcr
    case 0x11: fcr[crn] = r[s1]; return;     // fstcr
    case 0x19: {                             // fxcr
      u32 old = fcr[crn];
      fcr[crn] = r[s1];
      r[d] = old;
      return;
    }
  }
  illegal(op);
}

double Quacker::get_fp(u32 reg, u32 type) const {
  if (type == 0) return double(bits_to_f32(r[reg]));
  return bits_to_f64(u64(r[reg]) << 32 | r[(reg + 1) & 31]);
}

void Quacker::set_fp(u32 reg, u32 type, double v) {
  if (reg == 0) return;
  if (type == 0) {
    r[reg] = f32_to_bits(float(v));
  } else {
    u64 bits = f64_to_bits(v);
    r[reg] = u32(bits >> 32);
    r[(reg + 1) & 31] = u32(bits);
  }
}

long double Quacker::get_x(u32 reg, u32 type) const {
  const long double v = reg ? x[reg] : 0.0L;
  if (type == 0) return (long double)(float)v;
  if (type == 1) return (long double)(double)v;
  return v;
}

void Quacker::set_x(u32 reg, u32 type, long double v) {
  if (reg == 0) return;  // x0 is hardwired to +0.0
  if (type == 0) v = (long double)(float)v;
  else if (type == 1) v = (long double)(double)v;
  x[reg] = v;
}

// Extended registers in memory: 4 bytes (single), 8 (double) or 16 bytes
// (extended: sign/exponent in the top half of word 0, 64-bit mantissa in words 1-2).
void Quacker::ld_x(u32 xd, u32 ea, u32 size) {
  static_assert(LDBL_MANT_DIG == 64, "x87 80-bit long double required");
  long double v;
  if (size == 4) {
    u32 w = rd32(ea);
    GUARD();
    v = bits_to_f32(w);
  } else if (size == 8) {
    u64 w = u64(rd32(ea)) << 32 | rd32(ea + 4);
    GUARD();
    v = bits_to_f64(w);
  } else {
    u32 se = rd32(ea) >> 16;
    u64 mant = u64(rd32(ea + 4)) << 32 | rd32(ea + 8);
    GUARD();
    u8 raw[16] = {};
    std::memcpy(raw, &mant, 8);
    raw[8] = u8(se);
    raw[9] = u8(se >> 8);
    std::memcpy(&v, raw, sizeof(long double) < 16 ? sizeof(long double) : 16);
  }
  if (xd) x[xd] = v;
}

void Quacker::st_x(u32 xs, u32 ea, u32 size) {
  const long double v = xs ? x[xs] : 0.0L;
  if (size == 4) {
    wr32(ea, f32_to_bits(float(v)));
  } else if (size == 8) {
    u64 w = f64_to_bits(double(v));
    wr32(ea, u32(w >> 32));
    wr32(ea + 4, u32(w));
  } else {
    u8 raw[16] = {};
    std::memcpy(raw, &v, sizeof(long double) < 16 ? sizeof(long double) : 16);
    u64 mant;
    std::memcpy(&mant, raw, 8);
    const u32 se = u32(raw[8]) | u32(raw[9]) << 8;
    wr32(ea, se << 16);
    wr32(ea + 4, u32(mant >> 32));
    wr32(ea + 8, u32(mant));
    wr32(ea + 12, 0);
  }
}

// SFU1. Field layouts follow the binutils table: the general-register forms
// use T1[10:9] T2[8:7] TD[6:5]; the extended-register forms (bit 15) use
// TD[10:9] T1[8:7] T2[6:5]; compares keep T1/T2 at [10:9]/[8:7] and use bit 5
// for the unordered variant; fsqrt uses TD[8:7] T2[6:5].
void Quacker::op_fp(u32 op) {
  if (cr[kPsr] & kPsrSfu1Disable) {
    trap(kVecSfu1);
    return;
  }
  const u32 d = fD(op), s1 = fS1(op), s2 = fS2(op);
  const bool xf = op & 0x8000;
  const u32 base = (op >> 11) & 0xF;
  auto f = [&](u32 v, int sh) { return (v >> sh) & 3; };
  auto round_int = [&](long double v, int mode) -> u32 {
    long double rv = mode == 0 ? std::nearbyint(v) : mode == 1 ? std::trunc(v) : mode == 2 ? std::floor(v) : std::ceil(v);
    if (std::isnan(rv) || rv >= 2147483648.0L || rv < -2147483648.0L) return 0x8000'0000;
    return u32(s32(rv));
  };
  // Operand access: general registers (single or double pair) or x registers.
  auto src = [&](u32 reg, u32 type) -> long double {
    if (xf) return get_x(reg, type);
    return get_fp(reg, type);
  };
  auto dst = [&](u32 reg, u32 type, long double v) {
    if (xf) set_x(reg, type, v);
    else set_fp(reg, type, double(v));
  };

  switch (base) {
    case 0x0: case 0x5: case 0x6: case 0xE: {  // fmul fadd fsub fdiv
      const u32 td = xf ? f(op, 9) : f(op, 5), t1 = xf ? f(op, 7) : f(op, 9), t2 = xf ? f(op, 5) : f(op, 7);
      if (!xf && (td > 1 || t1 > 1 || t2 > 1)) break;
      if (td == 3 || t1 == 3 || t2 == 3) break;
      const long double a = src(s1, t1), b = src(s2, t2);
      const long double r = base == 0x0 ? a * b : base == 0x5 ? a + b : base == 0x6 ? a - b : a / b;
      if (base == 0xE) penalty(td == 0 ? kFdivSExtra : kFdivDExtra);
      dst(d, td, r);
      return;
    }
    case 0x7: {  // fcmp, fcmpu
      const u32 t1 = f(op, 9), t2 = f(op, 7);
      if ((!xf && (t1 > 1 || t2 > 1)) || t1 == 3 || t2 == 3) break;
      const long double a = src(s1, t1), b = src(s2, t2);
      r[d] = fcmp_bits(double(a), double(b));  // fcmpu differs only in exception signalling
      return;
    }
    case 0x4: {  // flt: bit 9 selects an x destination
      const u32 td = f(op, 5);
      if (td == 3 || (!(op & 0x200) && td > 1)) break;
      const long double v = (long double)s32(r[s2]);
      if (op & 0x200) set_x(d, td, v);
      else set_fp(d, td, double(v));
      return;
    }
    case 0x8: {  // mov between register files
      if (!xf) {  // mov.s/mov.d xD, rS2
        if (!(op & 0x200)) break;
        if (op & 0x80) set_x(d, 1, bits_to_f64(pair(s2)));
        else set_x(d, 0, bits_to_f32(r[s2]));
        return;
      }
      if (((op >> 8) & 3) == 3) {  // mov xD, xS2
        if (d) x[d] = s2 ? x[s2] : 0.0L;
        return;
      }
      if (op & 0x80) set_pair(d, f64_to_bits(double(get_x(s2, 1))));  // mov.d rD, xS2
      else r[d] = f32_to_bits(float(get_x(s2, 0)));                     // mov.s rD, xS2
      return;
    }
    case 0x9: case 0xA: case 0xB: {  // int, nint, trnc
      const u32 t2 = f(op, 7);
      if (t2 == 3 || (!xf && t2 > 1)) break;
      const long double v = src(s2, t2);
      const int mode = base == 0xA ? 0 : base == 0xB ? 1 : int((fcr[63] >> 14) & 3);
      r[d] = round_int(v, mode);
      return;
    }
    case 0xF: {  // fsqrt
      const u32 td = f(op, 7), t2 = f(op, 5);
      if (td == 3 || t2 == 3 || (!xf && (td > 1 || t2 > 1))) break;
      penalty(kFsqrtExtra);
      dst(d, td, std::sqrt(src(s2, t2)));
      return;
    }
  }
  illegal(op);
}

// SFU2 graphics unit, per the MC88110 User's Manual (section 5 and the
// padd/padds/pcmp/pmul/ppack/prot/psub/psubs/punpk pages of section 10).
// Pixel operands are 64-bit even/odd register pairs (rN:rN+1). Invalid
// encodings, odd pair registers or a disabled SFU2 raise the graphics
// exception (vector 116).
void Quacker::op_graphics(u32 op) {
  const u32 d = fD(op), s1 = fS1(op), s2 = fS2(op);
  const u32 sub = (op >> 11) & 0x1F;
  const u32 t = (op >> 5) & 3;
  auto fail = [&] { trap(kVecSfu2); };
  if (cr[kPsr] & kPsrSfu2Disable) return fail();
  auto even = [](u32 reg) { return (reg & 1) == 0; };

  switch (sub) {
    case 0x04:    // padd / padds
    case 0x06: {  // psub / psubs
      // T[6:5]: 01 8-bit, 10 16-bit, 11 32-bit fields. S[8:7]: 00 modulo,
      // 01 unsigned, 10 mixed (unsigned S1, signed S2, unsigned result), 11 signed.
      if (t == 0 || (op & 0x600) || !even(d) || !even(s1) || !even(s2)) return fail();
      const u32 fw = 4u << t;  // 8, 16, 32
      const u64 fmask = (1ull << fw) - 1;
      const u32 sat = (op >> 7) & 3;
      const bool subtract = sub == 0x06;
      auto sext = [&](u64 v) { return s64(v << (64 - fw)) >> (64 - fw); };
      const u64 a = pair(s1), b = pair(s2);
      u64 out = 0;
      for (u32 sh = 0; sh < 64; sh += fw) {
        const u64 x = (a >> sh) & fmask, y = (b >> sh) & fmask;
        u64 v;
        if (sat == 0) {
          v = subtract ? x - y : x + y;
        } else {
          const s64 xa = sat == 3 ? sext(x) : s64(x);
          const s64 yb = sat == 1 ? s64(y) : sext(y);
          const s64 r = subtract ? xa - yb : xa + yb;
          if (sat == 3) v = u64(std::clamp(r, -s64(fmask >> 1) - 1, s64(fmask >> 1)));
          else v = u64(std::clamp(r, s64(0), s64(fmask)));
        }
        out |= (v & fmask) << sh;
      }
      set_pair(d, out);
      return;
    }
    case 0x07: {  // pcmp: unsigned compare of the two 32-bit fields of each pair
      if ((op & 0x7E0) != 0x060 || !even(s1) || !even(s2)) return fail();
      const u64 a = pair(s1), b = pair(s2);
      const bool hi_ge = (a >> 32) >= (b >> 32), lo_ge = u32(a) >= u32(b);
      u32 v = 0;
      if (hi_ge && lo_ge) v |= 1u << 4;
      if (!hi_ge && !lo_ge) v |= 1u << 5;
      if (hi_ge && !lo_ge) v |= 1u << 6;
      if (!hi_ge && lo_ge) v |= 1u << 7;
      v |= (~v >> 4 & 0xF) << 8;  // bits 8-11 are the complements of bits 4-7
      r[d] = v;
      return;
    }
    case 0x00: {  // pmul: 64-bit pair x 32-bit scalar, low 64 bits, unsigned
      if ((op & 0x7E0) || !even(d) || !even(s1)) return fail();
      set_pair(d, pair(s1) * r[s2]);
      return;
    }
    case 0x0C: {  // ppack.r.t
      // R[10:7] = r/4 (r = 8, 16 or 32 result bits); T = source field size t.
      const u32 rbits = ((op >> 7) & 0xF) * 4;
      if (t == 0 || (rbits != 8 && rbits != 16 && rbits != 32) || !even(d) || !even(s1) || !even(s2)) return fail();
      const u32 tbits = 4u << t;
      const u32 keep = tbits * rbits / 64;  // most significant bits kept per field
      if (keep < 4) return fail();          // undefined combinations (t=8 r=8/16, t=16 r=8)
      const u64 src = pair(s2);
      u64 packed = 0;
      for (u32 sh = 64; sh >= tbits; sh -= tbits) {  // MSB field first
        const u64 field = (src >> (sh - tbits)) & ((tbits == 64 ? 0 : (1ull << tbits)) - 1);
        packed = (packed << keep) | (field >> (tbits - keep));
      }
      // The packed field replaces the top r bits of S1:S1+1, then the pair rotates left by r.
      set_pair(d, (pair(s1) << rbits) | packed);
      return;
    }
    case 0x0D: {  // punpk.t: fields of t bits into the low half of 2t-bit fields
      if ((op & 0x79F) || !even(d)) return fail();
      const u32 fw = 4u << t;  // 4, 8, 16, 32
      const u64 m = fw == 32 ? 0xFFFF'FFFFull : (1ull << fw) - 1;
      u64 out = 0;
      for (u32 i = 0; i < 32 / fw; ++i) out |= u64((r[s1] >> (i * fw)) & m) << (i * 2 * fw);
      set_pair(d, out);
      return;
    }
    case 0x0E:    // prot immediate: R[10:7] = count / 4
    case 0x0F: {  // prot register: count / 4 in bits 5-2 of rS2
      if (!even(d) || !even(s1)) return fail();
      if (sub == 0x0E ? (op & 0x7F) != 0 : (op & 0x7E0) != 0) return fail();
      const u32 n = sub == 0x0E ? ((op >> 7) & 0xF) * 4 : ((r[s2] >> 2) & 0xF) * 4;
      set_pair(d, std::rotl(pair(s1), int(n)));
      return;
    }
  }
  fail();
}

}  // namespace lion
