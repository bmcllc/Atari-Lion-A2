// The boot ROM's intro and dashboard, driven through the pad like a player would.
#include <algorithm>
#include <cstring>

#include "bootrom.h"
#include "rom/assets.h"
#include "demo_rom.h"
#include "iso9660.h"
#include "test.h"

using lion::Lion;
using lion::Pads;
namespace br = lion::bootrom;

namespace {

u32 ram(Lion& sys, u32 off) { return load_be32(sys.bus.host_ptr(br::kRam + off)); }

// Runs until `done` or `limit` frames; returns the frames used (limit + 1 on timeout).
template <typename F>
int run_until(Lion& sys, int limit, F done) {
  for (int f = 0; f < limit; ++f) {
    if (done()) return f;
    sys.run_frame();
    sys.audio.clear();
  }
  return done() ? limit : limit + 1;
}

void press(Lion& sys, u32 buttons) {
  for (int i = 0; i < 2; ++i) {
    sys.pads.port[0].buttons = buttons;
    sys.run_frame();
  }
  for (int i = 0; i < 3; ++i) {
    sys.pads.port[0].buttons = 0;
    sys.run_frame();
  }
  sys.audio.clear();
}

Lion* dev_unit() {
  auto* sys = new Lion;
  sys->pic.straps = lion::Pic::kStrapFastBoot;
  return sys;
}

}  // namespace

TEST(dashboard_intro_plays_jingle_then_menu) {
  Lion sys;
  sys.power_on();
  int peak = 0;
  for (int f = 0; f < 150; ++f) {
    sys.run_frame();
    for (s16 v : sys.audio) peak = std::max(peak, std::abs(int(v)));
    sys.audio.clear();
  }
  CHECK_EQ(ram(sys, br::kScene), br::kSceneIntro);
  CHECK(peak > 4000);  // the roar and the bells
  // The emblem sits on a red glow in the middle of the screen.
  std::vector<u32> px;
  CHECK(sys.vi.scanout(px));
  CHECK_EQ(sys.vi.width(), 640);
  const u32 c = px[190 * 640 + 320];
  CHECK(((c >> 16) & 0xFF) > 0x80);
  CHECK(run_until(sys, 200, [&] { return ram(sys, br::kScene) == br::kSceneDashboard; }) <= 200);
  CHECK_EQ(sys.tuffy.illegal_count, 0u);
}

TEST(dashboard_runs_at_full_frame_rate) {
  std::unique_ptr<Lion> sys(dev_unit());
  sys->power_on();
  CHECK(run_until(*sys, 10, [&] { return ram(*sys, br::kScene) == br::kSceneDashboard; }) <= 10);
  for (int f = 0; f < 10; ++f) sys->run_frame();
  const u32 before = sys->tuffy.gpr[24];  // the ROM's frame counter
  for (int f = 0; f < 60; ++f) sys->run_frame();
  CHECK(sys->tuffy.gpr[24] - before >= 58);
}

TEST(dashboard_navigation) {
  std::unique_ptr<Lion> sys(dev_unit());
  sys->power_on();
  run_until(*sys, 30, [&] { return ram(*sys, br::kScene) == br::kSceneDashboard; });
  CHECK_EQ(ram(*sys, br::kDiscState), br::kNoDisc);
  CHECK_EQ(ram(*sys, br::kSelection), br::kItemDisc);
  press(*sys, Pads::kRight);
  CHECK_EQ(ram(*sys, br::kSelection), br::kItemAudio);
  press(*sys, Pads::kRight);
  press(*sys, Pads::kRight);  // already at the end
  CHECK_EQ(ram(*sys, br::kSelection), br::kItemSystem);
  press(*sys, Pads::kA);
  CHECK_EQ(ram(*sys, br::kSubmenu), br::kSubSystem);
  press(*sys, Pads::kLeft);  // ignored while a panel is open
  CHECK_EQ(ram(*sys, br::kSelection), br::kItemSystem);
  press(*sys, Pads::kB);
  CHECK_EQ(ram(*sys, br::kSubmenu), br::kSubNone);
  press(*sys, Pads::kLeft);
  press(*sys, Pads::kA);
  CHECK_EQ(ram(*sys, br::kSubmenu), br::kSubAudio);
  // The sound test replays the jingle: voices start within a few frames.
  int voices = 0;
  for (int f = 0; f < 10; ++f) {
    sys->run_frame();
    voices = 0;
    for (int ch = 0; ch < lion::Tyke::kChannels; ++ch) voices += sys->tyke.data(0x40 + 8 * u32(ch) + 5) & 1;
    if (voices) break;
  }
  CHECK(voices > 0);
  // No disc: choosing DISC does nothing but complain.
  press(*sys, Pads::kB);
  press(*sys, Pads::kLeft);
  press(*sys, Pads::kA);
  CHECK_EQ(ram(*sys, br::kLaunch), 0u);
  CHECK(sys->in_boot_rom());
  CHECK_EQ(sys->tuffy.illegal_count, 0u);
}

TEST(dashboard_hot_inserted_disc_boots) {
  std::unique_ptr<Lion> sys(dev_unit());
  sys->power_on();
  run_until(*sys, 30, [&] { return ram(*sys, br::kScene) == br::kSceneDashboard; });
  sys->cd.insert(demo::build_disc());
  CHECK(run_until(*sys, 60, [&] { return ram(*sys, br::kDiscState) == br::kReady; }) <= 60);
  CHECK(std::strcmp(reinterpret_cast<const char*>(sys->bus.host_ptr(br::kRam + br::kLabel)), "APEX_CUBE") == 0);
  press(*sys, Pads::kA);
  CHECK_EQ(ram(*sys, br::kLaunch), 1u);
  CHECK(run_until(*sys, 200, [&] { return !sys->in_boot_rom(); }) <= 200);
  // The game took over with a quiet mixer and its own textured cube.
  for (int f = 0; f < 60; ++f) sys->run_frame();
  CHECK_EQ(sys->tuffy.gpr[19], demo::kTexturedMode);
  CHECK_EQ(sys->tuffy.illegal_count, 0u);
  CHECK_EQ(sys->quacker.illegal_count, 0u);
}

TEST(dashboard_rejects_unbootable_disc) {
  std::unique_ptr<Lion> sys(dev_unit());
  std::string err;
  CHECK(!sys->boot_disc(iso9660::build("EMPTY", {{"README.TXT", std::vector<u8>(10, 1)}}), &err));
  CHECK(run_until(*sys, 120, [&] { return ram(*sys, br::kScene) == br::kSceneDashboard; }) <= 120);
  CHECK_EQ(ram(*sys, br::kDiscState), br::kUnplayable);
  press(*sys, Pads::kA);
  CHECK_EQ(ram(*sys, br::kLaunch), 0u);
  CHECK(sys->in_boot_rom());
}

TEST(retail_unit_boots_disc_after_intro) {
  Lion sys;
  std::string err;
  CHECK(sys.boot_disc(demo::build_disc(), &err));
  const int frames = run_until(sys, 400, [&] { return !sys.in_boot_rom(); });
  CHECK(frames > int(lion::rom::kIntroFrames) - 10 && frames <= 400);
  CHECK_EQ(sys.tuffy.illegal_count, 0u);
}
