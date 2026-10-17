#pragma once

#include <vector>

#include "common.h"

namespace lion {

// Assembles the boot ROM image (PowerPC code at 0xFFF0_0000, reset at +0x100).
std::vector<u8> build_boot_rom();

}  // namespace lion
