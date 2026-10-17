#include "tyke/firmware.h"

#include "tykeasm.h"

namespace lion {

// Data RAM layout (word addresses):
//   0x000            MASTER_VOL
//   0x040 + 8 * ch   START, LENGTH, LOOP_START, PITCH, VOLUME, CTRL, POSITION, -
//   0x400 + 4 * ch   private: sample index, 16-bit fraction
// plus the effects and the scope (see firmware.h).
// CTRL bit 8 is the firmware's acknowledgement of a key-on: any host write of
// CTRL clears it, so writing KEY_ON always (re)starts the sample.
std::vector<u32> tyke_mixer_firmware() {
  using namespace tykeasm;
  Asm a;
  // Registers: r1 channel block, r2 private block, r3 channels left, r5 = 0xFFFF,
  // r4 and r6-r14 scratch.
  a << movi(4, 0x100) << st(4, 0, 0);  // master volume defaults to unity, effects off
  for (u32 w : {kMixBass, kMixTreble, kMixEcho, kScopePos, kMixState, kMixState + 1, kMixState + 2, kMixState + 3,
                kMixState + 4})
    a << st(0, int32_t(w), 0);
  a << movi(5, 0xFFFF);

  a.label("frame");
  a << wait() << clr(0) << clr(1);
  a << movi(1, 0x40) << movi(2, 0x400) << movi(3, 24);

  a.label("channel");
  a << ld(4, 5, 1) << andi(6, 4, 1);  // CTRL
  a.beq(6, 0, "next");
  a << andi(6, 4, 0x100);
  a.bne(6, 0, "playing");
  a << st(0, 0, 2) << st(0, 1, 2);  // key on: restart the sample and acknowledge
  a << movi(6, 0x100) << or_(4, 4, 6) << st(4, 5, 1);
  a.label("playing");
  a << ld(7, 0, 2) << ld(8, 1, 1);  // index, length
  a.bltu(7, 8, "in_range");
  a << andi(6, 4, 2);
  a.beq(6, 0, "stop");
  a << ld(9, 2, 1);  // loop start
  a.bgeu(9, 8, "stop");
  a << sub(7, 7, 8) << add(7, 7, 9);  // index = loop + (index - length)
  a.bltu(7, 8, "in_range");
  a << add(7, 9, 0);
  a.label("in_range");
  a << ld(9, 0, 1) << ldxh(10, 9, 7);  // sample = start[index]
  a << ld(11, 4, 1) << macs(10, 11);   // left += s * volL, right += s * volR
  a << st(7, 6, 1);                    // POSITION
  // Advance by PITCH (16.16): fraction in the private block.
  a << ld(12, 1, 2) << ld(13, 3, 1);
  a << and_(14, 13, 5) << add(12, 12, 14) << shri(14, 12, 16) << add(7, 7, 14);
  a << shri(13, 13, 16) << add(7, 7, 13) << and_(12, 12, 5);
  a << st(12, 1, 2) << st(7, 0, 2);
  a.label("next");
  a << addi(1, 1, 8) << addi(2, 2, 4) << addi(3, 3, -1);
  a.bne(3, 0, "channel");

  // Volumes are 0..0x100 (8.8): drop 8 bits and apply the master volume.
  a << ld(4, 0, 0);
  a << mva(6, 0, 8) << mul(6, 6, 4) << sari(6, 6, 8);  // left
  a << mva(7, 1, 8) << mul(7, 7, 4) << sari(7, 7, 8);  // right

  // Tone: bass adds a ~220 Hz low-pass of the signal, treble what is above ~2 kHz.
  a << ld(8, kMixBass, 0) << ld(9, kMixTreble, 0) << or_(10, 8, 9);
  a.beq(10, 0, "no_eq");
  for (u32 side = 0; side < 2; ++side) {
    const u32 x = side ? 7 : 6;
    const int32_t low = int32_t(kMixState + side), mid = int32_t(kMixState + 2 + side);
    a << ld(11, low, 0) << sub(12, x, 11) << sari(12, 12, 5) << add(11, 11, 12) << st(11, low, 0);
    a << ld(13, mid, 0) << sub(12, x, 13) << sari(12, 12, 2) << add(13, 13, 12) << st(13, mid, 0);
    a << sub(14, x, 13);                                         // highs (before the bass is added)
    a << mul(12, 11, 8) << sari(12, 12, 8) << add(x, x, 12);     // + bass * low
    a << mul(12, 14, 9) << sari(12, 12, 8) << add(x, x, 12);     // + treble * high
  }
  a.label("no_eq");

  // Echo: an 81 ms mono delay line fed back at half level.
  a << ld(8, kMixEcho, 0);
  a.beq(8, 0, "no_echo");
  a << ld(9, kMixState + 4, 0) << movi(10, int32_t(kEchoBase)) << add(10, 10, 9) << ld(11, 0, 10);
  a << mul(12, 11, 8) << sari(12, 12, 8) << add(6, 6, 12) << add(7, 7, 12);
  a << add(12, 6, 7) << sari(12, 12, 2) << sari(13, 11, 1) << add(12, 12, 13) << st(12, 0, 10);
  a << addi(9, 9, 1) << movi(10, int32_t(kEchoWords));
  a.bltu(9, 10, "echo_keep");
  a << movi(9, 0);
  a.label("echo_keep");
  a << st(9, kMixState + 4, 0);
  a.label("no_echo");

  a << sat16(6, 6) << sat16(7, 7) << out(0, 6) << out(1, 7);
  // Scope: the last 256 output frames for visualizers.
  a << ld(9, kScopePos, 0) << shli(10, 9, 1) << st(6, int32_t(kScopeBase), 10) << st(7, int32_t(kScopeBase + 1), 10);
  a << addi(9, 9, 1) << andi(9, 9, kScopeFrames - 1) << st(9, kScopePos, 0);
  a.br("frame");

  a.label("stop");
  a << movi(14, -2) << and_(4, 4, 14) << st(4, 5, 1);
  a.br("next");
  return a.finish();
}

}  // namespace lion
