#!/bin/bash
# Phase-1 milestone: the lifted default.xbe boots (XBE mapped, TLS/stack,
# kernel thunk table bound to HLE) and runs from the entry point 0x10E698 to
# the first call into the D3D section, logging every kernel call.
#
#   tests/scripts/phase1_first_d3d.sh [build_dir]     (default: out)
#
# Silent and in the background: dummy audio, HP2_BACKGROUND=1, scratch HOME,
# its own log dir under .cache/HP2Cache/runs/. Exit 0 = milestone reached.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN (cmake --build $BUILD --target hp2)"; exit 2; }

RUN="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/phase1_first_d3d-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN/home"
ulimit -c 0
export HOME="$RUN/home" HP2_LOG_DIR="$RUN" HP2_STOP_AT=D3D HP2_KTRACE=1 \
       SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 \
       HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}"

# 120 s is far more than booting needs; a hang is a failure.
"$BIN" > "$RUN/stdout.log" 2> "$RUN/stderr.log" &
PID=$!
for _ in $(seq 1 240); do
  kill -0 $PID 2>/dev/null || break
  sleep 0.5
done
if kill -0 $PID 2>/dev/null; then
  kill -9 $PID
  echo "FAIL: timeout (log: $RUN)"
  exit 1
fi
wait $PID
CODE=$?

KCALLS=$(grep -c "^K> " "$RUN/runtime.log" 2>/dev/null || echo 0)
echo "exit $CODE, $KCALLS kernel calls, log: $RUN/runtime.log"
grep -E "^hp2: (first call into|kernel export|exit)|^FATAL" "$RUN/runtime.log" | head -5
if [[ $CODE == 0 ]] && grep -q "^hp2: first call into D3D" "$RUN/runtime.log"; then
  echo "PASS"
  exit 0
fi
echo "FAIL"
exit 1
