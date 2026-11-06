#!/usr/bin/env python3
"""shot_check.py shot.png [previous.png]

Smoke check of one screenshot: not blank (enough brightness and detail) and,
with a previous shot of the same run, not frozen (differs from it).
Prints the numbers; exit 0 ok, 1 blank/frozen, 2 error.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_diff import read_png  # noqa: E402


def stats(path):
    w, h, px = read_png(path)
    n = w * h
    lum = [(px[3 * i] * 3 + px[3 * i + 1] * 6 + px[3 * i + 2]) // 10 for i in range(0, n, 7)]
    mean = sum(lum) / len(lum)
    var = sum((v - mean) ** 2 for v in lum) / len(lum)
    return px, mean, var ** 0.5


try:
    px, mean, sd = stats(sys.argv[1])
    diff = None
    if len(sys.argv) > 2 and os.path.exists(sys.argv[2]):
        prev = read_png(sys.argv[2])[2]
        diff = sum(1 for i in range(0, len(px), 21) if abs(px[i] - prev[i]) > 12) / (len(px) / 21)
except (OSError, ValueError) as e:
    print(f"shot_check: {e}")
    sys.exit(2)
blank = mean < 6 or sd < 4
frozen = diff is not None and diff < 0.002
print(f"shot_check: mean {mean:.1f}, sd {sd:.1f}" + (f", {diff*100:.2f}% changed" if diff is not None else "")
      + (" -> BLANK" if blank else "") + (" -> FROZEN" if frozen else ""))
sys.exit(1 if blank or frozen else 0)
