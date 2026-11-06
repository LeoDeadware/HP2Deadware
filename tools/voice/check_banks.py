#!/usr/bin/env python3
"""Check built voice banks against the originals (no game run).

For every <dir>/_UK/Speech_*.sdt: same file size, same PT positions and data offsets
as the original _UK bank (the layout the game relies on), every clip inside its
original byte slot, and no clip longer than the English clip it replaces (the game
starts the next line on the English timing) + TOL seconds.

  check_banks.py [DIR]   (default .cache/HP2Cache/voice/uk/mods)  -> exit 1 on a failure
"""
import glob, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sdt  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))  # tools/
import hp2paths  # noqa: E402

DATA = os.path.join(hp2paths.DATA, 'game/extracted/_UK')
TOL = 0.05


def layout(b):
    _, cnt = struct.unpack_from('<HH', b, 4)
    out = []
    for i in range(cnt):
        pt = 20 + 4 * i + struct.unpack_from('<I', b, 20 + 4 * i)[0]
        t = dict(sdt._tags(b, pt + 4))
        out.append((pt, int.from_bytes(t[0x88], 'big')))
    return out


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(hp2paths.CACHE, 'voice/uk/mods')
    fails, banks, clips, longer = [], 0, 0, []
    for f in sorted(glob.glob(os.path.join(d, '_UK', 'Speech_*.sdt'))):
        name = os.path.basename(f)
        o = os.path.join(DATA, name)
        A, B = open(o, 'rb').read(), open(f, 'rb').read()
        banks += 1
        if len(A) != len(B):
            fails.append('%s: size %d != %d' % (name, len(B), len(A))); continue
        if layout(A) != layout(B):
            fails.append('%s: layout differs' % name); continue
        for i, (ca, cb) in enumerate(zip(sdt.read_bank(o)['clips'], sdt.read_bank(f)['clips'])):
            clips += 1
            if (cb['samples'] + 63) // 64 > (ca['samples'] + 63) // 64 * cb['rate'] // ca['rate'] + 1 \
                    and (cb['samples'] + 63) // 64 * 36 > (ca['samples'] + 63) // 64 * 36:
                fails.append('%s slot %d: outside its byte slot' % (name, i))
            if cb['samples'] / cb['rate'] > ca['samples'] / ca['rate'] + TOL:
                longer.append('%s slot %d: %.2fs > %.2fs' % (name, i, cb['samples'] / cb['rate'],
                                                           ca['samples'] / ca['rate']))
    print('%d banks, %d clips; layout failures %d; clips longer than English %d'
          % (banks, clips, len(fails), len(longer)))
    for x in (fails + longer)[:20]:
        print('  ' + x)
    return 1 if fails or longer or not banks else 0


if __name__ == '__main__':
    sys.exit(main())
