#!/bin/bash
# Builds and runs the ADPCM decoder test against ffmpeg on warner.xmv's audio.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/hp2_adpcm_test"
mkdir -p "$OUT"
XMV="${HP2_DATA:-$("$(dirname "$0")/../../../../tools/main_checkout.sh")}/game/extracted/warner.xmv"
FF="$(command -v ffmpeg || echo /opt/homebrew/bin/ffmpeg)"
"$FF" -v error -y -i "$XMV" -map 0:a:0 -c copy -f data "$OUT/packets.bin"
"$FF" -v error -y -i "$XMV" -vn -f s16le "$OUT/ref.s16le"
c++ -std=c++20 -O1 -I"$SRC" "$HERE/adpcm_test.cpp" "$HERE/../adpcm.cpp" -o "$OUT/adpcm_test"
"$OUT/adpcm_test" "$OUT/packets.bin" "$OUT/ref.s16le" 2
