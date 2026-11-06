#!/usr/bin/env python3
"""Whole-texture edits in Eurocom .geo files (same layout as geo_font.py).

Texture i's level 0 sits at align128(base + off_i), base = (located font
texture's file offset) - its off (geo_font.Geo.locate; the rule from
docs/handoff/polish.md). The mip chain follows level 0 contiguously, so a
texture can be repainted in place: decode, change, regenerate every mip,
re-encode into the same bytes.

  repaint_flag(geo, index, colours)  the waving language flags of the
      front-end picker (titlemap.geo: 1 US, 2 GER, 3 ITA, 4 SPA, 5 UK,
      6 FRA, 7 POR; 128x128 DXT3, the flag in the middle rows, transparent
      around). The flag's own alpha (edges) is kept; its rows are split into
      equal horizontal bands of the given colours.

  split_flag(geo, index, font, labels)  the hybrid slot's flag: Union Jack
      on the left half, Ukrainian flag on the right, and under each half its
      label (gold, dark outline) on a transparent strip at the bottom of the
      flag rows: the mesh shows only those rows, so "under" must be inside.

  geo_tex.py dump GEO INDEX OUT.png        decode one texture (level 0)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dxt  # noqa: E402
import geo_font  # noqa: E402

FMT = {4: "DXT3"}


def _mip_error(d, pos, t):
    """RGBA difference between level 1 and the 2x downsample of level 0."""
    w, h, fmt = t.w32, t.h32, FMT[t.fmt]
    a = dxt.decode(d, w, h, pos, fmt)
    b = dxt.decode(d, w // 2, h // 2, pos + w * h, fmt)  # DXT3: 1 byte/pixel
    err = 0
    for y in range(0, h // 2, 2):
        for x in range(w // 2):
            for c in range(4):
                q = ((2 * y) * w + 2 * x) * 4 + c
                m = (a[q] + a[q + 4] + a[q + w * 4] + a[q + w * 4 + 4]) // 4
                err += abs(m - b[(y * (w // 2) + x) * 4 + c])
    return err


_POS = {}


def tex_pos(geo, index):
    """Level-0 offset: the rule's estimate, corrected to the 128-aligned
    candidate within +-0x400 whose level 1 matches level 0 best (the rule is
    off by 0x200 for some titlemap textures). Cached per file content."""
    key = (id(geo), index)
    if key in _POS:
        return _POS[key]
    ref = next((t for t in geo.textures if t.pos is not None), None)
    if ref is None:
        raise ValueError("no located texture to derive the data base from")
    t = geo.textures[index]
    est = (ref.pos - ref.off + t.off + 0x7F) & ~0x7F
    if t.mips > 1:
        est = min(range(est - 0x400, est + 0x480, 0x80), key=lambda p: _mip_error(geo.d, p, t))
    _POS[key] = est
    return est


def chain(t):
    w, h, out = t.w32, t.h32, []
    for _ in range(max(1, t.mips)):
        out.append((w, h, max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16))
        w, h = max(1, w // 2), max(1, h // 2)
    return out


def decode(geo, index):
    t = geo.textures[index]
    return dxt.decode(geo.d, t.w32, t.h32, tex_pos(geo, index), FMT[t.fmt])


def write(geo, index, rgba):
    """Level 0 = rgba; every mip regenerated; all in place."""
    t = geo.textures[index]
    pos, img, w, h = tex_pos(geo, index), bytearray(rgba), t.w32, t.h32
    for lw, lh, size in chain(t):
        blob = dxt.encode(img, lw, lh, FMT[t.fmt])
        assert len(blob) == size
        geo.d[pos:pos + size] = blob
        pos += size
        img, w, h = dxt.downsample(img, lw, lh)


def repaint_flag(geo, index, colours):
    t = geo.textures[index]
    w, h = t.w32, t.h32
    img = bytearray(decode(geo, index))
    rows = [y for y in range(h) if max(img[(y * w + x) * 4 + 3] for x in range(w)) > 128]
    if not rows:
        raise ValueError(f"texture {index}: no opaque rows (not a flag?)")
    y0, y1 = rows[0], rows[-1] + 1
    for y in range(h):
        band = min(len(colours) - 1, max(0, (y - y0) * len(colours) // max(1, y1 - y0)))
        r, g, b = colours[band]
        for x in range(w):
            q = (y * w + x) * 4
            img[q:q + 3] = bytes((r, g, b))
    write(geo, index, img)
    return y0, y1


UJ_BLUE, UJ_RED, WHITE = (1, 33, 105), (200, 16, 46), (255, 255, 255)
UA_BLUE, UA_YELLOW = (0, 87, 183), (255, 215, 0)
GOLD, OUTLINE = (255, 214, 72), (20, 12, 0)


def _union_jack(u, v, w, h):
    """Colour at (u, v) in a w x h rect (u right, v down). Proportions of
    the 60x30 flag scaled to the rect height: saltire white 6 (broad 3, red 2,
    narrow 1, red on the anticlockwise side of each arm), cross white 10, red 6."""
    k = h / 30.0
    x, y = u - w / 2.0, h / 2.0 - v  # centred, y up
    if abs(x) < 3 * k or abs(y) < 3 * k:
        return UJ_RED
    if abs(x) < 5 * k or abs(y) < 5 * k:
        return WHITE
    ax, ay = (w / 2.0 if x > 0 else -w / 2.0), (h / 2.0 if y > 0 else -h / 2.0)
    n = (ax * ax + ay * ay) ** 0.5
    d = (ax * y - ay * x) / n  # > 0: anticlockwise of the arm
    if 0 < d < 2 * k:
        return UJ_RED
    if -3 * k < d < 3 * k:
        return WHITE
    return UJ_BLUE


def _text_mask(font, text, cap, cell, base):
    """Coverage (w, h, bytes) of `text` set with glyphs.swift (CoreText)."""
    gl = geo_font.render(font, cap, cell, base, sorted({ord(c) for c in text}))
    cols = [gl[ord(c)] for c in text]
    w = sum(g[0] - 1 for g in cols) + 1
    out = bytearray(w * cell)
    x = 0
    for gw, gh, px in cols:
        for yy in range(gh):
            for xx in range(gw):
                q = yy * w + x + xx
                out[q] = max(out[q], px[yy * gw + xx])
        x += gw - 1
    return w, cell, out


def split_flag(geo, index, font, labels, strip=22):
    t = geo.textures[index]
    w, h = t.w32, t.h32
    img = bytearray(decode(geo, index))
    rows = [y for y in range(h) if max(img[(y * w + x) * 4 + 3] for x in range(w)) > 128]
    cols = [x for x in range(w) if max(img[(y * w + x) * 4 + 3] for y in range(h)) > 128]
    if not rows:
        raise ValueError(f"texture {index}: no opaque rows (not a flag?)")
    y0, y1, x0, x1 = rows[0], rows[-1] + 1, cols[0], cols[-1] + 1
    ys = (y1 - strip) & ~3  # label strip starts on a DXT block row
    xm = w // 2
    ss = 4  # supersampling per axis
    for y in range(h):
        for x in range(w):
            q = (y * w + x) * 4
            if y < y0 or y >= ys:
                if y >= ys and y < y1:
                    img[q + 3] = 0  # strip: transparent, labels drawn below
                continue
            acc = [0, 0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    u, v = x + (sx + 0.5) / ss, y + (sy + 0.5) / ss
                    if u < xm:
                        c = _union_jack(u - x0, v - y0, xm - x0, ys - y0)
                    else:
                        c = UA_BLUE if v < (y0 + ys) / 2.0 else UA_YELLOW
                    for i in range(3):
                        acc[i] += c[i]
            img[q:q + 3] = bytes(a // (ss * ss) for a in acc)
    # labels, centred under each half, 1-pixel outline, clear of the flag's
    # soft side edges (the waving cloth curls there); largest cap that fits
    cell = strip - 2
    for (lx0, lx1), text in zip(((x0 + 4, xm - 2), (xm + 2, x1 - 4)), labels):
        for cap in range(strip // 2, 5, -1):
            tw, th, cov = _text_mask(font, text, cap, cell, cell - 4)
            if tw + 2 <= lx1 - lx0:
                break
        ox, oy = (lx0 + lx1 - tw) // 2, ys + (strip - th) // 2 + 1
        for yy in range(-1, th + 1):
            for xx in range(-1, tw + 1):
                x, y = ox + xx, oy + yy
                if not (0 <= x < w and ys <= y < h):
                    continue
                c = cov[yy * tw + xx] if 0 <= yy < th and 0 <= xx < tw else 0
                o = max((cov[(yy + dy) * tw + xx + dx] for dy in (-1, 0, 1) for dx in (-1, 0, 1)
                         if 0 <= yy + dy < th and 0 <= xx + dx < tw), default=0)
                q = (y * w + x) * 4
                a = max(c, o)
                if not a:
                    continue
                f = c / 255.0
                img[q:q + 4] = bytes([int(GOLD[i] * f + OUTLINE[i] * (1 - f)) for i in range(3)] + [a])
    write(geo, index, img)
    return y0, ys, y1


def main(argv):
    if len(argv) == 5 and argv[1] == "dump":
        g = geo_font.Geo(open(argv[2], "rb").read())
        i = int(argv[3])
        t = g.textures[i]
        dxt.write_png(argv[4], t.w32, t.h32, decode(g, i))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
