#!/usr/bin/env python3
"""Extracts an Xbox XDVDFS image (XISO, or a full Redump image) to a folder.

  tools/xiso_extract.py game/iso/hp2.iso game/extracted        extract
  tools/xiso_extract.py game/iso/hp2.iso --list                list only
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# XISO: game partition at 0; Redump full image: game partition at 0x18300000.
PARTITION_OFFSETS = (0, 0x18300000, 0x0FD90000, 0x02080000)
ATTR_DIR = 0x10


def find_partition(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return base
    sys.exit("not an XDVDFS image")


def walk(f, base, sector, size, prefix, out):
    f.seek(base + sector * SECTOR)
    table = f.read(size)

    def node(off):
        if off + 14 > len(table):
            return
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF and right == 0xFFFF:
            return  # padding
        name = table[off + 14:off + 14 + nlen].decode("latin-1")
        path = f"{prefix}/{name}" if prefix else name
        if left:
            node(left * 4)
        if attr & ATTR_DIR:
            out.append((path, None, None))
            if length:
                walk(f, base, start, length, path, out)
        else:
            out.append((path, start, length))
        if right:
            node(right * 4)

    if size:
        node(0)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    image, dest = sys.argv[1], sys.argv[2]
    with open(image, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + len(MAGIC))
        root_sector, root_size = struct.unpack("<II", f.read(8))
        entries = []
        walk(f, base, root_sector, root_size, "", entries)
        total = sum(e[2] for e in entries if e[2])
        if dest == "--list":
            for path, start, length in entries:
                print(f"{'<dir>' if start is None else length:>12}  {path}")
            print(f"{len(entries)} entries, {total / 2**20:.1f} MiB, partition at {base:#x}")
            return
        for path, start, length in entries:
            target = os.path.join(dest, path)
            if start is None:
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            f.seek(base + start * SECTOR)
            with open(target, "wb") as o:
                left = length
                while left:
                    chunk = f.read(min(left, 1 << 20))
                    o.write(chunk)
                    left -= len(chunk)
        print(f"extracted {len(entries)} entries, {total / 2**20:.1f} MiB, partition at {base:#x}")


if __name__ == "__main__":
    main()
