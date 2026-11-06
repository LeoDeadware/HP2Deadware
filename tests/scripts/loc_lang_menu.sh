#!/bin/bash
# Language picker with only English + Ukrainian (Localizer): Options -> first
# row -> flags. Until the Kernel zeroes the picker masks itself (STATUS.md
# request), HP2_CALL_AT pokes them: the 6-row table at 0x20F348 {lang index,
# mask, flag sprite}; rows with mask & 0x3F == 0 are not listed. Rows 2..5 =
# FRA GER SPA ITA -> 0 (masks at 0x20F364/70/7C/88). Ukrainian overlay on.
#   tests/scripts/loc_lang_menu.sh [build_dir] [--all]   (--all: no pokes, all 6 flags)
# Shots 560 (options), 680 (picker), 740/820 (after RIGHT/DOWN).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/out}/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}"
RUN="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/loc_lang_menu-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN/home"
ulimit -c 0
python3 "$ROOT/tools/loc/build_mod.py" uk --data "$DATA" --out "$RUN/overlay" > "$RUN/build.log" 2>&1 \
  || { echo "FAIL: overlay build"; exit 1; }
POKES="1:poke:20F364:0,1:poke:20F370:0,1:poke:20F37C:0,1:poke:20F388:0"
[[ "${2:-}" == "--all" ]] && POKES=""
HOME="$RUN/home" HP2_LOG_DIR="$RUN" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy \
  HP2_BACKGROUND=1 HP2_DATA="$DATA" HP2_SKIP_MOVIES=1 HP2_PAD_SCRIPT="$ROOT/tools/loc/lang_menu.pad" \
  HP2_LANG=uk HP2_MODS="$RUN/overlay" HP2_CALL_AT="$POKES" HP2_SHOT_FRAMES=560,680,740,820 HP2_EXIT_AFTER=820 \
  "$BIN" > "$RUN/stdout.log" 2> "$RUN/stderr.log"
code=$?
shots=$(ls "$RUN"/frame_*.png 2>/dev/null | wc -l | tr -d ' ')
echo "exit $code, $shots/4 screenshots in $RUN"
[[ $code == 0 && $shots == 4 ]] && echo PASS || { echo FAIL; exit 1; }
