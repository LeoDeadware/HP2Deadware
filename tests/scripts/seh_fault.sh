#!/bin/bash
# Guest faults go through guest SEH: XAPI's lstrcpynA (0x10E70A) wraps its
# copy loop in __try/__except(EXCEPTION_EXECUTE_HANDLER) { return NULL; }.
#   1. readable source (the XBE header at 0x10000, n=4): copies "XBE", returns dst;
#   2. unreadable source (guest 0x4): the lifted loop faults; the runtime raises
#      STATUS_ACCESS_VIOLATION through the kernel's RtlRaiseException,
#      __except_handler3 unwinds and resumes in the __except block (0x10E75E),
#      which returns 0.
#
#   tests/scripts/seh_fault.sh [build_dir]     (default: out)
#
# Silent and in the background (dummy audio, HP2_BACKGROUND=1, scratch HOME,
# log dirs under .cache/HP2Cache/runs/). Exit 0 = both cases pass.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN (cmake --build $BUILD --target hp2)"; exit 2; }
ulimit -c 0
export SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 \
       HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
FAIL=0

run() {  # name args expected-regex
  local name=$1 args=$2 want=$3
  local run="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/seh_fault-$STAMP-$name"
  mkdir -p "$run/home"
  HOME="$run/home" HP2_LOG_DIR="$run" HP2_CALL="10E70A:$args" "$BIN" \
    > "$run/stdout.log" 2> "$run/stderr.log" &
  local pid=$!
  for _ in $(seq 1 120); do kill -0 $pid 2>/dev/null || break; sleep 0.5; done
  kill -0 $pid 2>/dev/null && { kill -9 $pid; echo "FAIL $name: timeout ($run)"; FAIL=1; return; }
  wait $pid
  local code=$? line
  line="$(grep -a "HP2_CALL 0010E70A returned" "$run/runtime.log" | head -1)"
  if [[ $code == 0 ]] && grep -aEq "$want" <<< "$line"; then
    echo "PASS $name: $line"
  else
    echo "FAIL $name (exit $code): ${line:-no result} (log: $run/runtime.log)"
    grep -aE "^FATAL|access violation|unhandled" "$run/runtime.log" | head -3
    FAIL=1
  fi
}

run readable "buf,10000,4" 'returned FD000000 \(buf FD000000: "XBE"\)'
run fault    "buf,4,10"    'returned 00000000'
exit $FAIL
