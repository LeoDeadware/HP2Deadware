#!/bin/bash
# Polish: the HD world texture pack (src/polish/texpack HP2_HDWORLD, tools/hdworld).
#
#   tests/scripts/hdworld_pack.sh [build_dir]          (default: out)
#   UPDATE_GOLDEN=1 tests/scripts/hdworld_pack.sh      (re-take tests/golden/hdworld_*.png)
#
# Needs the pack: tools/hdworld/make_pack.sh (default .cache/HP2Cache/hdworld/pack,
# or HP2_HDWORLD_PACK=dir). Diagon Alley is reached as in levels_smoke.sh (a
# crafted save: New Game -> chapter 1 start map -> save aimed at row 5), then
# two silent runs from copies of that profile, 16:10 (HP2_WIDESCREEN=1) at
# render scale 3 (a window on a 2x panel), without and with the pack, standing
# still on arrival (same view): shots at frames 1500 and 1800. Checks:
#   - the pack was used in Diagon Alley (log "polish: hd_ui <hash>.png", >= 10);
#   - every texture the pack builds as tiling (manifest edges = wrap) still
#     tiles: hdfix seam ratio (wrap seam vs the other texel boundaries)
#     <= max(1.5, 1.6 x the original's) (stone/brick textures whose edge runs
#     along a mortar line have a high ratio in the original already; the
#     worst, 14dc3daedd63b63e at 1.42x, tiles 2x2 without a visible seam);
#   - per crop: changed (mean |diff| > 0.3) and nothing moved (8x shrunk
#     crops differ by mean < 4); after == tests/golden/hdworld_<name>_after.png
#     within png_diff tolerance (crops kept as _before/_after goldens);
#   - frame time: the D3D8 "busy" ms (CPU+GPU work per frame, 120-frame
#     averages from frame 1200 on) with the pack <= without x 1.15 + 0.3 ms.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/tests/scripts"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
PACK="${HP2_HDWORLD_PACK:-${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/hdworld/pack}"
GOLDEN="$ROOT/tests/golden"
DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
[[ -f "$PACK/manifest.tsv" ]] || { echo "FAIL: no pack $PACK (tools/hdworld/make_pack.sh)"; exit 2; }
FAILS=0
fail() { echo "  FAIL: $*"; FAILS=$((FAILS + 1)); }
R="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/hdworld_pack-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$R/home"
echo "runs in $R"

# run <dir> <home> <pad text> [env...]
run() {
  local d=$1 home=$2 pad=$3
  shift 3
  mkdir -p "$d"
  printf "$pad" > "$d/pad.txt"
  (
    ulimit -c 0
    export HOME="$home" HP2_LOG_DIR="$d" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy \
           HP2_KTRACE_KERNEL=1 HP2_BACKGROUND=1 HP2_SKIP_MOVIES=1 HP2_DATA="$DATA" HP2_PAD_SCRIPT="$d/pad.txt" HP2_HDUI=0
    unset HP2_LOOK HP2_STUB HP2_NO_PACING HP2_HDWORLD
    exec env "$@" "$BIN" > "$d/stdout.log" 2> "$d/stderr.log"
  )
  echo $? > "$d/exit"
}
# save point at frame $3 writing slot 0 aimed at map row $1, chapter $2 (levels_smoke.sh)
save_call() {
  local f=$3
  echo "$f:4C140/0,$f:copy:529C10:527B78:2064,$f:poke:529C10:$(printf %X $1),$f:poke:529C14:$(printf %X $2),$f:poke:529C18:$(printf %X $((1 << $2))),$f:poke:529BE0:1,$f:39A30/1E8058@1E7F9C"
}
run "$R/newgame" "$R/home" '0 disconnect\n1 connect\n450 press A\n600 press A\n7060 press A\n' \
  HP2_CALL_AT="$(save_call 4 1 7000)" HP2_EXIT_AFTER=7300
run "$R/hop" "$R/home" '0 disconnect\n1 connect\n450 press A\n600 press A\n2560 press A\n' \
  HP2_CALL_AT="$(save_call 5 1 2500)" HP2_EXIT_AFTER=2800
for v in newgame hop; do [[ $(cat "$R/$v/exit") == 0 ]] || { echo "FAIL: $v exit $(cat "$R/$v/exit")"; exit 1; }; done
for v in before after; do cp -c -R "$R/home" "$R/home_$v"; done
look() {  # <name> <pack>
  run "$R/$1" "$R/home_$1" '0 disconnect\n1 connect\n450 press A\n600 press A\n' \
    HP2_WIDESCREEN=1 HP2_RENDER_SCALE=3 HP2_HDWORLD="$2" HP2_SHOT_FRAMES=1500,1800 HP2_EXIT_AFTER=2400
}
look before 0 & look after "$PACK" & wait
for v in before after; do
  c=$(cat "$R/$v/exit")
  echo "$v: exit $c"
  [[ $c == 0 ]] || fail "$v exit $c"
  grep -Eiq "open .*diagonally\.geo" "$R/$v/stderr.log" || fail "$v: Diagon Alley not loaded"
done
n=$(grep -c "polish: hd_ui [0-9a-f]\{16\}\.png " "$R/after/runtime.log")
echo "pack textures used: $n"
[[ $n -ge 10 ]] || fail "only $n pack textures used"
grep -h "polish: hd pack:" "$R/after/runtime.log" | tail -1
# seams: tiling textures still tile after the upscale
HDFIX="$R/hdfix"
swiftc -O -o "$HDFIX" "$ROOT/tools/hdui/hdfix.swift" || fail "hdfix build"
DUMP="${HP2_HDWORLD_DUMP:-${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/hdworld/dump}"
nseam=0
while IFS=$'\t' read -r h w hh k edges _; do
  [[ "$h" == \#* || "$edges" != wrap ]] && continue
  after=$("$HDFIX" seam "$PACK/$h.png" "$k")
  orig=$([[ -f "$DUMP/$h.png" ]] && "$HDFIX" seam "$DUMP/$h.png" || echo 1)
  nseam=$((nseam + 1))
  awk -v a="$after" -v o="$orig" 'BEGIN{m = 1.6 * o; if (m < 1.5) m = 1.5; exit !(a <= m)}' ||
    fail "seam in $h: ratio $after (original $orig)"
done < "$PACK/manifest.tsv"
echo "seams: $nseam tiling textures checked"
# frame time
busy() { grep -Eo "frame [0-9]+: .*busy [0-9.]+" "$1" | awk '{f=$2+0; b=$NF} f>=1200 {s+=b; n++} END{printf "%.2f", n ? s/n : 0}'; }
tb=$(busy "$R/before/runtime.log") ta=$(busy "$R/after/runtime.log")
echo "frame time (busy, avg from frame 1200): without $tb ms, with $ta ms"
awk -v a="$ta" -v b="$tb" 'BEGIN{exit !(a <= b * 1.15 + 0.3)}' || fail "frame time $ta ms vs $tb ms"
# crops at 2304x1440: name:frame:w:h:y:x
python3 "$HERE/hdui_check.py" "$R" "$GOLDEN" "${UPDATE_GOLDEN:-0}" --prefix hdworld \
  ${HDWORLD_CROPS:-street:001500:1100:700:200:600 street2:001800:1100:700:200:600} || fail "crop checks"
if [[ $FAILS == 0 ]]; then echo "PASS"; exit 0; fi
echo "FAIL ($FAILS)"; exit 1
