#!/bin/bash
# Front-end screens at 16:10 (Render), Ukrainian:
#  - card album (a 3D book; the page buttons, numbers and text are 2D): nothing
#    on its pages is edge-anchored, whichever button is selected (once the
#    page-number row jumped to the screen edge and back as the ring moved;
#    the spellbook's text ran off both pages);
#  - title at 4x render scale: no seams between the sky/logo tiles (WRAP at
#    the tile edge, and half-pixel tile overlaps blended twice above 1x).
#   tests/scripts/wide_book.sh [build_dir]      UPDATE_GOLDEN=1: re-take goldens
# Goldens: wide_book.png (album, frame 7540), wide_title_4x.png (frame 380).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/out}/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}"
GOLDEN="$ROOT/tests/golden"
FAILS=0
fail() {
  echo "  FAIL: $*"
  FAILS=$((FAILS + 1))
}
RUN="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/wide_book-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN"
python3 "$ROOT/tools/loc/build_mod.py" uk --data "$DATA" --out "$RUN/overlay" > "$RUN/build.log" 2>&1 \
  || { echo "FAIL: overlay build"; exit 1; }
launch() {  # name pad last_frame env...
  local name=$1 pad=$2 last=$3
  shift 3
  mkdir -p "$RUN/$name/home"
  (
    ulimit -c 0
    unset HP2_STUB HP2_NO_PACING HP2_WIDE_HUD HP2_RENDER_SCALE HP2_CALL_AT HP2_HDUI
    exec env HOME="$RUN/$name/home" HP2_LOG_DIR="$RUN/$name" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy \
      SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_WIDESCREEN=1 HP2_DATA="$DATA" HP2_SKIP_MOVIES=1 \
      HP2_LANG=uk HP2_MODS="$RUN/overlay" HP2_PAD_SCRIPT="$pad" HP2_SHOT_FRAMES=$last \
      HP2_EXIT_AFTER=$last "$@" "$BIN" > "$RUN/$name/stdout.log" 2> "$RUN/$name/stderr.log"
  )
  local code=$?
  [[ $code == 0 ]] || fail "$name: exit code $code"
}
check() {  # png golden [x0,y0,x1,y1: compare only that rect]
  if [[ ! -f "$1" ]]; then
    fail "no screenshot $1"
  elif [[ "${UPDATE_GOLDEN:-0}" == 1 || ! -f "$GOLDEN/$2" ]]; then
    cp "$1" "$GOLDEN/$2"
    echo "  golden updated: $GOLDEN/$2"
  else
    PNG_DIFF_RECT="${3:-}" python3 "$ROOT/tests/scripts/png_diff.py" "$1" "$GOLDEN/$2" || fail "$1 differs from $2"
  fi
}
echo "400 press START" > "$RUN/title.pad"
launch title "$RUN/title.pad" 380 HP2_RENDER_SCALE=4 &
launch book "$ROOT/tests/scripts/wide_book.pad" 7540
wait
check "$RUN/title/frame_000380.png" wide_title_4x.png
# The sky behind the book drifts with play time: compare the book only.
check "$RUN/book/frame_007540.png" wide_book.png 120,80,640,440
if [[ $FAILS == 0 ]]; then
  echo "PASS"
  exit 0
fi
echo "FAIL ($FAILS)"
exit 1
