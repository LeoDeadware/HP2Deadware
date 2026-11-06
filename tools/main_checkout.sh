#!/bin/bash
# Prints the main checkout of this repo: worktrees share its untracked data (game/, .cache/HP2Cache).
# Outside git (a source export without .git) it is the repo this script lives in.
d="$(cd "$(dirname "$0")/.." && pwd)"
g="$(git -C "$d" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)"
if [[ -n "$g" && "$(basename "$g")" == .git ]]; then dirname "$g"; else echo "$d"; fi
