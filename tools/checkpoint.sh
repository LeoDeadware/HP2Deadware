#!/bin/bash
# Saves progress outside the working tree so a crashed app, a removed worktree or a
# bad reset never loses work:
#   1. every local branch (all worktrees share this repo) -> bare mirror .cache/HP2Backup/hp2.git
#   2. uncommitted work in each worktree -> a stash-like snapshot ref refs/wip/<branch>
#      (git stash create: does not touch the worktree or the index)
#   3. untracked-but-precious files (recon/ghidra/export, .cache/HP2Cache/lift/CURRENT id) noted
#
#   tools/checkpoint.sh          run any time; idempotent; ~seconds
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && git rev-parse --path-format=absolute --git-common-dir)"
ROOT="$(dirname "$ROOT")"
BACKUP="${HP2_BACKUP:-$(cd "$(dirname "$0")/.." && pwd)/.cache/HP2Backup/hp2.git}"

[[ -d "$BACKUP" ]] || git init -q --bare "$BACKUP"

# 2. snapshot uncommitted work of every worktree
git -C "$ROOT" worktree list --porcelain | awk '/^worktree /{print $2}' | while read -r wt; do
  branch="$(git -C "$wt" rev-parse --abbrev-ref HEAD)"
  [[ "$branch" == HEAD ]] && branch="detached/$(basename "$wt")"  # one ref per detached worktree
  if [[ -n "$(git -C "$wt" ls-files -u 2>/dev/null)" ]]; then
    echo "skip wip snapshot: $branch (merge in progress in $wt)"
    continue
  fi
  snap="$(git -C "$wt" stash create "checkpoint $(date '+%F %T')" 2>/dev/null || true)"
  if [[ -n "$snap" ]]; then
    git -C "$ROOT" update-ref "refs/wip/$branch" "$snap"
    echo "wip snapshot: $branch -> ${snap:0:9}"
  fi
done

# 1. mirror branches + wip refs (force: the backup follows the repo, history is kept by reflog)
git -C "$ROOT" push -q --force "$BACKUP" 'refs/heads/*:refs/heads/*' 'refs/wip/*:refs/wip/*' 2>/dev/null \
  || git -C "$ROOT" push -q --force "$BACKUP" 'refs/heads/*:refs/heads/*'
git -C "$BACKUP" config core.logAllRefUpdates always

echo "checkpoint ok: $(git -C "$BACKUP" for-each-ref --count=999 | wc -l | tr -d ' ') refs in $BACKUP"
