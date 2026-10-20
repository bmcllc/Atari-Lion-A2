#!/usr/bin/env bash
# Builds a disc that plays a map of the retail Unreal (your own copy of the CD, files extracted to <retail dir>).
#   unreal/build_retail.sh <retail dir> [Map.unr] [out.iso]
# <retail dir> has Maps/ Textures/ Sounds/ Music/ System200/ (the version-61 scripts of the retail CD). The stock Core,
# Fire, IpDrv and Editor packages and the .int files are not on the CD in version 61: they are taken from BASE_ISO,
# a disc built earlier with build.sh from the upstream sources (default build/unreal.iso). Level of detail and
# memory trade-offs of the retail path are described in README.md ("Mapas originais").
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
retail="$(cd "${1:?usage: build_retail.sh <retail dir> [Map.unr] [out.iso]}" && pwd)"
map="${2:-Vortex2.unr}"
out="${3:-build/retail.iso}"
base="${BASE_ISO:-build/unreal.iso}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src/Unreal/Scripts" "$work/src/Engine/Localization"
isoinfo -i "$base" -x '/BOOT.LION;1' > "$work/base.lion"
python3 -I - "$work/base.lion" "$work/src" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
i = d.find(b'KSDR')  # the RAM disc: magic, count, {48-byte name, offset, size}...
n = struct.unpack('>I', d[i + 4:i + 8])[0]
dst = {'Core.u': 'Unreal/Scripts/', 'Fire.u': 'Unreal/Scripts/', 'IpDrv.u': 'Unreal/Scripts/',
       'Core.int': 'Engine/Localization/', 'Engine.int': 'Engine/Localization/'}
for k in range(n):
    e = d[i + 8 + k * 56:i + 8 + (k + 1) * 56]
    name = e[:48].rstrip(b'\0').decode()
    off, size = struct.unpack('>II', e[48:56])
    if name in dst: open(sys.argv[2] + '/' + dst[name] + name, 'wb').write(d[i + off:i + off + size])
PY
isoinfo -i "$base" -x '/EDITOR.U;1' > "$work/src/Unreal/Scripts/Editor.u"
cp "$retail/System200/Engine.u" "$work/src/Unreal/Scripts/Engine.u"
cp "$retail/System200/UnrealI.u" "$work/src/Unreal/Scripts/UnrealI.u"
args=()
for f in "$retail"/Textures/*.utx "$retail"/Sounds/*.uax "$retail"/Music/*.umx; do args+=(--cdfile "$f"); done
UNREAL_SRC="$work/src" UNRPORT_GAME=UnrealI "$here/build.sh" "$retail/Maps/Entry.unr" "$out" \
    --start "$map" --cdfile "$retail/Maps/$map" "${args[@]}"
