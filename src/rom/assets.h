#pragma once
// Data baked into the boot ROM next to the dashboard code: the system font,
// the 3D meshes, the audio samples, the start-up jingle and the intro timeline.

#include <vector>

#include "common.h"

namespace lion::rom {

// Fixed ROM layout (offsets from map::kRomBase).
constexpr u32 kFontOffset = 0x6'0000;    // 256x128 RGB565 font texture (64 KB)
constexpr u32 kSampleOffset = 0x7'0000;  // 16-bit PCM samples, up to the end of the ROM

// Font: 16x8 cells of 16x16 texels, ASCII 0-127, white glyphs with a soft halo on black.
constexpr u32 kFontLog2W = 8, kFontLog2H = 7;
std::vector<u8> font_texture();

// Mesh blob, big-endian words:
//   +0 vertex count, +4 triangle count, +8 flags (bit 0: unlit), +12 0
//   +16 vertices: x, y, z, 1 (floats)
//   then triangles: three vertex byte offsets (index * 16), ARGB, face normal (3 floats)
//   (flags bit 0: unlit; bit 1: per-vertex colours, see kMeshVertexColors)
constexpr u32 kMeshUnlit = 1;
// Bit 1: per-vertex colours (Gouraud, unlit): ARGB is vertex 0, the normal slots hold vertices 1 and 2.
constexpr u32 kMeshVertexColors = 2;
std::vector<u32> mesh_emblem(bool small = false);  // the Lion mark: three bars inside a mane of rays
std::vector<u32> mesh_disc();
std::vector<u32> mesh_audio();
std::vector<u32> mesh_chip();
std::vector<u32> mesh_grid();    // floor grid, unlit, colours fade with distance
constexpr float kGridStep = 1.5f;

// World light for the flat shading (normalised) and the shading terms.
constexpr float kLight[3] = {-0.35f, 0.55f, 0.76f};
constexpr float kAmbient = 0.35f, kDiffuse = 0.75f;

// Audio. Each sample is described to the Tyke mixer by address and length.
struct SampleRef {
  u32 addr = 0, length = 0;
  double base_hz = 0;  // pitch at PITCH = 1.0
};
struct SoundBank {
  std::vector<u8> pcm;  // big-endian s16, placed at kSampleOffset
  SampleRef bell, pad, roar, tick, ok, back;
};
SoundBank sound_bank();

// Sound effect record read by the ROM: START, LENGTH, PITCH (16.16), VOLUME (L << 16 | R).
std::vector<u32> sfx_record(const SampleRef& s, double pitch, u32 left, u32 right);

// Sequencer events: (frame, Tyke byte offset, value), terminated by frame 0xFFFFFFFF.
std::vector<u32> jingle_events(const SoundBank& bank);

// Intro: one row of kIntroColumns floats per frame.
//   0 yaw, 1 pitch, 2 scale, 3 tx, 4 ty, 5 tz   (emblem transform, as the mesh routine reads it)
//   6 emblem brightness, 7 glow, 8 "ATARI", 9 "LION", 10 tagline, 11 flash, 12 fade to black
constexpr u32 kIntroFrames = 240, kIntroColumns = 16;
std::vector<float> intro_table();

}  // namespace lion::rom
