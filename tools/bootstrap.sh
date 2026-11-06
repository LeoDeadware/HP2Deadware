#!/bin/bash
# Fetches third-party code into third_party/ and applies our local patches.
# Pinned to the x86recomp commit H4Port was built from.
#
#   tools/bootstrap.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
X86RECOMP_URL="https://github.com/JTM-rootstorm/x86recomp.git"
X86RECOMP_REV="1c90fe3a5dad37d0cca551fc9c8ab66a106ede1e"

mkdir -p "$ROOT/third_party"
cd "$ROOT/third_party"

# Prefer the local H4Port clone (same pinned commit) over the network.
LOCAL_CLONE="$HOME/Desktop/Claude/H4Port/third_party/x86recomp"
if [[ ! -d x86recomp ]]; then
  if git -C "$LOCAL_CLONE" cat-file -e "$X86RECOMP_REV" 2>/dev/null; then
    git clone -q --no-checkout "$LOCAL_CLONE" x86recomp
  else
    git clone -q --no-checkout "$X86RECOMP_URL" x86recomp
  fi
  git -C x86recomp checkout -q "$X86RECOMP_REV"
fi
cd x86recomp
if [[ -n "$(git status --porcelain --untracked-files=no)" ]] ||
   [[ "$(git rev-parse HEAD)" != "$X86RECOMP_REV" ]]; then
  # Already bootstrapped (patches applied, maybe being edited): leave it.
  echo "x86recomp: local changes present, left as is (rm -rf third_party/x86recomp to redo)"
else
  # H4Port fixes (SEH/C++ EH, WM_PAINT, present filters), then ours (XBE).
  for p in "$ROOT"/patches/*.patch; do
    if git apply --check "$p" 2>/dev/null; then
      git apply "$p"
      echo "applied $(basename "$p")"
    else
      echo "skipped $(basename "$p") (already applied or conflicts)"
    fi
  done
fi

# Build-time sources x86recomp would fetch (FetchContent), vendored into
# third_party/deps so HP2Port builds without the network and without H4Port.
# Pinned to the commits x86recomp's CMake names; APFS clone (cp -c) from the
# local H4Port build when it has them, else git clone.
mkdir -p "$ROOT/third_party/deps"
H4_DEPS="$HOME/Desktop/Claude/H4Port/third_party/x86recomp/out/build/release/_deps"
vendor() {  # name url rev [submodules]
  local name=$1 url=$2 rev=$3 dst="$ROOT/third_party/deps/$1-src"
  if [[ -d "$dst" ]] && [[ "$(git -C "$dst" rev-parse HEAD 2>/dev/null)" == "$rev" ]]; then
    return
  fi
  rm -rf "$dst"
  if [[ "$(git -C "$H4_DEPS/$name-src" rev-parse HEAD 2>/dev/null)" == "$rev" ]]; then
    cp -c -R "$H4_DEPS/$name-src" "$dst"
    echo "vendored $name (clone of the H4Port copy)"
  else
    git clone -q "$url" "$dst"
    git -C "$dst" checkout -q "$rev"
    [[ "${4:-}" == submodules ]] && git -C "$dst" submodule update -q --init --recursive
    echo "vendored $name (git clone)"
  fi
}
vendor zydis https://github.com/zyantific/zydis.git a2278f1d254e492f6a6b39f6cb5d1f5d515659dc submodules
vendor softfloat https://github.com/ucb-bar/berkeley-softfloat-3.git 5c06db33fc1e2130f67c045327b0ec949032df1d
