#!/usr/bin/env python3
"""Turns dispatch misses (an indirect branch to code the lift does not have)
into lift/manifest.txt entries.

The runtime logs every miss as
  hp2: dispatch-miss target=XXXXXXXX from=YYYYYYYY
(and FATAL "indirect branch to unlifted address X from Y", also in crash
reports). This scans the given logs / directories (recursively: runtime.log,
hp2-crash-*.txt), drops targets the manifest already has, and prints the
entries; --apply appends them to the manifest. Then re-lift: lift/lift.sh --pin.

  lift/miss_roots.py [--apply] LOG_OR_DIR...

Prefer a lifter fix when the misses share a cause (a jump-table or callback
pattern the lifter does not recognise): an entry only covers what was seen.
"""
import os
import re
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../tools'))  # tools/
import hp2paths  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "lift", "manifest.txt")
PATS = [re.compile(rb"dispatch-miss target=([0-9A-Fa-f]{8}) from=([0-9A-Fa-f]{8})"),
        re.compile(rb"indirect branch to unlifted address ([0-9A-Fa-f]{8}) from ([0-9A-Fa-f]{8})")]


def files(paths):
    for p in paths:
        if os.path.isdir(p):
            for d, _, names in os.walk(p):
                for n in names:
                    if n == "runtime.log" or n.startswith("hp2-crash-") or n.endswith(".log"):
                        yield os.path.join(d, n)
        else:
            yield p


def main(argv):
    apply = "--apply" in argv
    paths = [a for a in argv[1:] if a != "--apply"]
    if not paths:
        print(__doc__)
        return 2
    have = set()
    for line in open(MANIFEST, encoding="utf-8"):
        parts = line.split("#")[0].split()
        if len(parts) >= 2 and parts[0] in ("entry", "name", "resume", "sehprolog"):
            have.add(int(parts[1], 16))
    misses = {}
    for f in files(paths):
        data = open(f, "rb").read()
        for pat in PATS:
            for m in pat.finditer(data):
                t, fr = int(m.group(1), 16), int(m.group(2), 16)
                misses.setdefault(t, (fr, f))
    # Also skip what the pinned lift already reaches (a function or a label
    # inside one, e.g. a switch case added by a lifter fix since the log).
    lifted = set()
    lift_id = open(os.path.join(ROOT, "lift", "LIFT_ID")).read().strip().split("=")[-1]
    gen = os.path.join(os.path.dirname(hp2paths.CACHE), "HP2Cache", "lift", lift_id, "generated")
    if os.path.isdir(gen):
        lab = re.compile(rb"(?:^L_|^void f_)([0-9A-F]{8})", re.M)
        for n in os.listdir(gen):
            if n.endswith(".cpp"):
                lifted.update(int(m.group(1), 16) for m in lab.finditer(open(os.path.join(gen, n), "rb").read()))
    covered = [t for t in misses if t in lifted and t not in have]
    for t in covered:
        print(f"# {t:08X}: already in lift {lift_id}", file=sys.stderr)
    new = {t: v for t, v in sorted(misses.items()) if t not in have and t not in lifted}
    lines = [f"entry  {t:08X}  # dispatch miss from {fr:08X} ({os.path.relpath(f, os.path.expanduser('~'))})"
             for t, (fr, f) in new.items()]
    for l in lines:
        print(l)
    print(f"{len(misses)} miss target(s), {len(new)} new", file=sys.stderr)
    if apply and lines:
        with open(MANIFEST, "a", encoding="utf-8") as m:
            m.write("\n# ---- dispatch misses (lift/miss_roots.py) ----\n" + "\n".join(lines) + "\n")
        print(f"appended to {MANIFEST}; now lift/lift.sh --pin", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
