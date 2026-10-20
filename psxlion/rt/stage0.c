/* psxlion/rt/stage0.c — BOOT.LION of the PS1-on-Lion disc: the boot ROM only loads ~6 MB, so this small first stage
 * reads PSX.IMG (game + BIOS + runtime, see mkimg.py) from the disc straight to its final addresses and jumps to it.
 *
 *   PSX.IMG: sector 0 = header { "PSXI", entry, nseg, { addr, size, file offset (multiple of 2048) } x nseg }, big-endian.
 */
#include "lioncd.h"

static uint8_t s_buf[2048] __attribute__((aligned(32)));
static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

void stage0_main(void) {
    uint32_t lba, size;
    if (!lion_find_file("PSX.IMG", 7, s_buf, &lba, &size) || !lion_cd_read(lba, 1, s_buf) || be32(s_buf) != 0x50535849u)
        for (;;) { }
    uint32_t entry = be32(s_buf + 4), nseg = be32(s_buf + 8);
    for (uint32_t i = 0; i < nseg; i++) {
        const uint8_t *e = s_buf + 12 + i * 12;
        uint32_t addr = be32(e), bytes = be32(e + 4), off = be32(e + 8);
        if (!lion_cd_read(lba + off / 2048, (bytes + 2047) / 2048, (void *)(uintptr_t)addr)) for (;;) { }
    }
    ((void (*)(void))(uintptr_t)entry)();
    for (;;) { }
}
