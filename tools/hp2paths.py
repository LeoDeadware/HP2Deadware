"""Where the untracked data lives, for the dev tools (no home paths in the repo).

MAIN   the main checkout (worktrees share its game/ and .cache/; tools/main_checkout.sh)
DATA   $HP2_DATA, else MAIN (holds game/extracted)
CACHE  $HP2_CACHE, else MAIN/.cache/HP2Cache
"""
import os
import subprocess

_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _main():
    try:
        g = subprocess.run(["git", "-C", _REPO, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                           capture_output=True, text=True).stdout.strip()
    except OSError:
        g = ""
    return os.path.dirname(g) if g and os.path.basename(g) == ".git" else _REPO


MAIN = _main()
DATA = os.environ.get("HP2_DATA") or MAIN
CACHE = os.environ.get("HP2_CACHE") or os.path.join(MAIN, ".cache", "HP2Cache")
