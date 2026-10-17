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
constexpr u32 kHasGame = 0x100, kTrackCount = 0x104, kTheme = 0x10C, kBootFailed = 0x178;
constexpr u32 kMusicState = 0x120, kMusicTrack = 0x124, kMusicError = 0x160, kMusicElapsed = 0x150;
constexpr u32 kTracks = 0x200, kTrackSize = 24, kMaxTracks = 16;  // {LBA, size, name[16]}
enum Scene : u32 { kSceneReset = 0, kSceneIntro = 1, kSceneDashboard = 2, kSceneLoading = 3 };
enum DiscState : u32 { kNoDisc = 0, kReading = 1, kReady = 2, kUnplayable = 3, kReadingDir = 4 };
enum Item : u32 { kItemPlay = 0, kItemMusic = 1, kItemTheme = 2, kItemTyke = 3, kItemSystem = 4, kItemCount = 5 };
enum Submenu : u32 { kSubNone = 0, kSubSystem = 1, kSubTyke = 2, kSubMusic = 3, kSubTheme = 4 };
enum MusicState : u32 { kMusicStopped = 0, kMusicPlaying = 1, kMusicPaused = 2, kMusicStarting = 3 };
}  // namespace bootrom

}  // namespace lion
