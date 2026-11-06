#!/usr/bin/env python3
"""hdui_check.py RUN GOLDEN UPDATE [--prefix P] name:frame:w:h:y:x ...

Crops RUN/{before,after}/frame_<frame>.png, checks the HD UI pack changed the
crop without moving anything (see tests/scripts/hdui_pack.sh), writes the
crops as GOLDEN/hdui_<name>_{before,after}.png (UPDATE=1 or missing) or
compares the after crop with its golden (--prefix: P_<name>, default hdui;
tests/scripts/hdworld_pack.sh uses hdworld). Exit 0 ok, 1 failed.
"""
import os
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_diff import read_png  # noqa: E402


def write_png(path, w, h, rgb):
    raw = b"".join(b"\0" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    chunk = lambda k, d: struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def crop(img, w, h, y, x):
    W, H, px = img
    out = bytearray(w * h * 3)
    for r in range(h):
        out[r * w * 3:(r + 1) * w * 3] = px[((y + r) * W + x) * 3:((y + r) * W + x + w) * 3]
    return out


def shrink(c, w, h, k=8):
    out = []
    for by in range(h // k):
        for bx in range(w // k):
            for ch in range(3):
                s = 0
                for yy in range(by * k, by * k + k):
                    row = (yy * w + bx * k) * 3 + ch
                    s += sum(c[row:row + k * 3:3])
                out.append(s / (k * k))
    return out


def main():
    run, golden, update = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
    specs, prefix = sys.argv[4:], "hdui"
    if specs[:1] == ["--prefix"]:
        prefix, specs = specs[1], specs[2:]
    ok = True
    for spec in specs:
        name, frame, w, h, y, x = spec.split(":")
        w, h, y, x = int(w), int(h), int(y), int(x)
        cb = crop(read_png(f"{run}/before/frame_{frame}.png"), w, h, y, x)
        ca = crop(read_png(f"{run}/after/frame_{frame}.png"), w, h, y, x)
        changed = sum(abs(a - b) for a, b in zip(ca, cb)) / len(ca)
        sb, sa = shrink(cb, w, h), shrink(ca, w, h)
        moved = sum(abs(a - b) for a, b in zip(sa, sb)) / len(sa)
        print(f"  {name}: changed {changed:.2f}, low-frequency diff {moved:.2f}")
        if changed <= 0.3:
            print(f"  FAIL: {name}: the pack did not change the crop")
            ok = False
        if moved >= 4:
            print(f"  FAIL: {name}: the crop moved / changed shape")
            ok = False
        gb, ga = f"{golden}/{prefix}_{name}_before.png", f"{golden}/{prefix}_{name}_after.png"
        tmp = f"{run}/{prefix}_{name}_after.png"
        write_png(tmp, w, h, ca)
        if update or not os.path.exists(ga):
            write_png(gb, w, h, cb)
            write_png(ga, w, h, ca)
            print(f"  golden updated: {ga}")
        elif subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), "png_diff.py"), tmp, ga]).returncode:
            print(f"  FAIL: {name}: after differs from {ga}")
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
