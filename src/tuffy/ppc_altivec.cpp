// Tuffy AltiVec: the full MPC7400 vector instruction set.
// Elements are big-endian: element 0 is the lowest address and the most
// significant part of the register. VSCR[NJ] (non-Java mode) flushes denormals.
#include <algorithm>
#include <cmath>

#include "tuffy/ppc.h"

namespace lion {

namespace {

using Vr = Tuffy::Vr;

inline u32 vD(u32 op) { return (op >> 21) & 31; }
inline u32 vA(u32 op) { return (op >> 16) & 31; }
inline u32 vB(u32 op) { return (op >> 11) & 31; }
inline u32 vC(u32 op) { return (op >> 6) & 31; }

inline u8 B(const Vr& v, int i) { return u8(v.w[i >> 2] >> (24 - 8 * (i & 3))); }
inline void setB(Vr& v, int i, u32 x) {
  const int sh = 24 - 8 * (i & 3);
  v.w[i >> 2] = (v.w[i >> 2] & ~(0xFFu << sh)) | (x & 0xFF) << sh;
}
inline u16 H(const Vr& v, int i) { return u16(v.w[i >> 1] >> (16 - 16 * (i & 1))); }
inline void setH(Vr& v, int i, u32 x) {
  const int sh = 16 - 16 * (i & 1);
  v.w[i >> 1] = (v.w[i >> 1] & ~(0xFFFFu << sh)) | (x & 0xFFFF) << sh;
}
inline s8 SB(const Vr& v, int i) { return s8(B(v, i)); }
inline s16 SH(const Vr& v, int i) { return s16(H(v, i)); }
inline s32 SW(const Vr& v, int i) { return s32(v.w[i]); }

struct Sat {
  bool hit = false;
  s64 clamp(s64 v, s64 lo, s64 hi) {
    if (v < lo) { hit = true; return lo; }
    if (v > hi) { hit = true; return hi; }
    return v;
  }
  u32 u8_(s64 v) { return u32(clamp(v, 0, 0xFF)); }
  u32 s8_(s64 v) { return u32(clamp(v, -128, 127)); }
  u32 u16_(s64 v) { return u32(clamp(v, 0, 0xFFFF)); }
  u32 s16_(s64 v) { return u32(clamp(v, -32768, 32767)); }
  u32 u32_(s64 v) { return u32(clamp(v, 0, 0xFFFF'FFFFll)); }
  u32 s32_(s64 v) { return u32(clamp(v, INT32_MIN, INT32_MAX)); }
};

// Applies `fn(a, b)` to each element of width `bits` (8/16/32), unsigned values.
template <typename F>
Vr lanes(int bits, const Vr& a, const Vr& b, F fn) {
  Vr r{};
  if (bits == 8)
    for (int i = 0; i < 16; ++i) setB(r, i, fn(u32(B(a, i)), u32(B(b, i))));
  else if (bits == 16)
    for (int i = 0; i < 8; ++i) setH(r, i, fn(u32(H(a, i)), u32(H(b, i))));
  else
    for (int i = 0; i < 4; ++i) r.w[i] = fn(a.w[i], b.w[i]);
  return r;
}
inline s32 sext(u32 v, int bits) { return bits == 32 ? s32(v) : s32(v << (32 - bits)) >> (32 - bits); }

}  // namespace

void Tuffy::altivec_ldst(u32 op, u32 xo) {
  if (!check_vec()) return;
  const u32 d = vD(op);
  const u32 ea = (vA(op) ? gpr[vA(op)] : 0) + gpr[vB(op)];
  Vr& v = vr[d];
  switch (xo) {
    case 342: case 374: case 822: return;  // dst, dstst, dss: prefetch hints
    case 6:                                // lvsl
      for (int i = 0; i < 16; ++i) setB(v, i, (ea & 15) + u32(i));
      return;
    case 38:  // lvsr
      for (int i = 0; i < 16; ++i) setB(v, i, 16 - (ea & 15) + u32(i));
      return;
    case 103: case 359: {  // lvx, lvxl
      const u32 base = ea & ~15u;
      Vr t;
      for (int i = 0; i < 4; ++i) t.w[i] = rd32(base + 4 * u32(i));
      if (bus_.stall || fault_) return;
      v = t;
      return;
    }
    case 231: case 487: {  // stvx, stvxl
      const u32 base = ea & ~15u;
      for (int i = 0; i < 4; ++i) wr32(base + 4 * u32(i), v.w[i]);
      return;
    }
    case 7: { u8 x = rd8(ea); if (!bus_.stall && !fault_) setB(v, int(ea & 15), x); return; }                  // lvebx
    case 39: { u16 x = rd16(ea & ~1u); if (!bus_.stall && !fault_) setH(v, int((ea & 15) >> 1), x); return; }  // lvehx
    case 71: { u32 x = rd32(ea & ~3u); if (!bus_.stall && !fault_) v.w[(ea & 15) >> 2] = x; return; }          // lvewx
    case 135: wr8(ea, B(v, int(ea & 15))); return;                                                             // stvebx
    case 167: wr16(ea & ~1u, H(v, int((ea & 15) >> 1))); return;                                               // stvehx
    case 199: wr32(ea & ~3u, v.w[(ea & 15) >> 2]); return;                                                     // stvewx
  }
  illegal(op);
}

void Tuffy::op4(u32 op) {
  if (!check_vec()) return;
  const u32 d = vD(op);
  const Vr a = vr[vA(op)], b = vr[vB(op)], c = vr[vC(op)];
  const u32 uimm = vA(op);
  const bool nj = vscr & 0x1'0000;
  Vr r{};
  Sat sat;

  auto fl = [&](float x) { return (nj && std::fpclassify(x) == FP_SUBNORMAL) ? std::copysign(0.0f, x) : x; };
  auto F = [&](const Vr& v, int i) { return fl(bits_to_f32(v.w[i])); };
  auto mapf = [&](auto fn) {
    for (int i = 0; i < 4; ++i) r.w[i] = f32_to_bits(fl(fn(F(a, i), F(b, i), F(c, i))));
  };

  // VA-form (6-bit extended opcode 32..47).
  if ((op & 0x30) == 0x20) {
    switch (op & 0x3F) {
      case 32:  // vmhaddshs
        for (int i = 0; i < 8; ++i) setH(r, i, sat.s16_(((s32(SH(a, i)) * SH(b, i)) >> 15) + SH(c, i)));
        penalty(2);
        break;
      case 33:  // vmhraddshs
        for (int i = 0; i < 8; ++i) setH(r, i, sat.s16_(((s32(SH(a, i)) * SH(b, i) + 0x4000) >> 15) + SH(c, i)));
        penalty(2);
        break;
      case 34:  // vmladduhm
        for (int i = 0; i < 8; ++i) setH(r, i, u32(H(a, i)) * H(b, i) + H(c, i));
        penalty(2);
        break;
      case 36: case 37: case 38: case 39: case 40: case 41: {  // vmsum{ubm,mbm,uhm,uhs,shm,shs}
        const u32 k = op & 0x3F;
        for (int w = 0; w < 4; ++w) {
          s64 sum = (k == 40 || k == 41) ? s64(SW(c, w)) : s64(c.w[w]);
          if (k == 36 || k == 37) {
            for (int j = 0; j < 4; ++j) {
              const int i = w * 4 + j;
              sum += (k == 37 ? s64(SB(a, i)) : s64(B(a, i))) * B(b, i);
            }
          } else {
            for (int j = 0; j < 2; ++j) {
              const int i = w * 2 + j;
              sum += (k >= 40) ? s64(SH(a, i)) * SH(b, i) : s64(H(a, i)) * H(b, i);
            }
          }
          r.w[w] = k == 39 ? sat.u32_(sum) : k == 41 ? sat.s32_(sum) : u32(sum);
        }
        penalty(2);
        break;
      }
      case 42:  // vsel
        for (int i = 0; i < 4; ++i) r.w[i] = (a.w[i] & ~c.w[i]) | (b.w[i] & c.w[i]);
        break;
      case 43:  // vperm
        for (int i = 0; i < 16; ++i) {
          const u8 s = B(c, i) & 31;
          setB(r, i, s < 16 ? B(a, s) : B(b, s - 16));
        }
        break;
      case 44: {  // vsldoi
        const u32 sh = (op >> 6) & 15;
        for (int i = 0; i < 16; ++i) {
          const u32 s = sh + u32(i);
          setB(r, i, s < 16 ? B(a, int(s)) : B(b, int(s - 16)));
        }
        break;
      }
      case 46: mapf([](float x, float y, float z) { return std::fma(x, z, y); }); break;    // vmaddfp
      case 47: mapf([](float x, float y, float z) { return -std::fma(x, z, -y); }); break;  // vnmsubfp
      default: illegal(op); return;
    }
    if (sat.hit) vscr |= 1;
    vr[d] = r;
    return;
  }

  const u32 xo = op & 0x7FF;

  // VXR-form compares (Rc in bit 21 = 0x400).
  {
    const u32 cxo = xo & 0x3FF;
    bool handled = true;
    auto cmp = [&](int bits, auto pred) {
      r = lanes(bits, a, b, [&](u32 x, u32 y) { return pred(x, y) ? ~0u : 0u; });
    };
    switch (cxo) {
      case 6: cmp(8, [](u32 x, u32 y) { return x == y; }); break;                                  // vcmpequb
      case 70: cmp(16, [](u32 x, u32 y) { return x == y; }); break;                                // vcmpequh
      case 134: cmp(32, [](u32 x, u32 y) { return x == y; }); break;                               // vcmpequw
      case 518: cmp(8, [](u32 x, u32 y) { return x > y; }); break;                                 // vcmpgtub
      case 582: cmp(16, [](u32 x, u32 y) { return x > y; }); break;                                // vcmpgtuh
      case 646: cmp(32, [](u32 x, u32 y) { return x > y; }); break;                                // vcmpgtuw
      case 774: cmp(8, [](u32 x, u32 y) { return sext(x, 8) > sext(y, 8); }); break;               // vcmpgtsb
      case 838: cmp(16, [](u32 x, u32 y) { return sext(x, 16) > sext(y, 16); }); break;            // vcmpgtsh
      case 902: cmp(32, [](u32 x, u32 y) { return s32(x) > s32(y); }); break;                      // vcmpgtsw
      case 198: for (int i = 0; i < 4; ++i) r.w[i] = F(a, i) == F(b, i) ? ~0u : 0; break;          // vcmpeqfp
      case 454: for (int i = 0; i < 4; ++i) r.w[i] = F(a, i) >= F(b, i) ? ~0u : 0; break;          // vcmpgefp
      case 710: for (int i = 0; i < 4; ++i) r.w[i] = F(a, i) > F(b, i) ? ~0u : 0; break;           // vcmpgtfp
      case 966:                                                                                    // vcmpbfp
        for (int i = 0; i < 4; ++i) {
          const float x = F(a, i), y = F(b, i);
          r.w[i] = (x <= y ? 0 : 0x8000'0000u) | (x >= -y ? 0 : 0x4000'0000u);
        }
        break;
      default: handled = false;
    }
    if (handled) {
      vr[d] = r;
      if (xo & 0x400) {
        bool all = true, none = true;
        for (u32 w : r.w) {
          all &= w == ~0u;
          none &= w == 0;
        }
        set_cr_field(6, cxo == 966 ? (none ? 2 : 0) : ((all ? 8 : 0) | (none ? 2 : 0)));
      }
      return;
    }
  }

  auto add_sat = [&](int bits, bool sgn, bool sub) {
    r = lanes(bits, a, b, [&](u32 x, u32 y) -> u32 {
      const s64 xa = sgn ? sext(x, bits) : s64(x), yb = sgn ? sext(y, bits) : s64(y);
      const s64 v = sub ? xa - yb : xa + yb;
      if (bits == 8) return sgn ? sat.s8_(v) : sat.u8_(v);
      if (bits == 16) return sgn ? sat.s16_(v) : sat.u16_(v);
      return sgn ? sat.s32_(v) : sat.u32_(v);
    });
  };
  auto minmax = [&](int bits, bool sgn, bool max) {
    r = lanes(bits, a, b, [&](u32 x, u32 y) -> u32 {
      const bool gt = sgn ? sext(x, bits) > sext(y, bits) : x > y;
      return (gt == max) ? x : y;
    });
  };
  auto avg = [&](int bits, bool sgn) {
    r = lanes(bits, a, b, [&](u32 x, u32 y) -> u32 {
      const s64 v = sgn ? (s64(sext(x, bits)) + sext(y, bits) + 1) >> 1 : (s64(x) + y + 1) >> 1;
      return u32(v);
    });
  };
  auto shift = [&](int bits, int kind) {  // 0 rl, 1 sl, 2 sr, 3 sra
    r = lanes(bits, a, b, [&](u32 x, u32 y) -> u32 {
      const u32 n = y & u32(bits - 1);
      const u32 m = bits == 32 ? ~0u : (1u << bits) - 1;
      switch (kind) {
        case 0: return n ? ((x << n) | (x >> (u32(bits) - n))) & m : x;
        case 1: return (x << n) & m;
        case 2: return x >> n;
        default: return u32(sext(x, bits) >> n) & m;
      }
    });
  };
  auto mul_eo = [&](int bits, bool sgn, bool odd) {
    if (bits == 8) {
      for (int i = 0; i < 8; ++i) {
        const int s = 2 * i + (odd ? 1 : 0);
        setH(r, i, sgn ? u32(s32(SB(a, s)) * SB(b, s)) : u32(B(a, s)) * B(b, s));
      }
    } else {
      for (int i = 0; i < 4; ++i) {
        const int s = 2 * i + (odd ? 1 : 0);
        r.w[i] = sgn ? u32(s32(SH(a, s)) * SH(b, s)) : u32(H(a, s)) * H(b, s);
      }
    }
    penalty(2);
  };
  auto merge = [&](int bits, bool high) {
    const int n = 128 / bits / 2, base = high ? 0 : n;
    for (int i = 0; i < n; ++i) {
      if (bits == 8) {
        setB(r, 2 * i, B(a, base + i));
        setB(r, 2 * i + 1, B(b, base + i));
      } else if (bits == 16) {
        setH(r, 2 * i, H(a, base + i));
        setH(r, 2 * i + 1, H(b, base + i));
      } else {
        r.w[2 * i] = a.w[base + i];
        r.w[2 * i + 1] = b.w[base + i];
      }
    }
  };
  // Packs a then b, from elements of `bits` into elements of bits/2.
  auto pack = [&](int bits, auto conv) {
    if (bits == 16) {
      for (int i = 0; i < 8; ++i) {
        setB(r, i, conv(u32(H(a, i))));
        setB(r, 8 + i, conv(u32(H(b, i))));
      }
    } else {
      for (int i = 0; i < 4; ++i) {
        setH(r, i, conv(a.w[i]));
        setH(r, 4 + i, conv(b.w[i]));
      }
    }
  };
  auto shift128 = [&](bool left, u32 bits) {
    // Whole-register shift of a by `bits` (0..127).
    u8 in[16], out[16] = {};
    for (int i = 0; i < 16; ++i) in[i] = B(a, i);
    const u32 bytes = bits / 8, rem = bits % 8;
    for (int i = 0; i < 16; ++i) {
      u32 v = 0;
      if (left) {
        const int s = i + int(bytes);
        if (s < 16) v = u32(in[s]) << rem;
        if (s + 1 < 16 && rem) v |= u32(in[s + 1]) >> (8 - rem);
      } else {
        const int s = i - int(bytes);
        if (s >= 0) v = u32(in[s]) >> rem;
        if (s - 1 >= 0 && rem) v |= u32(in[s - 1]) << (8 - rem);
      }
      out[i] = u8(v);
    }
    for (int i = 0; i < 16; ++i) setB(r, i, out[i]);
  };

  switch (xo) {
    // Modulo add/subtract.
    case 0: r = lanes(8, a, b, [](u32 x, u32 y) { return x + y; }); break;      // vaddubm
    case 64: r = lanes(16, a, b, [](u32 x, u32 y) { return x + y; }); break;    // vadduhm
    case 128: r = lanes(32, a, b, [](u32 x, u32 y) { return x + y; }); break;   // vadduwm
    case 1024: r = lanes(8, a, b, [](u32 x, u32 y) { return x - y; }); break;   // vsububm
    case 1088: r = lanes(16, a, b, [](u32 x, u32 y) { return x - y; }); break;  // vsubuhm
    case 1152: r = lanes(32, a, b, [](u32 x, u32 y) { return x - y; }); break;  // vsubuwm
    case 384: r = lanes(32, a, b, [](u32 x, u32 y) { return u32((u64(x) + y) >> 32); }); break;  // vaddcuw
    case 1408: r = lanes(32, a, b, [](u32 x, u32 y) { return x >= y ? 1u : 0u; }); break;         // vsubcuw
    // Saturating add/subtract.
    case 512: add_sat(8, false, false); break;    // vaddubs
    case 576: add_sat(16, false, false); break;   // vadduhs
    case 640: add_sat(32, false, false); break;   // vadduws
    case 768: add_sat(8, true, false); break;     // vaddsbs
    case 832: add_sat(16, true, false); break;    // vaddshs
    case 896: add_sat(32, true, false); break;    // vaddsws
    case 1536: add_sat(8, false, true); break;    // vsububs
    case 1600: add_sat(16, false, true); break;   // vsubuhs
    case 1664: add_sat(32, false, true); break;   // vsubuws
    case 1792: add_sat(8, true, true); break;     // vsubsbs
    case 1856: add_sat(16, true, true); break;    // vsubshs
    case 1920: add_sat(32, true, true); break;    // vsubsws
    // Max / min / average.
    case 2: minmax(8, false, true); break;     // vmaxub
    case 66: minmax(16, false, true); break;   // vmaxuh
    case 130: minmax(32, false, true); break;  // vmaxuw
    case 258: minmax(8, true, true); break;    // vmaxsb
    case 322: minmax(16, true, true); break;   // vmaxsh
    case 386: minmax(32, true, true); break;   // vmaxsw
    case 514: minmax(8, false, false); break;  // vminub
    case 578: minmax(16, false, false); break; // vminuh
    case 642: minmax(32, false, false); break; // vminuw
    case 770: minmax(8, true, false); break;   // vminsb
    case 834: minmax(16, true, false); break;  // vminsh
    case 898: minmax(32, true, false); break;  // vminsw
    case 1026: avg(8, false); break;           // vavgub
    case 1090: avg(16, false); break;          // vavguh
    case 1154: avg(32, false); break;          // vavguw
    case 1282: avg(8, true); break;            // vavgsb
    case 1346: avg(16, true); break;           // vavgsh
    case 1410: avg(32, true); break;           // vavgsw
    // Rotates and shifts.
    case 4: shift(8, 0); break;      // vrlb
    case 68: shift(16, 0); break;    // vrlh
    case 132: shift(32, 0); break;   // vrlw
    case 260: shift(8, 1); break;    // vslb
    case 324: shift(16, 1); break;   // vslh
    case 388: shift(32, 1); break;   // vslw
    case 516: shift(8, 2); break;    // vsrb
    case 580: shift(16, 2); break;   // vsrh
    case 644: shift(32, 2); break;   // vsrw
    case 772: shift(8, 3); break;    // vsrab
    case 836: shift(16, 3); break;   // vsrah
    case 900: shift(32, 3); break;   // vsraw
    case 452: shift128(true, B(b, 15) & 7); break;           // vsl
    case 708: shift128(false, B(b, 15) & 7); break;          // vsr
    case 1036: shift128(true, (B(b, 15) >> 3 & 15) * 8); break;   // vslo
    case 1100: shift128(false, (B(b, 15) >> 3 & 15) * 8); break;  // vsro
    // Logical.
    case 1028: r = lanes(32, a, b, [](u32 x, u32 y) { return x & y; }); break;     // vand
    case 1092: r = lanes(32, a, b, [](u32 x, u32 y) { return x & ~y; }); break;    // vandc
    case 1156: r = lanes(32, a, b, [](u32 x, u32 y) { return x | y; }); break;     // vor
    case 1220: r = lanes(32, a, b, [](u32 x, u32 y) { return x ^ y; }); break;     // vxor
    case 1284: r = lanes(32, a, b, [](u32 x, u32 y) { return ~(x | y); }); break;  // vnor
    // Multiply even/odd.
    case 520: mul_eo(8, false, false); break;   // vmuleub
    case 8: mul_eo(8, false, true); break;      // vmuloub
    case 776: mul_eo(8, true, false); break;    // vmulesb
    case 264: mul_eo(8, true, true); break;     // vmulosb
    case 584: mul_eo(16, false, false); break;  // vmuleuh
    case 72: mul_eo(16, false, true); break;    // vmulouh
    case 840: mul_eo(16, true, false); break;   // vmulesh
    case 328: mul_eo(16, true, true); break;    // vmulosh
    // Sums across.
    case 1544:  // vsum4ubs
      for (int w = 0; w < 4; ++w) {
        s64 s = b.w[w];
        for (int j = 0; j < 4; ++j) s += B(a, w * 4 + j);
        r.w[w] = sat.u32_(s);
      }
      penalty(2);
      break;
    case 1800:  // vsum4sbs
      for (int w = 0; w < 4; ++w) {
        s64 s = SW(b, w);
        for (int j = 0; j < 4; ++j) s += SB(a, w * 4 + j);
        r.w[w] = sat.s32_(s);
      }
      penalty(2);
      break;
    case 1608:  // vsum4shs
      for (int w = 0; w < 4; ++w) r.w[w] = sat.s32_(s64(SW(b, w)) + SH(a, w * 2) + SH(a, w * 2 + 1));
      penalty(2);
      break;
    case 1672:  // vsum2sws
      for (int k = 0; k < 2; ++k) r.w[k * 2 + 1] = sat.s32_(s64(SW(a, k * 2)) + SW(a, k * 2 + 1) + SW(b, k * 2 + 1));
      penalty(2);
      break;
    case 1928:  // vsumsws
      r.w[3] = sat.s32_(s64(SW(a, 0)) + SW(a, 1) + SW(a, 2) + SW(a, 3) + SW(b, 3));
      penalty(2);
      break;
    // Floating point.
    case 10: mapf([](float x, float y, float) { return x + y; }); break;                    // vaddfp
    case 74: mapf([](float x, float y, float) { return x - y; }); break;                    // vsubfp
    case 1034: mapf([](float x, float y, float) { return std::fmax(x, y); }); break;        // vmaxfp
    case 1098: mapf([](float x, float y, float) { return std::fmin(x, y); }); break;        // vminfp
    case 266: mapf([](float, float y, float) { return 1.0f / y; }); penalty(3); break;      // vrefp
    case 330: mapf([](float, float y, float) { return 1.0f / std::sqrt(y); }); penalty(3); break;  // vrsqrtefp
    case 394: mapf([](float, float y, float) { return std::exp2(y); }); penalty(3); break;  // vexptefp
    case 458: mapf([](float, float y, float) { return std::log2(y); }); penalty(3); break;  // vlogefp
    case 522: mapf([](float, float y, float) { return std::nearbyint(y); }); break;         // vrfin
    case 586: mapf([](float, float y, float) { return std::trunc(y); }); break;             // vrfiz
    case 650: mapf([](float, float y, float) { return std::ceil(y); }); break;              // vrfip
    case 714: mapf([](float, float y, float) { return std::floor(y); }); break;             // vrfim
    case 842: {  // vcfsx
      const float scale = std::ldexp(1.0f, -int(uimm));
      for (int i = 0; i < 4; ++i) r.w[i] = f32_to_bits(float(SW(b, i)) * scale);
      break;
    }
    case 778: {  // vcfux
      const float scale = std::ldexp(1.0f, -int(uimm));
      for (int i = 0; i < 4; ++i) r.w[i] = f32_to_bits(float(b.w[i]) * scale);
      break;
    }
    case 970: {  // vctsxs
      const double scale = std::ldexp(1.0, int(uimm));
      for (int i = 0; i < 4; ++i) {
        const double v = std::trunc(double(F(b, i)) * scale);
        r.w[i] = std::isnan(v) ? (sat.hit = true, 0u) : sat.s32_(s64(std::clamp(v, -9.3e18, 9.3e18)));
      }
      break;
    }
    case 906: {  // vctuxs
      const double scale = std::ldexp(1.0, int(uimm));
      for (int i = 0; i < 4; ++i) {
        const double v = std::trunc(double(F(b, i)) * scale);
        r.w[i] = std::isnan(v) ? (sat.hit = true, 0u) : sat.u32_(s64(std::clamp(v, -9.3e18, 9.3e18)));
      }
      break;
    }
    // Merge.
    case 12: merge(8, true); break;    // vmrghb
    case 76: merge(16, true); break;   // vmrghh
    case 140: merge(32, true); break;  // vmrghw
    case 268: merge(8, false); break;  // vmrglb
    case 332: merge(16, false); break; // vmrglh
    case 396: merge(32, false); break; // vmrglw
    // Splat.
    case 524: for (int i = 0; i < 16; ++i) setB(r, i, B(b, int(uimm & 15))); break;   // vspltb
    case 588: for (int i = 0; i < 8; ++i) setH(r, i, H(b, int(uimm & 7))); break;     // vsplth
    case 652: for (u32& w : r.w) w = b.w[uimm & 3]; break;                            // vspltw
    case 780: for (int i = 0; i < 16; ++i) setB(r, i, u32(sext(uimm, 5))); break;     // vspltisb
    case 844: for (int i = 0; i < 8; ++i) setH(r, i, u32(sext(uimm, 5))); break;      // vspltish
    case 908: for (u32& w : r.w) w = u32(sext(uimm, 5)); break;                       // vspltisw
    // Pack.
    case 14: pack(16, [](u32 x) { return x & 0xFF; }); break;                            // vpkuhum
    case 78: pack(32, [](u32 x) { return x & 0xFFFF; }); break;                          // vpkuwum
    case 142: pack(16, [&](u32 x) { return sat.u8_(x); }); break;                        // vpkuhus
    case 206: pack(32, [&](u32 x) { return sat.u16_(x); }); break;                       // vpkuwus
    case 270: pack(16, [&](u32 x) { return sat.u8_(sext(x, 16)); }); break;              // vpkshus
    case 334: pack(32, [&](u32 x) { return sat.u16_(s32(x)); }); break;                  // vpkswus
    case 398: pack(16, [&](u32 x) { return sat.s8_(sext(x, 16)) & 0xFF; }); break;       // vpkshss
    case 462: pack(32, [&](u32 x) { return sat.s16_(s32(x)) & 0xFFFF; }); break;        // vpkswss
    case 782:                                                                            // vpkpx
      pack(32, [](u32 x) { return ((x >> 24) & 1) << 15 | ((x >> 19) & 0x1F) << 10 | ((x >> 11) & 0x1F) << 5 | ((x >> 3) & 0x1F); });
      break;
    // Unpack.
    case 526: for (int i = 0; i < 8; ++i) setH(r, i, u32(s32(SB(b, i)))); break;       // vupkhsb
    case 654: for (int i = 0; i < 8; ++i) setH(r, i, u32(s32(SB(b, 8 + i)))); break;   // vupklsb
    case 590: for (int i = 0; i < 4; ++i) r.w[i] = u32(s32(SH(b, i))); break;          // vupkhsh
    case 718: for (int i = 0; i < 4; ++i) r.w[i] = u32(s32(SH(b, 4 + i))); break;      // vupklsh
    case 846: case 974: {                                                              // vupkhpx, vupklpx
      const int base = xo == 846 ? 0 : 4;
      for (int i = 0; i < 4; ++i) {
        const u32 p = H(b, base + i);
        r.w[i] = ((p & 0x8000) ? 0xFF00'0000u : 0) | ((p >> 10) & 0x1F) << 16 | ((p >> 5) & 0x1F) << 8 | (p & 0x1F);
      }
      break;
    }
    // Status.
    case 1540: r.w[3] = vscr; break;  // mfvscr
    case 1604:                        // mtvscr
      vscr = b.w[3] & 0x0001'0001;
      return;
    default: illegal(op); return;
  }
  if (sat.hit) vscr |= 1;
  vr[d] = r;
}

}  // namespace lion
