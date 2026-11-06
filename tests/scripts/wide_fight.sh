#!/bin/bash
# Widescreen (16:10) fight HUD + LT lock-on (Render), Aragog arena:
#   health bar (left edge), button cluster + its "Dodge" label (right edge,
#   one piece), enemy bar with portrait (right, one piece), lock-on reticle.
#   frame 1600 == golden wide_fight.png. Once the "Dodge" line swallowed the
#   cluster (a text line merged with the 129-px sprite) and nothing anchored.
# The same input in 4:3 locks on the same target, frame for frame
# (widescreen does not change the game's choice here; one target only).
#
#   tests/scripts/wide_fight.sh [build_dir]     UPDATE_GOLDEN=1: re-take the golden
# ~4 min: levels_smoke.sh row 55 makes the save, then one 16:10 run.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
GOLDEN="$ROOT/tests/golden/wide_fight.png"
OUT=$(env -u HP2_WIDESCREEN "$ROOT/tests/scripts/levels_smoke.sh" "$BUILD" 55 2>&1 | sed -n 's/^levels_smoke: .* out \(.*\)$/\1/p' | head -1)
[[ -d "$OUT/home" ]] || { echo "FAIL: no save profile from levels_smoke"; exit 1; }
RUN="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/wide_fight-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN"
cp -cR "$OUT/home" "$RUN/home"
(
  ulimit -c 0
  export HOME="$RUN/home" HP2_LOG_DIR="$RUN" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy \
         SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_WIDESCREEN=1 HP2_SKIP_MOVIES=1 \
         HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}" \
         HP2_PAD_SCRIPT="$ROOT/tests/scripts/wide_fight.pad" HP2_SHOT_FRAMES=1600 HP2_EXIT_AFTER=1600
  unset HP2_STUB HP2_NO_PACING HP2_WIDE_HUD HP2_RENDER_SCALE
  exec "$BIN" > "$RUN/stdout.log" 2> "$RUN/stderr.log"
)
CODE=$?
echo "exit $CODE, log $RUN/runtime.log"
PNG="$RUN/frame_001600.png"
[[ $CODE == 0 && -f "$PNG" ]] || { echo FAIL; exit 1; }
if [[ "${UPDATE_GOLDEN:-0}" == 1 || ! -f "$GOLDEN" ]]; then
  cp "$PNG" "$GOLDEN"
  echo "  golden updated: $GOLDEN"
elif ! python3 "$ROOT/tests/scripts/png_diff.py" "$PNG" "$GOLDEN"; then
  echo FAIL
  exit 1
fi
echo PASS
