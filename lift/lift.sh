#!/bin/bash
# Lifts default.xbe and publishes an immutable lift to .cache/HP2Cache/lift/<lift_id>/:
#   generated/        the lifter's C++ (gen_NNN.cpp, table.cpp, lift_report.txt)
#   lib/libhp2_game.a the compiled lifted code
#   manifest.txt, symbols/, inputs.txt, lift_id.txt
# lift_id = <date>-<hash of every input>: XBE, symbols, manifest, x86recomp
# rev + patches, lifter flags, lifted-code headers, compiler. The same inputs
# give the same id, and an existing id is reused, never rebuilt.
# The lift and the library build run under the `heavy` lock (which waits on
# tools/guard.py) with a memory-aware -j (tools/guard.py --jobs).
# Resumable: the staging dir is per lift_id; a rerun keeps the generated code
# and the objects already built (ninja) and finishes the rest.
#
#   lift/lift.sh [--pin]           (--pin: also write lift/LIFT_ID and lift/CURRENT)
#   lift/lift.sh --detach [--pin]  (runs under nohup: log .cache/HP2Cache/logs/lift.log,
#                                   pid .cache/HP2Cache/logs/lift.pid)
set -euo pipefail

if [[ "${1:-}" == "--detach" ]]; then
  shift
  mkdir -p "${HP2_CACHE:-$("$(dirname "$0")/../tools/main_checkout.sh")/.cache/HP2Cache}/logs"
  nohup "$0" "$@" > "${HP2_CACHE:-$("$(dirname "$0")/../tools/main_checkout.sh")/.cache/HP2Cache}/logs/lift.log" 2>&1 &
  echo $! > "${HP2_CACHE:-$("$(dirname "$0")/../tools/main_checkout.sh")/.cache/HP2Cache}/logs/lift.pid"
  echo "lift running detached (pid $!), log .cache/HP2Cache/logs/lift.log"
  exit 0
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../tools/main_checkout.sh")}"
XBE="${HP2_XBE:-$HP2_DATA/game/extracted/default.xbe}"
export HP2_CACHE="${HP2_CACHE:-$("$(dirname "$0")/../tools/main_checkout.sh")/.cache/HP2Cache}"
LIFT_ROOT="$HP2_CACHE/lift"
X86="$ROOT/third_party/x86recomp"
LIFT_FLAGS="--overridable --insns-per-file 4000 --lines-per-file 18000"
PIN=0
[[ "${1:-}" == "--pin" ]] && PIN=1

[[ -f "$XBE" ]] || { echo "no XBE at $XBE" >&2; exit 1; }
[[ -d "$X86" ]] || { echo "run tools/bootstrap.sh first" >&2; exit 1; }

# The lifter tool (small, normal build).
if [[ ! -f "$ROOT/out/build.ninja" ]]; then
  cmake -S "$ROOT" -B "$ROOT/out" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
fi
cmake --build "$ROOT/out" --target recomp_lift -j3 >/dev/null
LIFTER="$ROOT/out/x86recomp_lifter/recomp_lift"

# Inputs -> lift_id.
INPUTS="$(mktemp)"
trap 'rm -f "$INPUTS"' EXIT
{
  echo "xbe $(shasum -a 256 "$XBE" | cut -d' ' -f1)"
  for f in "$ROOT"/lift/symbols/*.tsv "$ROOT/lift/manifest.txt" "$ROOT"/patches/x86recomp-*.patch \
           "$ROOT/lift/game/CMakeLists.txt" \
           "$X86"/src/runtime/{lifted,ctx,x87,sse}.h; do
    echo "$(shasum -a 256 "$f" | cut -d' ' -f1) ${f#$ROOT/}"
  done
  echo "x86recomp $(git -C "$X86" rev-parse HEAD) + $(git -C "$X86" diff HEAD | shasum -a 256 | cut -d' ' -f1)"
  echo "lifter-src $(cat "$X86"/tools/lifter/*.cpp "$X86"/tools/lifter/*.h | shasum -a 256 | cut -d' ' -f1)"
  echo "flags $LIFT_FLAGS"
  echo "cxx $(c++ --version | head -1)"
} > "$INPUTS"
HASH="$(shasum -a 256 "$INPUTS" | cut -c1-12)"
EXISTING="$(ls -d "$LIFT_ROOT"/*-"$HASH" 2>/dev/null | head -1 || true)"
if [[ -n "$EXISTING" && -f "$EXISTING/lib/libhp2_game.a" ]]; then
  LIFT_ID="$(basename "$EXISTING")"
  echo "lift $LIFT_ID already published (same inputs)"
else
  LIFT_ID="$(date +%Y%m%d)-$HASH"
  STAGE="$LIFT_ROOT/.staging-$HASH"   # per inputs: a rerun resumes here
  mkdir -p "$STAGE/lib"
  cp "$INPUTS" "$STAGE/inputs.txt"
  cp "$ROOT/lift/manifest.txt" "$STAGE/"
  rm -rf "$STAGE/symbols" && cp -R "$ROOT/lift/symbols" "$STAGE/symbols"
  if [[ ! -f "$STAGE/generated/lift_report.txt" ]]; then
    echo "lifting -> $LIFT_ID"
    python3 "$ROOT/tools/lock.py" heavy -- "$LIFTER" --exe "$XBE" --symbols "$ROOT/lift/symbols" \
      --manifest "$ROOT/lift/manifest.txt" --out "$STAGE/generated" $LIFT_FLAGS
  else
    echo "resuming $LIFT_ID (generated code kept)"
  fi
  python3 "$ROOT/tools/lock.py" heavy -- bash -c "
    cmake -S '$ROOT/lift/game' -B '$STAGE/build' -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DHP2_GEN_DIR='$STAGE/generated' -DX86RECOMP_DIR='$X86' >/dev/null &&
    cmake --build '$STAGE/build' -j\$(python3 '$ROOT/tools/guard.py' --jobs)"
  cp "$STAGE/build/libhp2_game.a" "$STAGE/lib/"
  rm -rf "$STAGE/build"
  echo "$LIFT_ID" > "$STAGE/lift_id.txt"
  chmod -R a-w "$STAGE"
  chmod u+w "$STAGE"  # the rename needs the dir writable on some filesystems
  mv "$STAGE" "$LIFT_ROOT/$LIFT_ID"
  chmod u-w "$LIFT_ROOT/$LIFT_ID"
  echo "published $LIFT_ROOT/$LIFT_ID"
fi
grep -E "^(functions|instructions|files) " "$LIFT_ROOT/$LIFT_ID/generated/lift_report.txt" || true

if [[ $PIN == 1 ]]; then
  echo "LIFT_ID=$LIFT_ID" > "$ROOT/lift/LIFT_ID"
  echo "$LIFT_ID" > "$ROOT/lift/CURRENT"
  echo "pinned lift/LIFT_ID and lift/CURRENT to $LIFT_ID"
fi
