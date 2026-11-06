#!/bin/bash
# Owner's full installer (HP2-full.dmg): the installed ~/Desktop/HP2.app with EVERYTHING inside (disc data,
# Ukrainian text + voice banks, fonts, HD UI + HD world packs) as one compressed .dmg, for the
# owner's own backup (external SSD, Dropbox online-only). Contains the game's data: never share it
# (XboxPortKit RULES R-L1/R-L2; tools/release_check.sh would rightly fail on it).
#
#   tools/package_personal.sh [out.dmg]   (default: Deadware/Leo's Games/HP2/HP2-full.dmg)
# Result: the .dmg + .sha256. Install on any Mac: open the .dmg,
# drag HP2 to Applications (README inside). Staging uses APFS clones (no extra disk); the .dmg
# itself is a real file of ~2.6 GB.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${HP2_APP:-$HOME/Desktop/HP2.app}"
R="$APP/Contents/Resources"
GAMES="$ROOT/../Leo's Games"
DMG="${1:-$GAMES/HP2/HP2-full.dmg}"
OUT="$(mkdir -p "$(dirname "$DMG")" && cd "$(dirname "$DMG")" && pwd)"; DMG="$OUT/$(basename "$DMG")"
REV="$(git -C "$ROOT" rev-parse --short main)"
NAME="HP2-$REV"

# What the installer must carry (fails loudly instead of packing a half app).
for p in "$APP/Contents/MacOS/hp2-bin" "$R/game/extracted/default.xbe" "$R/mods/_UK" "$R/mods/_UKT" \
         "$R/mods/hd_ui" "$R/mods/hd_world" "$R/mods/panel.geo"; do
  [[ -e "$p" ]] || { echo "missing $p — run tools/install_app.sh first"; exit 2; }
done
banks=$(ls "$R"/mods/_UK/Speech_*.sdt 2>/dev/null | wc -l | tr -d ' ')
(( banks > 0 )) || { echo "no Ukrainian speech banks in $R/mods/_UK"; exit 2; }

mkdir -p "$ROOT/.cache/tmp"; STAGE="$(mktemp -d "$ROOT/.cache/tmp/hp2pkg.XXXXXX")"   # in the project (isolation rule)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/$NAME"
cp -c -R "$APP" "$STAGE/$NAME/HP2.app"
# the Desktop app bakes this Mac's project path for player files; the copy uses the standard place
clang -O2 -o "$STAGE/$NAME/HP2.app/Contents/MacOS/HP2" "$ROOT/tools/launcher/launcher.c"
codesign --force --deep -s - "$STAGE/$NAME/HP2.app" >/dev/null 2>&1 || true
ln -s /Applications "$STAGE/$NAME/Applications"
cat > "$STAGE/$NAME/READ ME.txt" <<EOF
Harry Potter and the Chamber of Secrets — native macOS port (personal copy, do not share)
Build: HP2Port $REV, packed $(date '+%Y-%m-%d %H:%M').

Inside HP2.app: the game data from my own disc, Ukrainian text and voice ($banks speech banks),
the hybrid "Sound | Text" language, fonts, HD UI and HD world texture packs.

Install: drag HP2 onto Applications (or anywhere). First start: if macOS blocks it,
right-click HP2 -> Open. Saves, settings and logs live in ~/Library/Application Support/HP2/player —
they are not in this image, back them up separately.
EOF

rm -f "$DMG"
echo "creating $DMG (lzfse, a few minutes)…"
hdiutil create -quiet -volname "HP2 $REV" -srcfolder "$STAGE/$NAME" -format ULFO -fs HFS+ "$DMG"
hdiutil verify -quiet "$DMG"

# Check the image really carries the app with its data.
MNT="$(mktemp -d)"
hdiutil attach -quiet -nobrowse -readonly -mountpoint "$MNT" "$DMG"
ok=1
for p in Contents/MacOS/hp2-bin Contents/Resources/game/extracted/default.xbe Contents/Resources/mods/hd_world; do
  [[ -e "$MNT/HP2.app/$p" ]] || { echo "image lacks $p"; ok=0; }
done
inner=$(ls "$MNT"/HP2.app/Contents/Resources/mods/_UK/Speech_*.sdt | wc -l | tr -d ' ')
codesign --verify --deep "$MNT/HP2.app" 2>/dev/null || echo "note: signature check failed (ad-hoc app; right-click -> Open on first start)"
hdiutil detach -quiet "$MNT"
(( ok )) && [[ "$inner" == "$banks" ]] || { echo "FAIL: image incomplete"; exit 1; }

shasum -a 256 "$DMG" | sed "s#$OUT/##" > "$DMG.sha256"
echo "ok: $DMG ($(du -h "$DMG" | cut -f1), $inner speech banks) + .sha256"
