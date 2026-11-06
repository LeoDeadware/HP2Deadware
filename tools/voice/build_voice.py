#!/usr/bin/env python3
"""Generated Ukrainian audio -> overlay speech banks the game plays.

For every _UK/Speech_*.sdt slot whose source wav belongs to a line of lines.tsv with
a generated .cache/HP2Cache/voice/uk/audio/<key>.mp3, the clip is replaced by that audio
(22050 Hz mono Xbox ADPCM); every other slot keeps the English data.
Loudness: every Ukrainian line gets the loudness of the English clip it replaces
(loudness.lufs: K-weighted, speech parts only), held within +-LEVEL_CLAMP dB of that
speaker's English median, plus the intended offsets of levels.tsv (per speaker, per
scene bank). So a scene keeps the original balance (Molly shouting over the twins) and
a deep TTS voice is not quieter for the same RMS (the narrator, 2026-11-01 report);
peaks above -4.4 dBFS go through a soft (tanh) limiter, ceiling -1 dBFS,
instead of lowering the whole line. Banks without any replacement are not written.
Per line: pitch shift from voices.tsv (semitones, `pitch` column), then the English
clip's pause pattern (timing.retime: lead-in, inner pauses, tail), then never longer
than the English clip (tempo to the English length; lines that needed more than
MAX_TEMPO are listed in overruns.tsv for a shorter text; tempo.tsv has every line).
Layout: the bank keeps the ORIGINAL layout byte for byte (patch_inplace) — same file
size, every clip at its original offset in its original byte slot; only the PT sample
count and rate change. (A re-laid bank, even no bigger than the original, still played
garbage at the washing-machine fight: a 16 kHz voice with rms 0.54 in the game log.) The game loads speech banks
into the sound memory it shares with the music stream; a bigger bank corrupted the
music (heavy distortion after Molly's long line in the Burrow, user report 2026-10-31).
When a bank would grow, the clips longer than their English take are stored at a lower
sample rate (16 kHz, then 14 kHz; the PT 0x84 rate is honoured per clip — tone test:
1 kHz at 16 kHz plays as 1 kHz), longest-over first, until the bank fits; only if that
is not enough (rates down to 11025 Hz) are they sped up (tempo, pitch kept), at most
1.25x; a clip that still does not fit stays English, so a bank never grows.

  build_voice.py [--out DIR]    (default .cache/HP2Cache/voice/uk/mods; run the game with
                                 HP2_MODS=$HP2_DATA/mods/uk:DIR HP2_LANG=uk)
"""
import argparse, csv, glob, math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sdt  # noqa: E402
import timing  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))  # tools/
import hp2paths  # noqa: E402

DATA = os.path.join(hp2paths.DATA, 'game/extracted')
WORK = os.path.join(hp2paths.CACHE, 'voice/uk')


def rms(pcm):
    return math.sqrt(sum(x * x for x in pcm) / len(pcm)) if pcm else 0.0


KNEE, CEIL = 19700.0, 29200.0  # -4.4 dBFS knee, -1 dBFS ceiling


def limit(x):
    a = abs(x)
    if a <= KNEE:
        return int(x)
    y = KNEE + (CEIL - KNEE) * math.tanh((a - KNEE) / (CEIL - KNEE))
    return int(y if x > 0 else -y)


def tempo(pcm, n):
    """Change the length of pcm to n samples, pitch kept (ffmpeg atempo)."""
    import struct, subprocess
    f = len(pcm) / n
    chain = []
    while f > 2.0:
        chain.append('atempo=2.0'); f /= 2.0
    chain.append('atempo=%f' % f)
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-f', 's16le', '-ar', str(sdt.RATE), '-ac', '1',
                          '-i', '-', '-af', ','.join(chain), '-f', 's16le', '-'],
                         input=struct.pack('<%dh' % len(pcm), *pcm), check=True,
                         capture_output=True).stdout
    out = list(struct.unpack('<%dh' % (len(raw) // 2), raw))
    return (out + [0] * n)[:n]


def resample(pcm, rate):
    """22050 Hz pcm -> rate (ffmpeg, high quality)."""
    import struct, subprocess
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-f', 's16le', '-ar', str(sdt.RATE), '-ac', '1',
                          '-i', '-', '-af', 'aresample=%d:filter_size=64:cutoff=0.97' % rate, '-f', 's16le', '-'],
                         input=struct.pack('<%dh' % len(pcm), *pcm), check=True,
                         capture_output=True).stdout
    return list(struct.unpack('<%dh' % (len(raw) // 2), raw))


def clip_bytes(samples):
    return ((samples + 63) // 64 * 36 + 15) // 16 * 16


def fit_budget(new, eng, budget):
    """new/eng: {slot: samples}; -> {slot: capped samples} so the data fits budget."""
    def total(k):
        return sum(clip_bytes(min(n, max(eng[i], int(eng[i] * k)))) for i, n in new.items())
    if total(100.0) <= budget:
        return dict(new)
    lo, hi = 1.0, 3.0
    for _ in range(30):
        mid = (lo + hi) / 2
        lo, hi = (mid, hi) if total(mid) <= budget else (lo, mid)
    return {i: min(n, max(eng[i], int(eng[i] * lo))) for i, n in new.items()}


RATES = (22050, 16000, 14000, 12000, 11025)
MAX_TEMPO = 1.15  # speed-up still natural for speech (1.3 sounded rushed, 2026-11-01)
LEVEL_CLAMP = 4.0  # dB: a line's loudness stays within this of its speaker's English median


def patch_inplace(src, out, done):
    """Write Ukrainian clips into the ORIGINAL bank layout: every clip keeps its data
    offset and its byte slot; only the PT sample count (0x85) and rate (0x84) change,
    the file keeps its size. A clip that does not fit its English slot at any rate (down
    to 11025 Hz, then tempo <= 1.25x) stays English. -> (put, kept, low, sped)"""
    import struct
    b = bytearray(open(src, 'rb').read())
    put = kept = low = sped = 0
    for i, pcm in sorted(done.items()):
        pt = 20 + 4 * i + struct.unpack_from('<I', b, 20 + 4 * i)[0]
        q, pos = pt + 4, {}
        while b[q] != 0xFF:  # tag -> (value offset, width)
            t = b[q]; q += 1
            if t in (0xFD, 0xFE):
                continue
            w = b[q]; q += 1
            pos[t] = (q, w); q += w
        val = lambda t: int.from_bytes(b[pos[t][0]:pos[t][0] + pos[t][1]], 'big')
        samples, off = val(0x85), val(0x88)
        room = (samples + 63) // 64 * 36
        cap = room // 36 * 64  # samples that fit the slot
        choice = None
        for r in RATES:
            if len(pcm) * r // sdt.RATE <= cap:
                choice = (r, pcm); break
        if choice is None:
            need = cap * sdt.RATE // RATES[-1]
            if len(pcm) <= need * 1.25:
                choice = (RATES[-1], tempo(pcm, need)); sped += 1
        if choice is None:
            kept += 1; continue
        r, p = choice
        if r != sdt.RATE:
            p = resample(p, r); low += 1
        p = p[:cap]
        data = sdt.adpcm_encode(p)
        b[off:off + room] = data + bytes(room - len(data))
        o, w = pos[0x85]
        if len(p) >= 1 << (8 * w):
            kept += 1; continue
        b[o:o + w] = len(p).to_bytes(w, 'big')
        o, w = pos[0x84]
        b[o:o + w] = r.to_bytes(w, 'big')
        put += 1
    with open(out, 'wb') as fh:
        fh.write(b)
    return put, kept, low, sped


def speech_rms(pcm):
    """RMS over the speech parts only (timing.segments), so pauses do not count."""
    seg = [x for a, b in timing.segments(pcm) for x in pcm[a:b]]
    return rms(seg) if seg else rms(pcm)


def english_level(data, cache):
    """Median speech RMS of the English clips (every 7th clip of every _UK bank)."""
    if os.path.exists(cache):
        return float(open(cache).read())
    vals = []
    for f in sorted(glob.glob(os.path.join(data, '_UK', 'Speech_*.sdt'))):
        for c in sdt.read_bank(f)['clips'][::7]:
            v = speech_rms(sdt.adpcm_decode(c['data'], c['samples']))
            if v > 0:
                vals.append(v)
    lvl = sorted(vals)[len(vals) // 2]
    open(cache, 'w').write('%.1f' % lvl)
    return lvl


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(WORK, 'mods'))
    ap.add_argument('--data', default=DATA)
    ap.add_argument('--only', help='build just these banks (comma list, e.g. Speech_Burrows)')
    ap.add_argument('--silence-english', action='store_true',
                    help='test: clips without Ukrainian audio become silence')
    a = ap.parse_args()
    target = english_level(a.data, os.path.join(WORK, 'en_level.txt'))
    print('target speech RMS %.0f (%.1f dBFS)' % (target, 20 * math.log10(target / 32768)))
    pitch = {}
    vt = os.path.join(HERE, 'voices.tsv')
    for r in csv.DictReader(open(vt, encoding='utf-8'), delimiter='\t'):
        pitch[r['speaker']] = float(r.get('pitch') or 0)
    import loudness
    en_lufs = loudness.english_table(a.data)
    off = loudness.offsets()
    spk_med = {}
    for w, v in en_lufs.items():
        if v is not None and w.startswith('SV_'):
            spk_med.setdefault(w.split('_')[1], []).append(v)
    spk_med = {k: loudness.med(v) for k, v in spk_med.items()}
    audio_of = {}
    for r in csv.DictReader(open(os.path.join(WORK, 'lines.tsv'), encoding='utf-8'), delimiter='\t'):
        mp3 = os.path.join(WORK, 'audio', r['key'] + '.mp3')
        if os.path.exists(mp3):
            for w in r['wavs'].split(','):
                audio_of[w] = (mp3, pitch.get(r['speaker'], 0.0), r['speaker'])
    import speech_map
    banks, _ = speech_map.read_fxmap(os.path.join(a.data, 'EASound/FXMap.dbg'))
    os.makedirs(os.path.join(a.out, '_UK'), exist_ok=True)
    total = 0
    overruns, tempos = [], []
    for f in sorted(glob.glob(os.path.join(a.data, '_UK', 'Speech_*.sdt'))):
        name = os.path.basename(f)[:-4]
        if a.only and name not in a.only.split(','):
            continue
        table = banks.get(name.upper(), {})
        bank = sdt.read_bank(f)
        n = 0
        done = {}  # slot -> pcm (Ukrainian, levelled)
        for i, c in enumerate(bank['clips']):
            src = audio_of.get(table.get(i, (None, ''))[1])
            if not src:
                if a.silence_english:
                    done[i] = [0] * c['samples']
                continue
            mp3, semis, spk = src
            pcm = sdt.load_audio(mp3, semis)
            en = sdt.adpcm_decode(c['data'], c['samples'])
            pcm = timing.retime(pcm, en, len(en) * sdt.RATE // c['rate'])
            # never longer than the English line: the game times the next line by it
            # (cutscene ticks); speed up to MAX_TEMPO, longer ones are listed for a
            # shorter text (overruns.tsv)
            en_n = len(en) * sdt.RATE // c['rate']
            wav = table.get(i, (0, ''))[1]
            f_need = len(pcm) / en_n
            tempos.append((name, i, wav, spk, '%.3f' % f_need))
            if len(pcm) > en_n:
                pcm = tempo(pcm, en_n)  # always fits: no overlap with the next line
                if f_need > MAX_TEMPO:  # faster than natural: wants a shorter text
                    overruns.append((name, i, wav, spk, '%.2f' % f_need))
            m = spk_med.get(spk)
            want = en_lufs.get(wav)
            if m is not None:
                want = m if want is None else min(m + LEVEL_CLAMP, max(m - LEVEL_CLAMP, want))
            have = loudness.lufs(pcm)
            if want is None or have is None:  # no English reference: overall level
                lvl = speech_rms(pcm)
                g = target / lvl if lvl else 1.0
            else:
                g = 10 ** ((want + loudness.offset_of(off, spk, name, wav) - have) / 20)
            done[i] = [limit(x * g) for x in pcm]
        if done:
            out = os.path.join(a.out, '_UK', name + '.sdt')
            put, kept, low, sped = patch_inplace(f, out, done)
            n += put
            print('%s: %d in place (%d at a lower rate, %d sped up), %d kept English (no room)'
                  % (name, put, low, sped, kept))
        if n:
            print('%s: %d of %d clips Ukrainian' % (name, n, len(bank['clips'])))
            total += n
    print('%d clips replaced -> %s' % (total, a.out))
    for fn, rows in (('overruns.tsv', overruns), ('tempo.tsv', tempos)):
        if a.only:
            fn = fn.replace('.tsv', '_only.tsv')
        with open(os.path.join(WORK, fn), 'w') as o:
            o.write('bank\tslot\twav\tspeaker\tneed\n')
            for r in rows:
                o.write('\t'.join(map(str, r)) + '\n')
    print('%d clips still longer than English after %.2fx (overruns.tsv)' % (len(overruns), MAX_TEMPO))


if __name__ == '__main__':
    main()
