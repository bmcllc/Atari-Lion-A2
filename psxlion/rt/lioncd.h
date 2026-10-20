/* Lion CD driver (0x1F060000, see docs/hardware.md): blocking sector reads by DMA, and a root-directory lookup. */
#pragma once
#include <stdint.h>

#define LCD(o) (*(volatile uint32_t *)(0x1F060000u + (o)))

static inline int lion_cd_read(uint32_t lba, uint32_t count, void *dst) {
    LCD(0x08) = lba; LCD(0x0C) = count; LCD(0x10) = (uint32_t)(uintptr_t)dst;
    LCD(0x04) = 1;                                          /* READ */
    uint32_t st;
    do st = LCD(0x00); while (!(st & 0xC));                 /* done (bit 3) or error (bit 2) */
    LCD(0x04) = 3;                                          /* ACK */
    return !(st & 4);
}

/* Looks `name` (n chars, upper case, no version suffix) up in the root directory. `buf` is a 2048-byte, 32-aligned scratch. */
static inline int lion_find_file(const char *name, int n, uint8_t *buf, uint32_t *lba, uint32_t *size) {
    if (!lion_cd_read(16, 1, buf) || buf[0] != 1) return 0;
    uint32_t dir = buf[158] | buf[159] << 8 | buf[160] << 16 | (uint32_t)buf[161] << 24;
    if (!lion_cd_read(dir, 1, buf)) return 0;
    for (uint32_t i = 0; i < 2048 && buf[i];) {
        int ok = buf[i + 32] >= n;
        for (int k = 0; ok && k < n; k++) ok = buf[i + 33 + k] == (uint8_t)name[k];
        if (ok) {
            *lba = buf[i + 2] | buf[i + 3] << 8 | buf[i + 4] << 16 | (uint32_t)buf[i + 5] << 24;
            *size = buf[i + 10] | buf[i + 11] << 8 | buf[i + 12] << 16 | (uint32_t)buf[i + 13] << 24;
            return 1;
        }
        i += buf[i];
    }
    return 0;
}
