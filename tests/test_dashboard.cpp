// The boot ROM's intro and dashboard, driven through the pad like a player would.
#include <algorithm>
#include <cstring>
#include <memory>

#include "bootrom.h"
#include "demo_rom.h"
#include "iso9660.h"
#include "music_disc.h"
#include "ima_adpcm.h"
#include "rom/assets.h"
#include "rom/blobs.h"
#include "test.h"
#include "tyke/firmware.h"

using lion::Lion;
using lion::Pads;
namespace br = lion::bootrom;

namespace {

u32 ram(Lion& sys, u32 off) { return load_be32(sys.bus.host_ptr(br::kRam + off)); }
std::string ram_str(Lion& sys, u32 off) { return reinterpret_cast<const char*>(sys.bus.host_ptr(br::kRam + off)); }

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

std::unique_ptr<Lion> dev_unit() {
  auto sys = std::make_unique<Lion>();
  sys->pic.straps = lion::Pic::kStrapFastBoot;
  return sys;
}

// A development unit sitting in the dashboard, optionally with a disc in the drive.
std::unique_ptr<Lion> in_dashboard(std::vector<u8> disc = {}) {
  auto sys = dev_unit();
  sys->power_on();
  run_until(*sys, 30, [&] { return ram(*sys, br::kScene) == br::kSceneDashboard; });
  if (!disc.empty()) {
    sys->cd.insert(std::move(disc));
    run_until(*sys, 90, [&] { return ram(*sys, br::kDiscState) == br::kReady; });
  }
  return sys;
}

void select_card(Lion& sys, u32 card) {
  while (ram(sys, br::kSelection) > card) press(sys, Pads::kLeft);
  while (ram(sys, br::kSelection) < card) press(sys, Pads::kRight);
}

int voices_on(Lion& sys, int first, int last) {
  int n = 0;
  for (int ch = first; ch <= last; ++ch) n += sys.tyke.data(lion::kMixChannels + 8 * u32(ch) + 5) & 1;
  return n;
}

}  // namespace

TEST(dashboard_intro_roar_claws_and_crown) {
  Lion sys;
  sys.power_on();
  int peak = 0;
  for (int f = 0; f < 100; ++f) {  // the roar
    sys.run_frame();
    for (s16 v : sys.audio) peak = std::max(peak, std::abs(int(v)));
    sys.audio.clear();
  }
  CHECK(peak > 4000);
  CHECK_EQ(ram(sys, br::kScene), br::kSceneIntro);
  std::vector<u32> px;
  // The plate is whole before the claws...
  CHECK(sys.vi.scanout(px));
  const u32 plate = px[200 * 640 + 140];
  CHECK(plate != 0);
  // ...the claws strike...
  int claws = 0;
  for (int f = 100; f < 130; ++f) {
    sys.run_frame();
    sys.audio.clear();
    claws = std::max(claws, voices_on(sys, 3, 3));
  }
  CHECK_EQ(claws, 1);
  // ...and the crowned Lion stands where the plate was.
  for (int f = 130; f < 290; ++f) sys.run_frame();
  sys.audio.clear();
  CHECK(sys.vi.scanout(px));
  CHECK(((px[215 * 640 + 320] >> 16) & 0xFF) > 0x80);  // red emblem
  const u32 crown = px[100 * 640 + 320];                // gold crown above it
  CHECK(((crown >> 16) & 0xFF) > 0x80 && ((crown >> 8) & 0xFF) > 0x50);
  CHECK(run_until(sys, 200, [&] { return ram(sys, br::kScene) == br::kSceneDashboard; }) <= 200);
  CHECK_EQ(sys.tuffy.illegal_count, 0u);
}

TEST(dashboard_runs_at_full_frame_rate) {
  auto sys = in_dashboard();
  for (int f = 0; f < 10; ++f) sys->run_frame();
  const u32 before = sys->tuffy.gpr[24];  // the ROM's frame counter
  for (int f = 0; f < 60; ++f) sys->run_frame();
  CHECK(sys->tuffy.gpr[24] - before >= 58);
}

TEST(dashboard_navigation_and_panels) {
  auto sys = in_dashboard();
  CHECK_EQ(ram(*sys, br::kDiscState), br::kNoDisc);
  CHECK_EQ(ram(*sys, br::kSelection), br::kItemPlay);
  for (int i = 0; i < 6; ++i) press(*sys, Pads::kRight);  // stops at the last card
  CHECK_EQ(ram(*sys, br::kSelection), br::kItemSystem);
  press(*sys, Pads::kA);
  CHECK_EQ(ram(*sys, br::kSubmenu), br::kSubSystem);
  press(*sys, Pads::kLeft);  // ignored while a panel is open
  CHECK_EQ(ram(*sys, br::kSelection), br::kItemSystem);
  press(*sys, Pads::kB);
  CHECK_EQ(ram(*sys, br::kSubmenu), br::kSubNone);
  for (auto [card, sub] : {std::pair{br::kItemTyke, br::kSubTyke}, std::pair{br::kItemTheme, br::kSubTheme},
                           std::pair{br::kItemMusic, br::kSubMusic}}) {
    select_card(*sys, card);
    press(*sys, Pads::kA);
    CHECK_EQ(ram(*sys, br::kSubmenu), sub);
    press(*sys, Pads::kB);
  }
  // No disc: PLAY does nothing but complain.
  select_card(*sys, br::kItemPlay);
  press(*sys, Pads::kA);
  CHECK_EQ(ram(*sys, br::kLaunch), 0u);
  CHECK(sys->in_boot_rom());
  CHECK_EQ(sys->tuffy.illegal_count, 0u);
}

TEST(dashboard_tyke_settings) {
  auto sys = in_dashboard();
  select_card(*sys, br::kItemTyke);
  press(*sys, Pads::kA);
  CHECK_EQ(sys->tyke.data(lion::kMixMaster), 0x100u);
  press(*sys, Pads::kRight);  // already at 100%
  CHECK_EQ(sys->tyke.data(lion::kMixMaster), 0x100u);
  press(*sys, Pads::kLeft);
  CHECK_EQ(sys->tyke.data(lion::kMixMaster), 0xF0u);
  press(*sys, Pads::kDown);
  for (int i = 0; i < 3; ++i) press(*sys, Pads::kRight);
  CHECK_EQ(sys->tyke.data(lion::kMixBass), 48u);
  press(*sys, Pads::kDown);
  press(*sys, Pads::kDown);
  press(*sys, Pads::kRight);
  CHECK_EQ(sys->tyke.data(lion::kMixEcho), 16u);
  // Holding a direction repeats it.
  sys->pads.port[0].buttons = Pads::kRight;
  for (int f = 0; f < 60; ++f) sys->run_frame();
  sys->pads.port[0].buttons = 0;
  sys->run_frame();
  CHECK(sys->tyke.data(lion::kMixEcho) >= 0x80u);
  // A plays the jingle on the music voices.
  press(*sys, Pads::kA);
  CHECK(run_until(*sys, 30, [&] { return voices_on(*sys, 0, 1) > 0; }) <= 30);
}

TEST(dashboard_theme_stream_decodes_exactly) {
  // The default theme streams the dashboard music from the ROM: what Tuffy decodes
  // into the ring is the reference IMA ADPCM decode, sample for sample.
  auto sys = in_dashboard();
  CHECK_EQ(ram(*sys, br::kTheme), 0u);
  CHECK(run_until(*sys, 30, [&] { return voices_on(*sys, 16, 17) == 2; }) <= 30);
  for (int f = 0; f < 120; ++f) sys->run_frame();
  const auto ref = ima::decode_stereo(lion::rom::theme_ladp(0).data);
  const u32 next_block = ram(*sys, 0x1E4);  // kTsBlock: the ring holds the 32 blocks before it
  CHECK(next_block > 32);
  const u8* ring = sys->bus.host_ptr(0x00B0'0000);
  int mismatches = 0;
  for (u32 b = next_block - 32; b < next_block; ++b)
    for (u32 f = 0; f < ima::kBlockFrames; ++f) {
      const u32 frame = b * ima::kBlockFrames + f, at = frame % 65536;
      for (u32 c = 0; c < 2; ++c) mismatches += s16(load_be16(ring + 4 * at + 2 * c)) != ref[2 * frame + c];
    }
  CHECK_EQ(mismatches, 0);
  int peak = 0;
  for (s16 v : sys->audio) peak = std::max(peak, std::abs(int(v)));
  CHECK(peak > 2000);
}

TEST(dashboard_sound_themes) {
  auto sys = in_dashboard();
  select_card(*sys, br::kItemTheme);
  press(*sys, Pads::kA);
  for (int i = 0; i < 3; ++i) press(*sys, Pads::kDown);
  press(*sys, Pads::kA);
  CHECK_EQ(ram(*sys, br::kTheme), 3u);
  CHECK_EQ(voices_on(*sys, 16, 17), 0);  // the stream stopped
  // NEON SAVANNA has a kick on every beat: the kick voice keeps restarting.
  int kicks = 0;
  u32 last = 0;
  for (int f = 0; f < 200; ++f) {
    sys->run_frame();
    const u32 pos = sys->tyke.data(lion::kMixChannels + 6);
    if (pos < last) ++kicks;
    last = pos;
  }
  CHECK(kicks >= 4);
  // SILENCE: every music voice goes quiet.
  press(*sys, Pads::kDown);
  press(*sys, Pads::kDown);
  press(*sys, Pads::kA);
  CHECK_EQ(ram(*sys, br::kTheme), 5u);
  for (int f = 0; f < 30; ++f) sys->run_frame();
  CHECK_EQ(voices_on(*sys, 0, 17), 0);
  // Back to the first theme: the stream starts again.
  for (int i = 0; i < 5; ++i) press(*sys, Pads::kUp);
  press(*sys, Pads::kA);
  CHECK(run_until(*sys, 30, [&] { return voices_on(*sys, 16, 17) == 2; }) <= 30);
}

TEST(dashboard_cd_player_plays_wav_tracks) {
  const std::vector<u8> pride = music::track_pride();
  auto sys = in_dashboard(music::build_disc());
  CHECK_EQ(ram(*sys, br::kDiscState), br::kReady);
  CHECK_EQ(ram(*sys, br::kHasGame), 0u);
  CHECK_EQ(ram(*sys, br::kTrackCount), 3u);
  CHECK(ram_str(*sys, br::kTracks + 8) == "PRIDE");
  CHECK(ram_str(*sys, br::kTracks + br::kTrackSize + 8) == "SAVANNA");
  select_card(*sys, br::kItemMusic);
  press(*sys, Pads::kA);
  press(*sys, Pads::kA);
  CHECK(run_until(*sys, 90, [&] { return ram(*sys, br::kMusicState) == br::kMusicPlaying; }) <= 90);
  // Past the button sound, the output is the track itself, sample for sample.
  for (int f = 0; f < 40; ++f) sys->run_frame();
  sys->audio.clear();
  for (int f = 0; f < 10; ++f) sys->run_frame();
  const std::vector<s16> out = sys->audio;
  const s16* src = reinterpret_cast<const s16*>(pride.data() + 44);
  const size_t src_frames = (pride.size() - 44) / 4;
  bool found = false;
  for (size_t p = 0; p + 64 < src_frames && !found; ++p) {
    bool same = true;
    for (size_t k = 0; k < 64 && same; ++k)
      same = src[2 * (p + k)] == out[2 * k] && src[2 * (p + k) + 1] == out[2 * k + 1];
    found = same;
  }
  CHECK(found);
  CHECK(ram(*sys, br::kMusicElapsed) > 44100u / 2);
  // Pause freezes the clock; stop brings the theme back.
  press(*sys, Pads::kA);
  CHECK_EQ(ram(*sys, br::kMusicState), br::kMusicPaused);
  const u32 paused_at = ram(*sys, br::kMusicElapsed);
  for (int f = 0; f < 20; ++f) sys->run_frame();
  CHECK_EQ(ram(*sys, br::kMusicElapsed), paused_at);
  press(*sys, Pads::kX);
  CHECK_EQ(ram(*sys, br::kMusicState), br::kMusicStopped);
  CHECK(run_until(*sys, 60, [&] { return voices_on(*sys, 16, 17) == 2; }) <= 60);  // the theme's stream
  CHECK_EQ(sys->tuffy.illegal_count, 0u);
}

TEST(dashboard_cd_player_next_track_and_formats) {
  // Short tracks: mono 22.05 kHz then stereo 32 kHz; the player moves on by itself.
  auto mono = std::vector<s16>(22050 / 4, 1000);
  std::vector<s16> stereo(32000 / 4 * 2, -1000);
  auto sys = in_dashboard(music::build_disc({{"ONE.WAV", music::wav(mono, 22050, 1)}, {"TWO.WAV", music::wav(stereo, 32000, 2)}}));
  CHECK_EQ(ram(*sys, br::kTrackCount), 2u);
  select_card(*sys, br::kItemMusic);
  press(*sys, Pads::kA);
  press(*sys, Pads::kA);
  CHECK(run_until(*sys, 120, [&] { return ram(*sys, br::kMusicTrack) == 1; }) <= 120);
  CHECK(run_until(*sys, 120, [&] { return ram(*sys, br::kMusicState) == br::kMusicStopped; }) <= 120);
  CHECK_EQ(ram(*sys, br::kMusicError), 0u);
}

TEST(dashboard_cd_player_rejects_bad_wav) {
  std::vector<u8> junk(4096, 0x55);
  auto sys = in_dashboard(music::build_disc({{"JUNK.WAV", junk}}));
  select_card(*sys, br::kItemMusic);
  press(*sys, Pads::kA);
  press(*sys, Pads::kA);
  CHECK(run_until(*sys, 60, [&] { return ram(*sys, br::kMusicError) != 0; }) <= 60);
  CHECK_EQ(ram(*sys, br::kMusicState), br::kMusicStopped);
  CHECK_EQ(sys->tuffy.illegal_count, 0u);
}

TEST(dashboard_hot_inserted_disc_boots) {
  auto sys = in_dashboard();
  sys->cd.insert(demo::build_disc());
  CHECK(run_until(*sys, 90, [&] { return ram(*sys, br::kDiscState) == br::kReady; }) <= 90);
  CHECK(ram_str(*sys, br::kLabel) == "APEX_CUBE");
  CHECK_EQ(ram(*sys, br::kHasGame), 1u);
  press(*sys, Pads::kA);
  CHECK_EQ(ram(*sys, br::kLaunch), 1u);
  CHECK(run_until(*sys, 200, [&] { return !sys->in_boot_rom(); }) <= 200);
  // The game took over with a quiet mixer and its own textured cube.
  for (int f = 0; f < 60; ++f) sys->run_frame();
  CHECK_EQ(sys->tuffy.gpr[19], demo::kTexturedMode);
  CHECK_EQ(sys->tuffy.illegal_count, 0u);
  CHECK_EQ(sys->quacker.illegal_count, 0u);
}

TEST(dashboard_disc_without_game) {
  auto sys = dev_unit();
  std::string err;
  CHECK(!sys->boot_disc(iso9660::build("EMPTY", {{"README.TXT", std::vector<u8>(10, 1)}}), &err));
  CHECK(run_until(*sys, 120, [&] { return ram(*sys, br::kScene) == br::kSceneDashboard; }) <= 120);
  CHECK(run_until(*sys, 90, [&] { return ram(*sys, br::kDiscState) == br::kReady; }) <= 90);
  CHECK_EQ(ram(*sys, br::kHasGame), 0u);
  press(*sys, Pads::kA);
  CHECK_EQ(ram(*sys, br::kLaunch), 0u);
  CHECK(sys->in_boot_rom());
}

TEST(dashboard_broken_game_is_not_retried) {
  // BOOT.LION is there but its header is wrong: the loader gives up once and the
  // dashboard refuses it until the disc is changed; the music on it still plays.
  auto sys = dev_unit();
  std::vector<u8> bad(64, 0xEE);
  sys->cd.insert(music::build_disc({{"BOOT.LION", bad}, {"SONG.WAV", music::wav(std::vector<s16>(4410, 500), 44100, 1)}}));
  sys->power_on();
  CHECK(run_until(*sys, 200, [&] {
          return ram(*sys, br::kScene) == br::kSceneDashboard && ram(*sys, br::kDiscState) == br::kReady;
        }) <= 200);
  CHECK_EQ(ram(*sys, br::kHasGame), 1u);
  CHECK_EQ(ram(*sys, br::kBootFailed), 1u);
  CHECK_EQ(ram(*sys, br::kTrackCount), 1u);
  press(*sys, Pads::kA);
  CHECK_EQ(ram(*sys, br::kLaunch), 0u);
  sys->cd.eject();
  for (int f = 0; f < 5; ++f) sys->run_frame();
  CHECK_EQ(ram(*sys, br::kBootFailed), 0u);
}

TEST(retail_unit_boots_disc_after_intro) {
  Lion sys;
  std::string err;
  CHECK(sys.boot_disc(demo::build_disc(), &err));
  const int frames = run_until(sys, 600, [&] { return !sys.in_boot_rom(); });
  CHECK(frames > int(lion::rom::kIntroFrames) - 10 && frames <= 600);
  CHECK_EQ(sys.tuffy.illegal_count, 0u);
}

TEST(retail_unit_opens_player_for_music_disc) {
  Lion sys;
  sys.cd.insert(music::build_disc());
  sys.power_on();
  CHECK(run_until(sys, 600, [&] { return ram(sys, br::kScene) == br::kSceneDashboard; }) <= 600);
  CHECK_EQ(ram(sys, br::kSelection), br::kItemMusic);
  CHECK(sys.in_boot_rom());
}
