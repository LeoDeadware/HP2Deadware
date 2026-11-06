#!/bin/bash
# Soak / fuzz test of the playable build: boots to the first level (the Burrow)
# and drives a long seeded pseudo-random pad script (tests/scripts/pad_fuzz.py).
#
#   tests/scripts/soak_level1.sh [build_dir]           (default: out)
#   SEEDS="1 2 3"   seeds, one run each (default 1 2 3)
#   FRAMES=60000    frames per run (default 60000 = 16.7 min of game time;
#                   gameplay starts at ~6000)
#   PACED=1         60 Hz pacing (default: unpaced, HP2_NO_PACING=1, faster)
#   SLOT=A          save slot Potter A (default: Continue Without Saving)
#   JOBS=2          runs at once (default 1; never more than 2 on this Mac)
#   HANG_S=90       watchdog: no new frame for this long = hang
#
# Silent background runs (dummy video/audio, HP2_BACKGROUND=1, scratch HOME);
# per run under .cache/HP2Cache/runs/soak-<stamp>/seed<N>/: pad.txt, logs, a PNG
# every 1800 frames, hang_sample.txt (macOS `sample` of a hung process).
# A run fails on: exit code != 0, FATAL, a hang, or the "reconnect the
# controller" overlay in a screenshot. Warnings and not-implemented calls are
# listed per run (unique, with counts) for triage.
# Repro of a failure: the seed + frame from the report; HP2_PAD_SCRIPT=<run>/pad.txt.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN (cmake --build $BUILD --target hp2)"; exit 2; }
SEEDS="${SEEDS:-1 2 3}"
FRAMES="${FRAMES:-60000}"
JOBS="${JOBS:-1}"
HANG_S="${HANG_S:-90}"
(( JOBS > 2 )) && JOBS=2
OUT="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/soak-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$OUT"
echo "soak: seeds [$SEEDS], $FRAMES frames, ${PACED:+paced}${PACED:-unpaced}, slot ${SLOT:-none}, $JOBS at once -> $OUT"

# Menu prefix (frames as in phase3_new_game.pad): New Game, then a save slot.
prefix() {
  echo "0 disconnect"
  echo "1 connect"
  echo "450 press A"
  if [[ "${SLOT:-}" == A ]]; then
    echo "600 press A            # Potter A"
  else
    printf '520 press DOWN 4\n540 press DOWN 4\n560 press DOWN 4\n'
    echo "600 press A            # Continue Without Saving"
    echo "700 press A            # Yes"
  fi
}

# run_seed <seed>: one run, result in $OUT/seed<seed>/result.txt
run_seed() {
  local seed=$1 run="$OUT/seed$1"
  mkdir -p "$run/home"
  { prefix; python3 "$ROOT/tests/scripts/pad_fuzz.py" "$seed" 6000 "$FRAMES"; } > "$run/pad.txt"
  (
    ulimit -c 0
    export HOME="$run/home" HP2_LOG_DIR="$run" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy \
           SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_SKIP_MOVIES=1 \
           HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}" \
           HP2_PAD_SCRIPT="$run/pad.txt" HP2_SHOT_EVERY=1800 HP2_EXIT_AFTER="$FRAMES"
    [[ -n "${PACED:-}" ]] || export HP2_NO_PACING=1
    unset HP2_STUB
    exec "$BIN" > "$run/stdout.log" 2> "$run/stderr.log"
  ) &
  local pid=$! last=-1 still=0 hang=0 t0=$SECONDS
  while kill -0 $pid 2>/dev/null; do
    sleep 5
    local fr
    fr=$(grep -c '^d3d8: frame ' "$run/runtime.log" 2>/dev/null)  # a stats line per 120 frames
    if [[ $fr == "$last" ]]; then
      still=$((still + 5))
      if (( still >= HANG_S )); then
        hang=1
        sample $pid 5 -file "$run/hang_sample.txt" > /dev/null 2>&1
        kill -9 $pid
        break
      fi
    else
      last=$fr
      still=0
    fi
  done
  wait $pid
  local code=$? log="$run/runtime.log"
  local lastframe
  lastframe=$(sed -n 's/^d3d8: frame \([0-9]*\):.*/\1/p' "$log" | tail -1)
  {
    echo "seed $seed: exit $code, last stats frame ${lastframe:-?}, $((SECONDS - t0)) s"
    local bad=0
    [[ $code == 0 ]] || bad=1
    if [[ $hang == 1 ]]; then
      echo "  HANG: no new frame for ${HANG_S} s (sample: $run/hang_sample.txt)"
      bad=1
    fi
    if grep -q "FATAL" "$log" "$run/stderr.log" 2>/dev/null; then
      grep -h -m1 -A4 "FATAL" "$log" "$run/stderr.log" | head -6 | sed 's/^/  /'
      bad=1
    fi
    local over=0 shot
    for shot in "$run"/frame_*.png; do
      [[ -f "$shot" ]] || continue
      python3 "$ROOT/tests/scripts/overlay_check.py" "$shot" > /dev/null || {
        echo "  OVERLAY (reconnect controller): $shot"
        over=$((over + 1))
      }
    done
    (( over )) && bad=1
    echo "  warnings / not implemented (unique):"
    grep -hE "warning|not implemented|not supported|unsupported|unknown|stub" "$log" "$run/stderr.log" 2>/dev/null |
      grep -v " 0 not implemented" | sed -E 's/0x[0-9a-fA-F]+/0x?/g; s/[0-9A-F]{8}/????????/g' | sort | uniq -c | sort -rn | head -15 |
      sed 's/^/   /'
    echo "  saves: $(find "$run/home/Library/Application Support/HP2Port" -path '*UDATA*' -type f 2>/dev/null |
      sed 's|.*/UDATA/||' | tr '\n' ' ')"
    [[ $bad == 0 ]] && echo "  OK" || echo "  FAILED (repro: SEED=$seed, HP2_PAD_SCRIPT=$run/pad.txt)"
  } > "$run/result.txt"
}

# Batches of $JOBS (macOS bash 3.2: no wait -n).
batch=()
for seed in $SEEDS; do
  run_seed "$seed" &
  batch+=($!)
  if (( ${#batch[@]} >= JOBS )); then
    wait "${batch[@]}"
    batch=()
  fi
done
wait

FAILS=0
for seed in $SEEDS; do
  cat "$OUT/seed$seed/result.txt"
  grep -q "^  OK" "$OUT/seed$seed/result.txt" || FAILS=$((FAILS + 1))
done
if [[ $FAILS == 0 ]]; then
  echo "PASS"
  exit 0
fi
echo "FAIL ($FAILS of $(echo $SEEDS | wc -w) seeds)"
exit 1
