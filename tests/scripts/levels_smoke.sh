#!/bin/bash
# All-levels smoke test (QA): every level of tests/scripts/levels.tsv is loaded
# directly, played briefly with a random walk and checked.
#
#   tests/scripts/levels_smoke.sh [build_dir] [first_row ...]   (default: out, all rows)
#
# How a level is reached without playing there: a crafted save. The game's own
# save point (as in save_roundtrip.sh) copies the state block 0x527B78 into
# slot 0 (0x529C10) and writes POTTER A; before it writes, HP2_CALL_AT pokes the
# slot's map row (+0, index into the map table 0x1E3DE8) and chapter (+4, +8 =
# 1 << chapter). Continue -> Potter A then loads that map through the normal
# front-end load path. Runs are chained on one scratch profile: each level's run
# ends at the save point, writing the save for the next level. A failed level
# (or a save that did not happen) restarts the chain from a New Game save run.
#
# Per level (silent, paced, background; logs/PNGs/WAV in
# .cache/HP2Cache/runs/levels_smoke-<stamp>/<row>_<name>/):
#   frame 1500: arrival shot; 1600..2300: seeded random walk (sticks, A);
#   frame 2400: shot after the walk.
#   Checks: exit 0, no FATAL, frames reached 2400 (no hang), both shots not
#   blank and the second not frozen, no reconnect overlay, audio non-silent
#   in the level, the map's own .geo (levels.tsv column 4) was opened — the
#   proof that the crafted save loaded that map and not a fallback.
# Look at every PNG: the script cannot judge geometry or textures.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/tests/scripts"
BUILD="${1:-$ROOT/out}"
shift || true
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
ONLY=(${@+"$@"})
OUT="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/levels_smoke-$(date +%Y%m%d-%H%M%S)-$$"
PROFILE="$OUT/home"
mkdir -p "$PROFILE"
SUMMARY="$OUT/summary.txt"
ARRIVE=1500 WALK0=1600 WALK1=2300 SHOT2=2400 SAVE_AT=2500 EXIT_AT=2800

# run <dir> <timeout s> <pad> [env...] -> CODE
run() {
  local dir=$1 secs=$2 pad=$3
  shift 3
  mkdir -p "$dir"
  python3 "$ROOT/tools/guard.py" > /dev/null 2>&1 || sleep 30
  (
    ulimit -c 0
    export HOME="$PROFILE" HP2_LOG_DIR="$dir" HP2_KTRACE=0 HP2_KTRACE_KERNEL=1 \
           SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_SKIP_MOVIES=1 \
           HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}" HP2_PAD_SCRIPT="$pad" \
           HP2_AUDIO_WAV="$dir/mix.wav"
    unset HP2_STUB HP2_NO_PACING
    exec env "$@" "$BIN" > "$dir/stdout.log" 2> "$dir/stderr.log"
  ) &
  local pid=$!
  for _ in $(seq 1 $((secs * 2))); do
    kill -0 $pid 2>/dev/null || break
    sleep 0.5
  done
  if kill -0 $pid 2>/dev/null; then
    kill -9 $pid
    echo "timeout" > "$dir/timeout"
  fi
  wait $pid
  CODE=$?
}

# save_call <row> <chapter>: HP2_CALL_AT entries for the save point at SAVE_AT
# (or $2nd arg frame) writing slot 0 with that map/chapter.
save_call() {
  local f=$3
  echo "$f:4C140/0,$f:copy:529C10:527B78:2064,$f:poke:529C10:$(printf %X $1),$f:poke:529C14:$(printf %X $2),$f:poke:529C18:$(printf %X $((1 << $2))),$f:poke:529BE0:1,$f:39A30/1E8058@1E7F9C"
}

save_mtime() { find "$PROFILE" -name 'POTTER A' -type f -exec stat -f %m {} \; 2>/dev/null | head -1; }

# new_game_save <row> <chapter>: New Game -> Potter A, save point at 7000 with the target.
new_game_save() {
  local d="$OUT/_newgame_$1"
  mkdir -p "$d"
  # No save in the profile: otherwise the menu's first item is Continue, not New Game.
  find "$PROFILE" -path '*UDATA/4541000e/*' -type d -mindepth 1 -prune -exec rm -rf {} + 2>/dev/null
  printf '0 disconnect\n1 connect\n450 press A\n600 press A\n7060 press A\n' > "$d/pad.txt"
  run "$d" 200 "$d/pad.txt" HP2_CALL_AT="$(save_call $1 $2 7000)" HP2_EXIT_AFTER=7300
  [[ $CODE == 0 ]]
}

# Chapter start maps (table 0x1E5AB8): a New Game save aimed straight at some
# maps (e.g. DADA, Grand Staircase) is loaded as the Burrow instead, while the
# chapter's own start map always loads; restarts hop through it.
CH_START=(0 4 30 30 30 30 30 30 30 30 30 30 27 52)

# restart <row> <chapter>: New Game save at the chapter start map, then (if the
# target is another map) one load run there that saves the target.
restart() {
  local row=$1 ch=$2 start=${CH_START[$2]}
  new_game_save "$start" "$ch" || return 1
  [[ $start == $row ]] && return 0
  local d="$OUT/_hop_${start}_to_$row"
  mkdir -p "$d"
  printf '0 disconnect\n1 connect\n450 press A\n600 press A\n%d press A\n' $((SAVE_AT + 60)) > "$d/pad.txt"
  local before=$(save_mtime)
  run "$d" 150 "$d/pad.txt" HP2_CALL_AT="$(save_call $row $ch $SAVE_AT)" HP2_EXIT_AFTER=$EXIT_AT
  [[ $CODE == 0 && "$(save_mtime)" != "$before" ]]
}

# pad for a level run: Continue -> Potter A, seeded walk, A at the save prompt.
level_pad() {  # <file> <seed>
  python3 - "$1" "$2" "$WALK0" "$WALK1" "$((SAVE_AT + 60))" <<'EOF'
import random, sys
path, seed, w0, w1, save_a = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
r = random.Random(seed)
lines = ["0 disconnect", "1 connect", "450 press A", "600 press A"]
f = w0
while f < w1:
    n = r.randint(20, 60)
    if r.random() < 0.15:
        lines.append(f"{f} press A")
    else:
        x, y = r.choice([(0, 1), (0, 1), (1, 0), (-1, 0), (0.7, 0.7), (-0.7, 0.7), (0, -1)])
        lines.append(f"{f} stick L {x} {y} {n}")
    f += n + 5
lines.append(f"{w1 + 10} release all")
lines.append(f"{save_a} press A")
open(path, "w").write("\n".join(lines) + "\n")
EOF
}

audio_level() {  # <wav>: rms of the last 10 s and its non-silent seconds
  python3 - "$1" <<'EOF'
import array, math, sys, wave
try:
    w = wave.open(sys.argv[1]); r = w.getframerate(); n = w.getnframes()
    w.setpos(max(0, n - 10 * r)); a = array.array('h', w.readframes(10 * r))
except Exception as e:
    print("0 0"); sys.exit()
loud = sum(1 for i in range(0, len(a), 2 * r // 100) if max(abs(x) for x in a[i:i + 2 * r // 100] or [0]) > 100)
print(f"{math.sqrt(sum(x * x for x in a) / max(1, len(a))) / 32768:.4f} {loud / 100:.1f}")
EOF
}

LEVELS=()
while IFS= read -r line; do
  [[ -z "$line" || "$line" == \#* ]] && continue
  if [[ ${#ONLY[@]} -gt 0 ]]; then
    keep=0
    for r in ${ONLY[@]+"${ONLY[@]}"}; do [[ "${line%%$'\t'*}" == "$r" ]] && keep=1; done
    [[ $keep == 1 ]] || continue
  fi
  LEVELS+=("$line")
done < "$HERE/levels.tsv"
echo "levels_smoke: ${#LEVELS[@]} levels, out $OUT" | tee "$SUMMARY"
FAILS=0
need_new=1
for i in "${!LEVELS[@]}"; do
  IFS=$'\t' read -r row ch name want quiet <<< "${LEVELS[$i]}"
  tag="$(printf %02d $row)_$(echo "$name" | tr -c 'A-Za-z0-9\n' '_')"
  d="$OUT/$tag"
  if [[ $need_new == 1 ]]; then
    if ! restart "$row" "$ch"; then
      echo "$tag: FAIL (could not make a save for it via New Game + chapter start map, exit $CODE)" | tee -a "$SUMMARY"
      FAILS=$((FAILS + 1))
      continue
    fi
  fi
  next="${LEVELS[$((i + 1))]:-}"
  env_save=()
  if [[ -n "$next" ]]; then
    IFS=$'\t' read -r nrow nch _ <<< "$next"
    env_save=(HP2_CALL_AT="$(save_call $nrow $nch $SAVE_AT)")
  fi
  mkdir -p "$d"
  level_pad "$d/pad.txt" "$row"
  before=$(save_mtime)
  run "$d" 150 "$d/pad.txt" ${env_save[@]+"${env_save[@]}"} HP2_SHOT_FRAMES=$ARRIVE,$SHOT2 HP2_EXIT_AFTER=$EXIT_AT
  problems=()
  [[ -f "$d/timeout" ]] && problems+=("hang (timeout)")
  [[ $CODE == 0 ]] || problems+=("exit $CODE")
  fatal=$(grep -h -m1 "FATAL" "$d/runtime.log" "$d/stderr.log" 2>/dev/null | cut -c1-160)
  [[ -n "$fatal" ]] && problems+=("$fatal")
  last=$(grep -o "frame [0-9]*" "$d/stderr.log" | tail -1 | awk '{print $2}')
  for f in $ARRIVE $SHOT2; do
    png=$(printf "$d/frame_%06d.png" $f)
    if [[ ! -f "$png" ]]; then problems+=("no shot $f (last frame ${last:-?})"); continue; fi
    prev=""; [[ $f == $SHOT2 ]] && prev=$(printf "$d/frame_%06d.png" $ARRIVE)
    python3 "$HERE/shot_check.py" "$png" $prev > "$d/shot_$f.txt" || problems+=("shot $f: $(cat "$d/shot_$f.txt")")
    python3 "$HERE/overlay_check.py" "$png" > /dev/null || problems+=("reconnect overlay at $f")
  done
  geo=$(grep -c "open .*\.geo" "$d/stderr.log")
  # The right map: its geometry was opened in this run (a crafted save the
  # game rejects falls back to another map, e.g. the Burrow).
  # (one grep: with pipefail, `grep | grep -q` fails when -q exits early)
  grep -Eiq "open .*(${want})" "$d/stderr.log" || problems+=("wrong map: no ${want} opened")
  read -r rms loud <<< "$(audio_level "$d/mix.wav")"
  [[ "${quiet:-}" == quiet ]] || awk -v l="$loud" 'BEGIN{exit !(l >= 1)}' ||
    problems+=("audio silent (last 10 s: rms $rms, ${loud}s audible)")
  # The music the level streams (for review: crafted-save arrivals may not start
  # the map's own cue).
  mus=$(grep -Eio "open [^ ]*\.mus" "$d/stderr.log" | tail -1 | grep -Eo '[A-Za-z_]+\.mus')
  if [[ "${quiet:-}" == xfail* && ${#problems[@]} != 0 ]]; then
    printf '%s: XFAIL (%s): %s\n' "$tag" "${quiet#xfail: }" "${problems[0]}" | tee -a "$SUMMARY"
  elif [[ ${#problems[@]} == 0 ]]; then
    echo "$tag: ok (audio rms $rms, ${loud}s/10s audible, music ${mus:-none}, $geo .geo opened)" | tee -a "$SUMMARY"
  else
    FAILS=$((FAILS + 1))
    printf '%s: FAIL: %s\n' "$tag" "$(IFS='; '; echo "${problems[*]}")" | tee -a "$SUMMARY"
  fi
  after=$(save_mtime)
  need_new=0
  [[ ${#problems[@]} != 0 || "$after" == "$before" ]] && need_new=1
done
echo "levels_smoke: $FAILS of ${#LEVELS[@]} failed; review every PNG under $OUT" | tee -a "$SUMMARY"
[[ $FAILS == 0 ]]
