#!/usr/bin/env python3
"""Listen to a cutscene in Ukrainian at the game's own timing, without a game run.

Reads the subtitle records of CS_<name>.geo (speech_map.geo_pairs: text id + the
matching speech record, 1/60 s ticks), takes each line's generated audio
(.cache/HP2Cache/voice/uk/audio/<speaker>_<text_id>.mp3) with the voices.tsv pitch and the
English clip's pauses (timing.retime) and the banks' loudness target, and mixes them
at their start times. Lines
without Ukrainian audio play the English clip, so gaps are audible.

  preview_scene.py CS_LVDA_01e OUT.wav [--en]   (--en: the English original instead)
"""
import csv, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_voice, sdt, speech_map, timing  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))  # tools/
import hp2paths  # noqa: E402

DATA = speech_map.DATA
WORK = os.path.join(hp2paths.CACHE, 'voice/uk')


def scene_lines(geo):
    """-> [(tick, text_id, sample hash)] in time order (same matching as speech_map)."""
    banks, hash_of = speech_map.read_fxmap(os.path.join(DATA, 'EASound/FXMap.dbg'))
    text = speech_map.read_text(os.path.join(os.path.dirname(os.path.dirname(HERE)), 'mods/loc/uk'))
    sv = {h for nm, h in hash_of.items() if nm.startswith('SV_')}
    out = [(t, tid, h) for tid, h, _src, t in speech_map.geo_pairs(geo, sv, set(text), with_time=True)]
    return sorted(out), banks, hash_of


def main(argv):
    if len(argv) < 3:
        print(__doc__); return 2
    name, out_wav, english = argv[1], argv[2], '--en' in argv
    lines, banks, hash_of = scene_lines(os.path.join(DATA, name + '.geo'))
    if not lines:
        print('%s: no subtitled speech' % name)
        return 1
    name_of = {h: nm for nm, h in hash_of.items()}
    where = {}  # wav name -> (bank file, slot)
    for bank, slots in banks.items():
        for slot, (_sid, wav) in slots.items():
            where.setdefault(wav, (bank, slot))
    pitch = {r['speaker']: float(r.get('pitch') or 0)
             for r in csv.DictReader(open(os.path.join(HERE, 'voices.tsv'), encoding='utf-8'),
                                     delimiter='\t')}
    files = {f.upper(): os.path.join(DATA, '_UK', f) for f in os.listdir(os.path.join(DATA, '_UK'))}
    target = build_voice.english_level(DATA, os.path.join(WORK, 'en_level.txt'))
    levels = build_voice.speaker_levels(DATA, banks, target)
    cache, parts, uk_n = {}, [], 0
    for tick, tid, h in lines:
        wav = name_of[h]
        bank, slot = where[wav]
        path = files[bank + '.SDT']
        if path not in cache:
            cache[path] = sdt.read_bank(path)
        c = cache[path]['clips'][slot]
        en = sdt.adpcm_decode(c['data'], c['samples'])
        spk = wav.split('_')[1]
        mp3 = os.path.join(os.environ.get('VOICE_AUDIO', os.path.join(WORK, 'audio')),
                           '%s_%d.mp3' % (spk, tid))
        if not english and os.path.exists(mp3):
            pcm = timing.retime(sdt.load_audio(mp3, pitch.get(spk, 0.0)), en)
            if len(pcm) > len(en):  # as the build: never longer than the English line
                pcm = build_voice.tempo(pcm, len(en))
            lv = levels.get(spk, target)
            g = lv / (build_voice.speech_rms(pcm) or lv)  # same level as the banks
            parts.append((tick, [build_voice.limit(x * g) for x in pcm]))
            uk_n += 1
        else:
            parts.append((tick, en))
    sdt.write_wav(out_wav, timing.scene(parts))
    print('%s: %d lines, %d Ukrainian, %.1f s' % (name, len(lines), uk_n,
                                                  max(t for t, _ in parts) / 60 + 3))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
