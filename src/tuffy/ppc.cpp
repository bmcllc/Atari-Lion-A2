#include "tuffy/ppc.h"

namespace lion {

namespace {

inline u32 rD(u32 op) { return (op >> 21) & 31; }
inline u32 rA(u32 op) { return (op >> 16) & 31; }
inline u32 rB(u32 op) { return (op >> 11) & 31; }
inline s32 simm(u32 op) { return s16(op & 0xFFFF); }
inline u32 uimm(u32 op) { return op & 0xFFFF; }
inline bool rc(u32 op) { return op & 1; }
inline bool oe(u32 op) { return (op >> 10) & 1; }

// PowerPC rotate mask: bits mb..me set, bit 0 = MSB, wrapping when mb > me.
inline u32 rot_mask(u32 mb, u32 me) {
  u32 m1 = 0xFFFF'FFFFu >> mb;
  u32 m2 = 0xFFFF'FFFFu << (31 - me);
  return mb <= me ? (m1 & m2) : (m1 | m2);
}

struct AddResult {
  u32 r;
  bool ca, ov;
};
inline AddResult add3(u32 a, u32 b, u32 c) {
  u64 wide = u64(a) + b + c;
  u32 r = u32(wide);
  return {r, (wide >> 32) != 0, (((a ^ r) & (b ^ r)) >> 31) != 0};
}

// Program exception reasons (SRR1).
constexpr u32 kProgIllegal = 0x0008'0000;
constexpr u32 kProgPrivileged = 0x0004'0000;
constexpr u32 kProgTrap = 0x0002'0000;

bool trap_cond(u32 to, s32 a, s32 b) {
  return ((to & 0x10) && a < b) || ((to & 0x08) && a > b) || ((to & 0x04) && a == b) ||
         ((to & 0x02) && u32(a) < u32(b)) || ((to & 0x01) && u32(a) > u32(b));
}

}  // namespace

// Abandon an instruction whose memory access must be retried (MMIO wait) or faulted.
#define GUARD() \
  if (bus_.stall || fault_) return

void Tuffy::reset(u32 start_pc, u32 start_msr) {
  gpr.fill(0);
  fpr.fill(0);
  for (auto& v : vr) v = {};
  pc = start_pc;
  msr = start_msr;
  lr = ctr = xer = cr = fpscr = 0;
  vscr = 0x10000;
  srr0 = srr1 = dar = dsisr = 0;
  dec = 0xFFFF'FFFF;
  tb = 0;
  spr.fill(0);
  sr.fill(0);
  spr[kSprHid0] = kHid0Ice | kHid0Dce;
  spr[kSprL2cr] = kL2crEnable;
  l1i.invalidate_all();
  l1d.invalidate_all();
  l2.invalidate_all();
  flush_tlb();
  fault_ = false;
  extra_ = 0;
  debt_ = 0;
  stall_cycles = 0;
  last_fetch_line_ = ~0u;
  dec_pending_ = false;
  reserve_ = false;
  total_cycles = instructions = 0;
}

u32 Tuffy::ea_d(u32 op) const { return (rA(op) ? gpr[rA(op)] : 0) + u32(simm(op)); }
u32 Tuffy::ea_x(u32 op) const { return (rA(op) ? gpr[rA(op)] : 0) + gpr[rB(op)]; }
u32 Tuffy::ea_du(u32 op) const { return gpr[rA(op)] + u32(simm(op)); }
u32 Tuffy::ea_xu(u32 op) const { return gpr[rA(op)] + gpr[rB(op)]; }

void Tuffy::set_cr0(u32 r) {
  u32 bits = s32(r) < 0 ? 8 : (r ? 4 : 2);
  bits |= xer >> 31;
  cr = (cr & 0x0FFF'FFFF) | bits << 28;
}

void Tuffy::exception(u32 vector, u32 srr1_bits, u32 return_pc) {
  srr0 = return_pc == 0xFFFF'FFFF ? next_pc_ : return_pc;
  srr1 = (msr & 0x87C0'FFFF) | srr1_bits;
  // Interrupt entry: clear POW, EE, PR, FP, FE0/1, SE, BE, IR, DR, RI, VEC; keep ME and IP.
  const u32 old = msr;
  msr &= kMsrMe | kMsrIp;
  update_mode(old);
  last_fetch_line_ = ~0u;
  next_pc_ = ((msr & kMsrIp) ? 0xFFF0'0000u : 0u) + vector;
  reserve_ = false;
}

void Tuffy::illegal(u32 op) {
  if (illegal_count < 16) LION_WARN("Tuffy: illegal opcode %08X @%08X", op, pc);
  ++illegal_count;
  program(kProgIllegal);
}

bool Tuffy::check_fp() {
  if (msr & kMsrFp) return true;
  exception(kVecFpUnavailable, 0, pc);
  return false;
}

bool Tuffy::check_vec() {
  if (msr & kMsrVec) return true;
  exception(kVecVecUnavailable, 0, pc);
  return false;
}

void Tuffy::tick_timebase(u64 cycles) {
  // Time base and decrementer run at bus clock / 4.
  constexpr u64 kDiv = kTuffyHz / (kBusHz / 4);
  tb_frac_ += cycles;
  u64 ticks = tb_frac_ / kDiv;
  tb_frac_ %= kDiv;
  if (!ticks) return;
  tb += ticks;
  u32 old = dec;
  dec = u32(dec - ticks);
  // Exception when the MSB goes 0 -> 1 (or the counter wraps past zero).
  if ((!(old & 0x8000'0000) && (dec & 0x8000'0000)) || ticks > old) dec_pending_ = true;
}

u64 Tuffy::run(u64 budget) {
  // Cycles already spent past the previous budget (long instruction or cache miss).
  if (debt_ >= budget) {
    debt_ -= budget;
    tick_timebase(budget);
    total_cycles += budget;
    clock_ += budget;
    return budget;
  }
  u64 done = debt_;
  debt_ = 0;
  while (done < budget) {
    if (msr & kMsrEe) {
      if (pic_.asserted()) {
        next_pc_ = pc;
        exception(kVecExternal, 0, pc);
        pc = next_pc_;
      } else if (dec_pending_) {
        dec_pending_ = false;
        next_pc_ = pc;
        exception(kVecDecrementer, 0, pc);
        pc = next_pc_;
      }
    }
    if (msr & kMsrPow) {
      // Doze until an interrupt arrives; the time base keeps running.
      tick_timebase(budget - done);
      total_cycles += budget - done;
      clock_ += budget;
      return budget;
    }

    extra_ = 0;
    slice_done_ = done;
    u32 op;
    if (!fetch(pc, &op)) {
      pc = next_pc_;
      done += 1 + extra_;
      continue;
    }
    if (trace) std::fprintf(stderr, "T %08X: %08X r3=%08X r4=%08X\n", pc, op, gpr[3], gpr[4]);

    next_pc_ = pc + 4;
    execute(op);
    if (bus_.stall) {
      // MMIO asked us to wait: retry this instruction in the next slice.
      bus_.stall = false;
      fault_ = false;
      tick_timebase(budget - done);
      total_cycles += budget - done;
      clock_ += budget;
      return budget;
    }
    fault_ = false;  // a faulting access already redirected next_pc_ to its vector
    if (next_pc_ != pc + 4) last_fetch_line_ = ~0u;
    // Dispatch: predicted branches are folded; others may pair with the previous instruction.
    const IssueInfo info = classify(op);
    // Not-taken branches are folded away; a taken branch ends the fetch group (1 cycle).
    const u32 cost = info.branch ? (next_pc_ != pc + 4 ? 1 : 0) : dispatch.issue(info, prev_extra_ != 0, pairable);
    prev_extra_ = extra_;
    pc = next_pc_;
    done += cost + extra_;
    ++instructions;
  }
  if (done > budget) debt_ = done - budget;
  tick_timebase(budget);
  total_cycles += budget;
  clock_ += budget;
  return budget;
}

u32 Tuffy::read_spr(u32 n) {
  switch (n) {
    case 1: return xer;
    case 8: return lr;
    case 9: return ctr;
    case 18: return dsisr;
    case 19: return dar;
    case 22: return dec;
    case 26: return srr0;
    case 27: return srr1;
    case 268: return u32(tb);
    case 269: return u32(tb >> 32);
    case 287: return kPvr;
  }
  return spr[n & 1023];
}

void Tuffy::write_spr(u32 n, u32 v) {
  switch (n) {
    case 1: xer = v & 0xE000'007F; return;
    case 8: lr = v; return;
    case 9: ctr = v; return;
    case 18: dsisr = v; return;
    case 19: dar = v; return;
    case 22: dec = v; return;
    case 26: srr0 = v; return;
    case 27: srr1 = v; return;
    case 25:  // SDR1
      spr[n] = v;
      flush_tlb();
      return;
    case kSprHid0:
      // Flash invalidate (ICFI/DCFI) and cache enables.
      if (v & 0x800) l1i.invalidate_all();
      if (v & 0x400) l1d.invalidate_all();
      spr[n] = v & ~0xC00u;
      return;
    case kSprL2cr:
      if (v & 0x0020'0000) l2.invalidate_all();  // L2I: global invalidate
      spr[n] = v & ~0x0020'0000u;
      return;
    case 284: tb = (tb & 0xFFFF'FFFF'0000'0000ull) | v; return;
    case 285: tb = (tb & 0xFFFF'FFFFull) | u64(v) << 32; return;
    case 287: return;  // PVR is read-only
  }
  spr[n & 1023] = v;
  if (n >= 528 && n <= 543) flush_tlb();  // BATs
}

void Tuffy::execute(u32 op) {
  const u32 d = rD(op), a = rA(op), b = rB(op);
  switch (op >> 26) {
    case 3:  // twi
      if (trap_cond(d, s32(gpr[a]), simm(op))) program(kProgTrap);
      return;
    case 4: op4(op); return;
    case 7: gpr[d] = u32(s32(gpr[a]) * simm(op)); penalty(2); return;  // mulli
    case 8: {  // subfic
      AddResult r = add3(~gpr[a], u32(simm(op)), 1);
      gpr[d] = r.r;
      set_ca(r.ca);
      return;
    }
    case 10: {  // cmpli
      u32 x = gpr[a], y = uimm(op);
      set_cr_field(int(d >> 2), (x < y ? 8 : x > y ? 4 : 2) | xer >> 31);
      return;
    }
    case 11: {  // cmpi
      s32 x = s32(gpr[a]), y = simm(op);
      set_cr_field(int(d >> 2), (x < y ? 8 : x > y ? 4 : 2) | xer >> 31);
      return;
    }
    case 12:
    case 13: {  // addic, addic.
      AddResult r = add3(gpr[a], u32(simm(op)), 0);
      gpr[d] = r.r;
      set_ca(r.ca);
      if (op >> 26 == 13) set_cr0(r.r);
      return;
    }
    case 14: gpr[d] = (a ? gpr[a] : 0) + u32(simm(op)); return;          // addi
    case 15: gpr[d] = (a ? gpr[a] : 0) + (uimm(op) << 16); return;       // addis
    case 16: {                                                           // bc
      const u32 bo = d, bi = a;
      if (!(bo & 4)) --ctr;
      bool ctr_ok = (bo & 4) || ((ctr != 0) != bool(bo & 2));
      bool cond_ok = (bo & 16) || (cr_bit(int(bi)) == bool(bo & 8));
      predict_branch(!(bo & 4) || !(bo & 16), ctr_ok && cond_ok);
      if (ctr_ok && cond_ok) {
        u32 disp = u32(s32(s16(op & 0xFFFC)));
        next_pc_ = (op & 2) ? disp : pc + disp;
      }
      if (op & 1) lr = pc + 4;
      return;
    }
    case 17:  // sc
      exception(kVecSyscall);
      return;
    case 18: {  // b
      predict_branch(false, true);
      u32 disp = u32(s32(op << 6) >> 6) & ~3u;
      next_pc_ = (op & 2) ? disp : pc + disp;
      if (op & 1) lr = pc + 4;
      return;
    }
    case 19: op19(op); return;
    case 20: {  // rlwimi
      u32 m = rot_mask((op >> 6) & 31, (op >> 1) & 31);
      u32 r = (rotl32(gpr[d], b) & m) | (gpr[a] & ~m);
      gpr[a] = r;
      if (rc(op)) set_cr0(r);
      return;
    }
    case 21: {  // rlwinm
      u32 r = rotl32(gpr[d], b) & rot_mask((op >> 6) & 31, (op >> 1) & 31);
      gpr[a] = r;
      if (rc(op)) set_cr0(r);
      return;
    }
    case 23: {  // rlwnm
      u32 r = rotl32(gpr[d], gpr[b] & 31) & rot_mask((op >> 6) & 31, (op >> 1) & 31);
      gpr[a] = r;
      if (rc(op)) set_cr0(r);
      return;
    }
    case 24: gpr[a] = gpr[d] | uimm(op); return;                    // ori
    case 25: gpr[a] = gpr[d] | (uimm(op) << 16); return;            // oris
    case 26: gpr[a] = gpr[d] ^ uimm(op); return;                    // xori
    case 27: gpr[a] = gpr[d] ^ (uimm(op) << 16); return;            // xoris
    case 28: gpr[a] = gpr[d] & uimm(op); set_cr0(gpr[a]); return;   // andi.
    case 29: gpr[a] = gpr[d] & (uimm(op) << 16); set_cr0(gpr[a]); return;  // andis.
    case 31: op31(op); return;

    // Integer loads and stores (D-form).
    case 32: { u32 v = rd32(ea_d(op)); GUARD(); gpr[d] = v; return; }                         // lwz
    case 33: { u32 ea = ea_du(op); u32 v = rd32(ea); GUARD(); gpr[d] = v; gpr[a] = ea; return; }  // lwzu
    case 34: { u32 v = rd8(ea_d(op)); GUARD(); gpr[d] = v; return; }                          // lbz
    case 35: { u32 ea = ea_du(op); u32 v = rd8(ea); GUARD(); gpr[d] = v; gpr[a] = ea; return; }   // lbzu
    case 36: wr32(ea_d(op), gpr[d]); return;                                                   // stw
    case 37: { u32 ea = ea_du(op); wr32(ea, gpr[d]); GUARD(); gpr[a] = ea; return; }            // stwu
    case 38: wr8(ea_d(op), u8(gpr[d])); return;                                                // stb
    case 39: { u32 ea = ea_du(op); wr8(ea, u8(gpr[d])); GUARD(); gpr[a] = ea; return; }         // stbu
    case 40: { u32 v = rd16(ea_d(op)); GUARD(); gpr[d] = v; return; }                         // lhz
    case 41: { u32 ea = ea_du(op); u32 v = rd16(ea); GUARD(); gpr[d] = v; gpr[a] = ea; return; }  // lhzu
    case 42: { u32 v = u32(s32(s16(rd16(ea_d(op))))); GUARD(); gpr[d] = v; return; }          // lha
    case 43: { u32 ea = ea_du(op); u32 v = u32(s32(s16(rd16(ea)))); GUARD(); gpr[d] = v; gpr[a] = ea; return; }  // lhau
    case 44: wr16(ea_d(op), u16(gpr[d])); return;                                              // sth
    case 45: { u32 ea = ea_du(op); wr16(ea, u16(gpr[d])); GUARD(); gpr[a] = ea; return; }       // sthu
    case 46: {  // lmw
      u32 ea = ea_d(op);
      for (u32 r = d; r < 32; ++r, ea += 4) gpr[r] = rd32(ea);
      return;
    }
    case 47: {  // stmw
      u32 ea = ea_d(op);
      for (u32 r = d; r < 32; ++r, ea += 4) wr32(ea, gpr[r]);
      return;
    }

    // Floating-point loads and stores.
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55: {
      if (!check_fp()) return;
      const u32 opc = op >> 26;
      const bool update = opc & 1;
      if (update && a == 0) { illegal(op); return; }
      u32 ea = update ? ea_du(op) : ea_d(op);
      switch (opc & ~1u) {
        case 48: { u32 v = rd32(ea); GUARD(); fpr[d] = f64_to_bits(double(bits_to_f32(v))); break; }  // lfs
        case 50: { u64 v = rd64(ea); GUARD(); fpr[d] = v; break; }                                       // lfd
        case 52: wr32(ea, f32_to_bits(float(bits_to_f64(fpr[d])))); GUARD(); break;                     // stfs
        case 54: wr64(ea, fpr[d]); GUARD(); break;                                                       // stfd
      }
      if (update) gpr[a] = ea;
      return;
    }
    case 59: if (check_fp()) op59(op); return;
    case 63: if (check_fp()) op63(op); return;
  }
  illegal(op);
}

void Tuffy::op19(u32 op) {
  const u32 d = rD(op), a = rA(op), b = rB(op);
  const u32 xo = (op >> 1) & 0x3FF;
  auto crop = [&](bool r) {
    u32 bit = 0x8000'0000u >> d;
    cr = r ? cr | bit : cr & ~bit;
  };
  switch (xo) {
    case 0: {  // mcrf
      u32 f = (cr >> (28 - (a >> 2) * 4)) & 0xF;
      set_cr_field(int(d >> 2), f);
      return;
    }
    case 16:    // bclr
    case 528: {  // bcctr
      const u32 bo = d, bi = a;
      bool ctr_ok = true;
      if (xo == 16 && !(bo & 4)) {
        --ctr;
        ctr_ok = (ctr != 0) != bool(bo & 2);
      }
      bool cond_ok = (bo & 16) || (cr_bit(int(bi)) == bool(bo & 8));
      u32 target = (xo == 16 ? lr : ctr) & ~3u;
      predict_branch(!(bo & 4) || !(bo & 16), ctr_ok && cond_ok);
      if (op & 1) lr = pc + 4;
      if (ctr_ok && cond_ok) next_pc_ = target;
      return;
    }
    case 33: crop(!(cr_bit(int(a)) || cr_bit(int(b)))); return;   // crnor
    case 129: crop(cr_bit(int(a)) && !cr_bit(int(b))); return;    // crandc
    case 193: crop(cr_bit(int(a)) != cr_bit(int(b))); return;     // crxor
    case 225: crop(!(cr_bit(int(a)) && cr_bit(int(b)))); return;  // crnand
    case 257: crop(cr_bit(int(a)) && cr_bit(int(b))); return;     // crand
    case 289: crop(cr_bit(int(a)) == cr_bit(int(b))); return;     // creqv
    case 417: crop(cr_bit(int(a)) || !cr_bit(int(b))); return;    // crorc
    case 449: crop(cr_bit(int(a)) || cr_bit(int(b))); return;     // cror
    case 150: return;                                              // isync
    case 50:                                                       // rfi
      if (msr & kMsrPr) { program(kProgPrivileged); return; }
    {
      const u32 old = msr;
      msr = (msr & ~0x87C0'FF73u) | (srr1 & 0x87C0'FF73u);
      update_mode(old);
    }
      next_pc_ = srr0 & ~3u;
      reserve_ = false;
      return;
  }
  illegal(op);
}

void Tuffy::op31(u32 op) {
  const u32 d = rD(op), a = rA(op), b = rB(op);
  const u32 xo = (op >> 1) & 0x3FF;

  // XO-form arithmetic (9-bit opcode, OE in bit 21).
  switch (xo & 0x1FF) {
    case 266: case 10: case 138: case 202: case 234:     // add, addc, adde, addze, addme
    case 40: case 8: case 136: case 200: case 232: {     // subf, subfc, subfe, subfze, subfme
      const u32 k = xo & 0x1FF;
      const bool sub = k == 40 || k == 8 || k == 136 || k == 200 || k == 232;
      u32 x = sub ? ~gpr[a] : gpr[a];
      u32 y, cin;
      switch (k) {
        case 266: case 10: y = gpr[b]; cin = 0; break;
        case 40: case 8: y = gpr[b]; cin = 1; break;
        case 138: case 136: y = gpr[b]; cin = ca(); break;
        case 202: case 200: y = 0; cin = ca(); break;
        default: y = 0xFFFF'FFFF; cin = ca(); break;  // addme, subfme
      }
      AddResult r = add3(x, y, cin);
      gpr[d] = r.r;
      if (k != 266 && k != 40) set_ca(r.ca);
      if (oe(op)) set_ov(r.ov);
      if (rc(op)) set_cr0(r.r);
      return;
    }
    case 104: {  // neg
      u32 r = ~gpr[a] + 1;
      gpr[d] = r;
      if (oe(op)) set_ov(gpr[a] == 0x8000'0000);
      if (rc(op)) set_cr0(r);
      return;
    }
    case 235: {  // mullw
      s64 p = s64(s32(gpr[a])) * s32(gpr[b]);
      gpr[d] = u32(p);
      penalty(3);
      if (oe(op)) set_ov(p != s64(s32(p)));
      if (rc(op)) set_cr0(gpr[d]);
      return;
    }
    case 75: gpr[d] = u32((s64(s32(gpr[a])) * s32(gpr[b])) >> 32); penalty(3); if (rc(op)) set_cr0(gpr[d]); return;  // mulhw
    case 11: gpr[d] = u32((u64(gpr[a]) * gpr[b]) >> 32); penalty(3); if (rc(op)) set_cr0(gpr[d]); return;            // mulhwu
    case 491: {  // divw
      s32 x = s32(gpr[a]), y = s32(gpr[b]);
      bool bad = y == 0 || (x == INT32_MIN && y == -1);
      penalty(18);
      gpr[d] = bad ? (x < 0 && y == 0 ? 0xFFFF'FFFF : 0) : u32(x / y);
      if (oe(op)) set_ov(bad);
      if (rc(op)) set_cr0(gpr[d]);
      return;
    }
    case 459: {  // divwu
      bool bad = gpr[b] == 0;
      penalty(18);
      gpr[d] = bad ? 0 : gpr[a] / gpr[b];
      if (oe(op)) set_ov(bad);
      if (rc(op)) set_cr0(gpr[d]);
      return;
    }
  }

  auto logical = [&](u32 r) {
    gpr[a] = r;
    if (rc(op)) set_cr0(r);
  };

  switch (xo) {
    case 0: {  // cmp
      s32 x = s32(gpr[a]), y = s32(gpr[b]);
      set_cr_field(int(d >> 2), (x < y ? 8 : x > y ? 4 : 2) | xer >> 31);
      return;
    }
    case 32: {  // cmpl
      u32 x = gpr[a], y = gpr[b];
      set_cr_field(int(d >> 2), (x < y ? 8 : x > y ? 4 : 2) | xer >> 31);
      return;
    }
    case 4:  // tw
      if (trap_cond(d, s32(gpr[a]), s32(gpr[b]))) program(kProgTrap);
      return;
    case 28: logical(gpr[d] & gpr[b]); return;     // and
    case 60: logical(gpr[d] & ~gpr[b]); return;    // andc
    case 124: logical(~(gpr[d] | gpr[b])); return; // nor
    case 284: logical(~(gpr[d] ^ gpr[b])); return; // eqv
    case 316: logical(gpr[d] ^ gpr[b]); return;    // xor
    case 412: logical(gpr[d] | ~gpr[b]); return;   // orc
    case 444: logical(gpr[d] | gpr[b]); return;    // or
    case 476: logical(~(gpr[d] & gpr[b])); return; // nand
    case 26: logical(u32(std::countl_zero(gpr[d]))); return;  // cntlzw
    case 922: logical(u32(s32(s16(gpr[d])))); return;          // extsh
    case 954: logical(u32(s32(s8(gpr[d])))); return;           // extsb
    case 24: {  // slw
      u32 n = gpr[b] & 63;
      logical(n > 31 ? 0 : gpr[d] << n);
      return;
    }
    case 536: {  // srw
      u32 n = gpr[b] & 63;
      logical(n > 31 ? 0 : gpr[d] >> n);
      return;
    }
    case 792: {  // sraw
      u32 n = gpr[b] & 63;
      s32 v = s32(gpr[d]);
      s32 r = n > 31 ? (v >> 31) : (v >> n);
      bool lost = n > 31 ? v != 0 : (n && (u32(v) << (32 - n)) != 0);
      set_ca(v < 0 && lost);
      logical(u32(r));
      return;
    }
    case 824: {  // srawi
      u32 n = b;
      s32 v = s32(gpr[d]);
      set_ca(v < 0 && n && (u32(v) << (32 - n)) != 0);
      logical(u32(v >> n));
      return;
    }

    // Condition register and special registers.
    case 19: gpr[d] = cr; return;  // mfcr
    case 144: {                    // mtcrf
      u32 crm = (op >> 12) & 0xFF, mask = 0;
      for (int i = 0; i < 8; ++i)
        if (crm & (0x80 >> i)) mask |= 0xF000'0000u >> (i * 4);
      cr = (cr & ~mask) | (gpr[d] & mask);
      return;
    }
    case 512:  // mcrxr
      set_cr_field(int(d >> 2), xer >> 28);
      xer &= 0x0FFF'FFFF;
      return;
    case 83:  // mfmsr
      if (msr & kMsrPr) { program(kProgPrivileged); return; }
      gpr[d] = msr;
      return;
    case 146: {  // mtmsr
      if (msr & kMsrPr) { program(kProgPrivileged); return; }
      const u32 old = msr;
      msr = gpr[d];
      update_mode(old);
      return;
    }
    case 339: {  // mfspr
      u32 n = ((op >> 16) & 31) | ((op >> 11) & 31) << 5;
      if ((msr & kMsrPr) && (n & 0x10)) { program(kProgPrivileged); return; }
      gpr[d] = read_spr(n);
      return;
    }
    case 467: {  // mtspr
      u32 n = ((op >> 16) & 31) | ((op >> 11) & 31) << 5;
      if ((msr & kMsrPr) && (n & 0x10)) { program(kProgPrivileged); return; }
      write_spr(n, gpr[d]);
      return;
    }
    case 371: {  // mftb
      u32 n = ((op >> 16) & 31) | ((op >> 11) & 31) << 5;
      gpr[d] = n == 269 ? u32(tb >> 32) : u32(tb);
      return;
    }
    // Segment registers (supervisor).
    case 595: case 659: case 210: case 242: {  // mfsr mfsrin mtsr mtsrin
      if (msr & kMsrPr) { program(kProgPrivileged); return; }
      const u32 n = (xo == 595 || xo == 210) ? (a & 15) : (gpr[b] >> 28);
      if (xo == 595 || xo == 659) {
        gpr[d] = sr[n];
      } else {
        sr[n] = gpr[d];
        flush_tlb();
      }
      return;
    }

    // TLB management (supervisor).
    case 306: case 370: case 566:  // tlbie tlbia tlbsync
      if (msr & kMsrPr) { program(kProgPrivileged); return; }
      if (xo != 566) flush_tlb();
      return;

    // Cache management: the caches are timing-only, so these update residency.
    case 86: case 470: {  // dcbf, dcbi (supervisor)
      if (xo == 470 && (msr & kMsrPr)) { program(kProgPrivileged); return; }
      Translation t;
      u32 bits;
      if (translate(ea_x(op), Access::Read, &t, &bits)) {
        l1d.invalidate(t.pa);
        l2.invalidate(t.pa);
      }
      return;
    }
    case 982: {  // icbi
      Translation t;
      u32 bits;
      if (translate(ea_x(op), Access::Read, &t, &bits)) l1i.invalidate(t.pa);
      last_fetch_line_ = ~0u;
      return;
    }
    case 246: case 278: {  // dcbtst, dcbt: prefetch the line
      Translation t;
      u32 bits;
      if (translate(ea_x(op), Access::Read, &t, &bits) && !t.inhibited && (spr[kSprHid0] & kHid0Dce)) {
        if (!l1d.access(t.pa) && (spr[kSprL2cr] & kL2crEnable)) l2.access(t.pa);
      }
      return;
    }
    case 54: case 598: case 854:  // dcbst sync eieio
      return;
    case 1014: zero_block(ea_x(op)); return;  // dcbz

    // Indexed loads and stores.
    case 23: { u32 v = rd32(ea_x(op)); GUARD(); gpr[d] = v; return; }                           // lwzx
    case 55: { u32 ea = ea_xu(op); u32 v = rd32(ea); GUARD(); gpr[d] = v; gpr[a] = ea; return; }  // lwzux
    case 87: { u32 v = rd8(ea_x(op)); GUARD(); gpr[d] = v; return; }                            // lbzx
    case 119: { u32 ea = ea_xu(op); u32 v = rd8(ea); GUARD(); gpr[d] = v; gpr[a] = ea; return; }  // lbzux
    case 279: { u32 v = rd16(ea_x(op)); GUARD(); gpr[d] = v; return; }                          // lhzx
    case 311: { u32 ea = ea_xu(op); u32 v = rd16(ea); GUARD(); gpr[d] = v; gpr[a] = ea; return; } // lhzux
    case 343: { u32 v = u32(s32(s16(rd16(ea_x(op))))); GUARD(); gpr[d] = v; return; }           // lhax
    case 375: { u32 ea = ea_xu(op); u32 v = u32(s32(s16(rd16(ea)))); GUARD(); gpr[d] = v; gpr[a] = ea; return; }  // lhaux
    case 151: wr32(ea_x(op), gpr[d]); return;                                                    // stwx
    case 183: { u32 ea = ea_xu(op); wr32(ea, gpr[d]); GUARD(); gpr[a] = ea; return; }            // stwux
    case 215: wr8(ea_x(op), u8(gpr[d])); return;                                                 // stbx
    case 247: { u32 ea = ea_xu(op); wr8(ea, u8(gpr[d])); GUARD(); gpr[a] = ea; return; }         // stbux
    case 407: wr16(ea_x(op), u16(gpr[d])); return;                                               // sthx
    case 439: { u32 ea = ea_xu(op); wr16(ea, u16(gpr[d])); GUARD(); gpr[a] = ea; return; }       // sthux
    case 534: { u32 v = std::byteswap(rd32(ea_x(op))); GUARD(); gpr[d] = v; return; }           // lwbrx
    case 790: { u32 v = std::byteswap(rd16(ea_x(op))); GUARD(); gpr[d] = v; return; }           // lhbrx
    case 662: wr32(ea_x(op), std::byteswap(gpr[d])); return;                                     // stwbrx
    case 918: wr16(ea_x(op), std::byteswap(u16(gpr[d]))); return;                                // sthbrx
    case 20: {  // lwarx
      u32 ea = ea_x(op);
      u32 v = rd32(ea);
      GUARD();
      gpr[d] = v;
      reserve_ = true;
      reserve_addr_ = ea & ~31u;
      return;
    }
    case 150: {  // stwcx.
      u32 ea = ea_x(op);
      bool ok = reserve_ && reserve_addr_ == (ea & ~31u);
      if (ok) {
        wr32(ea, gpr[d]);
        GUARD();
      }
      reserve_ = false;
      set_cr_field(0, (ok ? 2 : 0) | xer >> 31);
      return;
    }
    case 597:    // lswi
    case 533: {  // lswx
      u32 ea = xo == 597 ? (a ? gpr[a] : 0) : ea_x(op);
      u32 n = xo == 597 ? (b ? b : 32) : (xer & 0x7F);
      u32 r = d - 1;
      for (u32 i = 0; i < n; ++i) {
        if (i % 4 == 0) {
          r = (r + 1) & 31;
          gpr[r] = 0;
        }
        gpr[r] |= u32(rd8(ea + i)) << (24 - 8 * (i % 4));
      }
      return;
    }
    case 725:    // stswi
    case 661: {  // stswx
      u32 ea = xo == 725 ? (a ? gpr[a] : 0) : ea_x(op);
      u32 n = xo == 725 ? (b ? b : 32) : (xer & 0x7F);
      u32 r = d - 1;
      for (u32 i = 0; i < n; ++i) {
        if (i % 4 == 0) r = (r + 1) & 31;
        wr8(ea + i, u8(gpr[r] >> (24 - 8 * (i % 4))));
      }
      return;
    }

    // Indexed floating-point loads and stores.
    case 535: case 567: case 599: case 631: case 663: case 695: case 727: case 759: case 983: {
      if (!check_fp()) return;
      const bool update = xo == 567 || xo == 631 || xo == 695 || xo == 759;
      if (update && a == 0) { illegal(op); return; }
      u32 ea = update ? ea_xu(op) : ea_x(op);
      switch (xo) {
        case 535: case 567: { u32 v = rd32(ea); GUARD(); fpr[d] = f64_to_bits(double(bits_to_f32(v))); break; }
        case 599: case 631: { u64 v = rd64(ea); GUARD(); fpr[d] = v; break; }
        case 663: case 695: wr32(ea, f32_to_bits(float(bits_to_f64(fpr[d])))); GUARD(); break;
        case 727: case 759: wr64(ea, fpr[d]); GUARD(); break;
        case 983: wr32(ea, u32(fpr[d])); GUARD(); break;  // stfiwx
      }
      if (update) gpr[a] = ea;
      return;
    }

    // AltiVec loads, stores and stream hints.
    case 6: case 38: case 7: case 39: case 71: case 103: case 359:
    case 135: case 167: case 199: case 231: case 487:
    case 342: case 374: case 822:
      altivec_ldst(op, xo);
      return;
  }
  illegal(op);
}

}  // namespace lion
