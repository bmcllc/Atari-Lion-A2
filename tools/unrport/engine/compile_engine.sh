#!/usr/bin/env bash
# Compiles the patched Engine into <work>/libengine.a.
#   compile_engine.sh <engine work dir> <patched core work dir> [extra .cpp files]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
work="${1:?usage: compile_engine.sh engine_workdir core_workdir}"
core="${2:?}"
shift 2
res="$(clang --print-resource-dir)/include"
flags=(--target=powerpc-unknown-none-elf -mcpu=7400 -mno-altivec -mno-vsx -O2 -w -Wno-non-pod-varargs -Wno-address-of-temporary
       -fms-extensions -fms-compatibility -fdelayed-template-parsing -D_MSC_VER=1200 -std=gnu++03
       -fexceptions -fno-rtti -mms-bitfields -fpack-struct=4 -fno-pic -fno-stack-protector -ffreestanding -fno-threadsafe-statics
       -DENGINE_EXPORTS -DNOAMD3D -DGPackage=GPackage_Engine -DhInstance=hInstance_Engine -DDllMain=DllMain_Engine -nostdinc -nostdinc++ -isystem "$res" -isystem "$here/../core/shim"
       -I"$work/Inc" -I"$work/Src" -I"$core/Inc" -I"$core/Src" -I"$here/../core")
mkdir -p "$work/obj"
objs=()
for f in "$work"/Src/*.cpp "$@"; do
  o="$work/obj/$(basename "${f%.cpp}").o"
  if [[ "$f" != "$work"/Src/* ]]; then
    clang++ "${flags[@]}" ${LION_CXXDEFS:-} -c "$f" -o "$o"  # extra sources are rebuilt every time
  elif [[ ! -f "$o" ]]; then
    python3 -I "$here/../core/hoist_for.py" "$f" "${flags[@]}" >/dev/null
    clang++ "${flags[@]}" -c "$f" -o "$o"
  fi
  objs+=("$o")
done
rm -f "$work/libengine.a"
llvm-ar rcs "$work/libengine.a" "${objs[@]}"
echo "compile_engine: ${#objs[@]} objects -> $work/libengine.a"
