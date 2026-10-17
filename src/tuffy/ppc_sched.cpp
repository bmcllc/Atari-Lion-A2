// Tuffy dispatch model: the MPC7400 dispatches two instructions per cycle to
// IU1/IU2 (only IU2 multiplies and divides), the LSU, the FPU, the vector
// ALU and the vector permute unit; branches are folded by the branch unit.
#include "tuffy/ppc.h"

namespace lion {

namespace {

enum Unit : u8 { kIu, kIuComplex, kLsu, kFpu, kValu, kVperm, kBpu, kSru };

constexpr u32 kGpr = 0, kFpr = 32, kVr = 64, kCr = 96, kXer = 99, kVscr = 101;

bool is_vperm(u32 op) {
  if ((op & 0x30) == 0x20) return (op & 0x3F) == 43 || (op & 0x3F) == 44;  // vperm, vsldoi
  switch (op & 0x7FF) {
    case 12: case 76: case 140: case 268: case 332: case 396:            // merges
    case 524: case 588: case 652: case 780: case 844: case 908:          // splats
    case 14: case 78: case 142: case 206: case 270: case 334: case 398: case 462: case 782:  // packs
    case 526: case 590: case 654: case 718: case 846: case 974:          // unpacks
    case 452: case 708: case 1036: case 1100:                            // vsl vsr vslo vsro
      return true;
  }
  return false;
}

}  // namespace

bool Tuffy::pairable(u8 a, u8 b) {
  if (a == kSru || b == kSru) return false;
  if (a == b) return a == kIu;  // two simple integer units
  if ((a == kIu && b == kIuComplex) || (a == kIuComplex && b == kIu)) return true;
  return true;  // different units
}

IssueInfo Tuffy::classify(u32 op) const {
  IssueInfo info;
  const u32 d = (op >> 21) & 31, a = (op >> 16) & 31, b = (op >> 11) & 31, c = (op >> 6) & 31;
  auto src_gpr = [&](u32 r) {
    if (r) info.src.add(kGpr + r);
  };
  auto set = [&](Unit u) { info.unit = u; };
  const u32 primary = op >> 26;
  switch (primary) {
    case 4: {
      set(is_vperm(op) ? kVperm : kValu);
      const u32 xo = op & 0x7FF;
      if (xo == 1540) { info.src.add(kVscr); info.dst.add(kVr + d); break; }  // mfvscr
      if (xo == 1604) { info.src.add(kVr + b); info.dst.add(kVscr); break; }  // mtvscr
      info.dst.add(kVr + d);
      info.src.add(kVr + a);
      info.src.add(kVr + b);
      if ((op & 0x30) == 0x20) info.src.add(kVr + c);
      if (xo & 0x400) info.dst.add(kCr);
      break;
    }
    case 7: set(kIuComplex); info.dst.add(kGpr + d); src_gpr(a); break;               // mulli
    case 8: case 12: case 13: case 14: case 15:                                       // subfic addic addi addis
      set(kIu);
      info.dst.add(kGpr + d);
      src_gpr(a);
      if (primary == 8 || primary == 12 || primary == 13) info.dst.add(kXer);
      if (primary == 13) info.dst.add(kCr);
      break;
    case 3: case 10: case 11: set(kIu); src_gpr(a); info.dst.add(kCr); break;          // twi cmpli cmpi
    case 16: case 18:                                                                   // bc, b
      set(kBpu);
      info.branch = true;
      break;
    case 19: {
      const u32 xo = (op >> 1) & 0x3FF;
      if (xo == 16 || xo == 528) { set(kBpu); info.branch = true; break; }  // bclr, bcctr
      if (xo == 50 || xo == 150) { info.serialize = true; set(kSru); break; }  // rfi, isync
      set(kSru);  // CR logical ops execute in the system register unit
      info.src.add(kCr);
      info.dst.add(kCr);
      break;
    }
    case 20: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29:  // rotates, logical imm
      set(kIu);
      info.dst.add(kGpr + a);
      src_gpr(d);
      if (primary == 20) src_gpr(a);
      if (primary == 23) src_gpr(b);
      if ((primary <= 23 && (op & 1)) || primary == 28 || primary == 29) info.dst.add(kCr);
      break;
    case 31: {
      const u32 xo = (op >> 1) & 0x3FF;
      // Loads and stores.
      switch (xo) {
        case 23: case 55: case 87: case 119: case 279: case 311: case 343: case 375: case 534: case 790: case 20:
          set(kLsu);
          info.dst.add(kGpr + d);
          src_gpr(a);
          src_gpr(b);
          if (xo == 55 || xo == 119 || xo == 311 || xo == 375) info.dst.add(kGpr + a);
          return info;
        case 151: case 183: case 215: case 247: case 407: case 439: case 662: case 918: case 150:
          set(kLsu);
          src_gpr(d);
          src_gpr(a);
          src_gpr(b);
          if (xo == 183 || xo == 247 || xo == 439) info.dst.add(kGpr + a);
          if (xo == 150) info.dst.add(kCr);
          return info;
        case 535: case 567: case 599: case 631:  // FP loads
          set(kLsu);
          info.dst.add(kFpr + d);
          src_gpr(a);
          src_gpr(b);
          if (xo == 567 || xo == 631) info.dst.add(kGpr + a);
          return info;
        case 663: case 695: case 727: case 759: case 983:  // FP stores
          set(kLsu);
          info.src.add(kFpr + d);
          src_gpr(a);
          src_gpr(b);
          if (xo == 695 || xo == 759) info.dst.add(kGpr + a);
          return info;
        case 6: case 38: case 7: case 39: case 71: case 103: case 359:  // vector loads
          set(kLsu);
          info.dst.add(kVr + d);
          src_gpr(a);
          src_gpr(b);
          return info;
        case 135: case 167: case 199: case 231: case 487:  // vector stores
          set(kLsu);
          info.src.add(kVr + d);
          src_gpr(a);
          src_gpr(b);
          return info;
        case 54: case 86: case 246: case 278: case 982: case 1014: case 342: case 374: case 822:  // cache, streams
          set(kLsu);
          src_gpr(a);
          src_gpr(b);
          return info;
        case 0: case 32: case 4:  // cmp, cmpl, tw
          set(kIu);
          src_gpr(a);
          src_gpr(b);
          info.dst.add(kCr);
          return info;
      }
      switch (xo & 0x1FF) {  // XO-form arithmetic
        case 266: case 10: case 138: case 202: case 234: case 40: case 8: case 136: case 200: case 232: case 104:
          set(kIu);
          info.dst.add(kGpr + d);
          src_gpr(a);
          src_gpr(b);
          info.dst.add(kXer);
          info.src.add(kXer);
          if (op & 1) info.dst.add(kCr);
          return info;
        case 235: case 75: case 11: case 491: case 459:  // multiply / divide
          set(kIuComplex);
          info.dst.add(kGpr + d);
          src_gpr(a);
          src_gpr(b);
          if (op & 1) info.dst.add(kCr);
          return info;
      }
      switch (xo) {  // logical and shifts: rA <- rS op rB
        case 28: case 60: case 124: case 284: case 316: case 412: case 444: case 476: case 26: case 922: case 954:
        case 24: case 536: case 792: case 824:
          set(kIu);
          info.dst.add(kGpr + a);
          src_gpr(d);
          if (xo != 26 && xo != 922 && xo != 954 && xo != 824) src_gpr(b);
          if (xo == 792 || xo == 824) info.dst.add(kXer);
          if (op & 1) info.dst.add(kCr);
          return info;
      }
      info.serialize = true;  // SPR/MSR/segment/TLB moves, sync, string ops
      set(kSru);
      return info;
    }
    case 32: case 33: case 34: case 35: case 40: case 41: case 42: case 43:  // integer loads
      set(kLsu);
      info.dst.add(kGpr + d);
      src_gpr(a);
      if (primary & 1) info.dst.add(kGpr + a);
      break;
    case 36: case 37: case 38: case 39: case 44: case 45:  // integer stores
      set(kLsu);
      src_gpr(d);
      src_gpr(a);
      if (primary & 1) info.dst.add(kGpr + a);
      break;
    case 48: case 49: case 50: case 51:  // FP loads
      set(kLsu);
      info.dst.add(kFpr + d);
      src_gpr(a);
      if (primary & 1) info.dst.add(kGpr + a);
      break;
    case 52: case 53: case 54: case 55:  // FP stores
      set(kLsu);
      info.src.add(kFpr + d);
      src_gpr(a);
      if (primary & 1) info.dst.add(kGpr + a);
      break;
    case 59: case 63: {
      set(kFpu);
      const u32 x10 = (op >> 1) & 0x3FF;
      if (primary == 63 && (x10 == 583 || x10 == 711 || x10 == 134 || x10 == 38 || x10 == 70 || x10 == 64)) {
        info.serialize = true;  // FPSCR moves
        break;
      }
      if (primary == 63 && (x10 == 0 || x10 == 32)) {  // compares
        info.src.add(kFpr + a);
        info.src.add(kFpr + b);
        info.dst.add(kCr);
        break;
      }
      info.dst.add(kFpr + d);
      info.src.add(kFpr + a);
      info.src.add(kFpr + b);
      info.src.add(kFpr + c);
      if (op & 1) info.dst.add(kCr);
      break;
    }
    default:
      info.serialize = true;  // lmw/stmw, sc and anything unusual
      set(kSru);
      break;
  }
  return info;
}

}  // namespace lion
