// Tuffy MMU: block address translation (4 IBAT + 4 DBAT pairs), segment
// registers and the hashed page table described by SDR1, fronted by a
// direct-mapped software TLB per access type.
#include "tuffy/ppc.h"

namespace lion {

namespace {

constexpr u32 kSprSdr1 = 25;
constexpr u32 kSprIbat0U = 528, kSprDbat0U = 536;

// DSISR / SRR1 fault bits.
constexpr u32 kFaultNoTranslation = 0x4000'0000;
constexpr u32 kFaultProtection = 0x0800'0000;
constexpr u32 kFaultNoExecute = 0x1000'0000;
constexpr u32 kFaultStore = 0x0200'0000;

// PP protection check. key: 0 = supervisor-style, 1 = user-style.
bool pp_allows(u32 pp, u32 key, bool write) {
  if (key == 0) return !(write && pp == 3);
  switch (pp) {
    case 0: return false;
    case 1: case 3: return !write;
    default: return true;
  }
}

}  // namespace

void Tuffy::flush_tlb() {
  for (auto* t : {&tlb_read_, &tlb_write_, &tlb_exec_})
    for (TlbEntry& e : *t) e.tag = ~0u;
}

void Tuffy::update_mode(u32 old_msr) {
  // Protection depends on MSR[PR]; cached translations must be redone when it changes.
  if ((old_msr ^ msr) & kMsrPr) flush_tlb();
}

bool Tuffy::translate_bat(u32 ea, Access acc, Translation* out, u32* fault_bits, bool* hit) {
  *hit = false;
  const u32 base = acc == Access::Exec ? kSprIbat0U : kSprDbat0U;
  const bool user = msr & kMsrPr;
  for (u32 i = 0; i < 4; ++i) {
    const u32 upper = spr[base + i * 2], lower = spr[base + i * 2 + 1];
    if (!(user ? (upper & 1) : (upper & 2))) continue;  // Vp / Vs
    const u32 bl = (upper >> 2) & 0x7FF;
    const u32 mask = ~((bl << 17) | 0x1FFFF);
    if ((ea & mask) != (upper & 0xFFFE'0000 & mask)) continue;
    *hit = true;
    const u32 pp = lower & 3;
    const bool write = acc == Access::Write;
    if (pp == 0 || (write && (pp & 1))) {
      *fault_bits = kFaultProtection | (write ? kFaultStore : 0);
      return false;
    }
    out->pa = (lower & 0xFFFE'0000 & mask) | (ea & ~mask);
    out->inhibited = lower & 0x20;  // WIMG.I
    return true;
  }
  return false;
}

bool Tuffy::translate_page(u32 ea, Access acc, Translation* out, u32* fault_bits) {
  const bool write = acc == Access::Write;
  const u32 seg = sr[ea >> 28];
  if (seg & 0x8000'0000) {  // T=1: direct-store segments are not supported
    *fault_bits = kFaultNoTranslation | (write ? kFaultStore : 0);
    return false;
  }
  if (acc == Access::Exec && (seg & 0x1000'0000)) {  // N: no-execute segment
    *fault_bits = kFaultNoExecute;
    return false;
  }
  const u32 key = (msr & kMsrPr) ? (seg >> 29) & 1 : (seg >> 30) & 1;
  const u32 vsid = seg & 0x00FF'FFFF;
  const u32 page_index = (ea >> 12) & 0xFFFF;
  const u32 api = page_index >> 10;
  const u32 sdr1 = spr[kSprSdr1];
  const u32 htaborg = sdr1 & 0xFFFF'0000, htabmask = sdr1 & 0x1FF;

  for (u32 h = 0; h < 2; ++h) {
    u32 hash = (vsid & 0x7FFFF) ^ page_index;
    if (h) hash = ~hash;
    const u32 pteg = (htaborg & 0xFE00'0000) |
                     ((((htaborg >> 16) & 0x1FF) | ((hash >> 10) & htabmask)) << 16) | ((hash & 0x3FF) << 6);
    for (u32 i = 0; i < 8; ++i) {
      const u32 pte_addr = pteg + i * 8;
      u8* p = bus_.host_range(pte_addr, 8);
      if (!p) continue;
      const u32 w0 = load_be32(p);
      if (!(w0 & 0x8000'0000) || ((w0 >> 6) & 1) != h || ((w0 >> 7) & 0xFF'FFFF) != vsid || (w0 & 0x3F) != api)
        continue;
      u32 w1 = load_be32(p + 4);
      if (!pp_allows(w1 & 3, key, write)) {
        *fault_bits = kFaultProtection | (write ? kFaultStore : 0);
        return false;
      }
      w1 |= 0x100;               // R
      if (write) w1 |= 0x80;     // C
      store_be32(p + 4, w1);
      out->pa = (w1 & 0xFFFF'F000) | (ea & 0xFFF);
      out->inhibited = w1 & 0x20;
      return true;
    }
  }
  *fault_bits = kFaultNoTranslation | (write ? kFaultStore : 0);
  return false;
}

bool Tuffy::translate(u32 ea, Access acc, Translation* out, u32* fault_bits) {
  const bool enabled = acc == Access::Exec ? (msr & kMsrIr) : (msr & kMsrDr);
  if (!enabled) {
    out->pa = ea;
    out->inhibited = (ea >> 24) == 0x1F;  // MMIO region is never cached
    return true;
  }
  auto& tlb = acc == Access::Exec ? tlb_exec_ : acc == Access::Write ? tlb_write_ : tlb_read_;
  TlbEntry& e = tlb[(ea >> 12) % kTlbSize];
  const u32 tag = (ea & 0xFFFF'F000) | 1;
  if (e.tag == tag) {
    out->pa = e.pa_page | (ea & 0xFFF);
    out->inhibited = e.inhibited;
    return true;
  }
  bool bat_hit;
  bool ok = translate_bat(ea, acc, out, fault_bits, &bat_hit);
  if (!bat_hit) ok = translate_page(ea, acc, out, fault_bits);
  if (!ok) return false;
  e.tag = tag;
  e.pa_page = out->pa & 0xFFFF'F000;
  e.inhibited = out->inhibited;
  return true;
}

void Tuffy::data_fault(u32 ea, u32 bits, bool write) {
  dar = ea;
  dsisr = bits | (write ? kFaultStore : 0);
  fault_ = true;
  exception(kVecDsi, 0, pc);
}

bool Tuffy::fetch(u32 ea, u32* op) {
  Translation t;
  u32 bits = 0;
  if (!translate(ea, Access::Exec, &t, &bits)) {
    next_pc_ = pc;
    exception(kVecIsi, bits & 0x5800'0000, pc);
    return false;
  }
  const u8* p = bus_.host_ptr(t.pa);
  if (!p || (ea & 3)) {
    next_pc_ = pc;
    exception(kVecIsi, kFaultNoTranslation, pc);
    return false;
  }
  account_fetch(t.pa, t.inhibited);
  *op = load_be32(p);
  return true;
}

u64 Tuffy::load(u32 ea, u32 size) {
  if (fault_) return 0;
  // Accesses that cross a page boundary are split into bytes.
  if ((msr & kMsrDr) && size > 1 && (ea & 0xFFF) + size > 0x1000) {
    u64 v = 0;
    for (u32 i = 0; i < size; ++i) v = v << 8 | load(ea + i, 1);
    return v;
  }
  Translation t;
  u32 bits = 0;
  if (!translate(ea, Access::Read, &t, &bits)) {
    data_fault(ea, bits, false);
    return 0;
  }
  account_data(t.pa, size, false, t.inhibited);
  switch (size) {
    case 1: return bus_.read8(t.pa, Master::Tuffy);
    case 2: return bus_.read16(t.pa, Master::Tuffy);
    case 4: return bus_.read32(t.pa, Master::Tuffy);
    default: return bus_.read64(t.pa, Master::Tuffy);
  }
}

void Tuffy::store(u32 ea, u64 v, u32 size) {
  if (fault_) return;
  if ((msr & kMsrDr) && size > 1 && (ea & 0xFFF) + size > 0x1000) {
    for (u32 i = 0; i < size; ++i) store(ea + i, u8(v >> (8 * (size - 1 - i))), 1);
    return;
  }
  Translation t;
  u32 bits = 0;
  if (!translate(ea, Access::Write, &t, &bits)) {
    data_fault(ea, bits, true);
    return;
  }
  account_data(t.pa, size, true, t.inhibited);
  switch (size) {
    case 1: bus_.write8(t.pa, u8(v), Master::Tuffy); break;
    case 2: bus_.write16(t.pa, u16(v), Master::Tuffy); break;
    case 4: bus_.write32(t.pa, u32(v), Master::Tuffy); break;
    default: bus_.write64(t.pa, v, Master::Tuffy); break;
  }
}

void Tuffy::zero_block(u32 ea) {
  ea &= ~31u;
  Translation t;
  u32 bits = 0;
  if (!translate(ea, Access::Write, &t, &bits)) {
    data_fault(ea, bits, true);
    return;
  }
  account_zero(t.pa, t.inhibited);
  if (u8* p = bus_.host_range(t.pa, 32)) {
    std::memset(p, 0, 32);
  } else {
    for (u32 i = 0; i < 32; i += 4) bus_.write32(t.pa + i, 0, Master::Tuffy);
  }
}

// ---- Timing -------------------------------------------------------------

u32 Tuffy::miss_penalty(u32 pa) {
  // L1 miss: look in the backside L2, then go to SDRAM over the bus.
  if ((spr[kSprL2cr] & kL2crEnable) && l2.access(pa)) return kL2Latency;
  return kMemLatency + bus_wait(Bus::kLineFillCycles);
}

void Tuffy::account_fetch(u32 pa, bool inhibited) {
  const u32 line = pa >> 5;
  if (line == last_fetch_line_) return;  // sequential fetch within the same line
  last_fetch_line_ = line;
  if (inhibited || !(spr[kSprHid0] & kHid0Ice)) {
    penalty(kUncachedLatency + bus_wait(Bus::kSingleCycles));
    return;
  }
  if (!l1i.access(pa)) penalty(miss_penalty(pa));
}

void Tuffy::account_data(u32 pa, u32 size, bool write, bool inhibited) {
  if (inhibited || !(spr[kSprHid0] & kHid0Dce)) {
    // MMIO goes over the I/O side and does not occupy the SDRAM.
    penalty(kUncachedLatency + ((pa >> 24) == 0x1F ? 0 : bus_wait(Bus::kSingleCycles)));
    return;
  }
  // Write-back, write-allocate L1 with a store buffer: store misses cost half.
  u32 p = 0;
  if (!l1d.access(pa)) p = miss_penalty(pa);
  if (size > 1 && ((pa + size - 1) >> 5) != (pa >> 5) && !l1d.access(pa + size - 1)) p += miss_penalty(pa + size - 1);
  penalty(write ? p / 2 : p);
}

void Tuffy::account_zero(u32 pa, bool inhibited) {
  // dcbz allocates the line without reading memory.
  if (!inhibited && (spr[kSprHid0] & kHid0Dce)) l1d.access(pa);
}

}  // namespace lion
