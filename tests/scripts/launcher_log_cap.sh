#!/bin/bash
# The launcher's log cap (owner 2026-11-06: hp2.log had grown to 5.4 GB): a fake game writes 130 MB
# to stdout; hp2.log and hp2.log.1 must each stay <= 50 MB and the newest output must be in hp2.log.
# Also: kernel tracing is off by default (HP2_KTRACE=0 reaches the game), the language defaults to en.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="$(mktemp -d "${TMPDIR:-/tmp}/hp2_logcap.XXXXXX")"; trap 'rm -rf "$T"' EXIT
A="$T/HP2.app/Contents"; mkdir -p "$A/MacOS" "$A/Resources/game/extracted"; touch "$A/Resources/game/extracted/default.xbe"
clang -O2 -o "$A/MacOS/HP2" "$ROOT/tools/launcher/launcher.c" || exit 2
cat > "$A/MacOS/hp2-bin" <<'G'
#!/bin/bash
echo "fake: ktrace=$HP2_KTRACE lang=$HP2_LANG"
[[ -n "${FAKE_SHORT:-}" ]] && { echo "fake: short run"; exit 0; }
head -c 130000000 /dev/zero | tr '\0' 'x' | fold -w 100
echo "fake: ktrace=$HP2_KTRACE lang=$HP2_LANG"; echo "fake: last line"
G
chmod +x "$A/MacOS/hp2-bin"
HP2_PLAYER_ROOT="$T/player" HP2_NO_UI=1 "$A/MacOS/HP2"
sleep 1
L="$T/player/logs/hp2.log"; fails=0
for f in "$L" "$L.1"; do
  s=$(stat -f %z "$f" 2>/dev/null || echo 0)
  (( s > 0 && s <= 50 * 1048576 )) || { echo "FAIL $f size $s"; fails=1; }
done
tail -1 "$L" | grep -q "fake: last line" || { echo "FAIL newest output not in hp2.log"; fails=1; }
grep -q -h "fake: ktrace=0 lang=en" "$L" "$L.1" || { echo "FAIL ktrace/lang defaults"; fails=1; }
# every start rotates (2026-11-06: the crashed Whomping Willow run's log was lost to the next start):
# a second run gets a fresh hp2.log, the first run's end is in hp2.log.1, at most hp2.log + 5 parts
FAKE_SHORT=1 HP2_PLAYER_ROOT="$T/player" HP2_NO_UI=1 "$A/MacOS/HP2"; sleep 1
grep -q "fake: short run" "$L" && ! grep -q "fake: last line" "$L" || { echo "FAIL second run not in a fresh hp2.log"; fails=1; }
grep -q "fake: last line" "$L.1" || { echo "FAIL first run's log not kept as hp2.log.1"; fails=1; }
for i in 1 2 3 4 5 6; do FAKE_SHORT=1 HP2_PLAYER_ROOT="$T/player" HP2_NO_UI=1 "$A/MacOS/HP2"; done; sleep 1
n=$(ls "$T/player/logs" | grep -c '^hp2\.log')
(( n == 6 )) || { echo "FAIL $n log files after 8 runs (want hp2.log + 5)"; fails=1; }
(( fails )) && exit 1
echo "launcher_log_cap: PASS rotation per start ($n files); cap: (hp2.log $(($(stat -f %z "$L") >> 20)) MB, hp2.log.1 $(($(stat -f %z "$L.1") >> 20)) MB)"
