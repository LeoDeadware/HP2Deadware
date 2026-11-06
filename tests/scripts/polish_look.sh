#!/bin/bash
# Polish: the «TV 2002» post-processing looks (HP2_LOOK, src/polish/postfx).
#
#   tests/scripts/polish_look.sh [build_dir]      (default: out)
#   UPDATE_GOLDEN=1 tests/scripts/polish_look.sh  (re-take tests/golden/look_*.png)
#
# Silent background runs of New Game -> the first level (phase3_new_game.pad,
# movies skipped), one per look, at most 2 at once. The window present is
# checked through HP2_DEV_PRESENT_TEST (an unattached layer; present_NNNNNN.png
# = what the window would show) at gameplay frame 6000:
#   modern, authentic  at 640x480: the present == the same run's back buffer
#                      (frame_006000.png) pixel for pixel (modern bypasses the
#                      chain; authentic runs it with every strength 0: the chain
#                      itself is neutral), and == golden phase3_gameplay.png
#                      (png_diff tolerance, as phase3_first_level.sh: the game's
#                      frame differs by a few pixels run to run);
#   authentic_hd, crt  at 1200x900 (2.5 px per scanline: non-integer scale):
#                      look_check.py against modern (mean luma +-25%, no black /
#                      saturated excess = no NaN, the picture changed);
#   all four frames    == tests/golden/look_<name>.png (png_diff tolerance; the
#                      grain is animated by frame number, so runs are repeatable).
# Cost at 2880x1800 (fit 2400x1800): HP2_LOOK_BENCH=1 times every look's chain
# x200 in one command buffer (sustained GPU clock), in a run of its own (no
# other run of this script on the GPU): FAIL if a look adds more than 1 ms per
# frame over the plain present it replaces (authentic's time: the same scaled
# copy), so other sessions' GPU load does not fail the test; totals reported.
# The in-game timestamps of two short runs (crt, authentic_hd; 1300 frames) are
# reported too: a light game frame leaves the GPU at a low clock, so those read
# higher than the bench (not failed).
# Exit 0, no FATAL in every run.
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

STAMP=$(date +%Y%m%d-%H%M%S)-$$
# run <look> <WxH> <frames> -> ${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/polish_look-<stamp>/<look>-<WxH>
run() {
  local look=$1 size=$2 frames=$3
  local dir="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/polish_look-$STAMP/$look-$size"
  mkdir -p "$dir/home"
  (
    ulimit -c 0
    export HOME="$dir/home" HP2_LOG_DIR="$dir" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy \
           SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_DEV_PRESENT_TEST="$size" HP2_LOOK="$look" \
           HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}" \
           HP2_SKIP_MOVIES=1 HP2_PAD_SCRIPT="$ROOT/tests/scripts/phase3_new_game.pad" \
           HP2_SHOT_FRAMES="$frames" HP2_EXIT_AFTER="$frames"
    unset HP2_STUB HP2_NO_PACING HP2_LOOK_BENCH HP2_LOOK_GRADE HP2_LOOK_BLOOM HP2_LOOK_GRAIN HP2_LOOK_VIGNETTE \
          HP2_LOOK_CRT HP2_WIDESCREEN HP2_FILTER HP2_RENDER_SCALE
    [[ $look == bench ]] && export HP2_LOOK=crt HP2_LOOK_BENCH=1
    exec "$BIN" > "$dir/stdout.log" 2> "$dir/stderr.log"
  ) &
  local pid=$!
  for _ in $(seq 1 480); do
    kill -0 $pid 2>/dev/null || break
    sleep 0.5
  done
  kill -0 $pid 2>/dev/null && { kill -9 $pid; echo "timeout after 240 s" > "$dir/timeout"; }
  wait $pid
  echo $? > "$dir/exit"
}

check_run() {
  local dir="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/polish_look-$STAMP/$1"
  local code
  code=$(cat "$dir/exit" 2>/dev/null || echo none)
  echo "$1: exit $code, log $dir/runtime.log"
  [[ $code == 0 ]] || fail "$1: exit code $code"
  [[ -f "$dir/timeout" ]] && fail "$1: timeout"
  grep -m1 "^FATAL" "$dir/runtime.log" && fail "$1: fatal error"
  grep -m1 "polish: look " "$dir/runtime.log" | sed 's/^/  /'
}

R="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/polish_look-$STAMP"
echo "runs in $R"
run modern 640x480 6000 & run authentic 640x480 6000 & wait
run authentic_hd 1200x900 6000 & run crt 1200x900 6000 & wait
run crt 2880x1800 1300 & run authentic_hd 2880x1800 1300 & wait
run bench 2880x1800 60

for r in modern-640x480 authentic-640x480 authentic_hd-1200x900 crt-1200x900 crt-2880x1800 \
         authentic_hd-2880x1800 bench-2880x1800; do
  check_run "$r"
done

shot() { echo "$R/$1/present_006000.png"; }
gold() {
  local png=$1 g="$GOLDEN/look_$2.png"
  [[ -f "$png" ]] || { fail "no screenshot $png"; return; }
  if [[ "${UPDATE_GOLDEN:-0}" == 1 || ! -f "$g" ]]; then
    cp "$png" "$g"
    echo "  golden updated: $g"
    return
  fi
  python3 "$ROOT/tests/scripts/png_diff.py" "$png" "$g" || fail "$png differs from $g"
}

echo "plain present (modern, authentic) == back buffer pixel for pixel, == phase3_gameplay.png:"
for l in modern authentic; do
  p=$(shot "$l-640x480")
  [[ -f "$p" ]] || { fail "no screenshot $p"; continue; }
  python3 "$ROOT/tests/scripts/png_diff.py" "$p" "$R/$l-640x480/frame_006000.png" 0 0 ||
    fail "$l present is not pixel-identical to the back buffer"
  python3 "$ROOT/tests/scripts/png_diff.py" "$p" "$GOLDEN/phase3_gameplay.png" ||
    fail "$l present differs from phase3_gameplay.png"
  gold "$p" "$l"
done

echo "looks vs modern:"
for l in authentic_hd crt; do
  p=$(shot "$l-1200x900")
  [[ -f "$p" ]] || { fail "no screenshot $p"; continue; }
  echo " $l:"
  python3 "$ROOT/tests/scripts/look_check.py" "$p" "$(shot modern-640x480)" || fail "$l frame check"
  gold "$p" "$l"
done

echo "cost at 2880x1800 (fit 2400x1800):"
bench_ms() {
  grep "polish: bench $1 at 2880x1800" "$R/bench-2880x1800/runtime.log" | tail -1 |
    sed 's/.* \([0-9.]*\) ms per frame.*/\1/'
}
base=$(bench_ms authentic)
[[ -n "$base" ]] || fail "no bench line for authentic (the plain-present baseline)"
echo "  authentic (the chain at strength 0 = the plain present's cost): $base ms"
for l in authentic_hd crt; do
  ms=$(bench_ms $l)
  [[ -n "$ms" && -n "$base" ]] || { fail "$l: no bench line"; continue; }
  net=$(awk -v m="$ms" -v b="$base" 'BEGIN { printf "%.3f", m - b }')
  echo "  $l: $ms ms total, $net ms over the plain present"
  awk -v m="$net" 'BEGIN { exit !(m <= 1.0) }' || fail "$l adds $net ms (> 1 ms)"
done
for l in crt authentic_hd; do
  line=$(grep "polish: look $l at 2880x1800: GPU" "$R/$l-2880x1800/runtime.log" | tail -1)
  [[ -n "$line" ]] && echo "  in game (low GPU clock): ${line#*polish: }" || fail "$l: no in-game GPU timing"
done

if [[ $FAILS == 0 ]]; then
  echo "PASS"
  exit 0
fi
echo "FAIL ($FAILS)"
exit 1
