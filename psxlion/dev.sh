#!/usr/bin/env bash
# One bring-up iteration: build the runtime (and the game objects the first time), update the disc, run it headless,
# print the log.
#   psxlion/dev.sh <project dir (game.toml, generated/)> <work dir> [frames] [data track .bin]
# Env: PSX_BIOS / BIOS (see build.sh), PSX_EXTRA_CFLAGS, FW (psxrecomp checkout; default MK4/work/mk4/psxrecomp),
#      UNTIL (extra --until-log texts, '|'-separated), TAIL (log lines shown, default 40),
#      LION_ARGS (more emulator options, e.g. "--hold 0:0 --hold 900:1000 --hold 910:0 --dump-every 300").
# The disc is <work>/psx.iso: packed once by mkiso.sh (needs the .bin), afterwards patched in place by patchiso.py.
# Outputs: <work>/run.log (emulator summary), run.bin (log ring dump), run.ppm (last frame), run.txt (the log as text).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; root="$(cd "$here/.." && pwd)"
proj="${1:?project dir}"; work="${2:?work dir}"; frames="${3:-2000}"; bin="${4:-}"
fw="${FW:-$root/MK4/work/mk4/psxrecomp}"
mkdir -p "$work"
if [ -n "$(ls "$work"/out/game/*.o 2>/dev/null | head -1)" ]; then rt_only=1; else rt_only=0; fi
PSX_RT_ONLY=$rt_only "$here/build.sh" "$proj" "$fw" "$work/out" > "$work/build.log" 2>&1 || { tail -20 "$work/build.log"; echo "dev.sh: build failed" >&2; exit 1; }
if [ -f "$work/psx.iso" ]; then python3 -I "$here/patchiso.py" "$work/psx.iso" "$work/out"
else "$here/mkiso.sh" "$work/out" "${bin:?data track .bin (first run only)}" "$work/psx.iso"; fi
stop="psx: stopped|BREAK|overflow${UNTIL:+|$UNTIL}"
SECONDS=0
"$root/build/lion" "$work/psx.iso" --fast-boot --frames "$frames" --until-log "$stop" \
    --dump-ram "$work/run.bin,0x1F0000,0x20000" --dump "$work/run.ppm" ${LION_ARGS:-} > "$work/run.log" 2>&1 || true
python3 -I "$here/logcat.py" "$work/run.bin" > "$work/run.txt"
head -2 "$work/run.log"; echo "(${SECONDS}s)"
grep -v '^mmio write\|^ww \|^cd INT\|^S \|^W \|^D3\|^DMA3\|^IRQ\|^DICR\|irq: deliverable\|^       i_stat' "$work/run.txt" | tail -n "${TAIL:-40}"
