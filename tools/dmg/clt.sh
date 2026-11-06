#!/bin/bash
# Apple Command Line Tools for the DMG installer (coordinator 2026-11-06): the disc import needs
# python3 and the game build needs clang, both from CLT. Present -> exit 0. Missing ->
# `xcode-select --install` (the system dialog), then wait until clang is there (HP2_CLT_WAIT
# seconds, default 3600); still missing -> exit 1. Progress lines "N text" go to $1.
PROG="${1:-/dev/null}"
UK=0; [[ "${HP2_UI_LANG:-en}" == uk ]] && UK=1   # interface language: English unless uk
clt_ok() { xcode-select -p >/dev/null 2>&1 && xcrun -f clang++ >/dev/null 2>&1; }
clt_ok && exit 0
(( UK )) && echo "1 Встановіть Command Line Tools від Apple у системному вікні, гра чекає" >> "$PROG" ||
  echo "1 Install Apple Command Line Tools in the system dialog; the game waits" >> "$PROG"
echo "clt: Command Line Tools missing: xcode-select --install"
xcode-select --install >/dev/null 2>&1
MSG="The game needs Apple Command Line Tools: click Install in the system dialog. HP2 continues by itself."
(( UK )) && MSG="Для гри потрібні Command Line Tools від Apple: натисніть «Встановити» у системному вікні й дочекайтеся кінця. HP2 продовжить сам."
[[ -n "${HP2_NO_UI:-}" ]] || /usr/bin/osascript -e "display dialog \"$MSG\" buttons {\"OK\"} with title \"HP2\"" >/dev/null 2>&1 &
waited=0
until clt_ok; do
  if (( waited >= ${HP2_CLT_WAIT:-3600} )); then
    E="ERROR: Command Line Tools are not installed"; (( UK )) && E="ПОМИЛКА: Command Line Tools не встановлено"
    echo "100 $E (xcode-select --install)" >> "$PROG"
    echo "clt: still missing after ${waited}s"
    exit 1
  fi
  sleep 5; waited=$((waited + 5))
done
echo "clt: installed after ${waited}s"
