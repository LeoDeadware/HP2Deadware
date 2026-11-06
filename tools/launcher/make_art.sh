#!/bin/bash
# The launcher window's art: the game's own title (logo over the blue sky) shot from the player's disc
# by a silent run at render scale 4, 16:10, and cropped above the menu (New Game / Options and the
# copyright line are cut off); never committed or shipped:
#   make_art.sh <game binary> <data dir (game/extracted inside)> <out.png>
# The title is up with the movies skipped from frame ~420; frame 500 is shot.
set -uo pipefail
BIN=$1 DATA=$2 OUT=$3
T="$(mktemp -d "${TMPDIR:-/tmp}/hp2_art.XXXXXX")"; trap 'rm -rf "$T"' EXIT
mkdir -p "$T/home"
( ulimit -c 0
  env HOME="$T/home" HP2_LOG_DIR="$T" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 \
    HP2_DATA="$DATA" HP2_MODS="$DATA/mods" HP2_LANG=en HP2_SKIP_MOVIES=1 HP2_WIDESCREEN=1 HP2_RENDER_SCALE=4 \
    HP2_SHOT_DIR="$T" HP2_SHOT_FRAMES=500 HP2_EXIT_AFTER=510 nice -n 10 "$BIN" > "$T/out.log" 2>&1 ) &
pid=$!; t=0
while kill -0 $pid 2>/dev/null && (( t < 240 )); do sleep 0.5; t=$((t + 1)); done
kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
f="$T/frame_000500.png"
[[ -f "$f" ]] || { echo "make_art: no title shot ($(tail -1 "$T/out.log"))"; exit 1; }
HERE="$(cd "$(dirname "$0")" && pwd)"   # repo tools/launcher, or the DMG app's Resources/build
if [[ -x "$HERE/crop_top" ]]; then "$HERE/crop_top" "$f" "$OUT" 0.62; else swift "$HERE/crop_top.swift" "$f" "$OUT" 0.62; fi &&
  echo "art -> $OUT"
