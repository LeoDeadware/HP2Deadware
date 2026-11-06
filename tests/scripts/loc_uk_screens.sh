#!/bin/bash
# Ukrainian translation on screen (Localizer): builds the real overlay from
# mods/loc/uk (tools/loc/build_mod.py), runs the game silently with
# HP2_LANG=uk through a pad script and keeps the screenshots:
#   tools/loc/test_page.pad (default): 400 title, 600 save slots,
#   4200 Burrow cutscene subtitle, 6460 pause menu, 6660 inventory.
#
#   tests/scripts/loc_uk_screens.sh [build_dir] [pad] [frames]
# Output: .cache/HP2Cache/runs/loc_uk_screens-<stamp>/frame_*.png (look at them:
# overflow, missing letters). PASS = exit 0, no FATAL, every frame written.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/out}/src/loader/hp2"
PAD="${2:-$ROOT/tools/loc/test_page.pad}"
FRAMES="${3:-400,600,4200,6460,6660}"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}"
RUN="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/loc_uk_screens-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN/home"
ulimit -c 0
python3 "$ROOT/tools/loc/build_mod.py" uk --data "$DATA" --out "$RUN/overlay" > "$RUN/build.log" 2>&1 \
  || { echo "FAIL: overlay build ($RUN/build.log)"; exit 1; }
LAST="${FRAMES##*,}"
HOME="$RUN/home" HP2_LOG_DIR="$RUN" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy \
  HP2_BACKGROUND=1 HP2_DATA="$DATA" HP2_SKIP_MOVIES=1 HP2_PAD_SCRIPT="$PAD" \
  HP2_LANG=uk HP2_MODS="$RUN/overlay" HP2_SHOT_FRAMES="$FRAMES" HP2_EXIT_AFTER="$LAST" \
  "$BIN" > "$RUN/stdout.log" 2> "$RUN/stderr.log"
code=$?
want=$(echo "$FRAMES" | tr ',' '\n' | wc -l | tr -d ' ')
shots=$(ls "$RUN"/frame_*.png 2>/dev/null | wc -l | tr -d ' ')
echo "exit $code, $shots/$want screenshots in $RUN"
if grep -q "^FATAL" "$RUN/runtime.log" 2>/dev/null || [[ $code != 0 || $shots != "$want" ]]; then
  echo FAIL; exit 1
fi
echo PASS
