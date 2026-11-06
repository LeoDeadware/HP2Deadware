#!/bin/bash
# Phase-3 milestone: scripted controller input (HP2_PAD_SCRIPT, XInput HLE)
# starts a new game from the main menu and the first level is drawn.
#
#   tests/scripts/phase3_new_game.sh [build_dir]      (default: out)
#   UPDATE_GOLDEN=1 tests/scripts/phase3_new_game.sh  (re-take tests/golden/phase3_*.png)
#
# One silent background run (dummy video/audio, HP2_BACKGROUND=1, scratch HOME,
# logs + PNGs under .cache/HP2Cache/runs/phase3_new_game-*), movies skipped, paced:
#   frame 640  A          main menu: New Game
#   frame 760+ DOWN x3, A save slot: Continue Without Saving
#   frame 900  A          "unable to save ... continue?": Yes
# Checks: every script line was applied (xinput log); the confirm dialog
# (frame 880) and the loading screen (frame 1000, without the progress bar)
# match tests/golden/phase3_*.png; the run reaches frame $LEVEL_FRAME and its
# screenshot matches tests/golden/phase3_first_level.png. A missing golden is
# a failure unless UPDATE_GOLDEN=1 (look at the PNG before you take it).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN (cmake --build $BUILD --target hp2)"; exit 2; }
GOLDEN="$ROOT/tests/golden"
LEVEL_FRAME=${LEVEL_FRAME:-1800}
RUN="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/phase3_new_game-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN/home"
FAILS=0

cat > "$RUN/pad.txt" <<'EOF'
# main menu (New Game is highlighted)
640 press A
# save slots: Potter A/B/C, Continue Without Saving
760 press DOWN
780 press DOWN
800 press DOWN
830 press A
# "You will be unable to save ... continue?" -> Yes
900 press A
EOF

echo "1. scripted new game, $LEVEL_FRAME frames"
(
  ulimit -c 0
  export HOME="$RUN/home" HP2_LOG_DIR="$RUN" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy \
         SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_SKIP_MOVIES=1 \
         HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}" \
         HP2_PAD_SCRIPT="$RUN/pad.txt" HP2_SHOT_FRAMES="880,1000,$LEVEL_FRAME" \
         HP2_EXIT_AFTER="$LEVEL_FRAME"
  exec "$BIN" > "$RUN/stdout.log" 2> "$RUN/stderr.log"
) &
PID=$!
for _ in $(seq 1 360); do
  kill -0 $PID 2>/dev/null || break
  sleep 0.5
done
if kill -0 $PID 2>/dev/null; then
  kill -9 $PID
  echo "  timeout after 180s"
fi
wait $PID
CODE=$?
echo "  exit $CODE, log $RUN/runtime.log"
[[ $CODE == 0 ]] || { echo "  FAIL: exit code $CODE"; grep -m1 -A3 FATAL "$RUN/runtime.log"; FAILS=$((FAILS + 1)); }

NLINES=$(grep -cvE '^\s*(#|$)' "$RUN/pad.txt")
NAPPLIED=$(grep -c '\[krnl\] xinput: frame ' "$RUN/stderr.log")
echo "  pad script: $NAPPLIED of $NLINES lines applied"
[[ $NAPPLIED == "$NLINES" ]] || { echo "  FAIL: pad script not fully applied"; FAILS=$((FAILS + 1)); }

# check_shot <png> <golden name> [rect x0,y0,x1,y1]
check_shot() {
  local png=$1 gold="$GOLDEN/$2" rect=${3:-}
  if [[ ! -f "$png" ]]; then
    echo "  FAIL: no screenshot $png"
    FAILS=$((FAILS + 1))
  elif [[ "${UPDATE_GOLDEN:-0}" == 1 ]]; then
    cp "$png" "$gold"
    echo "  golden updated: $gold"
  elif [[ ! -f "$gold" ]]; then
    echo "  FAIL: no golden $gold (check $png, then UPDATE_GOLDEN=1)"
    FAILS=$((FAILS + 1))
  elif ! PNG_DIFF_RECT="$rect" python3 "$ROOT/tests/scripts/png_diff.py" "$png" "$gold"; then
    echo "  FAIL: $png differs from $gold"
    FAILS=$((FAILS + 1))
  fi
}
printf -v LF '%06d' "$LEVEL_FRAME"
check_shot "$RUN/frame_000880.png" phase3_confirm.png 0,140,640,300
check_shot "$RUN/frame_001000.png" phase3_loading.png 0,0,640,310
check_shot "$RUN/frame_$LF.png" phase3_first_level.png

if [[ $FAILS == 0 ]]; then
  echo "PASS"
  exit 0
fi
echo "FAIL ($FAILS)"
exit 1
