#pragma once

#include <vector>

#include "common.h"

namespace lion {

// Default Tyke program, uploaded by the boot ROM: a 24-channel PCM16 mixer
// driven by the channel registers in data RAM (docs/hardware.md, "Tyke").
std::vector<u32> tyke_mixer_firmware();

// Mixer data RAM, in words (the host sees word n at Tyke + 4n).
constexpr u32 kMixMaster = 0x000;  // master volume 0..0x100
constexpr u32 kMixBass = 0x001;    // 0..0x100: up to +6 dB below ~220 Hz
constexpr u32 kMixTreble = 0x002;  // 0..0x100: up to +6 dB above ~2 kHz
constexpr u32 kMixEcho = 0x003;    // 0..0x100: echo level
constexpr u32 kScopePos = 0x008;   // next scope frame the firmware writes
constexpr u32 kMixState = 0x010;   // private: filter states and echo position
constexpr u32 kMixChannels = 0x040;  // 8 words per channel
constexpr u32 kScopeBase = 0x1000, kScopeFrames = 256;  // L, R pairs of saturated output
constexpr u32 kEchoBase = 0x1200, kEchoWords = 3584;    // private delay line

}  // namespace lion
