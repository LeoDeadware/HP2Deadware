#!/usr/bin/env python3
"""xbe_title_image.py <default.xbe> <out.png>: the game's title image (XBE section $$XTIMAGE,
an XPR0 with one DXT1 texture) decoded to an RGBA PNG. Used at install time
for the app icon (tools/icon/make_icns.sh); the image is the disc's, never committed."""
import struct, sys, zlib

def sections(d):
    base = struct.unpack_from('<I', d, 0x104)[0]
    n, addr = struct.unpack_from('<II', d, 0x11C)
    for i in range(n):
        o = addr - base + i * 56
        _, _, _, raw, size, name = struct.unpack_from('<IIIIII', d, o)
        yield d[name - base:name - base + 16].split(b'\0')[0], d[raw:raw + size]

def rgb565(c):
    return ((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)

def dxt1(data, w, h):
    px = bytearray(w * h * 4)
    o = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            c0, c1, bits = struct.unpack_from('<HHI', data, o); o += 8
            a, b = rgb565(c0), rgb565(c1)
            if c0 > c1:
                pal = [a + (255,), b + (255,), tuple((2 * x + y) // 3 for x, y in zip(a, b)) + (255,),
                       tuple((x + 2 * y) // 3 for x, y in zip(a, b)) + (255,)]
            else:
                pal = [a + (255,), b + (255,), tuple((x + y) // 2 for x, y in zip(a, b)) + (255,), (0, 0, 0, 0)]
            for i in range(16):
                x, y = bx + i % 4, by + i // 4
                if x < w and y < h:
                    px[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(pal[(bits >> (2 * i)) & 3])
    return px


def write_png(path, w, h, rgba):
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    chunk = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

d = open(sys.argv[1], 'rb').read()
img = dict(sections(d)).get(b'$$XTIMAGE')
if not img or img[:4] != b'XPR0': sys.exit('no $$XTIMAGE XPR0 in ' + sys.argv[1])
hdr = struct.unpack_from('<I', img, 8)[0]  # XPR0: magic, total size, data offset
fmt = struct.unpack_from('<I', img, 0x18)[0]
kind, w, h = (fmt >> 8) & 0xFF, 1 << ((fmt >> 20) & 15), 1 << ((fmt >> 24) & 15)
if kind != 0x0C: sys.exit(f'title image format 0x{kind:02X}, not DXT1')
write_png(sys.argv[2], w, h, dxt1(img[hdr:], w, h))
print(f'title image {w}x{h} -> {sys.argv[2]}')
