#include <cmath>

#include "test.h"

using namespace ppcasm;
using lion::Lion;

TEST(ppc_integer_arith) {
  Lion sys;
  Asm a(0x1000);
  a.li32(3, 0xFFFF'FFFF);
  a << li(4, 1);
  a << addc(5, 3, 4);      // 0, CA=1
  a << addze(6, 0);        // r6 = 0 + CA = 1
  a << li(7, 100) << li(8, 7);
  a << divw(9, 7, 8);      // 14
  a << mullw(10, 9, 8);    // 98
  a << subf(11, 10, 7);    // 100 - 98 = 2
  a << neg(12, 11);        // -2
  a << srawi(13, 12, 1);   // -1
  a << li(14, -8) << divwu(15, 14, 8);  // 0xFFFFFFF8 / 7
  a.li32(16, 0x8000'0000);
  a << cntlzw(17, 16) << cntlzw(18, 0);
  a.label("halt");
  a.b("halt");
  test::run_tuffy(sys, a);
  auto& g = sys.tuffy.gpr;
  CHECK_EQ(g[5], 0u);
  CHECK_EQ(g[6], 1u);
  CHECK_EQ(g[9], 14u);
  CHECK_EQ(g[10], 98u);
  CHECK_EQ(g[11], 2u);
  CHECK_EQ(g[12], u32(-2));
  CHECK_EQ(g[13], u32(-1));
  CHECK_EQ(g[15], 0xFFFF'FFF8u / 7);
  CHECK_EQ(g[17], 0u);
  CHECK_EQ(g[18], 32u);  // r0 is an ordinary GPR holding 0
}

TEST(ppc_cntlzw_zero) {
  Lion sys;
  Asm a(0x1000);
  a << li(3, 0) << cntlzw(4, 3);
  a.label("halt");
  a.b("halt");
  test::run_tuffy(sys, a);
  CHECK_EQ(sys.tuffy.gpr[4], 32u);
}

TEST(ppc_rotate_masks) {
  Lion sys;
  Asm a(0x1000);
  a.li32(3, 0x1234'5678);
  a << rlwinm(4, 3, 8, 24, 31);  // 0x12
  a << slwi(5, 3, 4);            // 0x23456780
  a << srwi(6, 3, 28);           // 0x1
  a << clrlwi(7, 3, 16);         // 0x5678
  a.li32(8, 0xFFFF'FFFF);
  a << rlwimi(8, 3, 0, 8, 15);   // 0xFF34FFFF
  a << rlwinm(9, 3, 0, 28, 3);   // wrap mask: 0x10000008
  a.label("halt");
  a.b("halt");
  test::run_tuffy(sys, a);
  auto& g = sys.tuffy.gpr;
  CHECK_EQ(g[4], 0x12u);
  CHECK_EQ(g[5], 0x2345'6780u);
  CHECK_EQ(g[6], 0x1u);
  CHECK_EQ(g[7], 0x5678u);
  CHECK_EQ(g[8], 0xFF34'FFFFu);
  CHECK_EQ(g[9], 0x1000'0008u);
}

TEST(ppc_loop_and_memory) {
  Lion sys;
  Asm a(0x1000);
  // Sum 1..10 with a CTR loop, store words, then reload with lmw.
  a << li(3, 0) << li(4, 1) << li(5, 10) << mtctr(5);
  a.li32(6, 0x8000);
  a.label("loop");
  a << add(3, 3, 4) << stw(3, 0, 6) << addi(6, 6, 4) << addi(4, 4, 1);
  a.bdnz("loop");
  a.li32(6, 0x8000);
  a << lmw(22, 0, 6);  // r22..r31 = partial sums
  a << lbz(20, 3, 6) << lha(21, 38, 6);
  a.label("halt");
  a.b("halt");
  test::run_tuffy(sys, a);
  auto& g = sys.tuffy.gpr;
  CHECK_EQ(g[3], 55u);
  CHECK_EQ(g[22], 1u);
  CHECK_EQ(g[31], 55u);
  CHECK_EQ(g[20], 1u);   // low byte of first word (big-endian)
  CHECK_EQ(g[21], 55u);  // low half of the 10th word
  CHECK_EQ(load_be32(sys.bus.host_ptr(0x8024)), 55u);
}

TEST(ppc_compare_branches) {
  Lion sys;
  Asm a(0x1000);
  a << li(3, -5) << li(10, 0);
  a << cmpwi(0, 3, 0);
  a.bge("skip1");
  a << ori(10, 10, 1);  // taken: signed -5 < 0
  a.label("skip1");
  a << cmplwi(1, 3, 0);
  a.ble("skip2", 1);
  a << ori(10, 10, 2);  // taken: unsigned 0xFFFFFFFB > 0
  a.label("skip2");
  a.bl("func");
  a << ori(10, 10, 8);
  a.label("halt");
  a.b("halt");
  a.label("func");
  a << ori(10, 10, 4) << blr();
  test::run_tuffy(sys, a);
  CHECK_EQ(sys.tuffy.gpr[10], 15u);
}

TEST(ppc_reservation) {
  Lion sys;
  Asm a(0x1000);
  a.li32(3, 0x9000);
  a << li(4, 42) << lwarx(5, 0, 3) << stwcx_(4, 0, 3) << mfcr(6);
  a << stwcx_(4, 0, 3) << mfcr(7);  // no reservation any more
  a.label("halt");
  a.b("halt");
  test::run_tuffy(sys, a);
  CHECK_EQ(load_be32(sys.bus.host_ptr(0x9000)), 42u);
  CHECK_EQ(sys.tuffy.gpr[6] >> 28, 2u);
  CHECK_EQ(sys.tuffy.gpr[7] >> 28, 0u);
}

TEST(ppc_floating_point) {
  Lion sys;
  Asm a(0x1000);
  a.la(3, "consts");
  a << lfs(1, 0, 3) << lfs(2, 4, 3);  // 1.5, 2.25
  a << fadd(3, 1, 2) << fmul(4, 1, 2) << fdiv(5, 2, 1) << fmadd(6, 1, 2, 1);
  a << fctiwz(7, 4) << li(4, 0x40) << stfiwx(7, 3, 4);
  a << stfs(3, 8, 3) << fcmpu(2, 1, 2);
  a << fneg(8, 1) << stfd(8, 16, 3);
  a.label("halt");
  a.b("halt");
  a.align(16);
  a.label("consts");
  a.f32(1.5f);
  a.f32(2.25f);
  for (int i = 0; i < 32; ++i) a.word(0);
  test::run_tuffy(sys, a);
  auto f = [&](int i) { return bits_to_f64(sys.tuffy.fpr[i]); };
  CHECK(f(3) == 3.75);
  CHECK(f(4) == 3.375);
  CHECK(f(5) == 1.5);
  CHECK(f(6) == 1.5 * 2.25 + 1.5);
  const u32 consts = a.addr("consts");
  CHECK_EQ(load_be32(sys.bus.host_ptr(consts + 0x40)), 3u);
  CHECK_EQ(load_be32(sys.bus.host_ptr(consts + 8)), f32_to_bits(3.75f));
  CHECK_EQ((sys.tuffy.cr >> (28 - 8)) & 0xF, 8u);  // cr2: 1.5 < 2.25
  CHECK(bits_to_f64(load_be64(sys.bus.host_ptr(consts + 16))) == -1.5);
}

TEST(ppc_altivec) {
  Lion sys;
  Asm a(0x1000);
  a.la(3, "vecs");
  a << li(4, 0) << li(5, 16) << li(6, 32);
  a << lvx(1, 3, 4) << lvx(2, 3, 5);
  a << vmaddfp(3, 1, 2, 1);  // v1*v2 + v1
  a << vaddfp(4, 1, 2);
  a << vspltw(5, 1, 2);       // splat element 2
  a << vspltisw(6, -3);
  a << stvx(3, 3, 6);
  a << vcmpeqfp_(7, 1, 1);
  a.label("halt");
  a.b("halt");
  a.align(16);
  a.label("vecs");
  for (float x : {1.0f, 2.0f, 3.0f, 4.0f}) a.f32(x);
  for (float x : {0.5f, 0.5f, 2.0f, -1.0f}) a.f32(x);
  for (int i = 0; i < 4; ++i) a.word(0);
  test::run_tuffy(sys, a);
  auto& v = sys.tuffy.vr;
  CHECK(bits_to_f32(v[3].w[0]) == 1.5f);
  CHECK(bits_to_f32(v[3].w[2]) == 9.0f);
  CHECK(bits_to_f32(v[3].w[3]) == 0.0f);
  CHECK(bits_to_f32(v[4].w[1]) == 2.5f);
  CHECK(bits_to_f32(v[5].w[0]) == 3.0f && bits_to_f32(v[5].w[3]) == 3.0f);
  CHECK_EQ(v[6].w[1], u32(-3));
  CHECK_EQ(load_be32(sys.bus.host_ptr(a.addr("vecs") + 32 + 8)), f32_to_bits(9.0f));
  CHECK_EQ((sys.tuffy.cr >> 4) & 0xF, 8u);  // cr6: all elements equal
}

TEST(ppc_syscall_and_rfi) {
  Lion sys;
  // Handler at 0xC00: r20 = SRR0, return.
  Asm h(0xC00);
  h << mfspr(20, 26) << li(21, 77) << rfi();
  auto hc = h.finish();
  std::copy(hc.begin(), hc.end(), sys.bus.host_ptr(0xC00));

  Asm a(0x2000);
  a << li(21, 0) << sc() << addi(22, 21, 1);
  a.label("halt");
  a.b("halt");
  test::run_tuffy(sys, a);
  CHECK_EQ(sys.tuffy.gpr[20], 0x2008u);  // SRR0 = instruction after sc
  CHECK_EQ(sys.tuffy.gpr[22], 78u);
  CHECK(sys.tuffy.msr & Lion::kBootMsr);
}

TEST(ppc_external_interrupt_wakes_doze) {
  Lion sys;
  Asm h(0x500);
  // Acknowledge all PIC sources and count interrupts in r30.
  h.li32(28, lion::map::kPic);
  h << li(29, -1) << stw(29, 0, 28) << addi(30, 30, 1) << rfi();
  auto hc = h.finish();
  std::copy(hc.begin(), hc.end(), sys.bus.host_ptr(0x500));

  Asm a(0x2000);
  a.li32(28, lion::map::kPic);
  a << li(29, 1) << stw(29, 4, 28);  // unmask VBLANK
  a << li(30, 0);
  a << mfmsr(3);
  a.li32(4, Lion::kBootMsr | lion::Tuffy::kMsrEe | lion::Tuffy::kMsrPow);
  a << mtmsr(4);  // doze with interrupts on
  a.label("halt");
  a.b("halt");
  auto code = a.finish();
  std::copy(code.begin(), code.end(), sys.bus.host_ptr(0x2000));
  sys.tuffy.reset(0x2000, Lion::kBootMsr);
  sys.tuffy.run(100);
  CHECK(sys.tuffy.dozing());
  const u64 before = sys.tuffy.instructions;
  sys.tuffy.run(1'000'000);
  CHECK_EQ(sys.tuffy.instructions, before);  // asleep: nothing executed
  sys.pic.raise(lion::Pic::kVblank);
  sys.tuffy.run(100);
  CHECK_EQ(sys.tuffy.gpr[30], 1u);
  CHECK(!sys.tuffy.dozing());
}

TEST(ppc_decrementer) {
  Lion sys;
  Asm h(0x900);
  h << li(31, 1) << rfi();
  auto hc = h.finish();
  std::copy(hc.begin(), hc.end(), sys.bus.host_ptr(0x900));
  Asm a(0x2000);
  a << li(31, 0) << li(3, 10) << mtspr(22, 3);
  a.li32(4, Lion::kBootMsr | lion::Tuffy::kMsrEe);
  a << mtmsr(4);
  a.label("spin");
  a << cmpwi(0, 31, 0);
  a.beq("spin");
  a.label("halt");
  a.b("halt");
  test::run_tuffy(sys, a, 10'000);
  CHECK_EQ(sys.tuffy.gpr[31], 1u);
  CHECK(sys.tuffy.tb > 10);
}

TEST(ppc_illegal_opcode_traps) {
  Lion sys;
  Asm h(0x700);
  h << mfspr(25, 27) << mfspr(26, 26) << addi(26, 26, 4) << mtspr(26, 26) << rfi();
  auto hc = h.finish();
  std::copy(hc.begin(), hc.end(), sys.bus.host_ptr(0x700));
  Asm a(0x2000);
  a << 0x0000'0000u << li(24, 5);
  a.label("halt");
  a.b("halt");
  test::run_tuffy(sys, a);
  CHECK_EQ(sys.tuffy.gpr[24], 5u);
  CHECK(sys.tuffy.gpr[25] & 0x0008'0000u);  // SRR1: illegal instruction
}

namespace {

using V = lion::Tuffy::Vr;

// Runs one vector instruction with v1 = a, v2 = b, v3 = c and returns v4.
V vexec(u32 op, V a, V b, V c = {}, u32* vscr = nullptr) {
  Lion sys;
  Asm p(0x2000);
  p << op;
  p.label("halt");
  p.b("halt");
  auto code = p.finish();
  std::copy(code.begin(), code.end(), sys.bus.host_ptr(0x2000));
  sys.tuffy.reset(0x2000, Lion::kBootMsr);
  sys.tuffy.vr[1] = a;
  sys.tuffy.vr[2] = b;
  sys.tuffy.vr[3] = c;
  for (int i = 0; i < 100 && sys.tuffy.pc != p.addr("halt"); ++i) sys.tuffy.run(1);
  if (vscr) *vscr = sys.tuffy.vscr;
  return sys.tuffy.vr[4];
}
V vw(u32 a, u32 b, u32 c, u32 d) { return V{{a, b, c, d}}; }
V vf(float a, float b, float c, float d) { return vw(f32_to_bits(a), f32_to_bits(b), f32_to_bits(c), f32_to_bits(d)); }
bool same(const V& x, const V& y) { return std::equal(std::begin(x.w), std::end(x.w), std::begin(y.w)); }

}  // namespace

TEST(altivec_integer_saturation) {
  u32 vscr = 0;
  // vaddubs: 0xF0 + 0x20 saturates to 0xFF and sets VSCR[SAT].
  V r = vexec(VX(4, 1, 2, 512), vw(0xF001'0203, 0, 0, 0), vw(0x2001'0101, 0, 0, 0), {}, &vscr);
  CHECK_EQ(r.w[0], 0xFF02'0304u);
  CHECK(vscr & 1);
  // vsubshs: -32768 - 1 saturates.
  r = vexec(VX(4, 1, 2, 1856), vw(0x8000'0005, 0, 0, 0), vw(0x0001'0007, 0, 0, 0));
  CHECK_EQ(r.w[0], 0x8000'FFFEu);
  // vmaxsw / vminuw / vavgub
  CHECK(same(vexec(VX(4, 1, 2, 386), vw(u32(-5), 3, 0, 9), vw(2, u32(-1), 0, 10)), vw(2, 3, 0, 10)));
  CHECK(same(vexec(VX(4, 1, 2, 642), vw(u32(-5), 3, 0, 9), vw(2, u32(-1), 0, 10)), vw(2, 3, 0, 9)));
  CHECK_EQ(vexec(VX(4, 1, 2, 1026), vw(0x0103'FF00, 0, 0, 0), vw(0x0204'FF01, 0, 0, 0)).w[0], 0x0204'FF01u);
}

TEST(altivec_shifts_and_permutes) {
  // vslw / vsraw per element.
  CHECK(same(vexec(VX(4, 1, 2, 388), vw(1, 1, 1, 1), vw(0, 4, 31, 33)), vw(1, 16, 0x8000'0000, 2)));
  CHECK(same(vexec(VX(4, 1, 2, 900), vw(0x8000'0000, 16, 0, 0), vw(4, 2, 0, 0)), vw(0xF800'0000, 4, 0, 0)));
  // vsl by 4 bits and vslo by 4 bytes (count in byte 15 of vB).
  CHECK(same(vexec(VX(4, 1, 2, 452), vw(0x1234'5678, 0x9ABC'DEF0, 0, 0x0000'0001), vw(0, 0, 0, 0x0404'0404)),
             vw(0x2345'6789, 0xABCD'EF00, 0, 0x0000'0010)));
  CHECK(same(vexec(VX(4, 1, 2, 1036), vw(1, 2, 3, 4), vw(0, 0, 0, 0x20)), vw(2, 3, 4, 0)));
  // vmrghb
  CHECK_EQ(vexec(VX(4, 1, 2, 12), vw(0x0102'0304, 0, 0, 0), vw(0x1112'1314, 0, 0, 0)).w[0], 0x0111'0212u);
  // vsldoi by 4
  CHECK(same(vexec(VA(4, 1, 2, 0, 44) | 4u << 6, vw(1, 2, 3, 4), vw(5, 6, 7, 8)), vw(2, 3, 4, 5)));
}

TEST(altivec_multiply_and_sums) {
  // vmuleub: even bytes 0x02*0x03, 0xFF*0xFF ...
  V r = vexec(VX(4, 1, 2, 520), vw(0x0200'FF00, 0, 0, 0), vw(0x0300'FF00, 0, 0, 0));
  CHECK_EQ(r.w[0], 0x0006'FE01u);
  // vmsumubm: per word sum of byte products + c.
  r = vexec(VA(4, 1, 2, 3, 36), vw(0x0102'0304, 0, 0, 0), vw(0x0101'0101, 0, 0, 0), vw(100, 0, 0, 0));
  CHECK_EQ(r.w[0], 110u);
  // vsum4ubs and vsumsws
  CHECK_EQ(vexec(VX(4, 1, 2, 1544), vw(0xFFFF'FFFF, 0, 0, 0), vw(1, 0, 0, 0)).w[0], 4u * 255 + 1);
  r = vexec(VX(4, 1, 2, 1928), vw(1, 2, 3, 4), vw(0, 0, 0, 10));
  CHECK(same(r, vw(0, 0, 0, 20)));
  // vmhraddshs: (0x4000 * 0x4000 + 0x4000) >> 15 = 0x2000, + 1
  CHECK_EQ(vexec(VA(4, 1, 2, 3, 33), vw(0x4000'0000, 0, 0, 0), vw(0x4000'0000, 0, 0, 0), vw(0x0001'0000, 0, 0, 0)).w[0],
           0x2001'0000u);
}

TEST(altivec_pack_unpack_float) {
  u32 vscr = 0;
  // vpkuhus: 0x0100 saturates to 0xFF.
  V r = vexec(VX(4, 1, 2, 142), vw(0x0100'0042, 0, 0, 0), vw(0, 0, 0, 0), {}, &vscr);
  CHECK_EQ(r.w[0], 0xFF42'0000u);
  CHECK(vscr & 1);
  // vpkpx then vupkhpx round trip of the 5-bit fields.
  V px = vexec(VX(4, 1, 2, 782), vw(0x01F8'F808, 0, 0, 0), vw(0, 0, 0, 0));
  CHECK_EQ(px.w[0] >> 16, 0xFFE1u);
  CHECK_EQ(vexec(VX(4, 0, 1, 846), px, {}).w[0] & 0x00FF'FFFF, 0x001F'1F01u);
  // vupkhsb sign-extends.
  CHECK_EQ(vexec(VX(4, 0, 1, 526), vw(0x80FF'0102, 0, 0, 0), {}).w[0], 0xFF80'FFFFu);
  // vrfiz / vctsxs saturation / vcmpgtub.
  CHECK(same(vexec(VX(4, 0, 1, 586), vf(1.7f, -1.7f, 2.5f, -0.2f), {}), vf(1, -1, 2, -0.0f)));
  r = vexec(VX(4, 0, 1, 970), vf(3e9f, -3e9f, 1.5f, 0), {}, {}, &vscr);
  CHECK(same(r, vw(0x7FFF'FFFF, 0x8000'0000, 1, 0)));
  r = vexec(VX(4, 1, 2, 518), vw(0x0280'0000, 0, 0, 0), vw(0x0170'0000, 0, 0, 0));
  CHECK_EQ(r.w[0], 0xFFFF'0000u);
  // vexptefp / vlogefp
  r = vexec(VX(4, 0, 1, 394), vf(3, 0, -1, 0.5f), {});
  CHECK(bits_to_f32(r.w[0]) == 8.0f && bits_to_f32(r.w[2]) == 0.5f);
  r = vexec(VX(4, 0, 1, 458), vf(8, 1, 0.25f, 2), {});
  CHECK(bits_to_f32(r.w[0]) == 3.0f && bits_to_f32(r.w[2]) == -2.0f);
}

TEST(ppc_fsqrt_extension_and_strict_mode) {
  for (bool strict : {false, true}) {
    Lion sys;
    Asm h(0x700);
    h << li(25, 1) << mfspr(26, 26) << addi(26, 26, 4) << mtspr(26, 26) << rfi();
    auto hc = h.finish();
    std::copy(hc.begin(), hc.end(), sys.bus.host_ptr(0x700));
    Asm a(0x2000);
    a.la(3, "c");
    a << li(25, 0) << lfd(1, 0, 3) << A(63, 2, 0, 1, 0, 22);  // fsqrt f2, f1
    a.label("halt");
    a.b("halt");
    a.align(8);
    a.label("c");
    a.word(0x4022'0000);  // 9.0
    a.word(0);
    auto code = a.finish();
    std::copy(code.begin(), code.end(), sys.bus.host_ptr(0x2000));
    sys.tuffy.reset(0x2000, Lion::kBootMsr);
    sys.tuffy.strict_7400 = strict;
    for (int i = 0; i < 200 && sys.tuffy.pc != a.addr("halt"); ++i) sys.tuffy.run(1);
    if (strict) {
      CHECK_EQ(sys.tuffy.gpr[25], 1u);  // program exception
    } else {
      CHECK(bits_to_f64(sys.tuffy.fpr[2]) == 3.0);
      CHECK_EQ(sys.tuffy.gpr[25], 0u);
    }
  }
}
