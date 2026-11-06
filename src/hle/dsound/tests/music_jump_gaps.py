#!/usr/bin/env python3
# music_jump_gaps.py <mix.wav> <stderr.log> [max_ms]: the hole at each music
# section jump. Jumps are the "immediate" queues of a new section in the log
# (HP2_DS_TRACE_CLOSE: "QueueSegment(s, -1, ..., <offset > 0>, ...) ... mix <frame>");
# around each (-300..+300 ms) the longest run of 1 ms windows quieter than 10 %
# of the music's level is the hole (level: the lower of the medians of the
# second before and of 100..400 ms after, so a quietly starting section is
# not counted as a hole). FAIL if any hole
# exceeds max_ms (default 5) or fewer than 2 jumps were seen.
import array, math, re, sys
wav, log = sys.argv[1], sys.argv[2]
max_ms = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
a = array.array('h', open(wav, 'rb').read()[44:])
n = len(a) // 2
jumps = [int(m.group(1)) for m in re.finditer(
    r'QueueSegment\(\d+, -1, \w+ "[^"]*", [1-9]\d*, \d+\).* mix (\d+)', open(log, errors='replace').read())]
def rms(f):  # 1 ms window at frame f, both channels
    s = sum(a[k] * a[k] for k in range(2 * f, 2 * f + 96))
    return math.sqrt(s / 96)
worst, seen = 0.0, 0
for j in jumps:
    if j - 48000 < 0 or j + 48000 > n: continue
    seen += 1
    before = sorted(rms(f) for f in range(j - 48000, j, 480))[50]
    after = sorted(rms(f) for f in range(j + 4800, j + 19200, 480))[15]
    ref = min(before, after)
    run = best = 0
    for f in range(j - 48 * 300, j + 48 * 300, 48):
        run = run + 1 if rms(f) < 0.1 * ref else 0
        best = max(best, run)
    worst = max(worst, best)
    print(f"jump at {j / 48000:7.2f} s: hole {best} ms (level {ref:.0f})")
ok = seen >= 2 and worst <= max_ms
print(f"{seen} jumps, worst hole {worst:.0f} ms (limit {max_ms:.0f}):", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
