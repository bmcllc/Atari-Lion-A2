#include "demo_rom.h"
#include "iso9660.h"
#include "test.h"

using lion::Cdrom;
using lion::Lion;

namespace {

std::vector<u8> pattern(size_t n, u8 seed) {
  std::vector<u8> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = u8(seed + i * 7);
  return v;
}

void cd_write(Lion& sys, u32 off, u32 v) { sys.bus.write32(lion::map::kCd + off, v, lion::Master::Tuffy); }
u32 cd_read(Lion& sys, u32 off) { return sys.bus.read32(lion::map::kCd + off, lion::Master::Tuffy); }

}  // namespace

TEST(cdrom_iso9660_lookup) {
  Lion sys;
  sys.cd.insert(iso9660::build("TEST", {{"README.TXT", pattern(100, 1)}, {"DATA.BIN", pattern(5000, 2)}}));
  u32 lba, size;
  CHECK(sys.cd.find_file("data.bin", &lba, &size));
  CHECK_EQ(lba, iso9660::kFirstFileLba + 1);
  CHECK_EQ(size, 5000u);
  std::vector<u8> out;
  CHECK(sys.cd.read_file("README.TXT", &out));
  CHECK(out == pattern(100, 1));
  CHECK(!sys.cd.find_file("MISSING.BIN", &lba, &size));
}

TEST(cdrom_dma_read_timing_and_interrupt) {
  Lion sys;
  const auto data = pattern(3 * Cdrom::kSectorSize, 9);
  sys.cd.insert(iso9660::build("TEST", {{"DATA.BIN", data}}));
  CHECK(cd_read(sys, 0x00) & Cdrom::kPresent);
  cd_write(sys, 0x08, iso9660::kFirstFileLba);
  cd_write(sys, 0x0C, 3);
  cd_write(sys, 0x10, 0x0050'0000);
  cd_write(sys, 0x04, Cdrom::kRead);
  CHECK(cd_read(sys, 0x00) & Cdrom::kBusy);

  // Seek (>= 80 ms) plus 3 sectors at 12x (~1.1 ms each).
  sys.cd.tick(u64(0.079 * lion::kBusHz));
  CHECK_EQ(cd_read(sys, 0x18), 0u);
  sys.cd.tick(u64(0.010 * lion::kBusHz));
  CHECK(cd_read(sys, 0x00) & Cdrom::kDone);
  CHECK(!(cd_read(sys, 0x00) & Cdrom::kBusy));
  CHECK(sys.pic.pending() & lion::Pic::kCd);
  CHECK(std::equal(data.begin(), data.end(), sys.bus.host_ptr(0x0050'0000)));

  // Sequential read right after: no long seek.
  cd_write(sys, 0x04, Cdrom::kAck);
  cd_write(sys, 0x08, iso9660::kFirstFileLba + 3);
  cd_write(sys, 0x0C, 1);
  cd_write(sys, 0x04, Cdrom::kRead);
  sys.cd.tick(u64(0.082 * lion::kBusHz));
  CHECK(cd_read(sys, 0x00) & Cdrom::kDone);
}

TEST(cdrom_errors) {
  Lion sys;
  cd_write(sys, 0x0C, 1);
  cd_write(sys, 0x04, Cdrom::kRead);  // no disc
  CHECK(cd_read(sys, 0x00) & Cdrom::kError);
  sys.cd.insert(iso9660::build("TEST", {}));
  cd_write(sys, 0x04, Cdrom::kAck);
  cd_write(sys, 0x08, 1000);
  cd_write(sys, 0x04, Cdrom::kRead);  // beyond the disc
  CHECK(cd_read(sys, 0x00) & Cdrom::kError);
}

TEST(cdrom_boot_rom_loads_disc) {
  Lion sys;
  std::string err;
  CHECK(sys.boot_disc(demo::build_disc(), &err));
  CHECK(sys.in_boot_rom());
  int frames = 0;
  while (sys.in_boot_rom() && frames < 120) {
    sys.run_frame();
    ++frames;
  }
  CHECK(!sys.in_boot_rom());
  CHECK(frames > 5);  // seeking and reading the disc takes real time
  CHECK(sys.tyke.running());
  // The segments of BOOT.LION are in memory exactly as in the image.
  const lion::LionImage img = [] {
    lion::LionImage i;
    std::vector<u8> f;
    Lion probe;
    probe.cd.insert(demo::build_disc());
    probe.cd.read_file("BOOT.LION", &f);
    i.parse(f, nullptr);
    return i;
  }();
  for (const auto& seg : img.segments)
    CHECK(std::equal(seg.data.begin(), seg.data.end(), sys.bus.host_ptr(seg.addr)));
  CHECK_EQ(sys.tuffy.illegal_count, 0u);
}

TEST(cdrom_boot_rom_error_screens) {
  auto screen = [](Lion& sys) {
    for (int i = 0; i < 30; ++i) sys.run_frame();
    std::vector<u32> px;
    sys.vi.scanout(px);
    return px.empty() ? 0u : px[px.size() / 2];
  };
  Lion empty;
  empty.power_on();
  CHECK_EQ(screen(empty), 0x0000FFu);  // blue: insert a disc
  Lion bad;
  std::string err;
  CHECK(!bad.boot_disc(iso9660::build("EMPTY", {{"README.TXT", pattern(10, 1)}}), &err));
  CHECK(err.find("BOOT.LION") != std::string::npos);
  CHECK_EQ(screen(bad), 0xFF0000u);  // red: no bootable program
}
