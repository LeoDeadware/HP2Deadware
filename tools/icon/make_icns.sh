#!/bin/bash
# make_icns.sh <default.xbe> <out.icns>: the app icon from the disc's title image (XBE $$XTIMAGE,
# 128x128), made at install time (never committed). Upscaled 4x by Real-ESRGAN when it is
# installed (.cache/tools/realesrgan, tools/hdui/get_esrgan.sh), else by sips; iconutil builds
# the .icns (16..1024 px). squircle.swift fits the round Xbox art into the macOS icon grid
# (824 px rounded square, transparent margin) so it sits in the Finder container like native apps.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"   # repo tools/icon, or the DMG app's Resources/build
ROOT="$(cd "$HERE/../.." && pwd)"
XBE="$1"; OUT="$2"
T="$(mktemp -d "${TMPDIR:-/tmp}/hp2_icon.XXXXXX")"; trap 'rm -rf "$T"' EXIT
python3 -I "$HERE/xbe_title_image.py" "$XBE" "$T/title.png" >/dev/null
E="$ROOT/.cache/tools/realesrgan/x"
big="$T/title.png"
if [[ -x "$E/realesrgan-ncnn-vulkan" ]] &&
   "$E/realesrgan-ncnn-vulkan" -i "$T/title.png" -o "$T/x4.png" -m "$E/models" -n realesrgan-x4plus >/dev/null 2>&1; then
  big="$T/x4.png"
elif [[ -x "$HERE/upscale" ]]; then   # the DMG kit (no Real-ESRGAN on a player's Mac): MetalFX 4x
  mkdir -p "$T/in" "$T/up"; cp "$T/title.png" "$T/in/title.png"
  "$HERE/upscale" "$T/in" "$T/up" 4 >/dev/null 2>&1 && [[ -f "$T/up/title.png" ]] && big="$T/up/title.png"
fi
if [[ -x "$HERE/squircle" ]]; then "$HERE/squircle" "$big" "$T/sq.png"; else swift "$HERE/squircle.swift" "$big" "$T/sq.png"; fi
big="$T/sq.png"
mkdir -p "$T/i.iconset"
for s in 16 32 128 256 512; do
  sips -z $s $s "$big" --out "$T/i.iconset/icon_${s}x${s}.png" >/dev/null
  sips -z $((s * 2)) $((s * 2)) "$big" --out "$T/i.iconset/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns -o "$OUT" "$T/i.iconset"
echo "icon $(basename "$big" .png) -> $OUT"
