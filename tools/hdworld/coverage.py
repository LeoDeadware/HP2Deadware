#!/usr/bin/env python3
"""coverage.py RUN_DIR... [--out FILE] [--from FRAME]

Screen coverage of every texture in HP2_HDUI_PROBE=1 runs (the level dirs
NN_<name>/ of tests/scripts/levels_smoke.sh, shots via HP2_SHOT_EVERY): each
pixel of a shot names its texture by colour (src/polish/texpack/texpack.mm),
counted by probe_count.swift. A texture's score = the sum over levels of its
mean share of the screen in that level's shots (frame >= --from, default 1500:
in the level, not the front-end). Writes a TSV sorted by score:
hash w h fmt levels score level:share,...  (default .cache/HP2Cache/hdworld/coverage.tsv)
"""
import argparse
import collections
import glob
import os
import re
import subprocess
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))  # tools/
import hp2paths  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(hp2paths.CACHE, 'hdworld')
PROBE = re.compile(r"hd_ui probe (\d+) code (\d+) = ([0-9a-f]{16}) (\d+)x(\d+) fmt ([0-9A-F]+) levels (\d+)")


def counter():
    exe = os.path.join(WORK, "probe_count")
    src = os.path.join(HERE, "probe_count.swift")
    if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
        os.makedirs(WORK, exist_ok=True)
        subprocess.run(["swiftc", "-O", "-o", exe, src], check=True)
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--out", default=os.path.join(WORK, "coverage.tsv"))
    ap.add_argument("--from", dest="first", type=int, default=1500)
    a = ap.parse_args()
    exe = counter()
    info = {}  # hash -> (w, h, fmt, levels)
    share = collections.defaultdict(dict)  # hash -> level -> mean share
    for run in a.runs:
        for d in sorted(glob.glob(os.path.join(run, "[0-9][0-9]_*"))):
            level = os.path.basename(d)
            if not os.path.exists(os.path.join(d, "runtime.log")):
                continue
            code = {}  # the latest texture given each code (codes wrap at 1023)
            for line in open(os.path.join(d, "runtime.log"), errors="replace"):
                m = PROBE.search(line)
                if m:
                    n, c, h, w, hh, f, lv = m.groups()
                    code[int(c)] = h
                    info[h] = (int(w), int(hh), f, int(lv))
            shots = [p for p in sorted(glob.glob(os.path.join(d, "frame_*.png")))
                     if int(re.search(r"frame_(\d+)", p).group(1)) >= a.first]
            if not shots:
                continue
            out = subprocess.run([exe] + shots, capture_output=True, text=True, check=True).stdout
            acc = collections.Counter()
            for line in out.splitlines():
                _, c, n, total = line.split("\t")
                if int(c) in code:
                    acc[code[int(c)]] += int(n) / int(total)
            for h, s in acc.items():
                share[h][level] = s / len(shots)
            print(f"  {level}: {len(shots)} shots, {len(acc)} textures seen", file=sys.stderr)
    rows = sorted(share, key=lambda h: -sum(share[h].values()))
    with open(a.out, "w") as f:
        f.write("# hash\tw\th\tfmt\tlevels\tscore\tper level (mean share of the screen)\n")
        for h in rows:
            w, hh, fm, lv = info[h]
            per = ",".join(f"{k}:{v:.4f}" for k, v in sorted(share[h].items(), key=lambda kv: -kv[1]))
            f.write(f"{h}\t{w}\t{hh}\t{fm}\t{lv}\t{sum(share[h].values()):.4f}\t{per}\n")
    print(f"coverage: {len(rows)} textures -> {a.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
