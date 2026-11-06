#!/usr/bin/env python3
"""build_pack.py DUMP_DIR [DUMP_DIR...] [--out DIR] [--only HASH,...]

Builds the HD UI texture pack (src/polish/texpack) from a texture dump
(HP2_HDUI_DUMP=DUMP_DIR, a run that shows the screens) and the list
tools/hdui/ui_textures.tsv: hdfix split -> Real-ESRGAN x4 -> hdfix join.
Modes: "art" (realesrgan-x4plus-anime, tone corrected to the original),
"ink" (same model, uncorrected: text), "photo" (realesrgan-x4plus, corrected).
Output: <hash>.png in --out (default $HP2_DATA/mods/hd_ui, which the game
loads by default). The pack is derived from the game's data: never committed.

Real-ESRGAN: .cache/HP2Cache/tools/realesrgan/x (official release v0.2.5.0,
realesrgan-ncnn-vulkan-20220424-macos.zip from github.com/xinntao/Real-ESRGAN).
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
ESRGAN = os.path.join(hp2paths.CACHE, 'tools/realesrgan/x')
MODELS = {"art": "realesrgan-x4plus-anime", "ink": "realesrgan-x4plus-anime", "photo": "realesrgan-x4plus"}


def entries():
    for line in open(os.path.join(HERE, "ui_textures.tsv")):
        if line.startswith("#") or not line.strip():
            continue
        h, mode, size, name = line.rstrip("\n").split("\t")
        yield h, mode, size, name


def tool(name, work):
    """Compiles a Swift tool once into work/ (swiftc -O)."""
    exe = os.path.join(work, name)
    if not os.path.exists(exe):
        subprocess.run(["swiftc", "-O", "-o", exe, os.path.join(HERE, name + ".swift")], check=True)
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump", nargs="+")
    ap.add_argument("--out", default=os.path.join(hp2paths.DATA,
                                                  "mods", "hd_ui"))
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    only = set(filter(None, a.only.split(",")))
    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="hdui-")
    hdfix = tool("hdfix", work)
    todo = [e for e in entries() if e[1] in MODELS and (not only or e[0] in only)]
    def src(h):
        for d in a.dump:
            if os.path.exists(os.path.join(d, h + ".png")):
                return os.path.join(d, h + ".png")
        return None
    missing = [e for e in todo if not src(e[0])]
    for h, _, size, name in missing:
        print(f"  missing in the dump: {h} {size} {name}")
    todo = [e for e in todo if e not in missing]
    for model in sorted(set(MODELS.values())):
        batch = [e for e in todo if MODELS[e[1]] == model]
        if not batch:
            continue
        inp, up = os.path.join(work, model, "in"), os.path.join(work, model, "up")
        os.makedirs(inp), os.makedirs(up)
        for h, *_ in batch:
            subprocess.run([hdfix, "split", src(h), os.path.join(inp, h + "_c.png"),
                            os.path.join(inp, h + "_a.png")], check=True)
        subprocess.run([os.path.join(ESRGAN, "realesrgan-ncnn-vulkan"), "-i", inp, "-o", up, "-n", model, "-m",
                        os.path.join(ESRGAN, "models"), "-f", "png"],
                       check=True, cwd=work, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for h, mode, size, name in batch:
            subprocess.run([hdfix, "join", src(h), os.path.join(up, h + "_c.png"),
                            os.path.join(up, h + "_a.png"), os.path.join(a.out, h + ".png")] +
                           (["--ink"] if mode == "ink" else []), check=True)
            print(f"  {mode:5s} {h} {size} x4  {name}")
    shutil.rmtree(work)
    print(f"pack: {a.out} ({len(todo)} textures, {len(missing)} missing)")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
