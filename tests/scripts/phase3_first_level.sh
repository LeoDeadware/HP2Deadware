#!/bin/bash
# Phase-3 milestone (Render): New Game -> the first playable level (the
# Burrow) renders correctly, driven by a scripted controller, paced at 60 Hz.
#
#   tests/scripts/phase3_first_level.sh [build_dir]      (default: out)
#   UPDATE_GOLDEN=1 tests/scripts/phase3_first_level.sh  (re-take tests/golden/phase3_*.png)
#
# Two silent background runs (~110 s + ~100 s; log + PNGs in .cache/HP2Cache/runs/phase3_level-*).
# Input: tests/scripts/phase3_new_game.pad (HP2_PAD_SCRIPT of the XInput HLE, frame-locked).
# Movies are skipped (HP2_SKIP_MOVIES=1; phase2_main_menu.sh covers them):
# that keeps the run short and does not change the frame numbering.
# Checks (all frames are deterministic run to run):
#   frame  600  save-slot screen          == golden phase3_slots.png
#   frame 4200  intro cutscene (Burrow)   == golden phase3_cutscene.png
#   frame 6000  gameplay starts           == golden phase3_gameplay.png
#   frame 6320  after walking and turning == golden phase3_walk.png
#   frames 600, 6000 again in 16:10 (slots == golden phase3_wide_slots.png:
#               "(A) Select" legends whole, one anchor per string), and 6000 (HP2_WIDESCREEN=1, second run, wide HUD on by
#               default)                 == golden phase3_wide_hud.png: the
#               button cluster anchored to the right edge (not the 4:3 area's);
#   frames 6000/6320 (and their goldens) without the "reconnect the controller"
#   overlay (overlay_check.py), and 6320 != 6000 (the walk moved Harry);
#   Swap pacing: no 120-frame window faster than 60 Hz, run-wide average 16.67 ms
#   +-0.35; single over-budget windows (loads, other work on the host) are reported
#   with the host's busy time, not failed;
#   exit 0, no FATAL.
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

RUN="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/phase3_level-$(date +%Y%m%d-%H%M%S)-$$"
# launch <dir> <exit frame> <shot frames> [env...]: one silent paced run -> CODE
launch() {
  local dir=$1 exit_at=$2 shots=$3
  shift 3
  mkdir -p "$dir/home"
  (
    ulimit -c 0
    export HOME="$dir/home" HP2_LOG_DIR="$dir" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy \
           SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 \
           HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}" \
           HP2_SKIP_MOVIES=1 HP2_PAD_SCRIPT="$ROOT/tests/scripts/phase3_new_game.pad" \
           HP2_SHOT_FRAMES=$shots HP2_EXIT_AFTER=$exit_at
    unset HP2_STUB HP2_NO_PACING HP2_WIDESCREEN HP2_WIDE_HUD HP2_RENDER_SCALE
    exec env "$@" "$BIN" > "$dir/stdout.log" 2> "$dir/stderr.log"
  ) &
  local pid=$!
  for _ in $(seq 1 400); do
    kill -0 $pid 2>/dev/null || break
    sleep 0.5
  done
  if kill -0 $pid 2>/dev/null; then
    kill -9 $pid
    fail "timeout after 200 s ($dir)"
  fi
  wait $pid
  CODE=$?
}
launch "$RUN" 6320 600,4200,6000,6320
LOG="$RUN/runtime.log"
echo "exit $CODE, log $LOG"
[[ $CODE == 0 ]] || fail "exit code $CODE"
grep -m1 "^FATAL" "$LOG" && fail "fatal error"

check_shot() {
  local png=$1 gold="$GOLDEN/$2"
  if [[ ! -f "$png" ]]; then
    fail "no screenshot $png"
    return
  fi
  if [[ "${UPDATE_GOLDEN:-0}" == 1 || ! -f "$gold" ]]; then
    cp "$png" "$gold"
    echo "  golden updated: $gold"
    return
  fi
  python3 "$ROOT/tests/scripts/png_diff.py" "$png" "$gold" || fail "$png differs from $gold"
}

echo "frames:"
check_shot "$RUN/frame_000600.png" phase3_slots.png
check_shot "$RUN/frame_004200.png" phase3_cutscene.png
check_shot "$RUN/frame_006000.png" phase3_gameplay.png
check_shot "$RUN/frame_006320.png" phase3_walk.png

echo "16:10 HUD (second run):"
launch "$RUN/wide" 6000 600,6000 HP2_WIDESCREEN=1
[[ $CODE == 0 ]] || fail "widescreen run: exit code $CODE"
check_shot "$RUN/wide/frame_000600.png" phase3_wide_slots.png
check_shot "$RUN/wide/frame_006000.png" phase3_wide_hud.png

echo "input reached the game:"
# The game shows "PLEASE RECONNECT THE CONTROLLER TO PORT 1" when port 0 reads
# disconnected; goldens taken in that state once made this test pass while
# Harry never moved. So: no overlay, and the walk must change the picture.
for f in 006000 006320; do
  [[ -f "$RUN/frame_$f.png" ]] || continue
  python3 "$ROOT/tests/scripts/overlay_check.py" "$RUN/frame_$f.png" ||
    fail "frame $f shows the reconnect-controller overlay"
done
if [[ -f "$RUN/frame_006000.png" && -f "$RUN/frame_006320.png" ]]; then
  if python3 "$ROOT/tests/scripts/png_diff.py" "$RUN/frame_006000.png" "$RUN/frame_006320.png" > /dev/null; then
    fail "frame 6320 (after walking) looks like frame 6000: the scripted walk did nothing"
  else
    echo "  walk frame differs from the gameplay frame"
  fi
fi
for g in phase3_gameplay phase3_walk; do
  python3 "$ROOT/tests/scripts/overlay_check.py" "$GOLDEN/$g.png" > /dev/null ||
    fail "golden $g.png shows the reconnect overlay (re-take it once input works)"
done

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
