#!/bin/bash
# stream_stress.sh <build_dir> <iterations> [env...]: boots to the menu silently;
# once the frontend music stream is open, a second guest thread opens, queues,
# queries and closes streams <iterations> times (HP2_DS_STREAM_STRESS, see
# engine_fixes.cpp) while the sound tick and the game's pump service them.
# PASS = "stream stress: done" and no FATAL. Extra env, e.g.
# HP2_DS_QUEUE_WIDEN_MS=20 (widen QueueSegment's window) or
# HP2_DS_NO_SLOT_FIX=1 (as shipped, for a control run).
set -uo pipefail
BUILD="$1" N="$2"; shift 2
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
RUN="${HP2_CACHE:-$("$(dirname "$0")/../../../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/stream_stress-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN/home"
(
  ulimit -c 0
  export HOME="$RUN/home" HP2_LOG_DIR="$RUN" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy \
         SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_SKIP_MOVIES=1 \
         HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../../../tools/main_checkout.sh")}" \
         HP2_DS_STREAM_STRESS="$N" HP2_DS_STREAM_STRESS_EXIT=1 HP2_EXIT_AFTER=200000
  exec env "$@" "$BIN" > "$RUN/stdout.log" 2> "$RUN/stderr.log"
) &
PID=$!
for _ in $(seq 1 $((N * 2 + 120))); do kill -0 $PID 2>/dev/null || break; sleep 1; done
kill -0 $PID 2>/dev/null && { kill -9 $PID; echo "timeout"; }
wait $PID
CODE=$?
grep -E "stream stress: (done|[0-9]+ iterations)|no queued segment|FATAL" "$RUN/stderr.log" | head -8
echo "exit $CODE, run dir $RUN"
if [[ $CODE == 0 ]] && grep -q "stream stress: done" "$RUN/stderr.log" && ! grep -q FATAL "$RUN/stderr.log"; then
  echo PASS
else
  echo FAIL; exit 1
fi
