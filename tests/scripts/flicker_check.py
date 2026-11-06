#!/usr/bin/env python3
"""flicker_check.py frame1.png frame2.png ... (consecutive frames)

Counts one-frame spikes: pixels whose luma jumps away and back
(|b-a| > T, |b-c| > T, same direction, |a-c| < T/2 for frames a, b, c).
A smooth camera pan moves edges monotonically and makes few; z-fighting
(coplanar surfaces trading places frame to frame) makes many.
FLICKER_RECT=x0,y0,x1,y1 limits the count (default: the whole frame).
Prints the spike fraction per frame triple and the worst; exit 1 if the worst
exceeds FLICKER_MAX (default 0.002).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_diff import read_png  # noqa: E402

T = 40


def luma(img):
    w, h, px = img
    return w, h, [(px[i] * 77 + px[i + 1] * 150 + px[i + 2] * 29) >> 8 for i in range(0, len(px), 3)]


def main():
    files = sys.argv[1:]
    if len(files) < 3:
        print("flicker_check: need at least 3 frames")
        return 2
    frames = [luma(read_png(f)) for f in files]
    w, h, _ = frames[0]
    x0, y0, x1, y1 = 0, 0, w, h
    if os.environ.get("FLICKER_RECT"):
        x0, y0, x1, y1 = (int(v) for v in os.environ["FLICKER_RECT"].split(","))
    idx = [y * w + x for y in range(y0, y1) for x in range(x0, x1)]
    worst, total = 0.0, 0.0
    for k in range(1, len(frames) - 1):
        a, b, c = frames[k - 1][2], frames[k][2], frames[k + 1][2]
        n = 0
        for i in idx:
            d1, d2 = b[i] - a[i], b[i] - c[i]
            if (d1 > T and d2 > T or d1 < -T and d2 < -T) and abs(a[i] - c[i]) < T // 2:
                n += 1
        f = n / len(idx)
        total += f
        worst = max(worst, f)
    mean = total / (len(frames) - 2)
    limit = float(os.environ.get("FLICKER_MAX", "0.002"))
    print(f"flicker_check: {len(frames)} frames, spikes mean {mean * 100:.3f}%, "
          f"worst {worst * 100:.3f}% of pixels (limit {limit * 100:.3f}%)")
    return 0 if worst <= limit else 1


if __name__ == "__main__":
    sys.exit(main())
