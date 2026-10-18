// A tracker module player (MOD, S3M, XM, IT): the music of Unreal's .umx files.
//
// Independent of the engine: it takes the file image, keeps it (samples are converted in place), and renders
// stereo PCM16 at the requested rate. Pitch, envelopes, effects and IT's new note actions follow the formats'
// reference players closely enough for game music; resonant filters, panbrello and the MIDI macros are not played.
#pragma once

namespace lionmod {

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef short s16;
typedef unsigned int u32;
typedef int s32;

struct Impl;

class Player {
 public:
  Player();
  ~Player();
  // `data` (malloc'ed, `size` bytes) is owned by the player from here on, also when loading fails.
  bool Load(u8* data, u32 size, u32 rate);
  // Writes `count` frames. The song loops forever (from the restart order).
  void Render(s16* left, s16* right, u32 count);
  // Starts again from the given order (0 = the beginning).
  void Start(int order);
  const char* Format() const;
  int Channels() const;
  int Orders() const;
  unsigned Bytes() const;  // memory held

 private:
  Impl* p;
};

}  // namespace lionmod
