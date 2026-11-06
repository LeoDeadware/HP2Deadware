#!/bin/bash
# Right stick -> gameplay camera (XInput HLE axis signs vs the game's camera).
#
#   tests/scripts/camera_stick.sh [build_dir]      (default: out)
#
# The game (FUN_00019050) reads sThumbRX/RY with the XInput convention
# (right / up = positive, /32768, dead zone 0.35); the gameplay camera
# FUN_00022320 (object 0x50C0E8) turns with RX (yaw 0x50C454, rate 0x50C460)
# and ZOOMS with RY (distance +0x30 = 0x50C118, 1.0 .. 3.5): that is the
# original design. Options "Camera Control" (state byte 0x527B8A) flips both:
# 0 = "Reverse" (the original Xbox default): RX right turns the view left (yaw
# falls), RY up zooms in; 1 = "Normal": RX right turns right (yaw rises), RY up
# zooms out. Our default is Normal (xinput.cpp GameOptionDefaults, settings.toml
# [camera] default_option).
#
# Two silent paced runs of the phase-3 path (New Game, Burrow gameplay at
# 6000), right stick only: 6000 RX +1 (40 frames), 6100 RY +1 (60), 6200 RY -1 (60).
# Run "normal" keeps our default (asserts it is Normal); run "reverse" pokes
# 0x527B8A = 0 at 5980.
# Checks: exit 0; yaw 5990 -> 6025 falls (reverse) / rises (normal) (25
# frames: under a half turn with the modern camera's 330 deg/s; the yaw
# 0x50C454 wraps at +-pi); after
# 60 frames of RY up the distance is at its near bound 1.0 (reverse) / far
# bound 3.5 (normal), after RY down at the other bound.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN (cmake --build $BUILD --target hp2)"; exit 2; }
OUT="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/camera_stick-$(date +%Y%m%d-%H%M%S)-$$"
FAILS=0
fail() {
  echo "  FAIL: $*"
  FAILS=$((FAILS + 1))
}

# run <name> [env...]
run() {
  local dir="$OUT/$1"
  shift
  mkdir -p "$dir/home"
  grep -vE '^6[0-9]{3} ' "$ROOT/tests/scripts/phase3_new_game.pad" > "$dir/pad.txt"
  printf '6000 stick R 1 0 40\n6100 stick R 0 1 60\n6200 stick R 0 -1 60\n' >> "$dir/pad.txt"
  (
    ulimit -c 0
    unset HP2_STUB HP2_NO_PACING HP2_TEST_CLOCK
    export HOME="$dir/home" HP2_LOG_DIR="$dir" HP2_KTRACE=0 \
           SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_SKIP_MOVIES=1 \
           HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}" HP2_PAD_SCRIPT="$dir/pad.txt" \
           HP2_DUMP_AT=5990:50C454:4,6025:50C454:4,6095:50C118:4,6165:50C118:4,6265:50C118:4 \
           HP2_SHOT_FRAMES=5990,6025,6095,6165,6265 HP2_EXIT_AFTER=6270
    exec perl -e 'alarm 300; exec @ARGV' env "$@" "$BIN" > "$dir/stdout.log" 2> "$dir/stderr.log"
  )
}

# check <name> <sign>: sign +1 = normal (default), -1 = reverse.
check() {
  local dir="$OUT/$1" code=$2 sign=$3
  echo "  $1: exit $code, logs $dir"
  [[ $code == 0 ]] || { fail "$1: exit code $code"; return; }
  python3 - "$dir" "$sign" <<'EOF' || FAILS=$((FAILS + 1))
import glob, math, os, struct, sys
d, sign = sys.argv[1], int(sys.argv[2])
def val(va, frame):
    m = glob.glob(os.path.join(d, f"dump_{va}_{frame}.bin"))
    if not m:
        print(f"  FAIL: no dump {va} at frame {frame}"); sys.exit(1)
    return struct.unpack("<f", open(m[0], "rb").read(4))[0]
yaw0, yaw1 = val("0050C454", 5990), val("0050C454", 6025)
d0, d1, d2 = (val("0050C118", f) for f in (6095, 6165, 6265))
print(f"    RX right: yaw {yaw0:.3f} -> {yaw1:.3f}; RY up: distance {d0:.2f} -> {d1:.2f}; RY down: -> {d2:.2f}")
bad = []
if math.remainder(yaw1 - yaw0, 2 * math.pi) * sign <= 0.1: bad.append("RX right turned the camera the wrong way (or not at all)")
# 60 frames at 0.1 per frame always reach a bound (1.0 near, 3.5 far).
near, far = 1.0, 3.5
if abs(d1 - (far if sign > 0 else near)) > 0.01: bad.append("RY up zoomed the wrong way (or not at all)")
if abs(d2 - (near if sign > 0 else far)) > 0.01: bad.append("RY down zoomed the wrong way (or not at all)")
for b in bad: print(f"  FAIL: {b}")
sys.exit(1 if bad else 0)
EOF
}

echo "1. Camera Control: Normal (our default) and Reverse, two runs at once"
run normal & PA=$!
run reverse HP2_CALL_AT=5980:poke:527B88:0 & PB=$!
wait $PA; CA=$?
wait $PB; CB=$?
check normal $CA 1
check reverse $CB -1

if [[ $FAILS == 0 ]]; then
  echo "PASS"
  exit 0
fi
echo "FAIL ($FAILS)"
exit 1
