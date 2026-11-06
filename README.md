# HP2 for Mac

*Harry Potter and the Chamber of Secrets* (original Xbox, Eurocom 2002), running natively on macOS (Apple silicon)
by static recompilation of `default.xbe` (x86 → C++ via x86recomp) with the Xbox kernel, D3D8, DirectSound and
XInput replaced by native code (Metal, SDL3).
You need your own copy of the game: this repository and the DMG contain **no game data and no game code**
(only our HD texture packs). [Українською нижче.](#українською)

## Install
Needs **macOS 26 or later** on Apple silicon, a gamepad and your own disc image.
Download `HP2.dmg` from [Releases](../../releases/latest).

1. Open `HP2.dmg` and drag **HP2** to Applications (or anywhere you like).
2. The app is not signed by Apple: the first time, **right-click HP2 → Open → Open** (Gatekeeper).
3. Pick your disc image (`.iso`) when asked. HP2 imports it to `~/Library/Application Support/HP2/data`,
   then builds the game for your Mac with Apple's Command Line Tools (macOS offers to install them if
   they are missing). This takes a few minutes, once.
4. The launcher opens: choose the **language** — English (default), Українська, or Українська — текст
   (Ukrainian text, English voices) — then **Play**. Your choice is remembered. The DMG has no Ukrainian
   voices: both Ukrainian options speak English.

HD textures for the interface and the world come with the app and are on by default.
60 fps, 16:10 widescreen, 4× render scale, picture modes, modern camera; hold A for 4 s to skip a cutscene.

Controls: **gamepad only** (Xbox, DualSense, DualShock 4, Switch Pro, 8BitDo — anything SDL3 knows); no keyboard.
Discs: the US (NTSC) disc is tested; EU (PAL): untested.

## Building from source
```
tools/bootstrap.sh                                   # third_party (x86recomp, Zydis, SoftFloat)
lift/lift.sh --detach --pin                          # lift your default.xbe (heavy)
cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build out --target hp2 -j3
tools/install_app.sh out                             # ~/Desktop/HP2.app (your data, player files in .cache/player)
tools/make_dmg.sh out out/dmg/HP2.dmg                # the DMG: no game data, no game code
```
The Ukrainian translation is `mods/loc/uk/*.tsv` (id, a CRC of the English line, the translation); the English
text itself is read from your disc at import, it is not in this repository.

Our code: MIT (`LICENSE`). The app ships third-party code under its own licences (x86recomp, Zydis/Zycore — MIT;
Berkeley SoftFloat — BSD-3; SDL3 — zlib; AMD FSR1 — MIT): `THIRD_PARTY_NOTICES.txt` inside the app and the DMG.
Personal use only. Not affiliated with Warner Bros., EA or Microsoft. Rights holders: open an issue and we take
the material down within 24 hours.

---

## Українською

Нативний порт *Harry Potter and the Chamber of Secrets* (Xbox) для macOS на Apple Silicon. Потрібна власна копія
гри: ні репозиторій, ні DMG **не містять ні даних, ні коду гри** (лише наші HD-текстури).

Потрібні **macOS 26 або новіша** на Apple Silicon, геймпад і образ власного диска. `HP2.dmg` — у
[Releases](../../releases/latest).

1. Відкрийте `HP2.dmg` і перетягніть **HP2** у «Applications».
2. Програма без підпису Apple: перший раз — **правий клік на HP2 → «Відкрити» → «Відкрити»** (Gatekeeper).
3. Оберіть образ свого диска (`.iso`). HP2 імпортує його в `~/Library/Application Support/HP2/data` і збирає гру
   на вашому Маку з Command Line Tools від Apple (якщо їх нема, macOS запропонує встановити). Кілька хвилин, один раз.
4. У лаунчері оберіть **мову** — English (за замовчуванням), Українська або «Українська — текст» (текст українською,
   голоси англійською) — і натисніть **Грати**. Вибір запам'ятовується. Української озвучки в DMG нема.

Керування — **лише геймпад**. Диск US (NTSC) перевірено; EU (PAL) — не перевірено.
Наш код — MIT (`LICENSE`); ліцензії сторонніх частин — у `THIRD_PARTY_NOTICES.txt` у застосунку й DMG.
Лише для особистого використання. Проєкт не пов'язаний з Warner Bros., EA чи Microsoft. Правовласникам: відкрийте
issue — приберемо матеріал протягом 24 годин.
