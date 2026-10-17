#pragma once
// "Apex Cube" demo: exercises the whole A2 pipeline.
//   CD-ROM : when booted from disc, TEX.BIN (a mipmapped texture) is DMA'd into SGRAM.
//   Tuffy  : reads the pad, builds the rotation with its FPU, sends a command to Quacker,
//            dozes until Spike signals the frame, flips the framebuffer on VBlank.
//   Quacker: loads the matrix into the QVE, transforms the cube, feeds triangles to Spike.
//   Spike  : clears, Z-tests and Gouraud-shades the cube.

#include <cmath>

#include "bus.h"
#include "cdrom.h"
#include "iso9660.h"
#include "loader.h"
#include "m88kasm.h"
#include "ppcasm.h"
#include "pad.h"
#include "quacker/qve.h"
#include "spike/spike.h"
#include "tuffy/ppc.h"

namespace demo {

constexpr uint32_t kHandler = 0x500;
constexpr uint32_t kTuffyMain = 0x0001'0000;
constexpr uint32_t kData = 0x0002'0000;  // sin table, R, PT, M
constexpr uint32_t kVblankCount = 0x3000;
constexpr uint32_t kQuackerMain = 0x0008'0000;
constexpr uint32_t kBufA = lion::map::kSgramBase;
constexpr uint32_t kBufB = lion::map::kSgramBase + 640 * 480 * 2;
constexpr uint32_t kZBuf = lion::map::kSgramBase + 0x20'0000;
constexpr uint32_t kBackground = 0x1082;  // dark blue-grey RGB565
constexpr uint32_t kDataR = 0x400, kDataPT = 0x440, kDataM = 0x480;
constexpr uint32_t kTexture = lion::map::kSgramBase + 0x40'0000;
constexpr uint32_t kTexLog2 = 6, kTexLevels = 7;  // 64x64 down to 1x1
constexpr uint32_t kTexturedMode = lion::Spike::kTexture | lion::Spike::kBilinear | lion::Spike::kPerspective |
                                   lion::Spike::kMipmap | lion::Spike::kTrilinear;

// 64x64 RGB565 texture (Atari-red checker with a light grid) and its box-filtered mip chain.
inline std::vector<uint8_t> make_texture() {
  std::vector<uint32_t> level(64 * 64);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x) {
      const bool check = ((x / 8) ^ (y / 8)) & 1;
      const bool grid = x % 8 == 0 || y % 8 == 0;
      level[size_t(y * 64 + x)] = grid ? 0xF0F0F0 : check ? 0xE01818 : 0x202020;
    }
  std::vector<uint8_t> out;
  for (int size = 64; size >= 1; size /= 2) {
    for (uint32_t c : level) {
      const uint16_t p = uint16_t(((c >> 19) & 31) << 11 | ((c >> 10) & 63) << 5 | ((c >> 3) & 31));
      out.push_back(uint8_t(p >> 8));
      out.push_back(uint8_t(p));
    }
    if (size == 1) break;
    const int half = size / 2;
    std::vector<uint32_t> next(size_t(half * half));
    for (int y = 0; y < half; ++y)
      for (int x = 0; x < half; ++x) {
        uint32_t acc[3] = {};
        for (int k = 0; k < 4; ++k) {
          const uint32_t c = level[size_t((y * 2 + k / 2) * size + x * 2 + k % 2)];
          for (int ch = 0; ch < 3; ++ch) acc[ch] += (c >> (16 - 8 * ch)) & 0xFF;
        }
        next[size_t(y * half + x)] = (acc[0] / 4) << 16 | (acc[1] / 4) << 8 | (acc[2] / 4);
      }
    level = std::move(next);
  }
  return out;
}

inline std::vector<uint8_t> words_to_bytes(const std::vector<float>& f) {
  std::vector<uint8_t> out;
  for (float x : f) {
    uint32_t w;
    __builtin_memcpy(&w, &x, 4);
    for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(w >> s));
  }
  return out;
}

inline std::vector<uint8_t> tuffy_handler() {
  using namespace ppcasm;
  Asm a(kHandler);
  // Save scratch registers in SPRG0-2; no CR-modifying instructions here.
  a << mtspr(272, 3) << mtspr(273, 4) << mtspr(274, 5);
  a << lis(3, lion::map::kPic >> 16);
  a << lwz(4, 0, 3) << stw(4, 0, 3);    // acknowledge everything pending
  a << rlwinm(4, 4, 0, 31, 31);         // VBLANK bit
  a << li(3, kVblankCount) << lwz(5, 0, 3) << add(5, 5, 4) << stw(5, 0, 3);
  a << mfspr(5, 274) << mfspr(4, 273) << mfspr(3, 272);
  a << rfi();
  return a.finish();
}

inline std::vector<uint8_t> tuffy_main(uint32_t tex_lba, uint32_t tex_sectors) {
  using namespace ppcasm;
  Asm a(kTuffyMain);
  a.li32(31, lion::map::kPic);
  a.li32(30, lion::map::kVi);
  a.li32(29, lion::map::kQuackerCtrl);
  a.li32(28, lion::map::kSpike);
  a.li32(27, lion::map::kPad);
  a.li32(26, kData);

  // Video: 480p from buffer A.
  a << li(3, 3) << stw(3, 0, 30);
  a.li32(3, kBufA);
  a << stw(3, 4, 30) << li(3, 1280) << stw(3, 8, 30);
  // Start Quacker.
  a.li32(3, kQuackerMain);
  a << stw(3, 4, 29) << li(3, 1) << stw(3, 0, 29);
  // Interrupts: VBLANK + SPIKE + CD.
  a << li(3, 0x23) << stw(3, 4, 31);

  a << li(25, 0) << li(24, 0) << li(23, 0x100);
  a.li32(20, lion::Tuffy::kMsrMe | lion::Tuffy::kMsrFp | lion::Tuffy::kMsrVec | lion::Tuffy::kMsrEe);
  a.li32(21, lion::Tuffy::kMsrMe | lion::Tuffy::kMsrFp | lion::Tuffy::kMsrVec | lion::Tuffy::kMsrEe |
                 lion::Tuffy::kMsrPow);
  a << mtmsr(20);

  // With a disc in the drive, stream the texture into SGRAM and draw textured.
  a << li(19, 0);
  a.li32(18, lion::map::kCd);
  a << lwz(3, 0, 18) << andi_(3, 3, lion::Cdrom::kPresent);
  a.beq("no_disc");
  a.li32(3, tex_lba);
  a << stw(3, 8, 18);
  a.li32(3, tex_sectors);
  a << stw(3, 12, 18);
  a.li32(3, kTexture);
  a << stw(3, 16, 18) << li(3, lion::Cdrom::kRead) << stw(3, 4, 18);
  a.label("cd_wait");
  a << mtmsr(21);  // doze until the drive interrupts
  a << lwz(3, 0, 18) << andi_(4, 3, lion::Cdrom::kDone);
  a.beq("cd_wait");
  a << li(4, lion::Cdrom::kAck) << stw(4, 4, 18);
  a << andi_(3, 3, lion::Cdrom::kError);
  a.bne("no_disc");
  a.li32(19, kTexturedMode);
  a.label("no_disc");

  a.label("frame");
  a << addi(25, 25, 1);
  // Input: Left/Right change the speed, A stops.
  a << lwz(3, 0, 27);
  a << andi_(4, 3, lion::Pads::kLeft);
  a.beq("no_left");
  a << addi(23, 23, -16);
  a.label("no_left");
  a << andi_(4, 3, lion::Pads::kRight);
  a.beq("no_right");
  a << addi(23, 23, 16);
  a.label("no_right");
  a << andi_(4, 3, lion::Pads::kA);
  a.beq("no_a");
  a << li(23, 0);
  a.label("no_a");
  a << add(24, 24, 23);

  // Angles: a = (angle >> 8) & 255, b = (angle >> 9) & 255.
  a << rlwinm(5, 24, 24, 24, 31) << rlwinm(6, 24, 23, 24, 31);
  auto load_sin = [&](u32 freg, u32 idx, int offset) {
    a << addi(7, idx, int16_t(offset)) << clrlwi(7, 7, 24) << slwi(7, 7, 2) << add(8, 26, 7) << lfs(freg, 0, 8);
  };
  load_sin(1, 5, 0);   // sa
  load_sin(2, 5, 64);  // ca
  load_sin(3, 6, 0);   // sb
  load_sin(4, 6, 64);  // cb

  // R = Ry(a) * Rx(b); zeros and R[15] = 1 are preset in the data segment.
  a << addi(9, 26, kDataR);
  a << stfs(2, 0, 9);
  a << fmuls(5, 1, 3) << stfs(5, 4, 9);
  a << fmuls(5, 1, 4) << stfs(5, 8, 9);
  a << stfs(4, 20, 9);
  a << fneg(5, 3) << stfs(5, 24, 9);
  a << fneg(5, 1) << stfs(5, 32, 9);
  a << fmuls(5, 2, 3) << stfs(5, 36, 9);
  a << fmuls(5, 2, 4) << stfs(5, 40, 9);

  // M = PT * R.
  a << addi(10, 26, kDataPT) << addi(11, 26, kDataR) << addi(12, 26, kDataM);
  a << li(13, 0);
  a.label("mrow");
  a << li(14, 0);
  a.label("mcol");
  a << add(15, 10, 13) << add(16, 11, 14);
  a << lfs(1, 0, 15) << lfs(2, 0, 16) << fmuls(0, 1, 2);
  a << lfs(1, 4, 15) << lfs(2, 16, 16) << fmadds(0, 1, 2, 0);
  a << lfs(1, 8, 15) << lfs(2, 32, 16) << fmadds(0, 1, 2, 0);
  a << lfs(1, 12, 15) << lfs(2, 48, 16) << fmadds(0, 1, 2, 0);
  a << add(17, 12, 13) << add(17, 17, 14) << stfs(0, 0, 17);
  a << addi(14, 14, 4) << cmpwi(0, 14, 16);
  a.blt("mcol");
  a << addi(13, 13, 16) << cmpwi(0, 13, 64);
  a.blt("mrow");

  // Back buffer = frame parity.
  a.li32(11, kBufA);
  a << andi_(10, 25, 1);
  a.beq("use_a");
  a.li32(11, kBufB);
  a.label("use_a");

  // Command to Quacker: DRAW, matrix, framebuffer, token, extra Spike mode bits.
  a << li(12, 1) << stw(12, 8, 29) << addi(12, 26, kDataM) << stw(12, 8, 29) << stw(11, 8, 29) << stw(25, 8, 29)
    << stw(19, 8, 29);

  // Doze until Spike reports this frame's SYNC.
  a.label("wait_sync");
  a << mtmsr(21);
  a << lwz(12, 8, 28) << cmpw(0, 12, 25);
  a.bne("wait_sync");
  a << stw(11, 4, 30);  // flip

  // Pace to the next VBlank.
  a << li(3, kVblankCount) << lwz(13, 0, 3);
  a.label("wait_vb");
  a << mtmsr(21);
  a << lwz(14, 0, 3) << cmpw(0, 14, 13);
  a.beq("wait_vb");
  a.b("frame");
  a.label("halt");
  a.b("halt");
  return a.finish();
}

inline std::vector<uint8_t> tuffy_data() {
  std::vector<float> d(0x4C0 / 4, 0.0f);
  for (int i = 0; i < 256; ++i) d[size_t(i)] = float(std::sin(2.0 * M_PI * i / 256.0));
  d[kDataR / 4 + 15] = 1.0f;
  // PT = Perspective(60°, 4:3, near 1, far 10, depth 0..1) * Translate(0, 0, -4).
  const float f = 1.0f / std::tan(float(M_PI) / 6.0f), aspect = 640.0f / 480.0f, n = 1.0f, fa = 10.0f;
  float P[16] = {f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, fa / (n - fa), n * fa / (n - fa), 0, 0, -1, 0};
  float T[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -4, 0, 0, 0, 1};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += P[i * 4 + k] * T[k * 4 + j];
      d[kDataPT / 4 + size_t(i * 4 + j)] = s;
    }
  return words_to_bytes(d);
}

inline std::vector<uint8_t> quacker_program() {
  using namespace m88kasm;
  using lion::Spike;
  Asm a(kQuackerMain);
  a.label("start");
  a.li32(30, lion::map::kQuackerCtrl);
  a.li32(29, lion::map::kQve);
  a.li32(28, lion::map::kSpike);
  a.li32(27, lion::map::kSramBase);
  // Viewport: scale (320, -240), offset (320, 240).
  const float vp[4] = {320.0f, -240.0f, 320.0f, 240.0f};
  for (int i = 0; i < 4; ++i) {
    uint32_t w;
    __builtin_memcpy(&w, &vp[i], 4);
    a.li32(2, w);
    a << st(2, 29, 0x40 + 4 * u32(i));
  }

  a.label("loop");
  a << ld(2, 30, 8) << ld(3, 30, 8) << ld(4, 30, 8) << ld(5, 30, 8) << ld(19, 30, 8);  // cmd, matrix, fb, token, mode
  // Matrix -> QVE.
  a << or_i(6, 0, 0);
  a.label("copy");
  a << ld_r(7, 3, 6) << st_r(7, 29, 6) << addu_i(6, 6, 4) << cmp_i(8, 6, 64);
  a.bb1(kLt, 8, "copy");
  // QVE: project the 8 cube vertices into SRAM.
  a.la(9, "verts");
  a << st(9, 29, 0x60) << st(27, 29, 0x64) << or_i(10, 0, 8) << st(10, 29, 0x68);
  a << or_i(10, 0, lion::Qve::kXformProject) << st(10, 29, 0x74);

  // Spike setup while the QVE works.
  auto push_const = [&](uint32_t v) {
    a.li32(10, v);
    a << st(10, 28, 0);
  };
  push_const(Spike::kSetTarget << 24 | 5);
  a << st(4, 28, 0);
  push_const(1280);
  push_const(640);
  push_const(480);
  push_const(kZBuf);
  push_const(Spike::kClear << 24 | 3);
  push_const(3);
  push_const(kBackground);
  push_const(0xFFFFFF);
  push_const(Spike::kSetMode << 24 | 1);
  a.li32(10, Spike::kZTest | Spike::kZWrite | Spike::kGouraud | Spike::kCullBack);
  a << or_(10, 10, 19) << st(10, 28, 0);
  a.bb0(3, 19, "no_texture");  // bit 3 = kTexture
  push_const(Spike::kSetTexture << 24 | 4);
  push_const(kTexture);
  push_const(kTexLog2);
  push_const(kTexLog2);
  push_const(kTexLevels);
  a.label("no_texture");

  a.label("qve_wait");
  a << ld(10, 29, 0x78);
  a.bcnd(kNe0, 10, "qve_wait");

  // 12 triangles x 3 vertices.
  a.la(11, "tris");
  a.la(12, "colors");
  a.la(18, "uvs");
  a << or_i(13, 0, 12);
  a.label("tri");
  push_const(Spike::kTri << 24 | 21);
  a << or_i(14, 0, 3);
  a.label("vtx");
  a << ld(15, 11, 0) << addu_i(11, 11, 4);
  a << mak(16, 15, 0, 4) << addu(16, 16, 27);  // &projected[idx]
  for (u32 off = 0; off < 16; off += 4) a << ld(17, 16, off) << st(17, 28, 0);
  a << ld_r(17, 12, 15, true) << st(17, 28, 0);  // color
  a << ld(17, 18, 0) << st(17, 28, 0) << ld(17, 18, 4) << st(17, 28, 0) << addu_i(18, 18, 8);  // u, v
  a << subu_i(14, 14, 1);
  a.bcnd(kNe0, 14, "vtx");
  a << subu_i(13, 13, 1);
  a.bcnd(kNe0, 13, "tri");

  push_const(Spike::kSync << 24 | 1);
  a << st(5, 28, 0);
  a.br("loop");

  a.align(16);
  a.label("verts");
  for (int i = 0; i < 8; ++i) {
    a.f32(i & 1 ? 1.0f : -1.0f);
    a.f32(i & 2 ? 1.0f : -1.0f);
    a.f32(i & 4 ? 1.0f : -1.0f);
    a.f32(1.0f);
  }
  a.label("tris");
  // Counter-clockwise seen from outside (Y up).
  const uint32_t tris[36] = {4, 5, 7, 4, 7, 6,  1, 0, 2, 1, 2, 3,  5, 1, 3, 5, 3, 7,
                             0, 4, 6, 0, 6, 2,  6, 7, 3, 6, 3, 2,  0, 1, 5, 0, 5, 4};
  for (uint32_t t : tris) a.word(t);
  a.label("colors");
  for (uint32_t i = 0; i < 8; ++i)
    a.word(0xFF00'0000u | (i & 1 ? 0xFF0000u : 0x400000u) | (i & 2 ? 0xFF00u : 0x4000u) | (i & 4 ? 0xFFu : 0x40u));
  // Each face is (q0 q1 q2)(q0 q2 q3): the same texture corners for every quad.
  a.label("uvs");
  const float uv[12] = {0, 0, 2, 0, 2, 2, 0, 0, 2, 2, 0, 2};  // texture repeats twice per face
  for (int t = 0; t < 12; ++t)
    for (int k = 0; k < 6; ++k) a.f32(uv[(t & 1) * 6 + k]);
  return a.finish();
}

inline lion::LionImage build(uint32_t tex_lba = 0, uint32_t tex_sectors = 0) {
  lion::LionImage img;
  img.entry = kTuffyMain;
  img.segments.push_back({kHandler, tuffy_handler()});
  img.segments.push_back({kTuffyMain, tuffy_main(tex_lba, tex_sectors)});
  img.segments.push_back({kData, tuffy_data()});
  img.segments.push_back({kQuackerMain, quacker_program()});
  return img;
}

// Bootable disc: BOOT.LION followed by TEX.BIN. The image size does not depend on the
// LBA constants, so a first pass gives the layout.
inline std::vector<uint8_t> build_disc() {
  const auto tex = make_texture();
  const auto probe = build().serialize();
  const auto lba = iso9660::layout({{"BOOT.LION", probe}, {"TEX.BIN", tex}});
  const auto boot = build(lba[1], iso9660::sectors(tex.size())).serialize();
  return iso9660::build("APEX_CUBE", {{"BOOT.LION", boot}, {"TEX.BIN", tex}});
}

}  // namespace demo
