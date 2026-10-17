// Boot ROM: the start-up intro.
//
//   0.3 s  a "JAGUAR" plate glows in the dark; the lion roars and the plate shakes
//   1.9 s  three claw strokes rake across it and tear it into four pieces that fly apart
//   2.2 s  flash: the Lion emblem spins in, the crown drops onto its mane
//   3.6 s  ATARI LION A2 and the tagline; fade out at 5.4 s
// Every frame reads its parameters from tables built by rom/assets.cpp.
#include "rom/builder.h"

namespace lion::romgen {

void RomBuilder::intro() {
  using namespace rom;
  constexpr u32 kPlateMode = Spike::kTexture | Spike::kBilinear | Spike::kBlend;
  constexpr int16_t kStripBytes = int16_t(kStripWords * 4), kSlashBytes = int16_t(kSlashWords * 4);

  a_.label("intro");
  ram_set(kScene, kSceneIntro);
  a_.bl("jingle");
  a_ << li(17, 0);  // frame
  a_.la(18, "intro_tab");
  a_.label("intro_loop");
  a_.bl("read_pad");
  unless_pressed(Pads::kStart | Pads::kA, "intro_noskip");
  a_ << cmpwi(0, 17, 320);
  a_.bge("intro_noskip");
  a_ << li(17, 320);
  a_.label("intro_noskip");
  a_.bl("cd_async");
  a_ << slwi(0, 17, 6) << add(19, 18, 0);  // row (16 floats)
  static_assert(kIntroColumns == 16);

  a_ << li(3, 3) << li(4, 0);
  a_.bl("begin_frame");
  mode(kMAdd);
  // Red glow behind the plate while the lion roars.
  a_ << lfs(5, 60, 19) << fcmpu(0, 5, 31);
  a_.ble("intro_no_red");
  load(3, 0xFFC01010);
  a_ << fmr(1, 5);
  a_.bl("shade");
  a_ << lis(4, 0xFF00) << lfs(5, 60, 19);
  lf(6, 330.0f);
  a_ << fmuls(3, 5, 6);
  lf(6, 170.0f);
  a_ << fmuls(4, 5, 6);
  lf(1, 320), lf(2, 200);
  a_.bl("fan");
  a_.label("intro_no_red");
  // Glow behind the Lion mark: a wide red halo and a hot core.
  for (auto [rx, ry, color] : {std::tuple{330.0f, 250.0f, 0xFFB01414u}, std::tuple{130.0f, 125.0f, 0xFFFF6A2Au}}) {
    load(3, color);
    a_ << lfs(1, 28, 19);
    a_.bl("shade");
    a_ << lis(4, 0xFF00) << lfs(5, 28, 19);
    lf(6, rx);
    a_ << fmuls(3, 5, 6);
    lf(6, ry);
    a_ << fmuls(4, 5, 6);
    lf(1, 320), lf(2, 215);
    a_.bl("fan");
  }

  // The plate, whole or in pieces.
  a_ << cmpwi(0, 17, int16_t(kPlateFirst));
  a_.blt("intro_no_plate");
  a_ << cmpwi(0, 17, int16_t(kPlateLast));
  a_.bge("intro_no_plate");
  set_texture(kJaguarTex, kJaguarLog2W, kJaguarLog2H);
  mode(kPlateMode);
  a_.la(20, "plate_tab");
  a_ << addi(0, 17, -int16_t(kPlateFirst)) << mulli(0, 0, int16_t(kPlateStrips * kStripBytes)) << add(20, 20, 0);
  for (u32 s = 0; s < kPlateStrips; ++s) {
    const std::string hidden = L("strip_hidden");
    a_ << lwz(4, int16_t(s * kStripBytes), 20) << srwi(0, 4, 24) << cmpwi(0, 0, 0);
    a_.beq(hidden);
    a_ << addi(3, 20, int16_t(s * kStripBytes + 4));
    a_.bl("poly4t");
    a_.label(hidden);
  }
  set_texture(kFont, kFontLog2W, kFontLog2H);
  a_.label("intro_no_plate");

  // Claw slashes.
  a_ << cmpwi(0, 17, int16_t(kSlashFirst));
  a_.blt("intro_no_slash");
  a_ << cmpwi(0, 17, int16_t(kSlashLast));
  a_.bge("intro_no_slash");
  mode(kMAdd);
  a_.la(20, "slash_tab");
  a_ << addi(0, 17, -int16_t(kSlashFirst)) << mulli(0, 0, int16_t(kSlashes * kSlashBytes)) << add(20, 20, 0);
  for (u32 k = 0; k < kSlashes; ++k) {
    const std::string hidden = L("slash_hidden");
    a_ << lwz(0, int16_t(k * kSlashBytes), 20) << cmpwi(0, 0, 0);
    a_.beq(hidden);
    a_ << addi(3, 20, int16_t(k * kSlashBytes + 4));
    a_.bl("poly4c");
    a_.label(hidden);
  }
  a_.label("intro_no_slash");

  // The Lion mark and its crown (same transform, dropped by column 13).
  mode(kM3D);
  a_.la(3, "mesh_emblem");
  a_ << mr(4, 19) << lfs(1, 24, 19);
  a_.bl("mesh");
  for (int i = 0; i < 6; ++i) {
    a_ << lfs(1, int16_t(4 * i), 19);
    if (i == 4) a_ << lfs(2, 52, 19) << fadds(1, 1, 2);
    a_ << stfs(1, int16_t(kXform + 4 * i), rRam);
  }
  a_.la(3, "mesh_crown");
  a_ << addi(4, rRam, kXform) << lfs(1, 24, 19);
  a_.bl("mesh");

  mode(kMText);
  text_faded("s_atari", 168, 332, 32, 0xFFF4F0EA, false, 19, 32);
  text_faded("s_lion", 312, 332, 32, 0xFFE81E1E, false, 19, 36);
  text_faded("s_a2", 432, 332, 32, 0xFFF2C040, false, 19, 56);
  text_faded("s_tagline", 320, 380, 12, 0xFFB4A8A0, true, 19, 40);
  // Flash on the tear.
  a_ << lfs(1, 44, 19) << fcmpu(0, 1, 31);
  a_.ble("intro_noflash");
  mode(kMAdd);
  a_.li32(3, 0xFFFFC898);
  a_.bl("shade");
  a_ << mr(4, 3);
  lf(1, 0), lf(2, 0), lf(3, 640), lf(4, 480);
  a_.bl("quad");
  a_.label("intro_noflash");
  a_ << lfs(1, 48, 19);
  a_.bl("fade");
  a_.bl("end_frame");
  a_.bl("seq_step");
  a_ << addi(17, 17, 1) << cmpwi(0, 17, int16_t(kIntroFrames));
  a_.blt("intro_loop");

  // Let the drive finish looking at a disc (two seconds at most), then decide:
  // a game boots at once, a music disc opens the player, otherwise the dashboard.
  a_ << li(17, 0);
  a_.label("intro_wait");
  a_ << lwz(0, 0, rCd) << andi_(0, 0, Cdrom::kPresent);
  a_.beq("intro_dash");
  ram_ld(0, kDiscState);
  a_ << cmpwi(0, 0, kReading);
  a_.beq("intro_wait_frame");
  a_ << cmpwi(0, 0, kReadingDir);
  a_.bne("intro_decide");
  a_.label("intro_wait_frame");
  a_ << li(3, 3) << li(4, 0);
  a_.bl("begin_frame");
  a_.bl("end_frame");
  a_.bl("seq_step");
  a_.bl("cd_async");
  a_ << addi(17, 17, 1) << cmpwi(0, 17, 120);
  a_.blt("intro_wait");
  a_.label("intro_decide");
  ram_ld(0, kDiscState);
  a_ << cmpwi(0, 0, kReady);
  a_.bne("intro_dash");
  ram_ld(0, kHasGame);
  a_ << cmpwi(0, 0, 0);
  a_.bne("loading");
  ram_ld(0, kTrackCount);
  a_ << cmpwi(0, 0, 0);
  a_.beq("intro_dash");
  a_ << li(3, kItemMusic);
  a_.b("dash_enter_sel");
  a_.label("intro_dash");
  a_ << li(3, kItemPlay);
  a_.b("dash_enter_sel");
}

}  // namespace lion::romgen
