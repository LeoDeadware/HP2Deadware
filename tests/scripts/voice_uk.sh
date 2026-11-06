#!/bin/bash
# Ukrainian voice-over in the three language slots, as the installed app runs it
# (mods = the loc overlay + the voice banks), plus the static bank check:
#   0. tools/voice/check_banks.py: every voice bank keeps the original layout and no
#      clip is longer than the English line it replaces (the game times lines by it);
#   1. HP2_LANG=uk      -> Speech_Burrows.sdt is served from the voice banks (UA voice);
#   2. HP2_LANG=uk_text -> Speech_Burrows.sdt is the original from the disc (English
#      speech under Ukrainian text; the voice banks must not leak into _UKT);
#   3. HP2_LANG=en      -> Speech_Burrows.sdt is the disc's _USA bank;
#   each run: silent new game into the Burrow, 3000 frames, exit 0, speech in the mix.
#   4. (--quidditch) the Quidditch stadium (levels_smoke row 3) in the UA slot: no crash,
#      the speech banks it opens are served from the voice banks. The direct map load
#      starts no match, so Speech_Quidditch_Commentary itself is not opened there.
#   tests/scripts/voice_uk.sh [build_dir] [--quidditch]     VOICE=<dir> overrides the banks;
#   LOC_ROOT=<checkout> builds the text overlay with that checkout's tools/loc (the uk_text
#   slot needs an overlay with _UKT/text.bin, else the kernel falls back to uk).

set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/out"; QUID=0
for a in "$@"; do case "$a" in --quidditch) QUID=1 ;; *) BUILD="$a" ;; esac; done
BIN="$BUILD/src/loader/hp2"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
DATA="${HP2_DATA:-$("$(dirname "$0")/../../tools/main_checkout.sh")}"
VOICE="${VOICE:-${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/voice/uk/mods}"
[[ -d "$VOICE/_UK" ]] || { echo "FAIL: no voice banks in $VOICE/_UK (tools/voice/build_voice.py)"; exit 2; }
RUN="${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}/runs/voice_uk-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN"
ulimit -c 0
FAILS=0
fail() { echo "  FAIL: $*"; FAILS=$((FAILS + 1)); }

echo "0. voice banks ($VOICE)"
python3 "$ROOT/tools/voice/check_banks.py" "$VOICE" | sed 's/^/  /'
[[ ${PIPESTATUS[0]} == 0 ]] || fail "check_banks"

LOC_ROOT="${LOC_ROOT:-$ROOT}"
python3 "$LOC_ROOT/tools/loc/build_mod.py" uk --data "$DATA" --out "$RUN/overlay" > "$RUN/overlay.log" 2>&1 \
  || { echo "FAIL: loc overlay build"; exit 1; }
UKT=0; [[ -f "$RUN/overlay/_UKT/text.bin" || -f "$RUN/overlay/_UKT/TEXT.BIN" ]] && UKT=1
MODS="$RUN/overlay:$VOICE"

cat > "$RUN/pad.txt" <<'EOF'
640 press A
760 press DOWN
780 press DOWN
800 press DOWN
830 press A
900 press A
EOF

slot() {  # slot <n> <lang>
  local d="$RUN/$1_$2"
  mkdir -p "$d/home"
  ( export HOME="$d/home" HP2_LOG_DIR="$d" HP2_KTRACE=0 HP2_KTRACE_KERNEL=1 SDL_AUDIO_DRIVER=dummy \
      SDL_VIDEO_DRIVER=dummy HP2_BACKGROUND=1 HP2_SKIP_MOVIES=1 HP2_DATA="$DATA" HP2_LANG="$2" \
      HP2_MODS="$MODS" HP2_PAD_SCRIPT="$RUN/pad.txt" HP2_EXIT_AFTER=3000 HP2_AUDIO_WAV="$d/mix.wav"
    exec "$BIN" > "$d/stdout.log" 2> "$d/stderr.log" ) &
  local pid=$!
  for _ in $(seq 1 600); do kill -0 $pid 2>/dev/null || break; sleep 0.5; done
  kill -0 $pid 2>/dev/null && { kill -9 $pid; fail "$2: timeout"; }
  wait $pid; local code=$?
  [[ $code == 0 ]] || fail "$2: exit $code"
  # host file behind the game's Speech_Burrows.sdt (kernel open trace)
  HOST=$(grep -i 'speech_burrows\.sdt' "$d/stderr.log" | grep -i ' -> 00000000 ' | head -1 | sed 's/.* -> 00000000 //')
  echo "$1. $2: exit $code, Speech_Burrows -> ${HOST:-<not opened>}"
  python3 - "$d/mix.wav" <<'PY' || fail "$2: no speech-level audio after 30 s"
import wave, struct, math, sys
w = wave.open(sys.argv[1]); r = w.getframerate(); n = w.getnframes()
a = struct.unpack('<%dh' % (n * 2), w.readframes(n))
loud = sum(1 for s in range(30 * r, n - r // 10, r // 10)
           if math.sqrt(sum(v * v for v in a[2 * s:2 * (s + r // 10):2]) / (r // 10)) > 1500)
print('   %.1f s of the mix above speech level after 30 s' % (loud / 10))
sys.exit(0 if loud >= 10 else 1)
PY
}

slot 1 uk
[[ "$HOST" == "$VOICE/_UK/Speech_Burrows.sdt" ]] || fail "uk: not the Ukrainian voice bank"
if [[ $UKT == 1 ]]; then
  slot 2 uk_text
  [[ -n "$HOST" && "$HOST" != "$VOICE"* ]] || fail "uk_text: voice banks leaked (or the bank was not opened)"
else
  echo "2. uk_text: SKIP (overlay from $LOC_ROOT/tools/loc has no _UKT; set LOC_ROOT)"
fi
slot 3 en
[[ "$HOST" == *"_USA/Speech_Burrows.sdt" || "$HOST" == *"_USA/SPEECH_BURROWS.SDT" ]] || fail "en: not the _USA bank"

if [[ $QUID == 1 ]]; then
  echo "4. Quidditch stadium in the UA slot (levels_smoke row 3)"
  HP2_LANG=uk HP2_MODS="$MODS" "$ROOT/tests/scripts/levels_smoke.sh" "$BUILD" 3 > "$RUN/quidditch.log" 2>&1
  q=$?
  tail -3 "$RUN/quidditch.log" | sed 's/^/  /'
  [[ $q == 0 ]] || fail "quidditch: levels_smoke exit $q"
  qd=$(ls -d "${HP2_CACHE:-$("$(dirname "$0")/../../tools/main_checkout.sh")/.cache/HP2Cache}"/runs/levels_smoke-*/03_* 2>/dev/null | tail -1)
  opened=$(grep -i 'speech_.*\.sdt.* -> 00000000 ' "$qd/stderr.log" 2>/dev/null | sed 's/.* -> 00000000 //' | sort -u)
  echo "$opened" | sed 's/^/  opened /'
  if [[ -n "$opened" ]] && ! echo "$opened" | grep -qv "^$VOICE/"; then
    echo "  speech banks served from the voice banks"
  else fail "quidditch: speech banks not (all) from the voice banks"; fi
fi

echo "$RUN"
[[ $FAILS == 0 ]] && echo PASS || { echo "FAIL ($FAILS)"; exit 1; }
