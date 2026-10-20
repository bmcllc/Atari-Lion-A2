/* Minimal <math.h> for the freestanding Lion runtime.
 * The only function the shared GPU rasterizer needs is floorf; the 7400 has
 * frim (round toward -infinity), so it is one instruction. */
#pragma once

static inline float floorf(float x) {
    float r;
    __asm__("frim %0,%1" : "=f"(r) : "f"(x));
    return r;
}
