#!/usr/bin/env python3
"""Applies the PowerPC/clang portability patches to a copy of Unreal's Core (Inc/ and Src/).
Usage: patch_core.py <work dir containing Inc/ and Src/>   (the upstream tree is never modified)"""
import re, sys, pathlib

root = pathlib.Path(sys.argv[1])
edits = []  # (file, old, new, count)

def sub(path, old, new, count=1):
    edits.append((path, old, new, count))

# Old MSVC lets forward-declared enums through and leaks for-loop variables; clang does neither.
sub("Inc/UnNames.h", "enum EName {", "enum EName : int {")
sub("Inc/UnScript.h", "enum EExprToken\n", "enum EExprToken : int\n")
sub("Inc/UnArc.h", "ARCHIVE_ENUM_AS_BYTE(ECodeToken);\n", "")
sub("Inc/Core.h", "// Objects.\nclass	UObject;", "enum EName : int;\nenum EExprToken : int;\n\n// Objects.\nclass	UObject;")
sub("Inc/Core.h", '#include "UnCID.h"', '#include "UnCId.h"')
sub("Inc/UnTemplate.h",
    "\t\tfor( INT i=0; i<Pairs.Num(); i++ )\n\t\t\tif( Pairs(i).Key==Key )\n\t\t\t\tbreak;",
    "\t\tINT i;\n\t\tfor( i=0; i<Pairs.Num(); i++ )\n\t\t\tif( Pairs(i).Key==Key )\n\t\t\t\tbreak;")
sub("Inc/UnMem.h", "\tfriend void* operator new( size_t Size, FMemStack& Mem, INT Count=1, INT Align=DEFAULT_ALIGNMENT );\n", "")
sub("Inc/UnMem.h", "\tfriend void* operator new( size_t Size, FMemStack& Mem, EMemZeroed Tag, INT Count=1, INT Align=DEFAULT_ALIGNMENT );\n", "")
sub("Inc/UnMem.h", "\tfriend void* operator new( size_t Size, FMemStack& Mem, EMemOned Tag, INT Count=1, INT Align=DEFAULT_ALIGNMENT );\n", "")
sub("Inc/UnMem.h", "inline void* operator new( size_t Size, FMemStack& Mem, INT Count, INT Align )", "inline void* operator new( size_t Size, FMemStack& Mem, INT Count=1, INT Align=DEFAULT_ALIGNMENT )")
sub("Inc/UnMem.h", "inline void* operator new( size_t Size, FMemStack& Mem, EMemZeroed Tag, INT Count, INT Align )", "inline void* operator new( size_t Size, FMemStack& Mem, EMemZeroed Tag, INT Count=1, INT Align=DEFAULT_ALIGNMENT )")
sub("Inc/UnMem.h", "inline void* operator new( size_t Size, FMemStack& Mem, EMemOned Tag, INT Count, INT Align )", "inline void* operator new( size_t Size, FMemStack& Mem, EMemOned Tag, INT Count=1, INT Align=DEFAULT_ALIGNMENT )")
sub("Inc/UnMem.h", "\treturn (T*)Mem.PushBytes( Count*sizeof(T), Align );\n\tappMemset( Result, 0xff, Count*sizeof(T) );", "\tBYTE* Result = Mem.PushBytes( Count*sizeof(T), Align );\n\tappMemset( Result, 0xff, Count*sizeof(T) );")

sub("Src/UnLinker.h",
    "\t\t\t\t\tfor\n\t\t\t\t\t(\tFObjectImport* Top = &Import\n\t\t\t\t\t;\tTop->PackageIndex<0\n\t\t\t\t\t;\tTop = &ImportMap(-Top->PackageIndex-1) );",
    "\t\t\t\t\tFObjectImport* Top = &Import;\n\t\t\t\t\tfor( ; Top->PackageIndex<0; Top = &ImportMap(-Top->PackageIndex-1) );")
sub("Src/UnProp.cpp", 'char *Temp = ((*(DWORD *)PropertyValue) & BitMask) ? "True" : "False";', 'const char *Temp = ((*(DWORD *)PropertyValue) & BitMask) ? "True" : "False";')

# The Lion is big-endian: leave __INTEL__ undefined so FArchive byte-swaps (UnArc.h, UnFile.h, Core.cpp).
sub("Inc/UnVcWin32.h", "#define __INTEL__\t1\n", "")

sub("Src/UnFile.cpp", "CORE_API INT DOUBLE appFloor( FLOAT Value )", "CORE_API INT appFloor( FLOAT Value )")

# The Lion has no Engine.u/Core script packages on disc, and the viewer never creates objects:
# do not load the imported packages when opening a map.
sub("Src/UnLinker.h", "\t\t\tfor( INT i=0; i<Summary.ImportCount; i++ )\n\t\t\t\tVerifyImport( i );", "\t\t\t// (import verification skipped: no script packages on the Lion)")

# Old MSVC leaks the loop variable of the three TMap::Find overloads out of the for statement.
for old in ("\t\tfor( INT i=0; i<Pairs.Num(); i++ )\n\t\t\tif( Pairs(i).Key==Key )\n\t\t\t\tbreak;\n\t\tif( i<Pairs.Num() )",
            "\t\tfor (INT i = 0; i < Pairs.Num(); i++)\n\t\t\tif (Pairs(i).Key == Key)\n\t\t\t\tbreak;\n\t\tif (i < Pairs.Num())"):
    sub("Inc/UnTemplate.h", old, "\t\tINT i;\n" + old.replace("INT i", "i", 1), 2)

# Hand-written x86 MMX paths of appLargeMemset/appLargeMemcpy: keep the plain memset/memcpy branch.
mmx = re.compile(r"if \(GIsMMX\)\s*\{\s*__asm\s*\{.*?\n\t\t\}\s*return Dest;\s*\}\s*else\s*\{", re.S)
unfile = root / "Src/UnFile.cpp"
text = unfile.read_text(encoding="latin-1")
text, n = mmx.subn("{", text)
if n != 2:
    sys.exit(f"patch_core: expected 2 MMX blocks in UnFile.cpp, found {n}")
unfile.write_text(text, encoding="latin-1")

for path, old, new, count in edits:
    f = root / path
    text = f.read_text(encoding="latin-1")
    if old not in text:
        sys.exit(f"patch_core: pattern not found in {path}: {old[:60]!r}")
    f.write_text(text.replace(old, new, count), encoding="latin-1")
print(f"patch_core: {len(edits)} edits applied")
