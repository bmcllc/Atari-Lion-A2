#!/usr/bin/env bash
# Prepares a PowerPC-ready copy of Unreal's Core: copies Inc/ and Src/ from the upstream tree,
# applies patch_core.py and hoist_for.py. Nothing in the upstream tree is modified.
#   build_core.sh <UnrealEngine/Source/Core> <work dir>
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
upstream="${1:?usage: build_core.sh path/to/Source/Core workdir}"
work="${2:?}"
rm -rf "$work" && mkdir -p "$work"
cp -r "$upstream/Inc" "$upstream/Src" "$work/"
python3 -I "$here/patch_core.py" "$work"
