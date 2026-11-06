#!/bin/bash
# Skipping movies and cutscenes (Polish, src/polish/skip.cpp; HP2_SKIP=0 = off).
# Scripted / silent runs have it off by default; both runs here set HP2_SKIP=1.
#
#   tests/scripts/skip_cutscenes.sh [build_dir]       (default: out)
#   UPDATE_GOLDEN=1 tests/scripts/skip_cutscenes.sh   (re-take tests/golden/skip_*.png)
#
# The rules: movies skip on a single A / START press; in-game cutscenes and
# voice lines need A or START held 4 s (a short press never skips them).
# Silent paced runs (.cache/HP2Cache/runs/skip_cutscenes-*), at most 2 at a time:
#  1. boot movies (no HP2_SKIP_MOVIES; rule: a single press of A or START skips
#     a movie): the pad script counts presented D3D frames, which stand still
#     while a movie plays (overlay only), so the state of the frame a movie
#     starts on is held through it. ea_logo_english.xmv starts at ~26,
#     warner.xmv (unskippable in the original: PlayMovie(path, 0)) and hpengx.xmv
#     at ~327-331 (+-1 between runs): frames 20..40 and 300..360 alternate A
#     (even) and START (odd), one frame each, so whichever frame a movie freezes
#     on has a fresh single press. Checks: every movie ends in < 1 s (3.2 s,
#     6.5 s, 73 s unskipped), "made skippable" logged, frame 600 (the title
#     menu) reached in < 25 s wall from the start (unskipped: ~83 s of movies).
#  2. cutscenes: New Game (phase3_new_game.pad) with A held 1900..2500 (the Burrow
#     intro cutscene; then Ron's de-gnoming dialogue). Unskipped they end at
#     ~2450 and ~4580 (phase3_first_level.sh). Checks: both scripts end
#     "fast-forwarded", the first ~4 s after 1900, the second ~4 s after its
#     start, both before 2450; frame 2700 is gameplay in the yard (Fred and
#     George on the gate, the X/Y/A/B tutorial panel) == golden, no reconnect overlay.
#  3. Borgin & Burkes (crafted save): a short A press does not skip, a 4 s hold
#     does, story state == the full run's (skip_levels.inc.sh).
#  4. Leaky Cauldron: A on Hagrid's "Talk To" plays his dialogue (skip_levels.inc.sh).
#  All runs: exit 0, no FATAL.
#
#  PARTS="movies burrow levels" (default all) picks.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN (cmake --build $BUILD --target hp2)"; exit 2; }
GOLDEN="$ROOT/tests/golden"
OUT="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/skip_cutscenes-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$OUT/movies/home" "$OUT/cutscenes/home"
PARTS="${PARTS:-movies burrow levels}"
part() { [[ " $PARTS " == *" $1 "* ]]; }
FAILS=0
fail() {
  echo "  FAIL: $*"
  FAILS=$((FAILS + 1))
}

# start <dir> [env...]: one silent run in the background (pid in $dir/pid);
# HOME is $dir/home unless PROFILE_HOME names a shared profile.
start() {
  local dir=$1
  shift
  (
    ulimit -c 0
    export HOME="${PROFILE_HOME:-$dir/home}" HP2_LOG_DIR="$dir" HP2_KTRACE=0 \
           SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 \
           HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}" HP2_PAD_SCRIPT="$dir/pad.txt" HP2_AUDIO_WAV="$dir/mix.wav"
    unset HP2_STUB HP2_NO_PACING HP2_TEST_CLOCK HP2_SKIP
    exec env HP2_SKIP=1 "$@" "$BIN" > "$dir/stdout.log" 2> "$dir/stderr.log"
  ) &
  echo $! > "$dir/pid"
}

# finish <dir> <timeout s>
finish() {
  local dir=$1 secs=$2 pid
  pid=$(cat "$dir/pid")
  for _ in $(seq 1 $((secs * 2))); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.5
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid"
    fail "$dir: timeout after ${secs}s"
  fi
  wait "$pid"
  local code=$?
  echo "  $(basename "$dir"): exit $code, log $dir/runtime.log"
  [[ $code == 0 ]] || fail "$dir: exit code $code"
  grep -h -m1 -A3 "FATAL" "$dir/runtime.log" "$dir/stderr.log" 2>/dev/null && fail "$dir: fatal error"
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

# movie_secs <log> <name>: wall seconds of the movie played after "playing ...<name>"
movie_secs() {
  awk -v n="$2" 'index($0, "movies: playing") && index($0, n) {f=1; next}
                 f && /movies: [0-9.]+ s wall/ {print $2; exit}' "$1"
}

if part movies; then
for f in $(seq 20 40) $(seq 300 360); do
  if ((f % 2)); then echo "$f press START 1"; else echo "$f press A 1"; fi
done > "$OUT/movies/pad.txt"
MOVIES_T0=$(date +%s)
start "$OUT/movies" HP2_EXIT_AFTER=600

fi
if part burrow; then
awk '!/^#/ && $1 + 0 >= 1900 && !done {print "1900 hold A 600"; done = 1} {print}' \
  "$ROOT/tests/scripts/phase3_new_game.pad" > "$OUT/cutscenes/pad.txt"
start "$OUT/cutscenes" HP2_SKIP_MOVIES=1 HP2_SHOT_FRAMES=2700 HP2_EXIT_AFTER=2700

fi
if part movies; then
echo "1. boot movies: single A / START presses at frames 20..40, 300..360"
finish "$OUT/movies" 150
MOVIES_SECS=$(( $(date +%s) - MOVIES_T0 ))
LOG="$OUT/movies/runtime.log"
grep -h "movies:\|in between" "$LOG" | sed 's/^/    /'
for m in ea_logo_english.xmv warner.xmv hpengx.xmv; do
  s=$(movie_secs "$LOG" "$m")
  if [[ -z "$s" ]]; then
    fail "$m was not played to an end"
  elif ! awk -v s="$s" 'BEGIN {exit !(s < 1.0)}'; then
    fail "$m ran ${s}s (not skipped by a single press)"
  fi
done
grep -q "polish: movie made skippable" "$LOG" || fail "no 'movie made skippable' in the log"
echo "    title menu (frame 600) after ${MOVIES_SECS}s wall"
(( MOVIES_SECS < 25 )) || fail "title menu after ${MOVIES_SECS}s (want < 25 s: movies skipped)"

fi
if part burrow; then
echo "2. cutscenes: A held 1900..2500"
finish "$OUT/cutscenes" 200
LOG="$OUT/cutscenes/runtime.log"
grep -h "polish:" "$LOG" | sed 's/^/    /'
ENDS=$(grep -c "script ended (fast-forwarded)" "$LOG")
[[ $ENDS -ge 2 ]] || fail "$ENDS fast-forwarded script ends, want 2"
LATE=$(awk '/polish: frame [0-9]+: script ended/ {sub(":", "", $3); if ($3 + 0 >= 2450 || $3 + 0 < 2100) print $3}' "$LOG")
[[ -z "$LATE" ]] || fail "scripts ended at $LATE (want after the 4 s hold, before 2450)"
check_shot "$OUT/cutscenes/frame_002700.png" skip_cutscene_gameplay.png
python3 "$ROOT/tests/scripts/overlay_check.py" "$OUT/cutscenes/frame_002700.png" > /dev/null ||
  fail "reconnect overlay after the skipped cutscene"

fi

part levels && source "$ROOT/tests/scripts/skip_levels.inc.sh"

if [[ $FAILS == 0 ]]; then
  echo "PASS"
  exit 0
fi
echo "FAIL ($FAILS)"
exit 1
