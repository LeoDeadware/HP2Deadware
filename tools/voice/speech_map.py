#!/usr/bin/env python3
"""Map every speech clip (Speech_*.sdt slot) to its subtitle string and speaker.

Sources (all read-only, game data of the main checkout):
  EASound/FXMap.dbg   EA sound-designer listing of FXMap.FXM. Per bank the slot ->
                      source wav (sv_<speaker>_<scene>_<line>.wav) and its global
                      sample id; SAMPLE_LOOKUP gives the sample hash the game uses:
                      hash = 0x02000000 | k, k = position in SAMPLE_LOOKUP.
  default.xbe .data   tables of records {u32 text_id, u32 hash[, u32 hash2 ...]}
                      (in-level dialogue, shop keepers, pupils, cards ...).
  CS_*.geo            cutscene event tracks, 20-byte records {u32 time, 0, u32 value,
                      u32 param, 0}: one track holds sample hashes / SFX events,
                      another the subtitle text ids (param = 0,1,2,... line index).
                      A text line belongs to the speech record with the same time
                      (+-12 ticks; +-2 for one-line tracks).
  mods/loc/uk/*.tsv   uk text by id (+ crc); English from the disc (HP2_DATA).

Output TSV (one row per clip): bank index sec sha1 dup_of wav speaker scene hash
text_ids source en uk en_chars uk_chars uk_cps
Usage: speech_map.py OUT.tsv [--slot _UK] [--data DIR] [--loc DIR]   (prints totals)
"""
import argparse, collections, csv, glob, os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import speech_banks  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))  # tools/
import hp2paths  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(hp2paths.DATA, 'game/extracted')
CPS = 14.5  # TTS speaking rate assumed for the fit check (characters per second)


def read_fxmap(path):
    """-> (bank_slots {BANK: {slot: (sample_id, WAVNAME)}}, hash_of {WAVNAME: hash})."""
    lines = open(path, encoding='latin-1').read().splitlines()
    banks, cur, pend, vals = collections.defaultdict(dict), None, None, []
    for ln in lines:
        m = re.match(r'BANK_\d+_(\w+)_SAMPLES:', ln)
        if m:
            cur = m.group(1); continue
        if cur is None:
            continue
        if ln.endswith(':') and not ln.startswith('//'):
            cur = None; continue
        m = re.match(r'// (\d+) \S*?([^\\]+)\.wav', ln, re.I)
        if m:
            pend, vals = (int(m.group(1)), m.group(2).upper()), []; continue
        m = re.match(r'\s+DD\s+([0-9a-fA-F]+)h', ln)
        if m and pend:
            vals.append(int(m.group(1), 16))
            if len(vals) == 2:
                banks[cur][pend[0]] = (vals[0], pend[1]); pend = None
    i = lines.index('SAMPLE_LOOKUP:') + 1
    hash_of, k, name = {}, 0, None
    while i < len(lines) and not lines[i].endswith(':'):
        s = lines[i].strip()
        if s.startswith('//'):
            name = s[3:].strip()
        elif s.startswith('DW'):
            if name:
                hash_of[name] = 0x02000000 | k
            k += 1; name = None
        i += 1
    return banks, hash_of


def read_text(locdir):
    """id -> (en, uk), escaped. The table holds only a crc of the English; the English
    itself comes from the disc (HP2_DATA), never from the repo."""
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'loc'))
    import extract
    return extract.load_with_english(locdir)


def spoken(s):
    """Text as a TTS engine would get it: escapes and glyph codes removed."""
    s = s.replace('\\n', ' ').replace('\\t', ' ')
    # "(hold down [button])" / "(assign to [buttons])": on-screen prompts, never spoken
    s = re.sub(r'\s*\([^()]*\{U[0-9A-Fa-f]{4}\}[^()]*\)', '', s)
    s = re.sub(r'\{U[0-9A-Fa-f]{4}\}', '', s)
    s = re.sub(r'\(\s*\)', '', s)
    return re.sub(r'\s+', ' ', s).strip()


def xbe_pairs(path, sv_hashes, tids):
    b = open(path, 'rb').read()
    a = struct.unpack_from('<%dI' % (len(b) // 4), b)
    out = []
    for i, v in enumerate(a):
        if v not in sv_hashes:
            continue
        j = i - 1
        while j >= 0 and a[j] in sv_hashes:  # {text, hashA, hashB}: variants share the text
            j -= 1
        if j >= 0 and a[j] in tids and i - j <= 3:
            out.append((a[j], v, 'xbe:%x' % (i * 4)))
    return out


def geo_pairs(path, sv_hashes, tids, with_time=False):
    """Cutscene event lists: runs of 20-byte records {time, 0, value, param, 0}. One list
    mixes sample hashes (0x02...), SFX events (0x03.../0x05...) and subtitle ids, whose
    param counts 0,1,2,... over the text records of the list; times start at 0."""
    b = open(path, 'rb').read()
    n = len(b)
    recs = {}
    for o in range(0, n - 20, 4):
        t, z1, v, p, z2 = struct.unpack_from('<5I', b, o)
        if z1 == 0 and z2 == 0 and t < 200000 and ((v >> 24) in (2, 3, 5) or v in tids):
            recs[o] = (t, v, p)
    out, seen = [], set()
    for o0 in sorted(recs):
        if o0 in seen or o0 - 20 in recs:
            continue
        run, o = [], o0
        while o in recs:
            run.append(recs[o]); seen.add(o); o += 20
        sp = [(t, v) for t, v, _p in run if v in sv_hashes]
        tx = []
        for t, v, p in run:
            if v in tids and v < 0x01000000 and p == len(tx):
                tx.append((t, v))
        if not sp or not tx:
            continue
        cand = sorted((abs(ts - tt), i, j) for i, (tt, _) in enumerate(tx)
                      for j, (ts, _) in enumerate(sp) if abs(ts - tt) <= 12)
        ti, sj = set(), set()
        for _d, i, j in cand:  # closest pairs first, each record used once
            if i not in ti and j not in sj:
                ti.add(i); sj.add(j)
                out.append((tx[i][1], sp[j][1], os.path.basename(path)) +
                           ((tx[i][0],) if with_time else ()))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--data', default=DATA)
    ap.add_argument('--loc', default=os.path.join(ROOT, 'mods/loc/uk'))
    ap.add_argument('--slot', default='_UK', help='language dir whose banks are mapped '
                    '(_UK = the slot the Ukrainian mod replaces)')
    a = ap.parse_args()

    banks, hash_of = read_fxmap(os.path.join(a.data, 'EASound/FXMap.dbg'))
    text = read_text(a.loc)
    tids = set(text)
    sv = {h for nm, h in hash_of.items() if nm.startswith('SV_')}

    links = collections.defaultdict(list)  # hash -> [(text_id, source)]
    xp = xbe_pairs(os.path.join(a.data, 'default.xbe'), sv, tids)
    # False hits: a "text id" that sits before
    # many different hashes (3529 "On", 87 hashes) is a flag field of another table.
    fan = collections.Counter(tid for tid, _h, _s in set(xp))
    for tid, h, src in xp:
        if fan[tid] <= 8:
            links[h].append((tid, src))
    for f in sorted(glob.glob(os.path.join(a.data, 'CS_*.geo'))):
        for tid, h, src in geo_pairs(f, sv, tids):
            links[h].append((tid, src))

    # hand-checked fixes from link_audit.py (whisper of the English clip): wav -> text id,
    # '-' = the clip says no string of the game (keeps its own transcript/translation)
    fix = {}
    lp = os.path.join(HERE, 'links.tsv')
    if os.path.exists(lp):
        for r in csv.DictReader((l for l in open(lp, encoding='utf-8') if not l.startswith('#')),
                                delimiter='\t'):
            fix[r['wav']] = r['text_id']
    rows = []
    for f in sorted(glob.glob(os.path.join(a.data, a.slot, 'Speech_*.sdt'))):
        bank = os.path.basename(f)[:-4]
        table = banks.get(bank.upper(), {})
        for c in speech_banks.clips(f):
            sid, wav = table.get(c['index'], (None, ''))
            m = re.match(r'SV_([A-Z0-9]+)_([A-Z0-9]+)_', wav)
            h = hash_of.get(wav)
            ids = []
            for tid, src in links.get(h, []):
                if tid not in [x[0] for x in ids]:
                    ids.append((tid, src))
            if wav in fix:
                ids = [] if fix[wav] == '-' else [(int(fix[wav]), 'hand')]
            rows.append(dict(c, bank=bank, wav=wav, speaker=m.group(1) if m else '?',
                             scene=m.group(2) if m else '?', hash=h, ids=ids))

    first = {}
    for r in rows:
        first.setdefault(r['sha1'], '%s:%d' % (r['bank'], r['index']))
    with open(a.out, 'w', encoding='utf-8') as o:
        o.write('bank\tindex\tsec\tsha1\tdup_of\twav\tspeaker\tscene\thash\ttext_ids\tsource\t'
                'en\tuk\ten_chars\tuk_chars\tuk_cps\n')
        for r in rows:
            k = '%s:%d' % (r['bank'], r['index'])
            # a hash linked to several texts: keep the first (xbe before cutscene) that a
            # voice can say in the clip; > 30 chars/s is a false link (0.8 s "Like a race,
            # Harry? You might win this time.")
            ok = [t for t, _ in r['ids'] if len(spoken(text.get(t, ('', ''))[0])) <= 30 * r['sec']]
            r['ids'] = [x for x in r['ids'] if x[0] in ok]
            tid = ok[0] if ok else None
            en, uk = (spoken(x) for x in text.get(tid, ('', '')))
            r.update(tid=tid, en=en, uk=uk)
            cps = len(uk) / r['sec'] if uk and r['sec'] else 0
            o.write('\t'.join(map(str, [
                r['bank'], r['index'], '%.3f' % r['sec'], r['sha1'],
                '' if first[r['sha1']] == k else first[r['sha1']], r['wav'], r['speaker'],
                r['scene'], '%08x' % r['hash'] if r['hash'] else '',
                ','.join(str(t) for t, _ in r['ids']),
                ';'.join(sorted({s.split(':')[0] for _, s in r['ids']})),
                en, uk, len(en), len(uk), '%.1f' % cps])) + '\n')
    report(rows)


def report(rows):
    uniq = {}
    for r in rows:
        uniq.setdefault(r['sha1'], r)
    u = list(uniq.values())
    mapped = [r for r in u if r['tid'] is not None]
    print('clips %d, unique %d (%.1f min), mapped to a string %d (%.1f min), unmapped %d (%.1f min)' % (
        len(rows), len(u), sum(r['sec'] for r in u) / 60, len(mapped),
        sum(r['sec'] for r in mapped) / 60, len(u) - len(mapped),
        sum(r['sec'] for r in u if r['tid'] is None) / 60))
    # one TTS generation per (speaker, text): re-encoded duplicates are not paid twice
    gen = {}
    for r in mapped:
        gen.setdefault((r['speaker'], r['tid']), r)
    print('TTS generations (speaker,text) %d, uk chars %d, en chars %d' % (
        len(gen), sum(len(r['uk']) for r in gen.values()), sum(len(r['en']) for r in gen.values())))
    over = [r for r in gen.values() if r['sec'] and len(r['uk']) / r['sec'] > CPS]
    print('lines whose uk text needs > %.1f chars/s to fit the en clip: %d of %d' % (
        CPS, len(over), len(gen)))
    g = [r for r in gen.values() if r['sec'] > 0]
    for lang in ('en', 'uk'):
        v = sorted(len(r[lang]) / r['sec'] for r in g)
        print('%s chars/s over the en clip: median %.1f, p75 %.1f, p90 %.1f' % (
            lang, v[len(v) // 2], v[len(v) * 3 // 4], v[len(v) * 9 // 10]))
    ratio = sorted(len(r['uk']) / len(r['en']) for r in g if r['en'])
    print('uk/en length ratio: median %.2f; uk longer than en by >20%%: %d lines' % (
        ratio[len(ratio) // 2], sum(1 for x in ratio if x > 1.2)))
    # stretch needed at a natural TTS pace: seconds of uk speech / en seconds
    need = sorted(len(r['uk']) / CPS / r['sec'] for r in g)
    print('uk at %.1f chars/s vs en clip length: fits(<=1.0) %d, <=1.15 %d, <=1.3 %d, >1.3 %d' % (
        CPS, sum(1 for x in need if x <= 1.0), sum(1 for x in need if 1.0 < x <= 1.15),
        sum(1 for x in need if 1.15 < x <= 1.3), sum(1 for x in need if x > 1.3)))
    per = collections.defaultdict(lambda: [0, 0, 0.0])
    for r in gen.values():
        per[r['speaker']][0] += 1; per[r['speaker']][1] += len(r['uk']); per[r['speaker']][2] += r['sec']
    print('speaker lines uk_chars en_min (mapped)')
    for k, (n, c, sec) in sorted(per.items(), key=lambda kv: -kv[1][1]):
        print('  %-4s %4d %6d %5.1f' % (k, n, c, sec / 60))
    un = collections.defaultdict(float)
    for r in u:
        if r['tid'] is None:
            un[r['speaker']] += r['sec']
    print('unmapped en minutes by speaker: ' + ', '.join(
        '%s %.1f' % (k, v / 60) for k, v in sorted(un.items(), key=lambda kv: -kv[1]) if v >= 30))


if __name__ == '__main__':
    main()
