#!/usr/bin/env python3
"""Levels of a 16-bit WAV (the mixer's HP2_AUDIO_WAV output): duration, peak,
RMS, clipped samples, non-silent seconds, and a per-second RMS strip.

    src/hle/dsound/wav_stats.py mix.wav
"""
import array
import math
import sys
import wave

w = wave.open(sys.argv[1], 'rb')
ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
peak, sumsq, clipped, loud = 0, 0.0, 0, 0
strip = []
while True:
    raw = w.readframes(rate)
    if not raw:
        break
    a = array.array('h', raw)
    s = sum(x * x for x in a)
    sumsq += s
    p = max((abs(x) for x in a), default=0)
    peak = max(peak, p)
    clipped += sum(1 for x in a if x in (32767, -32768, -32767))
    loud += sum(1 for i in range(0, len(a), ch) if max(abs(a[i + c]) for c in range(ch)) > 33) // 1
    strip.append(math.sqrt(s / max(1, len(a))) / 32768)
print(f"wav: {n / rate:.1f} s, {ch} ch, {rate} Hz, peak {peak / 32768:.3f}, "
      f"rms {math.sqrt(sumsq / max(1, n * ch)) / 32768:.4f}, clipped {clipped}, "
      f"non-silent {loud / rate:.1f} s")
bars = ' .:-=+*#%@'
print('rms/s: ' + ''.join(bars[min(9, int(r * 40))] for r in strip))
