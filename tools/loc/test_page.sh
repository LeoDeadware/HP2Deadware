#!/bin/bash
# Localization test page: builds the Ukrainian overlay in alphabet-test mode
# (every string -> Ukrainian letters) plus the explicit full-alphabet strings
# of tools/loc/test_page/<n>/, runs the game silently through
# tools/loc/test_page.pad and keeps screenshots of every font context:
#   400  title screen   title font (menu items), panel font 0 (copyright)
#   600  save slots     title font (heading, Select/Exit), panel font 0 (items)
#   4200 cutscene       panel font 0 (subtitle: whole alphabet)
#   6460 pause menu     panel fonts
#   6660 inventory      inventory title font, panel fonts 0 and 1
#
#   tools/loc/test_page.sh [build_dir [page...]]     (default: out, every page)
# Output: .cache/HP2Cache/runs/loc_test_page-<stamp>/page<n>/frame_*.png (look at them).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/out}/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "no $BIN"; exit 2; }
DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}"
OUT="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/loc_test_page-$(date +%Y%m%d-%H%M%S)-$$"
ulimit -c 0
PAGES=("${@:2}")
[[ ${#PAGES[@]} -gt 0 ]] || PAGES=($(ls "$ROOT/tools/loc/test_page"))
FAIL=0
for n in "${PAGES[@]}"; do
  page="$ROOT/tools/loc/test_page/$n/"
  run="$OUT/page$n"
  mkdir -p "$run/home"
  python3 "$ROOT/tools/loc/build_mod.py" uk --data "$DATA" --out "$run/overlay" --test-alphabet 1 \
    --text "$page" > "$run/build.log" 2>&1 || { echo "page $n: build failed ($run/build.log)"; exit 1; }
  HOME="$run/home" HP2_LOG_DIR="$run" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy \
    HP2_BACKGROUND=1 HP2_DATA="$DATA" HP2_SKIP_MOVIES=1 HP2_PAD_SCRIPT="$ROOT/tools/loc/test_page.pad" \
    HP2_LANG=uk HP2_MODS="$run/overlay" HP2_SHOT_FRAMES=400,600,4200,6460,6660 HP2_EXIT_AFTER=6660 \
    "$BIN" > "$run/stdout.log" 2> "$run/stderr.log"
  code=$?
  shots=$(ls "$run"/frame_*.png 2>/dev/null | wc -l | tr -d ' ')
  echo "page $n: exit $code, $shots screenshots in $run"
  grep -m1 "^FATAL" "$run/runtime.log" 2>/dev/null && FAIL=1
  [[ $code == 0 && $shots == 5 ]] || FAIL=1
done
echo "$OUT"
exit $FAIL
