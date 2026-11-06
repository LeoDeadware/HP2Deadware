#!/usr/bin/env python3
"""pad_fuzz.py seed start_frame end_frame > script.pad

Seeded pseudo-random but plausible controller input for soak tests, in the
XInput HLE's HP2_PAD_SCRIPT syntax (src/hle/xinput/pad_script.h). From
start_frame to end_frame: walks (left stick), camera (right stick), jumps (A),
spells (B, sometimes held), X/Y, triggers, short idles, and pauses (START,
later START again to resume). Never presses D-pad down/up while paused, so it
does not pick "Quit Game" in the pause menu. Same seed -> same script.
"""
import math
import random
import sys


def main():
    if len(sys.argv) != 4:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    seed, start, end = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    rng = random.Random(seed)
    out = [f"# pad_fuzz.py seed {seed}, frames {start}..{end}"]
    f = start
    actions = [("walk", 30), ("camera", 8), ("jump", 14), ("spell", 14), ("hold_spell", 4),
               ("xy", 6), ("trigger", 4), ("idle", 8), ("pause", 2), ("walk_jump", 10)]
    names = [a for a, _ in actions]
    weights = [w for _, w in actions]
    while f < end - 200:
        act = rng.choices(names, weights)[0]
        if act in ("walk", "walk_jump"):
            ang = rng.uniform(0, 2 * math.pi)
            mag = rng.uniform(0.5, 1.0)
            n = rng.randint(30, 240)
            out.append(f"{f} stick L {mag * math.cos(ang):.2f} {mag * math.sin(ang):.2f} {n}")
            if act == "walk_jump":
                out.append(f"{f + rng.randint(5, n - 5)} press A")
            f += n + rng.randint(1, 20)
        elif act == "camera":
            n = rng.randint(15, 90)
            out.append(f"{f} stick R {rng.uniform(-1, 1):.2f} {rng.uniform(-0.5, 0.5):.2f} {n}")
            f += n + rng.randint(1, 10)
        elif act == "jump":
            out.append(f"{f} press A")
            f += rng.randint(20, 60)
        elif act == "spell":
            out.append(f"{f} press B")
            f += rng.randint(20, 80)
        elif act == "hold_spell":
            n = rng.randint(30, 120)
            out.append(f"{f} hold B {n}")
            f += n + rng.randint(10, 40)
        elif act == "xy":
            out.append(f"{f} press {rng.choice('XY')}")
            f += rng.randint(20, 60)
        elif act == "trigger":
            n = rng.randint(20, 90)
            out.append(f"{f} trigger {rng.choice('LR')} 1 {n}")
            f += n + rng.randint(5, 20)
        elif act == "idle":
            f += rng.randint(30, 150)
        elif act == "pause":
            out.append(f"{f} press START")
            f += rng.randint(40, 180)
            out.append(f"{f} press START")
            f += rng.randint(30, 60)
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
