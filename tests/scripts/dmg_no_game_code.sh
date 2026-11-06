#!/bin/bash
# The DMG carries only our tools (coordinator 2026-11-06: "DMG — це не для тестерів, це має бути
# інсталятор"): recompiler, runtime libraries, patches, the UA overlay sources. Never the lifted or
# generated game code, never game data. Scans every file of the image (DMG=path, default
# out/dmg/HP2.dmg; tools/make_dmg.sh builds it):
#   1. no lifted code: no Mach-O / archive defining lifted functions (recomp::gen::f_XXXXXXXX) or the table kLiftedFunctions, no
#      gen_*.cpp(.o), no libhp2_game;
#   2. no disc file: no file with the content (sha1) of a file of game/extracted, nothing starting
#      with the XBE magic, no 64-byte window of default.xbe's code (96 fixed samples) in any file;
#   3. no /Users/ path in any file (build paths in binaries, home paths in scripts: infra's publish
#      audit 2026-11-06; the build maps them away with -ffile-prefix-map, the kit is stripped of debug info);
#   4. tools/release_check.sh PASS (ALLOW_HD_PACKS=1: the app's mods/hd_ui + hd_world only);
#   5. under mods/ nothing but the HD packs' PNGs and manifests (owner 2026-11-06: HD packs in the DMG).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DATA="${HP2_DATA:-$ROOT}"
FAILS=(); SEMANTIC=0
fail() { echo "  FAIL: $*"; FAILS+=("$*"); }
finish() {
  if (( ${#FAILS[@]} )); then echo "dmg_no_game_code: FAIL (${#FAILS[@]})"; exit 1; fi
  echo "dmg_no_game_code: PASS ($SEMANTIC checks)"; exit 0
}
mkdir -p "$ROOT/.cache/tmp"
DMG="${DMG:-$ROOT/out/dmg/HP2.dmg}"
[[ -f "$DMG" ]] || { echo "dmg_no_game_code: no $DMG (tools/make_dmg.sh)"; exit 2; }
MNT="$(mktemp -d "$ROOT/.cache/tmp/dmgscan.XXXX")"
hdiutil attach -nobrowse -readonly -mountpoint "$MNT" "$DMG" >/dev/null || { fail "cannot mount $DMG"; finish; }
SEMANTIC=$((SEMANTIC + 1))
python3 -I - "$MNT" "$DATA/game/extracted" <<'PY' > "$ROOT/.cache/tmp/dmgscan.log" 2>&1
import hashlib, os, re, subprocess, sys
root, disc = sys.argv[1], sys.argv[2]
bad = []
disc_sha = set()
for d, _, fs in os.walk(disc):
    for f in fs:
        p = os.path.join(d, f)
        if os.path.getsize(p) >= 256:
            disc_sha.add(hashlib.sha1(open(p, 'rb').read()).hexdigest())
xbe = open(os.path.join(disc, 'default.xbe'), 'rb').read()
step = (len(xbe) - 0x20000) // 96
wins = [xbe[0x10000 + i * step:0x10000 + i * step + 64] for i in range(96)]
wins = [w for w in wins if len(set(w)) > 16]          # skip padding / constant runs
for d, _, fs in os.walk(root):
    for f in fs:
        p = os.path.join(d, f)
        if os.path.islink(p) or not os.path.isfile(p):
            continue
        rel = os.path.relpath(p, root)
        if re.search(r'gen_\d+\.cpp|libhp2_game', f):
            bad.append(f'{rel}: lifted code file')
        # the one allowed exception (owner 2026-11-06): the HD packs, PNG + manifest only
        if '/Resources/mods/' in '/' + rel and not re.fullmatch(
                r'HP2\.app/Contents/Resources/mods/hd_(ui|world)/([0-9a-f]+\.png|manifest\.tsv)', rel):
            bad.append(f'{rel}: not an HD pack file under mods/')
        b = open(p, 'rb').read()
        if b[:4] == b'XBEH':
            bad.append(f'{rel}: XBE image')
        if len(b) >= 256 and hashlib.sha1(b).hexdigest() in disc_sha:
            bad.append(f'{rel}: a file of the disc')
        hits = sum(1 for w in wins if w in b)
        if hits >= 2:
            bad.append(f'{rel}: {hits} windows of default.xbe code')
        if b'/Users/' in b:  # build or home paths (infra publish audit 2026-11-06)
            i = b.index(b'/Users/')
            bad.append(f'{rel}: a /Users/ path ({b[i:i + 60].split(bytes(1))[0].decode("latin-1")})')
        if b[:4] in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe') or b[:8] == b'!<arch>\n':
            nm = subprocess.run(['nm', '-gU', p], capture_output=True, text=True).stdout
            n = len(re.findall(r' [TtSD] (_sub_[0-9A-Fa-f]{8}|__ZN6recomp3gen\d+f_[0-9A-F]{8})', nm))
            if n or 'kLiftedFunctions' in nm:
                bad.append(f'{rel}: lifted code ({n} lifted functions)')
print(f'scanned {root}: {len(wins)} xbe windows, {len(disc_sha)} disc files')
for x in bad[:30]:
    print('  BAD', x)
sys.exit(1 if bad else 0)
PY
rc=$?; cat "$ROOT/.cache/tmp/dmgscan.log"
(( rc == 0 )) || fail "game code or data in $DMG"
hdiutil detach -quiet "$MNT"; rmdir "$MNT" 2>/dev/null
SEMANTIC=$((SEMANTIC + 1))
ALLOW_HD_PACKS=1 bash "$ROOT/tools/release_check.sh" "$DMG" | tail -1 | grep -q PASS || fail "release_check"
finish
