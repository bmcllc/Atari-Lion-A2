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
# Sound data is 11 MB of the game package: it is not loaded into the 24 MB of RAM. The audio device (LionDrv)
# streams a sound from the disc when it first plays: the file offset of the wave goes in Handle, its size in
# OriginalSize, until RegisterSound takes them over.
edit("Src/UnAudio.cpp", lambda t: t.replace("\t\tAr << Data;\n\t\tif( Ar.IsLoading() )\n\t\t{\n\t\t\t// Derive these from the exposed",
    "\t\tif( Ar.IsLoading() )\n\t\t{\n\t\t\tINT SkipCount = 0;\n\t\t\tAr << AR_INDEX(SkipCount);\n\t\t\tHandle = (void*)(DWORD)Ar.Tell();\n\t\t\tAr.Seek( Ar.Tell() + SkipCount );\n\t\t\tOriginalSize = SkipCount;\n\t\t\treturn;\n\t\t}\n\t\tAr << Data;\n\t\tif( Ar.IsLoading() )\n\t\t{\n\t\t\t// Derive these from the exposed", 1))
# The temporary camera that the editor's lighting code opens is spawned with a role other than authority.
edit("Src/UnLevAct.cpp", lambda t: t.replace("\tcheck(Actor->Role==ROLE_Authority);\n", "\t// (lion) view actors of editor cameras are not authoritative\n", 1))
print("patch_engine: ok")
