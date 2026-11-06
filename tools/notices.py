#!/usr/bin/env python3
"""THIRD_PARTY_NOTICES.txt for what a DMG/app redistributes (MIT/BSD/zlib keep their notice in binary copies).

  tools/notices.py OUT.txt [--sdl3]

Collects, in this order: every LICENSE*/COPYING*/NOTICE* file of third_party/*/ and third_party/deps/*-src/
(and their dependencies/*/), the SDL3 license from Homebrew when --sdl3 (the app ships libSDL3), and every
inline notice block of patches/*.patch (a contiguous comment block that contains "Copyright"), e.g. AMD FSR1.
Exit 1 if nothing was found (a DMG without notices is a release FAIL: publish_check --release).
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIC = re.compile(r"^(LICEN[CS]E|COPYING|NOTICE)([._-].*)?$", re.I)


def licence_files(d):
    out = []
    for base in [d] + sorted(glob.glob(os.path.join(d, "dependencies", "*"))):
        if os.path.isdir(base):
            out += [os.path.join(base, f) for f in sorted(os.listdir(base)) if LIC.match(f)]
    return out


def patch_blocks(path):
    blocks, cur = [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"^\+\s*(//|#|\*|/\*)\s?(.*)$", line.rstrip("\n"))
        if m:
            cur.append(m.group(2))
            continue
        if any("Copyright" in c for c in cur):
            blocks.append("\n".join(cur).strip())
        cur = []
    if any("Copyright" in c for c in cur):
        blocks.append("\n".join(cur).strip())
    return blocks


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out, parts = sys.argv[1], []
    dirs = sorted(glob.glob(os.path.join(ROOT, "third_party", "*"))) + \
        sorted(glob.glob(os.path.join(ROOT, "third_party", "deps", "*-src")))
    for d in dirs:
        for f in licence_files(d):
            name = os.path.relpath(f, ROOT)
            parts.append(f"== {name}\n\n" + open(f, encoding="utf-8", errors="replace").read().strip())
    if "--sdl3" in sys.argv:
        for f in ("/opt/homebrew/opt/sdl3/LICENSE.txt", "/usr/local/opt/sdl3/LICENSE.txt"):
            if os.path.exists(f):
                parts.append("== SDL3 (libSDL3.dylib)\n\n" + open(f, encoding="utf-8").read().strip())
                break
        else:
            print("notices: --sdl3 but no SDL3 LICENSE.txt found", file=sys.stderr)
            return 1
    for p in sorted(glob.glob(os.path.join(ROOT, "patches", "*.patch"))):
        for b in patch_blocks(p):
            parts.append(f"== {os.path.relpath(p, ROOT)} (inline notice)\n\n{b}")
    if not parts:
        print("notices: no third-party license found", file=sys.stderr)
        return 1
    head = ("Third-party software in this app and the licenses it is used under.\n"
            "Our own code: MIT (LICENSE in the repository). No game data or game code is included.\n\n")
    with open(out, "w", encoding="utf-8") as fh:
        fh.write(head + "\n\n".join(parts) + "\n")
    print(f"notices: {len(parts)} notices -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
