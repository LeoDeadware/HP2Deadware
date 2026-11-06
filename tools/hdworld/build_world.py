#!/usr/bin/env python3
"""build_world.py COVERAGE.tsv DUMP_DIR [DUMP_DIR...] [--out DIR] [--top N] [--budget MB] [--must PRE:SHARE]
                 [--only HASH,...]

Builds the HD world texture pack (src/polish/texpack, HP2_HDWORLD) from the
coverage ranking of tools/hdworld/coverage.py and a texture dump
(HP2_HDUI_DUMP): the N (default 200) world textures that cover the most of the screen
(not the UI pack's, at least 16x16), per texture:
  hdfix pad (16 texels, wrapped when the texture tiles, else edge-repeated)
  -> hdfix split -> Real-ESRGAN x4 (realesrgan-x4plus) -> hdfix join (tone
  corrected to the original) -> hdfix crop (the border off; --half for x2).
Scale: x4 up to 128 texels, x2 up to 512, larger ones are skipped; the total
(level 0, RGBA8) is kept under --budget (default 512 MB): VRAM (+1/3 for the
mips the D3D8 HLE builds) of the textures a level binds; not RAM (texpack
does not keep the images). Built first, whatever their rank: the hashes in
tools/hdworld/must_textures.tsv (--must-list) and --must PREFIX:SHARE.
Output: <hash>.png + manifest.tsv in --out (default .cache/HP2Cache/hdworld/pack;
for the player $HP2_DATA/mods/hd_world). Derived from the game's data: never
committed. Real-ESRGAN: see tools/hdui/build_pack.py.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))  # tools/
import hp2paths  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
HDUI = os.path.join(HERE, "..", "hdui")
ESRGAN = os.path.join(hp2paths.CACHE, 'tools/realesrgan/x')
MODEL = "realesrgan-x4plus"
PAD = 16


def ui_hashes():
    out = set()
    for line in open(os.path.join(HDUI, "ui_textures.tsv")):
        if line.strip() and not line.startswith("#"):
            out.add(line.split("\t")[0])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("coverage")
    ap.add_argument("dump", nargs="+")
    ap.add_argument("--out", default=os.path.join(hp2paths.CACHE, 'hdworld/pack'))
    ap.add_argument("--top", type=int, default=200)
    ap.add_argument("--budget", type=float, default=512)
    ap.add_argument("--only", default="")
    ap.add_argument("--must-list", default=os.path.join(HERE, "must_textures.tsv"))
    ap.add_argument("--must", action="append", default=[],
                    help="LEVEL_PREFIX:SHARE (repeatable): every texture whose summed share in the levels "
                         "named so is >= SHARE is built first, beyond --top (e.g. 05_:0.0005 = Diagon Alley)")
    a = ap.parse_args()
    only = set(filter(None, a.only.split(",")))
    ui = ui_hashes()

    def src(h):
        for d in a.dump:
            p = os.path.join(d, h + ".png")
            if os.path.exists(p):
                return p
        return None

    rows = []
    for line in open(a.coverage):
        if line.startswith("#"):
            continue
        h, w, hh, fmt, lv, score, per = line.rstrip("\n").split("\t")
        shares = {k: float(v) for k, v in (x.rsplit(":", 1) for x in per.split(","))}
        rows.append((h, int(w), int(hh), float(score), per.split(",")[0], shares))

    listed = {}  # tools/hdworld/must_textures.tsv: hash -> (w, h, level)
    for line in open(a.must_list):
        if line.strip() and not line.startswith("#"):
            h, w, hh, lvl = line.rstrip("\n").split("\t")
            listed[h] = (int(w), int(hh), lvl)
    known = {r[0] for r in rows}
    rows += [(h, w, hh, 0.0, lvl, {}) for h, (w, hh, lvl) in listed.items() if h not in known]

    def must(r):  # listed, or --must PREFIX:SHARE: in the set whatever its global rank
        if r[0] in listed:
            return True
        for m in a.must:
            pre, thr = m.rsplit(":", 1)
            if sum(v for k, v in r[5].items() if k.startswith(pre)) >= float(thr):
                return True
        return False

    todo, total, seen, nforced, nglobal = [], 0, set(), 0, 0
    forced = [r for r in rows if must(r)]
    # the --must textures, then the global ranking up to --top (counting those
    # already in): the set is the union, cut by --budget
    for i, r in enumerate(forced + rows):
        h, w, hh, score, top, _ = r
        glob_phase = i >= len(forced)
        if glob_phase and nglobal >= a.top:
            break
        if h in seen:
            nglobal += glob_phase
            continue
        if (only and h not in only) or h in ui or min(w, hh) < 16 or not src(h):
            continue
        k = 4 if max(w, hh) <= 128 else 2 if max(w, hh) <= 512 else 0
        if not k:
            continue
        size = w * hh * k * k * 4
        if total + size > a.budget * 2**20:
            print(f"  budget {a.budget:.0f} MB reached")
            break
        seen.add(h)
        todo.append((h, w, hh, k, score, top))
        total += size
        nforced += not glob_phase
        nglobal += glob_phase
    print(f"{nforced} textures by --must, {len(todo)} in all")
    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="hdworld-")
    hdfix = os.path.join(work, "hdfix")
    subprocess.run(["swiftc", "-O", "-o", hdfix, os.path.join(HDUI, "hdfix.swift")], check=True)
    inp, up = os.path.join(work, "in"), os.path.join(work, "up")
    os.makedirs(inp), os.makedirs(up)
    modes = {}
    for h, *_ in todo:
        padded = os.path.join(work, h + "_p.png")
        modes[h] = subprocess.run([hdfix, "pad", src(h), padded, str(PAD), "auto"], check=True,
                                  capture_output=True, text=True).stdout.strip()
        subprocess.run([hdfix, "split", padded, os.path.join(inp, h + "_c.png"), os.path.join(inp, h + "_a.png")],
                       check=True)
    subprocess.run([os.path.join(ESRGAN, "realesrgan-ncnn-vulkan"), "-i", inp, "-o", up, "-n", MODEL, "-m",
                    os.path.join(ESRGAN, "models"), "-f", "png"],
                   check=True, cwd=work, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(os.path.join(a.out, "manifest.tsv"), "w") as man:
        man.write("# hash\tw\th\tscale\tedges\tscore\ttop level\n")
        for h, w, hh, k, score, top in todo:
            joined = os.path.join(work, h + "_j.png")
            subprocess.run([hdfix, "join", os.path.join(work, h + "_p.png"), os.path.join(up, h + "_c.png"),
                            os.path.join(up, h + "_a.png"), joined], check=True)
            subprocess.run([hdfix, "crop", joined, os.path.join(a.out, h + ".png"), str(PAD * 4)] +
                           (["--half"] if k == 2 else []), check=True)
            man.write(f"{h}\t{w}\t{hh}\t{k}\t{modes[h]}\t{score:.4f}\t{top}\n")
            print(f"  {h} {w}x{hh} x{k} {modes[h]:5s} {score:.4f} {top}")
    shutil.rmtree(work)
    gpu = total * 4 / 3
    print(f"pack: {a.out} ({len(todo)} textures, {total / 2**20:.0f} MB level 0 RGBA; "
          f"VRAM with mips {gpu / 2**20:.0f} MB if every one were bound at once)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
