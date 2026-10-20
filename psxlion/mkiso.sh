#!/usr/bin/env bash
# Packs a Lion disc for the PS1 port: BOOT.LION (stage 0), PSX.IMG (the recompiled game + BIOS + runtime) and GAME.BIN (the raw 2352-byte-sector data track).
#   psxlion/mkiso.sh <build out dir (BOOT.LION, PSX.IMG)> <game.bin> <out.iso>
set -euo pipefail
dir="$(cd "${1:?build out dir}" && pwd)"; bin="$(cd "$(dirname "${2:?game.bin}")" && pwd)/$(basename "$2")"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
ln -s "$dir/BOOT.LION" "$tmp/BOOT.LION"; ln -s "$dir/PSX.IMG" "$tmp/PSX.IMG"; ln -s "$bin" "$tmp/GAME.BIN"
genisoimage -quiet -iso-level 3 -follow-links -V PSXLION -o "${3:?out.iso}" "$tmp"
