#include <cmath>

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
  // 4410 frames expected; the firmware must not fall behind the 44.1 kHz clock,
  // and must leave headroom for bus waits.
  CHECK(out.size() / 2 >= 4405);
  CHECK(sys.tyke.idle_cycles > sys.tyke.total_cycles / 6);
}

TEST(tyke_clock_absorbs_bus_stalls) {
  // Tuffy streams through memory (every access misses) while all 24 voices fetch
  // samples over the bus. Waits must not push the DSP's clock, and with it its
  // bus reservations, ahead of the rest of the system.
  Lion sys;
  for (int i = 0; i < 1024; ++i) store_be16(sys.bus.host_ptr(0x9000 + u32(i) * 2), u16(i));
  ppcasm::Asm t(0x2000);
  t.li32(3, 0x0040'0000);
  t.label("loop");
  t << ppcasm::lwz(5, 0, 3) << ppcasm::addi(3, 3, 32);
  t.b("loop");
  auto code = t.finish();
  std::copy(code.begin(), code.end(), sys.bus.host_ptr(0x2000));
  sys.tuffy.reset(0x2000, Lion::kBootMsr);
  sys.tyke.load_program(lion::tyke_mixer_firmware());
  for (int ch = 0; ch < 24; ++ch) channel(sys, ch, 0x9000, 1024, 0, 0x10000, 0x0008'0008, 3);
  for (int f = 1; f <= 30; ++f) {
    sys.run_frame();
    if (f > 1) CHECK_EQ(sys.audio.size(), 2u * 735);  // exactly one frame of 44.1 kHz stereo
    sys.audio.clear();
    const u64 now = u64(f) * lion::kBusHz / lion::kFrameRate;
    const u64 busy_until = now + sys.bus.acquire(lion::Master::Dma, now, 0);
    CHECK(busy_until <= now + 1000);  // within a few µs of real time
  }
  CHECK(sys.bus.contention[size_t(lion::Master::Dma)] > 0);  // the voices did wait for the bus
}

namespace {
// One voice playing a looped square wave of the given period (in samples).
void square_voice(Lion& sys, u32 period) {
  for (u32 i = 0; i < 1024; ++i) store_be16(sys.bus.host_ptr(0x9000 + i * 2), u16((i % period) < period / 2 ? 8000 : -8000));
  sys.tyke.load_program(lion::tyke_mixer_firmware());
  sys.tyke.run(100);  // past the firmware's start-up, which resets the effects
  channel(sys, 0, 0x9000, 1024 / period * period, 0, 0x10000, 0x0100'0100, 3);
}
double rms(const std::vector<s16>& v, size_t from) {
  double acc = 0;
  for (size_t i = from; i < v.size(); i += 2) acc += double(v[i]) * v[i];
  return std::sqrt(acc / double((v.size() - from) / 2));
}
}  // namespace

TEST(tyke_scope_holds_last_output) {
  Lion sys;
  square_voice(sys, 64);
  sys.tyke.run(Tyke::kHz / 20);
  std::vector<s16> out;
  sys.tyke.drain(out);
  const u32 pos = sys.tyke.data(lion::kScopePos);
  // The frame just before the write position is the last one sent to the DAC.
  const u32 last = (pos + lion::kScopeFrames - 1) % lion::kScopeFrames;
  CHECK_EQ(s16(sys.tyke.data(lion::kScopeBase + 2 * last)), out[out.size() - 2]);
  CHECK_EQ(s16(sys.tyke.data(lion::kScopeBase + 2 * last + 1)), out.back());
}

TEST(tyke_bass_and_treble) {
  // Bass lifts a low square wave; treble lifts a high one; neither touches the other much.
  auto level = [](u32 period, u32 bass, u32 treble) {
    Lion sys;
    square_voice(sys, period);
    reg(sys, lion::kMixBass, bass);
    reg(sys, lion::kMixTreble, treble);
    sys.tyke.run(Tyke::kHz / 10);
    std::vector<s16> out;
    sys.tyke.drain(out);
    return rms(out, out.size() / 2);
  };
  const double low = level(512, 0, 0), high = level(8, 0, 0);
  CHECK(level(512, 0x100, 0) > low * 1.3);
  CHECK(level(8, 0, 0x100) > high * 1.3);
  CHECK(level(8, 0x100, 0) < high * 1.15);
}

TEST(tyke_echo_repeats_after_delay) {
  Lion sys;
  for (u32 i = 0; i < 64; ++i) store_be16(sys.bus.host_ptr(0x9000 + i * 2), u16(i < 32 ? 10000 : 0));
  sys.tyke.load_program(lion::tyke_mixer_firmware());
  sys.tyke.run(100);
  reg(sys, lion::kMixEcho, 0x100);
  channel(sys, 0, 0x9000, 64, 0, 0x10000, 0x0100'0100, 1);  // one short burst
  sys.tyke.run(Tyke::kHz / 5);
  std::vector<s16> out;
  sys.tyke.drain(out);
  // Silence after the burst, then its echo one delay line later.
  auto at = [&](size_t frame) { return out[frame * 2]; };
  size_t start = 0;
  while (start < out.size() / 2 && at(start) == 0) ++start;
  CHECK(at(start + 200) == 0);
  CHECK(at(start + lion::kEchoWords + 2) > 1000);
}

TEST(tyke_effects_fit_with_24_voices) {
  Lion sys;
  for (int i = 0; i < 1024; ++i) store_be16(sys.bus.host_ptr(0x9000 + u32(i) * 2), u16(i));
  sys.tyke.load_program(lion::tyke_mixer_firmware());
  sys.tyke.run(100);
  reg(sys, lion::kMixBass, 0x80);
  reg(sys, lion::kMixTreble, 0x80);
  reg(sys, lion::kMixEcho, 0x80);
  for (int ch = 0; ch < 24; ++ch) channel(sys, ch, 0x9000, 1024, 0, 0x10000, 0x0008'0008, 3);
  sys.tyke.run(Tyke::kHz / 10);
  std::vector<s16> out;
  sys.tyke.drain(out);
  CHECK(out.size() / 2 >= 4405);
  CHECK(sys.tyke.idle_cycles > sys.tyke.total_cycles / 12);  // still some headroom
}
