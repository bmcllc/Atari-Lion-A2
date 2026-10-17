// Boot ROM: the dashboard. Five cards (PLAY, MUSIC, THEME, TYKE, SYSTEM) over a
// rolling floor grid; each but PLAY opens a panel (rom/audio.cpp has the audio
// ones). Registers in the loop: r14 = buttons pressed, r19 = open panel,
// r20 = selected card.
#include "rom/builder.h"

namespace lion::romgen {

namespace {
constexpr int kCards = int(kItemCount);
constexpr float kCardPitch = 120.0f, kCardHalf = 54.0f, kCardTop = 136.0f, kCardBottom = 318.0f;
float card_x(int i) { return 80.0f + kCardPitch * float(i); }
const char* const kCardMeshes[kCards] = {"mesh_disc", "mesh_note", "mesh_audio", "mesh_mixer", "mesh_chip"};
constexpr u32 kTextLight = 0xFFE6DCD2, kTextGrey = 0xFF988880, kTextAmber = 0xFFFFB040;
}  // namespace

void RomBuilder::dashboard() {
  a_.label("dash_enter");
  a_ << li(3, kItemPlay);
  a_.label("dash_enter_sel");  // r3 = card to select
  ram_st(3, kSelection);
  ram_set(kScene, kSceneDashboard);
  a_ << stfs(30, kFade, rRam);
  for (u32 off : {kSubmenu, kLaunch}) ram_set(off, 0);
  lf(1, 0.7f);
  lf(2, 0.4f);
  for (int i = 0; i < kCards; ++i) a_ << stfs(1, int16_t(kCardScale + 4 * i), rRam) << stfs(2, int16_t(kCardBright + 4 * i), rRam);
  // Music, unless the CD player or the jingle is already playing.
  ram_ld(0, kMusicState);
  a_ << cmpwi(0, 0, kMusicStopped);
  a_.bne("dash_music_on");
  ram_ld(0, kSeqJingle);
  a_ << cmpwi(0, 0, 0);
  a_.bne("dash_music_on");
  a_.bl("theme_start");
  a_.label("dash_music_on");
  a_.bl("read_pad");  // swallow the button that got us here

  a_.label("dash_loop");
  a_.bl("read_pad");
  a_.bl("cd_async");
  a_.bl("music_update");
  dash_input();

  // ---- animation
  // Fade in, or out while launching.
  a_ << lfs(1, kFade, rRam) << lwz(0, kLaunch, rRam) << cmpwi(0, 0, 0);
  a_.bne("dash_fading_out");
  lf(2, 0.05f);
  a_ << fsubs(1, 1, 2) << fsel(1, 1, 1, 31);
  a_.b("dash_faded");
  a_.label("dash_fading_out");
  lf(2, 0.06f);
  a_ << fadds(1, 1, 2);
  a_.label("dash_faded");
  a_ << stfs(1, kFade, rRam);
  // Grid scroll and the header emblem's spin.
  for (auto [off, step, wrap] : {std::tuple{kScroll, 0.04f, rom::kGridStep}, std::tuple{kEmblemYaw, 0.6f, 256.0f}}) {
    const std::string ok = L("wrap");
    a_ << lfs(1, int16_t(off), rRam);
    lf(2, step);
    a_ << fadds(1, 1, 2);
    lf(2, wrap);
    a_ << fcmpu(0, 1, 2);
    a_.blt(ok);
    a_ << fsubs(1, 1, 2);
    a_.label(ok);
    a_ << stfs(1, int16_t(off), rRam);
  }
  // Cards ease towards their targets; the selected icon sways, the others face front.
  for (int i = 0; i < kCards; ++i) {
    const std::string other = L("card_other"), set = L("card_set"), go = L("card_go"), still = L("icon_still");
    a_ << cmpwi(0, 20, i);
    a_.bne(other);
    lf(2, 1.0f), lf(3, 1.3f);
    a_.b(set);
    a_.label(other);
    lf(2, 0.72f), lf(3, 0.45f);
    a_.label(set);
    a_ << cmpwi(0, 19, kSubNone);
    a_.beq(go);
    lf(3, 0.22f);
    a_.label(go);
    lf(5, 0.18f);
    for (auto [off, target] : {std::pair{kCardScale + 4 * u32(i), 2u}, std::pair{kCardBright + 4 * u32(i), 3u}})
      a_ << lfs(1, int16_t(off), rRam) << fsubs(4, target, 1) << fmadds(1, 4, 5, 1) << stfs(1, int16_t(off), rRam);
    a_ << fmr(6, 31) << cmpwi(0, 20, i);
    a_.bne(still);
    a_ << slwi(3, rFrame, 1) << clrlwi(3, 3, 24) << slwi(3, 3, 2) << add(3, 3, rPool) << lfs(6, int16_t(sin_), 3);
    lf(7, 30.0f);
    a_ << fmuls(6, 6, 7);
    a_.label(still);
    a_ << lfs(1, int16_t(kIconYaw + 4 * i), rRam) << fsubs(4, 6, 1);
    lf(5, 0.12f);
    a_ << fmadds(1, 4, 5, 1) << stfs(1, int16_t(kIconYaw + 4 * i), rRam);
  }

  dash_render();

  a_ << lfs(1, kFade, rRam);
  a_.bl("fade");
  a_.bl("end_frame");
  a_.bl("seq_step");
  // Launch once the screen is black.
  a_ << lwz(0, kLaunch, rRam) << cmpwi(0, 0, 0);
  a_.beq("dash_loop");
  a_ << lfs(1, kFade, rRam) << fcmpu(0, 1, 30);
  a_.blt("dash_loop");
  a_.b("loading");
}

void RomBuilder::dash_input() {
  auto sfx = [&](const char* name) {
    a_.la(3, name);
    a_.bl("sfx");
  };
  // Moves `off` by `delta` within [0, limit) (limit in a register); sfx_tick when it moves.
  auto step = [&](u32 off, int delta, u32 rlimit) {
    const std::string skip = L("step");
    ram_ld(3, off);
    a_ << addi(3, 3, int16_t(delta)) << cmplw(0, 3, rlimit);  // unsigned: -1 is out of range too
    a_.bge(skip);
    ram_st(3, off);
    sfx("sfx_tick");
    a_.label(skip);
  };

  ram_ld(20, kSelection);
  ram_ld(19, kSubmenu);
  ram_ld(0, kLaunch);
  a_ << cmpwi(0, 0, 0);
  a_.bne("in_done");
  a_ << cmpwi(0, 19, kSubNone);
  a_.bne("in_panel");

  // Main row.
  a_ << li(21, kCards);
  unless_pressed(Pads::kLeft, "in_no_left");
  step(kSelection, -1, 21);
  a_.label("in_no_left");
  unless_pressed(Pads::kRight, "in_no_right");
  step(kSelection, 1, 21);
  a_.label("in_no_right");
  ram_ld(20, kSelection);
  unless_pressed(Pads::kA | Pads::kStart, "in_done");
  a_ << cmpwi(0, 20, kItemPlay);
  a_.bne("in_not_play");
  ram_ld(0, kDiscState);
  a_ << cmpwi(0, 0, kReady);
  a_.bne("in_cannot_play");
  ram_ld(0, kHasGame);
  a_ << cmpwi(0, 0, 0);
  a_.beq("in_cannot_play");
  ram_ld(0, kBootFailed);
  a_ << cmpwi(0, 0, 0);
  a_.bne("in_cannot_play");
  ram_set(kLaunch, 1);
  sfx("sfx_ok");
  a_.b("in_done");
  a_.label("in_cannot_play");
  sfx("sfx_back");
  a_.b("in_done");
  a_.label("in_not_play");
  sfx("sfx_ok");
  const std::pair<u32, u32> opens[] = {
      {kItemMusic, kSubMusic}, {kItemTheme, kSubTheme}, {kItemTyke, kSubTyke}, {kItemSystem, kSubSystem}};
  for (auto [item, sub] : opens) {
    const std::string next = L("in_open");
    a_ << cmpwi(0, 20, int16_t(item));
    a_.bne(next);
    ram_set(kSubmenu, sub);
    if (item == kItemTheme) {
      ram_ld(0, kTheme);
      ram_st(0, kThemeCursor);
    }
    if (item == kItemMusic) {  // the cursor starts on the track playing
      const std::string keep = L("in_keep");
      ram_ld(0, kMusicState);
      a_ << cmpwi(0, 0, kMusicStopped);
      a_.beq(keep);
      ram_ld(0, kMusicTrack);
      ram_st(0, kMusicCursor);
      a_.label(keep);
    }
    a_.b("in_done");
    a_.label(next);
  }
  a_.b("in_done");

  // Panels: B closes every one of them.
  a_.label("in_panel");
  unless_pressed(Pads::kB, "in_no_back");
  ram_set(kSubmenu, kSubNone);
  sfx("sfx_back");
  a_.b("in_done");
  a_.label("in_no_back");

  // MUSIC: up/down choose, A plays (or pauses the track playing), X stops.
  a_ << cmpwi(0, 19, kSubMusic);
  a_.bne("in_not_music");
  ram_ld(21, kTrackCount);
  a_ << cmpwi(0, 21, 0);
  a_.beq("in_done");
  unless_pressed(Pads::kUp, "in_m_no_up");
  step(kMusicCursor, -1, 21);
  a_.label("in_m_no_up");
  unless_pressed(Pads::kDown, "in_m_no_down");
  step(kMusicCursor, 1, 21);
  a_.label("in_m_no_down");
  unless_pressed(Pads::kA, "in_m_no_a");
  sfx("sfx_ok");
  ram_ld(0, kMusicState);
  a_ << cmpwi(0, 0, kMusicStopped);
  a_.beq("in_m_play");
  ram_ld(0, kMusicTrack);
  ram_ld(3, kMusicCursor);
  a_ << cmpw(0, 0, 3);
  a_.bne("in_m_play");
  a_.bl("music_toggle");
  a_.b("in_done");
  a_.label("in_m_play");
  ram_ld(3, kMusicCursor);
  a_.bl("music_play");
  a_.b("in_done");
  a_.label("in_m_no_a");
  unless_pressed(Pads::kX, "in_done");
  ram_ld(0, kMusicState);
  a_ << cmpwi(0, 0, kMusicStopped);
  a_.beq("in_done");
  sfx("sfx_back");
  a_.bl("music_stop");
  a_.bl("theme_start");
  a_.b("in_done");
  a_.label("in_not_music");

  // THEME: up/down choose, A selects (it plays unless the CD player is on).
  a_ << cmpwi(0, 19, kSubTheme);
  a_.bne("in_not_theme");
  a_ << li(21, rom::kThemeCount);
  unless_pressed(Pads::kUp, "in_t_no_up");
  step(kThemeCursor, -1, 21);
  a_.label("in_t_no_up");
  unless_pressed(Pads::kDown, "in_t_no_down");
  step(kThemeCursor, 1, 21);
  a_.label("in_t_no_down");
  unless_pressed(Pads::kA, "in_done");
  ram_ld(0, kThemeCursor);
  ram_st(0, kTheme);
  sfx("sfx_ok");
  ram_ld(0, kMusicState);
  a_ << cmpwi(0, 0, kMusicStopped);
  a_.bne("in_done");
  a_.bl("theme_start");
  a_.b("in_done");
  a_.label("in_not_theme");

  // TYKE: up/down choose a setting, left/right adjust it, A plays the jingle.
  a_ << cmpwi(0, 19, kSubTyke);
  a_.bne("in_done");
  a_ << li(21, 4);
  unless_pressed(Pads::kUp, "in_k_no_up");
  step(kTykeRow, -1, 21);
  a_.label("in_k_no_up");
  unless_pressed(Pads::kDown, "in_k_no_down");
  step(kTykeRow, 1, 21);
  a_.label("in_k_no_down");
  for (auto [button, delta] : {std::pair{Pads::kLeft, -16}, std::pair{Pads::kRight, 16}}) {
    const std::string skip = L("in_adjust"), low = L("in_low"), high = L("in_high");
    unless_pressed(button, skip);
    ram_ld(4, kTykeRow);
    a_ << slwi(4, 4, 2) << lwzx(3, rTyke, 4) << addi(3, 3, int16_t(delta)) << cmpwi(0, 3, 0);
    a_.bge(low);
    a_ << li(3, 0);
    a_.label(low);
    a_ << cmpwi(0, 3, 0x100);
    a_.ble(high);
    a_ << li(3, 0x100);
    a_.label(high);
    a_ << stwx(3, rTyke, 4);
    sfx("sfx_tick");
    a_.label(skip);
  }
  unless_pressed(Pads::kA, "in_done");
  ram_ld(0, kMusicState);
  a_ << cmpwi(0, 0, kMusicStopped);
  a_.bne("in_done");  // the CD player has the voices
  a_.bl("jingle");
  a_.label("in_done");
  ram_ld(20, kSelection);
  ram_ld(19, kSubmenu);
}

void RomBuilder::dash_render() {
  // Black clear, then only the bands that are not black: fill rate is precious.
  a_ << li(3, 3) << li(4, 0);
  a_.bl("begin_frame");
  mode(kM2D);
  rect(0, 84, 640, 240, 0xFF000000, 0xFF3E0B0D);   // sky glowing towards the horizon
  rect(0, 240, 640, 330, 0xFF140305, 0xFF000000);  // floor
  // Floor grid rolling towards the viewer.
  mode(kMGrid);
  a_ << stfs(31, kXform, rRam) << stfs(31, kXform + 4, rRam) << stfs(30, kXform + 8, rRam) << stfs(31, kXform + 12, rRam);
  lf(1, -1.3f);
  a_ << stfs(1, kXform + 16, rRam) << lfs(1, kScroll, rRam) << stfs(1, kXform + 20, rRam);
  a_.la(3, "mesh_grid");
  a_ << addi(4, rRam, kXform) << fmr(1, 30);
  a_.bl("mesh");
  mode(kMBlend);
  rect(0, 240, 640, 300, 0xD03E0B0D, 0x003E0B0D);  // haze on the horizon

  // Header: the crowned Lion mark, the name and a rule.
  mode(kM3D);
  a_ << lfs(1, kEmblemYaw, rRam) << stfs(1, kXform, rRam) << stfs(31, kXform + 4, rRam);
  const float t[4] = {0.36f, -4.85f, 3.30f, -7.5f};
  for (int i = 0; i < 4; ++i) {
    lf(1, t[i]);
    a_ << stfs(1, int16_t(kXform + 8 + 4 * i), rRam);
  }
  a_.la(3, "mesh_emblem_small");
  a_ << addi(4, rRam, kXform) << fmr(1, 30);
  a_.bl("mesh");
  a_.la(3, "mesh_crown");
  a_ << addi(4, rRam, kXform) << fmr(1, 30);
  a_.bl("mesh");
  mode(kMText);
  text("s_atari", 100, 30, 24, 0xFFF4F0EA, false);
  text("s_lion", 208, 30, 24, 0xFFE81E1E, false);
  text("s_a2", 298, 30, 24, 0xFFF2C040, false);
  mode(kMAdd);
  rect(100, 66, 600, 68, 0xFFA01818, 0xFF300808);

  // Cards.
  for (int i = 0; i < kCards; ++i) {
    const float cx = card_x(i), x0 = cx - kCardHalf, x1 = cx + kCardHalf;
    const std::string no_glow = L("card_noglow");
    mode(kMBlend);
    rect(x0, kCardTop, x1, kCardBottom, 0xB0300A0A, 0xB0080404);
    a_ << cmpwi(0, 20, i);
    a_.bne(no_glow);
    a_ << cmpwi(0, 19, kSubNone);
    a_.bne(no_glow);
    mode(kMAdd);
    // Pulsing glow: 0.5 + 0.15 sin(3 * frame).
    a_ << mulli(3, rFrame, 3) << clrlwi(3, 3, 24) << slwi(3, 3, 2) << add(3, 3, rPool) << lfs(5, int16_t(sin_), 3);
    lf(6, 0.15f), lf(7, 0.5f);
    a_ << fmadds(1, 5, 6, 7);
    load(3, 0xFFC02A14);
    a_.bl("shade");
    a_ << lis(4, 0xFF00);
    lf(1, cx), lf(2, 214), lf(3, 80), lf(4, 112);
    a_.bl("fan");
    a_.label(no_glow);
    // Frame, brighter on the selected card.
    mode(kMAdd);
    a_ << lfs(1, int16_t(kCardBright + 4 * i), rRam);
    lf(2, 0.5f);
    a_ << fmuls(1, 1, 2);
    load(3, 0xFFFF4A2A);
    a_.bl("shade");
    a_ << mr(4, 3);
    for (auto [qx0, qy0, qx1, qy1] : {std::tuple{x0, kCardTop, x1, kCardTop + 2}, std::tuple{x0, kCardBottom - 2, x1, kCardBottom},
                                      std::tuple{x0, kCardTop, x0 + 2, kCardBottom}, std::tuple{x1 - 2, kCardTop, x1, kCardBottom}}) {
      lf(1, qx0), lf(2, qy0), lf(3, qx1), lf(4, qy1);
      a_.bl("quad");
    }
    // Icon.
    mode(kM3D);
    a_ << lfs(1, int16_t(kIconYaw + 4 * i), rRam) << stfs(1, kXform, rRam);
    lf(1, -12.0f);
    a_ << stfs(1, kXform + 4, rRam) << lfs(1, int16_t(kCardScale + 4 * i), rRam);
    lf(2, 0.62f);
    a_ << fmuls(1, 1, 2) << stfs(1, kXform + 8, rRam);
    const float pos[3] = {float(i - 2) * 1.7321f, 0.40f, -6.0f};
    for (int j = 0; j < 3; ++j) {
      lf(1, pos[j]);
      a_ << stfs(1, int16_t(kXform + 12 + 4 * j), rRam);
    }
    a_.la(3, kCardMeshes[i]);
    a_ << addi(4, rRam, kXform) << lfs(1, int16_t(kCardBright + 4 * i), rRam);
    a_.bl("mesh");
    // Label.
    mode(kMText);
    a_ << lfs(1, int16_t(kCardBright + 4 * i), rRam);
    lf(2, 0.25f);
    a_ << fadds(1, 1, 2);
    a_.li32(3, 0xFFF4F0EA);
    a_.bl("shade");
    a_ << mr(4, 3);
    a_.la(3, "s_card" + std::to_string(i));
    lf(1, cx), lf(2, 288), lf(3, 13);
    a_.bl("textc");
  }

  // Description of the selected card: r21 = first line, r22 = second line (amber) or 0.
  a_ << cmpwi(0, 19, kSubNone);
  a_.bne("np_none");  // a panel covers it
  a_ << li(22, 0) << cmpwi(0, 20, kItemPlay);
  a_.bne("desc_not_play");
  ram_ld(0, kDiscState);
  const std::pair<u32, const char*> disc_lines[] = {
      {kNoDisc, "s_insert"}, {kReading, "s_reading"}, {kReadingDir, "s_reading"}, {kUnplayable, "s_unplayable"}};
  for (auto [state, label] : disc_lines) {
    const std::string next = L("desc_state");
    a_ << cmpwi(0, 0, int16_t(state));
    a_.bne(next);
    a_.la(21, label);
    a_.b("desc_draw");
    a_.label(next);
  }
  a_ << addi(22, rRam, kLabel);  // ready: the volume label underneath
  a_.la(21, "s_nogame");
  ram_ld(0, kHasGame);
  a_ << cmpwi(0, 0, 0);
  a_.beq("desc_draw");
  a_.la(21, "s_unplayable");
  ram_ld(0, kBootFailed);
  a_ << cmpwi(0, 0, 0);
  a_.bne("desc_draw");
  a_.la(21, "s_play");
  a_.b("desc_draw");
  a_.label("desc_not_play");
  const std::tuple<u32, const char*, const char*> items[] = {{kItemMusic, "s_music_desc", "s_music_desc2"},
                                                             {kItemTyke, "s_tyke_desc", "s_tyke_desc2"},
                                                             {kItemSystem, "s_system_desc", "s_system_desc2"}};
  for (auto [item, l1, l2] : items) {
    const std::string next = L("desc_item");
    a_ << cmpwi(0, 20, int16_t(item));
    a_.bne(next);
    a_.la(21, l1);
    a_.la(22, l2);
    a_.b("desc_draw");
    a_.label(next);
  }
  // THEME: the theme in use.
  a_.la(21, "s_theme_desc");
  ram_ld(3, kTheme);
  a_.la(4, "theme_names");
  a_ << slwi(3, 3, 2) << lwzx(22, 4, 3);
  a_.label("desc_draw");
  mode(kMText);
  a_ << mr(3, 21);
  text_r(320, 344, 14, kTextLight, true);
  a_ << cmpwi(0, 22, 0);
  a_.beq("desc_one_line");
  a_ << mr(3, 22);
  text_r(320, 368, 14, kTextAmber, true);
  a_.label("desc_one_line");

  // Now playing, under the description.
  ram_ld(0, kMusicState);
  a_ << cmpwi(0, 0, kMusicStopped);
  a_.beq("np_none");
  a_.la(3, "s_now");
  a_ << addi(4, rRam, int16_t(kNowStr));
  a_.bl("strcpy");
  ram_ld(3, kMusicTrack);
  a_ << mulli(3, 3, int16_t(kTrackSize)) << add(3, 3, rRam) << addi(3, 3, int16_t(kTracks + 8));
  a_.bl("strcpy");
  a_ << li(0, ' ') << stb(0, 0, 4) << stb(0, 1, 4) << addi(4, 4, 2);
  ram_ld(3, kMusicElapsed);
  a_.li32(5, u32(kAudioRate));
  a_ << divwu(3, 3, 5);
  a_.bl("fmt_time");
  a_ << addi(3, rRam, int16_t(kNowStr));
  text_r(320, 400, 12, 0xFFFF9A40, true);
  a_.label("np_none");

  // Panels.
  a_ << cmpwi(0, 19, kSubNone);
  a_.beq("dash_no_panel");
  mode(kMBlend);
  rect(40, 94, 600, 414, 0xF80C0405, 0xF8040203);
  mode(kMAdd);
  for (auto [y0, y1] : {std::pair{94.0f, 96.0f}, std::pair{412.0f, 414.0f}}) rect(40, y0, 600, y1, 0xFFB02020, 0xFFB02020);
  const std::pair<u32, void (RomBuilder::*)()> panels[] = {{kSubSystem, &RomBuilder::panel_system},
                                                          {kSubTheme, &RomBuilder::panel_theme},
                                                          {kSubTyke, &RomBuilder::panel_tyke},
                                                          {kSubMusic, &RomBuilder::panel_music}};
  for (auto [sub, emit] : panels) {
    const std::string next = L("panel");
    a_ << cmpwi(0, 19, int16_t(sub));
    a_.bne(next);
    (this->*emit)();
    a_.b("dash_no_panel");
    a_.label(next);
  }
  a_.label("dash_no_panel");

  // Button hints.
  mode(kMText);
  ram_ld(19, kSubmenu);
  const std::pair<u32, const char*> hints[] = {{kSubNone, "s_hint_main"}, {kSubMusic, "s_hint_music"},
                                               {kSubTheme, "s_hint_theme"}, {kSubTyke, "s_hint_tyke"}};
  for (auto [sub, label] : hints) {
    const std::string next = L("hint");
    a_ << cmpwi(0, 19, int16_t(sub));
    a_.bne(next);
    text(label, 320, 440, 11, kTextGrey, true);
    a_.b("hint_done");
    a_.label(next);
  }
  text("s_hint_back", 320, 440, 11, kTextGrey, true);
  a_.label("hint_done");
}

}  // namespace lion::romgen
