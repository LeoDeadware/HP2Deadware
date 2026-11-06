#!/bin/bash
# Save game round trip through the game's own save point UI, with options
# (Kernel HLE: U:\ partition, XAPI save API, signatures; game: save menu).
#
#   tests/scripts/save_roundtrip.sh [build_dir]      (default: out)
#   UPDATE_GOLDEN=1 tests/scripts/save_roundtrip.sh  (re-take tests/golden/save_*.png)
#
# Two silent paced runs on one scratch profile (.cache/HP2Cache/runs/save_roundtrip-*):
#  1. Main menu Options: Vibration Off, Music Volume 47; New Game on slot
#     Potter A. In the Burrow, at frame 7000, HP2_CALL_AT does what the game's
#     save point does (the tail of FUN_00054DB0 at 0x4DD30, not a function
#     entry): select slot 0 (FUN_0004C140), commit the state block 0x527B78
#     (0x2064 bytes, options included) into slot 0 at 0x529C10, mark the slot
#     valid, open the save menu (FUN_00039A30, ecx 0x1E7F9C, menu 0x1E8058).
#     The pad script answers "Would you like to save up to this point?" with A:
#     the game shows "Saving game" and "Game saved" (front-end state machine
#     FUN_0004C170 -> FUN_00018A40: XCreateSaveGame U:\ + "POTTER A", 0x2094
#     bytes + a 20-byte XCalculateSignature HMAC).
#     Checks: prompt and "Game saved" screens == goldens; U:\UDATA\4541000e\
#     <dir>\POTTER A is 8360 bytes (0x20A8, the loader's size check) with
#     Vibration 0 / Music 47 in the state block; SaveMeta.xbx has Name=POTTER A.
#  2. Restart. Main menu offers Continue: A, A (Potter A) loads the save (the
#     loader FUN_00018820 re-computes the signature; FUN_0004C0D0 restores the
#     state block). Checks: frame 590 slot list ("Potter A  The Burrow"), frame
#     5100 gameplay (no reconnect overlay), frame 5420 pause-menu Options
#     (Vibration Off, Music 47) == goldens.
#  Both runs: exit 0, no FATAL.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN (cmake --build $BUILD --target hp2)"; exit 2; }
GOLDEN="$ROOT/tests/golden"
OUT="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/save_roundtrip-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$OUT/home" "$OUT/save" "$OUT/load"
UDATA="$OUT/home/Library/Application Support/HP2Port/default/hdd/partition1/UDATA/4541000e"
FAILS=0
fail() {
  echo "  FAIL: $*"
  FAILS=$((FAILS + 1))
}

# run <dir> <timeout s> [env...]
run() {
  local dir=$1 secs=$2
  shift 2
  (
    ulimit -c 0
    export HOME="$OUT/home" HP2_LOG_DIR="$dir" HP2_KTRACE=0 \
           SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_SKIP_MOVIES=1 \
           HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}" HP2_PAD_SCRIPT="$dir/pad.txt"
    unset HP2_STUB HP2_NO_PACING HP2_TEST_CLOCK
    exec env "$@" "$BIN" > "$dir/stdout.log" 2> "$dir/stderr.log"
  ) &
  local pid=$!
  for _ in $(seq 1 $((secs * 2))); do
    kill -0 $pid 2>/dev/null || break
    sleep 0.5
  done
  if kill -0 $pid 2>/dev/null; then
    kill -9 $pid
    fail "timeout after ${secs}s"
  fi
  wait $pid
  CODE=$?
  echo "  exit $CODE, log $dir/runtime.log"
  [[ $CODE == 0 ]] || fail "exit code $CODE"
  grep -h -m1 -A3 "FATAL" "$dir/runtime.log" "$dir/stderr.log" 2>/dev/null && fail "fatal error"
}

check_shot() {
  local png=$1 gold="$GOLDEN/$2"
  if [[ ! -f "$png" ]]; then
    fail "no screenshot $png"
  elif [[ "${UPDATE_GOLDEN:-0}" == 1 ]]; then
    cp "$png" "$gold"
    echo "  golden updated: $gold"
  elif [[ ! -f "$gold" ]]; then
    fail "no golden $gold (check $png, then UPDATE_GOLDEN=1)"
  else
    python3 "$ROOT/tests/scripts/png_diff.py" "$png" "$gold" || fail "$png differs from $gold"
  fi
}

echo "1. options, new game (Potter A), the game's save point at frame 7000"
cat > "$OUT/save/pad.txt" <<'EOF'
0 disconnect
1 connect
# main menu -> Options: Vibration (A toggles: Off), Music Volume (LEFT x2: 47), back
450 press DOWN
480 press A
560 press DOWN
590 press DOWN
620 press DOWN
660 press A
700 press DOWN
730 press DOWN
760 press LEFT
790 press LEFT
1060 press B
# New Game -> Potter A
1100 press UP
1130 press A
1280 press A
# "Would you like to save up to this point?" (opened at 7000) -> Yes
7060 press A
EOF
run "$OUT/save" 240 \
  HP2_CALL_AT=7000:4C140/0,7000:copy:529C10:527B78:2064,7000:poke:529BE0:1,7000:39A30/1E8058@1E7F9C \
  HP2_SHOT_FRAMES=7040,7260 HP2_EXIT_AFTER=7300
check_shot "$OUT/save/frame_007040.png" save_prompt.png
check_shot "$OUT/save/frame_007260.png" save_done.png
SAVE=$(find "$UDATA" -name 'POTTER A' -type f 2>/dev/null | head -1)
if [[ -z "$SAVE" ]]; then
  fail "no save file POTTER A under $UDATA"
else
  SIZE=$(stat -f %z "$SAVE")
  echo "  $SAVE: $SIZE bytes"
  [[ $SIZE == 8360 ]] || fail "save is $SIZE bytes, the loader wants 8360"
  # State block at +0x30: Vibration @0x0C, Music @0x15 (0x527B84 / 0x527B8D).
  python3 -c "import sys; b=open(sys.argv[1],'rb').read(); sys.exit(not (b[0x3C]==0 and b[0x45]==47))" \
    "$SAVE" || fail "options (Vibration 0, Music 47) not in the save"
  META="$(dirname "$SAVE")/SaveMeta.xbx"
  python3 -c "import sys; t=open(sys.argv[1],'rb').read().decode('utf-16'); sys.exit('Name=POTTER A' not in t)" \
    "$META" 2>/dev/null || fail "SaveMeta.xbx has no Name=POTTER A"
fi

echo "2. restart: Continue -> Potter A, pause -> Options"
cat > "$OUT/load/pad.txt" <<'EOF'
0 disconnect
1 connect
450 press A    # Continue
600 press A    # Potter A
5200 press START
5260 press DOWN
5290 press DOWN
5320 press A   # Options
EOF
run "$OUT/load" 240 HP2_SHOT_FRAMES=590,5100,5420 HP2_EXIT_AFTER=5420
check_shot "$OUT/load/frame_000590.png" save_continue_slots.png
check_shot "$OUT/load/frame_005100.png" save_loaded.png
check_shot "$OUT/load/frame_005420.png" save_loaded_options.png
python3 "$ROOT/tests/scripts/overlay_check.py" "$OUT/load/frame_005100.png" > /dev/null ||
  fail "reconnect overlay in the loaded game"

if [[ $FAILS == 0 ]]; then
  echo "PASS"
  exit 0
fi
echo "FAIL ($FAILS)"
exit 1
