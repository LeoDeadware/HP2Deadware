#!/bin/bash
# HP2.dmg: an unsigned disk image with HP2.app WITHOUT game data (addendum h, 2026-11-06).
#   tools/make_dmg.sh [build_dir] [out.dmg]      (default: out/dmg/HP2.dmg)
# The app is install_app.sh in DMG mode (HP2_DMG=1): no Resources/game, no disc icon, mods = the ready
# HD packs only (R-L5, below);
# Resources/import has our importer (tools/dmg/import_disc.py, XboxPortKit xiso.py, tools/loc and
# our table). On the first run the launcher asks for the player's own disc image and imports it into
# ~/Library/Application Support/HP2/data (game + the Ukrainian text layer).
# No game code either (coordinator 2026-11-06): Resources/build has our lifter, runtime libraries and
# headers; the first run installs Apple Command Line Tools if missing, lifts and compiles the
# player's own default.xbe and puts the result in ~/Desktop/HP2.app (tools/dmg/build_game.sh).
# tools/release_check.sh and tests/scripts/dmg_no_game_code.sh gate the image.
# Unsigned: the first open is right-click → Open (README "Read me first.txt" inside the image).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/out}"
DMG="${2:-$ROOT/out/dmg/HP2.dmg}"
HD_FROM="${HD_FROM:-$HOME/Desktop/HP2.app/Contents/Resources/mods}"
HD_FILES="${HD_FILES:-[0-9a-f]+\.png|manifest\.tsv}"
STAGE="$ROOT/.cache/dmg-stage"
export TMPDIR="$ROOT/.cache/tmp"; mkdir -p "$TMPDIR" "$(dirname "$DMG")"
rm -rf "$STAGE"; mkdir -p "$STAGE"
HP2_DMG=1 HP2_APP="$STAGE/HP2.app" bash "$ROOT/tools/install_app.sh" "$BUILD" | tail -3

# Ready HD packs (RULES R-L5, owner 2026-11-06: «просто кладемо текстури в архів»; kit
# template/tools/make_dmg.sh): mods/hd_* copied from the app the owner tested (HD_FROM), only the
# packs' own files; the launcher points the game at them (HP2_HDUI / HP2_HDWORLD).
APP="$STAGE/HP2.app"; MODS="$APP/Contents/Resources/mods"; mkdir -p "$MODS"
want=""
for d in "$HD_FROM"/hd_*; do
  [[ -d "$d" ]] || continue
  n="$(basename "$d")"; mkdir -p "$MODS/$n"
  find "$d" -maxdepth 1 -type f | { grep -E "/($HD_FILES)\$" || true; } | while read -r f; do cp -c "$f" "$MODS/$n/"; done
  echo "hd pack $n: $(ls "$MODS/$n" | wc -l | tr -d ' ') files"
done
for n in ${want:-hd_ui hd_world}; do
  [[ -n "$(ls -A "$MODS/$n" 2>/dev/null)" ]] || { echo "make_dmg: FAIL — HD pack $n not in $HD_FROM"; exit 1; }
done
# License notices of what the app redistributes (x86recomp, Zydis, SoftFloat, SDL3, inline patch notices; kit 619dfae).
sdl=""; ls "$APP/Contents/Frameworks/"libSDL3* >/dev/null 2>&1 && sdl="--sdl3"
python3 -I "$ROOT/tools/notices.py" "$APP/Contents/Resources/THIRD_PARTY_NOTICES.txt" $sdl
cp "$APP/Contents/Resources/THIRD_PARTY_NOTICES.txt" "$STAGE/Third-party notices.txt"
codesign --force --deep -s - "$APP" >/dev/null 2>&1 || echo "warning: ad-hoc signing failed"
ln -s /Applications "$STAGE/Applications"
cat > "$STAGE/Read me first.txt" <<'TXT'
HP2 for Mac (unofficial port, not signed by Apple)

1. Drag HP2.app to Applications.
2. First open: right-click HP2.app -> Open -> Open (macOS Gatekeeper: the app is not signed by
   Apple; after that it opens normally).
3. Choose your own Harry Potter and the Chamber of Secrets disc image (Xbox, .iso, US). The EU (PAL) disc is untested.
4. Apple Command Line Tools are needed: if they are missing, HP2 opens the system dialog - click
   Install. HP2 then builds the game from your disc on this Mac (a few minutes, a progress window)
   and puts HP2.app on the Desktop. The game data and the game code come only from your disc.
5. The language (English, Українська or Українська - текст: Ukrainian text, English voices) is
   chosen in the launcher window; English by default. No Ukrainian voices in this build.
   Gamepad only.

HP2 для Mac (неофіційний порт, без підпису Apple)

1. Перетягніть HP2.app у «Applications».
2. Перший запуск: клацніть HP2.app правою кнопкою → «Відкрити» → «Відкрити»
   (Gatekeeper: програма без підпису Apple; далі відкривається звичайно).
3. Оберіть образ свого диска Harry Potter and the Chamber of Secrets (Xbox, .iso, США).
   Європейський диск (PAL) — не перевірено.
4. Потрібні Command Line Tools від Apple: якщо їх немає, HP2 сам відкриє системне вікно —
   натисніть «Встановити». Далі HP2 збирає гру з вашого диска (кілька хвилин, вікно з
   прогресом) і кладе готову HP2.app на робочий стіл.
5. Мову (English, Українська або «Українська — текст») обирають у вікні лаунчера; за
   замовчуванням — англійська. Української озвучки нема. Лише геймпад.
TXT
rm -f "$DMG"
hdiutil create -quiet -fs HFS+ -volname "HP2" -srcfolder "$STAGE" -format UDZO "$DMG"
ALLOW_HD_PACKS=1 bash "$ROOT/tools/release_check.sh" "$DMG"
# Release rules on the image (notices, Gatekeeper line, minos <= LSMinimumSystemVersion stay FAIL in --private;
# the public gate runs on the export: portkit export + publish_check --release HP2.dmg, .publish-allow for HD).
python3 -I "$ROOT/tools/publish_check.py" --repo "$ROOT" --private --release "$DMG" | tail -1
echo "dmg: $DMG ($(du -h "$DMG" | cut -f1))"
