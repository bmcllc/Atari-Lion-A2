// Lion A2 boot ROM, assembled at startup into the 1 MB ROM at 0xFFF0_0000.
//
// Reset (0xFFF0_0100, MSR[IP] = 1):
//   1. upload the Tyke mixer firmware and start the DSP;
//   2. wait for a disc (blue screen while the drive is empty);
//   3. read the ISO9660 volume descriptor and root directory;
//   4. find BOOT.LION, read it, check the header and copy its segments;
//   5. enter the game at its entry point with MSR = ME | FP | VEC.
// A bad disc shows a red screen.
#include "bootrom.h"

#include "bus.h"
#include "cdrom.h"
#include "ppcasm.h"
#include "tuffy/ppc.h"
#include "tyke/firmware.h"
#include "tyke/tyke.h"

namespace lion {

namespace {
constexpr u32 kSectorBuf = 0x00C0'0000;  // volume descriptor and directory
constexpr u32 kFileBuf = 0x00D0'0000;    // BOOT.LION image
constexpr u32 kMaxFileSectors = 3000;    // ~6 MB
constexpr u32 kStack = map::kSdramBase + map::kSdramSize - 0x100;
constexpr u32 kBlue = 0x001F, kRed = 0xF800;
}  // namespace

std::vector<u8> build_boot_rom() {
  using namespace ppcasm;
  Asm a(map::kRomBase);

  // 0x000-0x0FF: ROM header.
  a.word(0x4C494F4E);  // "LION"
  a.word(0x0000'0A02);  // board revision A2
  while (a.here() < map::kRomBase + 0x100) a.word(0);

  // 0x100: reset vector.
  a.b("start");
  // Every other vector: unexpected exception during boot -> red screen.
  for (u32 v = 0x200; v < 0x1000; v += 0x100) {
    while (a.here() < map::kRomBase + v) a.word(0);
    a.b("fail");
  }
  while (a.here() < map::kRomBase + 0x1000) a.word(0);

  a.label("start");
  a.li32(1, kStack);
  a.li32(31, map::kCd);
  a.li32(29, map::kVi);
  a.li32(28, map::kPic);
  a << li(3, 0) << stw(3, 4, 28);  // all interrupts masked

  // 1. Tyke firmware upload.
  const std::vector<u32> fw = tyke_mixer_firmware();
  a.la(3, "tyke_fw");
  a.li32(4, map::kTyke + Tyke::kProgramBase);
  a.li32(5, u32(fw.size()));
  a << mtctr(5);
  a.label("fw_copy");
  a << lwz(6, 0, 3) << stw(6, 0, 4) << addi(3, 3, 4) << addi(4, 4, 4);
  a.bdnz("fw_copy");
  a.li32(4, map::kTyke + Tyke::kCtrlBase);
  a << li(6, 1) << stw(6, 0, 4);

  // 2. Wait for a disc.
  a.label("wait_disc");
  a << lwz(3, 0, 31) << andi_(3, 3, Cdrom::kPresent);
  a.bne("have_disc");
  a << li(3, kBlue);
  a.bl("fill_screen");
  a.b("wait_disc");
  a.label("have_disc");
  a << li(3, 0) << stw(3, 0, 29);  // video off while loading

  // 3. Primary volume descriptor (sector 16).
  a << li(3, 16) << li(4, 1);
  a.li32(5, kSectorBuf);
  a.bl("cd_read");
  a << cmpwi(0, 3, 0);
  a.bne("fail");
  a.li32(20, kSectorBuf);
  a << lbz(3, 0, 20) << cmpwi(0, 3, 1);  // type 1 = primary
  a.bne("fail");
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
  a.bne("fail");

  // 4. Scan the root directory for "BOOT.LION;1".
  a.li32(6, kSectorBuf);
  a << add(7, 6, 22);  // end of directory data
  a.label("scan");
  a << cmplw(0, 6, 7);
  a.bge("fail");
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
  a.bgt("fail");
  a << mr(3, 23);
  a.li32(5, kFileBuf);
  a.bl("cd_read");
  a << cmpwi(0, 3, 0);
  a.bne("fail");

  // 5. Check the .lion header and copy the segments.
  a.li32(20, kFileBuf);
  a << lwz(3, 0, 20);
  a.li32(4, 0x4C49'4F4E);
  a << cmplw(0, 3, 4);
  a.bne("fail");
  a << lwz(3, 4, 20) << cmpwi(0, 3, 1);
  a.bne("fail");
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

  a.label("enter");
  a.li32(1, kStack);
  a << mtspr(26, 25);  // SRR0 = entry
  a.li32(3, Tuffy::kMsrMe | Tuffy::kMsrFp | Tuffy::kMsrVec);
  a << mtspr(27, 3) << li(3, 0) << sync() << isync() << rfi();

  a.label("fail");
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

  a.label("boot_name");
  const char name[12] = "BOOT.LION;1";  // 11 characters + NUL, three words
  for (int w = 0; w < 3; ++w)
    a.word(u32(u8(name[w * 4])) << 24 | u32(u8(name[w * 4 + 1])) << 16 | u32(u8(name[w * 4 + 2])) << 8 |
           u8(name[w * 4 + 3]));
  a.label("tyke_fw");
  for (u32 w : fw) a.word(w);
  return a.finish();
}

}  // namespace lion
