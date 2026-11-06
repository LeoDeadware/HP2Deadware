#!/bin/bash
# Owner's full installer (HP2-full.dmg): the Desktop app — the very build the players' DMG makes (owner
# 2026-11-06, Deadware/CLAUDE.md 931d33a: same binaries, byte for byte) — plus its data folder
# (~/Library/Application Support/HP2/data: disc data, Ukrainian text layer, fonts) and "Install HP2.command",
# which puts both in place, so a new Mac needs no disc and no compile. HD packs are inside the app.
# For the owner's own backup only (external SSD, Dropbox online-only): it contains the game's data, never
# share it (XboxPortKit RULES R-L1/R-L2; tools/release_check.sh would rightly fail on it).
#
#   tools/package_personal.sh [out.dmg]   (default: Deadware/Leo's Games/HP2/HP2-full.dmg)
# Result: the .dmg + .sha256. Staging uses APFS clones (no extra disk); the .dmg is a real file.
# The image's app must hash-equal the Desktop app (checked after packing).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${HP2_APP:-$HOME/Desktop/HP2.app}"
DATA="${HP2_DATA_DIR:-$HOME/Library/Application Support/HP2/data}"
GAMES="$ROOT/../Leo's Games"
DMG="${1:-$GAMES/HP2/HP2-full.dmg}"
OUT="$(mkdir -p "$(dirname "$DMG")" && cd "$(dirname "$DMG")" && pwd)"; DMG="$OUT/$(basename "$DMG")"
REV="$(git -C "$ROOT" rev-parse --short main)"
NAME="HP2-$REV"
# What the installer must carry (fails loudly instead of packing a half app).
for p in "$APP/Contents/MacOS/hp2-bin" "$APP/Contents/Resources/mods/hd_ui" "$APP/Contents/Resources/mods/hd_world" \
         "$DATA/game/extracted/default.xbe" "$DATA/mods"; do
  [[ -e "$p" ]] || { echo "missing $p — install the DMG build first"; exit 2; }
done
apphash() { (cd "$1" && find . -type f -print0 | sort -z | xargs -0 shasum -a 256) | shasum -a 256 | cut -c1-16; }
want="$(apphash "$APP")"
mkdir -p "$ROOT/.cache/tmp"; STAGE="$(mktemp -d "$ROOT/.cache/tmp/hp2pkg.XXXXXX")"   # in the project (isolation rule)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/$NAME/HP2 data"
cp -c -R "$APP" "$STAGE/$NAME/HP2.app"
for e in game mods region.txt launcher-art.png; do
  [[ -e "$DATA/$e" ]] && cp -c -R "$DATA/$e" "$STAGE/$NAME/HP2 data/"
done
cat > "$STAGE/$NAME/Install HP2.command" <<'CMD'
#!/bin/bash
# Puts HP2.app on the Desktop and its data in ~/Library/Application Support/HP2/data (saves are not touched).
set -e
H="$(cd "$(dirname "$0")" && pwd)"
pgrep -f "HP2.app/Contents/MacOS/hp2-bin" >/dev/null && { echo "HP2 is running: quit it first."; exit 1; }
D="$HOME/Library/Application Support/HP2"; mkdir -p "$D/player"
rm -rf "$D/data.new"; ditto "$H/HP2 data" "$D/data.new"; rm -rf "$D/data"; mv "$D/data.new" "$D/data"
rm -rf "$HOME/Desktop/HP2.app"; ditto "$H/HP2.app" "$HOME/Desktop/HP2.app"
echo "HP2 installed: ~/Desktop/HP2.app"
CMD
chmod +x "$STAGE/$NAME/Install HP2.command"
cat > "$STAGE/$NAME/READ ME.txt" <<TXT
Harry Potter and the Chamber of Secrets — native macOS port (personal copy, do not share)
Build: HP2Port $REV, packed $(date '+%Y-%m-%d %H:%M'). The same app the players' DMG builds (macOS 26+).
Inside: HP2.app (HD UI + HD world packs) and "HP2 data" (the game data from my own disc, Ukrainian text, fonts).
No Ukrainian voice in this build. Install: double-click "Install HP2.command" (right-click -> Open the first
time): HP2.app goes to the Desktop, the data to ~/Library/Application Support/HP2/data.
Saves, settings and logs live in ~/Library/Application Support/HP2/player: not in this image, back them up separately.
TXT
rm -f "$DMG"
echo "creating $DMG (lzfse, a few minutes)…"
hdiutil create -quiet -volname "HP2 $REV" -srcfolder "$STAGE/$NAME" -format ULFO -fs HFS+ "$DMG"
hdiutil verify -quiet "$DMG"
# Check the image really carries the app (hash-equal to the Desktop app) and its data.
MNT="$(mktemp -d "$ROOT/.cache/tmp/hp2mnt.XXXXXX")"
hdiutil attach -quiet -nobrowse -readonly -mountpoint "$MNT" "$DMG"
ok=1
[[ "$(apphash "$MNT/HP2.app")" == "$want" ]] || { echo "image app differs from $APP"; ok=0; }
[[ -e "$MNT/HP2 data/game/extracted/default.xbe" ]] || { echo "image lacks the data"; ok=0; }
hdiutil detach -quiet "$MNT"; rmdir "$MNT"
(( ok )) || { echo "FAIL: image incomplete"; exit 1; }
shasum -a 256 "$DMG" | sed "s#$OUT/##" > "$DMG.sha256"
echo "ok: $DMG ($(du -h "$DMG" | cut -f1)), app $want = Desktop app, + .sha256"
