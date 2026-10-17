#include "test.h"
#include "tyke/firmware.h"
#include "tykeasm.h"

using lion::Lion;
using lion::Tyke;

namespace {

void reg(Lion& sys, u32 word, u32 v) { sys.bus.write32(lion::map::kTyke + word * 4, v, lion::Master::Tuffy); }
void channel(Lion& sys, int ch, u32 start, u32 len, u32 loop, u32 pitch, u32 vol, u32 ctrl) {
  const u32 base = 0x40 + u32(ch) * 8;
  reg(sys, base + 0, start);
  reg(sys, base + 1, len);
  reg(sys, base + 2, loop);
  reg(sys, base + 3, pitch);
  reg(sys, base + 4, vol);
  reg(sys, base + 5, ctrl);
}

}  // namespace

TEST(tyke_isa_basics) {
  using namespace tykeasm;
  Lion sys;
  Asm a;
  a << movi(1, 7) << movi(2, -3) << mul(3, 1, 2);  // -21
  a.li32(4, 0x1234'5678);
  a << clr(0) << mac(0, 1, 1) << mac(0, 2, 2) << mva(5, 0, 0);  // 49 + 9
  a << movi(6, 40000) << sat16(7, 6);                              // 32767
  a << movi(8, 0) << movi(9, 5);
  a.label("loop");
  a << addi(8, 8, 2) << addi(9, 9, -1);
  a.bne(9, 0, "loop");
  a << st(8, 100, 0) << ld(10, 100, 0);
  a.call("sub");
  a << halt();
  a.label("sub");
  a << movi(11, 99) << ret();
  sys.tyke.load_program(a.finish());
  sys.tyke.run(1000);
  CHECK(!sys.tyke.running());
  CHECK_EQ(sys.tyke.data(100), 10u);
  CHECK_EQ(sys.bus.read32(lion::map::kTyke + 400, lion::Master::Tuffy), 10u);  // host view of data RAM
  CHECK_EQ(sys.tyke.illegal_count, 0u);
}

TEST(tyke_firmware_mixes_channel) {
  Lion sys;
  for (int i = 0; i < 16; ++i) store_be16(sys.bus.host_ptr(0x9000 + u32(i) * 2), u16(1000 * (i + 1)));
  sys.tyke.load_program(lion::tyke_mixer_firmware());
  sys.tyke.run(Tyke::kHz / 1000);  // let the firmware initialise
  std::vector<s16> skip;
  sys.tyke.drain(skip);
  channel(sys, 0, 0x9000, 16, 0, 0x10000, 0x0100'0080, 1);  // L unity, R half
  sys.tyke.run(Tyke::kHz / 100);                              // 10 ms = 441 frames
  std::vector<s16> out;
  sys.tyke.drain(out);
  CHECK(out.size() >= 2 * 430);
  // Find the first frame of the sample.
  size_t i = 0;
  while (i < out.size() && out[i] == 0) i += 2;
  CHECK(i + 32 <= out.size());
  for (int k = 0; k < 16 && i + 2 * size_t(k) + 1 < out.size(); ++k) {
    CHECK_EQ(out[i + 2 * size_t(k)], s16(1000 * (k + 1)));
    CHECK_EQ(out[i + 2 * size_t(k) + 1], s16(500 * (k + 1)));
  }
  // One-shot: the channel stops itself at the end.
  CHECK_EQ(sys.tyke.data(0x40 + 5) & 1, 0u);
  CHECK_EQ(out.back(), 0);
}

TEST(tyke_firmware_loops_and_pitch) {
  Lion sys;
  for (int i = 0; i < 4; ++i) store_be16(sys.bus.host_ptr(0x9000 + u32(i) * 2), u16(100 * (i + 1)));
  sys.tyke.load_program(lion::tyke_mixer_firmware());
  channel(sys, 3, 0x9000, 4, 2, 0x8000, 0x0100'0100, 3);  // half speed, loop from sample 2
  sys.tyke.run(Tyke::kHz / 50);
  std::vector<s16> out;
  sys.tyke.drain(out);
  size_t i = 0;
  while (i < out.size() && out[i] == 0) i += 2;
  // Each sample is played twice; after 4 samples the loop repeats 3, 4.
  const s16 want[] = {100, 100, 200, 200, 300, 300, 400, 400, 300, 300, 400, 400, 300};
  for (size_t k = 0; k < std::size(want); ++k) CHECK_EQ(out[i + 2 * k], want[k]);
  CHECK(sys.tyke.data(0x40 + 3 * 8 + 5) & 1);  // still playing
}

TEST(tyke_24_channels_fit_in_real_time) {
  Lion sys;
  for (int i = 0; i < 1024; ++i) store_be16(sys.bus.host_ptr(0x9000 + u32(i) * 2), u16(i));
  sys.tyke.load_program(lion::tyke_mixer_firmware());
  for (int ch = 0; ch < 24; ++ch) channel(sys, ch, 0x9000, 1024, 0, 0x10000, 0x0008'0008, 3);
  sys.tyke.run(Tyke::kHz / 10);  // 100 ms
  std::vector<s16> out;
  sys.tyke.drain(out);
  // 4410 frames expected; the firmware must not fall behind the 44.1 kHz clock.
  CHECK(out.size() / 2 >= 4405);
  CHECK(sys.tyke.idle_cycles > 0);
}
