#!/bin/bash
# Builds the HD UI texture pack from scratch (regenerable, never committed):
#   tools/hdui/make_pack.sh [build_dir] [out_dir]
# 1. the Ukrainian overlay (tools/loc/build_mod.py uk) into .cache/HP2Cache/hdui/uk,
# 2. two silent texture dumps (HP2_HDUI_DUMP) over tools/loc/test_page.pad
#    (title, slots, level 1 HUD, pause, inventory): with the overlay (Cyrillic
#    font atlases) and without (the disc's atlases),
# 3. tools/hdui/build_pack.py -> out_dir (default .cache/HP2Cache/hdui/pack; for
#    the player: $HP2_DATA/mods/hd_ui, which the game loads by default).
# Needs Real-ESRGAN in .cache/HP2Cache/tools/realesrgan/x (see build_pack.py).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/out}/src/loader/hp2"
OUT="${2:-${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/hdui/pack}"
DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}"
UK="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/hdui/uk"
RUN="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/hdui-make-$(date +%Y%m%d-%H%M%S)-$$"
python3 "$ROOT/tools/loc/build_mod.py" uk --out "$UK" > /dev/null
dump() {
  local d="$RUN/$1"
  mkdir -p "$d/home"
  (
    ulimit -c 0
    export HOME="$d/home" HP2_LOG_DIR="$d" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy \
           HP2_BACKGROUND=1 HP2_DATA="$DATA" HP2_MODS="$2" HP2_SKIP_MOVIES=1 HP2_HDUI=0 \
           HP2_PAD_SCRIPT="$ROOT/tools/loc/test_page.pad" HP2_LANG="$3" HP2_HDUI_DUMP="$d/dump" \
           HP2_EXIT_AFTER=6960
    exec "$BIN" > "$d/stdout.log" 2>&1
  )
}
dump uk "$UK" uk & dump disc "/nonexistent" en & wait
python3 "$ROOT/tools/hdui/build_pack.py" "$RUN/uk/dump" "$RUN/disc/dump" --out "$OUT"
