#!/usr/bin/env python3
"""png_diff.py a.png b.png [max_mean] [max_bad_fraction]

PNG_DIFF_RECT=x0,y0,x1,y1 compares only that rectangle (e.g. the static
part of an animated screen).

Compares two 8-bit RGB/RGBA PNGs (no third-party modules). Prints the mean
absolute channel difference and the fraction of pixels differing by more
than 24 in any channel. Exit 0 if mean <= max_mean (default 2.0) and the
bad fraction <= max_bad_fraction (default 0.01), 1 otherwise, 2 on error.
"""
import os
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError(f"{path}: unsupported PNG (depth {depth}, type {ctype})")
            bpp = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(h * w * 3)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        for x in range(w):
            out[(y * w + x) * 3:(y * w + x) * 3 + 3] = line[x * bpp:x * bpp + 3]
        prev = line
    return w, h, out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    max_mean = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
    max_bad = float(sys.argv[4]) if len(sys.argv) > 4 else 0.01
    try:
        wa, ha, a = read_png(sys.argv[1])
        wb, hb, b = read_png(sys.argv[2])
    except (OSError, ValueError, zlib.error) as e:
        print(f"png_diff: {e}")
        return 2
    if (wa, ha) != (wb, hb):
        print(f"png_diff: size {wa}x{ha} != {wb}x{hb}")
        return 1
    x0, y0, x1, y1 = 0, 0, wa, ha
    rect = os.environ.get("PNG_DIFF_RECT")
    if rect:
        x0, y0, x1, y1 = (int(v) for v in rect.split(","))
    total, bad, n = 0, 0, 0
    for y in range(max(0, y0), min(ha, y1)):
      for x in range(max(0, x0), min(wa, x1)):
        i = (y * wa + x) * 3
        n += 1
        d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
        total += abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2])
        bad += d > 24
    mean = total / max(1, 3 * n)
    frac = bad / max(1, n)
    ok = mean <= max_mean and frac <= max_bad
    print(f"png_diff: mean {mean:.3f}, {frac * 100:.3f}% pixels off -> {'same' if ok else 'DIFFERENT'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
