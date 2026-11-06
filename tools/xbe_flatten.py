#!/usr/bin/env python3
"""Lays an XBE out as its in-memory image (base..base+image size) for raw analysis
tools (Ghidra BinaryLoader), and prints the facts the loader needs.

  tools/xbe_flatten.py game/extracted/default.xbe recon/ghidra/default.flat
"""
import json
import struct
import sys

ENTRY_KEY = {"retail": 0xA8FC57AB, "debug": 0x94859D4B}
THUNK_KEY = {"retail": 0x5B6D40B6, "debug": 0xEFB1F152}


def main():
    src, dst = sys.argv[1], sys.argv[2]
    d = open(src, "rb").read()
    u = lambda o: struct.unpack_from("<I", d, o)[0]
    if d[:4] != b"XBEH":
        sys.exit("not an XBE")
    base, headers, image = u(0x104), u(0x108), u(0x10C)
    rel = lambda va: va - base
    img = bytearray(image)
    img[:headers] = d[:headers]

    sections = []
    sh = rel(u(0x120))
    for i in range(u(0x11C)):
        flags, va, vsize, raw, rsize, name_va = struct.unpack_from("<IIIIII", d, sh + i * 56)
        name = d[rel(name_va):d.index(b"\0", rel(name_va))].decode()
        img[rel(va):rel(va) + rsize] = d[raw:raw + rsize]
        sections.append({"name": name, "va": va, "vsize": vsize, "flags": flags})

    entry = u(0x128) ^ ENTRY_KEY["retail"]
    kind = "retail"
    if not any(s["va"] <= entry < s["va"] + s["vsize"] for s in sections):
        entry, kind = u(0x128) ^ ENTRY_KEY["debug"], "debug"
    thunk = u(0x158) ^ THUNK_KEY[kind]
    imports = []
    o = rel(thunk)
    while struct.unpack_from("<I", img, o)[0]:
        imports.append(struct.unpack_from("<I", img, o)[0] & 0x7FFFFFFF)
        o += 4

    open(dst, "wb").write(img)
    info = {"base": base, "image_size": image, "kind": kind, "entry": entry,
            "kernel_thunk": thunk, "kernel_ordinals": imports, "sections": sections}
    json.dump(info, open(dst + ".json", "w"), indent=1)
    print(f"{kind} base {base:#x} size {image:#x} entry {entry:#x} thunk {thunk:#x} "
          f"{len(imports)} kernel imports -> {dst}")


if __name__ == "__main__":
    main()
