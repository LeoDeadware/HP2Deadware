#!/bin/bash
# Widescreen (16:10) front-end menus (Render): text in the decorative font
# (drop-cap initials drawn as separate, larger glyphs) and the "(A) Select /
# (B) Exit" legends must stay whole: one string = one anchor. Once the
# initial and the rest of the word were anchored apart ("S ... ELECT").
#
#   tests/scripts/wide_menus.sh [build_dir]      UPDATE_GOLDEN=1: re-take goldens
#
# One silent run, English, HP2_WIDESCREEN=1, pad tools/loc/lang_menu.pad
# (main menu -> Options -> language row -> picker):
#   frame 560  Options                 == golden wide_options.png
#   frame 680  language picker         == golden wide_lang_menu.png
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/out}/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
GOLDEN="$ROOT/tests/golden"
FAILS=0
fail() {
  echo "  FAIL: $*"
  FAILS=$((FAILS + 1))
}
RUN="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/wide_menus-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN/home"
(
  ulimit -c 0
  export HOME="$RUN/home" HP2_LOG_DIR="$RUN" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy \
         SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_WIDESCREEN=1 \
         HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}" HP2_SKIP_MOVIES=1 \
         HP2_PAD_SCRIPT="$ROOT/tools/loc/lang_menu.pad" HP2_SHOT_FRAMES=560,680 HP2_EXIT_AFTER=680
  unset HP2_STUB HP2_NO_PACING HP2_WIDE_HUD HP2_RENDER_SCALE HP2_LANG HP2_MODS HP2_CALL_AT
  exec "$BIN" > "$RUN/stdout.log" 2> "$RUN/stderr.log"
)
CODE=$?
echo "exit $CODE, log $RUN/runtime.log"
[[ $CODE == 0 ]] || fail "exit code $CODE"
grep -m1 "^FATAL" "$RUN/runtime.log" && fail "fatal error"
for pair in 000560:wide_options 000680:wide_lang_menu; do
  png="$RUN/frame_${pair%%:*}.png" gold="$GOLDEN/${pair##*:}.png"
  if [[ ! -f "$png" ]]; then
    fail "no screenshot $png"
  elif [[ "${UPDATE_GOLDEN:-0}" == 1 || ! -f "$gold" ]]; then
    cp "$png" "$gold"
    echo "  golden updated: $gold"
  else
    python3 "$ROOT/tests/scripts/png_diff.py" "$png" "$gold" || fail "$png differs from $gold"
  fi
done
if [[ $FAILS == 0 ]]; then
  echo "PASS"
  exit 0
fi
echo "FAIL ($FAILS)"
exit 1
