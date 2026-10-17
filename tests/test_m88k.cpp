#include <cmath>

#include "test.h"

using namespace m88kasm;
using lion::Lion;

namespace {

// Runs Quacker code until it reaches the label "halt".
void run_quacker(Lion& sys, Asm& a, u64 max = 100'000) {
  auto code = a.finish();
  const u32 origin = a.addr("start");
  std::copy(code.begin(), code.end(), sys.bus.host_ptr(origin));
  sys.quacker.reset(origin);
  sys.quacker.running = true;
  const u32 halt = a.addr("halt");
  for (u64 n = 0; n < max && sys.quacker.pc != halt; ++n) sys.quacker.run(1);
}

}  // namespace

TEST(m88k_arith_and_logic) {
  Lion sys;
  Asm a(0x4000);
  a.label("start");
  a.li32(2, 0x1234'5678);
  a << and_i(3, 2, 0x00FF);    // keeps upper half: 0x12340078
  a << mask(4, 2, 0x00FF);     // 0x78
  a << or_u(5, 0, 0xABCD);     // 0xABCD0000
  a << addu_i(6, 0, 1000) << subu_i(7, 6, 1);  // 999
  a << mul_i(8, 7, 3) << divu_i(9, 8, 9);      // 2997, 333
  a.li32(10, 0xFFFF'FFFF);
  a << addu_i(11, 0, 1);
  a << addu_co(12, 10, 11);    // 0, carry
  a << addu_ci(13, 0, 0);      // 1
  a << ff1(14, 5) << ff0(15, 10);
  a << subu(16, 7, 6);         // -1
  a << mov(17, 2);
  a.label("halt");
  a.br("halt");
  run_quacker(sys, a);
  auto& r = sys.quacker.r;
  CHECK_EQ(r[3], 0x1234'0078u);
  CHECK_EQ(r[4], 0x78u);
  CHECK_EQ(r[5], 0xABCD'0000u);
  CHECK_EQ(r[7], 999u);
  CHECK_EQ(r[8], 2997u);
  CHECK_EQ(r[9], 333u);
  CHECK_EQ(r[12], 0u);
  CHECK_EQ(r[13], 1u);
  CHECK_EQ(r[14], 31u);
  CHECK_EQ(r[15], 32u);
  CHECK_EQ(r[16], u32(-1));
  CHECK_EQ(r[17], 0x1234'5678u);
  CHECK_EQ(r[0], 0u);
}

TEST(m88k_bitfields) {
  Lion sys;
  Asm a(0x4000);
  a.label("start");
  a.li32(2, 0xF0F0'1234);
  a << extu(3, 2, 8, 4);   // 0x23
  a << ext(4, 2, 4, 28);   // 0xF -> -1
  a << mak(5, 2, 8, 16);   // 0x00340000
  a << clr(6, 2, 16, 16);  // 0x00001234
  a << set(7, 0, 4, 8);    // 0x00000F00
  a << rot(8, 2, 4);       // 0x4F0F0123
  a.label("halt");
  a.br("halt");
  run_quacker(sys, a);
  auto& r = sys.quacker.r;
  CHECK_EQ(r[3], 0x23u);
  CHECK_EQ(r[4], 0xFFFF'FFFFu);
  CHECK_EQ(r[5], 0x0034'0000u);
  CHECK_EQ(r[6], 0x0000'1234u);
  CHECK_EQ(r[7], 0x0000'0F00u);
  CHECK_EQ(r[8], 0x4F0F'0123u);
}

TEST(m88k_branches_and_delay_slots) {
  Lion sys;
  Asm a(0x4000);
  a.label("start");
  a << or_i(2, 0, 10) << or_i(3, 0, 0);
  a.label("loop");  // r3 += r2 while r2 != 0
  a << addu(3, 3, 2) << subu_i(2, 2, 1);
  a.bcnd(kNe0, 2, "loop");
  a << cmp_i(4, 3, 55);
  a.bb1(kEq, 4, "eq");
  a << or_i(5, 0, 0xBAD);
  a.label("eq");
  // Delayed call: the instruction after bsr.n executes before the subroutine.
  a.bsr("sub", true);
  a << or_i(6, 0, 1);
  a << or_i(8, 0, 3);
  a.label("halt");
  a.br("halt");
  a.label("sub");
  a << or_i(7, 6, 0);  // sees r6 = 1 from the delay slot
  a.jmp(1);
  run_quacker(sys, a);
  auto& r = sys.quacker.r;
  CHECK_EQ(r[3], 55u);
  CHECK_EQ(r[5], 0u);
  CHECK_EQ(r[7], 1u);
  CHECK_EQ(r[8], 3u);
}

TEST(m88k_loads_stores) {
  Lion sys;
  Asm a(0x4000);
  a.label("start");
  a.li32(2, 0x6000);
  a.li32(3, 0x8899'AABB);
  a << st(3, 2, 0) << st_h(3, 2, 4) << st_b(3, 2, 8);
  a << ld(4, 2, 0) << ld_h(5, 2, 0) << ld_hu(6, 2, 0) << ld_b(7, 2, 3) << ld_bu(8, 2, 3);
  a << or_i(9, 0, 1) << ld_r(10, 2, 9, true);  // scaled index: word at 0x6004
  a << st_d(3, 2, 16) << ld_d(12, 2, 16);
  a.label("halt");
  a.br("halt");
  run_quacker(sys, a);
  auto& r = sys.quacker.r;
  CHECK_EQ(r[4], 0x8899'AABBu);
  CHECK_EQ(r[5], 0xFFFF'8899u);
  CHECK_EQ(r[6], 0x8899u);
  CHECK_EQ(r[7], 0xFFFF'FFBBu);
  CHECK_EQ(r[8], 0xBBu);
  CHECK_EQ(r[10], 0xAABB'0000u);
  CHECK_EQ(r[12], 0x8899'AABBu);
  CHECK_EQ(r[13], 0x8899'AABBu);  // st.d stored the pair r3:r4
}

TEST(m88k_floating_point) {
  Lion sys;
  Asm a(0x4000);
  a.label("start");
  a << or_i(2, 0, 7) << flt(3, 2);  // 7.0f
  a.li32(4, f32_to_bits(0.5f));
  a << fmul(5, 3, 4) << fadd(6, 3, 4) << fdiv(7, 3, 4) << trnc(8, 6) << fcmp(9, 4, 3);
  a.label("halt");
  a.br("halt");
  run_quacker(sys, a);
  auto& r = sys.quacker.r;
  CHECK(bits_to_f32(r[3]) == 7.0f);
  CHECK(bits_to_f32(r[5]) == 3.5f);
  CHECK(bits_to_f32(r[6]) == 7.5f);
  CHECK(bits_to_f32(r[7]) == 14.0f);
  CHECK_EQ(r[8], 7u);
  CHECK(r[9] & (1u << kLt));
}

TEST(m88k_trap_and_rte) {
  Lion sys;
  Asm a(0x4000);
  a.label("start");
  a.li32(2, 0x5000);
  a << stcr(2, 7);                 // VBR = 0x5000
  a << or_i(3, 0, 1);
  a << tb1(0, 3, 4);               // bit 0 set -> trap vector 4
  a << or_i(5, 0, 9);
  a.label("halt");
  a.br("halt");
  Asm h(0x5000 + 4 * 8);
  h << or_i(4, 0, 0x77) << rte();
  auto hc = h.finish();
  std::copy(hc.begin(), hc.end(), sys.bus.host_ptr(0x5000 + 4 * 8));
  run_quacker(sys, a);
  CHECK_EQ(sys.quacker.r[4], 0x77u);
  CHECK_EQ(sys.quacker.r[5], 9u);
}

TEST(m88k_fifo_stalls_until_data) {
  Lion sys;
  Asm a(0x4000);
  a.label("start");
  a.li32(2, lion::map::kQuackerCtrl);
  a << ld(3, 2, 0x08);  // pop: waits for Tuffy
  a.label("halt");
  a.br("halt");
  auto code = a.finish();
  std::copy(code.begin(), code.end(), sys.bus.host_ptr(0x4000));
  sys.quacker.reset(0x4000);
  sys.quacker.running = true;
  sys.quacker.run(1000);
  CHECK(sys.quacker.pc != a.addr("halt"));
  sys.bus.write32(lion::map::kQuackerCtrl + 0x08, 0xC0FFEE, lion::Master::Tuffy);
  sys.quacker.run(10);
  CHECK_EQ(sys.quacker.r[3], 0xC0FFEEu);
}

// Encodings checked against the GNU binutils m88k opcode table (gas/config/m88k-opcode.h).
TEST(m88k_encodings_match_binutils) {
  struct G {
    u32 got, want;
    const char* name;
  };
  const G golden[] = {
      {ld(0, 0, 0), 0x1400'0000, "ld imm"},          {ld_r(0, 0, 0), 0xF400'1400, "ld reg"},
      {ld_r(0, 0, 0, true), 0xF400'1600, "ld [ ]"}, {ld_d(0, 0, 0), 0x1000'0000, "ld.d"},
      {ld_h(0, 0, 0), 0x1800'0000, "ld.h"},         {ld_hu(0, 0, 0), 0x0800'0000, "ld.hu"},
      {ld_b(0, 0, 0), 0x1C00'0000, "ld.b"},         {ld_bu(0, 0, 0), 0x0C00'0000, "ld.bu"},
      {st(0, 0, 0), 0x2400'0000, "st"},             {st_r(0, 0, 0), 0xF400'2400, "st reg"},
      {st_d(0, 0, 0), 0x2000'0000, "st.d"},         {st_h(0, 0, 0), 0x2800'0000, "st.h"},
      {st_b(0, 0, 0), 0x2C00'0000, "st.b"},         {lda(0, 0, 0), 0xF400'3600, "lda"},
      {ld_xs(0, 0, 0), 0x0400'0000, "ld xd"},       {ld_xd(0, 0, 0), 0x0000'0000, "ld.d xd"},
      {ld_xx(0, 0, 0), 0x3C00'0000, "ld.x xd"},     {st_xs(0, 0, 0), 0x3400'0000, "st xd"},
      {st_xd(0, 0, 0), 0x3000'0000, "st.d xd"},     {st_xx(0, 0, 0), 0x3800'0000, "st.x xd"},
      {ld_xd_r(0, 0, 0), 0xF000'1000, "ld.d xd reg"},
      {and_(0, 0, 0), 0xF400'4000, "and"},          {and_i(0, 0, 0), 0x4000'0000, "and imm"},
      {and_c(0, 0, 0), 0xF400'4400, "and.c"},       {and_u(0, 0, 0), 0x4400'0000, "and.u"},
      {mask(0, 0, 0), 0x4800'0000, "mask"},         {mask_u(0, 0, 0), 0x4C00'0000, "mask.u"},
      {or_(0, 0, 0), 0xF400'5800, "or"},            {or_c(0, 0, 0), 0xF400'5C00, "or.c"},
      {or_u(0, 0, 0), 0x5C00'0000, "or.u"},         {add(0, 0, 0), 0xF400'7000, "add"},
      {add_i(0, 0, 0), 0x7000'0000, "add imm"},     {addu(0, 0, 0), 0xF400'6000, "addu"},
      {addu_ci(0, 0, 0), 0xF400'6200, "addu.ci"},   {addu_co(0, 0, 0), 0xF400'6100, "addu.co"},
      {sub(0, 0, 0), 0xF400'7400, "sub"},           {subu(0, 0, 0), 0xF400'6400, "subu"},
      {subu_co(0, 0, 0), 0xF400'6500, "subu.co"},   {mul(0, 0, 0), 0xF400'6C00, "mul"},
      {muls(0, 0, 0), 0xF400'6E00, "muls"},         {mulu_d(0, 0, 0), 0xF400'6D00, "mulu.d"},
      {divu(0, 0, 0), 0xF400'6800, "divu"},         {divu_d(0, 0, 0), 0xF400'6900, "divu.d"},
      {div(0, 0, 0), 0xF400'7800, "divs"},          {cmp(0, 0, 0), 0xF400'7C00, "cmp"},
      {cmp_i(0, 0, 0), 0x7C00'0000, "cmp imm"},     {ff1(0, 0), 0xF400'E800, "ff1"},
      {ff0(0, 0), 0xF400'EC00, "ff0"},              {clr(0, 0, 0, 0), 0xF000'8000, "clr"},
      {ext(0, 0, 0, 0), 0xF000'9000, "ext"},        {extu(0, 0, 0, 0), 0xF000'9800, "extu"},
      {mak(0, 0, 0, 0), 0xF000'A000, "mak"},        {rot(0, 0, 0), 0xF000'A800, "rot"},
      {tb0(0, 0, 0), 0xF000'D000, "tb0"},           {ldcr(0, 0), 0x8000'4000, "ldcr"},
      {stcr(0, 0), 0x8000'8000, "stcr"},            {rte(), 0xF400'FC00, "rte"},
      {fadd(0, 0, 0), 0x8400'2800, "fadd.sss"},     {F(0x05, 0, 0, 0, 0, 0, 1), 0x8400'2880, "fadd.ssd"},
      {F(0x05, 0, 0, 0, 1, 0, 0), 0x8400'2820, "fadd.dss"}, {F(0x05, 0, 0, 0, 0, 1, 0), 0x8400'2A00, "fadd.sds"},
      {fadd_x(0, 0, 0, 0, 0, 0), 0x8400'A800, "fadd.sss x"}, {fadd_x(0, 0, 0), 0x8400'AD40, "fadd.xxx"},
      {fadd_x(0, 0, 0, 1, 0, 0), 0x8400'AA00, "fadd.dss x"}, {fadd_x(0, 0, 0, 0, 1, 0), 0x8400'A880, "fadd.sds x"},
      {fcmp(0, 0, 0), 0x8400'3800, "fcmp.sss"},     {fcmpu(0, 0, 0), 0x8400'3820, "fcmpu.sss"},
      {fcmp_x(0, 0, 0), 0x8400'BD00, "fcmp.sxx"},   {flt(0, 0), 0x8400'2000, "flt.ss"},
      {flt_x(0, 0), 0x8400'2240, "flt.xs"},         {trnc(0, 0), 0x8400'5800, "trnc.ss"},
      {trnc_x(0, 0), 0x8400'D900, "trnc.sx"},       {fsqrt(0, 0), 0x8400'7800, "fsqrt.ss"},
      {fsqrt(0, 0, 0, 1), 0x8400'7820, "fsqrt.sd"}, {fsqrt_x(0, 0), 0x8400'F940, "fsqrt.xx"},
      {mov_s_from_x(0, 0), 0x8400'C000, "mov.s d,x"}, {mov_x_from_s(0, 0), 0x8400'4200, "mov.s x,d"},
      {mov_d_from_x(0, 0), 0x8400'C080, "mov.d d,x"}, {mov_xx(0, 0), 0x8400'C300, "mov x,x"},
      {padd(0, 0, 0, 1), 0x8800'2020, "padd.b"},    {padd(0, 0, 0, 2), 0x8800'2040, "padd.h"},
      {padd(0, 0, 0), 0x8800'2060, "padd"},         {psub(0, 0, 0), 0x8800'3060, "psub"},
      {pcmp(0, 0, 0), 0x8800'3860, "pcmp"},         {pmul(0, 0, 0), 0x8800'0000, "pmul"},
      {ppack(0, 0, 0, 32, 1), 0x8800'6420, "ppack.32.b"}, {ppack(0, 0, 0, 16, 2), 0x8800'6240, "ppack.16.h"},
      {ppack(0, 0, 0, 32, 2), 0x8800'6440, "ppack.32.h"}, {ppack(0, 0, 0, 8), 0x8800'6160, "ppack.8"},
      {ppack(0, 0, 0, 16), 0x8800'6260, "ppack.16"}, {ppack(0, 0, 0, 32), 0x8800'6460, "ppack.32"},
      {punpk(0, 0, 0), 0x8800'6800, "punpk.n"},     {punpk(0, 0, 1), 0x8800'6820, "punpk.b"},
      {punpk(0, 0, 2), 0x8800'6840, "punpk.h"},     {prot(0, 0, 0), 0x8800'7800, "prot reg"},
      {prot_i(0, 0, 0), 0x8800'7000, "prot imm"},
  };
  for (const G& g : golden) {
    if (g.got != g.want) std::printf("  encoding %s: %08X, binutils %08X\n", g.name, g.got, g.want);
    CHECK_EQ(g.got, g.want);
  }
}

TEST(m88k_extended_registers) {
  Lion sys;
  Asm a(0x4000);
  a.label("start");
  a.li32(2, 0x6000);
  a << or_i(3, 0, 10) << flt_x(1, 3);          // x1 = 10.0 (extended)
  a << or_i(3, 0, 3) << flt_x(2, 3);           // x2 = 3.0
  a << fdiv_x(3, 1, 2);                        // x3 = 10/3 in extended precision
  a << fmul_x(4, 3, 2);                        // x4 = 10 (rounded)
  a << trnc_x(5, 4);                           // r5 = 10
  a << st_xx(3, 2, 0) << ld_xx(6, 2, 0);       // round trip through memory
  a << fcmp_x(7, 3, 6);                        // equal
  a << mov_d_from_x(8, 3);                     // r8:r9 = double(10/3)
  a << fsqrt_x(10, 2) << mov_s_from_x(11, 10); // sqrt(3) as single
  a << flt_x(0, 3);                            // x0 stays zero
  a << mulu_d(12, 2, 2);                       // 0x6000^2 in r12:r13
  a.li32(14, 0xFFFF'FFFF);
  a << muls(15, 14, 3);                        // -1 * 3
  a.label("halt");
  a.br("halt");
  run_quacker(sys, a);
  auto& q = sys.quacker;
  CHECK(q.x[3] == 10.0L / 3.0L);
  CHECK_EQ(q.r[5], 10u);
  CHECK(q.x[6] == q.x[3]);
  CHECK(q.r[7] & (1u << kEq));
  CHECK(bits_to_f64(u64(q.r[8]) << 32 | q.r[9]) == 10.0 / 3.0);
  CHECK(bits_to_f32(q.r[11]) == std::sqrt(3.0f));
  CHECK(q.x[0] == 0.0L);
  CHECK_EQ(u64(q.r[12]) << 32 | q.r[13], 0x6000ull * 0x6000ull);
  CHECK_EQ(q.r[15], u32(-3));
}

TEST(m88k_graphics_unit) {
  Lion sys;
  Asm a(0x4000);
  a.label("start");
  // r2:r3 = 0x10F0'7F80'0102'0304, r4:r5 = 0x20'20'01'90'FF'01'01'01
  a.li32(2, 0x10F0'7F80);
  a.li32(3, 0x0102'0304);
  a.li32(4, 0x2020'0190);
  a.li32(5, 0xFF01'0101);
  a << padd(6, 2, 4, 1);          // byte-wise modulo add
  a << padd(8, 2, 4, 1, 1);       // byte-wise unsigned saturate
  a << psub(10, 2, 4, 2, 3);      // half-wise signed saturate (.s)
  a << pcmp(12, 2, 4);
  a << prot_i(14, 2, 8);          // rotate pair left 8
  a.li32(16, 0x1234'5678);
  a << punpk(18, 16, 1);          // bytes -> halves
  a << ppack(20, 26, 2, 16);      // ppack.16: top byte of each 32-bit field of r2:r3
  a << or_i(22, 0, 2) << pmul(24, 2, 22);
  a.label("halt");
  a.br("halt");
  run_quacker(sys, a);
  auto pr = [&](int n) { return u64(sys.quacker.r[n]) << 32 | sys.quacker.r[n + 1]; };
  CHECK_EQ(pr(6), 0x3010'8010'0003'0405ull);
  CHECK_EQ(pr(8), 0x30FF'80FF'FF03'0405ull);
  CHECK_EQ(pr(10), 0xF0D0'7DF0'0201'0203ull);
  // pcmp: hi 0x10F07F80 < 0x20200190, lo 0x01020304 < 0xFF010101 -> bit 5, complements in 8-11.
  CHECK_EQ(sys.quacker.r[12], (1u << 5) | (1u << 8) | (1u << 10) | (1u << 11));
  CHECK_EQ(pr(14), 0xF07F'8001'0203'0410ull);
  CHECK_EQ(pr(18), 0x0012'0034'0056'0078ull);
  CHECK_EQ(pr(20), 0x0000'0000'0000'1001ull);
  CHECK_EQ(pr(24), 0x10F0'7F80'0102'0304ull * 2);
}

TEST(m88k_cache_and_latency_model) {
  // The same loop from SDRAM (cache misses on the first pass) and with a divide in it.
  auto run = [](bool with_div) {
    Lion sys;
    Asm a(0x4000);
    a.label("start");
    a.li32(2, 0x0010'0000);
    a << or_i(3, 0, 256) << or_i(5, 0, 7);
    a.label("loop");
    a << ld(4, 2, 0) << addu_i(2, 2, 32);
    if (with_div) a << divu(6, 4, 5);
    a << subu_i(3, 3, 1);
    a.bcnd(kNe0, 3, "loop");
    a.label("halt");
    a.br("halt");
    auto code = a.finish();
    std::copy(code.begin(), code.end(), sys.bus.host_ptr(0x4000));
    sys.quacker.reset(0x4000);
    sys.quacker.running = true;
    u64 cycles = 0;
    while (sys.quacker.pc != a.addr("halt") && cycles < 1'000'000) cycles += sys.quacker.run(1);
    return std::pair{cycles, sys.quacker.dcache.misses};
  };
  auto [plain, misses] = run(false);
  auto [div, misses2] = run(true);
  CHECK_EQ(misses, 256u);  // one miss per 32-byte line
  CHECK(plain >= 256 * (2 + lion::Quacker::kMemLatency));  // 4 instructions dual-issue in ~2 cycles
  CHECK(div >= plain + 256 * lion::Quacker::kDivExtra);
  CHECK_EQ(misses2, 256u);
}

// Table 5-2 of the MC88110 User's Manual: 8-bit saturation examples.
TEST(m88k_padds_matches_manual_table) {
  struct Row { u8 s1, s2, padd, u, s, us; };
  const Row rows[] = {
      {0x00, 0x80, 0x80, 0x80, 0x80, 0x00}, {0x55, 0x55, 0xAA, 0xAA, 0x7F, 0xAA}, {0x55, 0xFF, 0x54, 0xFF, 0x54, 0x54},
      {0x7F, 0x7F, 0xFE, 0xFE, 0x7F, 0xFE}, {0x7F, 0x80, 0xFF, 0xFF, 0xFF, 0x00}, {0x80, 0x80, 0x00, 0xFF, 0x80, 0x00},
      {0x80, 0xFF, 0x7F, 0xFF, 0x80, 0x7F}, {0xAA, 0x7F, 0x29, 0xFF, 0x29, 0xFF}, {0xAA, 0xAA, 0x54, 0xFF, 0x80, 0x54},
      {0xFF, 0x80, 0x7F, 0xFF, 0x80, 0x7F}, {0xFF, 0xFF, 0xFE, 0xFF, 0xFE, 0xFE}, {0x55, 0x7F, 0xD4, 0xD4, 0x7F, 0xD4},
  };
  for (const Row& row : rows) {
    Lion sys;
    Asm a(0x4000);
    a.label("start");
    a << or_i(2, 0, row.s1) << or_i(4, 0, row.s2);  // low byte of the odd registers r3/r5 stays 0
    a << or_i(3, 0, row.s1) << or_i(5, 0, row.s2);
    a << padd(6, 2, 4, 1, 0) << padd(8, 2, 4, 1, 1) << padd(10, 2, 4, 1, 3) << padd(12, 2, 4, 1, 2);
    a.label("halt");
    a.br("halt");
    run_quacker(sys, a);
    auto& q = sys.quacker.r;
    CHECK_EQ(q[7] & 0xFF, u32(row.padd));
    CHECK_EQ(q[9] & 0xFF, u32(row.u));
    CHECK_EQ(q[11] & 0xFF, u32(row.s));
    CHECK_EQ(q[13] & 0xFF, u32(row.us));
  }
}

TEST(m88k_sfu2_exceptions) {
  Lion sys;
  Asm a(0x4000);
  a.label("start");
  a.li32(2, 0x5000);
  a << stcr(2, 7);
  a << padd(7, 2, 4, 1);  // odd destination pair -> graphics exception
  a.label("halt");
  a.br("halt");
  Asm h(0x5000 + lion::Quacker::kVecSfu2 * 8);
  h << or_i(20, 0, 116) << rte();
  auto hc = h.finish();
  std::copy(hc.begin(), hc.end(), sys.bus.host_ptr(0x5000 + lion::Quacker::kVecSfu2 * 8));
  run_quacker(sys, a);
  CHECK_EQ(sys.quacker.r[20], 116u);
}
