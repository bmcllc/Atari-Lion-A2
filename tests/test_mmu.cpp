#include "test.h"

using namespace ppcasm;
using lion::Lion;
using lion::Tuffy;

namespace {

constexpr u32 kHtab = 0x0020'0000;  // 64 KB hashed page table

// Inserts a PTE mapping `ea` (segment VSID `vsid`) to `pa` with protection `pp`.
void map_page(Lion& sys, u32 vsid, u32 ea, u32 pa, u32 pp) {
  const u32 page_index = (ea >> 12) & 0xFFFF;
  const u32 hash = (vsid & 0x7FFFF) ^ page_index;
  const u32 pteg = kHtab | ((hash & 0x3FF) << 6);  // HTABMASK = 0
  for (u32 i = 0; i < 8; ++i) {
    u8* p = sys.bus.host_ptr(pteg + i * 8);
    if (load_be32(p) & 0x8000'0000) continue;
    store_be32(p, 0x8000'0000u | vsid << 7 | (page_index >> 10));
    store_be32(p + 4, (pa & 0xFFFF'F000) | pp);
    return;
  }
}

u32 pte_word1(Lion& sys, u32 vsid, u32 ea) {
  const u32 page_index = (ea >> 12) & 0xFFFF;
  const u32 pteg = kHtab | ((((vsid & 0x7FFFF) ^ page_index) & 0x3FF) << 6);
  for (u32 i = 0; i < 8; ++i) {
    const u8* p = sys.bus.host_ptr(pteg + i * 8);
    if ((load_be32(p) & 0x8000'0000) && ((load_be32(p) >> 7) & 0xFFFFFF) == vsid) return load_be32(p + 4);
  }
  return 0;
}

// DSI handler: r20 = DAR, r21 = DSISR, skip the faulting instruction.
void install_dsi_handler(Lion& sys) {
  Asm h(0x300);
  h << mfspr(20, 19) << mfspr(21, 18) << mfspr(22, 26) << addi(22, 22, 4) << mtspr(26, 22) << rfi();
  auto c = h.finish();
  std::copy(c.begin(), c.end(), sys.bus.host_ptr(0x300));
}

}  // namespace

TEST(mmu_bat_translation) {
  Lion sys;
  Asm a(0x2000);
  // DBAT0: EA 0x8000_0000, 128 KB, supervisor valid -> PA 0x0010_0000, read/write.
  a.li32(3, 0x8000'0000 | 0x2);
  a.li32(4, 0x0010'0000 | 0x2);
  a << mtspr(537, 4) << mtspr(536, 3);
  a.li32(5, lion::Lion::kBootMsr | Tuffy::kMsrDr);
  a << mtmsr(5);
  a.li32(6, 0x8000'0010);
  a.li32(7, 0xCAFE'F00D);
  a << stw(7, 0, 6) << lwz(8, 4, 6);
  a.label("halt");
  a.b("halt");
  store_be32(sys.bus.host_ptr(0x0010'0014), 0x1234'5678);
  test::run_tuffy(sys, a);
  CHECK_EQ(load_be32(sys.bus.host_ptr(0x0010'0010)), 0xCAFE'F00Du);
  CHECK_EQ(sys.tuffy.gpr[8], 0x1234'5678u);
}

TEST(mmu_page_table_and_rc_bits) {
  Lion sys;
  constexpr u32 kVsid = 0x123;
  map_page(sys, kVsid, 0x4000'5000, 0x0030'0000, 2);
  Asm a(0x2000);
  a.li32(3, kHtab);
  a << mtspr(25, 3);  // SDR1
  a.li32(3, kVsid);
  a.li32(4, 0x4000'0000);
  a << X(31, 3, 0, 4, 242);  // mtsrin r3, r4
  a.li32(5, lion::Lion::kBootMsr | Tuffy::kMsrDr);
  a << mtmsr(5);
  a.li32(6, 0x4000'5008);
  a << lwz(9, 0, 6) << li(7, 99) << stw(7, 4, 6);
  a.label("halt");
  a.b("halt");
  store_be32(sys.bus.host_ptr(0x0030'0008), 0xABCD);
  test::run_tuffy(sys, a);
  CHECK_EQ(sys.tuffy.gpr[9], 0xABCDu);
  CHECK_EQ(load_be32(sys.bus.host_ptr(0x0030'000C)), 99u);
  const u32 w1 = pte_word1(sys, kVsid, 0x4000'5000);
  CHECK(w1 & 0x100);  // referenced
  CHECK(w1 & 0x080);  // changed
}

TEST(mmu_dsi_on_missing_page) {
  Lion sys;
  install_dsi_handler(sys);
  Asm a(0x2000);
  a.li32(3, kHtab);
  a << mtspr(25, 3);
  a.li32(5, lion::Lion::kBootMsr | Tuffy::kMsrDr);
  a << mtmsr(5);
  a.li32(6, 0x5000'1234);
  a << li(9, 7) << lwz(9, 0, 6);  // faults: r9 must keep 7
  a << li(10, 1);
  a.label("halt");
  a.b("halt");
  test::run_tuffy(sys, a);
  CHECK_EQ(sys.tuffy.gpr[20], 0x5000'1234u);
  CHECK(sys.tuffy.gpr[21] & 0x4000'0000u);   // no translation
  CHECK(!(sys.tuffy.gpr[21] & 0x0200'0000u));  // it was a load
  CHECK_EQ(sys.tuffy.gpr[9], 7u);
  CHECK_EQ(sys.tuffy.gpr[10], 1u);
}

TEST(mmu_protection_fault_on_store) {
  Lion sys;
  install_dsi_handler(sys);
  constexpr u32 kVsid = 0x77;
  map_page(sys, kVsid, 0x6000'0000, 0x0040'0000, 3);  // read-only
  Asm a(0x2000);
  a.li32(3, kHtab);
  a << mtspr(25, 3);
  a.li32(3, kVsid);
  a.li32(4, 0x6000'0000);
  a << X(31, 3, 0, 4, 242);
  a.li32(5, lion::Lion::kBootMsr | Tuffy::kMsrDr);
  a << mtmsr(5);
  a << lwz(9, 0, 4) << li(7, 5) << stw(7, 0, 4);
  a.label("halt");
  a.b("halt");
  test::run_tuffy(sys, a);
  CHECK(sys.tuffy.gpr[21] & 0x0800'0000u);  // protection
  CHECK(sys.tuffy.gpr[21] & 0x0200'0000u);  // store
  CHECK_EQ(load_be32(sys.bus.host_ptr(0x0040'0000)), 0u);
}

TEST(mmu_isi_on_unmapped_fetch) {
  Lion sys;
  Asm h(0x400);
  h << mfspr(20, 27) << li(21, 1);  // SRR1, then stop in real mode
  h.label("halt");
  h.b("halt");
  Asm a(0x2000);
  a.li32(3, kHtab);
  a << mtspr(25, 3);
  a.li32(4, 0x7000'0000);
  a << mtspr(26, 4);  // SRR0 = unmapped
  a.li32(5, lion::Lion::kBootMsr | Tuffy::kMsrIr);
  a << mtspr(27, 5) << rfi();
  a.label("unused");
  a.b("unused");
  auto code = a.finish();
  std::copy(code.begin(), code.end(), sys.bus.host_ptr(0x2000));
  test::run_tuffy(sys, h);
  sys.tuffy.reset(0x2000, lion::Lion::kBootMsr);
  for (int i = 0; i < 200 && sys.tuffy.pc != h.addr("halt"); ++i) sys.tuffy.run(1);
  CHECK_EQ(sys.tuffy.gpr[21], 1u);
  CHECK(sys.tuffy.gpr[20] & 0x4000'0000u);
}

TEST(cache_model_charges_misses) {
  // Stream over 4 KB (fits L1) and over 2 MB (misses L1 and L2) with the same instruction count.
  auto run = [](u32 bytes) {
    Lion sys;
    Asm a(0x2000);
    a.li32(3, 0x0040'0000);
    a.li32(4, bytes / 32);
    a << mtctr(4);
    a.label("loop");
    a << lwz(5, 0, 3) << addi(3, 3, 32);
    a.bdnz("loop");
    a.li32(3, 0x0040'0000);
    a.li32(4, bytes / 32);
    a << mtctr(4);
    a.label("loop2");
    a << lwz(5, 0, 3) << addi(3, 3, 32);
    a.bdnz("loop2");
    a.label("halt");
    a.b("halt");
    test::run_tuffy(sys, a, 10'000'000);
    return std::pair{sys.tuffy.stall_cycles, sys.tuffy.l1d.misses};
  };
  auto [small_stall, small_miss] = run(4096);
  auto [big_stall, big_miss] = run(2u << 20);
  CHECK_EQ(small_miss, 128u);              // only the first pass misses
  CHECK_EQ(big_miss, 2u * (2u << 20) / 32);  // both passes miss L1
  // Second pass over 2 MB misses L2 too (1 MB, 2-way): every access pays the SDRAM latency.
  CHECK(big_stall >= 2ull * (2u << 20) / 32 * Tuffy::kMemLatency);
  CHECK(small_stall < 128ull * Tuffy::kMemLatency + 1000);
}
