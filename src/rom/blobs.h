#pragma once
// Converted audio from Assets/rom/ (see tools/convert_assets.sh).

#include <cstddef>

#include "common.h"

namespace lion::rom {

struct Blob {
  const u8* data;
  size_t size;
};
Blob roar_pcm();         // the lion's roar: big-endian PCM16, 44.1 kHz mono
Blob claws_pcm();        // the claws tearing the plate: same format
Blob theme_ladp(int n);  // dashboard themes I and II: LADP (tools/ima_adpcm.h)
constexpr int kStreamThemes = 2;

}  // namespace lion::rom
