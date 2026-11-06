#!/bin/bash
# The game's own language picker, end to end, as the installed app runs it
# (no HP2_LANG; HP2_MODS = the built overlay; one profile per case, two runs):
#  uk      run 1 (tools/loc/pick_uk.pad): Options -> flags -> RIGHT -> A on the
#          Ukrainian flag; shots 680 picker, 740 UA selected, 900 Options,
#          1010 main menu, 1150 slots, 4780 Burrow subtitle, 5400 Burrow;
#          D:\_UK\text must be opened after the pick;
#          run 2 (same profile, tools/loc/menu_only.pad): _UK after a restart?
#  hybrid  run 1 (tools/loc/pick_hybrid.pad): RIGHT, RIGHT -> A on the split
#          flag (Ukrainian text + English speech); shots 820 picker, 1010 main
#          menu, 4780 Burrow subtitle; D:\_UKT\text and D:\_UKT\SPEECH_BURROWS.SDT
#          opened, and that bank is the disc's English _UK one (cmp);
#          run 2 (same profile): _UKT after a restart?
# The two cases run side by side (~2 min).
#   tests/scripts/loc_pick_persist.sh [build_dir]
# LOC_KERNEL_POKES=1: emulate the Kernel's hybrid row with HP2_CALL_AT pokes
# (docs/handoff/loc.md, "Hybrid slot") for checking the overlay before the
# Kernel side exists; persistence (run 2) still needs the Kernel.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/out}/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}"
RUN="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/loc_pick_persist-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN"
ulimit -c 0
python3 "$ROOT/tools/loc/build_mod.py" uk --data "$DATA" --out "$RUN/overlay" > "$RUN/build.log" 2>&1 \
  || { echo "FAIL: overlay build"; exit 1; }
POKES=""
[[ "${LOC_KERNEL_POKES:-0}" == 1 ]] &&
  POKES="1:poke:20F364:1,1:poke:1F7A90:1,1:poke:1F7A9C:1D8,1:poke:1F7AA0:1E1,1:poke:1887D8:746B75"
run() {  # run <case> <n> <pad> <frames>
  local d="$RUN/$1/$2"
  mkdir -p "$d" "$RUN/$1/home"
  HOME="$RUN/$1/home" HP2_LOG_DIR="$d" HP2_KTRACE=0 HP2_KTRACE_KERNEL=1 SDL_AUDIO_DRIVER=dummy \
    SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_DATA="$DATA" HP2_SKIP_MOVIES=1 HP2_PAD_SCRIPT="$3" \
    HP2_MODS="$RUN/overlay" HP2_SHOT_FRAMES="$4" HP2_EXIT_AFTER="${4##*,}" HP2_CALL_AT="$POKES" \
    env -u HP2_LANG "$BIN" > "$d/stdout.log" 2> "$d/stderr.log"
  echo "$1 run $2: exit $?, $(ls "$d"/frame_*.png 2>/dev/null | wc -l | tr -d ' ') shots" > "$d/result"
}
opened() { grep -qiF "open \\??\\D:\\$2" "$RUN/$1/stderr.log"; }  # opened <case/n> <path>
( run uk 1 "$ROOT/tools/loc/pick_uk.pad" 680,740,900,1010,1150,4780,5400
  run uk 2 "$ROOT/tools/loc/menu_only.pad" 420,600 ) &
( run hybrid 1 "$ROOT/tools/loc/pick_hybrid.pad" 820,1010,4780,5400
  run hybrid 2 "$ROOT/tools/loc/menu_only.pad" 420,600 ) &
wait
FAILS=0
check() {  # check <ok?> <message> <failure message>
  if [[ $1 == 0 ]]; then echo "  $2"; else echo "  FAIL: $3"; FAILS=$((FAILS + 1)); fi
}
cat "$RUN/uk/1/result"
opened uk/1 '_UK\text'; check $? 'D:\_UK\ text opened after the pick' 'never opened D:\_UK\text'
cat "$RUN/uk/2/result"
opened uk/2 '_UK\text'; check $? '_UK (Ukrainian) after restart' '_USA after restart (choice not kept)'
cat "$RUN/hybrid/1/result"
opened hybrid/1 '_UKT\text'; check $? 'D:\_UKT\ text opened after the pick' 'never opened D:\_UKT\text'
opened hybrid/1 '_UKT\SPEECH_BURROWS.SDT'; check $? 'Burrow speech from D:\_UKT\' 'Burrow speech not from _UKT'
cmp -s "$RUN/overlay/_UKT/Speech_Burrows.sdt" "$DATA/game/extracted/_UK/Speech_Burrows.sdt"
check $? '_UKT\Speech_Burrows.sdt = the disc'"'"'s English bank' '_UKT speech is not the English disc bank'
cat "$RUN/hybrid/2/result"
opened hybrid/2 '_UKT\text'; check $? '_UKT (hybrid) after restart' 'not _UKT after restart (choice not kept)'
echo "$RUN"
[[ $FAILS == 0 ]] && echo PASS || { echo "FAIL ($FAILS)"; exit 1; }
