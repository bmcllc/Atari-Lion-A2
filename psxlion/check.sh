#!/usr/bin/env bash
# Compiles every runtime file for the Tuffy and stops at the first error: run before a long dev.sh iteration.
#   psxlion/check.sh [extra cflags]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; fw="${FW:-$here/../MK4/work/mk4/psxrecomp}"; out="$(mktemp -d)"; trap 'rm -rf "$out"' EXIT
rd="$(clang --print-resource-dir)"
for f in core hw irq cd sio spu libc stubs main interp_deps gpu gpu_spike watch; do
  clang --target=powerpc-unknown-none-elf -mcpu=7400 -Os -ffreestanding -fno-builtin -nostdinc -isystem "$rd/include" -I"$here/shim" \
        -I"$fw/runtime/include" -I"$fw/runtime/src" -w -DPSX_NO_DEBUG_TOOLS -DPSX_HAS_GAME_DISPATCH -DPSX_GAME_ENTRY=0x10008 -DPSX_GAME_TEXT_END=0x117000 "$@" -c "$here/rt/$f.c" -o "$out/$f.o"
done
echo "rt: ok"
