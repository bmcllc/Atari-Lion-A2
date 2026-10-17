#include "system.h"

#include <algorithm>

#include "bootrom.h"
#include "tyke/firmware.h"

namespace lion {

Lion::Lion()
    : vi(bus, pic), tuffy(bus, pic), quacker(bus), qctrl(quacker, pic), qve(bus), spike(bus, pic), tyke(bus, pic), cd(bus, pic) {
  bus.map_device(map::kQuackerCtrl, 0x100, &qctrl);
  bus.map_device(map::kQve, 0x100, &qve);
  bus.map_device(map::kSpike, 0x100, &spike);
  bus.map_device(map::kTyke, 0x10000, &tyke);
  bus.map_device(map::kVi, 0x100, &vi);
  bus.map_device(map::kPic, 0x100, &pic);
  bus.map_device(map::kPad, 0x100, &pads);
  bus.map_device(map::kCd, 0x100, &cd);
  pads.port[0].connected = true;
  const std::vector<u8> rom = build_boot_rom();
  std::copy(rom.begin(), rom.end(), bus.rom().begin());
}

void Lion::power_on() {
  tuffy.reset(map::kRomBase + Tuffy::kVecReset, Tuffy::kMsrIp | Tuffy::kMsrMe);
  quacker.running = false;
  tyke.write32(Tyke::kCtrlBase, 0, Master::Dma, nullptr);
}

void Lion::boot(const LionImage& image) {
  for (const auto& seg : image.segments) {
    if (u8* p = bus.host_range(seg.addr, u32(seg.data.size()))) {
      std::copy(seg.data.begin(), seg.data.end(), p);
    } else {
      for (size_t i = 0; i < seg.data.size(); ++i) bus.write8(seg.addr + u32(i), seg.data[i], Master::Dma);
    }
  }
  tuffy.reset(image.entry, kBootMsr);
  tuffy.gpr[1] = kBootStack;
  quacker.running = false;
  tyke.load_program(tyke_mixer_firmware());  // what the boot ROM uploads
}

bool Lion::boot_disc(std::vector<u8> iso, std::string* error) {
  cd.insert(std::move(iso));
  power_on();
  std::vector<u8> file;
  if (!cd.read_file("BOOT.LION", &file)) {
    if (error) *error = "disc has no BOOT.LION";
    return false;
  }
  LionImage image;
  return image.parse(file, error);
}

void Lion::run_slice(u64 tuffy_cycles) {
  tuffy.run(tuffy_cycles);

  // Quacker at 100 MHz, Spike at 90 MHz, relative to the 500 MHz Tuffy.
  quacker_acc_ += tuffy_cycles * kQuackerHz;
  const u64 q = quacker_acc_ / kTuffyHz;
  quacker_acc_ %= kTuffyHz;
  if (q) {
    quacker.run(q);
    qve.tick(q);
  }

  spike_acc_ += tuffy_cycles * kSpikeHz;
  const u64 s = spike_acc_ / kTuffyHz;
  spike_acc_ %= kTuffyHz;
  if (s) spike.tick(s);

  bus_acc_ += tuffy_cycles * kBusHz;
  const u64 b = bus_acc_ / kTuffyHz;
  bus_acc_ %= kTuffyHz;
  if (b) cd.tick(b);

  tyke_acc_ += tuffy_cycles * Tyke::kHz;
  const u64 t = tyke_acc_ / kTuffyHz;
  tyke_acc_ %= kTuffyHz;
  if (t) tyke.run(t);
}

void Lion::run_frame() {
  const int lines = vi.lines_per_frame();
  const u64 frame_hz = u64(kFrameRate) * u64(lines);
  for (int line = 0; line < lines; ++line) {
    vi.set_line(line);
    line_acc_ += kTuffyHz;
    u64 cycles = line_acc_ / frame_hz;
    line_acc_ %= frame_hz;
    while (cycles) {
      const u64 n = std::min(cycles, kSliceTuffy);
      run_slice(n);
      cycles -= n;
    }
  }

  tyke.drain(audio);
}

}  // namespace lion
