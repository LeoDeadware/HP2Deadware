#!/usr/bin/env python3
"""Listening check of generated takes without ears: whisper.cpp (multilingual base,
-l uk) transcribes each take and the transcript is compared with the text that was
spoken (letters only, difflib). A low similarity or a transcript much longer than the
text marks a garbled take or an extra phrase (the {U1710} case).

  qa_takes.py KEYS_FILE [--audio DIR] [--min 0.55] [--retry]  -> prints flagged keys, writes
  .cache/HP2Cache/voice/uk/qa_<name>.tsv (key, similarity, length ratio, text, transcript)
"""
import argparse, csv, difflib, os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))  # tools/
import hp2paths  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(hp2paths.CACHE, 'voice/uk')
MODEL = os.path.join(hp2paths.CACHE, 'whisper/ggml-base.bin')
# the game's made-up words, as a whisper prompt (base hears «Гафелпаф» as "half a puff");
# a prompt can also make whisper invent these words on a silent take: the length ratio
# check flags that instead of hiding it
VOCAB = ('Ґрифіндор, Гафелпаф, Рейвенклов, Слизерин, квафл, снич, бладжер, Поттер, Гаррі, '
         'Мелфой, Спіннет, Белл, Джонсон, Флінт, Гоґвортс, Геґрід, Дамблдор, Локарт, драже')


U = ['', 'один', 'два', 'три', 'чотири', 'п’ять', 'шість', 'сім', 'вісім', 'дев’ять', 'десять',
     'одинадцять', 'дванадцять', 'тринадцять', 'чотирнадцять', 'п’ятнадцять', 'шістнадцять',
     'сімнадцять', 'вісімнадцять', 'дев’ятнадцять']
T = ['', '', 'двадцять', 'тридцять', 'сорок', 'п’ятдесят', 'шістдесят', 'сімдесят', 'вісімдесят',
     'дев’яносто']
H = ['', 'сто', 'двісті', 'триста', 'чотириста', 'п’ятсот', 'шістсот', 'сімсот', 'вісімсот',
     'дев’ятсот']
LAT = [('sh', 'ш'), ('ch', 'ч'), ('zh', 'ж'), ('kh', 'х'), ('ts', 'ц'), ('ya', 'я'), ('yu', 'ю'),
       ('a', 'а'), ('b', 'б'), ('c', 'к'), ('d', 'д'), ('e', 'е'), ('f', 'ф'), ('g', 'г'), ('h', 'г'),
       ('i', 'і'), ('j', 'дж'), ('k', 'к'), ('l', 'л'), ('m', 'м'), ('n', 'н'), ('o', 'о'), ('p', 'п'),
       ('q', 'к'), ('r', 'р'), ('s', 'с'), ('t', 'т'), ('u', 'у'), ('v', 'в'), ('w', 'в'), ('x', 'кс'),
       ('y', 'і'), ('z', 'з')]
# wordless lines (coughs, hums, screams): whisper hears nothing, nothing to compare
NONVERBAL = re.compile(r'^((кхе|к?х[еа]|г?м+н*|н*м+|а+|о+|у+|е+|б+|ох|ух|ах|гм|хм|ха|хі|ой|ай|гр+|р+)\s*)+$')


def number(n):
    if n >= 1000:
        return str(n)
    return ' '.join(x for x in (H[n // 100], T[n % 100 // 10] if n % 100 >= 20 else '',
                                U[n % 100] if n % 100 < 20 else U[n % 10]) if x)


def letters(s):
    """Lower-case letters only; digits as Ukrainian words, Latin transliterated (whisper
    writes "18" and "Harry Potter" for «вісімнадцять», «Гаррі Поттер»)."""
    s = re.sub(r'\[[^\]]*\]', ' ', s.lower()).replace('́', '')
    s = re.sub(r'\d+', lambda m: ' %s ' % number(int(m.group())), s)
    for a, b in LAT:
        s = s.replace(a, b)
    return ' '.join(re.findall(r"[а-яґєії’']+", s))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('keys')
    ap.add_argument('--audio', default=os.path.join(WORK, 'audio'))
    ap.add_argument('--min', type=float, default=0.55)
    a = ap.parse_args()
    L = {r['key']: r for r in csv.DictReader(open(os.path.join(WORK, 'lines.tsv'), encoding='utf-8'),
                                             delimiter='\t')}
    keys = [k for k in open(a.keys).read().split() if k in L]
    res = []
    with tempfile.TemporaryDirectory() as tmp:
        wavs = []
        for k in keys:
            src = os.path.join(a.audio, k + '.mp3')
            if not os.path.exists(src):
                continue
            w = os.path.join(tmp, k + '.wav')
            subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', src, '-ar', '16000', '-ac', '1', w],
                           check=True)
            wavs.append((k, w))
        for b in range(0, len(wavs), 40):
            part = wavs[b:b + 40]
            cmd = ['whisper-cli', '-m', MODEL, '-l', 'uk', '-nt', '-np', '-otxt', '-t', '6',
                   '-bs', '5', '--prompt', VOCAB]
            if subprocess.run(cmd + [x for _k, w in part for x in ('-f', w)], capture_output=True).returncode:
                for _k, w in part:
                    subprocess.run(cmd + ['-f', w], capture_output=True)
            for k, w in part:
                tr = open(w + '.txt').read().strip() if os.path.exists(w + '.txt') else ''
                ref = letters(L[k]['dub_uk'] or L[k]['uk'])
                hyp = letters(tr)
                sim = difflib.SequenceMatcher(None, ref, hyp, autojunk=False).ratio() if ref else 1.0
                if NONVERBAL.match(ref):
                    sim = 1.0
                ratio = len(hyp) / max(1, len(ref))
                res.append((k, sim, ratio, ref, hyp))
    name = os.path.basename(a.audio.rstrip('/'))
    with open(os.path.join(WORK, 'qa_%s.tsv' % name), 'w', encoding='utf-8') as o:
        o.write('key\tsim\tratio\ttext\ttranscript\n')
        for r in res:
            o.write('%s\t%.2f\t%.2f\t%s\t%s\n' % r)
    bad = [r for r in res if (r[1] < a.min or r[2] > 1.6) and len(r[3]) >= 8 and not NONVERBAL.match(r[3])]
    sims = sorted(r[1] for r in res)
    print('%s: %d takes, similarity median %.2f, flagged %d' % (name, len(res), sims[len(sims) // 2] if sims else 0, len(bad)))
    for r in bad:
        print('  %s sim %.2f len x%.1f | %s | heard: %s' % (r[0], r[1], r[2], r[3][:60], r[4][:60]))
    with open(os.path.join(WORK, 'qa_%s_flagged.txt' % name), 'w') as o:
        o.write('\n'.join(r[0] for r in bad))


if __name__ == '__main__':
    main()
