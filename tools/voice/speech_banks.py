#!/usr/bin/env python3
"""List every clip in the EA BNKl speech banks (_USA/Speech_*.sdt, Spell_*.sdt).

BNKl layout (little-endian header, big-endian PT tag values):
  0  'BNKl'   4 u16 version (5)   6 u16 slot count   8 u32 header size   12 u32 data size
  16 u32 0 (unused), 20 u32 slot[count]: PT offset RELATIVE TO THE SLOT ITSELF
     (pt = 20 + 4*i + slot[i]); 0 = empty slot (none seen).
  PT header: 'PT' u8 7 u8 0, then tags until 0xFF. 0xFD/0xFE take no length; other
  tags are u8 tag, u8 len, len bytes big-endian: 0x84 rate, 0x85 samples,
  0x88 data offset (absolute in the file), 0xA0 codec (0x14 = Xbox ADPCM), 0x8C ?.
  Data: Xbox ADPCM mono, 36 bytes per 64 samples.

Usage: speech_banks.py [--data DIR] [--tsv OUT]   (prints a summary; TSV lists clips)
"""
import argparse, collections, glob, hashlib, os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))  # tools/
import hp2paths  # noqa: E402

DEFAULT = os.path.join(hp2paths.DATA, 'game/extracted/_USA')


def parse_pt(b, q):
    tags = {}
    while True:
        t = b[q]; q += 1
        if t == 0xFF:
            return tags
        if t in (0xFD, 0xFE):
            continue
        n = b[q]; q += 1
        tags[t] = int.from_bytes(b[q:q + n], 'big'); q += n


def clips(path):
    b = open(path, 'rb').read()
    if b[:4] != b'BNKl':
        raise ValueError(f'{path}: not a BNKl bank')
    _ver, cnt = struct.unpack_from('<HH', b, 4)
    for i in range(cnt):
        rel = struct.unpack_from("<I", b, 20 + 4 * i)[0]
        if rel == 0:
            yield dict(index=i, empty=True)
            continue
        pt = 20 + 4 * i + rel
        if b[pt:pt + 2] != b'PT':
            raise ValueError(f'{path}: slot {i} -> {pt:#x} is not PT')
        tags = parse_pt(b, pt + 4)
        rate, samples, off = tags.get(0x84, 22050), tags[0x85], tags[0x88]
        size = (samples + 63) // 64 * 36
        data = b[off:off + size]
        yield dict(index=i, empty=False, rate=rate, samples=samples, offset=off,
                   size=size, codec=tags.get(0xA0), sec=samples / rate,
                   sha1=hashlib.sha1(data).hexdigest()[:16])


def bank_key(name):
    return name[:-4]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=DEFAULT)
    ap.add_argument('--tsv')
    a = ap.parse_args()
    rows = []
    for f in sorted(glob.glob(os.path.join(a.data, 'Speech_*.sdt')) +
                    glob.glob(os.path.join(a.data, 'Spell_*.sdt'))):
        for c in clips(f):
            c['bank'] = bank_key(os.path.basename(f))
            rows.append(c)
    full = [r for r in rows if not r['empty']]
    first = {}
    for r in full:
        first.setdefault(r['sha1'], f"{r['bank']}:{r['index']}")
    if a.tsv:
        with open(a.tsv, 'w') as o:
            o.write('bank\tindex\tsamples\trate\tsec\tsha1\tdup_of\n')
            for r in full:
                k = f"{r['bank']}:{r['index']}"
                d = '' if first[r['sha1']] == k else first[r['sha1']]
                o.write(f"{r['bank']}\t{r['index']}\t{r['samples']}\t{r['rate']}\t"
                        f"{r['sec']:.3f}\t{r['sha1']}\t{d}\n")
    tot = sum(r['sec'] for r in full)
    uniq = {}
    for r in full:
        uniq[r['sha1']] = r['sec']
    print(f'banks {len({r["bank"] for r in rows})}, slots {len(rows)}, '
          f'empty {len(rows) - len(full)}, clips {len(full)}, unique {len(uniq)}')
    print(f'total {tot / 60:.1f} min, unique {sum(uniq.values()) / 60:.1f} min')
    rates = collections.Counter(r['rate'] for r in full)
    codecs = collections.Counter(r['codec'] for r in full)
    print('rates', dict(rates), 'codecs', dict(codecs))


if __name__ == '__main__':
    main()
