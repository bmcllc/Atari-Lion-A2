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
#include "bootrom.h"

#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>

#include "bus.h"
#include "cdrom.h"
#include "pad.h"
#include "quacker/qve.h"
#include "ppcasm.h"
#include "rom/assets.h"
#include "spike/spike.h"
#include "tuffy/ppc.h"
#include "tyke/firmware.h"
#include "tyke/tyke.h"

namespace lion {

namespace {

using namespace ppcasm;
using namespace bootrom;

constexpr u32 kSectorBuf = 0x00C0'0000;  // volume descriptor and directory
constexpr u32 kFileBuf = 0x00D0'0000;    // BOOT.LION image
constexpr u32 kMaxFileSectors = 3000;    // ~6 MB
constexpr u32 kStack = map::kSdramBase + map::kSdramSize - 0x100;
constexpr u32 kRed = 0xF800;

constexpr u32 kBufA = map::kSgramBase, kBufB = map::kSgramBase + 640 * 480 * 2;
constexpr u32 kZBuf = map::kSgramBase + 0x20'0000;
constexpr u32 kFont = map::kSgramBase + 0x40'0000;
constexpr u32 kProj = kRam + 0x1'0000;  // QVE output

// Dashboard RAM (offsets from kRam, see bootrom.h for the public ones).
constexpr u32 kScratch = 0x00, kXform = 0x10, kFade = 0x40, kScroll = 0x44, kEmblemYaw = 0x48, kIconYaw = 0x4C,
              kCardScale = 0x58, kCardBright = 0x64, kSeqPtr = 0x70, kSeqT = 0x74, kSfxCh = 0x78, kPadPrev = 0x90;

// Global registers, valid everywhere outside the loader.
constexpr u32 rSpike = 31, rVi = 30, rPic = 29, rCd = 28, rTyke = 27, rPool = 26, rRam = 25, rFrame = 24, rBack = 23,
              rQve = 16, rPad = 15;
// f31 = 0.0, f30 = 1.0. Routines may clobber r0, r3-r12, f0-f27, CR and CTR.

constexpr u32 kBaseMsr = Tuffy::kMsrIp | Tuffy::kMsrMe | Tuffy::kMsrFp;
constexpr u32 kDozeMsr = kBaseMsr | Tuffy::kMsrEe | Tuffy::kMsrPow;

constexpr u32 kM2D = Spike::kGouraud;
constexpr u32 kMBlend = Spike::kGouraud | Spike::kBlend;
constexpr u32 kMAdd = Spike::kGouraud | Spike::kBlendAdd;
constexpr u32 kMText = Spike::kTexture | Spike::kBilinear | Spike::kBlendAdd;
constexpr u32 kM3D = Spike::kZTest | Spike::kZWrite | Spike::kCullBack;
constexpr u32 kMGrid = Spike::kBlendAdd;

// Constant pool addressed from rPool with 16-bit offsets.
class Pool {
 public:
  u32 f(float v) {
    const u32 bits = f32_to_bits(v);
    auto it = floats_.find(bits);
    if (it != floats_.end()) return it->second;
    const u32 off = u32(words_.size() * 4);
    words_.push_back(bits);
    floats_[bits] = off;
    return off;
  }
  u32 d(u64 bits) {
    if (words_.size() % 2) words_.push_back(0);
    const u32 off = u32(words_.size() * 4);
    words_.push_back(u32(bits >> 32));
    words_.push_back(u32(bits));
    return off;
  }
  u32 table(const std::vector<float>& t) {
    const u32 off = u32(words_.size() * 4);
    for (float v : t) words_.push_back(f32_to_bits(v));
    return off;
  }
  const std::vector<u32>& words() const { return words_; }

 private:
  std::vector<u32> words_;
  std::map<u32, u32> floats_;
};

class RomBuilder {
 public:
  std::vector<u8> build();

 private:
  // ---- emit helpers
  std::string L(const std::string& base) { return base + "#" + std::to_string(uid_++); }
  int16_t k(float v) { return int16_t(pool_.f(v)); }
  void lf(u32 fr, float v) { a_ << lfs(fr, k(v), rPool); }
  void load(u32 r, u32 v) {
    if (s32(v) >= -32768 && s32(v) < 32768) a_ << li(r, int16_t(v));
    else a_.li32(r, v);
  }
  void push(u32 r) { a_ << stw(r, 0, rSpike); }
  void pushf(u32 fr) { a_ << stfs(fr, 0, rSpike); }
  void pushk(u32 v) {
    load(0, v);
    push(0);
  }
  void tri_header() { pushk(Spike::kTri << 24 | 3 * Spike::kVertexWords); }
  void vtx(u32 fx, u32 fy, u32 rc) {
    pushf(fx), pushf(fy), pushf(31), pushf(30), push(rc), pushf(31), pushf(31);
  }
  void vtxt(u32 fx, u32 fy, u32 rc, u32 fu, u32 fv) {
    pushf(fx), pushf(fy), pushf(31), pushf(30), push(rc), pushf(fu), pushf(fv);
  }
  void mode(u32 m) {
    pushk(Spike::kSetMode << 24 | 1);
    pushk(m);
  }
  // fd = float(rs); uses r0, f0 and the scratch slot.
  void i2f(u32 fd, u32 rs) {
    a_ << xoris(0, rs, 0x8000) << stw(0, kScratch + 4, rRam) << lis(0, 0x4330) << stw(0, kScratch, rRam);
    a_ << lfd(fd, kScratch, rRam) << lfd(0, int16_t(magic_), rPool) << fsub(fd, fd, 0) << frsp(fd, fd);
  }
  // rd = int(fs) (truncated); uses f0 and the scratch slot.
  void f2i(u32 rd, u32 fs) { a_ << fctiwz(0, fs) << stfd(0, kScratch, rRam) << lwz(rd, kScratch + 4, rRam); }
  // rd = rc with RGB scaled by fI (0..1, saturating); alpha kept.
  void shade(u32 rc, u32 fI, u32 rd, u32 rk, u32 t1, u32 t2) {
    const std::string hi = L("shade_hi"), lo = L("shade_lo");
    lf(0, 256.0f);
    a_ << fmuls(0, fI, 0);
    f2i(rk, 0);
    a_ << cmpwi(0, rk, 256);
    a_.ble(hi);
    a_ << li(rk, 256);
    a_.label(hi);
    a_ << cmpwi(0, rk, 0);
    a_.bge(lo);
    a_ << li(rk, 0);
    a_.label(lo);
    a_ << rlwinm(t1, rc, 16, 24, 31) << mullw(t1, t1, rk) << rlwinm(t1, t1, 8, 8, 15);
    a_ << rlwinm(t2, rc, 24, 24, 31) << mullw(t2, t2, rk) << rlwinm(t2, t2, 0, 16, 23) << or_(t1, t1, t2);
    a_ << clrlwi(t2, rc, 24) << mullw(t2, t2, rk) << srwi(t2, t2, 8) << or_(t1, t1, t2);
    a_ << rlwinm(t2, rc, 0, 0, 7) << or_(rd, t1, t2);
  }
  void prologue() { a_ << mflr(0) << stwu(1, -16, 1) << stw(0, 8, 1); }
  void epilogue() { a_ << lwz(0, 8, 1) << addi(1, 1, 16) << mtlr(0) << blr(); }
  // Calls `routine` with f1..f4 = constants.
  void rect(float x0, float y0, float x1, float y1, u32 top, u32 bottom) {
    lf(1, x0), lf(2, y0), lf(3, x1), lf(4, y1);
    load(3, top), load(4, bottom);
    a_.bl("quad");
  }
  void text(const std::string& str, float x, float y, float size, u32 argb, bool centered) {
    a_.la(3, str);
    load(4, argb);
    lf(1, x), lf(2, y), lf(3, size);
    a_.bl(centered ? "textc" : "text");
  }
  // Text whose colour is `argb` scaled by the float at `off(base)`.
  void text_faded(const std::string& str, float x, float y, float size, u32 argb, bool centered, u32 base, int16_t off) {
    load(3, argb);
    a_ << lfs(1, off, base);
    a_.bl("shade");
    a_ << mr(4, 3);
    a_.la(3, str);
    lf(1, x), lf(2, y), lf(3, size);
    a_.bl(centered ? "textc" : "text");
  }
  void str(const std::string& label, const std::string& s) {
    a_.label(label);
    std::vector<u8> b(s.begin(), s.end());
    b.push_back(0);
    while (b.size() % 4) b.push_back(0);
    for (size_t i = 0; i < b.size(); i += 4) a_.word(u32(b[i]) << 24 | u32(b[i + 1]) << 16 | u32(b[i + 2]) << 8 | b[i + 3]);
  }
  // Pressed-button test on r14: branches to `skip` when none of `mask` is pressed.
  void unless_pressed(u32 mask, const std::string& skip) {
    a_ << andi_(0, 14, mask);
    a_.beq(skip);
  }

  void globals();
  void video_init();
  void vectors();
  void reset();
  void routines();
  void intro();
  void dashboard();
  void loading();
  void loader();
  void data();

  Asm a_{map::kRomBase};
  Pool pool_;
  int uid_ = 0;
  u32 magic_ = 0, sin_ = 0, circle_ = 0, uv_ = 0;
  rom::SoundBank bank_ = rom::sound_bank();
};

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
  while (a_.here() < map::kRomBase + 0x100) a_.word(0);
  a_.b("start");  // 0x100: reset
  for (u32 v = 0x200; v < 0x1000; v += 0x100) {
    while (a_.here() < map::kRomBase + v) a_.word(0);
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
  while (a_.here() < map::kRomBase + 0x1000) a_.word(0);
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
  a_ << li(0, 0) << li(3, 0x100 / 4) << mtctr(3) << mr(3, rRam);
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
  a_.label("mesh_tri");
  a_ << lwz(10, 12, 5) << andi_(0, 9, rom::kMeshUnlit);
  a_.bne("mesh_unlit");
  a_ << lfs(2, 16, 5) << lfs(3, 20, 5) << lfs(4, 24, 5);
  a_ << fmuls(27, 2, 7) << fmadds(27, 3, 8, 27) << fmadds(27, 4, 9, 27);
  a_ << fsel(27, 27, 27, 31) << fmadds(27, 27, 26, 25) << fmuls(27, 27, 1);
  a_.b("mesh_shade");
  a_.label("mesh_unlit");
  a_ << fmr(27, 1);
  a_.label("mesh_shade");
  shade(10, 27, 11, 12, 7, 8);
  tri_header();
  for (int v = 0; v < 3; ++v) {
    a_ << lwz(12, int16_t(4 * v), 5) << add(12, 12, 6);
    for (int w = 0; w < 4; ++w) a_ << lwz(0, int16_t(4 * w), 12) << stw(0, 0, rSpike);
    push(11), pushf(31), pushf(31);
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

  // seq_step: plays the sequencer events due this frame.
  a_.label("seq_step");
  a_ << lwz(3, kSeqPtr, rRam) << cmpwi(0, 3, 0);
  a_.beq("seq_done");
  a_ << lwz(4, kSeqT, rRam);
  a_.label("seq_loop");
  a_ << lwz(5, 0, 3) << cmplw(0, 5, 4);
  a_.bgt("seq_out");
  a_ << lwz(6, 4, 3) << lwz(7, 8, 3) << stwx(7, rTyke, 6) << addi(3, 3, 12);
  a_.b("seq_loop");
  a_.label("seq_out");
  a_ << stw(3, kSeqPtr, rRam) << addi(4, 4, 1) << stw(4, kSeqT, rRam);
  a_.label("seq_done");
  a_ << blr();

  // jingle: (re)starts the start-up jingle.
  a_.label("jingle");
  a_.la(3, "jingle_events");
  a_ << stw(3, kSeqPtr, rRam) << li(0, 0) << stw(0, kSeqT, rRam) << blr();

  // silence: stops the sequencer and every voice.
  a_.label("silence");
  a_ << li(3, 0) << stw(3, kSeqPtr, rRam) << addi(4, rTyke, 0x100 + 20) << li(5, Tyke::kChannels) << mtctr(5);
  a_.label("silence_loop");
  a_ << stw(3, 0, 4) << addi(4, 4, 32);
  a_.bdnz("silence_loop");
  a_ << blr();

  // sfx(r3 = {start, length, pitch, volume}): voices 21-23 in rotation; the next
  // one is keyed off so it can retrigger.
  a_.label("sfx");
  auto voice_base = [&](u32 rch, u32 rd) {
    a_ << slwi(rd, rch, 5) << addi(rd, rd, 0x100) << add(rd, rd, rTyke);
  };
  auto rotate = [&](u32 r) {
    const std::string ok = L("sfx_rot");
    a_ << addi(r, r, 1) << cmpwi(0, r, 24);
    a_.blt(ok);
    a_ << li(r, 21);
    a_.label(ok);
  };
  a_ << lwz(4, kSfxCh, rRam);
  rotate(4);
  a_ << stw(4, kSfxCh, rRam);
  voice_base(4, 5);
  a_ << lwz(6, 0, 3) << stw(6, 0, 5) << lwz(6, 4, 3) << stw(6, 4, 5) << stw(6, 8, 5);
  a_ << lwz(6, 8, 3) << stw(6, 12, 5) << lwz(6, 12, 3) << stw(6, 16, 5) << li(0, 1) << stw(0, 20, 5);
  rotate(4);
  voice_base(4, 5);
  a_ << li(0, 0) << stw(0, 20, 5) << blr();

  // cd_async: tracks the drive and reads the volume label without blocking.
  a_.label("cd_async");
  a_ << lwz(3, 0, rCd) << andi_(4, 3, Cdrom::kPresent);
  a_.bne("cda_present");
  a_ << li(0, kNoDisc) << stw(0, kDiscState, rRam) << blr();
  a_.label("cda_present");
  a_ << lwz(5, kDiscState, rRam) << cmpwi(0, 5, kNoDisc);
  a_.bne("cda_reading");
  a_ << li(0, 16) << stw(0, 8, rCd) << li(0, 1) << stw(0, 12, rCd);
  a_.li32(0, kSectorBuf);
  a_ << stw(0, 16, rCd) << li(0, Cdrom::kRead) << stw(0, 4, rCd);
  a_ << li(0, kReading) << stw(0, kDiscState, rRam) << blr();
  a_.label("cda_reading");
  a_ << cmpwi(0, 5, kReading);
  a_.bne("cda_done");
  a_ << andi_(4, 3, Cdrom::kDone);
  a_.beq("cda_done");
  a_ << li(0, Cdrom::kAck) << stw(0, 4, rCd) << andi_(4, 3, Cdrom::kError);
  a_.bne("cda_bad");
  a_.li32(6, kSectorBuf);
  a_ << lbz(7, 0, 6) << cmpwi(0, 7, 1);
  a_.bne("cda_bad");
  a_ << addi(6, 6, 40) << addi(7, rRam, kLabel) << li(0, 32) << mtctr(0);
  a_.label("cda_copy");
  a_ << lbz(0, 0, 6) << stb(0, 0, 7) << addi(6, 6, 1) << addi(7, 7, 1);
  a_.bdnz("cda_copy");
  a_ << li(0, 0) << stb(0, 0, 7) << addi(8, rRam, kLabel);
  a_.label("cda_trim");
  a_ << addi(7, 7, -1) << lbz(0, 0, 7) << cmpwi(0, 0, 32);
  a_.bne("cda_trimmed");
  a_ << li(0, 0) << stb(0, 0, 7) << cmplw(0, 7, 8);
  a_.bgt("cda_trim");
  a_.label("cda_trimmed");
  a_ << li(0, kReady) << stw(0, kDiscState, rRam) << blr();
  a_.label("cda_bad");
  a_ << li(0, kUnplayable) << stw(0, kDiscState, rRam);
  a_.label("cda_done");
  a_ << blr();

  // read_pad: r14 = buttons pressed since the last frame.
  a_.label("read_pad");
  a_ << lwz(3, 0, rPad) << lwz(4, kPadPrev, rRam) << stw(3, kPadPrev, rRam) << andc(14, 3, 4) << blr();
}

// ------------------------------------------------------------------ intro

void RomBuilder::intro() {
  using rom::kIntroColumns;
  a_.label("intro");
  a_ << li(0, kSceneIntro) << stw(0, kScene, rRam);
  a_.bl("jingle");
  a_ << li(17, 0);  // frame
  a_.la(18, "intro_tab");
  a_.label("intro_loop");
  a_.bl("read_pad");
  unless_pressed(Pads::kStart | Pads::kA, "intro_noskip");
  a_ << cmpwi(0, 17, 206);
  a_.bge("intro_noskip");
  a_ << li(17, 206);
  a_.label("intro_noskip");
  a_.bl("cd_async");
  a_ << slwi(0, 17, 6) << add(19, 18, 0);  // row (16 floats)
  static_assert(kIntroColumns == 16);

  a_ << li(3, 3) << li(4, 0);
  a_.bl("begin_frame");
  // Glow behind the mark: a wide red halo and a hot core.
  mode(kMAdd);
  for (auto [rx, ry, color] : {std::tuple{330.0f, 240.0f, 0xFFB01414u}, std::tuple{130.0f, 120.0f, 0xFFFF6A2Au}}) {
    load(3, color);
    a_ << lfs(1, 28, 19);
    a_.bl("shade");
    a_ << lis(4, 0xFF00);
    a_ << lfs(5, 28, 19);
    lf(6, rx);
    a_ << fmuls(3, 5, 6);
    lf(6, ry);
    a_ << fmuls(4, 5, 6);
    lf(1, 320), lf(2, 190);
    a_.bl("fan");
  }
  mode(kM3D);
  a_.la(3, "mesh_emblem");
  a_ << mr(4, 19) << lfs(1, 24, 19);
  a_.bl("mesh");
  mode(kMText);
  text_faded("s_atari", 190, 330, 36, 0xFFF4F0EA, false, 19, 32);
  text_faded("s_lion", 352, 330, 36, 0xFFE81E1E, false, 19, 36);
  text_faded("s_tagline", 320, 384, 12, 0xFFB4A8A0, true, 19, 40);
  // Flash on the hit.
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
  a_ << addi(17, 17, 1) << cmpwi(0, 17, int16_t(rom::kIntroFrames));
  a_.blt("intro_loop");

  // A disc in the drive boots straight away, as on the retail units.
  a_ << lwz(3, 0, rCd) << andi_(3, 3, Cdrom::kPresent);
  a_.bne("loading");
  a_.b("dash_enter");
}

// ------------------------------------------------------------------ dashboard

void RomBuilder::dashboard() {
  constexpr float kCardX[3] = {160, 320, 480};
  const char* meshes[3] = {"mesh_disc", "mesh_audio", "mesh_chip"};
  const char* labels[3] = {"s_disc", "s_audio", "s_system"};

  a_.label("dash_enter");
  a_ << li(0, kSceneDashboard) << stw(0, kScene, rRam);
  a_ << stfs(30, kFade, rRam) << li(0, 0) << stw(0, kSelection, rRam) << stw(0, kSubmenu, rRam)
     << stw(0, kLaunch, rRam);
  lf(1, 0.7f);
  lf(2, 0.4f);
  for (int i = 0; i < 3; ++i) a_ << stfs(1, int16_t(kCardScale + 4 * i), rRam) << stfs(2, int16_t(kCardBright + 4 * i), rRam);
  a_.bl("read_pad");  // swallow the button that got us here

  a_.label("dash_loop");
  a_.bl("read_pad");
  a_.bl("cd_async");
  a_ << lwz(20, kSelection, rRam) << lwz(19, kSubmenu, rRam);
  a_ << lwz(0, kLaunch, rRam) << cmpwi(0, 0, 0);
  a_.bne("dash_input_done");
  a_ << cmpwi(0, 19, kSubNone);
  a_.bne("dash_in_sub");
  // Main row: left/right moves, A or Start activates.
  unless_pressed(Pads::kLeft, "dash_no_left");
  a_ << cmpwi(0, 20, 0);
  a_.beq("dash_no_left");
  a_ << addi(20, 20, -1);
  a_.la(3, "sfx_tick");
  a_.bl("sfx");
  a_.label("dash_no_left");
  unless_pressed(Pads::kRight, "dash_no_right");
  a_ << cmpwi(0, 20, 2);
  a_.beq("dash_no_right");
  a_ << addi(20, 20, 1);
  a_.la(3, "sfx_tick");
  a_.bl("sfx");
  a_.label("dash_no_right");
  unless_pressed(Pads::kA | Pads::kStart, "dash_input_done");
  a_ << cmpwi(0, 20, kItemDisc);
  a_.bne("dash_not_disc");
  a_ << lwz(0, kDiscState, rRam) << cmpwi(0, 0, kReady);
  a_.bne("dash_disc_unavailable");
  a_ << li(0, 1) << stw(0, kLaunch, rRam);
  a_.la(3, "sfx_ok");
  a_.bl("sfx");
  a_.b("dash_input_done");
  a_.label("dash_disc_unavailable");
  a_.la(3, "sfx_back");
  a_.bl("sfx");
  a_.b("dash_input_done");
  a_.label("dash_not_disc");
  a_.la(3, "sfx_ok");
  a_.bl("sfx");
  a_ << cmpwi(0, 20, kItemAudio);
  a_.bne("dash_open_system");
  a_ << li(19, kSubAudio);
  a_.bl("jingle");
  a_.b("dash_input_done");
  a_.label("dash_open_system");
  a_ << li(19, kSubSystem);
  a_.b("dash_input_done");
  // Panels: B closes, A replays the jingle in the sound test.
  a_.label("dash_in_sub");
  unless_pressed(Pads::kB, "dash_sub_no_b");
  a_ << li(19, kSubNone);
  a_.la(3, "sfx_back");
  a_.bl("sfx");
  a_.b("dash_input_done");
  a_.label("dash_sub_no_b");
  a_ << cmpwi(0, 19, kSubAudio);
  a_.bne("dash_input_done");
  unless_pressed(Pads::kA, "dash_input_done");
  a_.bl("jingle");
  a_.label("dash_input_done");
  a_ << stw(20, kSelection, rRam) << stw(19, kSubmenu, rRam);

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
  // Angle wrap helper: x += step, minus 256 past a full turn.
  auto spin = [&](u32 off, float step) {
    const std::string ok = L("spin");
    a_ << lfs(1, int16_t(off), rRam);
    lf(2, step);
    a_ << fadds(1, 1, 2);
    lf(2, 256.0f);
    a_ << fcmpu(0, 1, 2);
    a_.blt(ok);
    a_ << fsubs(1, 1, 2);
    a_.label(ok);
    a_ << stfs(1, int16_t(off), rRam);
  };
  // Grid scroll.
  {
    a_ << lfs(1, kScroll, rRam);
    lf(2, 0.04f);
    a_ << fadds(1, 1, 2);
    lf(2, rom::kGridStep);
    a_ << fcmpu(0, 1, 2);
    a_.blt("dash_scroll_ok");
    a_ << fsubs(1, 1, 2);
    a_.label("dash_scroll_ok");
    a_ << stfs(1, kScroll, rRam);
  }
  spin(kEmblemYaw, 0.6f);
  // Cards ease towards their targets; the selected icon spins.
  for (int i = 0; i < 3; ++i) {
    const std::string other = L("card_other"), set = L("card_set"), go = L("card_go");
    a_ << cmpwi(0, 20, i);
    a_.bne(other);
    lf(2, 1.0f), lf(3, 1.3f);
    a_.b(set);
    a_.label(other);
    lf(2, 0.72f), lf(3, 0.45f);
    a_.label(set);
    a_ << cmpwi(0, 19, kSubNone);
    a_.beq(go);
    lf(3, 0.25f);
    a_.label(go);
    lf(5, 0.18f);
    for (auto [off, target] : {std::pair{kCardScale + 4 * i, 2u}, std::pair{kCardBright + 4 * i, 3u}}) {
      a_ << lfs(1, int16_t(off), rRam) << fsubs(4, target, 1) << fmadds(1, 4, 5, 1) << stfs(1, int16_t(off), rRam);
    }
    // The selected icon sways to show itself off; the others settle facing front.
    const std::string still = L("icon_still");
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

  // ---- render
  // Black clear, then only the bands that are not black: fill rate is precious.
  a_ << li(3, 3) << li(4, 0);
  a_.bl("begin_frame");
  mode(kM2D);
  rect(0, 84, 640, 240, 0xFF000000, 0xFF3E0B0D);   // sky glowing towards the horizon
  rect(0, 240, 640, 330, 0xFF140305, 0xFF000000);  // floor
  // Floor grid rolling towards the viewer.
  mode(kMGrid);
  a_ << stfs(31, kXform, rRam) << stfs(31, kXform + 4, rRam) << stfs(30, kXform + 8, rRam)
     << stfs(31, kXform + 12, rRam);
  lf(1, -1.3f);
  a_ << stfs(1, kXform + 16, rRam) << lfs(1, kScroll, rRam) << stfs(1, kXform + 20, rRam);
  a_.la(3, "mesh_grid");
  a_ << addi(4, rRam, kXform) << fmr(1, 30);
  a_.bl("mesh");
  mode(kMBlend);
  rect(0, 240, 640, 300, 0xD03E0B0D, 0x003E0B0D);  // haze on the horizon

  // Header: the Lion mark, the name and a rule.
  mode(kM3D);
  {
    a_ << lfs(1, kEmblemYaw, rRam) << stfs(1, kXform, rRam) << stfs(31, kXform + 4, rRam);
    const float t[4] = {0.42f, -4.85f, 3.45f, -7.5f};
    for (int i = 0; i < 4; ++i) {
      lf(1, t[i]);
      a_ << stfs(1, int16_t(kXform + 8 + 4 * i), rRam);
    }
    a_.la(3, "mesh_emblem");
    a_ << addi(4, rRam, kXform) << fmr(1, 30);
    a_.bl("mesh");
  }
  mode(kMText);
  text("s_atari", 100, 30, 24, 0xFFF4F0EA, false);
  text("s_lion", 208, 30, 24, 0xFFE81E1E, false);
  mode(kMAdd);
  rect(100, 66, 600, 68, 0xFFA01818, 0xFF300808);

  // Cards.
  for (int i = 0; i < 3; ++i) {
    const float cx = kCardX[i];
    const std::string no_glow = L("card_noglow");
    mode(kMBlend);
    rect(cx - 72, 128, cx + 72, 320, 0xB0300A0A, 0xB0080404);
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
    lf(1, cx), lf(2, 214), lf(3, 105), lf(4, 115);
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
    for (auto [x0, y0, x1, y1] : {std::tuple{cx - 72, 128.0f, cx + 72, 130.0f}, std::tuple{cx - 72, 318.0f, cx + 72, 320.0f},
                                  std::tuple{cx - 72, 128.0f, cx - 70, 320.0f}, std::tuple{cx + 70, 128.0f, cx + 72, 320.0f}}) {
      lf(1, x0), lf(2, y0), lf(3, x1), lf(4, y1);
      a_.bl("quad");
    }
    // Icon.
    mode(kM3D);
    a_ << lfs(1, int16_t(kIconYaw + 4 * i), rRam) << stfs(1, kXform, rRam);
    lf(1, -12.0f);
    a_ << stfs(1, kXform + 4, rRam) << lfs(1, int16_t(kCardScale + 4 * i), rRam);
    lf(2, 0.8f);
    a_ << fmuls(1, 1, 2) << stfs(1, kXform + 8, rRam);
    const float t[3] = {float(i - 1) * 2.3094f, 0.40f, -6.0f};
    for (int j = 0; j < 3; ++j) {
      lf(1, t[j]);
      a_ << stfs(1, int16_t(kXform + 12 + 4 * j), rRam);
    }
    a_.la(3, meshes[i]);
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
    a_.la(3, labels[i]);
    lf(1, cx), lf(2, 286), lf(3, 16);
    a_.bl("textc");
  }

  // Description of the selected item: r21 = first line, r22 = second line (amber) or 0.
  {
    a_ << li(22, 0) << cmpwi(0, 20, kItemDisc);
    a_.bne("desc_not_disc");
    a_ << lwz(0, kDiscState, rRam);
    const std::pair<u32, const char*> disc_lines[] = {
        {kNoDisc, "s_insert"}, {kReading, "s_reading"}, {kReady, "s_play"}, {kUnplayable, "s_unplayable"}};
    for (auto [state, label] : disc_lines) {
      const std::string next = L("desc_state");
      a_ << cmpwi(0, 0, int16_t(state));
      a_.bne(next);
      a_.la(21, label);
      if (state == kReady) a_ << addi(22, rRam, kLabel);
      a_.b("desc_draw");
      a_.label(next);
    }
    a_.label("desc_not_disc");
    a_ << cmpwi(0, 20, kItemAudio);
    a_.bne("desc_system");
    a_.la(21, "s_audio_desc");
    a_.la(22, "s_audio_desc2");
    a_.b("desc_draw");
    a_.label("desc_system");
    a_.la(21, "s_system_desc");
    a_.la(22, "s_system_desc2");
    a_.label("desc_draw");
    mode(kMText);
    a_ << mr(3, 21);
    load(4, 0xFFE6DCD2);
    lf(1, 320), lf(2, 356), lf(3, 14);
    a_.bl("textc");
    a_ << cmpwi(0, 22, 0);
    a_.beq("desc_one_line");
    a_ << mr(3, 22);
    load(4, 0xFFFFB040);
    lf(1, 320), lf(2, 380), lf(3, 14);
    a_.bl("textc");
    a_.label("desc_one_line");
  }

  // Panels.
  a_ << cmpwi(0, 19, kSubNone);
  a_.beq("dash_no_panel");
  mode(kMBlend);
  rect(70, 104, 570, 404, 0xEC0C0405, 0xEC040203);
  mode(kMAdd);
  for (auto [x0, y0, x1, y1] : {std::tuple{70.0f, 104.0f, 570.0f, 106.0f}, std::tuple{70.0f, 402.0f, 570.0f, 404.0f}})
    rect(x0, y0, x1, y1, 0xFFB02020, 0xFFB02020);
  a_ << cmpwi(0, 19, kSubSystem);
  a_.bne("panel_audio");
  mode(kMText);
  text("s_sysinfo", 320, 122, 18, 0xFFFF5A3A, true);
  {
    for (int i = 0; i < 7; ++i) {
      const float y = 168.0f + 30.0f * float(i);
      text("s_sys_k" + std::to_string(i), 96, y, 13, 0xFFFF9A40, false);
      text("s_sys_v" + std::to_string(i), 232, y, 13, 0xFFE6DCD2, false);
    }
  }
  a_.b("dash_panel_done");
  a_.label("panel_audio");
  mode(kMText);
  text("s_soundtest", 320, 122, 18, 0xFFFF5A3A, true);
  text("s_voices", 320, 160, 12, 0xFFB4A8A0, true);
  // One bar per Tyke voice: height follows its volume while it plays.
  mode(kM2D);
  a_ << li(17, 0);
  lf(20, 110.0f);
  a_.label("bar_loop");
  a_ << slwi(3, 17, 5) << add(3, 3, rTyke) << lwz(5, 0x100 + 20, 3) << lwz(6, 0x100 + 16, 3);
  a_ << andi_(0, 5, 1);
  a_.beq("bar_off");
  a_ << srwi(7, 6, 16) << clrlwi(6, 6, 16) << add(6, 6, 7);
  i2f(5, 6);
  lf(6, 0.30f), lf(7, 10.0f);
  a_ << fmadds(5, 5, 6, 7);
  load(3, 0xFFFFC848), load(4, 0xFFE02020);
  a_.b("bar_draw");
  a_.label("bar_off");
  lf(5, 6.0f);
  load(3, 0xFF4A1010), load(4, 0xFF4A1010);
  a_.label("bar_draw");
  lf(4, 340.0f);
  a_ << fsubs(2, 4, 5) << fmr(1, 20);
  lf(6, 12.0f);
  a_ << fadds(3, 20, 6);
  a_.bl("quad");
  lf(6, 18.0f);
  a_ << fadds(20, 20, 6) << addi(17, 17, 1) << cmpwi(0, 17, Tyke::kChannels);
  a_.blt("bar_loop");
  mode(kMText);
  text("s_voice_axis", 320, 352, 10, 0xFF806860, true);
  a_.label("dash_panel_done");
  a_.label("dash_no_panel");

  // Button hints.
  mode(kMText);
  a_ << cmpwi(0, 19, kSubNone);
  a_.bne("hint_sub");
  text("s_hint_main", 320, 436, 12, 0xFF988880, true);
  a_.b("hint_done");
  a_.label("hint_sub");
  a_ << cmpwi(0, 19, kSubAudio);
  a_.bne("hint_back");
  text("s_hint_audio", 320, 436, 12, 0xFF988880, true);
  a_.b("hint_done");
  a_.label("hint_back");
  text("s_hint_back", 320, 436, 12, 0xFF988880, true);
  a_.label("hint_done");

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

// ------------------------------------------------------------------ loading

void RomBuilder::loading() {
  a_.label("loading");
  a_ << li(0, kSceneLoading) << stw(0, kScene, rRam);
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

  // Hand over a quiet machine: interrupts masked, every voice off, unity master volume.
  a.label("enter");
  a.li32(3, map::kPic);
  a << li(0, 0) << stw(0, 4, 3) << li(0, -1) << stw(0, 0, 3);
  a.li32(3, map::kTyke + 0x100 + 20);
  a << li(0, 0) << li(4, Tyke::kChannels) << mtctr(4);
  a.label("enter_quiet");
  a << stw(0, 0, 3) << addi(3, 3, 32);
  a.bdnz("enter_quiet");
  a.li32(3, map::kTyke);
  a << li(0, 0x100) << stw(0, 0, 3);
  a.li32(1, kStack);
  a << mtspr(26, 25);  // SRR0 = entry
  a.li32(3, Tuffy::kMsrMe | Tuffy::kMsrFp | Tuffy::kMsrVec);
  a << mtspr(27, 3) << li(3, 0) << ppcasm::sync() << isync() << rfi();

  // Not bootable: back to the dashboard, which explains it.
  a.label("boot_fail");
  globals();
  video_init();
  a << li(0, kUnplayable) << stw(0, kDiscState, rRam);
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

  str("s_atari", "ATARI");
  str("s_lion", "LION");
  str("s_tagline", "INTERACTIVE ENTERTAINMENT SYSTEM");
  str("s_disc", "DISC");
  str("s_audio", "AUDIO");
  str("s_system", "SYSTEM");
  str("s_insert", "INSERT A LION DISC");
  str("s_reading", "READING DISC...");
  str("s_play", "PLAY DISC");
  str("s_unplayable", "THIS DISC CANNOT BE PLAYED");
  str("s_audio_desc", "SOUND TEST");
  str("s_audio_desc2", "TYKE DSP  -  24 VOICES");
  str("s_system_desc", "SYSTEM INFORMATION");
  str("s_system_desc2", "TUFFY  QUACKER  SPIKE  TYKE");
  str("s_sysinfo", "SYSTEM INFORMATION");
  const char* lines[][2] = {{"CPU", "TUFFY  POWERPC 7400  500 MHZ"}, {"GEOMETRY", "QUACKER  MC88110  100 MHZ"},
                            {"GRAPHICS", "SPIKE  2 PIPELINES  90 MHZ"},  {"AUDIO", "TYKE DSP  24 VOICES  40 MHZ"},
                            {"MEMORY", "24 MB SDRAM + 8 MB SGRAM"},     {"STORAGE", "12X CD-ROM"},
                            {"BOOT ROM", "VERSION 1.0  REV A2"}};
  for (int i = 0; i < 7; ++i) {
    str("s_sys_k" + std::to_string(i), lines[i][0]);
    str("s_sys_v" + std::to_string(i), lines[i][1]);
  }
  str("s_soundtest", "SOUND TEST");
  str("s_voices", "TYKE MIXER  -  44.1 KHZ STEREO");
  str("s_voice_axis", "VOICE 1                                    VOICE 24");
  str("s_hint_main", "<  >  CHOOSE      (A) SELECT");
  str("s_hint_audio", "(A) PLAY JINGLE      (B) BACK");
  str("s_hint_back", "(B) BACK");
  str("s_loading", "LOADING");

  auto blob = [&](const std::string& label, const std::vector<u32>& w) {
    a_.align(16);
    a_.label(label);
    for (u32 x : w) a_.word(x);
  };
  blob("mesh_emblem", rom::mesh_emblem());
  blob("mesh_disc", rom::mesh_disc());
  blob("mesh_audio", rom::mesh_audio());
  blob("mesh_chip", rom::mesh_chip());
  blob("mesh_grid", rom::mesh_grid());
  {
    std::vector<u32> t;
    for (float f : rom::intro_table()) t.push_back(f32_to_bits(f));
    blob("intro_tab", t);
  }
  blob("jingle_events", rom::jingle_events(bank_));
  blob("sfx_tick", rom::sfx_record(bank_.tick, 1.0, 0x70, 0x70));
  blob("sfx_ok", rom::sfx_record(bank_.ok, 1.0, 0x90, 0x90));
  blob("sfx_back", rom::sfx_record(bank_.back, 1.0, 0x90, 0x90));
  blob("tyke_fw", tyke_mixer_firmware());
  a_.label("boot_name");
  str("boot_name_str", "BOOT.LION;1");

  if (a_.here() > map::kRomBase + rom::kFontOffset) throw std::runtime_error("boot ROM: code and data overflow the font");
  while (a_.here() < map::kRomBase + rom::kFontOffset) a_.word(0);
  a_.label("font");
  const std::vector<u8> font = rom::font_texture();
  for (size_t i = 0; i < font.size(); i += 4)
    a_.word(u32(font[i]) << 24 | u32(font[i + 1]) << 16 | u32(font[i + 2]) << 8 | font[i + 3]);
  while (a_.here() < map::kRomBase + rom::kSampleOffset) a_.word(0);
  if (rom::kSampleOffset + bank_.pcm.size() > map::kRomSize) throw std::runtime_error("boot ROM: samples overflow");
  for (size_t i = 0; i < bank_.pcm.size(); i += 4)
    a_.word(u32(bank_.pcm[i]) << 24 | u32(bank_.pcm[i + 1]) << 16 | u32(bank_.pcm[i + 2]) << 8 | bank_.pcm[i + 3]);
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
  }
  vectors();
  reset();
  routines();
  intro();
  dashboard();
  loading();
  loader();
  data();
  std::vector<u8> rom = a_.finish();
  rom.resize(map::kRomSize, 0);
  return rom;
}

}  // namespace

std::vector<u8> build_boot_rom() { return RomBuilder().build(); }

}  // namespace lion
