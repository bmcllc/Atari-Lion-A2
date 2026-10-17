#pragma once
// Assembler for the Tyke DSP (ISA in docs/hardware.md, section "Tyke").
// Program addresses are in 32-bit words.

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace tykeasm {

using u32 = uint32_t;

enum Op : u32 {
  NOP = 0x00, MOVI = 0x01, MOVHI = 0x02, ADD = 0x03, SUB = 0x04, AND = 0x05, OR = 0x06, XOR = 0x07,
  SHL = 0x08, SHR = 0x09, SAR = 0x0A, MUL = 0x0B, ADDI = 0x0C, SHLI = 0x0D, SHRI = 0x0E, SARI = 0x0F,
  ANDI = 0x10, LD = 0x11, ST = 0x12, LDXH = 0x13, LDX = 0x14, CLR = 0x15, MAC = 0x16, MACS = 0x17,
  MVA = 0x18, SAT16 = 0x19, BR = 0x1A, CALL = 0x1B, RET = 0x1C, BEQ = 0x1D, BNE = 0x1E, BLT = 0x1F,
  BGE = 0x20, BLTU = 0x21, BGEU = 0x22, OUT = 0x23, IN = 0x24, WAIT = 0x25, HALT = 0x26,
};

// [31:26] op  [25:22] rd  [21:18] ra  [17:14] rb  [13:0] simm14
inline u32 R(u32 op, u32 rd, u32 ra, u32 rb, int32_t imm = 0) {
  return op << 26 | (rd & 15) << 22 | (ra & 15) << 18 | (rb & 15) << 14 | (u32(imm) & 0x3FFF);
}
inline u32 movi(u32 rd, int32_t v) { return MOVI << 26 | (rd & 15) << 22 | (u32(v) & 0x3FFFF); }
inline u32 movhi(u32 rd, u32 v) { return MOVHI << 26 | (rd & 15) << 22 | (v & 0xFFFF); }
inline u32 add(u32 d, u32 a, u32 b) { return R(ADD, d, a, b); }
inline u32 sub(u32 d, u32 a, u32 b) { return R(SUB, d, a, b); }
inline u32 and_(u32 d, u32 a, u32 b) { return R(AND, d, a, b); }
inline u32 or_(u32 d, u32 a, u32 b) { return R(OR, d, a, b); }
inline u32 mul(u32 d, u32 a, u32 b) { return R(MUL, d, a, b); }
inline u32 addi(u32 d, u32 a, int32_t i) { return R(ADDI, d, a, 0, i); }
inline u32 shli(u32 d, u32 a, u32 n) { return R(SHLI, d, a, 0, int32_t(n)); }
inline u32 shri(u32 d, u32 a, u32 n) { return R(SHRI, d, a, 0, int32_t(n)); }
inline u32 sari(u32 d, u32 a, u32 n) { return R(SARI, d, a, 0, int32_t(n)); }
inline u32 andi(u32 d, u32 a, u32 i) { return R(ANDI, d, a, 0, int32_t(i)); }
inline u32 ld(u32 d, int32_t off, u32 a) { return R(LD, d, a, 0, off); }
inline u32 st(u32 s, int32_t off, u32 a) { return R(ST, s, a, 0, off); }
inline u32 ldxh(u32 d, u32 base, u32 index) { return R(LDXH, d, base, index); }
inline u32 ldx(u32 d, u32 a, int32_t off) { return R(LDX, d, a, 0, off); }
inline u32 clr(u32 acc) { return R(CLR, acc, 0, 0); }
inline u32 mac(u32 acc, u32 a, u32 b) { return R(MAC, acc, a, b); }
inline u32 macs(u32 a, u32 b) { return R(MACS, 0, a, b); }
inline u32 mva(u32 d, u32 acc, u32 shift) { return R(MVA, d, acc, 0, int32_t(shift)); }
inline u32 sat16(u32 d, u32 a) { return R(SAT16, d, a, 0); }
inline u32 ret() { return RET << 26; }
inline u32 out(u32 port, u32 a) { return R(OUT, 0, a, 0, int32_t(port)); }
inline u32 in(u32 d, u32 port) { return R(IN, d, 0, 0, int32_t(port)); }
inline u32 wait() { return WAIT << 26; }
inline u32 halt() { return HALT << 26; }

class Asm {
 public:
  u32 here() const { return u32(code_.size()); }
  Asm& operator<<(u32 w) {
    code_.push_back(w);
    return *this;
  }
  void label(const std::string& l) { labels_[l] = here(); }
  void br(const std::string& l) { fix(l, BR << 26, true); }
  void call(const std::string& l) { fix(l, CALL << 26, true); }
  void beq(u32 a, u32 b, const std::string& l) { fix(l, R(BEQ, 0, a, b), false); }
  void bne(u32 a, u32 b, const std::string& l) { fix(l, R(BNE, 0, a, b), false); }
  void blt(u32 a, u32 b, const std::string& l) { fix(l, R(BLT, 0, a, b), false); }
  void bge(u32 a, u32 b, const std::string& l) { fix(l, R(BGE, 0, a, b), false); }
  void bltu(u32 a, u32 b, const std::string& l) { fix(l, R(BLTU, 0, a, b), false); }
  void bgeu(u32 a, u32 b, const std::string& l) { fix(l, R(BGEU, 0, a, b), false); }
  void li32(u32 d, u32 v) {
    code_.push_back(movi(d, int32_t(v & 0xFFFF)));
    code_.push_back(movhi(d, v >> 16));
  }

  std::vector<u32> finish() const {
    std::vector<u32> out = code_;
    for (const auto& [index, f] : fixups_) {
      auto it = labels_.find(f.label);
      if (it == labels_.end()) throw std::runtime_error("tykeasm: undefined label " + f.label);
      const int32_t disp = int32_t(it->second) - int32_t(index);
      out[index] |= f.wide ? (u32(disp) & 0x03FF'FFFF) : (u32(disp) & 0x3FFF);
    }
    return out;
  }

 private:
  struct Fix {
    std::string label;
    bool wide;
  };
  void fix(const std::string& l, u32 w, bool wide) {
    fixups_.emplace_back(code_.size(), Fix{l, wide});
    code_.push_back(w);
  }
  std::vector<u32> code_;
  std::map<std::string, u32> labels_;
  std::vector<std::pair<size_t, Fix>> fixups_;
};

}  // namespace tykeasm
