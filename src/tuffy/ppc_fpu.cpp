// Tuffy floating-point unit (IEEE 754, 32/64 bits).
// Exceptions and sticky flags are only partially modelled: FPCC is updated by
// compares, rounding mode RN is honoured by fctiw/frsp-style conversions only.
#include <cmath>

#include "tuffy/ppc.h"

namespace lion {

namespace {

inline u32 frD(u32 op) { return (op >> 21) & 31; }
inline u32 frA(u32 op) { return (op >> 16) & 31; }
inline u32 frB(u32 op) { return (op >> 11) & 31; }
inline u32 frC(u32 op) { return (op >> 6) & 31; }

s32 to_int32(double v, u32 rn) {
  if (std::isnan(v)) return INT32_MIN;
  double r;
  switch (rn) {
    case 0: r = std::nearbyint(v); break;  // host default is round-to-nearest-even
    case 1: r = std::trunc(v); break;
    case 2: r = std::ceil(v); break;
    default: r = std::floor(v); break;
  }
  if (r >= 2147483647.0) return INT32_MAX;
  if (r <= -2147483648.0) return INT32_MIN;
  return s32(r);
}

}  // namespace

void Tuffy::op59(u32 op) {
  const double a = bits_to_f64(fpr[frA(op)]);
  const double b = bits_to_f64(fpr[frB(op)]);
  const double c = bits_to_f64(fpr[frC(op)]);
  double r;
  switch ((op >> 1) & 31) {
    case 18: r = a / b; break;                      // fdivs
    case 20: r = a - b; break;                      // fsubs
    case 21: r = a + b; break;                      // fadds
    case 22:  // fsqrts (Atari extension)
      if (strict_7400) { illegal(op); return; }
      r = std::sqrt(b);
      break;
    case 24: r = 1.0 / b; break;                    // fres
    case 25: r = a * c; break;                      // fmuls
    case 28: r = std::fma(a, c, -b); break;         // fmsubs
    case 29: r = std::fma(a, c, b); break;          // fmadds
    case 30: r = -std::fma(a, c, -b); break;        // fnmsubs
    case 31: r = -std::fma(a, c, b); break;         // fnmadds
    default: illegal(op); return;
  }
  fpr[frD(op)] = f64_to_bits(double(float(r)));
  if (op & 1) set_cr_field(1, fpscr >> 28);
}

void Tuffy::op63(u32 op) {
  const u32 d = frD(op);
  const double a = bits_to_f64(fpr[frA(op)]);
  const double b = bits_to_f64(fpr[frB(op)]);
  const double c = bits_to_f64(fpr[frC(op)]);
  const u64 braw = fpr[frB(op)];
  auto done = [&](double r) {
    fpr[d] = f64_to_bits(r);
    if (op & 1) set_cr_field(1, fpscr >> 28);
  };

  // A-form (5-bit extended opcode).
  switch ((op >> 1) & 31) {
    case 18: done(a / b); return;                       // fdiv
    case 20: done(a - b); return;                       // fsub
    case 21: done(a + b); return;                       // fadd
    case 22:  // fsqrt (Atari extension)
      if (strict_7400) { illegal(op); return; }
      done(std::sqrt(b));
      return;
    case 23: done(a >= 0.0 ? c : b); return;            // fsel
    case 25: done(a * c); return;                       // fmul
    case 26: done(1.0 / std::sqrt(b)); return;          // frsqrte
    case 28: done(std::fma(a, c, -b)); return;          // fmsub
    case 29: done(std::fma(a, c, b)); return;           // fmadd
    case 30: done(-std::fma(a, c, -b)); return;         // fnmsub
    case 31: done(-std::fma(a, c, b)); return;          // fnmadd
  }

  // X-form (10-bit extended opcode).
  switch ((op >> 1) & 0x3FF) {
    case 0:     // fcmpu
    case 32: {  // fcmpo
      u32 fpcc = (std::isnan(a) || std::isnan(b)) ? 1 : a < b ? 8 : a > b ? 4 : 2;
      fpscr = (fpscr & ~0xF000u) | fpcc << 12;
      set_cr_field(int(d >> 2), fpcc);
      return;
    }
    case 12: done(double(float(b))); return;  // frsp
    case 14:                                  // fctiw
    case 15: {                                // fctiwz
      s32 v = to_int32(b, ((op >> 1) & 0x3FF) == 15 ? 1 : (fpscr & 3));
      fpr[d] = 0xFFF8'0000'0000'0000ull | u32(v);
      if (op & 1) set_cr_field(1, fpscr >> 28);
      return;
    }
    case 40: fpr[d] = braw ^ (1ull << 63); break;    // fneg
    case 72: fpr[d] = braw; break;                   // fmr
    case 136: fpr[d] = braw | (1ull << 63); break;   // fnabs
    case 264: fpr[d] = braw & ~(1ull << 63); break;  // fabs
    case 38: fpscr |= 0x8000'0000u >> d; break;      // mtfsb1
    case 70: fpscr &= ~(0x8000'0000u >> d); break;   // mtfsb0
    case 64: {                                       // mcrfs
      u32 sf = frA(op) >> 2;
      set_cr_field(int(d >> 2), (fpscr >> (28 - sf * 4)) & 0xF);
      return;
    }
    case 134: {  // mtfsfi
      u32 field = d >> 2, imm = (op >> 12) & 0xF;
      fpscr = (fpscr & ~(0xF000'0000u >> (field * 4))) | imm << (28 - field * 4);
      break;
    }
    case 583: fpr[d] = 0xFFF8'0000'0000'0000ull | fpscr; break;  // mffs
    case 711: {                                                // mtfsf
      u32 fm = (op >> 17) & 0xFF, mask = 0;
      for (int i = 0; i < 8; ++i)
        if (fm & (0x80 >> i)) mask |= 0xF000'0000u >> (i * 4);
      fpscr = (fpscr & ~mask) | (u32(braw) & mask);
      break;
    }
    default: illegal(op); return;
  }
  if (op & 1) set_cr_field(1, fpscr >> 28);
}

}  // namespace lion
