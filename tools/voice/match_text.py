#!/usr/bin/env python3
"""Find the game's own subtitle string for each transcribed (unlinked) speech clip.

Input .cache/HP2Cache/voice/uk/transcripts.tsv (transcribe.py); strings = en/uk columns of
mods/loc/uk/*.tsv. Text is normalised (case, punctuation, curly quotes, ". . ."),
then compared with difflib; the best string per clip is kept with its score.
  score >= 0.85        'match'   (the existing Ukrainian translation is used)
  0.60 <= score < 0.85 'review'  (checked by hand)
  else                 'none'    (needs a new translation)

  match_text.py   -> .cache/HP2Cache/voice/uk/matches.tsv (wav sec speaker score status
                     text_id transcript en uk)
"""
import csv, difflib, glob, os, re, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))  # tools/
import hp2paths  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WORK = os.path.join(hp2paths.CACHE, 'voice/uk')


def norm(s):
    s = s.lower().replace('’', "'").replace('‘', "'")
    s = re.sub(r'\{u[0-9a-f]{4}\}|\\n', ' ', s)
    s = re.sub(r"[^a-z0-9' ]+", ' ', s)
    return ' '.join(s.split())


def main():
    strings = {}
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'loc'))
    import extract  # English from the disc (HP2_DATA); the table keeps only its crc
    strings.update(extract.load_with_english(os.path.join(ROOT, 'mods/loc/uk')))
    cand = [(tid, norm(en), en, uk) for tid, (en, uk) in strings.items() if len(norm(en)) >= 2]
    by_word = {}
    for c in cand:  # index by words to keep the search fast
        for w in set(c[1].split()):
            by_word.setdefault(w, []).append(c)
    out = []
    for r in csv.DictReader(open(os.path.join(WORK, 'transcripts.tsv'), encoding='utf-8'), delimiter='\t', quoting=csv.QUOTE_NONE):
        t = norm(r['text'])
        best = (0.0, None)
        if t:
            pool = {id(c): c for w in set(t.split()) for c in by_word.get(w, [])}.values()
            for c in pool:
                sc = difflib.SequenceMatcher(None, t, c[1]).ratio()
                if sc > best[0]:
                    best = (sc, c)
        sc, c = best
        status = 'match' if sc >= 0.85 else 'review' if sc >= 0.6 else 'none'
        spk = r['wav'].split('_')[1] if r['wav'].startswith('SV_') else '?'
        out.append([r['wav'], r['sec'], spk, '%.2f' % sc, status, str(c[0]) if c else '',
                    r['text'], c[2] if c else '', c[3] if c else ''])
    with open(os.path.join(WORK, 'matches.tsv'), 'w', encoding='utf-8') as f:
        f.write('wav\tsec\tspeaker\tscore\tstatus\ttext_id\ttranscript\ten\tuk\n')
        for o in out:
            f.write('\t'.join(x.replace('\t', ' ') for x in o) + '\n')
    for st in ('match', 'review', 'none'):
        sel = [o for o in out if o[4] == st]
        print('%-6s %4d clips, %.1f min' % (st, len(sel), sum(float(o[1]) for o in sel) / 60))


if __name__ == '__main__':
    main()
