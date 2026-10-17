#pragma once

#include <vector>

#include "common.h"

namespace lion {

// Assembles the boot ROM image (PowerPC code at 0xFFF0_0000, reset at +0x100).
std::vector<u8> build_boot_rom();

// Dashboard state the ROM keeps in SDRAM (for tests and debuggers).
namespace bootrom {
constexpr u32 kRam = 0x00A0'0000;
constexpr u32 kDiscState = 0x7C, kScene = 0x80, kSelection = 0x84, kSubmenu = 0x88, kLaunch = 0x8C, kLabel = 0xA0;
enum Scene : u32 { kSceneReset = 0, kSceneIntro = 1, kSceneDashboard = 2, kSceneLoading = 3 };
enum DiscState : u32 { kNoDisc = 0, kReading = 1, kReady = 2, kUnplayable = 3 };
enum Item : u32 { kItemDisc = 0, kItemAudio = 1, kItemSystem = 2 };
enum Submenu : u32 { kSubNone = 0, kSubSystem = 1, kSubAudio = 2 };
}  // namespace bootrom

}  // namespace lion
