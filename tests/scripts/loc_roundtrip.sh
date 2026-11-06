#!/bin/bash
# Localization round trip (Localizer, tools/loc/):
#  1. textbank.py: every text bank of every language slot -> TSV -> bank is
#     byte-identical to the disc file;
#  2. extract.py: the translation table (mods/loc/uk) packed with --english
#     (translations ignored) gives every _UK bank byte for byte;
#  3. textbank.py check: the translation keeps every %x code and button glyph
#     of the English string, and the table covers every id on the disc.
#
#   tests/scripts/loc_roundtrip.sh          (no game run, ~10 s)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}"
DISC="$DATA/game/extracted"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/loc_roundtrip.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
FAILS=0
fail() { echo "  FAIL: $*"; FAILS=$((FAILS + 1)); }

echo "1. text banks of every language"
python3 "$ROOT/tools/loc/textbank.py" roundtrip "$DISC"/_??? "$DISC"/_?? || fail "textbank round trip"

echo "2. translation table, --english -> _UK banks"
python3 "$ROOT/tools/loc/extract.py" pack --data "$DATA" --english --out "$TMP/en" > /dev/null || fail "pack"
n=0
for f in "$DISC"/_UK/text*.bin; do
  n=$((n + 1))
  cmp -s "$f" "$TMP/en/_UK/$(basename "$f")" || fail "$(basename "$f") differs"
done
echo "  $n banks compared"

echo "3. translation keeps codes; table covers the disc"
python3 "$ROOT/tools/loc/extract.py" verify --data "$DATA" || fail "verify"

if [[ $FAILS == 0 ]]; then echo "PASS"; else echo "FAIL ($FAILS)"; exit 1; fi
