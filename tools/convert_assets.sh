#!/bin/sh
# Rebuilds Assets/rom/ (the audio baked into the boot ROM) from the source files
# in Assets/. Needs ffmpeg and the lionasset tool (cmake --build build --target lionasset).
#   sfx_intro.mp3          the lion's roar           -> roar.pcm   (44.1 kHz mono)
#   sfx_intro_2.mp3        the claws                 -> claws.pcm  (44.1 kHz mono)
#   dashboard_theme.ogg    dashboard theme I         -> theme1.ladp (22.05 kHz stereo IMA ADPCM)
#   dashboard_theme_2.ogg  dashboard theme II        -> theme2.ladp
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
tool=${LIONASSET:-$root/build/lionasset}
src=$root/Assets
out=$root/Assets/rom
tmp=$(mktemp -d)
trap 'rm -r "$tmp"' EXIT
mkdir -p "$out"

ffmpeg -v error -y -i "$src/sfx_intro.mp3" -ac 1 -ar 44100 -f s16le "$tmp/roar.raw"
ffmpeg -v error -y -i "$src/sfx_intro_2.mp3" -ac 1 -ar 44100 -f s16le "$tmp/claws.raw"
"$tool" pcm "$tmp/roar.raw" "$out/roar.pcm"
"$tool" pcm "$tmp/claws.raw" "$out/claws.pcm"
for n in 1 2; do
  in=$src/dashboard_theme.ogg
  [ "$n" = 2 ] && in=$src/dashboard_theme_2.ogg
  ffmpeg -v error -y -i "$in" -ac 2 -ar 22050 -f s16le "$tmp/theme$n.raw"
  "$tool" adpcm "$tmp/theme$n.raw" 22050 "$out/theme$n.ladp"
done
ls -l "$out"
