#!/bin/bash
# Modern orbit camera (src/hle/xinput/camera.cpp, settings.toml [camera]
# modern / yaw_speed / recenter_delay) against the original camera.
#
#   tests/scripts/camera_modern.sh [build_dir]      (default: out)
#
# Two silent paced runs of the phase-3 path side by side (New Game, Burrow
# gameplay at 6000): "modern" (the default) and "original"
# (HP2_INPUT_CAMERA_MODERN=0), both with HP2_CAMERA_LOG=1 (one line per camera
# frame: vblank, raw RX, visible camera and target x/z). Pad:
#   6000 left stick up 60 frames   (Harry walks: arms the game's follow swing)
#   6060 right stick full right 90 (Harry stands: the follow swing would pull back)
#   6560 left stick up 40          (walk "up" after the turn)
# Checks (modern): the visible camera turns >= 300 deg in the first second of
# full right stick (yaw_speed 330 deg/s), never backwards in any frame (no
# snap-back while the stick is held), and the turn rate is reached within 2
# frames (no ramp-up); after the release it stays put for 3.8 s and the
# game's swing back behind Harry is held off until recenter_delay (4 s) and
# allowed again then; walking "up" moves Harry away from the camera (within
# 30 deg of its view direction, once he has turned to the stick). The original run
# reports its numbers only.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN (cmake --build $BUILD --target hp2)"; exit 2; }
OUT="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/camera_modern-$(date +%Y%m%d-%H%M%S)-$$"
FAILS=0

# run <name> [env...]
run() {
  local dir="$OUT/$1"
  shift
  mkdir -p "$dir/home"
  grep -vE '^6[0-9]{3} ' "$ROOT/tests/scripts/phase3_new_game.pad" > "$dir/pad.txt"
  printf '6000 stick L 0 1 60\n6060 stick R 1 0 90\n6560 stick L 0 1 120\n' >> "$dir/pad.txt"
  (
    ulimit -c 0
    unset HP2_STUB HP2_NO_PACING HP2_TEST_CLOCK
    export HOME="$dir/home" HP2_LOG_DIR="$dir" HP2_KTRACE=0 HP2_CAMERA_LOG=1 \
           SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_SKIP_MOVIES=1 \
           HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}" HP2_PAD_SCRIPT="$dir/pad.txt" \
           HP2_SHOT_FRAMES=6055,6150,6400,6555,6700 HP2_EXIT_AFTER=6700
    exec perl -e 'alarm 300; exec @ARGV' env "$@" "$BIN" > "$dir/stdout.log" 2> "$dir/stderr.log"
  )
}

# check <name> <exit code> <assert 0|1>
check() {
  local dir="$OUT/$1" code=$2 strict=$3
  echo "  $1: exit $code, logs $dir"
  [[ $code == 0 ]] || { echo "  FAIL: $1: exit code $code"; FAILS=$((FAILS + 1)); return; }
  python3 - "$dir/stderr.log" "$strict" <<'EOF' || FAILS=$((FAILS + 1))
import math, re, sys
log, strict = sys.argv[1], sys.argv[2] == "1"
rows = []
pat = re.compile(r"camera: vb (\d+) rx (\S+) hold (\d) pos (\S+) (\S+) target (\S+) (\S+)")
for line in open(log, errors="replace"):
    m = pat.search(line)
    if m:
        vb, rx, hold, px, pz, tx, tz = m.groups()
        rows.append((int(vb), float(rx), int(hold), float(px), float(pz), float(tx), float(tz)))
if not rows:
    print("  FAIL: no camera trace (HP2_CAMERA_LOG)"); sys.exit(1)
def ang(r):  # visible camera around the target, the game's convention
    return math.atan2(r[3] - r[5], r[4] - r[6])
bad = []
turn = [r for r in rows if r[1] > 0.9]
if len(turn) < 10:
    print(f"  FAIL: only {len(turn)} camera frames with the right stick held"); sys.exit(1)
# Unwrapped per-frame steps over the hold (from the frame before it).
i0 = rows.index(turn[0])
seg = rows[max(i0 - 1, 0): rows.index(turn[-1]) + 1]
steps = []
for a, b in zip(seg, seg[1:]):
    d = ang(b) - ang(a)
    d = (d + math.pi) % (2 * math.pi) - math.pi
    steps.append((b[0] - a[0], math.degrees(d)))
t0 = seg[0][0]
total_1s = 0.0
for (dv, d), r in zip(steps, seg[1:]):
    if r[0] - t0 <= 60:
        total_1s += d
held_s = (seg[-1][0] - t0) / 60
total = sum(d for _, d in steps)
rates = [d / dv * 60 for dv, d in steps if dv > 0]
print(f"    right stick held {held_s:.2f} s ({len(steps)} camera frames): turned {total:.1f} deg, "
      f"{total_1s:.1f} deg in the first second; per-frame rate min {min(rates):.0f} / "
      f"max {max(rates):.0f} deg/s; first frames {', '.join(f'{x:.0f}' for x in rates[:3])} deg/s")
# After the release: still for 3.8 s; the hold on the game's swing back ends at 4 s.
rel = seg[-1][0]
def diff(a, b):
    return abs((math.degrees(ang(b) - ang(a)) + 180) % 360 - 180)
still = [diff(seg[-1], r) for r in rows if rel < r[0] <= rel + 228]
held = [r[2] for r in rows if rel < r[0] <= rel + 228]
freed = [r[0] - rel for r in rows if r[0] > rel and r[2] == 0]
print(f"    after the release: moved {max(still or [0]):.1f} deg in 3.8 s; swing back allowed "
      f"after {freed[0] / 60 if freed else -1:.2f} s")
# Walk "up" after the turn: once Harry has turned to the stick, his
# displacement over the 30 vblanks where he goes furthest vs the camera's view
# direction at the start of that window.
walk = [r for r in rows if r[0] > rel + 400]
best = None
for i, a in enumerate(walk):
    b = next((r for r in walk[i:] if r[0] >= a[0] + 30), None)
    if b:
        dist = math.hypot(b[5] - a[5], b[6] - a[6])
        if dist > 0.3 and (best is None or dist > best[0]): best = (dist, a, b)
off = None
if best:
    _, a, b = best
    view = math.atan2(a[5] - a[3], a[6] - a[4])
    move = math.atan2(b[5] - a[5], b[6] - a[6])
    off = abs((math.degrees(move - view) + 180) % 360 - 180)
print(f"    left stick up after the turn: Harry walks {off if off is not None else -1:.0f} deg off "
      "the camera's view direction")
if strict:
    if total_1s < 300: bad.append(f"turned only {total_1s:.0f} deg in 1 s of full right stick (want >= 300)")
    if min(d for _, d in steps) < 0: bad.append("the camera turned back while the stick was held")
    if min(rates[1:3]) < 0.8 * 330: bad.append("slow ramp-up: frames 2-3 below 80% of yaw_speed")
    if max(still or [0]) > 3: bad.append("the camera moved within 3.8 s of the release (recenter_delay 4 s)")
    if min(held or [0]) != 1: bad.append("the swing back was allowed within 3.8 s of the release")
    if not freed or not 3.9 <= freed[0] / 60 <= 4.2: bad.append("the swing back is not allowed again at 4 s")
    if off is None or off > 30: bad.append("left stick up does not move Harry away from the camera")
for b in bad: print(f"  FAIL: {b}")
sys.exit(1 if bad else 0)
EOF
}

echo "1. modern camera (default) vs original, two runs at once"
run modern & PA=$!
run original HP2_INPUT_CAMERA_MODERN=0 & PB=$!
wait $PA; CA=$?
wait $PB; CB=$?
check modern $CA 1
check original $CB 0

if [[ $FAILS == 0 ]]; then
  echo "PASS"
  exit 0
fi
echo "FAIL ($FAILS)"
exit 1
