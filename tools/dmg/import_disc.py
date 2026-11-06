#!/usr/bin/env python3
"""First-run import for a HP2.app without game data (the DMG build, tools/make_dmg.sh).

    python3 -I import_disc.py <disc.iso> <data dir>

Extracts the player's own disc image into <data>/game/extracted (XDVDFS, XboxPortKit xiso.py)
and builds the Ukrainian layer <data>/mods (_UK, _UKT, fonts) from it (tools/loc/build_mod.py and
our sources in loc/uk). The _UK slot speaks English: the Ukrainian voice banks are made from disc
audio and are not in the DMG.
Nothing of the game is in the DMG; everything here comes from the player's disc.
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import xiso  # noqa: E402


REGIONS = {1: "US", 2: "JP", 4: "EU"}


def region(xbe):
    """XBE certificate game region (dwGameRegion, cert + 0xA0)."""
    with open(xbe, "rb") as f:
        d = f.read(0x1000)
    base, cert = struct.unpack_from("<I", d, 0x104)[0], struct.unpack_from("<I", d, 0x118)[0]
    with open(xbe, "rb") as f:
        f.seek(cert - base + 0xA0)
        r = struct.unpack("<I", f.read(4))[0]
    return "+".join(n for b, n in REGIONS.items() if r & b) or hex(r)


def main():
    iso, data = sys.argv[1], sys.argv[2]
    dest = os.path.join(data, "game", "extracted")
    os.makedirs(dest, exist_ok=True)
    print(f"import: extracting {iso} -> {dest}", flush=True)
    xiso.extract(iso, dest, log=lambda *a: None)
    xbe = os.path.join(dest, "default.xbe")
    if not os.path.isfile(xbe):
        print("import: no default.xbe on this disc: not Harry Potter and the Chamber of Secrets?", flush=True)
        return 3
    reg = region(xbe)
    print(f"import: disc region {reg}", flush=True)
    if "US" not in reg:
        # The port is lifted from the US disc; the EU (PAL) disc is supported by its known
        # differences (same files, 50/60 Hz choice, other languages) but NOT TESTED.
        print("import: EU/PAL disc: не перевірено (untested)", flush=True)
    with open(os.path.join(data, "region.txt"), "w") as f:
        f.write(reg + "\n")
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "loc", "build_mod.py"), "uk",
                        "--data", data, "--out", os.path.join(data, "mods"),
                        "--src", os.path.join(HERE, "loc", "uk")])
    if r.returncode:
        print("import: Ukrainian layer not built (English still works)", flush=True)
    print("import: done", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
