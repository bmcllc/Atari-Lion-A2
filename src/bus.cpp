#include "bus.h"

namespace lion {

bool g_verbose = false;

Bus::Bus()
    : sdram_(map::kSdramSize),
      sgram_(map::kSgramSize),
      // The 16 KB SRAM occupies one 64 KB page; the remaining 48 KB are unused.
      sram_(64u << 10),
      rom_(map::kRomSize) {
  map_ram(map::kSdramBase, sdram_, true);
  map_ram(map::kSgramBase, sgram_, true);
  map_ram(map::kSramBase, sram_, true);
  map_ram(map::kRomBase, rom_, false);
}

void Bus::map_ram(u32 base, std::vector<u8>& mem, bool writable) {
  for (u32 off = 0; off < mem.size(); off += 0x10000) {
    rpages_[(base + off) >> 16] = mem.data() + off;
    if (writable) wpages_[(base + off) >> 16] = mem.data() + off;
  }
}

void Bus::map_device(u32 base, u32 size, MmioDevice* dev) { windows_.push_back({base, size, dev}); }

MmioDevice* Bus::find(u32 addr, u32* offset) {
  for (const Window& w : windows_) {
    if (addr - w.base < w.size) {
      *offset = addr - w.base;
      return w.dev;
    }
  }
  return nullptr;
}

u32 Bus::mmio_read(u32 addr, Master who) {
  u32 off;
  if (MmioDevice* dev = find(addr & ~3u, &off)) {
    bool st = false;
    u32 v = dev->read32(off, who, &st);
    if (st) stall = true;
    return v;
  }
  if (unmapped_warnings_ < 32) {
    ++unmapped_warnings_;
    LION_WARN("unmapped read @%08X by %s", addr, who == Master::Tuffy ? "Tuffy" : "Quacker");
  }
  return 0;
}

void Bus::mmio_write(u32 addr, u32 v, Master who) {
  u32 off;
  if (MmioDevice* dev = find(addr & ~3u, &off)) {
    bool st = false;
    dev->write32(off, v, who, &st);
    if (st) stall = true;
    return;
  }
  if (unmapped_warnings_ < 32) {
    ++unmapped_warnings_;
    LION_WARN("unmapped write @%08X = %08X by %s", addr, v, who == Master::Tuffy ? "Tuffy" : "Quacker");
  }
}

u8* Bus::host_range(u32 addr, u32 len) {
  if (len == 0) return host_ptr(addr);
  u8* first = host_ptr(addr);
  u8* last = host_ptr(addr + len - 1);
  if (!first || !last || last - first != std::ptrdiff_t(len - 1)) return nullptr;
  return first;
}

// Sub-word MMIO accesses act on the containing big-endian word.
u8 Bus::read8(u32 addr, Master who) {
  if (u8* p = rpages_[addr >> 16]) return p[addr & 0xFFFF];
  return u8(mmio_read(addr, who) >> (24 - 8 * (addr & 3)));
}

u16 Bus::read16(u32 addr, Master who) {
  if ((addr & 0xFFFF) <= 0xFFFE) {
    if (u8* p = rpages_[addr >> 16]) return load_be16(p + (addr & 0xFFFF));
  } else {
    return u16(read8(addr, who) << 8 | read8(addr + 1, who));
  }
  return u16(mmio_read(addr, who) >> (16 - 8 * (addr & 2)));
}

u32 Bus::read32(u32 addr, Master who) {
  if ((addr & 0xFFFF) <= 0xFFFC) {
    if (u8* p = rpages_[addr >> 16]) return load_be32(p + (addr & 0xFFFF));
  } else {
    return u32(read16(addr, who)) << 16 | read16(addr + 2, who);
  }
  return mmio_read(addr, who);
}

u64 Bus::read64(u32 addr, Master who) { return u64(read32(addr, who)) << 32 | read32(addr + 4, who); }

void Bus::write8(u32 addr, u8 v, Master who) {
  if (u8* p = wpages_[addr >> 16]) {
    p[addr & 0xFFFF] = v;
  } else if (!rpages_[addr >> 16]) {
    mmio_write(addr, v, who);
  }
}

void Bus::write16(u32 addr, u16 v, Master who) {
  if ((addr & 0xFFFF) > 0xFFFE) {
    write8(addr, u8(v >> 8), who);
    write8(addr + 1, u8(v), who);
  } else if (u8* p = wpages_[addr >> 16]) {
    store_be16(p + (addr & 0xFFFF), v);
  } else if (!rpages_[addr >> 16]) {
    mmio_write(addr, v, who);
  }
}

void Bus::write32(u32 addr, u32 v, Master who) {
  if ((addr & 0xFFFF) > 0xFFFC) {
    write16(addr, u16(v >> 16), who);
    write16(addr + 2, u16(v), who);
  } else if (u8* p = wpages_[addr >> 16]) {
    store_be32(p + (addr & 0xFFFF), v);
  } else if (!rpages_[addr >> 16]) {
    mmio_write(addr, v, who);
  }
}

void Bus::write64(u32 addr, u64 v, Master who) {
  write32(addr, u32(v >> 32), who);
  write32(addr + 4, u32(v), who);
}

}  // namespace lion
