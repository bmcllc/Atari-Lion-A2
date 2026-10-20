#!/usr/bin/env bash
# Compiles the psxrecomp-generated game C for the Tuffy (PowerPC 7400) without the cycle instrumentation.
#   psxlion/build_game.sh <project dir with generated/> <psxrecomp dir> <out dir> [-Os|-O2]
set -euo pipefail
export here="$(cd "$(dirname "$0")" && pwd)"
export proj="$(cd "${1:?project dir}" && pwd)" fw="$(cd "${2:?psxrecomp dir}" && pwd)" opt="${4:--Os}"
mkdir -p "${3:?out dir}"; export out="$(cd "$3" && pwd)"
export rd="$(clang --print-resource-dir)"
compile_one() {
  clang --target=powerpc-unknown-none-elf -mcpu=7400 "$opt" -ffreestanding -nostdinc -isystem "$rd/include" \
        -I"$here/shim" -I"$fw/runtime/include" -I"$proj/generated" -w -DPSX_NO_DEBUG_TOOLS -fno-exceptions \
        -ffunction-sections -c "$1" -o "$out/$(basename "${1%.c}").o"
}
export -f compile_one
printf '%s\0' "$proj"/generated/*_full_*.c "$proj"/generated/*_dispatch.c |
  xargs -0 -P "$(nproc)" -I{} bash -c 'compile_one "$@"' _ {}
llvm-size --totals "$out"/*.o | tail -1
