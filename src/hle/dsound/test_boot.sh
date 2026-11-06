#!/bin/bash
# Audio HLE boot check: runs hp2 silently in the background with the real
# sound init (no HP2_STUB 117940, no HP2_DEV_APU_RAM), records the mix to a
# WAV and prints the dsound log lines + WAV levels.
#
#   src/hle/dsound/test_boot.sh <build_dir> <name> <timeout s> [VAR=value...]
#   e.g. test_boot.sh out menu 120 HP2_NO_PACING=1 HP2_SHOT_FRAMES=900 HP2_EXIT_AFTER=1000
#
# Logs, PNGs and mix.wav go to .cache/HP2Cache/runs/audio_<name>-<stamp>/.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$1" NAME="$2" SECS="$3"
shift 3
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
RUN="${HP2_CACHE:-$("$(dirname "$0")/../../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/audio_$NAME-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN/home"
(
  ulimit -c 0
  export HOME="$RUN/home" HP2_LOG_DIR="$RUN" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy \
         SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_AUDIO_WAV="$RUN/mix.wav" \
         HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../../tools/main_checkout.sh")}" \
         ${HP2_STUB:+HP2_STUB="$HP2_STUB"}
  exec env "$@" "$BIN" > "$RUN/stdout.log" 2> "$RUN/stderr.log"
) &
PID=$!
for _ in $(seq 1 $((SECS * 2))); do
  kill -0 $PID 2>/dev/null || break
  sleep 0.5
done
if kill -0 $PID 2>/dev/null; then
  kill -9 $PID
  echo "timeout after ${SECS}s"
fi
wait $PID
CODE=$?
echo "exit $CODE, run dir $RUN"
grep -E "dsound|FATAL|fatal|first call|exit" "$RUN/runtime.log" | grep -v "^ds: " | head -40
ls "$RUN" | grep -E "png$" | tail -3
[[ -f "$RUN/mix.wav" ]] && python3 "$HERE/wav_stats.py" "$RUN/mix.wav"
exit $CODE
