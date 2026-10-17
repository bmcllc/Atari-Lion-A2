#!/usr/bin/env python3
"""Portability patches for a copy of Unreal's Engine (Inc/ and Src/) for the Lion (clang, PowerPC).
Usage: patch_engine.py <work dir containing Inc/ and Src/>"""
import re, sys, pathlib

root = pathlib.Path(sys.argv[1])

def edit(path, fn):
    f = root / path
    text = f.read_text(encoding="latin-1")
    new = fn(text)
    if new == text:
        sys.exit(f"patch_engine: nothing changed in {path}")
    f.write_text(new, encoding="latin-1")

# The hand-written x86 transform routines are only referenced when ASM is on: compile them out.
def wrap_asm_transforms(t):
    a = t.index("inline void ASMTransformPoint")
    b = t.index("//\n// Transform a point by a coordinate system, moving")
    return t[:a] + "#if ASM\n" + t[a:b] + "#endif\n\n" + t[b:]
edit("Inc/UnMath.h", wrap_asm_transforms)

# Anonymous structs holding FVector (non-trivial constructor) are an MSVC extension clang rejects.
def fix_scene_node_union(t):
    pat = re.compile(r"\tunion\n\t\{\n\t\tstruct\n\t\t\{\n\t\t\tFVector\tProj;[^\n]*\n\t\t\tFVector RProj;[^\n]*\n\t\t\};\n\t\tstruct\n\t\t\{.*?\n\t\t\};\n\t\};\n", re.S)
    new, n = pat.subn("\tFVector Proj;      \t// Projection vector.\n\tFVector RProj;\t\t// Reverse projection vector.\n", t)
    if n != 1:
        sys.exit("patch_engine: FSceneNode union not found")
    return new
edit("Inc/UnRender.h", fix_scene_node_union)
edit("Src/UnMesh.cpp", lambda t: t.replace('#include "amd3d.h"', '#include "Amd3d.h"'))
# Declared and defined `inline` in different files: give it external linkage.
edit("Inc/UnReach.h", lambda t: t.replace("inline int supports (int iRadius", "int supports (int iRadius"))
edit("Src/UnReach.cpp", lambda t: t.replace("inline int FReachSpec::supports", "int FReachSpec::supports"))
print("patch_engine: ok")
