#!/bin/bash
# Long soak in one map: a seeded pad_fuzz wander (unpaced) with a hang watchdog.
#
#   tests/scripts/level_wander.sh <build_dir> <row> <chapter> <frames> <seed>
#   e.g. level_wander.sh out 16 2 70000 12     (Great Hall, ~20 min of game time)
#
# How the map is reached: a New Game save at the chapter's start map (as in
# levels_smoke.sh), Continue, then at frame 1500 the game's own level change to
# <row> (pending map 0x527B94 via 0x4AD50, the call doors and "caught" use).
# Crafted saves aimed at some maps (Great Hall, Diagon Alley) fall back to the
# Burrow; this path does not. pad_fuzz input from frame 3000 to <frames>.
#
# Silent background run; output in .cache/HP2Cache/runs/wander-r<row>-s<seed>-<stamp>-<pid>/:
# result.txt (exit, hang, last frame, the map's .geo files, FATAL, component
# heap / HLE region lines), 3_wander/{pad.txt,runtime.log,stderr.log,frame_*.png}.
# Fails on exit != 0, a hang (no new frame for 90 s) or a FATAL.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
[[ $# == 5 ]] || { sed -n '2,5p' "$0"; exit 2; }
BIN="$1/src/loader/hp2"; ROW=$2; CH=$3; FR=$4; SEED=$5
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
CH_START=(0 4 30 30 30 30 30 30 30 30 30 30 27 52); START=${CH_START[$CH]}
OUT="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/wander-r$ROW-s$SEED-$(date +%Y%m%d-%H%M%S)-$$"; PROFILE="$OUT/home"
mkdir -p "$PROFILE"
DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}"

# env for a silent run in <dir> with <pad>
envs() {
  echo HOME="$PROFILE" HP2_LOG_DIR="$1" HP2_KTRACE=0 HP2_KTRACE_KERNEL=1 SDL_AUDIO_DRIVER=dummy \
    SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_SKIP_MOVIES=1 HP2_DATA="$DATA" HP2_PAD_SCRIPT="$2"
}
save_call() {  # <row> <chapter> <frame>: the save point writes slot 0 aimed at that map
  local f=$3
  echo "$f:4C140/0,$f:copy:529C10:527B78:2064,$f:poke:529C10:$(printf %X $1),$f:poke:529C14:$(printf %X $2),$f:poke:529C18:$(printf %X $((1 << $2))),$f:poke:529BE0:1,$f:39A30/1E8058@1E7F9C"
}

d="$OUT/1_newgame"; mkdir -p "$d"
printf '0 disconnect\n1 connect\n450 press A\n600 press A\n7060 press A\n' > "$d/pad.txt"
( ulimit -c 0; exec env $(envs "$d" "$d/pad.txt") HP2_CALL_AT="$(save_call $START $CH 7000)" \
    HP2_EXIT_AFTER=7300 "$BIN" > "$d/stdout.log" 2> "$d/stderr.log" ) || { echo "FAIL: New Game save ($d)"; exit 1; }

d="$OUT/3_wander"; mkdir -p "$d"
{ printf '0 disconnect\n1 connect\n450 press A\n600 press A\n'
  python3 "$ROOT/tests/scripts/pad_fuzz.py" "$SEED" 3000 "$FR"; } > "$d/pad.txt"
( ulimit -c 0; exec env $(envs "$d" "$d/pad.txt") HP2_NO_PACING=1 HP2_SHOT_EVERY=6000 \
    HP2_EXIT_AFTER="$FR" HP2_CALL_AT="1500:4AD50/$(printf %X $ROW)" "$BIN" > "$d/stdout.log" 2> "$d/stderr.log" ) &
pid=$! last=-1 still=0 hang=0
while kill -0 $pid 2>/dev/null; do
  sleep 5
  fr=$(grep -c '^d3d8: frame ' "$d/runtime.log" 2>/dev/null)
  if [[ $fr == "$last" ]]; then
    still=$((still + 5))
    if (( still >= 90 )); then
      hang=1; sample $pid 5 -file "$d/hang_sample.txt" > /dev/null 2>&1; kill -9 $pid; break
    fi
  else last=$fr; still=0; fi
done
wait $pid; code=$?
lf=$(sed -n 's/^d3d8: frame \([0-9]*\):.*/\1/p' "$d/runtime.log" | tail -1)
fatal=$(grep -h -m1 "FATAL" "$d/runtime.log" "$d/stderr.log" 2>/dev/null | head -1)
{
  echo "row $ROW ch $CH seed $SEED: exit $code, hang $hang, last frame ${lf:-?}"
  echo "  maps: $(grep -oE 'open [^ ]*\.geo' "$d/stderr.log" | grep -oE '[A-Za-z_]+\.geo' | sort -fu | tr '\n' ' ')"
  [[ -n "$fatal" ]] && echo "  $fatal"
  grep -h "component heap peak\|component heap full\|object slots full\|arena allocation failed\|HLE region" \
    "$d/runtime.log" | tail -4 | sed 's/^/  /'
  if [[ $code == 0 && $hang == 0 && -z "$fatal" ]]; then echo "  OK"; else echo "  FAILED ($d)"; fi
} | tee "$OUT/result.txt"
[[ $code == 0 && $hang == 0 && -z "$fatal" ]]
