#pragma once
// Boot ROM generator: register conventions, memory layout and the RomBuilder
// class shared by the ROM sources (bootrom.cpp and src/rom/*.cpp).

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
#include "bootrom.h"
#include "ima_adpcm.h"
#include "rom/blobs.h"

namespace lion::romgen {

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
constexpr u32 kJaguarTex = map::kSgramBase + 0x50'0000;
constexpr u32 kProj = kRam + 0x1'0000;  // QVE output
constexpr u32 kDirBuf = kSectorBuf + 0x800, kDirMax = 4 * 2048;  // root directory read by the dashboard
// CD player: a 256 KB ring of PCM16 (two halves refilled in turn) fed from a staging area.
constexpr u32 kRing = 0x00B0'0000, kRingHalf = 0x2'0000, kStage = 0x00B4'0000, kChunkSectors = 65;
constexpr u32 kMusicVoice = 16;

// Dashboard RAM (offsets from kRam, see bootrom.h for the public ones).
constexpr u32 kScratch = 0x00, kXform = 0x10, kFade = 0x40, kScroll = 0x44, kEmblemYaw = 0x48, kSeqPtr = 0x70,
              kSeqT = 0x74, kSfxCh = 0x78, kPadPrev = 0x90;
constexpr u32 kCdOwner = 0x108, kThemeCursor = 0x110, kTykeRow = 0x114, kHold = 0x118, kSeqLoop = 0x11C,
              kMusicCursor = 0x128, kMFcur = 0x12C, kMDlen = 0x130, kMDoff = 0x134, kMLba = 0x138, kMFsize = 0x13C,
              kMPitch = 0x140, kMChans = 0x144, kMHalf = 0x148, kMPhase = 0x14C, kMTotal = 0x154, kMReading = 0x158,
              kMEof = 0x15C, kHeld = 0x164, kSeqJingle = 0x168, kDirSize = 0x16C, kMSkip = 0x170, kMRate = 0x174;
constexpr u32 kIconYaw = 0x180, kCardScale = 0x1A0, kCardBright = 0x1C0;  // five floats each
// Theme stream: LADP blob, block count, next block, ring write position (frames), started, rate.
constexpr u32 kTsActive = 0x1D8, kTsBase = 0x1DC, kTsBlocks = 0x1E0, kTsBlock = 0x1E4, kTsW = 0x1E8,
              kTsStarted = 0x1EC, kTsRate = 0x1F0;
constexpr u32 kVizBars = 0x380;                                            // 16 floats
constexpr u32 kTimeStr = 0x400, kNumStr = 0x420, kNowStr = 0x440, kPctStr = 0x4A0;
constexpr u32 kVizF = 0x800, kScopeY = 0xC00;  // 256 and 128 floats
constexpr u32 kRamClear = 0x1000;
// CD drive users (kCdOwner).
enum CdOwner : u32 { kOwnerNone = 0, kOwnerDash = 1, kOwnerMusic = 2 };
// Music phases (kMPhase): header read, first half, second half, streaming.
enum MusicPhase : u32 { kPhaseHeader = 0, kPhaseHalf0 = 1, kPhaseHalf1 = 2, kPhaseRun = 3 };
constexpr int kVizBands = 16;

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
constexpr u32 kMGrid = Spike::kBlendAdd | Spike::kGouraud | Spike::kPerspective;

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
  void set_texture(u32 addr, u32 log2w, u32 log2h) {
    pushk(Spike::kSetTexture << 24 | 4);
    pushk(addr), pushk(log2w), pushk(log2h), pushk(1);
  }
  // Tyke byte offset of register `r` of voice `ch` (START, LENGTH, LOOP, PITCH, VOLUME, CTRL, POSITION).
  static int16_t voice_reg(u32 ch, u32 r) { return int16_t(0x100 + 32 * ch + 4 * r); }
  // Text from a string in a register (r3 = string).
  void text_r(float x, float y, float size, u32 argb, bool centered) {
    load(4, argb);
    lf(1, x), lf(2, y), lf(3, size);
    a_.bl(centered ? "textc" : "text");
  }
  // RAM word load / store / small constant store.
  void ram_ld(u32 r, u32 off) { a_ << lwz(r, int16_t(off), rRam); }
  void ram_st(u32 r, u32 off) { a_ << stw(r, int16_t(off), rRam); }
  void ram_set(u32 off, u32 v) {
    load(0, v);
    ram_st(0, off);
  }

  void globals();
  void video_init();
  void vectors();
  void reset();
  void routines();
  void intro();
  void dashboard();
  void dash_input();
  void dash_render();
  void panel_system();
  void panel_theme();
  void panel_tyke();
  void panel_music();
  void audio_routines();
  void loading();
  void loader();
  void data();

  Asm a_{map::kRomVectors};
  Pool pool_;
  int uid_ = 0;
  u32 magic_ = 0, sin_ = 0, circle_ = 0, uv_ = 0, hann_ = 0, goertzel_ = 0;
  u32 code_end_ = 0;
  rom::SoundBank bank_ = rom::sound_bank();
};

}  // namespace lion::romgen
