#!/bin/bash
# Languages (Kernel HLE): the port ships English (US) and Ukrainian only.
#  - HP2_LANG, else "language=<key>" in the profile's hp2.ini, else en; keys en
#    (_USA), uk (_UK slot) and uk_text (_UKT: Ukrainian text + English speech,
#    only when the overlay has _UKT, else uk); anything else -> warning, en.
#  - $HP2_DATA/mods/uk (the translation overlay: _UK text + glyphs) is mounted
#    whenever it exists, so the game's own picker can switch to it.
#  - The game's picker (Options -> Language Selection: flags; table 0x20F348)
#    lists only US + the _UK row (Ukrainian flag from the overlay): the kernel
#    zeroes the FRA/GER/SPA/ITA masks before the game starts (misc.cpp); with
#    _UKT in the overlay the FRA row becomes the uk_text slot (3 flags).
#
#   tests/scripts/language.sh [build_dir]       (default: out)
#   UPDATE_GOLDEN=1 tests/scripts/language.sh   (re-take tests/golden/language_*.png)
#   LOC_ROOT=<checkout> ...                     (where tools/loc + mods/loc/uk are;
#                                                default: this checkout; the
#                                                goldens are of Localizer's
#                                                overlay: Ukrainian flag, menu)
#
# A scratch data root (.cache/HP2Cache/runs/language-*/root: game -> the real
# game folder, symlink; mods/uk built by tools/loc/build_mod.py). Silent paced runs:
#  1. picker: no HP2_LANG; mods/uk auto-mounted; picker masks rows 2..5 == 0
#     (memory dump); picker frames 680 (US in front, small UA flag) and 740
#     (after RIGHT: UA in front) == goldens; 800 (RIGHT again: US in front:
#     a list of exactly 2) == golden.
#  2. pick Ukrainian (RIGHT, A): the game opens D:\_UK\text*.bin from mods/uk;
#     the Options screen (frame 1000) is Ukrainian == golden; the profile's
#     hp2.ini now says language=uk and a restart on it boots in _UK.
#  3. HP2_LANG=uk opens D:\_UK\ at boot; hp2.ini language=fr -> warning, D:\_USA\;
#     language=uk_text without _UKT -> warning, D:\_UK\ (with _UKT: D:\_UKT\).
# The 2-flag goldens are checked only when the overlay has no _UKT
# (tools/loc/loc_pick_persist.sh covers the 3-flag picker).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN (cmake --build $BUILD --target hp2)"; exit 2; }
LOC="${LOC_ROOT:-$ROOT}"
GAME="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}"
GOLDEN="$ROOT/tests/golden"
OUT="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/language-$(date +%Y%m%d-%H%M%S)-$$"
FAILS=0
fail() {
  echo "  FAIL: $*"
  FAILS=$((FAILS + 1))
}

mkdir -p "$OUT/root/mods"
ln -s "$GAME/game" "$OUT/root/game"
python3 "$LOC/tools/loc/build_mod.py" uk --data "$GAME" --out "$OUT/root/mods/uk" \
  > "$OUT/build_mod.log" 2>&1 || { echo "FAIL: overlay build ($OUT/build_mod.log)"; exit 1; }
HYBRID=0
[[ -f "$OUT/root/mods/uk/_UKT/text.bin" ]] && HYBRID=1

# run <name> <pad lines> [env...]: scratch profile, kernel trace of opens.
run() {
  local name=$1 pad=$2
  shift 2
  local dir="$OUT/$name"
  mkdir -p "$dir/home"
  printf '0 disconnect\n1 connect\n%b' "$pad" > "$dir/pad.txt"
  (
    ulimit -c 0
    unset HP2_LANG HP2_MODS HP2_STUB HP2_NO_PACING HP2_PROFILE HP2_TEST_CLOCK
    export HOME="$dir/home" HP2_LOG_DIR="$dir" HP2_KTRACE=0 HP2_KTRACE_KERNEL=1 \
           SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_SKIP_MOVIES=1 \
           HP2_DATA="$OUT/root" HP2_PAD_SCRIPT="$dir/pad.txt"
    exec perl -e 'alarm 200; exec @ARGV' env "$@" "$BIN" > "$dir/stdout.log" 2> "$dir/stderr.log"
  )
  CODE=$?
  LOG="$dir/stderr.log"
  echo "  $name: exit $CODE, $(grep -m1 -E '\] language (en|uk)' "$LOG" | sed 's/^\[krnl\] //')"
  [[ $CODE == 0 ]] || fail "$name: exit code $CODE"
}

check_shot() {  # <png> <golden> [rect]
  local png=$1 gold="$GOLDEN/$2"
  if [[ ! -f "$png" ]]; then
    fail "no screenshot $png"
  elif [[ "${UPDATE_GOLDEN:-0}" == 1 ]]; then
    cp "$png" "$gold"
    echo "  golden updated: $gold"
  else
    PNG_DIFF_RECT="${3:-}" python3 "$ROOT/tests/scripts/png_diff.py" "$png" "$gold" ||
      fail "$png differs from $gold"
  fi
}

# Main menu -> Options (2nd item) -> first row (Language Selection) -> picker.
TO_PICKER='450 press DOWN\n480 press A\n580 press UP\n600 press A\n'

echo "1. picker: English (US) + Ukrainian only$([[ $HYBRID == 1 ]] && echo ' + Ukrainian text/English speech (_UKT)')"
run picker "${TO_PICKER}700 press RIGHT\n760 press RIGHT\n" \
  HP2_DUMP_AT=5:20F348:48 HP2_SHOT_FRAMES=680,740,800 HP2_EXIT_AFTER=800
grep -q "mods dir(s) $OUT/root/mods/uk" "$LOG" || fail "mods/uk not mounted without HP2_LANG"
grep -q 'language picker: English (US) + Ukrainian' "$LOG" || fail "picker masks not set"
python3 - "$OUT/picker" "$HYBRID" <<'EOF' || FAILS=$((FAILS + 1))
import glob, struct, sys
m = glob.glob(sys.argv[1] + "/dump_0020F348_*.bin")
if not m:
    print("  FAIL: no dump of the picker table"); sys.exit(1)
rows = [struct.unpack_from("<3I", open(m[0], "rb").read(), 12 * k) for k in range(6)]
listed = [k for k, (_, mask, _) in enumerate(rows) if mask & 0x3F]
print(f"    picker rows listed: {listed} (0 = USA, 1 = UK slot, 2 = _UKT)")
sys.exit(0 if listed == ([0, 1, 2] if sys.argv[2] == "1" else [0, 1]) else 1)
EOF
if [[ $HYBRID == 0 ]]; then
  check_shot "$OUT/picker/frame_000680.png" language_picker_us.png
  check_shot "$OUT/picker/frame_000740.png" language_picker_uk.png
  check_shot "$OUT/picker/frame_000800.png" language_picker_wrap.png
fi

echo "2. pick Ukrainian in the game's picker"
run pick_uk "${TO_PICKER}700 press RIGHT\n780 press A\n" HP2_SHOT_FRAMES=1000 HP2_EXIT_AFTER=1000
grep -qi "open \\\\??\\\\D:\\\\_uk\\\\text.bin .*$OUT/root/mods/uk/_UK/text.bin" "$LOG" ||
  fail "D:\\_UK\\text.bin not served from mods/uk"
# Menu text and labels (the background clouds drift).
check_shot "$OUT/pick_uk/frame_001000.png" language_uk_options.png 0,60,640,440

# The choice is kept: hp2.ini of that profile says uk; a restart on it boots in _UK.
INI="$OUT/pick_uk/home/Library/Application Support/HP2Port/default/hp2.ini"
grep -qx 'language=uk' "$INI" 2>/dev/null || fail "the pick was not saved to hp2.ini"
mkdir -p "$OUT/restart"
cp -R "$OUT/pick_uk/home" "$OUT/restart/home"
run restart "" HP2_EXIT_AFTER=400
grep -qi 'open \\??\\D:\\_uk\\' "$LOG" || fail "restart after the pick: D:\\_UK\\ not opened at boot"

echo "3. HP2_LANG=uk at boot; hp2.ini language=fr is not offered"
run env_uk "" HP2_LANG=uk HP2_EXIT_AFTER=400
grep -qi 'open \\??\\D:\\_uk\\' "$LOG" || fail "HP2_LANG=uk: the game did not open D:\\_UK\\"
mkdir -p "$OUT/ini_fr/home/Library/Application Support/HP2Port/default"
echo "language=fr" > "$OUT/ini_fr/home/Library/Application Support/HP2Port/default/hp2.ini"
run ini_fr "" HP2_EXIT_AFTER=400
grep -q "unknown language 'fr'" "$LOG" || fail "language=fr: no warning"
grep -qi 'open \\??\\D:\\_usa\\' "$LOG" || fail "language=fr: the game did not fall back to D:\\_USA\\"
mkdir -p "$OUT/ini_uk_text/home/Library/Application Support/HP2Port/default"
echo "language=uk_text" > "$OUT/ini_uk_text/home/Library/Application Support/HP2Port/default/hp2.ini"
run ini_uk_text "" HP2_EXIT_AFTER=400
if [[ $HYBRID == 1 ]]; then
  grep -qi 'open \\??\\D:\\_ukt\\' "$LOG" || fail "language=uk_text: the game did not open D:\\_UKT\\"
else
  grep -q "no _UKT" "$LOG" || fail "language=uk_text without _UKT: no warning"
  grep -qi 'open \\??\\D:\\_uk\\' "$LOG" || fail "language=uk_text without _UKT: D:\\_UK\\ not opened"
fi

if [[ $FAILS == 0 ]]; then
  echo "PASS"
  exit 0
fi
echo "FAIL ($FAILS)"
exit 1
