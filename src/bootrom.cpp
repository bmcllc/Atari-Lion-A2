// Lion A2 boot ROM, assembled at startup into the 1 MB ROM at 0xFFF0_0000.
//
// Reset (0xFFF0_0100, MSR[IP] = 1):
//   1. upload the Tyke mixer firmware and start the DSP; copy the font to SGRAM;
//   2. intro: the Lion mark spins in over a red glow while the start-up jingle
//      plays (skipped on development units, see Pic::kStrapFastBoot);
//   3. with a disc in the drive, boot it; otherwise run the dashboard;
//   4. loader: read the ISO9660 volume, find BOOT.LION, check its header, copy
//      its segments and enter the game with MSR = ME | FP | VEC.
// A disc that cannot be booted returns to the dashboard with a message.
//
// Everything is drawn by Spike from a FIFO the ROM fills directly: 640x480
// RGB565, double-buffered, with the QVE projecting the meshes. Tuffy dozes
// between frames and wakes on the Spike and VBlank interrupts.
#include "rom/builder.h"

namespace lion {
namespace romgen {

// ------------------------------------------------------------------ setup

void RomBuilder::globals() {
  a_.li32(1, kStack);
  a_.li32(0, kBaseMsr);
  a_ << mtmsr(0) << isync();
  a_.li32(rSpike, map::kSpike);
  a_.li32(rVi, map::kVi);
  a_.li32(rPic, map::kPic);
  a_.li32(rCd, map::kCd);
  a_.li32(rTyke, map::kTyke);
  a_.la(rPool, "pool");
  a_.li32(rRam, kRam);
  a_.li32(rQve, map::kQve);
  a_.li32(rPad, map::kPad);
  lf(31, 0.0f);
  lf(30, 1.0f);
}

void RomBuilder::video_init() {
  a_ << li(0, 3) << stw(0, 0, rVi);  // 480p
  a_.li32(0, kBufA);
  a_ << stw(0, 4, rVi) << li(0, 1280) << stw(0, 8, rVi);
  a_.li32(rBack, kBufB);
  a_ << li(rFrame, 1);
  a_ << li(0, -1) << stw(0, 0, rPic);  // drop stale interrupts
  a_ << li(0, 3) << stw(0, 4, rPic);   // VBlank + Spike wake the dozing CPU
  // QVE viewport: scale (320, -240), offset (320, 240).
  const float vp[4] = {320.0f, -240.0f, 320.0f, 240.0f};
  for (int i = 0; i < 4; ++i) {
    lf(1, vp[i]);
    a_ << stfs(1, int16_t(0x40 + 4 * i), rQve);
  }
}

void RomBuilder::vectors() {
  // 0x000-0x0FF: ROM header.
  a_.word(0x4C494F4E);   // "LION"
  a_.word(0x0000'0A02);  // board revision A2
  a_.word(0x0001'0000);  // ROM version 1.0
  while (a_.here() < map::kRomVectors + 0x100) a_.word(0);
  a_.b("start");  // 0x100: reset
  for (u32 v = 0x200; v < 0x1000; v += 0x100) {
    while (a_.here() < map::kRomVectors + v) a_.word(0);
    if (v == 0x500) {
      // External interrupt: acknowledge everything (the ROM only uses them to wake up).
      a_ << mtspr(272, 3) << mtspr(273, 4) << lis(3, map::kPic >> 16) << lwz(4, 0, 3) << stw(4, 0, 3);
      a_ << mfspr(4, 273) << mfspr(3, 272) << rfi();
    } else if (v == 0x900) {
      // Decrementer: push it far away.
      a_ << mtspr(272, 3) << lis(3, 0x7FFF) << mtspr(22, 3) << mfspr(3, 272) << rfi();
    } else {
      a_.b("fatal");  // unexpected exception
    }
  }
  while (a_.here() < map::kRomVectors + 0x1000) a_.word(0);
}

void RomBuilder::reset() {
  a_.label("start");
  globals();
  a_ << li(0, 0) << stw(0, 4, rPic) << li(0, -1) << stw(0, 0, rPic);  // interrupts masked

  // Tyke firmware upload.
  const std::vector<u32> fw = tyke_mixer_firmware();
  a_.la(3, "tyke_fw");
  a_.li32(4, map::kTyke + Tyke::kProgramBase);
  a_.li32(5, u32(fw.size()));
  a_ << mtctr(5);
  a_.label("fw_copy");
  a_ << lwz(6, 0, 3) << stw(6, 0, 4) << addi(3, 3, 4) << addi(4, 4, 4);
  a_.bdnz("fw_copy");
  a_.li32(4, map::kTyke + Tyke::kCtrlBase);
  a_ << li(6, 1) << stw(6, 0, 4);

  // Dashboard RAM.
  a_ << li(0, 0) << li(3, kRamClear / 4) << mtctr(3) << mr(3, rRam);
  a_.label("ram_clear");
  a_ << stw(0, 0, 3) << addi(3, 3, 4);
  a_.bdnz("ram_clear");
  a_ << li(0, 20) << stw(0, kSfxCh, rRam);

  // Font into SGRAM.
  a_.la(3, "font");
  a_.li32(4, kFont);
  a_.li32(5, (256 * 128 * 2) / 4);
  a_ << mtctr(5);
  a_.label("font_copy");
  a_ << lwz(6, 0, 3) << stw(6, 0, 4) << addi(3, 3, 4) << addi(4, 4, 4);
  a_.bdnz("font_copy");
  a_.la(3, "jaguar");
  a_.li32(4, kJaguarTex);
  a_.li32(5, (256 * 64 * 2) / 4);
  a_ << mtctr(5);
  a_.label("jaguar_copy");
  a_ << lwz(6, 0, 3) << stw(6, 0, 4) << addi(3, 3, 4) << addi(4, 4, 4);
  a_.bdnz("jaguar_copy");

  // Development units go straight to the disc (or the dashboard).
  a_ << lwz(3, 8, rPic) << andi_(3, 3, Pic::kStrapFastBoot);
  a_.beq("retail");
  a_ << lwz(3, 0, rCd) << andi_(3, 3, Cdrom::kPresent);
  a_.bne("loader");
  video_init();
  a_.b("dash_enter");
  a_.label("retail");
  video_init();
  a_.b("intro");
}

// ------------------------------------------------------------------ routines

void RomBuilder::routines() {
  // begin_frame(r3 = clear flags, r4 = colour): target the back buffer, set the font texture.
  a_.label("begin_frame");
  pushk(Spike::kSetTarget << 24 | 5);
  push(rBack);
  pushk(1280), pushk(640), pushk(480), pushk(kZBuf);
  pushk(Spike::kClear << 24 | 3);
  push(3), push(4), pushk(0xFFFFFF);
  pushk(Spike::kSetTexture << 24 | 4);
  pushk(kFont), pushk(rom::kFontLog2W), pushk(rom::kFontLog2H), pushk(1);
  a_ << blr();

  // end_frame: SYNC, doze until Spike finishes, flip on the next VBlank.
  a_.label("end_frame");
  pushk(Spike::kSync << 24 | 1);
  push(rFrame);
  a_.li32(5, kDozeMsr);
  a_.label("ef_sync");
  a_ << lwz(0, 8, rSpike) << cmpw(0, 0, rFrame);
  a_.beq("ef_synced");
  a_ << mtmsr(5);
  a_.b("ef_sync");
  a_.label("ef_synced");
  a_ << lwz(3, 0x10, rVi);
  a_.label("ef_vblank");
  a_ << mtmsr(5) << lwz(0, 0x10, rVi) << cmpw(0, 0, 3);
  a_.beq("ef_vblank");
  a_.li32(0, kBaseMsr);
  a_ << mtmsr(0);
  a_ << stw(rBack, 4, rVi);
  a_.li32(0, kBufA ^ kBufB);
  a_ << xor_(rBack, rBack, 0) << addi(rFrame, rFrame, 1) << blr();

  // quad(f1 x0, f2 y0, f3 x1, f4 y1, r3 top colour, r4 bottom colour).
  a_.label("quad");
  tri_header();
  vtx(1, 2, 3), vtx(1, 4, 4), vtx(3, 4, 4);
  tri_header();
  vtx(1, 2, 3), vtx(3, 4, 4), vtx(3, 2, 3);
  a_ << blr();

  // fan(f1 cx, f2 cy, f3 rx, f4 ry, r3 centre colour, r4 rim colour): 24-segment ellipse.
  a_.label("fan");
  a_ << addi(5, rPool, int16_t(circle_)) << li(6, 24) << mtctr(6);
  a_.label("fan_loop");
  a_ << lfs(5, 0, 5) << lfs(6, 4, 5) << lfs(7, 8, 5) << lfs(8, 12, 5);
  a_ << fmadds(9, 3, 5, 1) << fmadds(10, 4, 6, 2) << fmadds(11, 3, 7, 1) << fmadds(12, 4, 8, 2);
  tri_header();
  vtx(1, 2, 3), vtx(9, 10, 4), vtx(11, 12, 4);
  a_ << addi(5, 5, 8);
  a_.bdnz("fan_loop");
  a_ << blr();

  // shade(r3 colour, f1 intensity) -> r3.
  a_.label("shade");
  shade(3, 1, 3, 4, 5, 6);
  a_ << blr();

  // textc: like text, but f1 is the centre of the line.
  a_.label("textc");
  a_ << mr(5, 3) << li(6, 0);
  a_.label("textc_len");
  a_ << lbz(7, 0, 5) << cmpwi(0, 7, 0);
  a_.beq("textc_go");
  a_ << addi(6, 6, 1) << addi(5, 5, 1);
  a_.b("textc_len");
  a_.label("textc_go");
  i2f(5, 6);
  lf(6, 0.75f);
  a_ << fmuls(6, 6, 3) << fmuls(5, 5, 6);  // n * advance
  lf(7, 0.125f);
  a_ << fmuls(7, 7, 3) << fsubs(5, 5, 7);  // minus the trailing gap
  lf(7, 0.5f);
  a_ << fmuls(5, 5, 7) << fsubs(1, 1, 5);
  // fall through

  // text(r3 string, f1 x, f2 y, f3 size, r4 colour): monospaced, advance 0.75 * size.
  a_.label("text");
  lf(12, 0.75f);
  a_ << fmuls(12, 3, 12);
  lf(13, 1.0f / 16.0f);
  lf(11, 1.0f / 8.0f);
  a_ << fadds(10, 2, 3);
  a_.label("text_loop");
  a_ << lbz(5, 0, 3) << cmpwi(0, 5, 0);
  a_.beq("text_done");
  a_ << addi(3, 3, 1) << cmpwi(0, 5, 32);
  a_.beq("text_next");
  a_ << clrlwi(5, 5, 25) << slwi(6, 5, 3) << add(6, 6, rPool);
  a_ << lfs(5, int16_t(uv_), 6) << lfs(6, int16_t(uv_ + 4), 6);
  a_ << fadds(7, 5, 13) << fadds(8, 6, 11) << fadds(9, 1, 3);
  tri_header();
  vtxt(1, 2, 4, 5, 6), vtxt(1, 10, 4, 5, 8), vtxt(9, 10, 4, 7, 8);
  tri_header();
  vtxt(1, 2, 4, 5, 6), vtxt(9, 10, 4, 7, 8), vtxt(9, 2, 4, 7, 6);
  a_.label("text_next");
  a_ << fadds(1, 1, 12);
  a_.b("text_loop");
  a_.label("text_done");
  a_ << blr();

  // mesh(r3 mesh, r4 transform {yaw, pitch, scale, tx, ty, tz}, f1 brightness).
  // Angles are in 1/256 turns. Builds M = P * T * S * Ry * Rx straight into the QVE,
  // projects the vertices and emits flat-shaded triangles lit by rom::kLight.
  a_.label("mesh");
  auto trig = [&](int16_t off, u32 rs, u32 fsin, u32 fcos) {
    a_ << lfs(2, off, 4);
    f2i(rs, 2);
    a_ << clrlwi(rs, rs, 24) << slwi(rs, rs, 2) << add(rs, rs, rPool);
    a_ << lfs(fsin, int16_t(sin_), rs) << lfs(fcos, int16_t(sin_ + 256), rs);
  };
  trig(0, 5, 3, 4);  // sa, ca
  trig(4, 6, 5, 6);  // sb, cb
  a_ << lfs(19, 8, 4);
  // Rotation: [ca sa*sb sa*cb; 0 cb -sb; -sa ca*sb ca*cb].
  a_ << fmuls(10, 3, 5) << fmuls(11, 3, 6) << fneg(12, 5) << fneg(13, 3) << fmuls(14, 4, 5) << fmuls(15, 4, 6);
  // Light in object space: R^T L.
  lf(16, rom::kLight[0]), lf(17, rom::kLight[1]), lf(18, rom::kLight[2]);
  a_ << fmuls(7, 4, 16) << fmadds(7, 13, 18, 7);
  a_ << fmuls(8, 10, 16) << fmadds(8, 6, 17, 8) << fmadds(8, 14, 18, 8);
  a_ << fmuls(9, 11, 16) << fmadds(9, 12, 17, 9) << fmadds(9, 15, 18, 9);
  // Projection: aspect 4:3, 60 degree vertical field, depth 1..40 mapped to 0..1.
  const float f = 1.7320508f, px = f * 0.75f, py = f, pz = 40.0f / (1.0f - 40.0f), pw = 40.0f / (1.0f - 40.0f);
  lf(20, px), lf(21, py), lf(22, pz);
  a_ << fmuls(20, 20, 19) << fmuls(21, 21, 19) << fmuls(22, 22, 19);
  auto m = [&](int i, u32 fr) { a_ << stfs(fr, int16_t(4 * i), rQve); };
  a_ << fmuls(0, 20, 4), m(0, 0);
  a_ << fmuls(0, 20, 10), m(1, 0);
  a_ << fmuls(0, 20, 11), m(2, 0);
  a_ << lfs(23, 12, 4);
  lf(0, px);
  a_ << fmuls(0, 0, 23), m(3, 0);
  m(4, 31);
  a_ << fmuls(0, 21, 6), m(5, 0);
  a_ << fmuls(0, 21, 12), m(6, 0);
  a_ << lfs(23, 16, 4);
  lf(0, py);
  a_ << fmuls(0, 0, 23), m(7, 0);
  a_ << fmuls(0, 22, 13), m(8, 0);
  a_ << fmuls(0, 22, 14), m(9, 0);
  a_ << fmuls(0, 22, 15), m(10, 0);
  a_ << lfs(23, 20, 4);
  lf(0, pz);
  lf(24, pw);
  a_ << fmadds(0, 0, 23, 24), m(11, 0);
  a_ << fmuls(0, 19, 3), m(12, 0);
  a_ << fmuls(0, 19, 14) << fneg(0, 0), m(13, 0);
  a_ << fmuls(0, 19, 15) << fneg(0, 0), m(14, 0);
  a_ << fneg(0, 23), m(15, 0);
  // QVE: transform and project every vertex into kProj.
  a_ << lwz(7, 0, 3) << lwz(8, 4, 3) << lwz(9, 8, 3) << addi(5, 3, 16);
  a_ << stw(5, 0x60, rQve);
  a_.li32(6, kProj);
  a_ << stw(6, 0x64, rQve) << stw(7, 0x68, rQve) << li(0, 16) << stw(0, 0x6C, rQve) << stw(0, 0x70, rQve);
  a_ << li(0, Qve::kXformProject) << stw(0, 0x74, rQve);
  a_.label("mesh_qve");
  a_ << lwz(0, 0x78, rQve) << cmpwi(0, 0, 0);
  a_.bne("mesh_qve");
  a_ << slwi(10, 7, 4) << add(5, 5, 10) << mtctr(8);
  lf(25, rom::kAmbient), lf(26, rom::kDiffuse);
  a_.li32(13, Spike::kTri << 24 | 3 * Spike::kVertexWords);  // triangle header
  a_.label("mesh_tri");
  a_ << lwz(10, 12, 5) << andi_(0, 9, rom::kMeshVertexColors);
  a_.beq("mesh_face");
  a_ << mr(11, 10) << lwz(7, 16, 5) << lwz(8, 20, 5);  // colours as given
  a_.b("mesh_emit");
  a_.label("mesh_face");
  a_ << andi_(0, 9, rom::kMeshUnlit);
  a_.bne("mesh_unlit");
  a_ << lfs(2, 16, 5) << lfs(3, 20, 5) << lfs(4, 24, 5);
  a_ << fmuls(27, 2, 7) << fmadds(27, 3, 8, 27) << fmadds(27, 4, 9, 27);
  a_ << fsel(27, 27, 27, 31) << fmadds(27, 27, 26, 25) << fmuls(27, 27, 1);
  a_.b("mesh_shade");
  a_.label("mesh_unlit");
  a_ << fmr(27, 1);
  a_.label("mesh_shade");
  shade(10, 27, 11, 12, 7, 8);
  a_ << mr(7, 11) << mr(8, 11);
  a_.label("mesh_emit");
  push(13);
  for (int v = 0; v < 3; ++v) {
    a_ << lwz(12, int16_t(4 * v), 5) << add(12, 12, 6);
    for (int w = 0; w < 4; ++w) a_ << lwz(0, int16_t(4 * w), 12) << stw(0, 0, rSpike);
    push(v == 0 ? 11 : v == 1 ? 7 : 8), pushf(31), pushf(31);
  }
  a_ << addi(5, 5, 28);
  a_.bdnz("mesh_tri");
  a_ << blr();

  // fade(f1 = 0..1): blends the screen towards black.
  a_.label("fade");
  a_ << fcmpu(0, 1, 31);
  a_.ble("fade_none");
  prologue();
  lf(2, 255.0f);
  a_ << fmuls(1, 1, 2);
  f2i(3, 1);
  a_ << cmpwi(0, 3, 255);
  a_.ble("fade_ok");
  a_ << li(3, 255);
  a_.label("fade_ok");
  a_ << slwi(3, 3, 24) << mr(4, 3);
  mode(kMBlend);
  lf(1, 0), lf(2, 0), lf(3, 640), lf(4, 480);
  a_.bl("quad");
  epilogue();
  a_.label("fade_none");
  a_ << blr();

  // seq_step: plays the sequencer events due this frame. A kSeqLoop event
  // restarts the sequence; at the end of the jingle the sound theme takes over.
  a_.label("seq_step");
  prologue();
  a_.bl("stream_update");  // streamed themes are fed from here too
  ram_ld(3, kSeqPtr);
  a_ << cmpwi(0, 3, 0);
  a_.beq("seq_done");
  ram_ld(4, kSeqT);
  a_.label("seq_loop");
  a_ << lwz(5, 0, 3) << cmplw(0, 5, 4);
  a_.bgt("seq_out");
  a_ << lwz(6, 4, 3) << li(0, -2) << cmpw(0, 6, 0);
  a_.beq("seq_restart");
  a_ << lwz(7, 8, 3) << stwx(7, rTyke, 6) << addi(3, 3, 12);
  a_.b("seq_loop");
  a_.label("seq_restart");
  ram_ld(3, kSeqLoop);
  a_ << li(4, 0);
  a_.b("seq_loop");
  a_.label("seq_out");
  ram_st(3, kSeqPtr);
  a_ << addi(4, 4, 1);
  ram_st(4, kSeqT);
  a_ << cmpwi(0, 5, -1);  // reached the end marker?
  a_.bne("seq_done");
  ram_set(kSeqPtr, 0);
  ram_ld(0, kSeqJingle);
  a_ << cmpwi(0, 0, 0);
  a_.beq("seq_done");
  ram_set(kSeqJingle, 0);
  ram_ld(0, kMusicState);
  a_ << cmpwi(0, 0, kMusicStopped);
  a_.bne("seq_done");
  a_.bl("theme_start");
  a_.label("seq_done");
  epilogue();

  // jingle: (re)starts the start-up jingle; the theme resumes when it ends.
  a_.label("jingle");
  a_.la(3, "jingle_events");
  ram_st(3, kSeqPtr);
  ram_st(3, kSeqLoop);
  ram_set(kSeqT, 0);
  ram_set(kSeqJingle, 1);
  a_ << blr();

  // theme_start: plays the selected sound theme from its beginning: its sequence,
  // and its music stream if it has one ({events, stream} per theme).
  a_.label("theme_start");
  ram_ld(3, kTheme);
  a_.la(4, "theme_table");
  a_ << slwi(3, 3, 3) << add(4, 4, 3) << lwz(3, 0, 4);
  ram_st(3, kSeqPtr);
  ram_st(3, kSeqLoop);
  ram_set(kSeqT, 0);
  ram_set(kSeqJingle, 0);
  a_ << lwz(3, 4, 4) << cmpwi(0, 3, 0);
  a_.bne("stream_start");  // tail calls
  a_.b("stream_stop");

  // silence: stops the sequencer and every voice.
  a_.label("silence");
  a_ << li(3, 0) << stw(3, kSeqPtr, rRam) << stw(3, int16_t(kTsActive), rRam) << addi(4, rTyke, 0x100 + 20)
     << li(5, Tyke::kChannels) << mtctr(5);
  a_.label("silence_loop");
  a_ << stw(3, 0, 4) << addi(4, 4, 32);
  a_.bdnz("silence_loop");
  a_ << blr();

  // sfx(r3 = {start, length, pitch, volume}): voices 21-23 in rotation.
  a_.label("sfx");
  auto voice_base = [&](u32 rch, u32 rd) {
    a_ << slwi(rd, rch, 5) << addi(rd, rd, 0x100) << add(rd, rd, rTyke);
  };
  a_ << lwz(4, kSfxCh, rRam) << addi(4, 4, 1) << cmpwi(0, 4, 24);
  a_.blt("sfx_rot");
  a_ << li(4, 21);
  a_.label("sfx_rot");
  a_ << stw(4, kSfxCh, rRam);
  voice_base(4, 5);
  a_ << lwz(6, 0, 3) << stw(6, 0, 5) << lwz(6, 4, 3) << stw(6, 4, 5) << stw(6, 8, 5);
  a_ << lwz(6, 8, 3) << stw(6, 12, 5) << lwz(6, 12, 3) << stw(6, 16, 5) << li(0, 1) << stw(0, 20, 5) << blr();

  // cd_async: tracks the drive without blocking. On a new disc it reads the
  // volume label, then scans the root directory for BOOT.LION and .WAV tracks.
  a_.label("cd_async");
  a_ << lwz(3, 0, rCd) << andi_(4, 3, Cdrom::kPresent);
  a_.bne("cda_present");
  a_ << li(0, 0);
  for (u32 off : {kDiscState, kHasGame, kTrackCount, kBootFailed}) ram_st(0, off);
  ram_ld(4, kCdOwner);
  a_ << cmpwi(0, 4, kOwnerDash);
  a_.bne("cda_done");
  ram_st(0, kCdOwner);
  a_.b("cda_done");
  a_.label("cda_present");
  ram_ld(5, kDiscState);
  a_ << cmpwi(0, 5, kNoDisc);
  a_.bne("cda_not_new");
  ram_ld(0, kCdOwner);
  a_ << cmpwi(0, 0, kOwnerNone);
  a_.bne("cda_done");  // the drive is busy
  a_ << li(0, 16) << stw(0, 8, rCd) << li(0, 1) << stw(0, 12, rCd);
  a_.li32(0, kSectorBuf);
  a_ << stw(0, 16, rCd) << li(0, Cdrom::kRead) << stw(0, 4, rCd);
  ram_set(kCdOwner, kOwnerDash);
  ram_set(kDiscState, kReading);
  a_.b("cda_done");
  a_.label("cda_not_new");
  a_ << cmpwi(0, 5, kReading);
  a_.beq("cda_pvd");
  a_ << cmpwi(0, 5, kReadingDir);
  a_.bne("cda_done");
  // Root directory arrived: scan it.
  a_ << andi_(4, 3, Cdrom::kDone);
  a_.beq("cda_done");
  a_ << li(0, Cdrom::kAck) << stw(0, 4, rCd);
  ram_set(kCdOwner, kOwnerNone);
  a_ << andi_(4, 3, Cdrom::kError);
  a_.bne("cda_bad");
  a_ << li(0, 0);
  ram_st(0, kHasGame);
  ram_st(0, kTrackCount);
  a_.li32(6, kDirBuf);
  ram_ld(7, kDirSize);
  a_ << add(7, 6, 7);
  a_.label("cda_scan");
  a_ << cmplw(0, 6, 7);
  a_.bge("cda_scanned");
  a_ << lbz(8, 0, 6) << cmpwi(0, 8, 0);
  a_.bne("cda_record");
  a_.li32(9, kDirBuf);  // records never cross sectors
  a_ << subf(10, 9, 6) << addi(10, 10, 2048) << rlwinm(10, 10, 0, 0, 20) << add(6, 9, 10);
  a_.b("cda_scan");
  a_.label("cda_record");
  a_ << lbz(9, 32, 6) << addi(10, 6, 33);  // name length, name
  // BOOT.LION;1?
  a_ << cmpwi(0, 9, 11);
  a_.bne("cda_not_boot");
  a_.la(11, "boot_name");
  a_ << li(12, 0);
  a_.label("cda_cmp_boot");
  a_ << lbzx(4, 10, 12) << lbzx(5, 11, 12) << cmpw(0, 4, 5);
  a_.bne("cda_not_boot");
  a_ << addi(12, 12, 1) << cmpwi(0, 12, 11);
  a_.blt("cda_cmp_boot");
  ram_set(kHasGame, 1);
  a_.label("cda_not_boot");
  // NAME.WAV or NAME.WAV;1?
  a_ << add(11, 10, 9);  // end of the name
  a_ << cmpwi(0, 9, 6);
  a_.blt("cda_next");
  a_ << lbz(4, -2, 11) << cmpwi(0, 4, ';');
  a_.bne("cda_no_version");
  a_ << addi(11, 11, -2);
  a_.label("cda_no_version");
  const char ext[4] = {'.', 'W', 'A', 'V'};
  for (int k = 0; k < 4; ++k) {
    a_ << lbz(4, int16_t(k - 4), 11) << cmpwi(0, 4, ext[k]);
    a_.bne("cda_next");
  }
  ram_ld(12, kTrackCount);
  a_ << cmpwi(0, 12, int16_t(kMaxTracks));
  a_.bge("cda_next");
  a_ << mulli(4, 12, int16_t(kTrackSize)) << add(4, 4, rRam) << addi(4, 4, int16_t(kTracks));
  a_ << li(5, 2) << lwbrx(5, 6, 5) << stw(5, 0, 4);   // LBA
  a_ << li(5, 10) << lwbrx(5, 6, 5) << stw(5, 4, 4);  // size
  // Name without ".WAV", at most 15 characters.
  a_ << addi(11, 11, -4) << subf(5, 10, 11) << cmpwi(0, 5, 15);
  a_.ble("cda_len_ok");
  a_ << li(5, 15);
  a_.label("cda_len_ok");
  a_ << addi(4, 4, 8) << cmpwi(0, 5, 0);
  a_.beq("cda_name_done");
  a_ << mtctr(5);
  a_.label("cda_name");
  a_ << lbz(0, 0, 10) << stb(0, 0, 4) << addi(10, 10, 1) << addi(4, 4, 1);
  a_.bdnz("cda_name");
  a_.label("cda_name_done");
  a_ << li(0, 0) << stb(0, 0, 4) << addi(12, 12, 1);
  ram_st(12, kTrackCount);
  a_.label("cda_next");
  a_ << add(6, 6, 8);
  a_.b("cda_scan");
  a_.label("cda_scanned");
  ram_set(kDiscState, kReady);
  a_.b("cda_done");
  // Volume descriptor arrived: keep the label and fetch the root directory.
  a_.label("cda_pvd");
  a_ << andi_(4, 3, Cdrom::kDone);
  a_.beq("cda_done");
  a_ << li(0, Cdrom::kAck) << stw(0, 4, rCd);
  ram_set(kCdOwner, kOwnerNone);
  a_ << andi_(4, 3, Cdrom::kError);
  a_.bne("cda_bad");
  a_.li32(6, kSectorBuf);
  a_ << lbz(7, 0, 6) << cmpwi(0, 7, 1);
  a_.bne("cda_bad");
  a_ << li(4, 158) << lwbrx(8, 6, 4) << li(4, 166) << lwbrx(9, 6, 4);  // root LBA, size
  a_ << addi(6, 6, 40) << addi(7, rRam, kLabel) << li(0, 32) << mtctr(0);
  a_.label("cda_copy");
  a_ << lbz(0, 0, 6) << stb(0, 0, 7) << addi(6, 6, 1) << addi(7, 7, 1);
  a_.bdnz("cda_copy");
  a_ << li(0, 0) << stb(0, 0, 7) << addi(10, rRam, kLabel);
  a_.label("cda_trim");
  a_ << addi(7, 7, -1) << lbz(0, 0, 7) << cmpwi(0, 0, 32);
  a_.bne("cda_trimmed");
  a_ << li(0, 0) << stb(0, 0, 7) << cmplw(0, 7, 10);
  a_.bgt("cda_trim");
  a_.label("cda_trimmed");
  a_.li32(0, kDirMax);
  a_ << cmplw(0, 9, 0);
  a_.ble("cda_dir_ok");
  a_ << mr(9, 0);
  a_.label("cda_dir_ok");
  ram_st(9, kDirSize);
  a_ << addi(9, 9, 2047) << srwi(9, 9, 11);
  a_ << stw(8, 8, rCd) << stw(9, 12, rCd);
  a_.li32(0, kDirBuf);
  a_ << stw(0, 16, rCd) << li(0, Cdrom::kRead) << stw(0, 4, rCd);
  ram_set(kCdOwner, kOwnerDash);
  ram_set(kDiscState, kReadingDir);
  a_.b("cda_done");
  a_.label("cda_bad");
  ram_set(kDiscState, kUnplayable);
  a_.label("cda_done");
  a_ << blr();

  // read_pad: r14 = buttons pressed since the last frame; held directions repeat.
  a_.label("read_pad");
  a_ << lwz(3, 0, rPad) << lwz(4, kPadPrev, rRam) << stw(3, kPadPrev, rRam) << andc(14, 3, 4);
  constexpr u32 kDirs = Pads::kUp | Pads::kDown | Pads::kLeft | Pads::kRight;
  a_ << andi_(5, 3, kDirs) << andi_(6, 4, kDirs) << cmpw(0, 5, 6) << li(7, 0);
  a_.bne("pad_hold_reset");
  a_ << cmpwi(0, 5, 0);
  a_.beq("pad_hold_reset");
  ram_ld(7, kHold);
  a_ << addi(7, 7, 1) << cmpwi(0, 7, 20);
  a_.blt("pad_hold_reset");
  a_ << andi_(0, 7, 3);
  a_.bne("pad_hold_reset");
  a_ << or_(14, 14, 5);  // auto-repeat every 4 frames after 1/3 s
  a_.label("pad_hold_reset");
  ram_st(7, kHold);
  a_ << blr();

  // poly4t(r3 = 4 x {x, y, u, v}, r4 = ARGB): textured quad.
  a_.label("poly4t");
  for (int k = 0; k < 4; ++k)
    for (int c = 0; c < 4; ++c) a_ << lfs(u32(1 + 4 * k + c), int16_t(16 * k + 4 * c), 3);
  tri_header();
  vtxt(1, 2, 4, 3, 4), vtxt(5, 6, 4, 7, 8), vtxt(9, 10, 4, 11, 12);
  tri_header();
  vtxt(1, 2, 4, 3, 4), vtxt(9, 10, 4, 11, 12), vtxt(13, 14, 4, 15, 16);
  a_ << blr();

  // poly4c(r3 = 4 x {x, y, ARGB}): Gouraud quad.
  a_.label("poly4c");
  for (int k = 0; k < 4; ++k) a_ << lfs(u32(1 + 2 * k), int16_t(12 * k), 3) << lfs(u32(2 + 2 * k), int16_t(12 * k + 4), 3)
                                 << lwz(u32(5 + k), int16_t(12 * k + 8), 3);
  tri_header();
  vtx(1, 2, 5), vtx(3, 4, 6), vtx(5, 6, 7);
  tri_header();
  vtx(1, 2, 5), vtx(5, 6, 7), vtx(7, 8, 8);
  a_ << blr();

  // strcpy(r3 = source, r4 = destination) -> r4 at the copied NUL.
  a_.label("strcpy");
  a_.label("strcpy_loop");
  a_ << lbz(0, 0, 3) << stb(0, 0, 4) << cmpwi(0, 0, 0);
  a_.beq("strcpy_done");
  a_ << addi(3, 3, 1) << addi(4, 4, 1);
  a_.b("strcpy_loop");
  a_.label("strcpy_done");
  a_ << blr();

  // fmt2(r3 = 0..99, r4 = destination): two digits and a NUL; r4 advances by 2.
  a_.label("fmt2");
  a_ << li(5, 10) << divwu(6, 3, 5) << mullw(7, 6, 5) << subf(7, 7, 3);
  a_ << addi(6, 6, '0') << addi(7, 7, '0') << stb(6, 0, 4) << stb(7, 1, 4) << li(0, 0) << stb(0, 2, 4) << addi(4, 4, 2)
     << blr();

  // fmt_time(r3 = seconds, r4 = destination): "MM:SS"; r4 advances by 5.
  a_.label("fmt_time");
  prologue();
  a_ << li(5, 60) << divwu(8, 3, 5) << mullw(9, 8, 5) << subf(9, 9, 3) << cmpwi(0, 8, 99);
  a_.ble("fmt_time_ok");
  a_ << li(8, 99);
  a_.label("fmt_time_ok");
  a_ << mr(3, 8);
  a_.bl("fmt2");
  a_ << li(0, ':') << stb(0, 0, 4) << addi(4, 4, 1) << mr(3, 9);
  a_.bl("fmt2");
  epilogue();

  // fmt_pct(r3 = 0..256, r4 = destination): "NNN%" right-aligned.
  a_.label("fmt_pct");
  a_ << mulli(3, 3, 100) << srwi(3, 3, 8) << li(5, 10);
  for (int d = 2; d >= 0; --d) {
    a_ << divwu(6, 3, 5) << mullw(7, 6, 5) << subf(7, 7, 3) << addi(7, 7, '0') << stb(7, int16_t(d), 4) << mr(3, 6);
  }
  a_ << lbz(0, 0, 4) << cmpwi(0, 0, '0');  // blank leading zeros
  a_.bne("pct_done");
  a_ << li(0, ' ') << stb(0, 0, 4) << lbz(0, 1, 4) << cmpwi(0, 0, '0');
  a_.bne("pct_done");
  a_ << li(0, ' ') << stb(0, 1, 4);
  a_.label("pct_done");
  a_ << li(0, '%') << stb(0, 3, 4) << li(0, 0) << stb(0, 4, 4) << blr();
}

// ------------------------------------------------------------------ intro

void RomBuilder::loading() {
  a_.label("loading");
  a_ << li(0, kSceneLoading) << stw(0, kScene, rRam);
  a_.bl("music_stop");
  a_.bl("silence");
  a_ << li(3, 3) << li(4, 0);
  a_.bl("begin_frame");
  mode(kMText);
  text("s_loading", 320, 222, 20, 0xFFE6DCD2, true);
  a_ << lwz(0, kDiscState, rRam) << cmpwi(0, 0, kReady);
  a_.bne("loading_nolabel");
  a_ << addi(3, rRam, kLabel);
  load(4, 0xFFFFB040);
  lf(1, 320), lf(2, 256), lf(3, 14);
  a_.bl("textc");
  a_.label("loading_nolabel");
  mode(kMAdd);
  rect(220, 286, 420, 288, 0xFFA01818, 0xFFA01818);
  a_.bl("end_frame");
  a_.b("loader");
}

// ------------------------------------------------------------------ loader

void RomBuilder::loader() {
  Asm& a = a_;
  a.label("loader");
  a.li32(31, map::kCd);
  a.li32(29, map::kVi);
  a.li32(28, map::kPic);
  // Let a label read from the dashboard finish first.
  a.label("loader_idle");
  a << lwz(3, 0, 31) << andi_(3, 3, Cdrom::kBusy);
  a.bne("loader_idle");
  a << li(3, Cdrom::kAck) << stw(3, 4, 31);

  // Primary volume descriptor (sector 16).
  a << li(3, 16) << li(4, 1);
  a.li32(5, kSectorBuf);
  a.bl("cd_read");
  a << cmpwi(0, 3, 0);
  a.bne("boot_fail");
  a.li32(20, kSectorBuf);
  a << lbz(3, 0, 20) << cmpwi(0, 3, 1);  // type 1 = primary
  a.bne("boot_fail");
  a << li(4, 158) << lwbrx(21, 20, 4);   // root directory LBA (little-endian half)
  a << li(4, 166) << lwbrx(22, 20, 4);   // root directory size
  a << addi(4, 22, 2047) << srwi(4, 4, 11);
  a << cmplwi(0, 4, 8);
  a.ble("dir_ok");
  a << li(4, 8);
  a.label("dir_ok");
  a << mr(3, 21);
  a.li32(5, kSectorBuf);
  a.bl("cd_read");
  a << cmpwi(0, 3, 0);
  a.bne("boot_fail");

  // Scan the root directory for "BOOT.LION;1".
  a.li32(6, kSectorBuf);
  a << add(7, 6, 22);  // end of directory data
  a.label("scan");
  a << cmplw(0, 6, 7);
  a.bge("boot_fail");
  a << lbz(8, 0, 6) << cmpwi(0, 8, 0);
  a.bne("record");
  // Zero length: records never cross sectors, skip to the next one.
  a.li32(9, kSectorBuf);
  a << subf(10, 9, 6) << addi(10, 10, 2048) << rlwinm(10, 10, 0, 0, 20) << add(6, 9, 10);
  a.b("scan");
  a.label("record");
  a << lbz(9, 32, 6) << cmpwi(0, 9, 11);
  a.bne("next_record");
  a.la(10, "boot_name");
  a << addi(11, 6, 33) << li(12, 11) << mtctr(12);
  a.label("cmp_name");
  a << lbz(13, 0, 10) << lbz(14, 0, 11) << cmpw(0, 13, 14);
  a.bne("next_record");
  a << addi(10, 10, 1) << addi(11, 11, 1);
  a.bdnz("cmp_name");
  a.b("found");
  a.label("next_record");
  a << add(6, 6, 8);
  a.b("scan");

  a.label("found");
  a << li(4, 2) << lwbrx(23, 6, 4);   // file LBA
  a << li(4, 10) << lwbrx(24, 6, 4);  // file size
  a << addi(4, 24, 2047) << srwi(4, 4, 11);
  a.li32(5, kMaxFileSectors);
  a << cmplw(0, 4, 5);
  a.bgt("boot_fail");
  a << mr(3, 23);
  a.li32(5, kFileBuf);
  a.bl("cd_read");
  a << cmpwi(0, 3, 0);
  a.bne("boot_fail");

  // Check the .lion header and copy the segments.
  a.li32(20, kFileBuf);
  a << lwz(3, 0, 20);
  a.li32(4, 0x4C49'4F4E);
  a << cmplw(0, 3, 4);
  a.bne("boot_fail");
  a << lwz(3, 4, 20) << cmpwi(0, 3, 1);
  a.bne("boot_fail");
  a << lwz(25, 8, 20) << lwz(26, 12, 20);  // entry, segment count
  a << addi(27, 20, 16);                   // segment table
  a.label("segment");
  a << cmpwi(0, 26, 0);
  a.beq("enter");
  a << lwz(3, 0, 27) << lwz(4, 4, 27) << lwz(5, 8, 27);  // address, size, file offset
  a << add(5, 5, 20);
  a << cmpwi(0, 4, 0);
  a.beq("segment_done");
  a << mtctr(4);
  a.label("copy");
  a << lbz(6, 0, 5) << stb(6, 0, 3) << addi(5, 5, 1) << addi(3, 3, 1);
  a.bdnz("copy");
  a.label("segment_done");
  a << addi(27, 27, 12) << addi(26, 26, -1);
  a.b("segment");

  // Hand over a quiet machine: interrupts masked, every voice off. The Tyke
  // settings (volume, tone, echo) stay as the player left them.
  a.label("enter");
  a.li32(3, map::kPic);
  a << li(0, 0) << stw(0, 4, 3) << li(0, -1) << stw(0, 0, 3);
  a.li32(3, map::kTyke + 0x100 + 20);
  a << li(0, 0) << li(4, Tyke::kChannels) << mtctr(4);
  a.label("enter_quiet");
  a << stw(0, 0, 3) << addi(3, 3, 32);
  a.bdnz("enter_quiet");
  a.li32(1, kStack);
  a << mtspr(26, 25);  // SRR0 = entry
  a.li32(3, Tuffy::kMsrMe | Tuffy::kMsrFp | Tuffy::kMsrVec);
  a << mtspr(27, 3) << li(3, 0) << ppcasm::sync() << isync() << rfi();

  // Not bootable: back to the dashboard, which explains it.
  a.label("boot_fail");
  globals();
  video_init();
  // The disc is looked at again (it may still hold music); its game is marked as failed.
  a << li(0, kNoDisc) << stw(0, kDiscState, rRam) << li(0, 1) << stw(0, kBootFailed, rRam);
  a.la(3, "sfx_back");
  a.bl("sfx");
  a.b("dash_enter");

  // Unexpected exception: red screen.
  a.label("fatal");
  a.li32(29, map::kVi);
  a << li(3, kRed);
  a.bl("fill_screen");
  a.label("halt");
  a.b("halt");

  // cd_read(r3 = lba, r4 = count, r5 = dma) -> r3 = 0 or error bits. Polls the drive.
  a.label("cd_read");
  a << stw(3, 8, 31) << stw(4, 12, 31) << stw(5, 16, 31) << li(10, Cdrom::kRead) << stw(10, 4, 31);
  a.label("cd_poll");
  a << lwz(10, 0, 31) << andi_(11, 10, Cdrom::kDone);
  a.beq("cd_poll");
  a << li(11, Cdrom::kAck) << stw(11, 4, 31) << andi_(3, 10, Cdrom::kError) << blr();

  // fill_screen(r3 = RGB565): 240p framebuffer at the start of SGRAM.
  a.label("fill_screen");
  a.li32(10, map::kSgramBase);
  a << li(11, 1) << stw(11, 0, 29) << stw(10, 4, 29) << li(11, 640) << stw(11, 8, 29);
  a.li32(11, 320 * 240);
  a << mtctr(11);
  a.label("fill");
  a << sth(3, 0, 10) << addi(10, 10, 2);
  a.bdnz("fill");
  a << blr();
}

// ------------------------------------------------------------------ data

void RomBuilder::data() {
  a_.align(8);
  a_.label("pool");
  for (u32 w : pool_.words()) a_.word(w);

  const std::pair<const char*, const char*> strings[] = {
      {"s_atari", "ATARI"}, {"s_lion", "LION"}, {"s_a2", "A2"}, {"s_tagline", "INTERACTIVE ENTERTAINMENT SYSTEM"},
      {"s_card0", "PLAY"}, {"s_card1", "MUSIC"}, {"s_card2", "THEME"}, {"s_card3", "TYKE"}, {"s_card4", "SYSTEM"},
      {"s_insert", "INSERT A LION DISC"}, {"s_reading", "READING DISC..."}, {"s_play", "PLAY DISC"},
      {"s_nogame", "NO GAME ON THIS DISC"}, {"s_unplayable", "THIS DISC CANNOT BE PLAYED"},
      {"s_music_desc", "CD AUDIO PLAYER"}, {"s_music_desc2", "PLAY .WAV TRACKS FROM A DISC"},
      {"s_theme_desc", "SOUND THEME"}, {"s_tyke_desc", "TYKE AUDIO"}, {"s_tyke_desc2", "VOLUME  BASS  TREBLE  ECHO"},
      {"s_system_desc", "SYSTEM INFORMATION"}, {"s_system_desc2", "TUFFY  QUACKER  SPIKE  TYKE"},
      {"s_sysinfo", "SYSTEM INFORMATION"}, {"s_cdaudio", "CD AUDIO"}, {"s_noaudio", "INSERT AN AUDIO DISC"},
      {"s_notracks", "NO .WAV TRACKS ON THIS DISC"}, {"s_badwav", "UNSUPPORTED .WAV FILE"},
      {"s_st0", "STOPPED"}, {"s_st1", "PLAYING"}, {"s_st2", "PAUSED"}, {"s_st3", "LOADING"},
      {"s_theme_title", "SOUND THEME"}, {"s_tyke_title", "TYKE AUDIO"}, {"s_row0", "VOLUME"}, {"s_row1", "BASS"},
      {"s_row2", "TREBLE"}, {"s_row3", "ECHO"}, {"s_spectrum", "SPECTRUM"}, {"s_scope", "OUTPUT"},
      {"s_voices", "VOICES"}, {"s_now", "NOW PLAYING  "}, {"s_slash", " / "}, {"s_active", "*"},
      {"s_hint_main", "<  >  CHOOSE      (A) SELECT"}, {"s_hint_back", "(B) BACK"},
      {"s_hint_music", "UP DOWN  CHOOSE   (A) PLAY/PAUSE   (X) STOP   (B) BACK"},
      {"s_hint_theme", "UP DOWN  CHOOSE    (A) SELECT    (B) BACK"},
      {"s_hint_tyke", "UP DOWN  CHOOSE   < >  ADJUST   (A) JINGLE   (B) BACK"}, {"s_loading", "LOADING"},
  };
  for (auto [label, text] : strings) str(label, text);
  const char* lines[][2] = {{"CPU", "TUFFY  POWERPC 7400  500 MHZ"}, {"GEOMETRY", "QUACKER  MC88110  100 MHZ"},
                            {"GRAPHICS", "SPIKE  2 PIPELINES  90 MHZ"},  {"AUDIO", "TYKE DSP  24 VOICES  40 MHZ"},
                            {"MEMORY", "24 MB SDRAM + 8 MB SGRAM"},     {"STORAGE", "12X CD-ROM"},
                            {"BOOT ROM", "VERSION 1.1  REV A2"}};
  for (int i = 0; i < 7; ++i) {
    str("s_sys_k" + std::to_string(i), lines[i][0]);
    str("s_sys_v" + std::to_string(i), lines[i][1]);
  }
  for (u32 t = 0; t < rom::kThemeCount; ++t) {
    str("s_theme_name" + std::to_string(t), rom::kThemeNames[t]);
    str("s_theme_blurb" + std::to_string(t), rom::kThemeBlurbs[t]);
  }
  a_.label("theme_names");
  for (u32 t = 0; t < rom::kThemeCount; ++t) a_.addr_word("s_theme_name" + std::to_string(t));
  a_.label("state_names");
  for (int k = 0; k < 4; ++k) a_.addr_word("s_st" + std::to_string(k));

  auto blob = [&](const std::string& label, const std::vector<u32>& w) {
    a_.align(16);
    a_.label(label);
    for (u32 x : w) a_.word(x);
  };
  blob("mesh_emblem", rom::mesh_emblem());
  blob("mesh_emblem_small", rom::mesh_emblem(true));
  blob("mesh_crown", rom::mesh_crown());
  blob("mesh_disc", rom::mesh_disc());
  blob("mesh_note", rom::mesh_note());
  blob("mesh_audio", rom::mesh_audio());
  blob("mesh_mixer", rom::mesh_mixer());
  blob("mesh_chip", rom::mesh_chip());
  blob("mesh_grid", rom::mesh_grid());
  {
    std::vector<u32> t;
    for (float f : rom::intro_table()) t.push_back(f32_to_bits(f));
    blob("intro_tab", t);
  }
  blob("plate_tab", rom::plate_table());
  blob("slash_tab", rom::slash_table());
  blob("jingle_events", rom::jingle_events(bank_));
  for (u32 t = 0; t < rom::kThemeCount; ++t) blob("theme_events" + std::to_string(t), rom::theme_events(bank_, t));
  a_.label("theme_table");
  const rom::LowLayout low = rom::low_layout();
  for (u32 t = 0; t < rom::kThemeCount; ++t) {
    a_.addr_word("theme_events" + std::to_string(t));
    const int stream = rom::theme_stream(t);
    a_.word(stream < 0 ? 0 : low.theme[stream]);
  }
  // IMA ADPCM tables for the stream decoder.
  a_.align(4);
  a_.label("ima_step");
  for (int i = 0; i < 89; i += 2) a_.word(u32(u16(ima::kStep[i])) << 16 | (i + 1 < 89 ? u16(ima::kStep[i + 1]) : 0u));
  a_.label("ima_index");
  for (int v : ima::kIndex) a_.word(u32(v));
  blob("sfx_tick", rom::sfx_record(bank_.tick, 1.0, 0x70, 0x70));
  blob("sfx_ok", rom::sfx_record(bank_.ok, 1.0, 0x90, 0x90));
  blob("sfx_back", rom::sfx_record(bank_.back, 1.0, 0x90, 0x90));
  blob("tyke_fw", tyke_mixer_firmware());
  a_.label("boot_name");
  str("boot_name_str", "BOOT.LION;1");

  if (a_.here() > map::kRomVectors + rom::kFontOffset) throw std::runtime_error("boot ROM: code and data overflow the font");
  code_end_ = a_.here();
  auto bytes = [&](const std::vector<u8>& b) {
    for (size_t i = 0; i < b.size(); i += 4) a_.word(u32(b[i]) << 24 | u32(b[i + 1]) << 16 | u32(b[i + 2]) << 8 | b[i + 3]);
  };
  while (a_.here() < map::kRomVectors + rom::kFontOffset) a_.word(0);
  a_.label("font");
  bytes(rom::font_texture());
  while (a_.here() < map::kRomVectors + rom::kJaguarOffset) a_.word(0);
  a_.label("jaguar");
  bytes(rom::jaguar_texture());
  if (a_.here() > map::kRomVectors + rom::kSampleOffset) throw std::runtime_error("boot ROM: textures overflow");
  while (a_.here() < map::kRomVectors + rom::kSampleOffset) a_.word(0);
  if (rom::kSampleOffset + bank_.pcm.size() > map::kRomSize) throw std::runtime_error("boot ROM: samples overflow");
  bytes(bank_.pcm);
}

std::vector<u8> RomBuilder::build() {
  // Tables first, so the code can address them.
  magic_ = pool_.d(0x4330'0000'8000'0000ull);
  {
    std::vector<float> s(320);
    for (int i = 0; i < 320; ++i) s[size_t(i)] = float(std::sin(2.0 * 3.14159265358979323846 * i / 256.0));
    sin_ = pool_.table(s);
    std::vector<float> c;
    for (int i = 0; i <= 24; ++i) {
      const double t = 2.0 * 3.14159265358979323846 * i / 24.0;
      c.push_back(float(std::cos(t)));
      c.push_back(float(std::sin(t)));
    }
    circle_ = pool_.table(c);
    std::vector<float> uv;
    for (int ch = 0; ch < 128; ++ch) {
      uv.push_back(float(ch % 16) / 16.0f);
      uv.push_back(float(ch / 16) / 8.0f);
    }
    uv_ = pool_.table(uv);
    // Visualizer: Hann window and Goertzel coefficients for 16 log-spaced bands (120 Hz - 12 kHz).
    std::vector<float> hann, coeff;
    for (int i = 0; i < 256; ++i) hann.push_back(float(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / 255.0)));
    for (int b = 0; b < kVizBands; ++b) {
      const double hz = 120.0 * std::pow(100.0, b / double(kVizBands - 1));
      coeff.push_back(float(2.0 * std::cos(2.0 * 3.14159265358979323846 * hz / kAudioRate)));
    }
    hann_ = pool_.table(hann);
    goertzel_ = pool_.table(coeff);
  }
  vectors();
  reset();
  routines();
  audio_routines();
  intro();
  dashboard();
  loading();
  loader();
  data();
  // The last megabyte holds the code; the converted audio goes below it.
  const std::vector<u8> code = a_.finish();
  std::vector<u8> rom(map::kRomSize, 0);
  if (code.size() > map::kRomBase + map::kRomSize - map::kRomVectors) throw std::runtime_error("boot ROM: code too large");
  std::copy(code.begin(), code.end(), rom.begin() + (map::kRomVectors - map::kRomBase));
  const rom::LowLayout low = rom::low_layout();
  if (low.end > map::kRomVectors) throw std::runtime_error("boot ROM: audio overflows into the code");
  auto put = [&](u32 addr, const rom::Blob& b) { std::copy(b.data, b.data + b.size, rom.begin() + (addr - map::kRomBase)); };
  for (int t = 0; t < rom::kStreamThemes; ++t) put(low.theme[t], rom::theme_ladp(t));
  put(low.roar, rom::roar_pcm());
  put(low.claws, rom::claws_pcm());
  return rom;
}

}  // namespace romgen

std::vector<u8> build_boot_rom() {
  static const std::vector<u8> rom = romgen::RomBuilder().build();  // the same for every console
  return rom;
}

}  // namespace lion
