#!/usr/bin/env bash
# Builds the Unreal-on-Lion port: client.bin (PowerPC) + a bootable disc from a .unr map.
#   tools/unrport/build.sh path/to/Map.unr out.iso [--wall P.utx:Name] [--floor P.utx:Name] [--ceiling P.utx:Name] [--gain N]
# Requires clang with the PowerPC target, ld.lld, llvm-objcopy and a built `unrport`.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
map="${1:?usage: build.sh Map.unr out.iso}"
out="${2:-unreal.iso}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cflags=(--target=powerpc-unknown-none-elf -mcpu=7400 -mno-altivec -mno-vsx -ffreestanding -fno-builtin
        -fno-pic -fno-stack-protector -O2 -Wall)
clang "${cflags[@]}" -c "$here/crt0.S" -o "$tmp/crt0.o"
clang "${cflags[@]}" -c "$here/client.c" -o "$tmp/client.o"
ld.lld -T "$here/link.ld" -nostdlib "$tmp/crt0.o" "$tmp/client.o" -o "$tmp/client.elf"
llvm-objcopy -O binary "$tmp/client.elf" "$tmp/client.bin"
"${UNRPORT:-build/unrport}" "$map" "$tmp/client.bin" "$out" "${@:3}"
