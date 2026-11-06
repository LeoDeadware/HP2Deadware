#!/bin/bash
# Builds and runs the device clock drift test (QueueController, drift.h).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/hp2_drift_test"
mkdir -p "$OUT"
c++ -std=c++20 -O2 -I"$SRC" "$HERE/drift_test.cpp" -o "$OUT/drift_test"
"$OUT/drift_test"
