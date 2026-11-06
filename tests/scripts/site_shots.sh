#!/bin/bash
# Site screenshots (owner 2026-11-06, Deadware site): 16:10, render scale 4 (3072x1920 PNG), no menus,
# UI language = page language. Scenes: burrow (The Burrow), knockturn (Borgin & Burkes -> Knockturn
# Alley, and Diagon Alley), spiders (Aragog arena, LT lock-on: the "Dodge" prompt), inventory (HD UI).
# Each scene is a silent background run from a crafted save (levels_smoke.sh makes one per map row)
# with a pad script and a shot every few frames; pick the frames from the contact sheets
# (<out>/<scene>-<lang>.sheet.png) and copy them to Deadware/site/static/img/incoming/hp2-<scene>-<lang>.png.
#
#   tests/scripts/site_shots.sh <build_dir> <out_dir> [lang ...]     (default: en uk)
# Needs HP2_DATA (game/extracted, mods/ with hd_ui, hd_world). ~45 min for both languages.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/tests/scripts"
BUILD=$1 OUT=$2; shift 2
LANGS=(${@:-en uk})
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
mkdir -p "$OUT"
SHEET="$OUT/contact_sheet"
[[ -x "$SHEET" ]] || swiftc -O -o "$SHEET" "$HERE/contact_sheet.swift"

# save <row>: a profile whose POTTER A loads that map (levels_smoke.sh with only that row)
save() {
  [[ -d "$OUT/save_$1" ]] && return 0
  local d
  d=$(env -u HP2_LANG -u HP2_RENDER_SCALE "$HERE/levels_smoke.sh" "$BUILD" "$1" 2>&1 | tee -a "$OUT/saves.log" |
      sed -n 's/^levels_smoke: .* out \(.*\)$/\1/p' | head -1)
  [[ -d "$d/home" ]] && cp -cR "$d/home" "$OUT/save_$1"
}

# scene <name> <row> <lang> <exit frame> <shot frames> <pad lines...>
scene() {
  local name=$1 row=$2 lang=$3 exitf=$4 frames=$5; shift 5
  local d="$OUT/$name-$lang"
  rm -rf "$d"; mkdir -p "$d"
  cp -cR "$OUT/save_$row" "$d/home" || { echo "$name-$lang: no save for row $row"; return 1; }
  printf '%s\n' "0 disconnect" "1 connect" "450 press A" "600 press A" "$@" > "$d/pad.txt"
  ( ulimit -c 0
    export HOME="$d/home" HP2_LOG_DIR="$d" HP2_SHOT_DIR="$d" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy \
           HP2_BACKGROUND=1 HP2_SKIP_MOVIES=1 HP2_SKIP=1 HP2_WIDESCREEN=1 HP2_RENDER_SCALE=4 HP2_LANG="$lang" \
           HP2_PAD_SCRIPT="$d/pad.txt" HP2_SHOT_FRAMES="$frames" HP2_EXIT_AFTER="$exitf" \
           HP2_HDUI="$HP2_DATA/mods/hd_ui" HP2_HDWORLD="$HP2_DATA/mods/hd_world"   # background runs: packs only when named
    exec "$BIN" > "$d/stdout.log" 2> "$d/stderr.log" ) &
  local pid=$! t=0
  while kill -0 $pid 2>/dev/null && (( t < 1200 )); do sleep 0.5; t=$((t + 1)); done
  kill -9 $pid 2>/dev/null; wait $pid
  "$SHEET" "$OUT/$name-$lang.sheet.png" 4 480 "$d"/frame_*.png 2>/dev/null
  echo "$name-$lang: $(ls "$d"/frame_*.png 2>/dev/null | wc -l | tr -d ' ') shots, sheet $OUT/$name-$lang.sheet.png"
}

for r in 0 4 5 55; do save $r; done
for l in "${LANGS[@]}"; do
  # The Burrow: hold A 4 s skips the arrival cutscene (HP2_SKIP=1), then a short walk.
  scene burrow 0 "$l" 4200 "$(seq -s, 1500 150 4200)" \
    "1500 hold A 260" "2000 hold A 260" "2600 stick L 0 1 120" "2800 stick L 0.5 1 120" "3200 stick R 1 0 30" "3400 stick L 0 1 200"
  # Borgin & Burkes, out into Knockturn Alley: skip the cutscene, walk forward/left.
  scene knockturn 4 "$l" 6000 "$(seq -s, 1500 250 6000)" \
    "1500 hold A 260" "2000 hold A 260" "2400 stick L 0 1 400" "2850 stick L -1 0.6 200" "3100 stick L 0 1 600" \
    "3750 stick L 1 0.6 200" "4000 stick L 0 1 800" "4850 stick L -1 0.5 200" "5100 stick L 0 1 800"
  scene diagon 5 "$l" 4800 "$(seq -s, 1500 250 4800)" \
    "1500 hold A 260" "2000 hold A 260" "2400 stick L 0 1 600" "3050 stick L -1 0.4 200" "3300 stick L 0 1 1200"
  # Aragog arena: hold LT (lock on the spider), strafe: the "Dodge" prompt (wide_fight).
  scene spiders 55 "$l" 1760 "$(seq -s, 1500 20 1760)" "1520 hold LT 240" "1560 stick L 1 0 80" "1660 stick L -1 0 80"
  # Inventory (HD UI) from the Burrow after the cutscene: Back opens it.
  scene inventory 0 "$l" 3000 "$(seq -s, 2550 50 3000)" "1500 hold A 260" "2000 hold A 260" "2500 press BACK" "2800 press RIGHT"
done
