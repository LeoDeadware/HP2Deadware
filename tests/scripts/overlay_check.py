#!/usr/bin/env python3
"""overlay_check.py shot.png

Detects the game's "PLEASE RECONNECT THE CONTROLLER TO PORT 1 / AND PRESS
START TO CONTINUE" overlay (white text across y 240..282 of a 640x480 frame):
counts near-white pixels (all channels >= 230) in that band. Prints the count;
exit 1 if the overlay is present (count >= 200), 0 if not, 2 on error.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_diff import read_png  # noqa: E402


def main():
    try:
        w, h, px = read_png(sys.argv[1])
    except (IndexError, OSError, ValueError) as e:
        print(f"overlay_check: {e}")
        return 2
    sy = h / 480.0

    def white(y0, y1):
        n = 0
        for y in range(int(y0 * sy), int(y1 * sy)):
            for x in range(int(45 * w / 640), int(595 * w / 640)):
                i = (y * w + x) * 3
                if px[i] >= 230 and px[i + 1] >= 230 and px[i + 2] >= 230:
                    n += 1
        return n

    n = white(240, 282)
    around = white(190, 238) + white(284, 330)
    present = n >= 200 * sy * sy and around < 50 * sy * sy
    print(f"overlay_check: {n} white pixels in the message band, {around} around it -> "
          f"{'RECONNECT OVERLAY' if present else 'no overlay'}")
    return 1 if present else 0


if __name__ == "__main__":
    sys.exit(main())
