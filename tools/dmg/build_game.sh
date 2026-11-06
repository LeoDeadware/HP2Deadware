#!/bin/bash
# The player's Mac builds the game code (coordinator 2026-11-06: the DMG never carries lifted or
# generated game code; the installer compiles with Apple Command Line Tools' clang):
#   build_game.sh <kit dir> <disc dir (game/extracted)> <out hp2-bin> [progress file]
# kit (Resources/build, tools/install_app.sh HP2_DMG=1): bin/recomp_lift (our lifter),
# lift/symbols + lift/manifest.txt + lift/flags (our analysis), include/ (the lifted code's runtime
# headers), lib/ (main.o + our runtime/HLE libraries + link.txt), ../../Frameworks (SDL3).
# Steps: 1. Command Line Tools (clt.sh: missing -> `xcode-select --install`, the system dialog, wait);
# 2. lift default.xbe -> C++; 3. compile (all cores, nice); 4. link + ad-hoc sign.
# Progress lines "N stage text" (0..100) go to the progress file (the launcher's window reads it).
set -uo pipefail
KIT=$1 DISC=$2 OUT=$3 PROG="${4:-/dev/null}"
WORK="${HP2_BUILD_WORK:-$(dirname "$OUT")/build-work}"   # the launcher puts it outside the app
# Interface language (HP2_UI_LANG, the launcher's saved choice): English unless uk (owner 2026-11-06).
msg() { [[ "${HP2_UI_LANG:-en}" == uk ]] && echo "$2" || echo "$1"; }
report() { echo "$1 $2" >> "$PROG"; echo "build_game: [$1%] $2"; }
die() { report 100 "$(msg ERROR ПОМИЛКА): $1"; exit 1; }
: > "$PROG"
# 1. compiler
bash "$KIT/clt.sh" "$PROG" || die "$(msg "Command Line Tools are not installed" "Command Line Tools не встановлено")"
CXX="$(xcrun -f clang++)"
SDK="$(xcrun --show-sdk-path 2>/dev/null)"   # CLT clang finds no C++ headers without it
[[ -d "$SDK" ]] || die "no macOS SDK in Command Line Tools (xcrun --show-sdk-path)"
report 3 "$(msg Compiler Компілятор): $("$CXX" --version | head -1)"
# 2. lift
mkdir -p "$WORK/gen" "$WORK/obj"
if [[ ! -f "$WORK/gen/lift_report.txt" ]]; then
  report 5 "$(msg "Translating the game code (default.xbe)" "Перекладаю код гри (default.xbe)")"
  rm -rf "$WORK/gen"; mkdir -p "$WORK/gen"
  nice -n 10 "$KIT/bin/recomp_lift" --exe "$DISC/default.xbe" --symbols "$KIT/lift/symbols" \
    --manifest "$KIT/lift/manifest.txt" --out "$WORK/gen" $(cat "$KIT/lift/flags") > "$WORK/lift.log" 2>&1 ||
    die "lift failed: $(tail -2 "$WORK/lift.log")"
fi
# 3. compile (resumable: existing objects newer than their source are kept)
srcs=("$WORK"/gen/*.cpp); n=${#srcs[@]}
(( n > 0 )) || die "no generated code"
jobs=$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || sysctl -n hw.ncpu)
FLAGS=(-isysroot "$SDK" -std=c++20 -O3 -g0 -DNDEBUG -arch arm64 -mmacosx-version-min=26.0 -fno-strict-aliasing
       -fnon-call-exceptions -ffp-contract=off -fno-fast-math -fno-omit-frame-pointer
       -mno-omit-leaf-frame-pointer -I "$KIT/include")
# paths may hold spaces ("Application Support"): flags go through a response file, names via -0
for f in "${FLAGS[@]}"; do printf '"%s"\n' "$f"; done > "$WORK/flags.rsp"
export CXX WORK
done_n() { ls "$WORK"/obj/*.o 2>/dev/null | wc -l | tr -d ' '; }
printf '%s\0' "${srcs[@]}" | xargs -0 -P "$jobs" -n 1 bash -c '
  s="$1"; o="$WORK/obj/$(basename "${s%.cpp}").o"
  [[ -f "$o" && "$o" -nt "$s" ]] && exit 0
  nice -n 10 "$CXX" @"$WORK/flags.rsp" -c "$s" -o "$o.tmp" 2>>"$WORK/compile.log" && mv "$o.tmp" "$o"' _ &
cpid=$!
while kill -0 $cpid 2>/dev/null; do
  report $((10 + 80 * $(done_n) / n)) "$(msg Compiling Компілюю) $(done_n)/$n"
  sleep 3
done
wait $cpid
(( $(done_n) == n )) || die "compile failed: $(grep -m2 error "$WORK/compile.log")"
# 4. link
report 92 "$(msg "Linking the game" "Збираю гру")"
FW="$(cd "$KIT/../../Frameworks" && pwd)"
libs=(); while read -r l; do libs+=("${l//@KIT@/$KIT}"); done < "$KIT/lib/link.txt"
libs=("${libs[@]//@FW@/$FW}")
"$CXX" -isysroot "$SDK" -arch arm64 -mmacosx-version-min=26.0 -Wl,-headerpad_max_install_names -o "$OUT.tmp" \
  "$KIT/lib/main.o" "$WORK"/obj/*.o "${libs[@]}" > "$WORK/link.log" 2>&1 || die "link failed: $(grep -m2 -i error "$WORK/link.log")"
for l in $(otool -L "$OUT.tmp" | awk 'NR>1 && /SDL/ {print $1}'); do
  install_name_tool -change "$l" "@executable_path/../Frameworks/$(basename "$l")" "$OUT.tmp"
done
codesign --force -s - "$OUT.tmp" >/dev/null 2>&1
mv "$OUT.tmp" "$OUT"
rm -rf "$WORK"
# the launcher window's art: the title screen of the player's disc (data dir = DISC/../..)
report 97 "$(msg "Making the launcher art" "Готую лаунчер")"
bash "$KIT/make_art.sh" "$OUT" "$(cd "$DISC/../.." && pwd)" "$(cd "$DISC/../.." && pwd)/launcher-art.png" >/dev/null 2>&1 || true
# the app icon: the disc's title image fitted to the macOS icon grid (tools/icon/make_icns.sh)
bash "$KIT/make_icns.sh" "$DISC/default.xbe" "$(dirname "$OUT")/../Resources/HP2.icns" >/dev/null 2>&1 &&
  touch "$(dirname "$OUT")/../.." || true
report 100 "$(msg Done Готово)"
