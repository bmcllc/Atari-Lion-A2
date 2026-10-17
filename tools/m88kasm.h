#pragma once
// Minimal Motorola 88000 instruction encoder with labels, for tests and demo ROMs.
// Immediates are unsigned 16-bit, as in the 88k ISA.

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace m88kasm {

using u32 = uint32_t;

inline u32 I(u32 op, u32 d, u32 s1, u32 imm) { return op << 26 | d << 21 | s1 << 16 | (imm & 0xFFFF); }
inline u32 T(u32 sub, u32 d, u32 s1, u32 s2, u32 flags = 0) {
  return 0x3Du << 26 | d << 21 | s1 << 16 | sub << 10 | flags << 5 | s2;
}

// Loads / stores, immediate offset.
inline u32 ld(u32 d, u32 s1, u32 o) { return I(0x05, d, s1, o); }
inline u32 ld_d(u32 d, u32 s1, u32 o) { return I(0x04, d, s1, o); }
inline u32 ld_h(u32 d, u32 s1, u32 o) { return I(0x06, d, s1, o); }
inline u32 ld_hu(u32 d, u32 s1, u32 o) { return I(0x02, d, s1, o); }
inline u32 ld_b(u32 d, u32 s1, u32 o) { return I(0x07, d, s1, o); }
inline u32 ld_bu(u32 d, u32 s1, u32 o) { return I(0x03, d, s1, o); }
inline u32 st(u32 s, u32 s1, u32 o) { return I(0x09, s, s1, o); }
inline u32 st_d(u32 s, u32 s1, u32 o) { return I(0x08, s, s1, o); }
inline u32 st_h(u32 s, u32 s1, u32 o) { return I(0x0A, s, s1, o); }
inline u32 st_b(u32 s, u32 s1, u32 o) { return I(0x0B, s, s1, o); }
// MC88110 extended-register loads/stores (immediate and register-indexed).
inline u32 ld_xs(u32 xd, u32 s1, u32 o) { return I(0x01, xd, s1, o); }   // ld xD (single)
inline u32 ld_xd(u32 xd, u32 s1, u32 o) { return I(0x00, xd, s1, o); }   // ld.d xD
inline u32 ld_xx(u32 xd, u32 s1, u32 o) { return I(0x0F, xd, s1, o); }   // ld.x xD
inline u32 st_xs(u32 xs, u32 s1, u32 o) { return I(0x0D, xs, s1, o); }
inline u32 st_xd(u32 xs, u32 s1, u32 o) { return I(0x0C, xs, s1, o); }
inline u32 st_xx(u32 xs, u32 s1, u32 o) { return I(0x0E, xs, s1, o); }
inline u32 ld_xd_r(u32 xd, u32 s1, u32 s2) { return 0x3Cu << 26 | xd << 21 | s1 << 16 | 0x04u << 10 | s2; }
inline u32 xmem_r(u32 d, u32 s1, u32 s2) { return 0x3Du << 26 | d << 21 | s1 << 16 | 0x01u << 10 | s2; }
// Register-indexed; scaled multiplies the index by the access size.
inline u32 ld_r(u32 d, u32 s1, u32 s2, bool scaled = false) { return T(0x05, d, s1, s2, scaled ? 0x10 : 0); }
inline u32 st_r(u32 s, u32 s1, u32 s2, bool scaled = false) { return T(0x09, s, s1, s2, scaled ? 0x10 : 0); }
inline u32 lda(u32 d, u32 s1, u32 s2) { return T(0x0D, d, s1, s2, 0x10); }

// Logical / arithmetic immediates.
inline u32 and_i(u32 d, u32 s1, u32 i) { return I(0x10, d, s1, i); }
inline u32 and_u(u32 d, u32 s1, u32 i) { return I(0x11, d, s1, i); }
inline u32 mask(u32 d, u32 s1, u32 i) { return I(0x12, d, s1, i); }
inline u32 mask_u(u32 d, u32 s1, u32 i) { return I(0x13, d, s1, i); }
inline u32 xor_i(u32 d, u32 s1, u32 i) { return I(0x14, d, s1, i); }
inline u32 xor_u(u32 d, u32 s1, u32 i) { return I(0x15, d, s1, i); }
inline u32 or_i(u32 d, u32 s1, u32 i) { return I(0x16, d, s1, i); }
inline u32 or_u(u32 d, u32 s1, u32 i) { return I(0x17, d, s1, i); }
inline u32 addu_i(u32 d, u32 s1, u32 i) { return I(0x18, d, s1, i); }
inline u32 subu_i(u32 d, u32 s1, u32 i) { return I(0x19, d, s1, i); }
inline u32 divu_i(u32 d, u32 s1, u32 i) { return I(0x1A, d, s1, i); }
inline u32 mul_i(u32 d, u32 s1, u32 i) { return I(0x1B, d, s1, i); }
inline u32 add_i(u32 d, u32 s1, u32 i) { return I(0x1C, d, s1, i); }
inline u32 sub_i(u32 d, u32 s1, u32 i) { return I(0x1D, d, s1, i); }
inline u32 div_i(u32 d, u32 s1, u32 i) { return I(0x1E, d, s1, i); }
inline u32 cmp_i(u32 d, u32 s1, u32 i) { return I(0x1F, d, s1, i); }

// Triadic register ops.
inline u32 and_(u32 d, u32 s1, u32 s2) { return T(0x10, d, s1, s2); }
inline u32 and_c(u32 d, u32 s1, u32 s2) { return T(0x11, d, s1, s2); }
inline u32 xor_(u32 d, u32 s1, u32 s2) { return T(0x14, d, s1, s2); }
inline u32 or_(u32 d, u32 s1, u32 s2) { return T(0x16, d, s1, s2); }
inline u32 or_c(u32 d, u32 s1, u32 s2) { return T(0x17, d, s1, s2); }
inline u32 addu(u32 d, u32 s1, u32 s2) { return T(0x18, d, s1, s2); }
inline u32 addu_co(u32 d, u32 s1, u32 s2) { return T(0x18, d, s1, s2, 0x08); }
inline u32 addu_ci(u32 d, u32 s1, u32 s2) { return T(0x18, d, s1, s2, 0x10); }
inline u32 subu(u32 d, u32 s1, u32 s2) { return T(0x19, d, s1, s2); }
inline u32 subu_co(u32 d, u32 s1, u32 s2) { return T(0x19, d, s1, s2, 0x08); }
inline u32 divu(u32 d, u32 s1, u32 s2) { return T(0x1A, d, s1, s2); }
inline u32 mul(u32 d, u32 s1, u32 s2) { return T(0x1B, d, s1, s2); }
inline u32 muls(u32 d, u32 s1, u32 s2) { return T(0x1B, d, s1, s2, 0x10); }
inline u32 mulu_d(u32 d, u32 s1, u32 s2) { return T(0x1B, d, s1, s2, 0x08); }
inline u32 divu_d(u32 d, u32 s1, u32 s2) { return T(0x1A, d, s1, s2, 0x08); }
inline u32 add(u32 d, u32 s1, u32 s2) { return T(0x1C, d, s1, s2); }
inline u32 sub(u32 d, u32 s1, u32 s2) { return T(0x1D, d, s1, s2); }
inline u32 div(u32 d, u32 s1, u32 s2) { return T(0x1E, d, s1, s2); }
inline u32 cmp(u32 d, u32 s1, u32 s2) { return T(0x1F, d, s1, s2); }
inline u32 ff1(u32 d, u32 s2) { return T(0x3A, d, 0, s2); }
inline u32 ff0(u32 d, u32 s2) { return T(0x3B, d, 0, s2); }
inline u32 mov(u32 d, u32 s) { return or_(d, 0, s); }

// Bit fields: width w (0 = 32), offset o.
inline u32 BF(u32 sub, u32 d, u32 s1, u32 w, u32 o) { return 0x3Cu << 26 | d << 21 | s1 << 16 | sub << 10 | w << 5 | o; }
inline u32 clr(u32 d, u32 s1, u32 w, u32 o) { return BF(0x20, d, s1, w, o); }
inline u32 set(u32 d, u32 s1, u32 w, u32 o) { return BF(0x22, d, s1, w, o); }
inline u32 ext(u32 d, u32 s1, u32 w, u32 o) { return BF(0x24, d, s1, w, o); }
inline u32 extu(u32 d, u32 s1, u32 w, u32 o) { return BF(0x26, d, s1, w, o); }
inline u32 mak(u32 d, u32 s1, u32 w, u32 o) { return BF(0x28, d, s1, w, o); }
inline u32 rot(u32 d, u32 s1, u32 o) { return BF(0x2A, d, s1, 0, o); }
inline u32 tb0(u32 b, u32 s1, u32 vec) { return 0x3Cu << 26 | b << 21 | s1 << 16 | 0x34u << 10 | (vec & 0x1FF); }
inline u32 tb1(u32 b, u32 s1, u32 vec) { return 0x3Cu << 26 | b << 21 | s1 << 16 | 0x36u << 10 | (vec & 0x1FF); }

// Control and floating point.
inline u32 ldcr(u32 d, u32 crn) { return 0x20u << 26 | d << 21 | 0x08u << 11 | crn << 5; }
inline u32 stcr(u32 s, u32 crn) { return 0x20u << 26 | s << 16 | 0x10u << 11 | crn << 5 | s; }
inline u32 rte() { return 0xF400'FC00u; }
// FP: types 0 = single, 1 = double (td, t1, t2).
inline u32 F(u32 sub, u32 d, u32 s1, u32 s2, u32 td = 0, u32 t1 = 0, u32 t2 = 0) {
  return 0x21u << 26 | d << 21 | s1 << 16 | sub << 11 | t1 << 9 | t2 << 7 | td << 5 | s2;
}
inline u32 fmul(u32 d, u32 s1, u32 s2) { return F(0x00, d, s1, s2); }
inline u32 fadd(u32 d, u32 s1, u32 s2) { return F(0x05, d, s1, s2); }
inline u32 fsub(u32 d, u32 s1, u32 s2) { return F(0x06, d, s1, s2); }
inline u32 fdiv(u32 d, u32 s1, u32 s2) { return F(0x0E, d, s1, s2); }
inline u32 fcmp(u32 d, u32 s1, u32 s2) { return F(0x07, d, s1, s2); }
inline u32 flt(u32 d, u32 s2) { return F(0x04, d, 0, s2); }
inline u32 trnc(u32 d, u32 s2) { return F(0x0B, d, 0, s2); }
inline u32 nint(u32 d, u32 s2) { return F(0x0A, d, 0, s2); }
inline u32 fcmpu(u32 d, u32 s1, u32 s2) { return F(0x07, d, s1, s2) | 0x20; }
// fsqrt on general registers: TD[8:7], T2[6:5].
inline u32 fsqrt(u32 d, u32 s2, u32 td = 0, u32 t2 = 0) { return 0x21u << 26 | d << 21 | 0x0Fu << 11 | td << 7 | t2 << 5 | s2; }

// Extended-register FP forms (bit 15): TD[10:9] T1[8:7] T2[6:5]; type 2 = extended.
inline u32 FX(u32 sub, u32 d, u32 s1, u32 s2, u32 td, u32 t1, u32 t2) {
  return 0x21u << 26 | d << 21 | s1 << 16 | 0x8000u | sub << 11 | td << 9 | t1 << 7 | t2 << 5 | s2;
}
inline u32 fadd_x(u32 xd, u32 x1, u32 x2, u32 td = 2, u32 t1 = 2, u32 t2 = 2) { return FX(0x05, xd, x1, x2, td, t1, t2); }
inline u32 fmul_x(u32 xd, u32 x1, u32 x2, u32 td = 2, u32 t1 = 2, u32 t2 = 2) { return FX(0x00, xd, x1, x2, td, t1, t2); }
inline u32 fdiv_x(u32 xd, u32 x1, u32 x2, u32 td = 2, u32 t1 = 2, u32 t2 = 2) { return FX(0x0E, xd, x1, x2, td, t1, t2); }
inline u32 fcmp_x(u32 d, u32 x1, u32 x2) { return 0x21u << 26 | d << 21 | x1 << 16 | 0x8000u | 0x07u << 11 | 2u << 9 | 2u << 7 | x2; }
inline u32 trnc_x(u32 d, u32 x2) { return 0x21u << 26 | d << 21 | 0x8000u | 0x0Bu << 11 | 2u << 7 | x2; }
inline u32 fsqrt_x(u32 xd, u32 x2) { return 0x21u << 26 | xd << 21 | 0x8000u | 0x0Fu << 11 | 2u << 7 | 2u << 5 | x2; }
inline u32 flt_x(u32 xd, u32 s2, u32 td = 2) { return 0x21u << 26 | xd << 21 | 0x04u << 11 | 0x200u | td << 5 | s2; }
inline u32 mov_x_from_s(u32 xd, u32 s2) { return 0x84004200u | xd << 21 | s2; }   // mov.s xD, rS2
inline u32 mov_x_from_d(u32 xd, u32 s2) { return 0x84004280u | xd << 21 | s2; }   // mov.d xD, rS2:rS2+1
inline u32 mov_s_from_x(u32 d, u32 x2) { return 0x8400C000u | d << 21 | x2; }    // mov.s rD, xS2
inline u32 mov_d_from_x(u32 d, u32 x2) { return 0x8400C080u | d << 21 | x2; }    // mov.d rD, xS2
inline u32 mov_xx(u32 xd, u32 x2) { return 0x8400C300u | xd << 21 | x2; }

// SFU2 graphics (even register pairs). Field: 0 nibble, 1 byte, 2 half, 3 word.
// Saturation: 0 modulo, 1 unsigned (.u), 2 mixed (.us), 3 signed (.s).
inline u32 G(u32 sub, u32 d, u32 s1, u32 s2, u32 low = 0) { return 0x22u << 26 | d << 21 | s1 << 16 | sub << 11 | low | s2; }
inline u32 padd(u32 d, u32 s1, u32 s2, u32 field = 3, u32 sat = 0) { return G(0x04, d, s1, s2, sat << 7 | field << 5); }
inline u32 psub(u32 d, u32 s1, u32 s2, u32 field = 3, u32 sat = 0) { return G(0x06, d, s1, s2, sat << 7 | field << 5); }
inline u32 pcmp(u32 d, u32 s1, u32 s2) { return G(0x07, d, s1, s2, 0x60); }
inline u32 pmul(u32 d, u32 s1, u32 s2) { return G(0x00, d, s1, s2); }
// ppack.r.t: r = result bits (8/16/32) in R[10:7] as r/4; t = source field (1 byte, 2 half, 3 word).
inline u32 ppack(u32 d, u32 s1, u32 s2, u32 r, u32 t = 3) { return G(0x0C, d, s1, s2, (r / 4) << 7 | t << 5); }
inline u32 punpk(u32 d, u32 s1, u32 field) { return G(0x0D, d, s1, 0, field << 5); }
inline u32 prot_i(u32 d, u32 s1, u32 o6) { return G(0x0E, d, s1, 0, ((o6 >> 2) & 0xF) << 7); }
inline u32 prot(u32 d, u32 s1, u32 s2) { return G(0x0F, d, s1, s2); }

// Compare result bits.
constexpr u32 kEq = 2, kNe = 3, kGt = 4, kLe = 5, kLt = 6, kGe = 7, kHi = 8, kLs = 9, kLo = 10, kHs = 11;
// bcnd conditions.
constexpr u32 kEq0 = 0x2, kNe0 = 0xD, kGt0 = 0x1, kLt0 = 0xC, kGe0 = 0x3, kLe0 = 0xE;

class Asm {
 public:
  explicit Asm(u32 origin) : origin_(origin) {}

  u32 here() const { return origin_ + u32(code_.size() * 4); }
  void emit(u32 w) { code_.push_back(w); }
  Asm& operator<<(u32 w) {
    emit(w);
    return *this;
  }
  void label(const std::string& name) { labels_[name] = here(); }
  u32 addr(const std::string& name) const {
    auto it = labels_.find(name);
    if (it == labels_.end()) throw std::runtime_error("m88kasm: undefined label " + name);
    return it->second;
  }

  void br(const std::string& l, bool n = false) { fix(l, (n ? 0x31u : 0x30u) << 26, true); }
  void bsr(const std::string& l, bool n = false) { fix(l, (n ? 0x33u : 0x32u) << 26, true); }
  void bb0(u32 bit, u32 s1, const std::string& l, bool n = false) { fix(l, (n ? 0x35u : 0x34u) << 26 | bit << 21 | s1 << 16, false); }
  void bb1(u32 bit, u32 s1, const std::string& l, bool n = false) { fix(l, (n ? 0x37u : 0x36u) << 26 | bit << 21 | s1 << 16, false); }
  void bcnd(u32 m5, u32 s1, const std::string& l, bool n = false) { fix(l, (n ? 0x3Bu : 0x3Au) << 26 | m5 << 21 | s1 << 16, false); }
  void jmp(u32 s2, bool n = false) { emit(T(n ? 0x31 : 0x30, 0, 0, s2)); }
  void jsr(u32 s2, bool n = false) { emit(T(n ? 0x33 : 0x32, 0, 0, s2)); }

  // Load a 32-bit constant (or.u + or).
  void li32(u32 d, u32 v) {
    emit(or_u(d, 0, v >> 16));
    emit(or_i(d, d, v & 0xFFFF));
  }
  void la(u32 d, const std::string& l) {
    fixups_.push_back({code_.size(), l, Kind::Hi});
    emit(or_u(d, 0, 0));
    fixups_.push_back({code_.size(), l, Kind::Lo});
    emit(or_i(d, d, 0));
  }
  void word(u32 w) { emit(w); }
  void f32(float f) {
    u32 w;
    __builtin_memcpy(&w, &f, 4);
    emit(w);
  }
  void align(u32 bytes) {
    while (here() % bytes) emit(0);
  }

  std::vector<uint8_t> finish() {
    for (const Fix& f : fixups_) {
      const u32 target = addr(f.label);
      u32& w = code_[f.index];
      const int32_t disp = int32_t(target - (origin_ + u32(f.index * 4))) / 4;
      switch (f.kind) {
        case Kind::D26: w |= u32(disp) & 0x03FF'FFFF; break;
        case Kind::D16:
          if (disp < -32768 || disp > 32767) throw std::runtime_error("m88kasm: branch out of range");
          w |= u32(disp) & 0xFFFF;
          break;
        case Kind::Hi: w |= target >> 16; break;
        case Kind::Lo: w |= target & 0xFFFF; break;
      }
    }
    std::vector<uint8_t> out;
    for (u32 w : code_) {
      out.push_back(uint8_t(w >> 24));
      out.push_back(uint8_t(w >> 16));
      out.push_back(uint8_t(w >> 8));
      out.push_back(uint8_t(w));
    }
    return out;
  }

 private:
  enum class Kind { D26, D16, Hi, Lo };
  struct Fix {
    size_t index;
    std::string label;
    Kind kind;
  };
  void fix(const std::string& l, u32 w, bool d26) {
    fixups_.push_back({code_.size(), l, d26 ? Kind::D26 : Kind::D16});
    emit(w);
  }

  u32 origin_;
  std::vector<u32> code_;
  std::map<std::string, u32> labels_;
  std::vector<Fix> fixups_;
};

}  // namespace m88kasm
