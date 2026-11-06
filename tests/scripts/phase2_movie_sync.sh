#!/bin/bash
# Movie A/V sync (Render + Audio): the three intro movies, paced, with the
# DSound HLE mixing on its real-time timer (HP2_AUDIO=0: silent, no device).
#
# The XMV player shows a frame when the vblank count reaches the frame's
# timestamp plus an offset that the audio stream's packet-completion callback
# keeps setting (ms since start - completed packet's timestamp): video follows
# the audio clock. Checks (HP2_XMV_TRACE logs, movies.cpp):
#   - every logged offset |video offset| <= 100 ms (video vs audio);
#   - each movie's last frame lands within 2% of its audio length
#     (ea_logo 3.200 s, warner 6.474 s, hpengx 72.957 s, per the Audio HLE);
#   - exit 0, no FATAL.
# Note: SDL's "dummy" audio driver (other silent tests) consumes samples ~5.5%
# slower than real time, which stretches movies by that much; this test does
# not use it.
#
#   tests/scripts/phase2_movie_sync.sh [build_dir]
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
FAILS=0
fail() {
  echo "  FAIL: $*"
  FAILS=$((FAILS + 1))
}

RUN="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/phase2_movie_sync-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN/home"
(
  ulimit -c 0
  export HOME="$RUN/home" HP2_LOG_DIR="$RUN" HP2_KTRACE=0 HP2_AUDIO=0 \
         SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_XMV_TRACE=1 HP2_EXIT_AFTER=330 \
         HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}"
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

echo "video offset against the audio clock:"
sed -n 's/.*video offset \(-\{0,1\}[0-9]*\) ms.*/\1/p' "$LOG" | awk '
  { n++; v = $1 < 0 ? -$1 : $1; if (v > m) m = v }
  END { printf "  %d audio packet samples, max |offset| %d ms\n", n, m; exit !(n > 0 && m <= 100) }' ||
  fail "video drifts from the audio (or no samples)"

echo "movie durations (last frame vs audio length):"
LENS=(3.200 6.474 72.957)
i=0
while read -r frames last; do
  [[ $i -lt 3 ]] || break
  awk -v l="$last" -v a="${LENS[$i]}" -v f="$frames" -v k="$((i + 1))" 'BEGIN {
    d = (l - a) / a * 100; printf "  movie %d: %s frames, last at %.3f s, audio %.3f s (%+.1f%%)\n", k, f, l, a, d
    exit !(d < 2 && d > -2) }' || fail "movie $((i + 1)) length off by more than 2%"
  i=$((i + 1))
done <<< "$(sed -n 's/^d3d8: movie: \([0-9]*\) overlay frames, last at \([0-9.]*\) s.*/\1 \2/p' "$LOG")"
[[ $i == 3 ]] || fail "expected 3 movies, saw $i"

if [[ $FAILS == 0 ]]; then
  echo "PASS"
  exit 0
fi
echo "FAIL ($FAILS)"
exit 1
