#!/usr/bin/env python3
"""Builds a language overlay (e.g. mods/uk) from its sources and the disc.

Sources (committed), mods/loc/<lang>/:
  <bank>.tsv        the translation table (tools/loc/extract.py: id, ctx, max,
                    crc, uk); rows with an empty uk or {=} stay English.
  fonts.txt         which font .geo files get which glyphs:
                      chars <characters to add>
                      reuse <pairs: existing+new, e.g. AА BВ>
                      geo <file.geo> <source font for font 0>[;<font 1>...]
                      flag <file.geo> <texture index> <#rrggbb> <#rrggbb>...
                        (repaint a picker flag, geo_tex.repaint_flag)
                      splitflag <file.geo> <texture index> <font>;<left label>;<right label>
                        (hybrid slot flag: Union Jack | Ukrainian, geo_tex.split_flag)
                      hybrid <folder>
                        (a second slot folder, e.g. _UKT, for the picker's
                        hybrid row (Kernel: the FRA row, folder "ukt"): every
                        file of the disc's _UK slot, with this overlay's text
                        banks and the disc's ORIGINAL English speech, APFS
                        clones (cp -c) so it costs no disk space)
Output (generated, never committed): <out>/_UK/<bank>.bin and <out>/<file>.geo,
the files the Kernel's VFS serves over the disc for HP2_LANG=<lang>
(default out: $HP2_DATA/mods/<lang>). No sources -> nothing is written, and the
game runs unchanged.

  tools/loc/build_mod.py uk [--out DIR] [--data HP2_DATA] [--test-alphabet 1] [--text DIR]
                            [--voice DIR] [--src DIR (default mods/loc/<lang>)]

--voice DIR: Ukrainian speech banks (DIR/_UK/*.sdt, tools/voice/build_voice.py
output) cloned into <out>/_UK/, so the _UK slot speaks Ukrainian; the hybrid
folder keeps the disc's English speech either way. Without it the _UK slot
falls back to the disc's English speech.

--text DIR: sparse "id<TAB>text" <bank>.tsv overrides (textbank.py escapes),
applied after the table (test pages).
--sheets DIR: PNG per font file with every added character as the game's
glyph records draw it (red box = no record), for checking coverage.

--test-alphabet 1: test overlay. Every letter of every string in every bank
becomes the next letter of the Ukrainian alphabet (case kept; spaces, digits,
%x codes, \n and button glyphs stay), starting at a different letter per id, so
every screen shows Cyrillic in every font with the English layout. Text
overrides (--text) are applied on top; the table is not used.
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import geo_font  # noqa: E402
import geo_tex  # noqa: E402
import extract  # noqa: E402
import textbank  # noqa: E402

SLOT = extract.SLOT
UPPER = "АБВГҐДЕЄЖЗИІЇЙКЛМНОПРСТУФХЦЧШЩЬЮЯ"
LOWER = "абвгґдеєжзиіїйклмнопрстуфхцчшщьюя’"


def alphabet_test(sid, text):
    """Latin letters -> consecutive Ukrainian letters (case kept)."""
    out, k = [], sid * 7
    for ch in text:
        if "A" <= ch <= "Z":
            out.append(UPPER[k % len(UPPER)])
            k += 1
        elif "a" <= ch <= "z":
            out.append(LOWER[k % len(LOWER)])
            k += 1
        else:
            out.append(ch)
    return "".join(out)


def clone(src, dst):
    """APFS clone (no extra space) when possible, else a copy."""
    if os.path.lexists(dst):
        os.remove(dst)
    if subprocess.run(["cp", "-c", src, dst], capture_output=True).returncode:
        shutil.copyfile(src, dst)


def read_overrides(path):
    out = {}
    with open(path, encoding="utf-8") as f:
        for ln, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            sid, sep, text = line.partition("\t")
            if not sep:
                raise SystemExit(f"{path}:{ln}: want id<TAB>text")
            out[int(sid)] = textbank.unescape(text)
    return out


def main(argv):
    if len(argv) < 2 or argv[1] not in SLOT:
        print(__doc__)
        return 2
    lang = argv[1]
    opts = dict(zip(argv[2::2], argv[3::2]))
    root = os.path.abspath(os.path.join(HERE, "..", ".."))
    data = opts.get("--data", os.environ.get("HP2_DATA", root))
    disc = os.path.join(data, "game", "extracted")
    src = opts.get("--src", os.path.join(root, "mods", "loc", lang))   # the DMG: Resources/import/loc/<lang>
    out = opts.get("--out", os.path.join(data, "mods", lang))
    slot = SLOT[lang]
    n_files = 0

    test = opts.get("--test-alphabet") == "1"
    table = {} if test else {i: uk for i, (_, uk) in extract.load_translations(src).items() if uk}
    extra = opts.get("--text")
    for bank, (ver, strings) in extract.disc_banks(data, slot).items():
        over = {i: textbank.unescape(t) for i, t in table.items()}
        tsv = os.path.join(extra, bank + ".tsv") if extra else ""
        if tsv and os.path.exists(tsv):
            over.update(read_overrides(tsv))
        ids = {i for i, _ in strings}
        if not test and not ids & set(over):
            continue
        if tsv and os.path.exists(tsv):
            missing = sorted(set(read_overrides(tsv)) - ids)
            if missing:
                raise SystemExit(f"{tsv}: ids not in {slot}/{bank}.bin: {missing[:10]}")
        n = len(ids & set(over))
        strings = [(i, over.get(i, alphabet_test(i, t) if test else t)) for i, t in strings]
        os.makedirs(os.path.join(out, slot), exist_ok=True)
        open(os.path.join(out, slot, bank + ".bin"), "wb").write(textbank.write_bank(ver, strings))
        print(f"{slot}/{bank}.bin: {n}/{len(ids)} strings translated" + (", rest alphabet test" if test else ""))
        n_files += 1

    cfg = os.path.join(src, "fonts.txt")
    if os.path.exists(cfg):
        chars, reuse, geos, flags, splits, hybrid = "", [], [], {}, {}, None
        for line in open(cfg, encoding="utf-8"):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, _, val = line.partition(" ")
            if key == "chars":
                chars += val.replace(" ", "")
            elif key == "reuse":
                reuse += [(p[0], p[1]) for p in val.split() if len(p) == 2]
            elif key == "flag":
                name, idx, *cols = val.split()
                flags.setdefault(name, []).append(
                    (int(idx), [tuple(int(c[k:k + 2], 16) for k in (1, 3, 5)) for c in cols]))
            elif key == "splitflag":
                name, idx, rest = val.split(" ", 2)
                font, left, right = rest.split(";")
                splits.setdefault(name, []).append((int(idx), font, [left, right]))
            elif key == "hybrid":
                hybrid = val.strip()
            elif key == "geo":
                name, _, fonts = val.partition(" ")
                geos.append((name, fonts.strip().split(";")))
        for name, fonts in geos:
            g = geo_font.Geo(open(os.path.join(disc, name), "rb").read())
            for idx, cols in flags.get(name, []):
                y0, y1 = geo_tex.repaint_flag(g, idx, cols)
                print(f"{name}: texture {idx} repainted as a flag (rows {y0}..{y1})")
            for idx, font, labels in splits.get(name, []):
                y0, ys, y1 = geo_tex.split_flag(g, idx, font, labels)
                print(f"{name}: texture {idx} = split flag, labels {labels} (rows {y0}..{ys}..{y1})")
            n = geo_font.add_glyphs(g, chars, reuse, fonts)
            if opts.get("--sheets"):
                os.makedirs(opts["--sheets"], exist_ok=True)
                geo_font.sheet(g, chars, os.path.join(opts["--sheets"], name.replace(".geo", ".png")))
            os.makedirs(out, exist_ok=True)
            open(os.path.join(out, name), "wb").write(g.d)
            print(f"{name}: {n} glyph records added")
            n_files += 1
        voice = opts.get("--voice")
        if voice:
            vdir = os.path.join(voice, slot)
            banks = sorted(f for f in os.listdir(vdir) if f.lower().endswith(".sdt"))
            if not banks:
                raise SystemExit(f"--voice {voice}: no {slot}/*.sdt")
            os.makedirs(os.path.join(out, slot), exist_ok=True)
            for f in banks:
                clone(os.path.join(vdir, f), os.path.join(out, slot, f))
            print(f"{slot}/: {len(banks)} speech banks from {vdir}")
            n_files += len(banks)
        if hybrid and n_files:
            # Every disc file of the slot; text from this overlay, speech
            # from the disc (never from --voice: the hybrid keeps English).
            sdir, hdir = os.path.join(disc, slot), os.path.join(out, hybrid)
            os.makedirs(hdir, exist_ok=True)
            n_text = n_disc = 0
            for f in sorted(os.listdir(sdir)):
                if f.lower() == "vssver.scc" or not os.path.isfile(os.path.join(sdir, f)):
                    continue
                built = os.path.join(out, slot, f)
                if f.lower().startswith("text") and os.path.exists(built):
                    clone(built, os.path.join(hdir, f))
                    n_text += 1
                else:
                    clone(os.path.join(sdir, f), os.path.join(hdir, f))
                    n_disc += 1
            print(f"{hybrid}/: {n_text} translated text banks + {n_disc} disc files (English speech)")
            n_files += n_text + n_disc
    print(f"{n_files} files -> {out}" if n_files else "no sources: nothing written")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
