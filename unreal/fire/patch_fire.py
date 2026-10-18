#!/usr/bin/env python3
"""Fire package patch: the fractal texture code has C fallbacks for every x86 routine, but the header forces
ASM on (and has a stray `#undef`). Turn the assembler off so the C paths are used."""
import sys, pathlib

f = pathlib.Path(sys.argv[1]) / "Src/FractalPrivate.h"
t = f.read_text(encoding="latin-1")
old = "#undef ASM\n#define ASM 1\n#undef \n"
if old not in t:
    sys.exit("patch_fire: pattern not found")
t = t.replace(old, "#undef ASM\n#define ASM 0\n", 1)
# MSVC does not place derived-class members in the tail padding of a base class; clang does. Make the padding explicit.
old2 = "    BYTE    AuxPhase;     //\n"
if old2 not in t:
    sys.exit("patch_fire: AuxPhase not found")
t = t.replace(old2, old2 + "    BYTE    TailPad_[2];  // keeps FireTexture's script layout (MSVC base-class tail padding)\n", 1)
f.write_text(t, encoding="latin-1")
print("patch_fire: ok")
