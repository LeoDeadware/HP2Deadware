"""DXT3 (BC2) / DXT5 (BC3) decode/encode and a minimal PNG reader/writer,
pure Python.

Images are (w, h, bytearray RGBA8 rows). Xbox compressed textures are
linear (not swizzled): 4x4 blocks row by row, 16 bytes per block
(8 alpha: a0 a1 + 48-bit 3-bit indices; 8 colour: c0 c1 RGB565 + 32-bit
2-bit indices).
"""
import struct
import zlib


def _565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def _colours(blk):
    c0, c1, cbits = struct.unpack_from("<HHI", blk, 8)
    p0, p1 = _565(c0), _565(c1)
    # BC2/BC3 colour blocks always use the 4-colour mode.
    cl = [p0, p1, tuple((2 * x + y) // 3 for x, y in zip(p0, p1)),
          tuple((x + 2 * y) // 3 for x, y in zip(p0, p1))]
    return [cl[(cbits >> (2 * i)) & 3] for i in range(16)]


def decode_block3(blk):
    """DXT3: 64-bit explicit 4-bit alpha, then the colour block."""
    abits = int.from_bytes(blk[0:8], "little")
    return [c + (((abits >> (4 * i)) & 15) * 17,) for i, c in enumerate(_colours(blk))]


def decode_block(blk):
    a0, a1 = blk[0], blk[1]
    abits = int.from_bytes(blk[2:8], "little")
    if a0 > a1:
        al = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    else:
        al = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
    c0, c1, cbits = struct.unpack_from("<HHI", blk, 8)
    p0, p1 = _565(c0), _565(c1)
    cl = [p0, p1, tuple((2 * x + y) // 3 for x, y in zip(p0, p1)),
          tuple((x + 2 * y) // 3 for x, y in zip(p0, p1))]
    px = []
    for i in range(16):
        r, g, b = cl[(cbits >> (2 * i)) & 3]
        px.append((r, g, b, al[(abits >> (3 * i)) & 7]))
    return px


def decode(data, w, h, off=0, fmt="DXT5"):
    dec = decode_block3 if fmt == "DXT3" else decode_block
    out = bytearray(w * h * 4)
    bw = (w + 3) // 4
    for by in range((h + 3) // 4):
        for bx in range(bw):
            o = off + (by * bw + bx) * 16
            px = dec(data[o:o + 16])
            for i, p in enumerate(px):
                x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                if x < w and y < h:
                    q = (y * w + x) * 4
                    out[q:q + 4] = bytes(p)
    return out


def _to565(r, g, b):
    return ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | ((b * 31 + 127) // 255)


def encode_block(px):
    """px: 16 (r,g,b,a). Alpha: 8-value ramp between min and max (exact for
    blocks of one or two alpha levels). Colour: endpoints = the extremes along
    the brightest/darkest opaque pixels (font glyphs are near-monochrome)."""
    alphas = [p[3] for p in px]
    amax, amin = max(alphas), min(alphas)
    if amax == amin:
        a0, a1, aidx = amax, amin, [0] * 16
    else:
        a0, a1 = amax, amin  # a0 > a1: 8-value mode
        ramp = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
        aidx = [min(range(8), key=lambda k: abs(ramp[k] - a)) for a in alphas]
    abits = 0
    for i, k in enumerate(aidx):
        abits |= k << (3 * i)
    vis = [p for p in px if p[3] > 0] or px
    lum = [(p[0] * 3 + p[1] * 6 + p[2]) for p in vis]
    hi, lo = vis[lum.index(max(lum))], vis[lum.index(min(lum))]
    c0, c1 = _to565(*hi[:3]), _to565(*lo[:3])
    if c0 < c1:
        c0, c1, hi, lo = c1, c0, lo, hi
    if c0 == c1:
        cbits = 0
    else:
        p0, p1 = _565(c0), _565(c1)
        cl = [p0, p1, tuple((2 * x + y) // 3 for x, y in zip(p0, p1)),
              tuple((x + 2 * y) // 3 for x, y in zip(p0, p1))]
        cbits = 0
        for i, p in enumerate(px):
            k = min(range(4), key=lambda j: sum((cl[j][t] - p[t]) ** 2 for t in range(3)))
            cbits |= k << (2 * i)
    return bytes([a0, a1]) + abits.to_bytes(6, "little") + struct.pack("<HHI", c0, c1, cbits)


def encode_block3(px):
    """DXT3: alpha quantised to 4 bits (exact for x*17), colour as in DXT5."""
    abits = 0
    for i, p in enumerate(px):
        abits |= ((p[3] + 8) // 17) << (4 * i)
    return abits.to_bytes(8, "little") + encode_block(px)[8:]


def encode(rgba, w, h, fmt="DXT5"):
    enc = encode_block3 if fmt == "DXT3" else encode_block
    out = bytearray()
    for by in range((h + 3) // 4):
        for bx in range((w + 3) // 4):
            px = []
            for i in range(16):
                x, y = min(bx * 4 + (i & 3), w - 1), min(by * 4 + (i >> 2), h - 1)
                q = (y * w + x) * 4
                px.append(tuple(rgba[q:q + 4]))
            out += enc(px)
    return bytes(out)


def downsample(rgba, w, h):
    w2, h2 = max(1, w // 2), max(1, h // 2)
    out = bytearray(w2 * h2 * 4)
    for y in range(h2):
        for x in range(w2):
            s = [0, 0, 0, 0]
            for dy in (0, 1):
                for dx in (0, 1):
                    q = (min(2 * y + dy, h - 1) * w + min(2 * x + dx, w - 1)) * 4
                    for t in range(4):
                        s[t] += rgba[q + t]
            q = (y * w2 + x) * 4
            out[q:q + 4] = bytes((v + 2) // 4 for v in s)
    return out, w2, h2


# ---- PNG (8-bit RGBA or grey, no interlace) ----------------------------------
def write_png(path, w, h, rgba):
    raw = b"".join(b"\0" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def read_png(path):
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", path
    o, idat, w = 8, b"", 0
    while o < len(d):
        n, = struct.unpack_from(">I", d, o)
        t = d[o + 4:o + 8]
        body = d[o + 8:o + 8 + n]
        if t == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and inter == 0 and ctype in (0, 2, 4, 6), (path, depth, ctype)
        elif t == b"IDAT":
            idat += body
        o += 12 + n
    bpp = {0: 1, 2: 3, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, o = [], bytearray(stride), 0
    for _ in range(h):
        ft, line = raw[o], bytearray(raw[o + 1:o + 1 + stride])
        o += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    out = bytearray(w * h * 4)
    for y, line in enumerate(rows):
        for x in range(w):
            px = line[x * bpp:(x + 1) * bpp]
            if ctype == 0:
                v = (px[0], px[0], px[0], 255)
            elif ctype == 4:
                v = (px[0], px[0], px[0], px[1])
            elif ctype == 2:
                v = (px[0], px[1], px[2], 255)
            else:
                v = tuple(px)
            out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(v)
    return w, h, out
