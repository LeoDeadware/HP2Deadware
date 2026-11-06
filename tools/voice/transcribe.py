#!/usr/bin/env python3
"""English transcripts of the speech clips that have no subtitle link (local, free).

Every unique clip of recon/notes/voice_over_map.tsv without text_ids is decoded from
its _UK bank (sdt.py), resampled to 16 kHz and transcribed by whisper.cpp
(`whisper-cli`, model .cache/HP2Cache/whisper/ggml-base.en.bin). Resumable: clips already
in the output are skipped; results are appended in batches.

  transcribe.py [--limit N]      -> .cache/HP2Cache/voice/uk/transcripts.tsv (wav, sec, text)
  transcribe.py --all            -> transcripts_all.tsv: every clip, subtitled ones too
                                    (audit of clip -> subtitle links, link_audit.py)
"""
import argparse, csv, glob, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sdt, speech_map  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))  # tools/
import hp2paths  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
MAP = os.path.join(ROOT, 'recon/notes/voice_over_map.tsv')
OUT = os.path.join(hp2paths.CACHE, 'voice/uk/transcripts.tsv')
MODEL = os.path.join(hp2paths.CACHE, 'whisper/ggml-base.en.bin')
BATCH = 40


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--limit', type=int)
    ap.add_argument('--all', action='store_true')
    a = ap.parse_args()
    global OUT
    if a.all:
        OUT = OUT.replace('transcripts.tsv', 'transcripts_all.tsv')
    done = set()
    if os.path.exists(OUT):
        done = {r['wav'] for r in csv.DictReader(open(OUT, encoding='utf-8'), delimiter='\t')}
    todo = []
    for r in csv.DictReader(open(MAP, encoding='utf-8'), delimiter='\t', quoting=csv.QUOTE_NONE):
        if (a.all or not r['text_ids']) and not r['dup_of'] and r['wav'] and r['wav'] not in done:
            todo.append(r)
    seen, uniq = set(), []
    for r in todo:  # one transcript per source wav
        if r['wav'] not in seen:
            seen.add(r['wav']); uniq.append(r)
    if a.limit:
        uniq = uniq[:a.limit]
    print('%d clips to transcribe' % len(uniq), flush=True)
    files = {os.path.basename(f).upper()[:-4]: f
             for f in glob.glob(os.path.join(speech_map.DATA, '_UK', 'Speech_*.sdt'))}
    banks = {}
    new = not os.path.exists(OUT)
    with open(OUT, 'a', encoding='utf-8') as out:
        if new:
            out.write('wav\tsec\ttext\n')
        for b0 in range(0, len(uniq), BATCH):
            part = uniq[b0:b0 + BATCH]
            with tempfile.TemporaryDirectory() as tmp:
                wavs = []
                for r in part:
                    bank = r['bank'].upper()
                    if bank not in banks:
                        banks[bank] = sdt.read_bank(files[bank])
                    c = banks[bank]['clips'][int(r['index'])]
                    p22 = os.path.join(tmp, r['wav'] + '_22.wav')
                    p16 = os.path.join(tmp, r['wav'] + '.wav')
                    sdt.write_wav(p22, sdt.adpcm_decode(c['data'], c['samples']), c['rate'])
                    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', p22, '-ar', '16000', p16],
                                   check=True)
                    wavs.append((r, p16))
                cmd = ['whisper-cli', '-m', MODEL, '-nt', '-np', '-otxt', '-l', 'en', '-t', '4']
                if subprocess.run(cmd + [x for _r, p in wavs for x in ('-f', p)],
                                  capture_output=True).returncode:
                    for _r, p in wavs:  # a bad clip (silent placeholder) crashed the batch
                        subprocess.run(cmd + ['-f', p], capture_output=True)
                for r, p in wavs:
                    txt = open(p + '.txt', encoding='utf-8').read() if os.path.exists(p + '.txt') else ''
                    txt = ' '.join(txt.split())
                    out.write('%s\t%s\t%s\n' % (r['wav'], r['sec'], txt))
            out.flush()
            print('%d/%d' % (min(b0 + BATCH, len(uniq)), len(uniq)), flush=True)


if __name__ == '__main__':
    main()
