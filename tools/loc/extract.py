#!/usr/bin/env python3
"""Translation table <-> the game's text banks.

Every string id has the same English text in every bank it appears in (checked:
3827 ids, 0 conflicts in _UK), so the table is keyed by id: one row per id, in
the file of the first bank that has it (text.bin -> text.tsv, textr.bin ->
textr.tsv, text00..13.bin -> text00..13.tsv). Columns (UTF-8 TSV, textbank.py
escapes: \\n, {U1701} button glyphs, {{ for a literal brace):

  id     string id (global)
  ctx    banks that contain it (text, textr, 00..13) - the context
  max    English length in characters (UTF-16 units)
  crc    CRC-32 (hex) of the escaped English string on the disc (_UK slot =
         British English). The game's own text is never committed: tools
         that need it read it from the player's disc (english()).
  uk     the translation; empty = not translated yet (English is used);
         {=} = deliberately the same as the English (names in the credits)

  tools/loc/extract.py [--lang uk] [--data HP2_DATA] [--out DIR]
      writes/updates mods/loc/<lang>/*.tsv. Existing translations are kept
      (matched by id); a row whose English changed (crc) is reported.
  tools/loc/extract.py pack [--lang uk] [--data ..] --out DIR [--english]
      writes DIR/_UK/<bank>.bin for every bank (translation, else English).
      --english ignores the uk column: the result must equal the disc
      byte for byte (round-trip test: tools/loc/test_roundtrip.sh).
  tools/loc/extract.py stats [--lang uk]
      translated / total per file.
  tools/loc/extract.py merge FILE... [--lang uk]
      sets the uk column from "id<TAB>translation" lines (escaped like the
      table; # comments and blank lines skipped). Unknown ids are an error.
  tools/loc/extract.py verify [--lang uk] [--data ..]
      the table covers every id of the disc with its current English; each
      translation keeps the %x codes and button glyphs of its English string
      and uses only characters the fonts have (ASCII, the English text's own
      characters, fonts.txt "chars"). Exit 1 on a problem.
"""
import re
import os
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import textbank  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SLOT = {"uk": "_UK"}  # language -> the disc slot it replaces
COLS = ["id", "ctx", "max", "crc", "uk"]
SAME = "{=}"  # uk value: keep the English string
HEADER = "\t".join(COLS)


def bank_order(names):
    """text, textr, text00..text13 (the order rows are assigned to files)."""
    key = {"text": (0, ""), "textr": (1, "")}
    return sorted(names, key=lambda n: key.get(n, (2, n)))


def short(bank):
    return bank[4:] if bank[4:].isdigit() else bank


def disc_banks(data, slot):
    d = os.path.join(data, "game", "extracted", slot)
    out = {}
    for n in os.listdir(d):
        if n.lower().endswith(".bin") and n.lower().startswith("text"):
            out[n[:-4]] = textbank.read_bank(open(os.path.join(d, n), "rb").read())
    return {b: out[b] for b in bank_order(out)}


def read_table(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for ln, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#") or line == HEADER:
                continue
            p = line.split("\t")
            if len(p) == 4:
                p.append("")
            if len(p) != 5:
                raise SystemExit(f"{path}:{ln}: want {len(COLS)} tab-separated columns, got {len(p)}")
            rows.append({"id": int(p[0]), "ctx": p[1], "max": p[2], "crc": p[3], "uk": p[4]})
    return rows


def write_table(path, rows):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER + "\n")
        for r in rows:
            f.write("\t".join(str(r[c]) for c in COLS) + "\n")


def table_dir(lang, opts):
    return opts.get("--out", os.path.join(ROOT, "mods", "loc", lang))


def crc(escaped):
    return "%08x" % (zlib.crc32(escaped.encode("utf-8")) & 0xFFFFFFFF)


def load_translations(tdir):
    """id -> (crc of the English, uk escaped or "") from every *.tsv in tdir.
    A {=} row (same as English) reads as "" here: the disc string is used."""
    out = {}
    if not os.path.isdir(tdir):
        return out
    for n in sorted(os.listdir(tdir)):
        if n.endswith(".tsv"):
            for r in read_table(os.path.join(tdir, n)):
                out[r["id"]] = (r["crc"], "" if r["uk"] == SAME else r["uk"])
    return out


def english(data, lang="uk"):
    """id -> escaped English string, read from the player's disc (HP2_DATA)."""
    out = {}
    for _, (_, strings) in disc_banks(data, SLOT[lang]).items():
        for sid, text in strings:
            out.setdefault(sid, textbank.escape(text))
    return out


def load_with_english(tdir, data=None, lang="uk"):
    """id -> (en escaped from the disc, uk escaped; {=} -> the English) for the dev tools
    (lint, voice) that compare both. Rows the disc lacks are skipped."""
    en = english(data or os.environ.get("HP2_DATA", ROOT), lang)
    out = {}
    for n in sorted(os.listdir(tdir)):
        if n.endswith(".tsv"):
            for r in read_table(os.path.join(tdir, n)):
                if r["id"] in en:
                    e = en[r["id"]]
                    out[r["id"]] = (e, e if r["uk"] == SAME else r["uk"])
    return out


def cmd_extract(lang, data, opts):
    banks = disc_banks(data, SLOT[lang])
    tdir = table_dir(lang, opts)
    old = load_translations(tdir)
    old_raw = {}
    if os.path.isdir(tdir):
        for n in os.listdir(tdir):
            if n.endswith(".tsv"):
                old_raw.update({r["id"]: r["uk"] for r in read_table(os.path.join(tdir, n))})
    where, en = {}, {}
    for b, (_, strings) in banks.items():
        for sid, text in strings:
            where.setdefault(sid, []).append(b)
            if sid in en and en[sid] != text:
                raise SystemExit(f"id {sid}: different English in {where[sid][0]} and {b}")
            en[sid] = text
    files = {}
    changed = 0
    for sid in sorted(en):
        e = textbank.escape(en[sid])
        uk = ""
        if sid in old:
            uk = old_raw.get(sid, "")
            if old[sid][0] != crc(e) and uk:
                changed += 1
                print(f"id {sid}: English changed, check the translation")
        files.setdefault(where[sid][0], []).append({
            "id": sid, "ctx": ",".join(short(b) for b in where[sid]),
            "max": len(en[sid].encode("utf-16-le")) // 2, "crc": crc(e), "uk": uk})
    os.makedirs(tdir, exist_ok=True)
    for b, rows in files.items():
        write_table(os.path.join(tdir, b + ".tsv"), rows)
    lost = sorted(set(old) - set(en))
    for sid in lost:
        print(f"id {sid}: translated but no longer on the disc (dropped)")
    done = sum(1 for r in sum(files.values(), []) if r["uk"])
    print(f"{len(en)} ids in {len(files)} files -> {tdir}; {done} translated, {changed} to re-check")
    return 0


def pack_banks(lang, data, tdir, english=False):
    """{bank: bytes} for every bank of the slot, translations applied."""
    tr = {} if english else {sid: uk for sid, (_, uk) in load_translations(tdir).items() if uk}
    out = {}
    for b, (ver, strings) in disc_banks(data, SLOT[lang]).items():
        out[b] = textbank.write_bank(ver, [(i, textbank.unescape(tr[i]) if i in tr else t)
                                           for i, t in strings])
    return out


def cmd_pack(lang, data, opts):
    if "--out" not in opts:
        raise SystemExit("pack: --out DIR required")
    tdir = opts.get("--table", os.path.join(ROOT, "mods", "loc", lang))
    banks = pack_banks(lang, data, tdir, english="--english" in opts)
    d = os.path.join(opts["--out"], SLOT[lang])
    os.makedirs(d, exist_ok=True)
    for b, blob in banks.items():
        open(os.path.join(d, b + ".bin"), "wb").write(blob)
    print(f"{len(banks)} banks -> {d}")
    return 0


def cmd_stats(lang, opts):
    tdir = table_dir(lang, opts)
    tot = done = 0
    for n in bank_order([f[:-4] for f in os.listdir(tdir) if f.endswith(".tsv")]):
        rows = read_table(os.path.join(tdir, n + ".tsv"))
        k = sum(1 for r in rows if r["uk"] or r["max"] == "0")
        tot, done = tot + len(rows), done + k
        print(f"{n:8} {k:5}/{len(rows):5}")
    print(f"total    {done:5}/{tot:5}")
    return 0


def cmd_merge(lang, files, opts):
    new = {}
    for path in files:
        with open(path, encoding="utf-8") as f:
            for ln, line in enumerate(f, 1):
                line = line.rstrip("\n")
                if not line.strip() or line.startswith("#"):
                    continue
                sid, sep, text = line.partition("\t")
                if not sep or not sid.strip().isdigit() or "\t" in text:
                    raise SystemExit(f"{path}:{ln}: want id<TAB>translation")
                new[int(sid)] = text.rstrip("\r")
    tdir = table_dir(lang, opts)
    seen = set()
    for n in sorted(os.listdir(tdir)):
        if not n.endswith(".tsv"):
            continue
        path = os.path.join(tdir, n)
        rows = read_table(path)
        hit = False
        for r in rows:
            if r["id"] in new:
                r["uk"], hit = new[r["id"]], True
                seen.add(r["id"])
        if hit:
            write_table(path, rows)
    unknown = sorted(set(new) - seen)
    if unknown:
        raise SystemExit(f"ids not in the table: {unknown[:20]}")
    print(f"merged {len(seen)} translations")
    return 0


CODES = re.compile(r"%[a-z]|[\u1700-\u171f]")


def cmd_verify(lang, data, opts):
    tdir = table_dir(lang, opts)
    table = load_translations(tdir)
    problems = 0
    disc = {}
    for b, (_, strings) in disc_banks(data, SLOT[lang]).items():
        disc.update(strings)
    allowed = set(chr(c) for c in range(0x20, 0x7F)) | {"\n"}
    for t in disc.values():
        allowed |= set(t)
    cfg = os.path.join(tdir, "fonts.txt")
    if os.path.exists(cfg):
        for line in open(cfg, encoding="utf-8"):
            if line.startswith("chars "):
                allowed |= set(line[6:].strip().replace(" ", ""))
    for sid, text in sorted(disc.items()):
        if sid not in table:
            problems += 1
            print(f"{sid}: not in the table (run tools/loc/extract.py)")
            continue
        c, uk = table[sid]
        if crc(textbank.escape(text)) != c:
            problems += 1
            print(f"{sid}: English in the table differs from the disc (run tools/loc/extract.py)")
        if not uk:
            continue
        u = textbank.unescape(uk)
        if sorted(CODES.findall(text)) != sorted(CODES.findall(u)):
            problems += 1
            print(f"{sid}: codes {sorted(CODES.findall(text))!r} -> {sorted(CODES.findall(u))!r}")
        bad = sorted(set(u) - allowed)
        if bad:
            problems += 1
            print(f"{sid}: characters the fonts lack: {''.join(bad)!r}")
    for sid in sorted(set(table) - set(disc)):
        problems += 1
        print(f"{sid}: in the table, not on the disc")
    done = sum(1 for _, uk in table.values() if uk)
    print(f"verify: {len(disc)} ids, {done} translated, {problems} problems")
    return 1 if problems else 0


def main(argv):
    args = argv[1:]
    cmd = "extract"
    if args and not args[0].startswith("--"):
        cmd = args.pop(0)
    opts, files = {}, []
    while args:
        k = args.pop(0)
        if not k.startswith("--"):
            files.append(k)
            continue
        opts[k] = args.pop(0) if args and not args[0].startswith("--") else "1"
    lang = opts.get("--lang", "uk")
    if lang not in SLOT:
        raise SystemExit(f"unknown language {lang}")
    data = opts.get("--data", os.environ.get("HP2_DATA", ROOT))
    if cmd == "extract":
        return cmd_extract(lang, data, opts)
    if cmd == "pack":
        return cmd_pack(lang, data, opts)
    if cmd == "stats":
        return cmd_stats(lang, opts)
    if cmd == "merge":
        return cmd_merge(lang, files, opts)
    if cmd == "verify":
        return cmd_verify(lang, data, opts)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
