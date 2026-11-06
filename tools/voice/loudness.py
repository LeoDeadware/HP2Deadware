#!/usr/bin/env python3
"""Speech loudness in LUFS (BS.1770 K-weighting, speech parts only) and the per-speaker
loudness report of built voice banks against the original English banks.

K-weighting is ffmpeg's highpass 38 Hz + high shelf +4 dB at 1681 Hz (the BS.1770
pre-filter), then BS.1770 gating: 400 ms blocks every 100 ms, absolute gate -70 LUFS,
relative gate -10 LU (a clip shorter than one block is one block). Checked against
ffmpeg ebur128 on joined clips. (A first version averaged the speech segments only: it
read TTS lines ~2 dB louder than ebur128 does, so the beta voice sat 2-3 dB under
English; the narrator 3.3 dB under, 2026-11-01.)

  loudness.py report DIR [--out FILE]   per speaker (and per bank for the scenes in
      levels.tsv): median LUFS English vs Ukrainian, delta, intended offset; exit 1 if a
      speaker with >= 5 lines is off its intended level by more than 1.5 dB.
  loudness.py file.wav|mp3              LUFS of one file
"""
import argparse, csv, glob, json, math, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sdt  # noqa: E402
import timing  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))  # tools/
import hp2paths  # noqa: E402

WORK = os.path.join(hp2paths.CACHE, 'voice/uk')
DATA = os.path.join(hp2paths.DATA, 'game/extracted')
KW = 'highpass=f=38:poles=2,highshelf=f=1681:g=4:t=q:w=0.707'


def to22k(pcm, rate):
    if rate == sdt.RATE:
        return pcm
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-f', 's16le', '-ar', str(rate), '-ac', '1', '-i', '-',
                          '-af', 'aresample=%d' % sdt.RATE, '-f', 's16le', '-'],
                         input=struct.pack('<%dh' % len(pcm), *pcm), check=True, capture_output=True).stdout
    return list(struct.unpack('<%dh' % (len(raw) // 2), raw))


def lufs(pcm, rate=sdt.RATE):
    """K-weighted loudness of the speech parts of pcm (s16 list) in LUFS; None if silent."""
    pcm = to22k(pcm, rate)
    if not pcm:
        return None
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-f', 's16le', '-ar', str(sdt.RATE), '-ac', '1', '-i', '-',
                          '-af', KW, '-f', 'f32le', '-'],
                         input=struct.pack('<%dh' % len(pcm), *pcm), check=True, capture_output=True).stdout
    y = struct.unpack('<%df' % (len(raw) // 4), raw)
    blk, hop = int(0.4 * sdt.RATE), int(0.1 * sdt.RATE)
    sq = [v * v for v in y]
    acc = [0.0]
    for v in sq:
        acc.append(acc[-1] + v)
    starts = range(0, max(1, len(sq) - blk + 1), hop)
    ms = [(acc[min(len(sq), a + blk)] - acc[a]) / min(blk, len(sq) - a) for a in starts]
    ms = [m for m in ms if m > 0 and -0.691 + 10 * math.log10(m) > -70]
    if not ms:
        return None
    rel = -0.691 + 10 * math.log10(sum(ms) / len(ms)) - 10
    ms = [m for m in ms if -0.691 + 10 * math.log10(m) > rel]
    return -0.691 + 10 * math.log10(sum(ms) / len(ms))


def offsets():
    """levels.tsv: speaker, bank ('*' = everywhere, a bank, or one clip's wav name),
    gain_db -> {(spk, bank): dB}."""
    out = {}
    p = os.path.join(HERE, 'levels.tsv')
    for r in csv.DictReader((l for l in open(p, encoding='utf-8') if not l.startswith('#')), delimiter='\t'):
        out[(r['speaker'], r['bank'])] = float(r['gain_db'])
    return out


def offset_of(off, spk, bank, wav=''):
    return off.get((spk, '*'), 0.0) + off.get((spk, bank), 0.0) + off.get((spk, wav), 0.0)


def english_table(data=DATA):
    """wav name -> English LUFS of every _UK clip (cached in en_lufs.json)."""
    cache = os.path.join(WORK, 'en_lufs.json')
    if os.path.exists(cache):
        return json.load(open(cache))
    import speech_map
    banks = speech_map.read_fxmap(os.path.join(data, 'EASound/FXMap.dbg'))[0]
    out = {}
    for f in sorted(glob.glob(os.path.join(data, '_UK', 'Speech_*.sdt'))):
        t = banks.get(os.path.basename(f)[:-4].upper(), {})
        for i, c in enumerate(sdt.read_bank(f)['clips']):
            w = t.get(i, (0, ''))[1]
            if w and w not in out:
                out[w] = lufs(sdt.adpcm_decode(c['data'], c['samples']), c['rate'])
    json.dump(out, open(cache, 'w'))
    return out


def med(v):
    v = sorted(x for x in v if x is not None)
    return v[len(v) // 2] if v else None


def report(vdir, out_path, data=DATA):
    import speech_map
    banks = speech_map.read_fxmap(os.path.join(data, 'EASound/FXMap.dbg'))[0]
    en = english_table(data)
    off = offsets()
    scenes = {b for _s, b in off if b != '*'}
    per = {}   # (spk, bank or '*') -> ([en], [uk])
    for f in sorted(glob.glob(os.path.join(vdir, '_UK', 'Speech_*.sdt'))):
        name = os.path.basename(f)[:-4]
        t = banks.get(name.upper(), {})
        orig = sdt.read_bank(os.path.join(data, '_UK', name + '.sdt'))['clips']
        for i, c in enumerate(sdt.read_bank(f)['clips']):
            if c['data'] == orig[i]['data']:
                continue  # kept English
            w = t.get(i, (0, ''))[1]
            if not w.startswith('SV_'):
                continue
            spk = w.split('_')[1]
            uk = lufs(sdt.adpcm_decode(c['data'], c['samples']), c['rate'])
            for key in [(spk, '*')] + ([(spk, name)] if name in scenes else []):
                e, u = per.setdefault(key, ([], []))
                e.append(en.get(w)); u.append(uk)
    bad = []
    with open(out_path, 'w') as o:
        o.write('speaker\tbank\tlines\ten_lufs\tuk_lufs\tdelta\tintended\tok\n')
        for (spk, b), (e, u) in sorted(per.items(), key=lambda kv: (kv[0][1] != '*', kv[0])):
            me, mu = med(e), med(u)
            if me is None or mu is None:
                continue
            want = offset_of(off, spk, b if b != '*' else '')
            d = mu - me
            ok = len(u) < 5 or abs(d - want) <= 1.5
            if not ok:
                bad.append(spk + ('' if b == '*' else ' in ' + b))
            o.write('%s\t%s\t%d\t%.1f\t%.1f\t%+.1f\t%+.1f\t%s\n' % (spk, b, len(u), me, mu, d, want, 'ok' if ok else 'OFF'))
    print(open(out_path).read())
    print('speakers off their level: %s' % (', '.join(bad) or 'none'))
    return not bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd')
    ap.add_argument('dir', nargs='?')
    ap.add_argument('--out', default=os.path.join(WORK, 'loudness_report.tsv'))
    a = ap.parse_args()
    if a.cmd == 'report':
        sys.exit(0 if report(a.dir, a.out) else 1)
    print('%.1f LUFS' % lufs(sdt.load_audio(a.cmd)))


if __name__ == '__main__':
    main()
