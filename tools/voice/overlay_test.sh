#!/bin/bash
# Proof that a speech-bank overlay is played: every clip of Speech_Burrows and
# Speech_Always becomes a 1 kHz tone of the same length (mods dir in the run dir),
# then one silent background new-game run into the Burrow (HP2_LANG=uk) writes
# the mix to WAV and tone_check.py counts 100 ms windows dominated by 1 kHz.
#   tools/voice/overlay_test.sh [hp2 binary]   (default: main checkout out/)
# PASS = tone windows > 0 (a run without the overlay has none: 0 of 50 s).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${1:-$("$(dirname "$0")/../../tools/main_checkout.sh")/out/src/loader/hp2}"
RUN="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/voice_overlay-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN/home" "$RUN/mods/_UK"
python3 -c "
import math, sys; sys.path.insert(0, '$HERE'); import sdt
U = '$("$(dirname "$0")/../../tools/main_checkout.sh")/game/extracted/_UK/'
for b in ('Speech_Burrows', 'Speech_Always'):
    bank = sdt.read_bank(U + b + '.sdt')
    for i, c in enumerate(bank['clips']):
        tone = [int(12000 * math.sin(2 * math.pi * 1000 * k / 22050)) for k in range(c['samples'])]
        bank['clips'][i] = sdt.clip_from_pcm(c, tone)
    sdt.write_bank('$RUN/mods/_UK/' + b + '.sdt', bank)
"
cat > "$RUN/pad.txt" <<'P'
640 press A
760 press DOWN
780 press DOWN
800 press DOWN
830 press A
900 press A
P
FR=3000
(
  ulimit -c 0
  export HOME="$RUN/home" HP2_LOG_DIR="$RUN" HP2_KTRACE=0 SDL_AUDIO_DRIVER=dummy \
    SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_SKIP_MOVIES=1 \
    HP2_DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}" HP2_LANG=uk HP2_MODS="$RUN/mods" \
    HP2_PAD_SCRIPT="$RUN/pad.txt" HP2_SHOT_FRAMES="$FR" HP2_EXIT_AFTER="$FR" \
    HP2_AUDIO_WAV="$RUN/mix.wav"
  exec "$BIN" > "$RUN/stdout.log" 2> "$RUN/stderr.log"
) &
P=$!
for _ in $(seq 400); do kill -0 $P 2>/dev/null || break; sleep 1; done
kill -9 $P 2>/dev/null; wait $P; echo "exit $? ($RUN)"
python3 "$HERE/tone_check.py" "$RUN/mix.wav" | tee "$RUN/result.txt"
grep -q "windows: [1-9]" "$RUN/result.txt" && echo PASS || { echo FAIL; exit 1; }
