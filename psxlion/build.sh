#!/usr/bin/env bash
# Builds and links the PS1-on-Lion image: generated game + recompiled BIOS + psxlion/rt + the framework's gte.cpp.
#   psxlion/build.sh <project dir (has generated/)> <psxrecomp dir> <out dir>
# Output: <out>/psx.elf. Undefined symbols are reported, not fatal, while the runtime is being ported.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
proj="$(cd "${1:?project dir}" && pwd)"; fw="$(cd "${2:?psxrecomp dir}" && pwd)"; mkdir -p "${3:?out dir}"; out="$(cd "$3" && pwd)"
rd="$(clang --print-resource-dir)"
load="$(grep -m1 '^load_address' "$proj/game.toml" | sed 's/.*= *"\(.*\)"/\1/')"; tsize="$(grep -m1 '^text_size' "$proj/game.toml" | sed 's/.*= *"\(.*\)"/\1/')"
text_end=$(printf '0x%X' $(( (load & 0x1FFFFFFF) + tsize )))   # physical end of the game text
entry="$(grep -m1 '^entry_pc' "$proj/game.toml" | sed 's/.*= *"\(.*\)"/\1/')"   # game.toml: entry_pc = "0x00010008"
cf=(--target=powerpc-unknown-none-elf -mcpu=7400 -Os -ffreestanding -fno-builtin -mllvm -disable-loop-idiom-memcpy
    -mllvm -disable-loop-idiom-memset -DPSX_HAS_GAME_DISPATCH -DPSX_HAS_GAME_CODEGEN -DPSX_GAME_ENTRY=$entry -DPSX_GAME_TEXT_END=$text_end -nostdinc -isystem "$rd/include" -I"$here/shim" -I"$fw/runtime/include" -w -ffunction-sections -fdata-sections -DMDEC_TRACE_CAP=8)
# BIOS: PSX_BIOS=OpenBIOS (bundled with the framework, the default) or SCPH1001 (recompiled from the player's dump into
# psxlion/work/bios/SCPH1001 by psxrecomp-bios; BIOS=<path to the dump> embeds the ROM). Games that patch the kernel by
# Sony's layout (GTA, MML: libapi's pad patch) need SCPH1001.
bios_name="${PSX_BIOS:-OpenBIOS}"
if [ "$bios_name" = OpenBIOS ]; then bios_gen="$fw/generated"; bios_rom="${BIOS:-$fw/bios/openbios.bin}"
else bios_gen="$here/work/bios/$bios_name"; bios_rom="${BIOS:?BIOS=<path to the $bios_name dump>}"; fi
cf+=(-DPSX_BIOS_NAME=$bios_name)
# PSX_RT_ONLY=1 relinks with the game/BIOS objects already in <out> (runtime iterations are seconds instead of minutes).
if [ "${PSX_RT_ONLY:-0}" != 1 ]; then
  "$here/build_game.sh" "$proj" "$fw" "$out/game" -Os
fi
if [ "${PSX_RT_ONLY:-0}" != 1 ] || [ ! -f "$out/bios-$bios_name/${bios_name}_dispatch.o" ]; then
  mkdir -p "$out/bios-$bios_name"
  for f in ${bios_name}_full ${bios_name}_dispatch; do
    clang "${cf[@]}" -DPSX_NO_DEBUG_TOOLS -I"$bios_gen" -c "$bios_gen/$f.c" -o "$out/bios-$bios_name/$f.o" &
  done
  wait
fi
mkdir -p "$out/rt"
for f in core hw irq cd sio spu libc stubs main interp_deps gpu gpu_spike watch; do clang "${cf[@]}" -DPSX_NO_DEBUG_TOOLS ${PSX_EXTRA_CFLAGS:-} -c "$here/rt/$f.c" -o "$out/rt/$f.o"; done
clang "${cf[@]}" -DPSX_NO_DEBUG_TOOLS ${PSX_EXTRA_CFLAGS:-} -I"$fw/runtime/src" -c "$fw/runtime/src/gpu_sw_renderer.c" -o "$out/rt/gpu_sw_renderer.o"
# mdec.c reads its output words with memcpy (host order); the Tuffy is big-endian, so the copy assembles them little-endian
# like the PS1 (and like the function's own byte-by-byte tail).
mkdir -p "$out/patch"
sed 's/memcpy(&value, mdec.output + start_pos, sizeof(value));/{ const uint8_t *o_ = mdec.output + start_pos; value = (uint32_t)o_[0] | (uint32_t)o_[1] << 8 | (uint32_t)o_[2] << 16 | (uint32_t)o_[3] << 24; }/' \
    "$fw/runtime/src/mdec.c" > "$out/patch/mdec.c"
grep -q 'o_\[3\] << 24' "$out/patch/mdec.c" || { echo "mdec.c: output-word patch did not apply" >&2; exit 1; }
clang "${cf[@]}" -DPSX_NO_DEBUG_TOOLS ${PSX_EXTRA_CFLAGS:-} -I"$fw/runtime/src" -c "$out/patch/mdec.c" -o "$out/rt/mdec.o"
clang --target=powerpc-unknown-none-elf -mcpu=7400 -DLION_MAIN=lion_main -DZERO_BSS -c "$here/rt/crt0.S" -o "$out/rt/crt0.o"
clang --target=powerpc-unknown-none-elf -mcpu=7400 -c "$here/rt/abandon.S" -o "$out/rt/abandon.o"
clang --target=powerpc-unknown-none-elf -mcpu=7400 -DGAME_EXE_PATH="\"$proj/disc/$(basename "$(grep -m1 '^exe' "$proj/game.toml" | sed 's/.*= *"\(.*\)"/\1/')")\"" -c "$here/rt/gameexe.S" -o "$out/rt/gameexe.o"
clang --target=powerpc-unknown-none-elf -mcpu=7400 -DBIOS_PATH="\"$bios_rom\"" -c "$here/rt/bios.S" -o "$out/rt/bios.o"
# The interpreter's diagnostic tables (entry-PC counters 8 MB, lockstep trace 768 KB, widescreen site caches) are sized for
# the PC; on the Lion they would push a big game past the 24 MB of SDRAM. A copy with small tables is compiled instead.
mkdir -p "$out/patch"
sed 's/^#define DIRTY_RAM_PC_TABLE_SIZE 262144/#define DIRTY_RAM_PC_TABLE_SIZE 1024/' "$fw/runtime/include/dirty_ram_interp.h" > "$out/patch/dirty_ram_interp.h"
sed -e 's/LS_TRACE_CAP = 65536/LS_TRACE_CAP = 16/' -e 's/^#define WS_SITE_CACHE_SLOTS 8192u/#define WS_SITE_CACHE_SLOTS 256u/' \
    "$fw/runtime/src/dirty_ram_interp.c" > "$out/patch/dirty_ram_interp.c"
clang "${cf[@]}" -DPSX_NO_DEBUG_TOOLS -I"$out/patch" -I"$fw/runtime/src" -I"$bios_gen" -c "$out/patch/dirty_ram_interp.c" -o "$out/rt/dirty_ram_interp.o"
clang++ "${cf[@]}" -fno-exceptions -fno-rtti -nostdinc++ -DPSX_NO_DEBUG_TOOLS -c "$fw/runtime/src/gte.cpp" -o "$out/rt/gte.o"
rm -f "$out/psx.elf"
ld.lld -m elf32ppc -nostdlib -T "$here/link.ld" --gc-sections --warn-unresolved-symbols -u psx_dispatch_game_compiled \
    "$out"/game/*.o "$out/bios-$bios_name"/*.o "$out"/rt/*.o -o "$out/psx.elf" 2> "$out/link.log" || { grep -v '^>>>' "$out/link.log" | sort -u | head -20; exit 1; }
grep -o 'undefined symbol: .*' "$out/link.log" | sort -u || true
llvm-size "$out/psx.elf"
python3 -I "$here/mkimg.py" "$out/psx.elf" "$out/PSX.IMG"
# stage 0 = BOOT.LION
mkdir -p "$out/s0"
clang --target=powerpc-unknown-none-elf -mcpu=7400 -DLION_MAIN=stage0_main -c "$here/rt/crt0.S" -o "$out/s0/crt0.o"
clang "${cf[@]}" -c "$here/rt/stage0.c" -o "$out/s0/stage0.o"
ld.lld -m elf32ppc -nostdlib -T "$here/link0.ld" "$out"/s0/*.o "$out/rt/libc.o" -o "$out/stage0.elf"
python3 -I "$here/mklion.py" "$out/stage0.elf" "$out/BOOT.LION"
