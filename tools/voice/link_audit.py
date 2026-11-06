#!/usr/bin/env python3
"""Audit of the clip -> subtitle links: does the English clip say its linked string?

Input: recon/notes/voice_over_map.tsv (links), .cache/HP2Cache/voice/uk/transcripts_all.tsv
(transcribe.py --all: whisper of every English clip), strings of mods/loc/uk/*.tsv.
Per linked clip: similarity of the transcript to the linked English string
(speech_map.spoken, match_text.norm) and the best string of the whole game.
  ok       sim >= 0.6 (or the transcript is too short to judge: < 2 words)
  relink   another string matches clearly better (best >= 0.8, best - sim >= 0.25)
  unlink   nothing matches: the clip does not say its string (a prompt or menu word
           linked to a grunt, a wrong neighbour in the cutscene event list)

  link_audit.py   -> .cache/HP2Cache/voice/uk/link_audit.tsv; prints the counts and the
                     non-ok rows. The fixes go to links.tsv (hand-checked).
"""
import csv, difflib, glob, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import speech_map  # noqa: E402
from match_text import norm  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))  # tools/
import hp2paths  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
WORK = os.path.join(hp2paths.CACHE, 'voice/uk')


def sim(a, b):
    return difflib.SequenceMatcher(None, a, b).ratio() if a and b else 0.0


def main():
    strings = {}
    for tid, (en, _) in speech_map.read_text(os.path.join(ROOT, 'mods/loc/uk')).items():
        if en:
            strings.setdefault(str(tid), norm(speech_map.spoken(en)))
    tr = {r['wav']: norm(r['text']) for r in csv.DictReader(
        open(os.path.join(WORK, 'transcripts_all.tsv'), encoding='utf-8'), delimiter='\t')}
    out, counts = [], {}
    for r in csv.DictReader(open(os.path.join(ROOT, 'recon/notes/voice_over_map.tsv'), encoding='utf-8'),
                            delimiter='\t', quoting=csv.QUOTE_NONE):
        if not r['text_ids'] or r['dup_of'] or r['wav'] not in tr:
            continue
        t = tr[r['wav']]
        linked = r['text_ids'].split(',')[0]
        s = max(sim(t, strings.get(i, norm(speech_map.spoken(r['en'])))) for i in r['text_ids'].split(','))
        best_id, best = linked, s
        if s < 0.6 and len(t.split()) >= 2:
            for i, e in strings.items():
                v = sim(t, e)
                if v > best:
                    best_id, best = i, v
        if s >= 0.6 or len(t.split()) < 2:
            st = 'ok'
        elif best >= 0.8 and best - s >= 0.25:
            st = 'relink'
        else:
            st = 'unlink'
        counts[st] = counts.get(st, 0) + 1
        out.append((r['bank'], r['index'], r['wav'], r['speaker'], r['text_ids'], '%.2f' % s, best_id,
                    '%.2f' % best, st, t, norm(speech_map.spoken(r['en']))))
    with open(os.path.join(WORK, 'link_audit.tsv'), 'w', encoding='utf-8') as o:
        o.write('bank\tindex\twav\tspeaker\ttext_ids\tsim\tbest_id\tbest\tstatus\ttranscript\ten\n')
        for x in out:
            o.write('\t'.join(x) + '\n')
    print('%d linked clips: %s' % (len(out), ', '.join('%s %d' % kv for kv in sorted(counts.items()))))
    for x in out:
        if x[8] != 'ok':
            print('%-7s %s %s/%s %s sim %s best %s:%s | heard: %s | linked: %s'
                  % (x[8], x[2], x[0], x[1], x[4], x[5], x[6], x[7], x[9][:60], x[10][:60]))


if __name__ == '__main__':
    main()
