#!/usr/bin/env python3
"""Frees disk in the places this project grows. Safe to run any time; prints what it removes.

  tools/cleanup.py            prune (keeps what is pinned or recent)
  tools/cleanup.py --dry      show only

- .cache/HP2Cache/lift/<id>: keeps lift/CURRENT, every id pinned by a worktree's lift/LIFT_ID,
  and the 2 newest; removes the rest.
- ccache: trims to its max size (10 GB).
- .cache/HP2Cache/runs/* and tests/runs/* (test-run scratch HOMEs/logs) older than 24 h.
- out/ build dirs of worktrees that no longer exist (.cache/HP2Cache/out/<branch>), once unused for 6 h.
"""
import os
import shutil
import subprocess
import sys
import time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hp2paths  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = hp2paths.CACHE
DRY = "--dry" in sys.argv


def rm(path, why):
    size = du(path)
    print(f"{'would remove' if DRY else 'remove'} {path} ({size / 2**30:.2f} GB) — {why}")
    if not DRY:
        shutil.rmtree(path, ignore_errors=True)


def du(path):
    total = 0
    for d, _, files in os.walk(path):
        for f in files:
            try:
                total += os.lstat(os.path.join(d, f)).st_size
            except OSError:
                pass
    return total


def recently_used(path, now, window):
    """True if anything under path was modified or accessed within window seconds."""
    for d, _, files in os.walk(path):
        for f in [None] + files:
            try:
                st = os.stat(os.path.join(d, f) if f else d)
            except OSError:
                continue
            if now - max(st.st_mtime, st.st_atime) < window:
                return True
    return False


def worktrees():
    out = subprocess.run(["git", "-C", ROOT, "worktree", "list", "--porcelain"],
                         capture_output=True, text=True).stdout
    return [l.split(" ", 1)[1] for l in out.splitlines() if l.startswith("worktree ")]


def main():
    trees = worktrees()
    keep = set()
    for t in trees:
        for name in ("lift/LIFT_ID", "lift/CURRENT"):
            try:
                v = open(os.path.join(t, name)).read().strip().split("=")[-1]
                keep.add(v)
            except OSError:
                pass
    lifts = os.path.join(CACHE, "lift")
    if os.path.isdir(lifts):
        ids = sorted(os.listdir(lifts), key=lambda i: os.path.getmtime(os.path.join(lifts, i)))
        keep.update(ids[-2:])
        for i in ids:
            if i not in keep:
                rm(os.path.join(lifts, i), "lift not pinned and not among 2 newest")
    now = time.time()
    for runs in (os.path.join(CACHE, "runs"), os.path.join(ROOT, "tests", "runs")):
        if os.path.isdir(runs):
            for r in os.listdir(runs):
                p = os.path.join(runs, r)
                if now - os.path.getmtime(p) > 86400:
                    rm(p, "test run older than 24 h")
    outs = os.path.join(CACHE, "out")
    live = {os.path.basename(t) for t in trees}
    if os.path.isdir(outs):
        for o in os.listdir(outs):
            p = os.path.join(outs, o)
            # A build dir without a worktree may still be in use (scratch builds, soak snapshots).
            if o not in live and not recently_used(p, now, 6 * 3600):
                rm(p, "worktree gone, unused for 6 h")
    if shutil.which("ccache") or os.path.exists("/opt/homebrew/bin/ccache"):
        cc = shutil.which("ccache") or "/opt/homebrew/bin/ccache"
        env = dict(os.environ, CCACHE_DIR=os.path.join(CACHE, "ccache"))
        if not DRY:
            subprocess.run([cc, "--max-size=10G"], env=env, capture_output=True)
            subprocess.run([cc, "--cleanup"], env=env, capture_output=True)
        print("ccache trimmed to 10 GB")


if __name__ == "__main__":
    main()
