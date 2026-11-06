#!/bin/bash
# Phase-3 visual audit (Render): a seeded random walk through the Burrow
# (tests/scripts/phase3_burrow_walk.pad), ~3 min, silent background run.
# Covers what the first-minute test does not: spells (Flipendo on the glass
# jar), the Magical Measurer, dialogue, and the Remembrall tutorial popup,
# whose translucent panel needs untextured fixed-function stages to read
# white (it was invisible before). Checks:
#   frame 9800 == tests/golden/phase3_burrow_panel.png (deterministic run);
#   the panel darkens the text area (mean luma inside the panel box well below
#   the scene's: a missing panel fails this even if the golden is re-taken);
#   exit 0, no FATAL.
# The route was recorded with the game's original Camera Control "Reverse"
# (the right-stick turn at 8545 steers the camera-relative walk after it); the
# port defaults to "Normal" and to the modern orbit camera (330 deg/s), so the
# run pins the original camera and option explicitly.
#
#   tests/scripts/phase3_burrow_walk.sh [build_dir]   (UPDATE_GOLDEN=1 to re-take)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
GOLDEN="$ROOT/tests/golden"
FAILS=0
fail() {
  echo "  FAIL: $*"
  FAILS=$((FAILS + 1))
}

RUN="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/phase3_burrow-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN/home"
(
  ulimit -c 0
  export HOME="$RUN/home" HP2_LOG_DIR="$RUN" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy \
         SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 \
         HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}" \
         HP2_SKIP_MOVIES=1 HP2_PAD_SCRIPT="$ROOT/tests/scripts/phase3_burrow_walk.pad" \
         HP2_INPUT_CAMERA_DEFAULT_OPTION=reverse HP2_INPUT_CAMERA_MODERN=0 \
         HP2_SHOT_FRAMES=9800 HP2_EXIT_AFTER=9800
  unset HP2_STUB HP2_NO_PACING
  exec "$BIN" > "$RUN/stdout.log" 2> "$RUN/stderr.log"
) &
PID=$!
for _ in $(seq 1 600); do
  kill -0 $PID 2>/dev/null || break
  sleep 0.5
done
if kill -0 $PID 2>/dev/null; then
  kill -9 $PID
  fail "timeout after 300 s"
fi
wait $PID
CODE=$?
echo "exit $CODE, log $RUN/runtime.log"
[[ $CODE == 0 ]] || fail "exit code $CODE"
grep -m1 "^FATAL" "$RUN/runtime.log" && fail "fatal error"

PNG="$RUN/frame_009800.png"
if [[ ! -f "$PNG" ]]; then
  fail "no screenshot $PNG"
else
  # The panel box spans about x 110..528, y 158..320; compare its luma with the
  # area right below it (ground, same lighting).
  python3 -c "
import sys; sys.path.insert(0, '$ROOT/tests/scripts'); from png_diff import read_png
w, h, p = read_png('$PNG')
def luma(x0, y0, x1, y1):
    s = n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            i = (y * w + x) * 3
            s += 0.3 * p[i] + 0.59 * p[i + 1] + 0.11 * p[i + 2]; n += 1
    return s / n
# panel border strips without text (left/right margins inside the box)
inside = (luma(115, 165, 145, 315) + luma(495, 165, 525, 315)) / 2
outside = (luma(60, 165, 100, 315) + luma(540, 165, 580, 315)) / 2
print(f'  panel margin luma {inside:.1f} vs scene beside it {outside:.1f}')
sys.exit(0 if inside < outside * 0.8 else 1)
" || fail "tutorial panel missing (text area not darkened)"
  if [[ "${UPDATE_GOLDEN:-0}" == 1 || ! -f "$GOLDEN/phase3_burrow_panel.png" ]]; then
    cp "$PNG" "$GOLDEN/phase3_burrow_panel.png"
    echo "  golden updated: $GOLDEN/phase3_burrow_panel.png"
  else
    python3 "$ROOT/tests/scripts/png_diff.py" "$PNG" "$GOLDEN/phase3_burrow_panel.png" ||
      fail "$PNG differs from the golden"
  fi
fi

if [[ $FAILS == 0 ]]; then
  echo "PASS"
  exit 0
fi
echo "FAIL ($FAILS)"
exit 1
