#!/usr/bin/env bash
# Compiles the patched Core (see build_core.sh) for the Lion into <work>/libcore.a.
#   compile_core.sh <work dir> [extra .cpp files to add, e.g. the Lion runtime]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
work="${1:?usage: compile_core.sh workdir [extra.cpp...]}"
shift
res="$(clang --print-resource-dir)/include"
flags=(--target=powerpc-unknown-none-elf -mcpu=7400 -mno-altivec -mno-vsx -O2 -w -Wno-non-pod-varargs
       -fms-extensions -fms-compatibility -fdelayed-template-parsing -D_MSC_VER=1200 -std=gnu++03
       -fexceptions -fno-rtti -fno-pic -fno-stack-protector -ffreestanding -fno-threadsafe-statics
       -DCORE_EXPORTS -nostdinc -nostdinc++ -isystem "$res" -isystem "$here/shim"
       -I"$work/Inc" -I"$work/Src" -I"$here")
mkdir -p "$work/obj"
objs=()
for f in "$work"/Src/*.cpp "$@"; do
  [[ "$(basename "$f")" == UnPlat.cpp ]] && continue
  o="$work/obj/$(basename "${f%.cpp}").o"
  if [[ "$f" == "$work"/Src/* ]]; then
    [[ -f "$o" ]] && { objs+=("$o"); continue; }  # Core objects are cached between runs
    python3 -I "$here/hoist_for.py" "$f" "${flags[@]}" >/dev/null
  fi
  extra=(); [[ "$(basename "$f")" == lionrt.cpp ]] && extra=(-fno-builtin)
  clang++ "${flags[@]}" "${extra[@]}" -c "$f" -o "$o"
  objs+=("$o")
done
rm -f "$work/libcore.a"
llvm-ar rcs "$work/libcore.a" "${objs[@]}"
echo "compile_core: ${#objs[@]} objects -> $work/libcore.a"
