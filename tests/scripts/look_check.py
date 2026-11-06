#!/usr/bin/env python3
"""look_check.py look.png modern.png

Checks a frame of a post-processing look (HP2_LOOK) against the plain present
(modern) of the same game frame; the sizes may differ (means are compared).
  - mean luma within +-25% of modern's,
  - not black / not saturated: the share of pure black and pure white pixels
    at most modern's + 2 points (a NaN in the shader comes out as black),
  - the look differs from modern (mean absolute luma change of a 64x48 grid > 0.5).
Prints the numbers; exit 0 ok, 1 failed, 2 error.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_diff import read_png  # noqa: E402


def stats(path):
    w, h, px = read_png(path)
    lum = [0.0] * (w * h)
    black = white = 0
    for i in range(w * h):
        r, g, b = px[3 * i], px[3 * i + 1], px[3 * i + 2]
        lum[i] = 0.299 * r + 0.587 * g + 0.114 * b
        black += r == 0 and g == 0 and b == 0
        white += r == 255 and g == 255 and b == 255
    grid = []
    for gy in range(48):
        for gx in range(64):
            x0, x1 = gx * w // 64, (gx + 1) * w // 64
            y0, y1 = gy * h // 48, (gy + 1) * h // 48
            s = sum(lum[y * w + x] for y in range(y0, y1) for x in range(x0, x1))
            grid.append(s / ((x1 - x0) * (y1 - y0)))
    n = w * h
    return sum(lum) / n, black / n, white / n, grid


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    try:
        lm, bl, wh, g = stats(sys.argv[1])
        rm, rbl, rwh, rg = stats(sys.argv[2])
    except (OSError, ValueError) as e:
        print(f"look_check: {e}")
        return 2
    diff = sum(abs(a - b) for a, b in zip(g, rg)) / len(g)
    ratio = lm / rm if rm else 0
    print(f"  mean luma {lm:.1f} (modern {rm:.1f}, x{ratio:.3f}); black {bl:.2%} (modern {rbl:.2%}); "
          f"white {wh:.2%} (modern {rwh:.2%}); grid change {diff:.2f}")
    ok = True
    if not 0.75 <= ratio <= 1.25:
        print("  FAIL: mean luma off by more than 25%")
        ok = False
    if bl > rbl + 0.02:
        print("  FAIL: black pixels (NaN?)")
        ok = False
    if diff <= 0.5:
        print("  FAIL: the look does not change the picture")
        ok = False
    if wh > rwh + 0.02:
        print("  FAIL: saturated pixels")
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
