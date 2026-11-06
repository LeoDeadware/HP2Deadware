#!/usr/bin/env python3
"""Length / width lint for a translation table against the game's fonts.

Width = sum of glyph rect widths (x1 - x0, the engine's advance) in the
Ukrainian overlay's panel.geo font 0 (dialogue/menus) for the translation and
in the disc's panel.geo for the English source; space = the font's p1 field
(GUESS) or the width of 'i'. Reports:
  - UI strings (short, no sentence end) wider than the English by > --ui
    (default 1.15): menu items, labels, button prompts -> overflow risk;
  - lines (subtitles / hints) whose characters exceed the English by > --len
    (default 1.30): reading-time risk against the voice;
  - characters without a glyph in any panel font.

  tools/loc/lint.py [--overlay DIR] [--ui 1.15] [--len 1.30] [--top 40]
(--overlay: a built overlay with panel.geo; default: build one in a temp dir)
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import extract  # noqa: E402
import geo_font  # noqa: E402
import textbank  # noqa: E402


def widths(geo_path):
    g = geo_font.Geo(open(geo_path, "rb").read())
    f = g.fonts[0]
    w = {}
    for c, x0, y0, x1, y1 in f.records:
        w.setdefault(chr(c), x1 - x0)
    w.setdefault(" ", max(4, w.get("i", 6)))
    return w


def text_width(w, s):
    return max((sum(w.get(ch, 0) for ch in line) for line in s.split("\n")), default=0)


def main(argv):
    opts = dict(zip(argv[1::2], argv[2::2]))
    data = os.environ.get("HP2_DATA", os.path.abspath(os.path.join(HERE, "..", "..")))
    ov = opts.get("--overlay")
    tmp = None
    if not ov:
        tmp = tempfile.TemporaryDirectory()
        ov = tmp.name
        subprocess.run([sys.executable, os.path.join(HERE, "build_mod.py"), "uk", "--out", ov],
                       check=True, stdout=subprocess.DEVNULL)
    wu = widths(os.path.join(ov, "panel.geo"))
    we = widths(os.path.join(data, "game", "extracted", "panel.geo"))
    ui_r, len_r, top = float(opts.get("--ui", 1.15)), float(opts.get("--len", 1.30)), int(opts.get("--top", 40))
    tab = extract.load_with_english(os.path.join(extract.ROOT, "mods", "loc", "uk"), data)
    ui, ln, missing = [], [], {}
    for sid, (en, uk) in tab.items():
        if not uk or uk == en:
            continue
        e, u = textbank.unescape(en), textbank.unescape(uk)
        for ch in set(u):
            if ch not in wu and ch not in "\n" and not ("ᜀ" <= ch <= "ᜟ"):
                missing.setdefault(ch, []).append(sid)
        short = len(e) <= 40 and not e.rstrip().endswith((".", "!", "?", "…"))
        if short:
            a, b = text_width(we, e), text_width(wu, u)
            if a and b / a > ui_r:
                ui.append((b / a, sid, a, b, e, u))
        elif len(e) >= 20 and len(u) / len(e) > len_r:
            ln.append((len(u) / len(e), sid, len(e), len(u), e, u))
    print(f"UI strings wider than English x{ui_r} ({len(ui)}):")
    for r, sid, a, b, e, u in sorted(ui, reverse=True)[:top]:
        print(f"  {sid:5} {r:4.2f} {a:4}->{b:4}px  {e!r} -> {u!r}")
    print(f"Lines longer than English x{len_r} ({len(ln)}):")
    for r, sid, a, b, e, u in sorted(ln, reverse=True)[:top]:
        print(f"  {sid:5} {r:4.2f} {a:4}->{b:4} ch  {e[:60]!r} -> {u[:60]!r}")
    print(f"Characters without a panel glyph: {''.join(sorted(missing)) or 'none'}")
    for ch, ids in sorted(missing.items()):
        print(f"  {ch!r} U+{ord(ch):04X}: {ids[:10]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
