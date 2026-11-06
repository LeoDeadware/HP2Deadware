#!/usr/bin/env python3
"""Give a generated line the pause pattern of the English clip (no credits).

Speech/silence is found with a 10 ms RMS envelope (threshold relative to the clip's
peak, pauses >= MIN_PAUSE). The Ukrainian clip gets the English lead-in and tail
silence, and its N longest inner pauses (N = number of English inner pauses) take the
English pause lengths in order; its other inner pauses are kept.

  timing.py retime UK_AUDIO EN_AUDIO OUT.wav
  timing.py scene OUT.wav TICK:AUDIO [TICK:AUDIO ...]   mix clips at cutscene times
                                                        (1/60 s ticks, as CS_*.geo tracks)
"""
import math, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sdt  # noqa: E402

RATE = sdt.RATE
WIN = RATE // 100          # 10 ms
MIN_PAUSE = 0.15           # s
REL_DB = -35.0             # silence = 35 dB under the clip's loudest 10 ms


def segments(pcm):
    """-> list of (start, end) sample ranges of speech."""
    env = [math.sqrt(sum(x * x for x in pcm[i:i + WIN]) / WIN) for i in range(0, len(pcm), WIN)]
    if not env or max(env) == 0:
        return []
    thr = max(env) * 10 ** (REL_DB / 20)
    on = [e > thr for e in env]
    segs, i = [], 0
    while i < len(on):
        if on[i]:
            j = i
            while j < len(on) and on[j]:
                j += 1
            segs.append([i, j]); i = j
        else:
            i += 1
    gap = int(MIN_PAUSE * 100)
    merged = []
    for s in segs:  # join speech separated by less than MIN_PAUSE
        if merged and s[0] - merged[-1][1] < gap:
            merged[-1][1] = s[1]
        else:
            merged.append(s)
    return [(a * WIN, min(len(pcm), b * WIN)) for a, b in merged]


MIN_LEAD, MIN_TAIL, MIN_INNER = 0.03, 0.05, 0.12   # s: the tightest still natural


def retime(uk, en, n=None):
    """n: room in samples (default len(en)). When the line is longer than that, the
    lead-in, the tail and then the inner pauses shrink (to MIN_*) before any tempo."""
    su, se = segments(uk), segments(en)
    if not su or not se:
        return uk
    n = n or len(en)
    lead, tail = se[0][0], len(en) - se[-1][1]
    en_p = [se[k + 1][0] - se[k][1] for k in range(len(se) - 1)]
    uk_p = [su[k + 1][0] - su[k][1] for k in range(len(su) - 1)]
    new_p = list(uk_p)
    if en_p and uk_p:
        n = min(len(en_p), len(uk_p))
        longest = sorted(sorted(range(len(uk_p)), key=lambda k: -uk_p[k])[:n])
        for k, e in zip(longest, en_p[:n]) if n == len(en_p) else zip(longest, sorted(en_p, reverse=True)[:n]):
            new_p[k] = e
    over = lead + tail + sum(new_p) + sum(b - a for a, b in su) - n
    if over > 0:
        cut = min(over, max(0, tail - int(MIN_TAIL * RATE))); tail -= cut; over -= cut
        cut = min(over, max(0, lead - int(MIN_LEAD * RATE))); lead -= cut; over -= cut
        room = sum(max(0, q - int(MIN_INNER * RATE)) for q in new_p)
        if over > 0 and room:
            k = min(1.0, over / room)
            new_p = [q - int(max(0, q - int(MIN_INNER * RATE)) * k) for q in new_p]
    out = [0] * lead
    for k, (a, b) in enumerate(su):
        out += uk[a:b]
        if k < len(new_p):
            out += [0] * new_p[k]
    return out + [0] * tail


def scene(parts):
    """parts: [(tick, pcm)] -> one pcm with every clip starting at tick / 60 s."""
    end = max(int(t / 60 * RATE) + len(p) for t, p in parts)
    mix = [0] * (end + RATE // 2)
    for t, p in parts:
        o = int(t / 60 * RATE)
        for i, x in enumerate(p):
            mix[o + i] += x
    return [max(-32768, min(32767, x)) for x in mix]


def main(argv):
    if len(argv) >= 5 and argv[1] == 'retime':
        sdt.write_wav(argv[4], retime(sdt.load_audio(argv[2]), sdt.load_audio(argv[3])))
        return 0
    if len(argv) >= 4 and argv[1] == 'scene':
        parts = [(float(s.split(':', 1)[0]), sdt.load_audio(s.split(':', 1)[1])) for s in argv[3:]]
        sdt.write_wav(argv[2], scene(parts))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
