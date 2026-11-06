#!/bin/bash
# Polish: the HD UI texture pack (src/polish/texpack, tools/hdui).
#
#   tests/scripts/hdui_pack.sh [build_dir]          (default: out)
#   UPDATE_GOLDEN=1 tests/scripts/hdui_pack.sh      (re-take tests/golden/hdui_*.png)
#
# Needs the pack: tools/hdui/make_pack.sh (default .cache/HP2Cache/hdui/pack, or
# HP2_HDUI_PACK=dir) and the Ukrainian overlay it builds (.cache/HP2Cache/hdui/uk).
# Two silent runs of tools/loc/test_page.pad in Ukrainian, 16:10
# (HP2_WIDESCREEN=1) at render scale 3 (a window on a 2x panel), without and
# with the pack; the same frames: 600 slots, 6000 level 1 HUD, 6460 pause,
# 6660 inventory. Checks, per crop (the UI there):
#   - the pack was used (log: "polish: hd_ui <hash> ... -> ...", >= 20 textures);
#   - the crop changed (mean |diff| > 0.3: sharper art/text);
#   - nothing moved: both crops shrunk 8x (box) differ by mean < 4 (a shifted
#     button or line of text differs far more);
#   - after == tests/golden/hdui_<name>_after.png (png_diff tolerance);
#     before/after crops kept as tests/golden/hdui_<name>_{before,after}.png.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
PACK="${HP2_HDUI_PACK:-${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/hdui/pack}"
UK="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/hdui/uk"
GOLDEN="$ROOT/tests/golden"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
[[ -d "$PACK" && -d "$UK" ]] || { echo "FAIL: no pack $PACK or overlay $UK (tools/hdui/make_pack.sh)"; exit 2; }
FAILS=0
fail() { echo "  FAIL: $*"; FAILS=$((FAILS + 1)); }
R="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/hdui_pack-$(date +%Y%m%d-%H%M%S)-$$"
run() {
  local d="$R/$1"
  mkdir -p "$d/home"
  (
    ulimit -c 0
    export HOME="$d/home" HP2_LOG_DIR="$d" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy \
           HP2_BACKGROUND=1 HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}" HP2_MODS="$UK" \
           HP2_SKIP_MOVIES=1 HP2_PAD_SCRIPT="$ROOT/tools/loc/test_page.pad" HP2_LANG=uk HP2_WIDESCREEN=1 \
           HP2_RENDER_SCALE=3 HP2_HDUI="$2" HP2_SHOT_FRAMES=600,6000,6460,6660 HP2_EXIT_AFTER=6700
    unset HP2_LOOK HP2_STUB HP2_NO_PACING
    exec "$BIN" > "$d/stdout.log" 2>&1
  )
  echo $? > "$d/exit"
}
echo "runs in $R"
run before 0 & run after "$PACK" & wait
for v in before after; do
  c=$(cat "$R/$v/exit")
  echo "$v: exit $c"
  [[ $c == 0 ]] || fail "$v exit $c"
done
n=$(grep -c "polish: hd_ui [0-9a-f]\{16\}\.png " "$R/after/runtime.log")
echo "pack textures used: $n"
[[ $n -ge 20 ]] || fail "only $n pack textures used"
# name frame crop(w h y x) at 2304x1440
python3 "$ROOT/tests/scripts/hdui_check.py" "$R" "$GOLDEN" "${UPDATE_GOLDEN:-0}" \
  hud:006000:760:520:0:1544 pause:006460:760:480:560:520 slots:000600:900:560:750:400 \
  inventory:006660:900:560:60:1300 || fail "crop checks"
if [[ $FAILS == 0 ]]; then echo "PASS"; exit 0; fi
echo "FAIL ($FAILS)"; exit 1
