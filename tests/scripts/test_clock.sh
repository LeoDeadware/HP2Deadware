#!/bin/bash
# HP2_TEST_CLOCK=frames (opt-in; kernel time.cpp, Audio's mixer per vblank):
# two runs of the phase-3 path started at the same time (so each loads the
# other) must render identical frames. The test clock moves kernel time only
# at vblanks, one vblank per Swap, and the Swap barrier lets every woken guest
# thread finish within its frame, so host speed cannot shift game events.
# The other tests stay on the real paced clock (what players run).
#
#   tests/scripts/test_clock.sh [build_dir]      (default: out)
#
# Checks: both runs exit 0; frames 600 (save slots), 2000, 4200 (cutscene),
# 6000 (gameplay), 6320 (after the scripted walk) match between the two runs:
# png_diff mean <= 0.05 and at most 0.05% of pixels off by more than 24
# (measured: a few dozen of 307200 pixels differ, mostly by <= 20 levels -
# effects seeded from rdtsc, which keeps the in-frame host time so spin loops
# end; a real divergence is mean 1..5 with percents of pixels off); the Swap
# barrier never timed
# out ("still busy" in the log means a thread stayed busy 250 ms: frames may
# then differ). Logs under .cache/HP2Cache/runs/test_clock-*.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN (cmake --build $BUILD --target hp2)"; exit 2; }
OUT="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/test_clock-$(date +%Y%m%d-%H%M%S)-$$"
FRAMES="600 2000 4200 6000 6320"
FAILS=0
fail() {
  echo "  FAIL: $*"
  FAILS=$((FAILS + 1))
}

run() {
  local dir="$OUT/$1"
  mkdir -p "$dir/home"
  (
    ulimit -c 0
    unset HP2_STUB HP2_NO_PACING
    export HOME="$dir/home" HP2_LOG_DIR="$dir" HP2_KTRACE=0 HP2_TEST_CLOCK=frames \
           SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_SKIP_MOVIES=1 \
           HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}" \
           HP2_PAD_SCRIPT="$ROOT/tests/scripts/phase3_new_game.pad" \
           HP2_SHOT_FRAMES="${FRAMES// /,}" HP2_EXIT_AFTER=6320
    exec perl -e 'alarm 400; exec @ARGV' "$BIN" > "$dir/stdout.log" 2> "$dir/stderr.log"
  )
}

echo "1. two test-clock runs at once"
run a & PA=$!
run b & PB=$!
wait $PA; CA=$?
wait $PB; CB=$?
echo "  exit $CA / $CB, logs $OUT/{a,b}"
[[ $CA == 0 && $CB == 0 ]] || fail "exit codes $CA / $CB"
for r in a b; do
  n=$(grep -c "still busy" "$OUT/$r/stderr.log")
  [[ $n == 0 ]] || fail "run $r: Swap barrier timed out $n time(s)"
done

echo "2. frames identical between the runs"
for f in $FRAMES; do
  png=$(printf 'frame_%06d.png' "$f")
  if [[ ! -f "$OUT/a/$png" || ! -f "$OUT/b/$png" ]]; then
    fail "missing $png"
    continue
  fi
  echo -n "  $f: "
  python3 "$ROOT/tests/scripts/png_diff.py" "$OUT/a/$png" "$OUT/b/$png" 0.05 0.0005 || fail "frame $f differs"
done

if [[ $FAILS == 0 ]]; then
  echo "PASS"
  exit 0
fi
echo "FAIL ($FAILS)"
exit 1
