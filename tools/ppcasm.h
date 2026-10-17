#pragma once
// Minimal PowerPC instruction encoder with labels, for tests and demo ROMs.

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace ppcasm {

using u32 = uint32_t;

inline u32 D(u32 op, u32 d, u32 a, u32 imm) { return op << 26 | d << 21 | a << 16 | (imm & 0xFFFF); }
inline u32 X(u32 op, u32 d, u32 a, u32 b, u32 xo, u32 rc = 0) {
  return op << 26 | d << 21 | a << 16 | b << 11 | xo << 1 | rc;
}
inline u32 XO(u32 d, u32 a, u32 b, u32 xo, u32 oe = 0, u32 rc = 0) {
  return 31u << 26 | d << 21 | a << 16 | b << 11 | oe << 10 | xo << 1 | rc;
}
inline u32 A(u32 op, u32 d, u32 a, u32 b, u32 c, u32 xo, u32 rc = 0) {
  return op << 26 | d << 21 | a << 16 | b << 11 | c << 6 | xo << 1 | rc;
}
inline u32 VX(u32 d, u32 a, u32 b, u32 xo) { return 4u << 26 | d << 21 | a << 16 | b << 11 | xo; }
inline u32 VA(u32 d, u32 a, u32 b, u32 c, u32 xo) { return 4u << 26 | d << 21 | a << 16 | b << 11 | c << 6 | xo; }
inline u32 spr_field(u32 n) { return (n & 31) << 5 | (n >> 5); }

// Integer.
inline u32 addi(u32 d, u32 a, int16_t i) { return D(14, d, a, u32(i)); }
inline u32 li(u32 d, int16_t i) { return addi(d, 0, i); }
inline u32 addis(u32 d, u32 a, u32 i) { return D(15, d, a, i); }
inline u32 lis(u32 d, u32 i) { return addis(d, 0, i); }
inline u32 addic(u32 d, u32 a, int16_t i) { return D(12, d, a, u32(i)); }
inline u32 addic_(u32 d, u32 a, int16_t i) { return D(13, d, a, u32(i)); }
inline u32 subfic(u32 d, u32 a, int16_t i) { return D(8, d, a, u32(i)); }
inline u32 mulli(u32 d, u32 a, int16_t i) { return D(7, d, a, u32(i)); }
inline u32 ori(u32 a, u32 s, u32 i) { return D(24, s, a, i); }
inline u32 oris(u32 a, u32 s, u32 i) { return D(25, s, a, i); }
inline u32 xori(u32 a, u32 s, u32 i) { return D(26, s, a, i); }
inline u32 xoris(u32 a, u32 s, u32 i) { return D(27, s, a, i); }
inline u32 andi_(u32 a, u32 s, u32 i) { return D(28, s, a, i); }
inline u32 nop() { return ori(0, 0, 0); }
inline u32 cmpwi(u32 crf, u32 a, int16_t i) { return D(11, crf << 2, a, u32(i)); }
inline u32 cmplwi(u32 crf, u32 a, u32 i) { return D(10, crf << 2, a, i); }
inline u32 cmpw(u32 crf, u32 a, u32 b) { return X(31, crf << 2, a, b, 0); }
inline u32 cmplw(u32 crf, u32 a, u32 b) { return X(31, crf << 2, a, b, 32); }
inline u32 add(u32 d, u32 a, u32 b, u32 rc = 0) { return XO(d, a, b, 266, 0, rc); }
inline u32 addo(u32 d, u32 a, u32 b) { return XO(d, a, b, 266, 1, 0); }
inline u32 addc(u32 d, u32 a, u32 b) { return XO(d, a, b, 10); }
inline u32 adde(u32 d, u32 a, u32 b) { return XO(d, a, b, 138); }
inline u32 addze(u32 d, u32 a) { return XO(d, a, 0, 202); }
inline u32 subf(u32 d, u32 a, u32 b, u32 rc = 0) { return XO(d, a, b, 40, 0, rc); }
inline u32 subfc(u32 d, u32 a, u32 b) { return XO(d, a, b, 8); }
inline u32 subfe(u32 d, u32 a, u32 b) { return XO(d, a, b, 136); }
inline u32 neg(u32 d, u32 a) { return XO(d, a, 0, 104); }
inline u32 mullw(u32 d, u32 a, u32 b) { return XO(d, a, b, 235); }
inline u32 mulhw(u32 d, u32 a, u32 b) { return XO(d, a, b, 75); }
inline u32 mulhwu(u32 d, u32 a, u32 b) { return XO(d, a, b, 11); }
inline u32 divw(u32 d, u32 a, u32 b) { return XO(d, a, b, 491); }
inline u32 divwu(u32 d, u32 a, u32 b) { return XO(d, a, b, 459); }
inline u32 and_(u32 a, u32 s, u32 b, u32 rc = 0) { return X(31, s, a, b, 28, rc); }
inline u32 or_(u32 a, u32 s, u32 b, u32 rc = 0) { return X(31, s, a, b, 444, rc); }
inline u32 mr(u32 a, u32 s) { return or_(a, s, s); }
inline u32 xor_(u32 a, u32 s, u32 b) { return X(31, s, a, b, 316); }
inline u32 nor(u32 a, u32 s, u32 b) { return X(31, s, a, b, 124); }
inline u32 andc(u32 a, u32 s, u32 b) { return X(31, s, a, b, 60); }
inline u32 slw(u32 a, u32 s, u32 b) { return X(31, s, a, b, 24); }
inline u32 srw(u32 a, u32 s, u32 b) { return X(31, s, a, b, 536); }
inline u32 sraw(u32 a, u32 s, u32 b) { return X(31, s, a, b, 792); }
inline u32 srawi(u32 a, u32 s, u32 sh) { return X(31, s, a, sh, 824); }
inline u32 cntlzw(u32 a, u32 s) { return X(31, s, a, 0, 26); }
inline u32 extsb(u32 a, u32 s) { return X(31, s, a, 0, 954); }
inline u32 extsh(u32 a, u32 s) { return X(31, s, a, 0, 922); }
inline u32 rlwinm(u32 a, u32 s, u32 sh, u32 mb, u32 me, u32 rc = 0) {
  return 21u << 26 | s << 21 | a << 16 | sh << 11 | mb << 6 | me << 1 | rc;
}
inline u32 rlwimi(u32 a, u32 s, u32 sh, u32 mb, u32 me) { return 20u << 26 | s << 21 | a << 16 | sh << 11 | mb << 6 | me << 1; }
inline u32 slwi(u32 a, u32 s, u32 n) { return rlwinm(a, s, n, 0, 31 - n); }
inline u32 srwi(u32 a, u32 s, u32 n) { return rlwinm(a, s, 32 - n, n, 31); }
inline u32 clrlwi(u32 a, u32 s, u32 n) { return rlwinm(a, s, 0, n, 31); }

// Loads and stores.
inline u32 lwz(u32 d, int16_t o, u32 a) { return D(32, d, a, u32(o)); }
inline u32 lwzu(u32 d, int16_t o, u32 a) { return D(33, d, a, u32(o)); }
inline u32 lbz(u32 d, int16_t o, u32 a) { return D(34, d, a, u32(o)); }
inline u32 lhz(u32 d, int16_t o, u32 a) { return D(40, d, a, u32(o)); }
inline u32 lha(u32 d, int16_t o, u32 a) { return D(42, d, a, u32(o)); }
inline u32 stw(u32 s, int16_t o, u32 a) { return D(36, s, a, u32(o)); }
inline u32 stwu(u32 s, int16_t o, u32 a) { return D(37, s, a, u32(o)); }
inline u32 stb(u32 s, int16_t o, u32 a) { return D(38, s, a, u32(o)); }
inline u32 sth(u32 s, int16_t o, u32 a) { return D(44, s, a, u32(o)); }
inline u32 lmw(u32 d, int16_t o, u32 a) { return D(46, d, a, u32(o)); }
inline u32 stmw(u32 s, int16_t o, u32 a) { return D(47, s, a, u32(o)); }
inline u32 lwzx(u32 d, u32 a, u32 b) { return X(31, d, a, b, 23); }
inline u32 stwx(u32 s, u32 a, u32 b) { return X(31, s, a, b, 151); }
inline u32 lwbrx(u32 d, u32 a, u32 b) { return X(31, d, a, b, 534); }
inline u32 lhbrx(u32 d, u32 a, u32 b) { return X(31, d, a, b, 790); }
inline u32 lbzx(u32 d, u32 a, u32 b) { return X(31, d, a, b, 87); }
inline u32 lhzx(u32 d, u32 a, u32 b) { return X(31, d, a, b, 279); }
inline u32 stbx(u32 s, u32 a, u32 b) { return X(31, s, a, b, 215); }
inline u32 sthx(u32 s, u32 a, u32 b) { return X(31, s, a, b, 407); }
inline u32 lwarx(u32 d, u32 a, u32 b) { return X(31, d, a, b, 20); }
inline u32 stwcx_(u32 s, u32 a, u32 b) { return X(31, s, a, b, 150, 1); }
inline u32 dcbz(u32 a, u32 b) { return X(31, 0, a, b, 1014); }
inline u32 lfs(u32 d, int16_t o, u32 a) { return D(48, d, a, u32(o)); }
inline u32 lfd(u32 d, int16_t o, u32 a) { return D(50, d, a, u32(o)); }
inline u32 stfs(u32 s, int16_t o, u32 a) { return D(52, s, a, u32(o)); }
inline u32 stfd(u32 s, int16_t o, u32 a) { return D(54, s, a, u32(o)); }

// Floating point.
inline u32 fadd(u32 d, u32 a, u32 b) { return A(63, d, a, b, 0, 21); }
inline u32 fsub(u32 d, u32 a, u32 b) { return A(63, d, a, b, 0, 20); }
inline u32 fmul(u32 d, u32 a, u32 c) { return A(63, d, a, 0, c, 25); }
inline u32 fdiv(u32 d, u32 a, u32 b) { return A(63, d, a, b, 0, 18); }
inline u32 fmadd(u32 d, u32 a, u32 c, u32 b) { return A(63, d, a, b, c, 29); }
inline u32 fadds(u32 d, u32 a, u32 b) { return A(59, d, a, b, 0, 21); }
inline u32 fmuls(u32 d, u32 a, u32 c) { return A(59, d, a, 0, c, 25); }
inline u32 fmadds(u32 d, u32 a, u32 c, u32 b) { return A(59, d, a, b, c, 29); }
inline u32 fsubs(u32 d, u32 a, u32 b) { return A(59, d, a, b, 0, 20); }
inline u32 fnmsubs(u32 d, u32 a, u32 c, u32 b) { return A(59, d, a, b, c, 30); }  // b - a * c
inline u32 fdivs(u32 d, u32 a, u32 b) { return A(59, d, a, b, 0, 18); }
inline u32 fsel(u32 d, u32 a, u32 c, u32 b) { return A(63, d, a, b, c, 23); }
inline u32 fabs_(u32 d, u32 b) { return X(63, d, 0, b, 264); }
inline u32 fcmpu(u32 crf, u32 a, u32 b) { return X(63, crf << 2, a, b, 0); }
inline u32 fctiwz(u32 d, u32 b) { return X(63, d, 0, b, 15); }
inline u32 frsp(u32 d, u32 b) { return X(63, d, 0, b, 12); }
inline u32 fmr(u32 d, u32 b) { return X(63, d, 0, b, 72); }
inline u32 fneg(u32 d, u32 b) { return X(63, d, 0, b, 40); }
inline u32 stfiwx(u32 s, u32 a, u32 b) { return X(31, s, a, b, 983); }

// AltiVec.
inline u32 lvx(u32 d, u32 a, u32 b) { return X(31, d, a, b, 103); }
inline u32 stvx(u32 s, u32 a, u32 b) { return X(31, s, a, b, 231); }
inline u32 vaddfp(u32 d, u32 a, u32 b) { return VX(d, a, b, 10); }
inline u32 vmaddfp(u32 d, u32 a, u32 c, u32 b) { return VA(d, a, b, c, 46); }
inline u32 vperm(u32 d, u32 a, u32 b, u32 c) { return VA(d, a, b, c, 43); }
inline u32 vspltw(u32 d, u32 b, u32 i) { return VX(d, i, b, 652); }
inline u32 vspltisw(u32 d, int s) { return VX(d, u32(s) & 31, 0, 908); }
inline u32 vadduwm(u32 d, u32 a, u32 b) { return VX(d, a, b, 128); }
inline u32 vxor(u32 d, u32 a, u32 b) { return VX(d, a, b, 1220); }
inline u32 vcmpeqfp_(u32 d, u32 a, u32 b) { return VX(d, a, b, 198 | 1024); }

// System.
inline u32 sc() { return 17u << 26 | 2; }
inline u32 rfi() { return X(19, 0, 0, 0, 50); }
inline u32 isync() { return X(19, 0, 0, 0, 150); }
inline u32 sync() { return X(31, 0, 0, 0, 598); }
inline u32 mfspr(u32 d, u32 n) { return X(31, d, 0, 0, 339) | spr_field(n) << 11; }
inline u32 mtspr(u32 n, u32 s) { return X(31, s, 0, 0, 467) | spr_field(n) << 11; }
inline u32 mflr(u32 d) { return mfspr(d, 8); }
inline u32 mtlr(u32 s) { return mtspr(8, s); }
inline u32 mtctr(u32 s) { return mtspr(9, s); }
inline u32 mfctr(u32 d) { return mfspr(d, 9); }
inline u32 mfmsr(u32 d) { return X(31, d, 0, 0, 83); }
inline u32 mtmsr(u32 s) { return X(31, s, 0, 0, 146); }
inline u32 mfcr(u32 d) { return X(31, d, 0, 0, 19); }
inline u32 mftb(u32 d) { return X(31, d, 0, 0, 371) | spr_field(268) << 11; }
inline u32 blr() { return 19u << 26 | 20u << 21 | 16u << 1; }
inline u32 bctr() { return 19u << 26 | 20u << 21 | 528u << 1; }
inline u32 bctrl() { return bctr() | 1; }

// BO values.
constexpr u32 kAlways = 20, kTrue = 12, kFalse = 4, kDnz = 16;
// CR bits within field 0.
constexpr u32 kLt = 0, kGt = 1, kEq = 2;

// Assembler with labels and branch fixups.
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
    if (it == labels_.end()) throw std::runtime_error("ppcasm: undefined label " + name);
    return it->second;
  }

  // Unconditional branch / call to a label.
  void b(const std::string& l, bool link = false) { fix(l, 18u << 26 | (link ? 1 : 0), Kind::B24); }
  void bl(const std::string& l) { b(l, true); }
  // Conditional branch: bo/bi per the ISA.
  void bc(u32 bo, u32 bi, const std::string& l) { fix(l, 16u << 26 | bo << 21 | bi << 16, Kind::B14); }
  void beq(const std::string& l, u32 crf = 0) { bc(kTrue, crf * 4 + kEq, l); }
  void bne(const std::string& l, u32 crf = 0) { bc(kFalse, crf * 4 + kEq, l); }
  void blt(const std::string& l, u32 crf = 0) { bc(kTrue, crf * 4 + kLt, l); }
  void bge(const std::string& l, u32 crf = 0) { bc(kFalse, crf * 4 + kLt, l); }
  void bgt(const std::string& l, u32 crf = 0) { bc(kTrue, crf * 4 + kGt, l); }
  void ble(const std::string& l, u32 crf = 0) { bc(kFalse, crf * 4 + kGt, l); }
  void bdnz(const std::string& l) { bc(kDnz, 0, l); }

  // Load a 32-bit constant.
  void li32(u32 d, u32 v) {
    emit(lis(d, v >> 16));
    emit(ori(d, d, v & 0xFFFF));
  }
  // Load a label's address.
  void la(u32 d, const std::string& l) {
    fixups_.push_back({code_.size(), l, Kind::Hi});
    emit(lis(d, 0));
    fixups_.push_back({code_.size(), l, Kind::Lo});
    emit(ori(d, d, 0));
  }

  // Align to `bytes` with zero words.
  void align(u32 bytes) {
    while (here() % bytes) emit(0);
  }
  void word(u32 w) { emit(w); }
  // A data word holding a label's address.
  void addr_word(const std::string& l) { fix(l, 0, Kind::Abs32); }
  void f32(float f) {
    u32 w;
    static_assert(sizeof(f) == 4);
    __builtin_memcpy(&w, &f, 4);
    emit(w);
  }

  std::vector<uint8_t> finish() {
    for (const Fix& f : fixups_) {
      const u32 target = addr(f.label);
      u32& w = code_[f.index];
      const u32 pc = origin_ + u32(f.index * 4);
      const int32_t disp = int32_t(target - pc);
      switch (f.kind) {
        case Kind::B24:
          if (disp < -(1 << 25) || disp >= (1 << 25)) throw std::runtime_error("ppcasm: branch out of range");
          w |= u32(disp) & 0x03FF'FFFC;
          break;
        case Kind::B14:
          if (disp < -(1 << 15) || disp >= (1 << 15)) throw std::runtime_error("ppcasm: bc out of range");
          w |= u32(disp) & 0xFFFC;
          break;
        case Kind::Hi: w |= target >> 16; break;
        case Kind::Lo: w |= target & 0xFFFF; break;
        case Kind::Abs32: w = target; break;
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

  u32 origin() const { return origin_; }

 private:
  enum class Kind { B24, B14, Hi, Lo, Abs32 };
  struct Fix {
    size_t index;
    std::string label;
    Kind kind;
  };
  void fix(const std::string& l, u32 w, Kind k) {
    fixups_.push_back({code_.size(), l, k});
    emit(w);
  }

  u32 origin_;
  std::vector<u32> code_;
  std::map<std::string, u32> labels_;
  std::vector<Fix> fixups_;
};

}  // namespace ppcasm
