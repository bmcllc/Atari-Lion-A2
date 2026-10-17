#pragma once
// Data baked into the boot ROM next to the dashboard code: the system font,
// textures, 3D meshes, audio samples, the music and the intro timelines.

#include <vector>

#include "common.h"

namespace lion::rom {

// Fixed layout of the ROM's last megabyte (offsets from map::kRomVectors).
constexpr u32 kFontOffset = 0x6'0000;    // 256x128 RGB565 font texture (64 KB)
constexpr u32 kJaguarOffset = 0x7'0000;  // 256x64 RGB565 "JAGUAR" plate (32 KB)
constexpr u32 kSampleOffset = 0x7'8000;  // 16-bit PCM samples, up to the end of the ROM

// Font: 16x8 cells of 16x16 texels, ASCII 0-127, white glyphs with a soft halo on black.
constexpr u32 kFontLog2W = 8, kFontLog2H = 7;
std::vector<u8> font_texture();

// The plate the lion tears apart in the intro: an italic "JAGUAR" wordmark on brushed metal.
constexpr u32 kJaguarLog2W = 8, kJaguarLog2H = 6;
std::vector<u8> jaguar_texture();

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
std::vector<u32> mesh_crown();   // sits on the emblem (same transform)
std::vector<u32> mesh_note();
std::vector<u32> mesh_mixer();
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
  SampleRef claw, pluck, kick, hat;
  SampleRef lion_roar, lion_claws;  // from Assets/ (rom/blobs.h), in the ROM's lower 7 MB
};

// Where the converted audio sits in the lower part of the ROM (absolute addresses).
struct LowLayout {
  u32 theme[2];  // LADP streams
  u32 roar, claws;
  u32 end;
};
LowLayout low_layout();
SoundBank sound_bank();

// Sound effect record read by the ROM: START, LENGTH, PITCH (16.16), VOLUME (L << 16 | R).
std::vector<u32> sfx_record(const SampleRef& s, double pitch, u32 left, u32 right);

// Sequencer events: (frame, Tyke byte offset, value). Offset kSeqLoop at a frame
// restarts the sequence from its beginning; frame kSeqEnd ends it.
constexpr u32 kSeqLoop = 0xFFFF'FFFE, kSeqEnd = 0xFFFF'FFFF;
// Voices: music and the jingle use 0-15, the CD player 16-17, sound effects 21-23.
std::vector<u32> jingle_events(const SoundBank& bank);
constexpr u32 kThemeCount = 6;
// Themes 0 and 1 stream the dashboard music from Assets/ (LADP); the others are sequenced.
int theme_stream(u32 theme);  // stream index, or -1
extern const char* const kThemeNames[kThemeCount];
extern const char* const kThemeBlurbs[kThemeCount];
std::vector<u32> theme_events(const SoundBank& bank, u32 theme);

// Intro, one row of kIntroColumns floats per frame:
//   0 yaw, 1 pitch, 2 scale, 3 tx, 4 ty, 5 tz   (emblem transform, as the mesh routine reads it)
//   6 emblem brightness, 7 glow, 8 "ATARI", 9 "LION", 10 tagline, 11 flash, 12 fade to black,
//   13 crown drop (added to ty), 14 "A2", 15 red glow behind the plate
constexpr u32 kIntroFrames = 360, kIntroColumns = 16;
std::vector<float> intro_table();

// The plate, frames [kPlateFirst, kPlateLast): four strips per frame, cut by the claws
// at kTearFrame. Each strip: ARGB (alpha 0 = hidden), then 4 corners {x, y, u, v}.
constexpr u32 kPlateFirst = 10, kPlateLast = 180, kTearFrame = 128, kPlateStrips = 4, kStripWords = 17;
std::vector<u32> plate_table();
// The claw slashes, frames [kSlashFirst, kSlashLast): three per frame. Each: visible flag,
// then 4 corners {x, y, ARGB} (head and tail of the streak).
constexpr u32 kSlashFirst = 110, kSlashLast = 160, kSlashes = 3, kSlashWords = 13;
constexpr u32 kSlashFrame[kSlashes] = {112, 118, 124};
std::vector<u32> slash_table();

}  // namespace lion::rom
