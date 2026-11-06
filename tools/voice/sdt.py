#!/usr/bin/env python3
"""Read, write and patch EA BNKl speech banks (Speech_*.sdt); Xbox ADPCM codec.

Layout (see speech_banks.py): 'BNKl', u16 version, u16 count, u32 header size,
u32 data size, u32 0, u32 slot[count] (PT offset relative to the slot), PT headers
(4-byte aligned), header padded to 32, then the clip data (16-byte aligned).
The writer keeps every PT tag of a clip as it was and rewrites only 0x85 (sample
count) and 0x88 (data offset); clips come out in slot order.

Commands:
  sdt.py roundtrip DIR...            every *.sdt: read + write + read == same clips
  sdt.py codec-test BANK.sdt         decoder == ffmpeg adpcm_ima_xbox (sample exact),
                                     encode(decode(clip)) SNR per clip
  sdt.py extract BANK.sdt OUTDIR     one 22050 Hz mono WAV per slot (<slot>.wav)
  sdt.py replace BANK.sdt OUT.sdt SLOT=AUDIO [SLOT=AUDIO ...]
                                     AUDIO: any file ffmpeg reads (converted to
                                     22050 Hz mono); other slots are copied as is
"""
import os, struct, subprocess, sys, tempfile, wave

INDEX = [-1, -1, -1, -1, 2, 4, 6, 8] * 2
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55,
        60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
        1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
        5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
        20350, 22385, 24623, 27086, 29794, 32767]
RATE = 22050


def _step(pred, index, nib):
    diff = ((2 * (nib & 7) + 1) * STEP[index]) >> 3
    pred = pred - diff if nib & 8 else pred + diff
    pred = -32768 if pred < -32768 else 32767 if pred > 32767 else pred
    index += INDEX[nib]
    return pred, 0 if index < 0 else 88 if index > 88 else index


def adpcm_decode(data, samples):
    """Mono Xbox ADPCM -> list of int16 (same rules as src/hle/dsound/adpcm.cpp)."""
    out = []
    for blk in range(0, len(data) - 35, 36):
        pred = struct.unpack_from('<h', data, blk)[0]
        index = min(data[blk + 2], 88)
        out.append(pred)
        s = 1
        for byte in data[blk + 4:blk + 36]:
            for nib in (byte & 15, byte >> 4):
                if s < 64:
                    pred, index = _step(pred, index, nib)
                    out.append(pred)
                s += 1
    return out[:samples]


def adpcm_encode(pcm):
    """list of int16 -> mono Xbox ADPCM (64 samples / 36 bytes; best nibble per sample)."""
    out = bytearray()
    index = 0
    n = len(pcm)
    for b0 in range(0, n, 64):
        blk = pcm[b0:b0 + 64]
        blk = blk + [blk[-1] if blk else 0] * (64 - len(blk))
        pred = blk[0]
        out += struct.pack('<hBB', pred, index, 0)
        nibs = []
        for x in blk[1:]:
            st = STEP[index]
            d = x - pred
            sign = 8 if d < 0 else 0
            m = min(7, (abs(d) * 4) // st)
            best = None
            for mm in (m - 1, m, m + 1):  # the exact best is next to the quantised value
                if 0 <= mm <= 7:
                    p, i = _step(pred, index, sign | mm)
                    e = abs(p - x)
                    if best is None or e < best[0]:
                        best = (e, sign | mm, p, i)
            if d == 0 or best[0] > 0:  # sign of a tiny delta can go either way
                p, i = _step(pred, index, sign ^ 8)
                if abs(p - x) < best[0]:
                    best = (abs(p - x), sign ^ 8, p, i)
            _, nib, pred, index = best
            nibs.append(nib)
        nibs.append(0)  # the 64th nibble is not played
        for k in range(0, 64, 2):
            out.append(nibs[k] | nibs[k + 1] << 4)
    return bytes(out)


def _tags(b, q):
    """PT tag list [(tag, raw value bytes or None)] from just after 'PT\\x07\\x00'."""
    tags = []
    while b[q] != 0xFF:
        t = b[q]; q += 1
        if t in (0xFD, 0xFE):
            tags.append((t, None)); continue
        n = b[q]; q += 1
        tags.append((t, bytes(b[q:q + n]))); q += n
    return tags


def read_bank(path):
    b = open(path, 'rb').read()
    if b[:4] != b'BNKl':
        raise ValueError(path + ': not a BNKl bank')
    ver, cnt = struct.unpack_from('<HH', b, 4)
    clips = []
    for i in range(cnt):
        pt = 20 + 4 * i + struct.unpack_from('<I', b, 20 + 4 * i)[0]
        tags = _tags(b, pt + 4)
        val = {t: int.from_bytes(v, 'big') for t, v in tags if v is not None}
        samples, off = val[0x85], val[0x88]
        size = (samples + 63) // 64 * 36
        clips.append(dict(tags=tags, samples=samples, rate=val.get(0x84, RATE),
                          data=b[off:off + size]))
    return dict(version=ver, clips=clips)


def _be(v, old=b''):
    """Big-endian bytes of v, as wide as the old field when it still fits."""
    n = max(len(old), 1)
    while v >= 1 << (8 * n):
        n += 1
    return v.to_bytes(n, 'big')


def write_bank(path, bank):
    clips = bank['clips']
    cnt = len(clips)

    def pt_bytes(c, off):
        out = bytearray(b'PT\x07\x00')
        for t, v in c['tags']:
            if v is None:
                out.append(t); continue
            if t == 0x85:
                v = _be(c['samples'], v)
            elif t == 0x88:
                v = off.to_bytes(4, 'big')
            out += bytes([t, len(v)]) + v
        out.append(0xFF)
        while len(out) % 4:
            out.append(0)
        return bytes(out)

    # header size depends only on tag lengths (0x88 is always 4 bytes)
    pos = 20 + 4 * cnt
    lens = [len(pt_bytes(c, 0)) for c in clips]
    hs = (pos + sum(lens) + 31) // 32 * 32
    offs, d = [], hs
    for c in clips:
        offs.append(d)
        d = (d + len(c['data']) + 15) // 16 * 16
    head = bytearray(struct.pack('<4sHHIII', b'BNKl', bank['version'], cnt, hs, d - hs, 0))
    pts = bytearray()
    for i, c in enumerate(clips):
        slot = 20 + 4 * i
        head += struct.pack('<I', pos + len(pts) - slot)
        pts += pt_bytes(c, offs[i])
    out = bytearray(head + pts)
    out += bytes(hs - len(out))
    for i, c in enumerate(clips):
        out += c['data']
        out += bytes((offs[i + 1] if i + 1 < cnt else d) - len(out))
    with open(path, 'wb') as f:
        f.write(out)


def load_audio(path, semitones=0.0):
    """Any audio file -> list of int16 at 22050 Hz mono (ffmpeg); optional pitch shift
    (resample: formants move with the pitch, length kept by atempo)."""
    af = []
    if semitones:
        r = 2 ** (semitones / 12)
        af = ['-af', 'asetrate=44100*%f,aresample=44100,atempo=%f' % (r, 1 / r)]
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-ar', '44100'] + af +
                         ['-f', 's16le', '-ac', '1', '-ar', str(RATE), '-'],
                         check=True, capture_output=True).stdout
    return list(struct.unpack('<%dh' % (len(raw) // 2), raw))


def clip_from_pcm(template, pcm):
    c = dict(template)
    c['samples'] = len(pcm)
    c['data'] = adpcm_encode(pcm)
    return c


def write_wav(path, pcm, rate=RATE):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(struct.pack('<%dh' % len(pcm), *pcm))


def _xbox_wav(data, rate):
    """Raw mono Xbox ADPCM -> RIFF WAVE (format 0x69) that ffmpeg decodes."""
    fmt = struct.pack('<HHIIHHHH', 0x69, 1, rate, rate * 36 // 64, 36, 4, 2, 64)
    return (b'RIFF' + struct.pack('<I', 4 + 8 + len(fmt) + 8 + len(data)) + b'WAVEfmt ' +
            struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(data)) + data)


def cmd_roundtrip(dirs):
    bad = n = 0
    for d in dirs:
        for name in sorted(os.listdir(d)):
            if not name.lower().endswith('.sdt'):
                continue
            a = read_bank(os.path.join(d, name))
            with tempfile.NamedTemporaryFile(suffix='.sdt') as t:
                write_bank(t.name, a)
                b = read_bank(t.name)
            # 0x88 (data offset) differs by design; compare tags without it
            strip = lambda bk: [([(t, v) for t, v in x['tags'] if t != 0x88], x['samples'],
                                 x['data']) for x in bk['clips']]
            same = strip(a) == strip(b)
            n += 1
            if not same:
                bad += 1; print('DIFF', name)
    print('%d banks, %d differ' % (n, bad))
    return 1 if bad else 0


def cmd_codec_test(path):
    bank = read_bank(path)
    worst_snr, exact = float('inf'), 0
    import math
    for i, c in enumerate(bank['clips']):
        mine = adpcm_decode(c['data'], c['samples'])
        raw = subprocess.run(['ffmpeg', '-v', 'error', '-f', 'wav', '-i', '-', '-f', 's16le', '-'],
                             input=_xbox_wav(c['data'], c['rate']), check=True,
                             capture_output=True).stdout
        ff = list(struct.unpack('<%dh' % (len(raw) // 2), raw))[:c['samples']]
        exact += mine == ff
        again = adpcm_decode(adpcm_encode(mine), len(mine))
        sig = sum(x * x for x in mine) or 1
        err = sum((x - y) ** 2 for x, y in zip(mine, again)) or 1
        snr = 10 * math.log10(sig / err)
        worst_snr = min(worst_snr, snr)
        print('slot %3d  %6d samples  ffmpeg-exact %s  re-encode SNR %.1f dB' % (
            i, c['samples'], mine == ff, snr))
    print('%d/%d decode sample-exact vs ffmpeg; worst re-encode SNR %.1f dB' % (
        exact, len(bank['clips']), worst_snr))
    return 0 if exact == len(bank['clips']) else 1


def main(argv):
    if len(argv) < 2:
        print(__doc__); return 2
    cmd, args = argv[1], argv[2:]
    if cmd == 'roundtrip':
        return cmd_roundtrip(args)
    if cmd == 'codec-test':
        return cmd_codec_test(args[0])
    if cmd == 'extract':
        bank = read_bank(args[0])
        os.makedirs(args[1], exist_ok=True)
        for i, c in enumerate(bank['clips']):
            write_wav(os.path.join(args[1], '%d.wav' % i), adpcm_decode(c['data'], c['samples']),
                      c['rate'])
        return 0
    if cmd == 'replace':
        bank = read_bank(args[0])
        for spec in args[2:]:
            slot, src = spec.split('=', 1)
            slot = int(slot)
            bank['clips'][slot] = clip_from_pcm(bank['clips'][slot], load_audio(src))
            print('slot %d <- %s (%.2f s)' % (slot, src, bank['clips'][slot]['samples'] / RATE))
        write_bank(args[1], bank)
        return 0
    print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
