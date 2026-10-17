#!/usr/bin/env bash
# Copies Unreal's Engine package from the upstream checkout and applies patch_engine.py.
#   build_engine.sh <UnrealEngine/Source/Engine> <work dir>
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
upstream="${1:?usage: build_engine.sh path/to/Source/Engine workdir}"
work="${2:?}"
rm -rf "$work" && mkdir -p "$work"
cp -r "$upstream/Inc" "$upstream/Src" "$work/"
python3 -I "$here/patch_engine.py" "$work"
