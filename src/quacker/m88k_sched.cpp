// Quacker dispatch model: the MC88110 issues two instructions per cycle to its
// two integer units, bit-field unit, multiplier (integer, FP and pmul), FP adder,
// divider, load/store unit and the two graphics units (pixel add, pixel pack).
#include "quacker/m88k.h"

namespace lion {

namespace {

enum Unit : u8 { kInt, kBit, kMul, kFpa, kDiv, kLsu, kGfxAdd, kGfxPack, kBr, kCtl };
constexpr u32 kGpr = 0, kXrf = 32, kCarry = 64;

}  // namespace

bool Quacker::pairable(u8 a, u8 b) {
  if (a == kCtl || b == kCtl) return false;
  if (a == b) return a == kInt;  // two integer units
  return true;
}

IssueInfo Quacker::classify(u32 op) const {
  IssueInfo info;
  const u32 d = (op >> 21) & 31, s1 = (op >> 16) & 31, s2 = op & 31;
  auto gpr = [](RegSet& set, u32 r) {
    if (r) set.add(kGpr + r);
  };
  auto pair = [&](RegSet& set, u32 r) {
    gpr(set, r);
    gpr(set, (r + 1) & 31);
  };
  auto unit = [&](Unit u) { info.unit = u; };
  const u32 primary = op >> 26;

  switch (primary) {
    case 0x00: case 0x01: case 0x0F:  // ld xD
      unit(kLsu);
      info.dst.add(kXrf + d);
      gpr(info.src, s1);
      return info;
    case 0x0C: case 0x0D: case 0x0E:  // st xS
      unit(kLsu);
      info.src.add(kXrf + d);
      gpr(info.src, s1);
      return info;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:  // loads
      unit(kLsu);
      if (primary == 0x04) pair(info.dst, d);
      else gpr(info.dst, d);
      gpr(info.src, s1);
      return info;
    case 0x08: case 0x09: case 0x0A: case 0x0B:  // stores
      unit(kLsu);
      if (primary == 0x08) pair(info.src, d);
      else gpr(info.src, d);
      gpr(info.src, s1);
      return info;
    case 0x1A: case 0x1E: unit(kDiv); gpr(info.dst, d); gpr(info.src, s1); return info;
    case 0x1B: unit(kMul); gpr(info.dst, d); gpr(info.src, s1); return info;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1C: case 0x1D: case 0x1F:
      unit(kInt);
      gpr(info.dst, d);
      gpr(info.src, s1);
      return info;
    case 0x20: info.serialize = true; unit(kCtl); return info;  // control registers
    case 0x21: {                                                // SFU1
      const u32 base = (op >> 11) & 0xF;
      const bool xf = op & 0x8000;
      unit(base == 0x0 ? kMul : (base == 0xE || base == 0xF) ? kDiv : kFpa);
      auto reg = [&](RegSet& set, u32 r) {
        if (xf) set.add(kXrf + r);
        else pair(set, r);
      };
      if (base == 0x7 || base == 0x9 || base == 0xA || base == 0xB) gpr(info.dst, d);  // compare / to integer
      else if (base == 0x4 && (op & 0x200)) info.dst.add(kXrf + d);                   // flt to x
      else reg(info.dst, d);
      if (base == 0x4) gpr(info.src, s2);
      else {
        reg(info.src, s1);
        reg(info.src, s2);
      }
      return info;
    }
    case 0x22: {  // SFU2
      const u32 sub = (op >> 11) & 0x1F;
      unit(sub == 0x00 ? kMul : (sub >= 0x0C ? kGfxPack : kGfxAdd));
      if (sub == 0x07) gpr(info.dst, d);
      else pair(info.dst, d);
      pair(info.src, s1);
      if (sub == 0x00 || sub == 0x0F) gpr(info.src, s2);
      else if (sub != 0x0D && sub != 0x0E) pair(info.src, s2);
      return info;
    }
    case 0x30: case 0x31: unit(kBr); return info;  // br
    case 0x32: case 0x33: unit(kBr); gpr(info.dst, 1); return info;  // bsr
    case 0x34: case 0x35: case 0x36: case 0x37: case 0x3A: case 0x3B:  // bb0, bb1, bcnd
      unit(kBr);
      gpr(info.src, s1);
      return info;
    case 0x3C: {
      const u32 sub = (op >> 10) & 0x3F;
      if (sub >= 0x04 && sub <= 0x0A) {  // x-register indexed load/store
        unit(kLsu);
        if (sub < 0x08) info.dst.add(kXrf + d);
        else info.src.add(kXrf + d);
        gpr(info.src, s1);
        gpr(info.src, s2);
        return info;
      }
      if (sub >= 0x34) {  // tb0, tb1, tcnd
        info.serialize = true;
        unit(kCtl);
        return info;
      }
      unit(kBit);
      gpr(info.dst, d);
      gpr(info.src, s1);
      return info;
    }
    case 0x3D: {
      const u32 sub = (op >> 10) & 0x3F;
      if (sub < 0x10) {  // register-indexed load/store/lda
        unit(sub >= 0x0C ? kInt : kLsu);
        const bool store = sub >= 0x08 && sub <= 0x0B;
        const bool dbl = sub == 0x04 || sub == 0x08;
        if (store) {
          if (dbl) pair(info.src, d);
          else gpr(info.src, d);
        } else {
          if (dbl) pair(info.dst, d);
          else gpr(info.dst, d);
          if (sub <= 0x01) gpr(info.src, d);  // xmem reads and writes D
        }
        gpr(info.src, s1);
        gpr(info.src, s2);
        return info;
      }
      if (sub >= 0x30 && sub <= 0x33) {  // jmp, jsr
        unit(kBr);
        gpr(info.src, s2);
        if (sub >= 0x32) gpr(info.dst, 1);
        return info;
      }
      if (sub == 0x3E || sub == 0x3F) {  // tbnd, rte
        info.serialize = true;
        unit(kCtl);
        return info;
      }
      unit(sub == 0x1A || sub == 0x1E ? kDiv : sub == 0x1B ? kMul : (sub >= 0x20 ? kBit : kInt));
      if ((sub == 0x1A || sub == 0x1B) && (op & 0x100)) pair(info.dst, d);  // .d forms
      else gpr(info.dst, d);
      gpr(info.src, s1);
      gpr(info.src, s2);
      if (sub == 0x18 || sub == 0x19 || sub == 0x1C || sub == 0x1D) {
        if (op & 0x200) info.src.add(kCarry);
        if (op & 0x100) info.dst.add(kCarry);
      }
      return info;
    }
    case 0x3E: info.serialize = true; unit(kCtl); return info;  // tbnd immediate
  }
  info.serialize = true;
  unit(kCtl);
  return info;
}

}  // namespace lion
