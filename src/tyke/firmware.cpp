#include "tyke/firmware.h"

#include "tykeasm.h"

namespace lion {

// Data RAM layout (word addresses):
//   0x000            MASTER_VOL
//   0x040 + 8 * ch   START, LENGTH, LOOP_START, PITCH, VOLUME, CTRL, POSITION, -
//   0x400 + 4 * ch   private: sample index, 16-bit fraction, previous CTRL
std::vector<u32> tyke_mixer_firmware() {
  using namespace tykeasm;
  Asm a;
  // Registers: r1 channel block, r2 private block, r3 channels left, r4-r14 scratch.
  a << movi(4, 0x100) << st(4, 0, 0);  // master volume defaults to unity

  a.label("frame");
  a << wait() << clr(0) << clr(1);
  a << movi(1, 0x40) << movi(2, 0x400) << movi(3, 24);

  a.label("channel");
  a << ld(4, 5, 1) << ld(5, 2, 2) << st(4, 2, 2);  // ctrl, previous ctrl
  a << andi(6, 4, 1);
  a.beq(6, 0, "next");
  a << andi(7, 5, 1);
  a.bne(7, 0, "playing");
  a << st(0, 0, 2) << st(0, 1, 2);  // key on: restart the sample
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
  a << shli(14, 13, 16) << shri(14, 14, 16) << add(12, 12, 14);
  a << shri(14, 12, 16) << add(7, 7, 14) << shri(13, 13, 16) << add(7, 7, 13);
  a << shli(12, 12, 16) << shri(12, 12, 16);
  a << st(12, 1, 2) << st(7, 0, 2);
  a.br("next");
  a.label("stop");
  a << movi(14, -2) << and_(4, 4, 14) << st(4, 5, 1) << st(4, 2, 2);
  a.label("next");
  a << addi(1, 1, 8) << addi(2, 2, 4) << addi(3, 3, -1);
  a.bne(3, 0, "channel");

  // Volumes are 0..0x100 (8.8): drop 8 bits, apply the master volume, saturate.
  a << ld(4, 0, 0);
  for (u32 side = 0; side < 2; ++side)
    a << mva(5, side, 8) << mul(5, 5, 4) << sari(5, 5, 8) << sat16(5, 5) << out(side, 5);
  a.br("frame");
  return a.finish();
}

}  // namespace lion
