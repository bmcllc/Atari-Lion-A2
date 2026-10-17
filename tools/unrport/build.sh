#!/usr/bin/env bash
# Builds the Unreal-on-Lion port: the PowerPC viewer (with Unreal's Core and Engine compiled for the Lion)
# and a bootable disc from a .unr map.
#   UNREAL_SRC=path/to/UnrealEngine tools/unrport/build.sh Map.unr out.iso \
#       [--wall P.utx:Name] [--floor P.utx:Name] [--ceiling P.utx:Name] [--gain N]
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
render="$cache-render"; sound="$cache-sound"; lion="$cache-liondrv"
if [[ ! -d "$render/Inc" ]]; then  # Render needs no source patches beyond the for-scope fixes done by compile_pkg.sh
  mkdir -p "$render" && cp -r "$src/Source/Render/Inc" "$src/Source/Render/Src" "$render/"
fi
if [[ ! -d "$sound/Src" ]]; then  # the null audio device, minus its Windows includes
  mkdir -p "$sound/Src" "$sound/Inc"
  grep -v -e '#include <windows.h>' -e '#include <mmsystem.h>' "$src/Source/SoundDrv/Null.cpp" > "$sound/Src/Null.cpp"
fi
mkdir -p "$lion/Src" "$lion/Inc"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"$here/core/compile_core.sh" "$cache" "$here/core/lionrt.cpp" "$here/core/lionplat.cpp" "$here/core/level.cpp" "$here/client.cpp"
"$here/engine/compile_engine.sh" "$engine" "$cache" "$here/engine/enginetest.cpp" "$here/engine/lionboot.cpp"
incs="$engine/Inc:$cache/Inc:$cache/Src"
"$here/engine/compile_pkg.sh" Render "$render" "$incs"
"$here/engine/compile_pkg.sh" SoundDrv "$sound" "$incs"
"$here/engine/compile_pkg.sh" LionDrv "$lion" "$incs:$here/liondrv" "$here/liondrv/LionDrv.cpp" "$here/liondrv/LionRenDev.cpp"
clang --target=powerpc-unknown-none-elf -c "$here/crt0.S" -o "$tmp/crt0.o"

link() {
  ld.lld -m elf32ppc -T "$here/link.ld" -nostdlib "$tmp/crt0.o" "$@" --whole-archive "$cache/libcore.a" \
      "$engine/libengine.a" "$render/libRender.a" "$sound/libSoundDrv.a" "$lion/libLionDrv.a" --no-whole-archive -o "$tmp/client.elf"
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

scripts="$src/Unreal/Scripts"
"${UNRPORT:-build/unrport}" "$map" "$tmp/client.bin" "$out" "${@:3}" \
    --file "$here/Lion.ini" --file "$scripts/Core.u" --file "$scripts/Engine.u" \
    --file "$src/Engine/Localization/Core.int" --file "$src/Engine/Localization/Engine.int"
