#!/bin/bash
# Release gate (RULES R-L1/R-L2): a .app/.dmg/zip for anyone but the owner must contain NO
# game data and nothing derived from it (packs, voice, flat images, Ghidra exports).
#   tools/release_check.sh path/to/HP2.app|.dmg|.zip
set -uo pipefail
t="$1"; tmp=""
case "$t" in
  *.dmg) tmp="$(mktemp -d)"; hdiutil attach -nobrowse -readonly -mountpoint "$tmp" "$t" >/dev/null || exit 2; root="$tmp" ;;
  *.zip) tmp="$(mktemp -d)"; unzip -q "$t" -d "$tmp"; root="$tmp" ;;
  *) root="$t" ;;
esac
bad="$(find "$root" \( -iname '*.xbe' -o -iname '*.iso' -o -iname '*.xiso' -o -iname 'default.flat*' \
  -o -path '*/game/extracted/*' -o -path '*/mods/hd_*' -o -path '*/voice/*' -o -iname '*.gpr' \) -print 2>/dev/null)"
# ALLOW_HD_PACKS=1 (tools/make_dmg.sh; owner 2026-11-06): the app's own HD packs are allowed, nothing else
[[ "${ALLOW_HD_PACKS:-0}" == 1 ]] && bad="$(grep -vE "^$root/HP2\.app/Contents/Resources/mods/hd_(ui|world)(/|\$)" <<< "$bad")"
[[ "$t" == *.dmg ]] && hdiutil detach -quiet "$tmp"
[[ -n "$tmp" && "$t" == *.zip ]] && rm -rf "$tmp"
if [[ -n "$bad" ]]; then echo "release_check: FAIL — game data / derived content:"; head -20 <<< "$bad"; exit 1; fi
echo "release_check: PASS (no game data found in $t)"
