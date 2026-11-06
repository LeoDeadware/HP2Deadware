#!/bin/bash
# Phase-2 milestone (Render + Audio): the real boot, paced at 60 Hz, no stubs:
# legal screen, the three intro movies (XMV decoder + DSound streams + D3D
# UpdateOverlay), main menu. Swap must hold 60.000 Hz.
#
#   tests/scripts/phase2_main_menu.sh [build_dir]      (default: out)
#   UPDATE_GOLDEN=1 tests/scripts/phase2_main_menu.sh  (re-take tests/golden/phase2_*.png)
#
# One silent background run (dummy audio, HP2_BACKGROUND=1, scratch HOME, log
# + PNGs in .cache/HP2Cache/runs/phase2_boot-*), ~100 s. Checks:
#   - legal screen, frame 240 == tests/golden/phase2_legal.png;
#   - ea_logo_english, warner, hpengx .xmv played, >= 4 movie (overlay)
#     screenshots (one per 400 overlay frames); overlay frame 1200 (intro,
#     Quidditch pitch) == tests/golden/phase2_movie.png (decoding is
#     deterministic: same frames run to run);
#   - main menu, frame 600 == tests/golden/phase2_main_menu.png in its static
#     part (y 280..480: menu items, copyright; the clouds above drift);
#   - Swap pacing: no 120-frame window faster than 60 Hz, run-wide average
#     16.67 ms +-0.35 (movies excluded: they show frames without Swap; single
#     over-budget windows are host hitches, reported with the host's busy time);
#   - exit 0 (HP2_EXIT_AFTER=600), no FATAL.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN (cmake --build $BUILD --target hp2)"; exit 2; }
GOLDEN="$ROOT/tests/golden"
FAILS=0
fail() {
  echo "  FAIL: $*"
  FAILS=$((FAILS + 1))
}

RUN="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/phase2_boot-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN/home"
(
  ulimit -c 0
  export HOME="$RUN/home" HP2_LOG_DIR="$RUN" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy \
         SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 \
         HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}" \
         HP2_SHOT_FRAMES=240,600 HP2_SHOT_EVERY=400 HP2_EXIT_AFTER=600
  unset HP2_STUB HP2_SKIP_MOVIES HP2_NO_PACING
  exec "$BIN" > "$RUN/stdout.log" 2> "$RUN/stderr.log"
) &
PID=$!
for _ in $(seq 1 400); do
  kill -0 $PID 2>/dev/null || break
  sleep 0.5
done
if kill -0 $PID 2>/dev/null; then
  kill -9 $PID
  fail "timeout after 200 s"
fi
wait $PID
CODE=$?
LOG="$RUN/runtime.log"
echo "exit $CODE, log $LOG"
[[ $CODE == 0 ]] || fail "exit code $CODE"
grep -m1 "^FATAL" "$LOG" && fail "fatal error"

# check_shot <png> <golden name> [rect x0,y0,x1,y1]
check_shot() {
  local png=$1 gold="$GOLDEN/$2" rect=${3:-}
  if [[ ! -f "$png" ]]; then
    fail "no screenshot $png"
    return
  fi
  if [[ "${UPDATE_GOLDEN:-0}" == 1 || ! -f "$gold" ]]; then
    cp "$png" "$gold"
    echo "  golden updated: $gold"
    return
  fi
  PNG_DIFF_RECT="$rect" python3 "$ROOT/tests/scripts/png_diff.py" "$png" "$gold" ||
    fail "$png differs from $gold"
}

echo "legal screen (frame 240):"
check_shot "$RUN/frame_000240.png" phase2_legal.png

echo "movies:"
for m in ea_logo_english warner hpengx; do
  grep -q "movies: playing D:\\\\$m.xmv" "$LOG" || fail "$m.xmv not played"
done
NOV=$(ls "$RUN"/overlay_*.png 2>/dev/null | wc -l | tr -d ' ')
echo "  $NOV movie screenshots"
[[ $NOV -ge 4 ]] || fail "too few movie frames"
grep "movie in between" "$LOG" | sed 's/^/  /'
check_shot "$RUN/overlay_001200.png" phase2_movie.png

echo "main menu (frame 600):"
check_shot "$RUN/frame_000600.png" phase2_main_menu.png 0,280,640,480

# Pacing rule: no window may be faster than 60 Hz (the pacer caps every frame);
# the run-wide average must be 16.67 ms +-0.35. Single windows over budget are
# host hitches (other builds/runs on this Mac, first-use loads): reported, not
# failed, unless the whole run is slow while the host had headroom (busy).
pacing_check() {
  local win=$1
  [[ -n "$win" ]] || { fail "no pacing statistics"; return; }
  echo "$win" | awk '
    { n++; s += $2; b += $4; if ($4 > bm) bm = $4
      if ($2 < 16.32) { fast++; printf "  frame %s: %s ms avg (faster than 60 Hz)\n", $1, $2 }
      else if ($2 > 17.02) { slow++; printf "  hitch window frame %s: %s ms avg, %s ms max, busy %s ms\n", $1, $2, $3, $4 } }
    END { printf "  %d windows: %.2f ms avg, busy %.2f ms avg (worst window %.2f); %d over budget\n", n, s / n, b / n, bm, slow
          if (fast) exit 1
          if (s / n > 17.02 && b / n < 15.5) exit 2
          if (s / n > 17.02) exit 3 }'
  case $? in
    0) ;;
    1) fail "Swap faster than 60 Hz: the pacer is not capping" ;;
    2) fail "Swap not paced at 60 Hz on average although the host had headroom" ;;
    3) echo "  WARN: run-wide average over budget with the host overloaded (busy >= 15.5 ms)" ;;
  esac
}

echo "Swap pacing (120-frame windows after the first):"
pacing_check "$(sed -n 's/^d3d8: frame \([0-9]*\): \([0-9.]*\) ms avg, \([0-9.]*\) ms max over 120 frames (busy \([0-9.]*\) ms).*/\1 \2 \3 \4/p' "$LOG" | tail -n +2)"
grep "slow frame" "$LOG" | sed 's/^/  /'

if [[ $FAILS == 0 ]]; then
  echo "PASS"
  exit 0
fi
echo "FAIL ($FAILS)"
exit 1
