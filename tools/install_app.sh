#!/bin/bash
# Installs HP2 as ~/Desktop/HP2.app ("Harry Potter and the Chamber of Secrets", native): a real
# bundle whose executable is the Mach-O launcher tools/launcher/launcher.c (RULES R-I5, the ROTK
# solution, owner 2026-11-06). Player files (settings, saves, logs) stay in HP2Port/.cache/player
# (baked into the launcher). Removes the old layout: ~/Applications/HP2.app, ~/Desktop/HP2.command,
# HP2Port/app/HP2.app. tools/app_smoke.sh opens the result like Finder does.
#
#   tools/install_app.sh [build_dir]          (default: out; tools/lock.py install -- …)
#   HP2_FROM_APP=<HP2.app> tools/install_app.sh   reuse an installed bundle's game binary, Frameworks,
#                                                game data and mods (no build dir; APFS clone)
# - game data is an APFS clone (cp -c): zero bytes;
# - Ukrainian overlay is built from mods/loc; HD packs and voice banks come from $ROOT/mods/hd_* and
#   $ROOT/.cache/voice (game-derived, never in git), else from the bundle being replaced.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/hp2"
APP="${HP2_APP:-$HOME/Desktop/HP2.app}"   # HP2_APP: a test install elsewhere
C="$APP/Contents"
RES="$C/Resources"
FROM="${HP2_FROM_APP:-}"
DMG="${HP2_DMG:-0}"   # 1: the DMG build (tools/make_dmg.sh): no game data, no game code
# never replace the game under a running copy (the owner may be playing): BUSY, nothing changed
if pgrep -f "$C/MacOS/hp2-bin" > /dev/null; then
  echo "install_app: BUSY — $APP is running; quit the game first (nothing changed)"; exit 2
fi
T="$(mktemp -d "${TMPDIR:-/tmp}/hp2_install.XXXXXX")"
# keep the rescued packs/voice when a step fails (they may be the only copy): rm only on success
trap '[[ $? == 0 ]] && rm -rf "$T" || echo "install failed: rescued packs/voice kept in $T"' EXIT

if [[ -n "$FROM" ]]; then
  [[ -d "$FROM/Contents/Resources/game/extracted" ]] || { echo "no game data in $FROM"; exit 2; }
  old_bin="$FROM/Contents/MacOS/hp2-bin"; [[ -f "$old_bin" ]] || old_bin="$FROM/Contents/MacOS/hp2"
  file -b "$old_bin" | grep -q Mach-O || { echo "no game binary in $FROM"; exit 2; }
  if [[ "$(cd "$FROM" && pwd -P)" != "$(mkdir -p "$APP" && cd "$APP" && pwd -P)" ]]; then
    rm -rf "$APP"; cp -c -R "$FROM" "$APP"
  fi
  [[ -f "$C/MacOS/hp2" ]] && file -b "$C/MacOS/hp2" | grep -q "Mach-O" && ! [[ -f "$C/MacOS/hp2-bin" ]] &&
    mv "$C/MacOS/hp2" "$T/hp2-bin" && mv "$T/hp2-bin" "$C/MacOS/hp2-bin"
else
  [[ -x "$BIN" ]] || { echo "no $BIN — build first (or HP2_FROM_APP=<app>)"; exit 2; }
  # keep the packs of the bundle being replaced when the repo has none
  for p in hd_ui hd_world; do
    [[ -d "$ROOT/mods/$p" || ! -d "$RES/mods/$p" ]] || cp -c -R "$RES/mods/$p" "$T/$p"
  done
  [[ -d "$ROOT/.cache/voice/uk/mods/_UK" || ! -d "$RES/mods/_UK" ]] ||
    { mkdir -p "$T/voice"; cp -c "$RES"/mods/_UK/Speech_*.sdt "$T/voice/" 2>/dev/null || true; }
  mkdir -p "$C/MacOS" "$C/Frameworks" "$RES/game"
  cp "$BIN" "$C/MacOS/hp2-bin"
  otool -L "$C/MacOS/hp2-bin" | awk 'NR>1{print $1}' | grep -vE '^(/usr/lib|/System)' | while read -r lib; do
    real="${lib/@rpath\//$BUILD/}"
    [[ -f "$real" ]] || real="$(find "$BUILD" /opt/homebrew/lib -name "$(basename "$lib")" -print -quit 2>/dev/null)"
    [[ -f "$real" ]] || { echo "warning: cannot find $lib"; continue; }
    dst="$C/Frameworks/$(basename "$lib")"; rm -f "$dst"; cp -L "$real" "$dst"; chmod u+w "$dst"
    install_name_tool -change "$lib" "@executable_path/../Frameworks/$(basename "$lib")" "$C/MacOS/hp2-bin" 2>/dev/null || true
  done
  if [[ $DMG == 1 ]]; then
    # No game code and no game data in the DMG (R-L5, ROTK 2026-11-06): the player's Mac imports its
    # own disc (Resources/import) and lifts + compiles default.xbe with Apple CLT (Resources/build:
    # our lifter, runtime/HLE libraries and headers, tools/dmg/kit.py). tools/make_dmg.sh adds the
    # ready HD packs to Resources/mods.
    rm -rf "$RES/game" "$RES/mods" "$RES/build" "$RES/import" "$C/MacOS/hp2-bin"
    python3 -I "$ROOT/tools/dmg/kit.py" "$BUILD" "$RES/build"
    cp "$ROOT"/tools/dmg/{build_game.sh,clt.sh,progress.js} "$ROOT/tools/launcher/make_art.sh" "$RES/build/"
    cp "$ROOT"/tools/icon/{make_icns.sh,xbe_title_image.py} "$RES/build/"
    swiftc -O -o "$RES/build/crop_top" "$ROOT/tools/launcher/crop_top.swift"
    swiftc -O -o "$RES/build/squircle" "$ROOT/tools/icon/squircle.swift"
    swiftc -O -o "$RES/build/upscale" "$ROOT/tools/hdui/upscale.swift"
    mkdir -p "$RES/import/loc"
    cp "$ROOT"/tools/dmg/import_disc.py "$ROOT/../XboxPortKit/portkit/xiso.py" "$RES/import/"
    cp "$ROOT"/tools/loc/*.py "$RES/import/loc/"; rm -f "$RES/import/loc/lint.py"
    swiftc -O -o "$RES/import/loc/glyphs" "$ROOT/tools/loc/glyphs.swift"
    cp -R "$ROOT/mods/loc/uk" "$RES/import/loc/uk"
  else
  if [[ ! -f "$RES/game/extracted/default.xbe" ]]; then
    rm -rf "$RES/game/extracted"; cp -c -R "${HP2_DATA:-$ROOT}/game/extracted" "$RES/game/extracted"
  fi
  rm -rf "$RES/mods"
  python3 "$ROOT/tools/loc/build_mod.py" uk --data "${HP2_DATA:-$RES}" --out "$RES/mods"   # the app's own copy of the disc
  for p in hd_ui hd_world; do
    if [[ -d "$ROOT/mods/$p" ]]; then cp -c -R "$ROOT/mods/$p" "$RES/mods/$p"
    elif [[ -d "$T/$p" ]]; then mv "$T/$p" "$RES/mods/$p"
    else echo "note: no mods/$p — the app runs without it"; fi
  done
  VOICE="$ROOT/.cache/voice/uk/mods/_UK"; [[ -d "$VOICE" ]] || VOICE="$T/voice"
  if ls "$VOICE"/Speech_*.sdt > /dev/null 2>&1; then
    mkdir -p "$RES/mods/_UK"; cp -c "$VOICE"/Speech_*.sdt "$RES/mods/_UK/"
  else echo "note: no Ukrainian voice banks — the Ukrainian slot speaks English"; fi
  fi
fi

# The bundle executable is a small Mach-O (tools/launcher/launcher.c): env, capped log, language,
# then execv(hp2-bin). A bash script there cannot read the app's files when opened from Finder (TCC).
rm -f "$C/MacOS/launch"
PLAYER="$ROOT/.cache/player"; [[ $DMG == 1 ]] && PLAYER=""   # DMG: ~/Library/Application Support/HP2/player
clang -O2 -DHP2_PLAYER_ROOT="\"$PLAYER\"" -o "$C/MacOS/HP2" "$ROOT/tools/launcher/launcher.c"
mkdir -p "$RES/launcher"; cp "$ROOT/tools/launcher/launcher.js" "$RES/launcher/"
# The launcher art and the icon come from the disc (never committed): the title screen, the XBE image.
XBE="$RES/game/extracted/default.xbe"
[[ $DMG == 1 ]] || [[ -f "$RES/launcher/art.png" ]] || bash "$ROOT/tools/launcher/make_art.sh" "$C/MacOS/hp2-bin" "$RES" "$RES/launcher/art.png" ||
  echo "warning: no launcher art"
rm -f "$RES/hp2.icns"
[[ $DMG == 1 ]] || bash "$ROOT/tools/icon/make_icns.sh" "$XBE" "$RES/HP2.icns" || echo "warning: no icon"
cat > "$C/Info.plist" <<EOS
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>HP2</string>
  <key>CFBundleDisplayName</key><string>Harry Potter and the Chamber of Secrets</string>
  <key>CFBundleIdentifier</key><string>local.hp2port.game</string>
  <key>CFBundleExecutable</key><string>HP2</string>
  <key>CFBundleIconFile</key><string>HP2</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.9.1</string>
  <key>LSMinimumSystemVersion</key><string>26.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>GCSupportsControllerUserInteraction</key><true/>
</dict></plist>
EOS
touch "$APP"   # Finder re-reads the icon
codesign --force --deep -s - "$APP" >/dev/null 2>&1 || echo "warning: ad-hoc signing failed"
if [[ -z "${HP2_APP:-}" && $DMG != 1 ]]; then   # the old layout
  rm -f "$HOME/Desktop/HP2.command"
  [[ -L "$HOME/Applications/HP2.app" ]] && rm -f "$HOME/Applications/HP2.app"
  [[ -d "$HOME/Applications/HP2.app" ]] && rm -rf "$HOME/Applications/HP2.app"
  [[ -n "$FROM" && "$FROM" == "$ROOT/app/HP2.app" ]] && rm -rf "$ROOT/app"
fi
echo "installed $APP ($(git -C "$ROOT" rev-parse --short HEAD))"
