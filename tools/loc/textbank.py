#!/usr/bin/env python3
"""Text banks (_LANG/text*.bin, textr.bin) <-> editable UTF-8 TSV.

Bank format (recon/notes/loc_formats.md 1.2):
  u16 version (1), u16 count N, N x {u32 id, u32 offset}, then the strings:
  UTF-16LE, NUL-terminated, packed back to back in table order (offsets
  absolute, ascending). Repacking a TSV that was not edited gives the
  original file byte for byte.

TSV (one row per string, header line first):
  id <TAB> max <TAB> text
  - max: the source string's length in UTF-16 units (a hint for translators;
    the engine has no per-string limit known, keep near it for UI strings).
  - text escapes: \\n newline, \\t tab, \\\\ backslash, and {Uxxxx} for
    controller-button glyphs (U+1700..U+171F) and any other character outside
    printable text, so they survive editors. Format codes %a %b %c ... stay
    as they are.

Commands:
  textbank.py extract BANK.bin OUT.tsv
  textbank.py pack IN.tsv OUT.bin [--version N]
  textbank.py check SOURCE.tsv TRANSLATED.tsv   (same ids; same %x codes and
                                                 button glyphs per string)
  textbank.py roundtrip DIR...                  (every *.bin: extract + pack
                                                 == original; exit 1 if not)
"""
import os
import re
import struct
import sys
import tempfile

HEADER = "id\tmax\ttext"


def read_bank(data):
    version, n = struct.unpack_from("<HH", data, 0)
    table = [struct.unpack_from("<II", data, 4 + 8 * i) for i in range(n)]
    out = []
    for i, (sid, off) in enumerate(table):
        end = table[i + 1][1] if i + 1 < n else len(data)
        raw = data[off:end]
        if len(raw) < 2 or raw[-2:] != b"\0\0" or len(raw) % 2:
            raise ValueError(f"string {sid}: not NUL-terminated UTF-16 ({len(raw)} bytes)")
        out.append((sid, raw[:-2].decode("utf-16-le", "surrogatepass")))
    return version, out


def write_bank(version, strings):
    n = len(strings)
    head = bytearray(struct.pack("<HH", version, n))
    heap = bytearray()
    base = 4 + 8 * n
    last = -1
    for sid, text in strings:
        if sid <= last:
            raise ValueError(f"ids must ascend: {sid} after {last}")
        last = sid
        head += struct.pack("<II", sid, base + len(heap))
        heap += text.encode("utf-16-le", "surrogatepass") + b"\0\0"
    return bytes(head + heap)


def _visible(ch):
    o = ord(ch)
    if 0x1700 <= o <= 0x171F:  # button glyphs
        return False
    if o < 0x20 or 0x7F <= o < 0xA0 or 0xD800 <= o <= 0xDFFF or 0xE000 <= o <= 0xF8FF:
        return False
    return True


def escape(text):
    out = []
    for ch in text:
        if ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "{":
            out.append("{{")
        elif _visible(ch):
            out.append(ch)
        else:
            out.append("{U%04X}" % ord(ch))
    return "".join(out)


_TOKEN = re.compile(r"\\\\|\\n|\\t|\{\{|\{U([0-9A-Fa-f]{4})\}")


def unescape(s):
    def sub(m):
        t = m.group(0)
        if t == "\\\\":
            return "\\"
        if t == "\\n":
            return "\n"
        if t == "\\t":
            return "\t"
        if t == "{{":
            return "{"
        return chr(int(m.group(1), 16))
    return _TOKEN.sub(sub, s)


def write_tsv(path, version, strings):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# textbank version {version}\n{HEADER}\n")
        for sid, text in strings:
            f.write(f"{sid}\t{len(text.encode('utf-16-le')) // 2}\t{escape(text)}\n")


def read_tsv(path):
    version = 1
    strings = []
    with open(path, encoding="utf-8") as f:
        for ln, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if line.startswith("# textbank version"):
                version = int(line.split()[-1])
                continue
            if not line or line.startswith("#") or line == HEADER:
                continue
            parts = line.split("\t", 2)
            if len(parts) != 3:
                raise ValueError(f"{path}:{ln}: want id<TAB>max<TAB>text")
            strings.append((int(parts[0]), unescape(parts[2])))
    return version, strings


_CODES = re.compile(r"%[a-z]|[ᜀ-ᜟ]")


def check(src_path, tr_path):
    _, src = read_tsv(src_path)
    _, tr = read_tsv(tr_path)
    problems = 0
    if [i for i, _ in src] != [i for i, _ in tr]:
        print(f"ids differ: {len(src)} vs {len(tr)} strings")
        return 1
    for (sid, a), (_, b) in zip(src, tr):
        ca, cb = sorted(_CODES.findall(a)), sorted(_CODES.findall(b))
        if ca != cb:
            problems += 1
            print(f"{sid}: codes {ca!r} -> {cb!r}")
    print(f"{len(src)} strings, {problems} with changed format/button codes")
    return 1 if problems else 0


def roundtrip(dirs):
    bad = n = 0
    with tempfile.TemporaryDirectory() as tmp:
        for d in dirs:
            for name in sorted(os.listdir(d)):
                if not name.lower().endswith(".bin"):
                    continue
                path = os.path.join(d, name)
                data = open(path, "rb").read()
                ver, strings = read_bank(data)
                tsv = os.path.join(tmp, "t.tsv")
                write_tsv(tsv, ver, strings)
                ver2, strings2 = read_tsv(tsv)
                n += 1
                if write_bank(ver2, strings2) != data:
                    bad += 1
                    print(f"DIFF {path}")
    print(f"roundtrip: {n - bad}/{n} banks byte-identical")
    return 1 if bad or not n else 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "extract" and len(argv) == 4:
        ver, strings = read_bank(open(argv[2], "rb").read())
        write_tsv(argv[3], ver, strings)
        print(f"{len(strings)} strings -> {argv[3]}")
        return 0
    if cmd == "pack" and len(argv) >= 4:
        ver, strings = read_tsv(argv[2])
        if "--version" in argv:
            ver = int(argv[argv.index("--version") + 1])
        open(argv[3], "wb").write(write_bank(ver, strings))
        print(f"{len(strings)} strings -> {argv[3]}")
        return 0
    if cmd == "check" and len(argv) == 4:
        return check(argv[2], argv[3])
    if cmd == "roundtrip" and len(argv) >= 3:
        return roundtrip(argv[2:])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
