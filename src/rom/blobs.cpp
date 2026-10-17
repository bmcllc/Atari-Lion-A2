// Audio baked into the boot ROM, converted from Assets/ by tools/convert_assets.sh
// and pulled into the binary as-is.
#include "rom/blobs.h"

#define LION_INCBIN(name, file)                                                                \
  asm(".section .rodata\n.balign 16\n.global " #name "\n" #name ":\n.incbin \"" LION_ROM_AUDIO_DIR \
      "/" file "\"\n.global " #name "_end\n" #name "_end:\n.previous\n");                       \
  extern "C" const u8 name[], name##_end[];

LION_INCBIN(lion_rom_roar, "roar.pcm")
LION_INCBIN(lion_rom_claws, "claws.pcm")
LION_INCBIN(lion_rom_theme1, "theme1.ladp")
LION_INCBIN(lion_rom_theme2, "theme2.ladp")

namespace lion::rom {

Blob roar_pcm() { return {lion_rom_roar, size_t(lion_rom_roar_end - lion_rom_roar)}; }
Blob claws_pcm() { return {lion_rom_claws, size_t(lion_rom_claws_end - lion_rom_claws)}; }
Blob theme_ladp(int n) {
  return n == 0 ? Blob{lion_rom_theme1, size_t(lion_rom_theme1_end - lion_rom_theme1)}
                : Blob{lion_rom_theme2, size_t(lion_rom_theme2_end - lion_rom_theme2)};
}

}  // namespace lion::rom
