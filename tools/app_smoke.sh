#!/bin/bash
# Smoke test of the INSTALLED app (RULES R-I3, R-I5; HP2: the .app showed the wrong flag and a
# "default on" HUD option was off — only the build dir had been tested).
# Launched exactly the way Finder does it: `open -n ~/Desktop/HP2.app` (LaunchServices; the app
# is its own responsible process for macOS privacy, TCC — a terminal run has Desktop access the
# Finder-launched app lacks, so it would pass while the game does not start for the owner). Smoke
# settings go by `open --env`: 600 frames, silent, background, test profile OFF, logs and player files
# in a run dir. Checks: CFBundleExecutable is a Mach-O (a bash script there cannot read the app's files
# from Finder: no process, no log), the launcher started the game (log "=== start pid N"), the game
# ended within 2 min, the game's clean-exit line (HP2_SMOKE_OK, default "EXIT_AFTER") is in the
# logs, no FATAL, every mods/ pack in tools/install.d/expect.txt was loaded ("mods: loaded <name>").
#   tools/app_smoke.sh            # APP=~/Desktop/HP2.app by default
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${HP2_APP:-$HOME/Desktop/HP2.app}"
OKRE="${HP2_SMOKE_OK:-EXIT_AFTER}"
C="$APP/Contents"
[[ -x "$C/MacOS/hp2-bin" ]] || { echo "app_smoke: FAIL no $APP"; exit 1; }
exe="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$C/Info.plist" 2>/dev/null)"
fails=0
file -b "$C/MacOS/$exe" | grep -q "Mach-O" \
  || { echo "app_smoke: FAIL CFBundleExecutable '$exe' is not a Mach-O ($(file -b "$C/MacOS/$exe" | cut -c1-40))"; fails=1; }
RUN="$ROOT/.cache/logs/app_smoke/$(date +%Y%m%d-%H%M%S)-$$"; mkdir -p "$RUN"
log="$RUN/hp2.log"
# Never touch a running game: it is the owner playing (2026-11-06 a pkill here killed his game at
# the Whomping Willow; the second `open -n` made the Dock icon bounce). BUSY = not tested, exit 2.
if pgrep -f "$C/MacOS/hp2-bin" > /dev/null; then
  echo "app_smoke: BUSY — HP2 is running (pid $(pgrep -f "$C/MacOS/hp2-bin" | tr '\n' ' ')); not touched, not tested"; exit 2
fi
# DMG-built app (no Resources/game): the disc data is in the player's library (launcher: <root>/../data);
# the smoke's own player root has none, so point it there (else the launcher waits for an import).
data_env=()
[[ -d "$C/Resources/game" ]] || data_env=(--env HP2_DATA="$HOME/Library/Application Support/HP2/data")
open -n "$APP" "${data_env[@]}" --env HP2_PLAYER_ROOT="$RUN/player" --env HP2_LOG_DIR="$RUN" \
  --env HP2_BACKGROUND=1 --env HP2_EXIT_AFTER=600 --env SDL_AUDIO_DRIVER=dummy \
  --env HP2_HDUI="$C/Resources/mods/hd_ui" --env HP2_HDWORLD="$C/Resources/mods/hd_world" \
  || { echo "app_smoke: FAIL open -n $APP"; exit 1; }
pid=""
for _ in $(seq 1 40); do pid="$(grep -o '^=== start pid [0-9]*' "$log" 2>/dev/null | tail -1 | awk '{print $4}')"; [[ -n "$pid" ]] && break; sleep 0.5; done
[[ -n "$pid" ]] || { echo "app_smoke: FAIL the launcher did not start (no '=== start pid' in $log; TCC?)"; exit 1; }
for _ in $(seq 1 240); do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
if kill -0 "$pid" 2>/dev/null; then kill -9 "$pid"; echo "app_smoke: FAIL timeout (pid $pid)"; fails=1; fi
# any other process of THIS smoke run (its env names $RUN) — never the owner's game
for p in $(pgrep -f "$C/MacOS/" || true); do
  ps eww -p "$p" 2>/dev/null | grep -q "HP2_LOG_DIR=$RUN" && { kill -9 "$p"; echo "app_smoke: killed leftover smoke pid $p"; }
done
grep -q -h -E "$OKRE" "$RUN"/*.log 2>/dev/null \
  || { echo "app_smoke: FAIL no clean-exit line /$OKRE/ in $RUN/*.log"; fails=1; }
grep -h -m1 -E "^(\[krnl\] )?FATAL" "$RUN"/*.log 2>/dev/null && fails=1
# HP2: the shipped HD packs load (background runs name them, as the launcher does not), the log is
# capped by the launcher's pump, kernel tracing is off (the 5.4 GB log, 2026-11-06)
for p in hd_ui hd_world; do
  grep -q -h "hd_ui pack .*/$p" "$RUN"/*.log 2>/dev/null || { echo "app_smoke: FAIL pack $p not loaded"; fails=1; }
done
grep -q -h "^K> " "$RUN/hp2.log" 2>/dev/null && { echo "app_smoke: FAIL kernel trace in the log (HP2_KTRACE)"; fails=1; }
if [[ -f "$ROOT/tools/install.d/expect.txt" ]]; then
  while read -r m; do
    [[ -z "$m" || "$m" == \#* ]] && continue
    grep -q -h "mods: loaded $m" "$RUN"/*.log 2>/dev/null || { echo "app_smoke: FAIL pack $m not loaded"; fails=1; }
  done < "$ROOT/tools/install.d/expect.txt"
fi
(( fails )) && { echo "app_smoke: evidence $RUN"; exit 1; }
echo "app_smoke: PASS ($RUN)"
