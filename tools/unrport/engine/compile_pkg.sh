#!/usr/bin/env bash
# Compiles one native package into <work>/lib<pkg>.a.
#   compile_pkg.sh <Pkg> <work dir> <include dirs, colon separated> [extra .cpp ...]
# Sources: <work>/Src/*.cpp (patched copies) plus the extra files (rebuilt every time).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
pkg="${1:?}"; work="${2:?}"; incs="${3:?}"
shift 3
res="$(clang --print-resource-dir)/include"
upper="$(echo "$pkg" | tr a-z A-Z)"
flags=(--target=powerpc-unknown-none-elf -mcpu=7400 -mno-altivec -mno-vsx -O2 -w -Wno-non-pod-varargs -Wno-address-of-temporary
       -fms-extensions -fms-compatibility -fdelayed-template-parsing -D_MSC_VER=1200 -std=gnu++03
       -fexceptions -fno-rtti -mms-bitfields -fpack-struct=4 -fno-pic -fno-stack-protector -ffreestanding -fno-threadsafe-statics
       -D${upper}_EXPORTS -DNOAMD3D -DGPackage=GPackage_$pkg -DhInstance=hInstance_$pkg -DDllMain=DllMain_$pkg
       -nostdinc -nostdinc++ -isystem "$res" -isystem "$here/../core/shim" -I"$work/Inc" -I"$work/Src" -I"$here/../core")
IFS=: read -ra dirs <<< "$incs"
for d in "${dirs[@]}"; do flags+=(-I"$d"); done
mkdir -p "$work/obj"
objs=()
for f in "$work"/Src/*.cpp "$@"; do
  [[ -f "$f" ]] || continue
  o="$work/obj/$(basename "${f%.cpp}").o"
  if [[ "$f" != "$work"/Src/* ]]; then
    clang++ "${flags[@]}" -c "$f" -o "$o"
  elif [[ ! -f "$o" ]]; then
    python3 -I "$here/../core/hoist_for.py" "$f" "${flags[@]}" >/dev/null
    clang++ "${flags[@]}" -c "$f" -o "$o"
  fi
  objs+=("$o")
done
rm -f "$work/lib$pkg.a"
llvm-ar rcs "$work/lib$pkg.a" "${objs[@]}"
echo "compile_pkg: $pkg: ${#objs[@]} objects -> $work/lib$pkg.a"
