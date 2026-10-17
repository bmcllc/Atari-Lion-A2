#pragma once

#include <vector>

#include "common.h"

namespace lion {

// Default Tyke program, uploaded by the boot ROM: a 24-channel PCM16 mixer
// driven by the channel registers in data RAM (docs/hardware.md, "Tyke").
std::vector<u32> tyke_mixer_firmware();

}  // namespace lion
