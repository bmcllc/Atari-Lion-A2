#!/usr/bin/env bash
# Builds the Unreal-on-Lion port: the PowerPC viewer (with Unreal's Core and Engine compiled for the Lion)
# and a bootable disc from a .unr map.
#   UNREAL_SRC=path/to/UnrealEngine tools/unrport/build.sh Map.unr out.iso \
#       [--wall P.utx:Name] [--floor P.utx:Name] [--ceiling P.utx:Name] [--gain N] \
#       [--start Name.unr] [--next Name.unr] [--song Package.Object] [--cdfile Other.unr]...
# Without --start the game begins in the stock Entry level, which the client fills with a level built on the device.
# --start makes another map (put on the disc with --cdfile, together with its textures/sounds/music packages) the first
# level; maps are played as they are, and travel between them is the engine's (Teleporters, triggers, "open").
# UNREAL_SRC is a checkout of https://github.com/RedPandaProjects/UnrealEngine (its Core and Engine are
# patched in scratch copies; the checkout is never modified). Requires clang with the PowerPC target, ld.lld,
# llvm-ar, llvm-nm, llvm-objcopy, python3 and a built `unrport`.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
map="${1:?usage: UNREAL_SRC=... build.sh Map.unr out.iso [options]}"
out="${2:-unreal.iso}"
src="${UNREAL_SRC:?set UNREAL_SRC to a RedPandaProjects/UnrealEngine checkout}"
cache="${UNRPORT_CACHE:-build/unrport-core}"
engine="$cache-engine"
[[ -d "$cache/Inc" ]] || "$here/core/build_core.sh" "$src/Source/Core" "$cache"
[[ -d "$engine/Inc" ]] || "$here/engine/build_engine.sh" "$src/Source/Engine" "$engine"
render="$cache-render"; sound="$cache-sound"; lion="$cache-liondrv"; fire="$cache-fire"
if [[ ! -d "$render/Inc" ]]; then  # Render: smaller tables (patch_render.py), plus the for-scope fixes done by compile_pkg.sh
  mkdir -p "$render" && cp -r "$src/Source/Render/Inc" "$src/Source/Render/Src" "$render/"
  python3 -I "$here/render/patch_render.py" "$render"
fi
if [[ ! -d "$fire/Inc" ]]; then  # procedural textures (fire, water, ice): C paths instead of x86 assembler
  mkdir -p "$fire" && cp -r "$src/Source/Fire/Inc" "$src/Source/Fire/Src" "$fire/"
  python3 -I "$here/fire/patch_fire.py" "$fire"
fi
if [[ ! -d "$sound/Src" ]]; then  # the null audio device, minus its Windows includes
  mkdir -p "$sound/Src" "$sound/Inc"
  grep -v -e '#include <windows.h>' -e '#include <mmsystem.h>' "$src/Source/SoundDrv/Null.cpp" > "$sound/Src/Null.cpp"
fi
editor="$cache-editor"
if [[ ! -d "$editor/Src" ]]; then  # CSG, BSP builder, visibility and lighting of the level editor, to build maps on the device
  mkdir -p "$editor/Src" "$editor/Inc"
  for f in UnBsp UnEdCsg UnVisi UnShadow UnTopics Editor UnEdTran; do cp "$src/Source/Editor/Src/$f.cpp" "$editor/Src/"; done
  cp "$src/Source/Editor/Src/"*.h "$src/Source/Editor/Inc/"* "$src/Source/Render/Inc/"*.h "$editor/Inc/"
  python3 -I "$here/editor/patch_editor.py" "$editor"
fi
mkdir -p "$lion/Src" "$lion/Inc"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"$here/core/compile_core.sh" "$cache" "$here/core/lionrt.cpp" "$here/core/lionplat.cpp" "$here/client.cpp"
"$here/engine/compile_engine.sh" "$engine" "$cache" "$here/engine/enginetest.cpp" "$here/engine/lionboot.cpp"
incs="$engine/Inc:$cache/Inc:$cache/Src"
"$here/engine/compile_pkg.sh" Render "$render" "$incs"
"$here/engine/compile_pkg.sh" SoundDrv "$sound" "$incs"
"$here/engine/compile_pkg.sh" Fire "$fire" "$incs"
"$here/engine/compile_pkg.sh" Editor "$editor" "$incs:$render/Inc" "$here/editor/LionEditor.cpp" "$here/editor/LionMap.cpp"
"$here/engine/compile_pkg.sh" LionDrv "$lion" "$incs:$here/liondrv" "$here/liondrv/LionDrv.cpp" "$here/liondrv/LionRenDev.cpp" "$here/liondrv/LionAudio.cpp" "$here/liondrv/LionMod.cpp"
clang --target=powerpc-unknown-none-elf -c "$here/crt0.S" -o "$tmp/crt0.o"

link() {
  ld.lld -m elf32ppc -T "$here/link.ld" -nostdlib "$tmp/crt0.o" "$@" --whole-archive "$cache/libcore.a" \
      "$engine/libengine.a" "$render/libRender.a" "$fire/libFire.a" "$sound/libSoundDrv.a" "$editor/libEditor.a" "$lion/libLionDrv.a" --no-whole-archive -o "$tmp/client.elf"
}
# Native class objects are found by name (UPackage::GetDllExport): link once, collect the autoclass* symbols,
# generate the export table and link again.
link
llvm-nm "$tmp/client.elf" | awk '$3 ~ /^(autoclass|int[A-Z])/ {print $3}' | sort > "$tmp/exports.txt"
python3 -I - "$tmp/exports.txt" "$tmp/exports.cpp" <<'PY'
import sys
names = [n.strip() for n in open(sys.argv[1]) if n.strip()]
with open(sys.argv[2], "w") as f:
    f.write('extern "C" {\n')
    for n in names:
        f.write(f"extern char {n}[];\n")
    f.write("struct LionExport { const char* name; void* addr; };\n")
    f.write("LionExport g_lion_exports[] = {\n")
    for n in names:
        f.write(f'  {{"{n}", {n}}},\n')
    f.write("  {0, 0}};\n}\n")
PY
clang++ --target=powerpc-unknown-none-elf -mcpu=7400 -O2 -ffreestanding -fno-exceptions -fno-rtti -c "$tmp/exports.cpp" -o "$tmp/exports.o"
link "$tmp/exports.o"
llvm-objcopy -O binary "$tmp/client.elf" "$tmp/client.bin"
# The image (code, data, bss) must end below the log ring at 0x1F0000, where the RAM disc and heap's neighbours start.
bss_end="$(llvm-size -A "$tmp/client.elf" | awk '$1 == ".bss" {print $3 + $2}')"
if (( bss_end > 0x1F0000 )); then echo "build.sh: the client image ends at $(printf %x "$bss_end"), past the log ring (1F0000)" >&2; exit 1; fi

scripts="$src/Unreal/Scripts"
opts=("${@:3}")
ini="$here/Lion.ini"
extra_ini=()
for ((i = 0; i < ${#opts[@]}; i++)); do  # --start Name.unr / --song Package.Object / --next Map.unr go to Lion.ini
  case "${opts[i]}" in
    --start) extra_ini+=("start=${opts[i+1]}"); unset 'opts[i]' 'opts[i+1]'; i=$((i+1)) ;;
    --song) extra_ini+=("Song=${opts[i+1]}"); unset 'opts[i]' 'opts[i+1]'; i=$((i+1)) ;;
    --next) extra_ini+=("NextMap=${opts[i+1]}"); unset 'opts[i]' 'opts[i+1]'; i=$((i+1)) ;;
  esac
done
opts=("${opts[@]}")
if ((${#extra_ini[@]})); then
  ini="$tmp/Lion.ini"
  cp "$here/Lion.ini" "$ini"
  printf '\n[Lion]\n' >> "$ini"
  for kv in "${extra_ini[@]}"; do
    if [[ "$kv" == start=* ]]; then sed -i "s/^LocalMap=.*/LocalMap=${kv#start=}/" "$ini"; else echo "$kv" >> "$ini"; fi
  done
fi
pack() {  # pack <out.iso> [extra unrport options]
  local iso="$1"; shift
  "${UNRPORT:-build/unrport}" "$map" "$tmp/client.bin" "$iso" "${opts[@]}" \
      --file "$ini" --file "$scripts/Core.u" --file "$scripts/Fire.u" --file "$scripts/IpDrv.u" \
      --file "$src/Engine/Localization/Core.int" --file "$src/Engine/Localization/Engine.int" \
      --cdfile "$scripts/Engine.u" --cdfile "$scripts/Editor.u" --cdfile "$scripts/UNreal.u" "$@"
}  # UNreal.u is the game (37 MB: scripts, meshes, textures, sounds), streamed from the CD

# Loading reads the disc in chunks, and every seek costs ~100 ms. The client records which chunks it touches and in
# which order; a second packing lays them out in that order (PRELOAD.BIN) so loading reads long runs instead. The
# recording is a headless run of the first disc (cached next to the output; UNRPORT_RETRACE=1 redoes it).
trace="${out%.iso}.trace"
lion="${LION:-build/lion}"
if [[ -x "$lion" && ( ! -f "$trace" || -n "${UNRPORT_RETRACE:-}" ) ]]; then
  pack "$tmp/record.iso" > /dev/null
  addr="$(llvm-nm "$tmp/client.elf" | awk '$3 == "g_cd_trace" {print $1}')"
  "$lion" --headless --fast-boot --frames "${UNRPORT_TRACE_FRAMES:-22000}" --hold 8500:11 --hold 11000:15 --hold 14000:19 --hold 17000:11 \
      --dump-ram "$tmp/trace.bin,$addr,10008" "$tmp/record.iso" > "$tmp/record.log" 2>&1 || true
  cp "$tmp/trace.bin" "$trace"
fi
if [[ -f "$trace" ]]; then
  pack "$out" --preload "$trace"
else
  pack "$out"
fi
