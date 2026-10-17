#!/usr/bin/env bash
# Builds the Unreal-on-Lion port: the PowerPC viewer (with Unreal's Core compiled for the Lion) and a
# bootable disc from a .unr map.
#   UNREAL_SRC=path/to/UnrealEngine tools/unrport/build.sh Map.unr out.iso \
#       [--wall P.utx:Name] [--floor P.utx:Name] [--ceiling P.utx:Name] [--gain N]
# UNREAL_SRC is a checkout of https://github.com/RedPandaProjects/UnrealEngine (its Core is patched in a
# scratch copy; the checkout is never modified). Requires clang with the PowerPC target, ld.lld,
# llvm-ar, llvm-objcopy, python3 and a built `unrport`.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
map="${1:?usage: UNREAL_SRC=... build.sh Map.unr out.iso [options]}"
out="${2:-unreal.iso}"
src="${UNREAL_SRC:?set UNREAL_SRC to a RedPandaProjects/UnrealEngine checkout}"
cache="${UNRPORT_CACHE:-build/unrport-core}"
if [[ ! -d "$cache/Inc" ]]; then
  "$here/core/build_core.sh" "$src/Source/Core" "$cache"
fi
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"$here/core/compile_core.sh" "$cache" "$here/core/lionrt.cpp" "$here/core/lionplat.cpp" "$here/core/level.cpp" "$here/client.cpp"
clang --target=powerpc-unknown-none-elf -c "$here/crt0.S" -o "$tmp/crt0.o"
ld.lld -m elf32ppc -T "$here/link.ld" -nostdlib "$tmp/crt0.o" "$cache/libcore.a" -o "$tmp/client.elf"
llvm-objcopy -O binary "$tmp/client.elf" "$tmp/client.bin"
"${UNRPORT:-build/unrport}" "$map" "$tmp/client.bin" "$out" "${@:3}"
