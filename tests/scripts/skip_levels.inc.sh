# Parts 3 and 4 of skip_cutscenes.sh (sourced): a level cutscene beyond the
# Burrow, short press vs 4 s hold, and an NPC dialogue that A starts.
# Uses start / finish / fail / check_shot / $OUT / $ROOT of skip_cutscenes.sh.
#
# Levels are reached as in levels_smoke.sh: the game's save point writes slot 0
# with a poked map row / chapter, Continue -> Potter A loads it.
#
# Which cutscenes a crafted save reaches (2026-10-31): a save loaded at Borgin
# & Burkes (chapter 1's start map) plays the chapter 1 arrival cutscene (Borgin
# and the Malfoys). A save at any other map plays none (the CS_*.geo a level
# opens are preloads, not playback), also not at the Gryffindor dormitory
# (start map of chapters 2..11), not even after the game's chapter start
# FUN_0004ADB0(chapter) before saving: the chapter openings (CS_DY*, CS_NIT*),
# Diagon Alley's entry (CS_LVDA_*), Quidditch (CS_QU) and the ending
# (CS_END01_*) are started by level triggers (FUN_00070C90(script, player) from
# 0x541E0 / 0x54130 / 0x542A0) when the story gets there. The game has no FMV
# beyond the logos and the intro.
#
# 3. Borgin & Burkes, three runs from one save (arrival script starts at 784,
#    ends at 1283 unskipped):
#    full  HP2_SKIP=0, no input                    -> reference
#    short HP2_SKIP=1, A pressed at 800 (6 frames) -> must NOT skip: ends like full
#    hold  HP2_SKIP=1, A held 800..1400            -> fast-forwarded after 4 s
#          (~1040), story state block 0x527B78 == full's; saves the Leaky Cauldron.
# 4. Leaky Cauldron (the hold run's save): walk to Hagrid at the bar, "Talk To"
#    at ~1490, A at 1495 -> his dialogue plays (subtitle, frame 1600 == golden);
#    HP2_SKIP=1, so the talk press goes through the skip code.
SAVE_AT=2500 STATE=527B78 STATE_LEN=2064
# State block bytes that change on their own while playing (offsets into
# 0x527B78, hex): +0xEC play time in frames.
VOLATILE="${VOLATILE:-EC ED EE EF}"
LPROF="$OUT/levels/home"
mkdir -p "$LPROF"

save_call() {  # <row> <chapter> <frame>
  local f=$3
  echo "$f:4C140/0,$f:copy:529C10:527B78:2064,$f:poke:529C10:$(printf %X $1),$f:poke:529C14:$(printf %X $2),$f:poke:529C18:$(printf %X $((1 << $2))),$f:poke:529BE0:1,$f:39A30/1E8058@1E7F9C"
}
save_mtime() { find "$LPROF" -name 'POTTER A' -type f -exec stat -f %m {} \; 2>/dev/null | head -1; }

# lstart <dir> <home> <pad text> [env...]: a run on a profile (pad text: printf format)
lstart() {
  local dir=$1 home=$2 pad=$3
  shift 3
  mkdir -p "$dir"
  printf "$pad" > "$dir/pad.txt"
  python3 "$ROOT/tools/guard.py" > /dev/null 2>&1 || sleep 30
  PROFILE_HOME="$home" start "$dir" HP2_SKIP_MOVIES=1 "$@"
}
clone() {  # <dir>: a copy of the chained profile for one run
  mkdir -p "$1"
  cp -c -R "$LPROF" "$1/home"
}

# script_ends <log>: "start end ff|-" per script of a run
script_ends() {
  awk '/polish: frame [0-9]+: script started/ {s = $3 + 0}
       /polish: frame [0-9]+: script ended/ {print s, $3 + 0, ($0 ~ /fast-forwarded/ ? "ff" : "-")}' "$1"
}

state_diff() {  # <a.bin> <b.bin>: differing offsets (hex) not in VOLATILE
  python3 - "$1" "$2" "$VOLATILE" <<'PY'
import sys
a, b = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
skip = {int(x, 16) for x in sys.argv[3].split()}
print(' '.join(f'{i:X}:{a[i]:02X}/{b[i]:02X}' for i in range(min(len(a), len(b))) if a[i] != b[i] and i not in skip))
PY
}

L="$OUT/levels"
echo "3. Borgin & Burkes: short A press vs A held 4 s"
mkdir -p "$L/newgame"
printf '0 disconnect\n1 connect\n450 press A\n600 press A\n7060 press A\n' > "$L/newgame/pad.txt"
PROFILE_HOME="$LPROF" start "$L/newgame" HP2_SKIP=0 HP2_SKIP_MOVIES=1 \
  HP2_CALL_AT="$(save_call 4 1 7000)" HP2_EXIT_AFTER=7300
finish "$L/newgame" 200
LOAD='0 disconnect\n1 connect\n450 press A\n600 press A\n'
clone "$L/full"
clone "$L/short"
lstart "$L/full" "$L/full/home" "$LOAD" HP2_SKIP=0 HP2_DUMP_AT=2450:$STATE:$STATE_LEN \
  HP2_SHOT_FRAMES=1500 HP2_EXIT_AFTER=2500
lstart "$L/short" "$L/short/home" "${LOAD}800 press A\n" HP2_SKIP=1 HP2_EXIT_AFTER=1500
finish "$L/full" 200
finish "$L/short" 200
before=$(save_mtime)
lstart "$L/hold" "$LPROF" "${LOAD}800 hold A 600\n$((SAVE_AT + 60)) press A\n" HP2_SKIP=1 \
  HP2_DUMP_AT=2450:$STATE:$STATE_LEN HP2_SHOT_FRAMES=1500 \
  HP2_CALL_AT="$(save_call 8 1 $SAVE_AT)" HP2_EXIT_AFTER=$((SAVE_AT + 300))
finish "$L/hold" 200
for r in full short hold; do echo "    $r: $(script_ends "$L/$r/runtime.log" | tr '\n' ' ')"; done
grep -h "skip: held\|fast-forward (" "$L/hold/runtime.log" | sed 's/^/    hold: /'
read -r fs fe _ <<< "$(script_ends "$L/full/runtime.log" | head -1)"
read -r ss se sf <<< "$(script_ends "$L/short/runtime.log" | head -1)"
read -r hs he hf <<< "$(script_ends "$L/hold/runtime.log" | head -1)"
if [[ -z "${fe:-}" || -z "${se:-}" || -z "${he:-}" ]]; then
  fail "Borgin: the arrival script did not run to an end in every run"
else
  [[ $sf == - ]] && (( se >= fe - 10 )) || fail "Borgin: a short A press skipped (ended $se $sf, full $fe)"
  [[ $hf == ff ]] && (( he < fe - 100 && he - hs >= 200 )) ||
    fail "Borgin: 4 s hold: ended $he $hf (started $hs, full $fe; want ff after ~240 frames)"
fi
sdump=$(ls "$L/hold"/dump_*.bin 2>/dev/null | head -1)
fdump=$(ls "$L/full"/dump_*.bin 2>/dev/null | head -1)
if [[ -z "$sdump" || -z "$fdump" ]]; then
  fail "Borgin: no state dump"
else
  diffs=$(state_diff "$sdump" "$fdump")
  echo "    state block hold vs full: ${diffs:-identical}"
  [[ -z "$diffs" ]] || fail "Borgin: story state differs after the skip (offset:hold/full) $diffs"
fi
# (Frame 1500 of both runs: the same shop, Harry in gameplay; idle poses differ
# with the end frame, so the shot is only checked for blank / overlay.)
python3 "$ROOT/tests/scripts/shot_check.py" "$L/hold/frame_001500.png" > /dev/null ||
  fail "Borgin: blank frame 1500 after the skip"
python3 "$ROOT/tests/scripts/overlay_check.py" "$L/hold/frame_001500.png" > /dev/null ||
  fail "Borgin: reconnect overlay after the skip"
[[ "$(save_mtime)" != "$before" ]] || fail "Borgin: the hold run did not save the Leaky Cauldron"

echo "4. Leaky Cauldron: A at Hagrid's 'Talk To' starts his dialogue"
lstart "$L/hagrid" "$LPROF" "${LOAD}1300 stick L 0 1 185\n1495 press A\n" HP2_SKIP=1 \
  HP2_SHOT_FRAMES=1490,1600 HP2_EXIT_AFTER=1650
finish "$L/hagrid" 150
check_shot "$L/hagrid/frame_001600.png" skip_hagrid_talk.png
