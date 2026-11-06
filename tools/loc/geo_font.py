#!/usr/bin/env python3
"""Fonts inside Eurocom .geo files (panel*.geo, inv_font*.geo, titlemap.geo):
list, dump, and add glyphs (e.g. Cyrillic) without relocating anything.

Layout (worked out 2026-10-31 on panel.geo / inv_font.geo, see
docs/handoff/polish.md):
- header u32 @0x14 = texture count, u32 @0x18 = texture table offset; each
  entry 0x80 bytes: +0x20 u16 w, u16 h (the size the game normalises glyph
  UVs with); +0x48 u32 w, h, fmt (4 = DXT3), +0x58 data offset, +0x60
  level-0 size, +0x64 mip count. The data sits in the file at
  align128(base + offset) for a per-file base; found here by matching the
  glyph rectangles against the decoded alpha.
- a font: "<NAME>.A-1\\0" then one 16-byte header per font
  {u32 flags 0xC000000x, u32 texture index, u8 line height?, u8 space?,
  u16 count, u32 records offset (self-relative, from this field)} and the
  12-byte records {u32 code, u16 x0, y0, x1, y1} of every font, in order,
  at the end of the file. u32 @0x40 / @0xE0 are file size - 0x40 / - 0xE0.

Adding glyphs (all in place, nothing else moves):
- records: inserted after each font's run (offsets of later runs fixed),
  file grows at the end (the two end-relative fields follow);
- pixels: packed into the rows below everything the texture already uses
  and, past the declared height, into the bytes of its mip chain (the
  texture becomes one level, its declared height the next power of two;
  rows past the old mip chain are never referenced). Glyphs are rendered by
  glyphs.swift (CoreText) at the font's measured cap height and baseline,
  coloured per row like the existing glyphs.
- reuse: a code can be mapped onto an existing glyph (e.g. Cyrillic A ->
  Latin A) instead of drawn.

Commands:
  geo_font.py info GEO
  geo_font.py dump GEO OUTDIR                       atlas PNGs + glyph lists
  geo_font.py add GEO OUT --chars CHARS [--reuse PAIRS] [--font NAME]
       CHARS: characters to draw; PAIRS: "AА BВ ..." (existing+new, no space
       inside a pair) codes that reuse an existing glyph.
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dxt  # noqa: E402

FMT = {4: "DXT3"}


def u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


class Texture:
    def __init__(self, d, i, e):
        self.index, self.entry = i, e
        self.w, self.h = u16(d, e + 0x20), u16(d, e + 0x22)
        self.w32, self.h32, self.fmt = struct.unpack_from("<III", d, e + 0x48)
        self.off, _, self.size, self.mips = struct.unpack_from("<IIII", d, e + 0x58)
        self.pos = None  # file offset of level 0 (found by locate())

    def chain_bytes(self):
        n, w, h = 0, self.w32, self.h32
        for _ in range(max(1, self.mips)):
            n += max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16
            w, h = max(1, w // 2), max(1, h // 2)
        return n


class Font:
    def __init__(self, d, hp, name):
        self.hp, self.name = hp, name
        self.flags, self.tex, self.p0, self.p1, self.count, q = struct.unpack_from("<IIBBHI", d, hp)
        self.rec = hp + 12 + q
        self.records = [struct.unpack_from("<IHHHH", d, self.rec + 12 * i) for i in range(self.count)]


class Geo:
    def __init__(self, data):
        self.d = bytearray(data)
        d = self.d
        n, table = u32(d, 0x14), u32(d, 0x18)
        self.textures = [Texture(d, i, table + 0x80 * i) for i in range(n)]
        self.fonts = []
        # Font headers: flags 0xC000000x, a texture index, a count and a
        # self-relative offset to records that look like glyphs. (panel and
        # inv_font put them after their texture name, titlemap elsewhere.)
        for hp in range(0, len(d) - 16, 4):
            if (u32(d, hp) & 0xFFFFFFF0) != 0xC0000000:
                continue
            _, tex, _, _, count, q = struct.unpack_from("<IIBBHI", d, hp)
            rec = hp + 12 + q
            if not (0 < count < 4096 and tex < len(self.textures) and q % 4 == 0
                    and rec + 12 * count <= len(d)):
                continue
            ok = True
            for k in range(min(count, 4)):
                c, x0, y0, x1, y1 = struct.unpack_from("<IHHHH", d, rec + 12 * k)
                if not (0x20 < c < 0x10000 and x0 < x1 <= 4096 and y0 < y1 <= 4096):
                    ok = False
                    break
            if ok:
                m = re.search(rb"([A-Za-z0-9_]+)\.A(-1|\d\d)\0", bytes(d[max(0, hp - 64):hp]))
                self.fonts.append(Font(d, hp, m.group(1).decode() if m else f"font@{hp:X}"))
        for t in self.textures:
            fs = [f for f in self.fonts if f.tex == t.index]
            # locate() needs the mip chain: a texture already rewritten by
            # add (one level) is not located again; build from the disc file.
            if fs and t.fmt in FMT and t.mips > 1:
                t.pos = self.locate(t, fs)

    def _alpha(self, pos, w, rows):
        """DXT3 alpha of the first `rows` rows of a w-wide level at pos."""
        bw = w // 4
        out = bytearray(w * rows)
        for by in range(rows // 4):
            for bx in range(bw):
                o = pos + (by * bw + bx) * 16
                v = int.from_bytes(self.d[o:o + 8], "little")
                for k in range(16):
                    out[(by * 4 + (k >> 2)) * w + bx * 4 + (k & 3)] = ((v >> (4 * k)) & 15) * 17
        return out

    def locate(self, t, fonts):
        """Level-0 file offset of t: the 128-aligned candidate at or after
        t.off whose next level (level 1) is the 2x downsample of it. Needs a
        mip chain (font textures have one)."""
        w, size0 = t.w32, t.w32 * t.h32  # DXT3: 1 byte per pixel
        rows = min(64, t.h32)
        best = (None, None)
        # The data block starts at the first 128-aligned offset after the
        # texture table (and a name that may follow it); texture i is at
        # align128(block - off[0] + off[i]).
        n, table = u32(self.d, 0x14), u32(self.d, 0x18)
        start = (table + 0x80 * n + 0x7F) & ~0x7F
        off0 = self.textures[0].off
        cands = sorted({(p0 - off0 + t.off + 0x7F) & ~0x7F for p0 in range(start, start + 0x400, 0x80)})
        for pos in cands:
            if pos < 0 or pos + size0 + (w // 2) * (rows // 2) > len(self.d):
                continue
            a0 = self._alpha(pos, w, rows)
            a1 = self._alpha(pos + size0, w // 2, rows // 2)
            if not any(a0):
                continue
            err = 0
            for y in range(rows // 2):
                for x in range(w // 2):
                    m = (a0[2 * y * w + 2 * x] + a0[2 * y * w + 2 * x + 1] + a0[(2 * y + 1) * w + 2 * x]
                         + a0[(2 * y + 1) * w + 2 * x + 1]) // 4
                    err += abs(m - a1[y * (w // 2) + x])
            if best[0] is None or err < best[0]:
                best = (err, pos)
        return best[1]

    def atlas(self, t, h=None):
        return dxt.decode(self.d, t.w32, h or t.h32, t.pos, FMT[t.fmt])


# ---- glyph rendering ------------------------------------------------------------
def measure(img, w, rect):
    """Cap top / baseline rows (relative to the cell) of a capital's rect."""
    _, x0, y0, x1, y1 = rect
    rows = [y for y in range(y0, y1) if any(img[(y * w + x) * 4 + 3] > 96 for x in range(x0, x1))]
    return rows[0] - y0, rows[-1] - y0 + 1


def row_colours(img, w, recs):
    """Per cell row: mean colour of strongly covered pixels, and whether the
    texture stores colour premultiplied by alpha."""
    cell = max(r[4] - r[2] for r in recs)
    acc = [[0, 0, 0, 0] for _ in range(cell)]
    over = total = 0
    for _, x0, y0, x1, y1 in recs:
        for y in range(y0, y1):
            for x in range(x0, x1):
                q = (y * w + x) * 4
                r, g, b, a = img[q:q + 4]
                if a:
                    total += 1
                    over += max(r, g, b) > a + 8
                if a > 200:
                    c = acc[y - y0]
                    c[0] += r; c[1] += g; c[2] += b; c[3] += 1
    out, last = [], (255, 255, 255)
    for c in acc:
        if c[3]:
            last = (c[0] // c[3], c[1] // c[3], c[2] // c[3])
        out.append(last)
    return out, over < total * 0.02


def render(font_name, cap, cell, baseline, codes):
    here = os.path.dirname(os.path.abspath(__file__))
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "g.bin")
        # glyphs: a compiled glyphs.swift next to this file (the DMG ships one: a player's Mac
        # has CLT but maybe no working `swift` interpreter), else the swift interpreter
        exe = os.path.join(here, "glyphs")
        cmd = [exe] if os.access(exe, os.X_OK) else ["swift", os.path.join(here, "glyphs.swift")]
        subprocess.run(cmd + [font_name, str(cap), str(cell), str(baseline), out] + [str(c) for c in codes],
                       check=True)
        d = open(out, "rb").read()
    n, o, res = u32(d, 0), 4, {}
    for _ in range(n):
        code, w, h = struct.unpack_from("<III", d, o)
        o += 12
        res[code] = (w, h, d[o:o + w * h])
        o += w * h
    return res


# ---- adding glyphs --------------------------------------------------------------
def aliases(f, have, recs, chars, caps_only):
    """Capitals-only fonts: lowercase codes reuse their capital's rect."""
    if not caps_only:
        return []
    known = {r[0]: r for r in list(have.values()) + recs}
    out = []
    for c in chars:
        if c.islower() and ord(c) not in known and ord(c.upper()) in known:
            out.append((ord(c),) + known[ord(c.upper())][1:])
    return out


def add_glyphs(geo, chars, reuse, font_names):
    d = geo.d
    new_records = {}  # font -> [records]
    skipped = []  # (font, code) that found no room
    fallback = []  # (font, lowercase code) drawn with its capital instead
    for t in geo.textures:
        fonts = [f for f in geo.fonts if f.tex == t.index]
        if not fonts or t.pos is None:
            continue
        w = t.w32
        img = geo.atlas(t)
        used_y = 0
        for y in range(t.h32 - 1, -1, -1):
            if any(img[(y * w + x) * 4 + 3] for x in range(w)):
                used_y = y + 1
                break
        used_y = max([used_y] + [r[4] for f in fonts for r in f.records])
        extra_rows = (t.chain_bytes() - t.w32 * t.h32) // ((w // 4) * 16) * 4 if t.mips > 1 else 0
        cap_rows = t.h32 + extra_rows
        canvas = bytearray(w * cap_rows * 4)
        canvas[:len(img)] = img
        # Occupancy per row as a bit mask: existing glyph rects and any ink
        # (other sprites share these textures), plus a 1-pixel margin.
        occ = [0] * cap_rows
        for y in range(t.h32):
            m = 0
            for x in range(w):
                if img[(y * w + x) * 4 + 3]:
                    m |= 1 << x
            occ[y] = m | (m << 1) | (m >> 1)
        for f in fonts:
            for _, x0, y0, x1, y1 in f.records:
                for y in range(y0, min(y1, cap_rows)):
                    occ[y] |= ((1 << (x1 - x0)) - 1) << x0
        full = (1 << w) - 1
        end_y = used_y
        touched = set()  # (bx, by) DXT blocks that get new pixels

        def place(gw, gh):
            want = (1 << gw) - 1
            for y in range(0, cap_rows - gh + 1):
                free = full
                for yy in range(y, y + gh):
                    free &= ~occ[yy]
                    if not free:
                        break
                x = 0
                while free >> x and x + gw <= w:
                    if (free >> x) & want == want:
                        return x, y
                    x += 1
            return None

        for f in fonts:
            have = {r[0]: r for r in f.records}
            recs = []
            for a, b in reuse:
                if ord(b) not in have and ord(a) in have:
                    recs.append((ord(b),) + have[ord(a)][1:])
            # A capitals-only font (no Latin lowercase): draw capitals, then
            # point the lowercase codes at them.
            caps_only = ord("a") not in have and ord("A") in have
            # (a capitals-only font gets letters only: its texts are titles)
            want = [c for c in chars if not (caps_only and (c.islower() or not c.isalpha()))]
            todo = [ord(c) for c in want if ord(c) not in have and all(ord(c) != ord(b) for _, b in reuse)]
            if not todo:
                new_records[f] = recs + aliases(f, have, recs, chars, caps_only)
                continue
            # Capitals take the metrics of H. Lowercase too, unless the font's
            # lowercase are small capitals (its h is a shorter cell than H):
            # then a lowercase letter is drawn as a capital at h's size.
            capref = have.get(ord("H")) or have.get(ord("E")) or f.records[0]
            lowref = have.get(ord("h")) or have.get(ord("n"))
            def ink(r):
                top, base = measure(img, w, r)
                return base - top
            # h without an ascender (shorter ink than H): small capitals.
            small_caps = lowref is not None and ink(lowref) < 0.85 * ink(capref)
            font_name = font_names[geo.fonts.index(f) % len(font_names)]
            glyphs, colour_of = {}, {}
            groups = {}
            for code in todo:
                lower = small_caps and chr(code).islower()
                groups.setdefault(lower, []).append(code)
            for lower, codes in groups.items():
                ref = lowref if lower else capref
                top, base = measure(img, w, ref)
                cell = ref[4] - ref[2]
                cols = row_colours(img, w, [r for r in f.records if r[4] - r[2] == cell])
                draw = [ord(chr(c).upper()) if lower else c for c in codes]
                got = render(font_name, base - top, cell, base, draw)
                for c, dc in zip(codes, draw):
                    glyphs[c] = got[dc]
                    colour_of[c] = cols
            def ink_rows(c):
                gw, gh, cov = glyphs[c]
                while gh > 1 and not any(cov[(gh - 1) * gw:gh * gw]):
                    gh -= 1
                return gh
            # Letters before punctuation, big before small (first fit).
            order = sorted(todo, key=lambda c: (not chr(c).isalpha(), -glyphs[c][0] * ink_rows(c)))
            for code in order:
                colours, premul = colour_of[code]
                gw, gh, cov = glyphs[code]
                # The engine draws a glyph at its rect's size from the rect's
                # top: rows below the lowest ink can be left out (checked
                # in game), which packs far more glyphs into the free rows.
                while gh > 1 and not any(cov[(gh - 1) * gw:gh * gw]):
                    gh -= 1
                at = place(gw, gh)
                if at is None:
                    up = chr(code).upper()
                    if chr(code).islower() and up != chr(code):
                        fallback.append((f, code))  # use the capital (small caps: one size up)
                    else:
                        skipped.append((f.name, code))
                    continue
                x, y = at
                for yy in range(y, y + gh):
                    occ[yy] |= ((1 << (gw + 1)) - 1) << max(0, x - 1) & full
                for gy in range(gh):
                    cr = colours[min(gy, len(colours) - 1)]
                    for gx in range(gw):
                        a = cov[gy * gw + gx]
                        q = ((y + gy) * w + x + gx) * 4
                        rgb = [min(255, c * a // 255) for c in cr] if premul else list(cr)
                        canvas[q:q + 4] = bytes(rgb + [a])
                recs.append((code, x, y, x + gw, y + gh))
                end_y = max(end_y, y + gh)
                for by in range(y // 4, (y + gh + 3) // 4):
                    for bx in range(x // 4, (x + gw + 3) // 4):
                        touched.add((bx, by))
            known = {r[0]: r for r in list(have.values()) + recs}
            for g2, code in [x for x in fallback if x[0] is f]:
                cap = known.get(ord(chr(code).upper()))
                if cap:
                    recs.append((code,) + cap[1:])
                    print(f"note: {f.name}: {chr(code)} drawn as {chr(code).upper()} (no room)", file=sys.stderr)
                else:
                    skipped.append((f.name, code))
            new_records[f] = recs + aliases(f, have, recs, chars, caps_only)
        if end_y > t.h32:
            # Taller texture, one level: rows past the old height come from the
            # old mip chain's bytes (place() never goes past them).
            new_h = 1 << (end_y - 1).bit_length()
            struct.pack_into("<H", d, t.entry + 0x22, new_h)
            struct.pack_into("<I", d, t.entry + 0x4C, new_h)
            struct.pack_into("<I", d, t.entry + 0x60, w * new_h)  # DXT3: 1 byte per pixel
            struct.pack_into("<I", d, t.entry + 0x64, 1)
        # Re-encode only the blocks that got new pixels: everything else
        # stays byte-exact.
        enc = dxt.encode_block3 if FMT[t.fmt] == "DXT3" else dxt.encode_block
        for bx, by in sorted(touched):
            px = []
            for k in range(16):
                q = ((by * 4 + (k >> 2)) * w + bx * 4 + (k & 3)) * 4
                px.append(tuple(canvas[q:q + 4]))
            o = t.pos + (by * (w // 4) + bx) * 16
            d[o:o + 16] = enc(px)
    # Records: insert after each font's run, from the last run backwards.
    size0 = len(d)
    for f in sorted(new_records, key=lambda f: f.rec, reverse=True):
        recs = new_records[f]
        if not recs:
            continue
        at = f.rec + 12 * f.count
        blob = b"".join(struct.pack("<IHHHH", *r) for r in recs)
        d[at:at] = blob
        struct.pack_into("<H", d, f.hp + 10, f.count + len(recs))
        for g in geo.fonts:
            if g.rec >= at and g is not f:
                q = u32(d, g.hp + 12)
                struct.pack_into("<I", d, g.hp + 12, q + len(blob))
    if len(d) != size0:
        for fld, k in ((0x40, 0x40), (0xE0, 0xE0)):
            if u32(d, fld) != size0 - k:
                raise SystemExit(f"field 0x{fld:X} is not file size - 0x{k:X}: layout not understood")
        new = (len(d) + 0xFFF) & ~0xFFF
        d += bytes(new - len(d))
        for fld, k in ((0x40, 0x40), (0xE0, 0xE0)):
            struct.pack_into("<I", d, fld, new - k)
    for name, code in skipped:
        print(f"warning: {name}: no room for {chr(code)} U+{code:04X} (texture full incl. its mip "
              f"chain; growing it needs relocation, not supported)", file=sys.stderr)
    return sum(len(r) for r in new_records.values())


def sheet(geo, chars, path):
    """PNG of `chars` as every font of geo draws them now (read back from the
    atlas through the glyph records): one line per font, grey background,
    a red box where a character has no record. For checking coverage."""
    lines = []
    for f in geo.fonts:
        t = geo.textures[f.tex]
        if t.pos is None:
            continue
        w = t.w32
        recs = {}
        cnt = u16(geo.d, f.hp + 10)
        rec = f.hp + 12 + u32(geo.d, f.hp + 12)
        for i in range(cnt):
            c, x0, y0, x1, y1 = struct.unpack_from("<IHHHH", geo.d, rec + 12 * i)
            recs[c] = (x0, y0, x1, y1)
        h = (max(r[3] for r in recs.values()) + 3) & ~3  # rows the glyphs use
        img = dxt.decode(geo.d, w, h, t.pos, FMT[t.fmt])
        cell = max(r[3] - r[1] for r in recs.values())
        lines.append((img, w, recs, cell))
    width = 1600
    rows = []  # (y, height) per wrapped row
    out_h = sum(((len(chars) * 40) // width + 2) * (c + 4) for _, _, _, c in lines) + 8
    out = bytearray(width * out_h * 4)
    for i in range(0, len(out), 4):
        out[i:i + 4] = b"\x50\x50\x50\xff"
    y = 4
    for img, w, recs, cell in lines:
        x = 4
        for ch in chars:
            r = recs.get(ord(ch))
            gw = (r[2] - r[0]) if r else 12
            if x + gw + 4 > width:
                x, y = 4, y + cell + 4
            if r is None:
                for yy in range(y, y + cell):
                    for xx in (x, x + gw - 1):
                        out[(yy * width + xx) * 4:(yy * width + xx) * 4 + 4] = b"\xff\x00\x00\xff"
            else:
                x0, y0, x1, y1 = r
                for yy in range(y1 - y0):
                    for xx in range(x1 - x0):
                        q = ((y0 + yy) * w + x0 + xx) * 4
                        a = img[q + 3]
                        o = ((y + yy) * width + x + xx) * 4
                        for k in range(3):
                            out[o + k] = (img[q + k] * a + out[o + k] * (255 - a)) // 255
            x += gw + 2
        y += cell + 8
    dxt.write_png(path, width, y + 4, out[:width * (y + 4) * 4])


def cmd_info(path):
    g = Geo(open(path, "rb").read())
    for t in g.textures:
        fs = [f for f in g.fonts if f.tex == t.index]
        if fs:
            print(f"texture {t.index}: {t.w}x{t.h} fmt {t.fmt} mips {t.mips} data 0x{t.pos or 0:X}")
    for f in g.fonts:
        hs = sorted({r[4] - r[2] for r in f.records})
        print(f"font {f.name} flags {f.flags:08X} texture {f.tex} p {f.p0},{f.p1}: {f.count} glyphs, "
              f"cell heights {hs}")


def cmd_dump(path, out):
    os.makedirs(out, exist_ok=True)
    g = Geo(open(path, "rb").read())
    base = os.path.splitext(os.path.basename(path))[0]
    for t in g.textures:
        if t.pos is None:
            continue
        dxt.write_png(os.path.join(out, f"{base}_tex{t.index}.png"), t.w32, t.h32, g.atlas(t))
    for k, f in enumerate(g.fonts):
        with open(os.path.join(out, f"{base}_font{k}.tsv"), "w", encoding="utf-8") as o:
            o.write("code\tchar\tx0\ty0\tx1\ty1\n")
            for c, x0, y0, x1, y1 in f.records:
                o.write(f"{c:04X}\t{chr(c) if c >= 0x20 else ''}\t{x0}\t{y0}\t{x1}\t{y1}\n")
    print(f"dumped {len(g.fonts)} fonts to {out}")


def main(argv):
    if len(argv) >= 3 and argv[1] == "info":
        return cmd_info(argv[2])
    if len(argv) >= 4 and argv[1] == "dump":
        return cmd_dump(argv[2], argv[3])
    if len(argv) >= 4 and argv[1] == "add":
        opts = dict(zip(argv[4::2], argv[5::2]))
        g = Geo(open(argv[2], "rb").read())
        reuse = [(p[0], p[1]) for p in opts.get("--reuse", "").split() if len(p) == 2]
        n = add_glyphs(g, opts.get("--chars", ""), reuse, opts.get("--font", "Times New Roman").split(";"))
        open(argv[3], "wb").write(g.d)
        print(f"{n} glyph records added -> {argv[3]}")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv) or 0)
