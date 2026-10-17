#!/usr/bin/env python3
"""Patches the scratch copy of the Editor sources that build.sh compiles (the checkout is never modified).
usage: patch_editor.py <editor work dir>"""
import pathlib, re, sys

root = pathlib.Path(sys.argv[1]) / "Src"

def edit(name, fn):
    p = root / name
    t = p.read_text(encoding="latin-1")
    n = fn(t)
    p.write_text(n, encoding="latin-1")

def sub(old, new, count=1):
    def f(t):
        if old not in t:
            sys.exit(f"patch_editor: pattern not found: {old[:50]!r}")
        return t.replace(old, new, count)
    return f

# MSVC leaks for-loop variables; clang does not.
for p in root.glob("*.cpp"):
    t = p.read_text(encoding="latin-1")
    t = t.replace("for( It=FStaticBrushIterator(Level); It; ++It )", "for( FStaticBrushIterator It(Level); It; ++It )")
    p.write_text(t, encoding="latin-1")

# There is no undo buffer on the device.
edit("UnShadow.cpp", sub("Trans->Reset(", "if(Trans) Trans->Reset("))
edit("UnBsp.cpp", sub("Trans->ForceOverflow(", "if(Trans) Trans->ForceOverflow("))

# Which surfaces a light can reach: the editor renders six views from the light. Every surface is a candidate
# instead (the per-texel ray trace of LightBspSurf does the occlusion), which needs no render pass.
edit("UnShadow.cpp", sub("GEditor->Render->GetVisibleSurfs( Viewport, iSurfs );",
    "for( INT iCand=0; iCand<Level->Model->Surfs->Num(); iCand++ ) iSurfs.AddItem( iCand );"))
print("patch_editor: ok")
