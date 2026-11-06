#!/usr/bin/env python3
"""Ukrainian voice-over through the ElevenLabs HTTP API (stdlib + ffmpeg only).

API key: $ELEVENLABS_API_KEY, else the file ~/.config/elevenlabs/api_key (one line,
chmod 600). The key is only sent as the xi-api-key header and never printed.

Free (no credits):
  tts.py credits                       characters used / left this month
  tts.py voices                        voices in your account (id, name, labels)
  tts.py library [--lang uk] [--q X]   shared voice library search
  tts.py worklist                      write the line list (recon map -> lines.tsv)
  tts.py plan [filters]                what `gen` would generate and its characters

Spends credits (only with --yes; prints the plan and stops otherwise):
  tts.py gen [filters] --yes

Filters: --speaker HRY,RON  --bank Speech_Borgin  --limit N  --model ID
Files:
  tools/voice/voices.tsv               speaker -> voice_id, model, stability, notes
                                       (edit by hand; a speaker without a voice is skipped)
  .cache/HP2Cache/voice/uk/lines.tsv        one row per (speaker, text): key, speaker, text_id,
                                       sec, wavs, uk; dub_uk (shorter spoken text) and tags
                                       (v3/v4 audio tags) are edits kept across worklist runs
  .cache/HP2Cache/voice/uk/audio/<key>.mp3  generated audio; existing files are never re-bought
"""
import argparse, csv, json, os, re, sys, urllib.error, urllib.parse, urllib.request
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))  # tools/
import hp2paths  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MAP = os.path.join(ROOT, 'recon/notes/voice_over_map.tsv')
VOICES = os.path.join(HERE, 'voices.tsv')
WORK = os.path.join(hp2paths.CACHE, 'voice/uk')
LINES = os.path.join(WORK, 'lines.tsv')
AUDIO = os.path.join(WORK, 'audio')
API = 'https://api.elevenlabs.io'
CPS = 14.5  # natural uk pace; above it a line gets a faster speed setting (max 1.2)
DEFAULT_MODEL = 'eleven_multilingual_v2'


def api_key():
    k = os.environ.get('ELEVENLABS_API_KEY', '').strip()
    if not k:
        p = os.path.expanduser('~/.config/elevenlabs/api_key')
        if os.path.exists(p):
            k = open(p).read().strip()
    if not k:
        sys.exit('no API key: set ELEVENLABS_API_KEY or write it to ~/.config/elevenlabs/api_key')
    return k


def call(method, path, body=None, query=None, raw=False):
    url = API + path + ('?' + urllib.parse.urlencode(query) if query else '')
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers={
        'xi-api-key': api_key(), 'Content-Type': 'application/json',
        'Accept': 'audio/mpeg' if raw else 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            out = r.read()
    except urllib.error.HTTPError as e:
        sys.exit('HTTP %d %s: %s' % (e.code, path, e.read()[:400].decode('utf-8', 'replace')))
    except (urllib.error.URLError, OSError) as e:  # DNS / connection drop: retryable
        sys.exit('HTTP 5xx-network %s: %s' % (path, e))
    return out if raw else json.loads(out)


def load_voices():
    v = {}
    if os.path.exists(VOICES):
        for r in csv.DictReader(open(VOICES, encoding='utf-8'), delimiter='\t'):
            if r.get('voice_id', '').strip():
                v[r['speaker']] = r
    return v


TAGS = os.path.join(HERE, 'tags.tsv')


def rule_tags(sp, en, wavs=''):
    """Tags for lines without a hand-written one in tags.tsv (patrol barks, races,
    prices, card swaps, card descriptions); first match wins."""
    e = en.lower()
    rules = [
        (('cough', 'splutter'), '[coughing, spluttering]'),
        (('locomotor mortis',), '[casting a spell, shouting]'),
        (('hearing things',), '[muttering, puzzled]'),
        (('lost him', 'where did he go', 'given me the slip', 'where have you gone'), '[frustrated]'),
        (('hey', "there's someone", 'has anyone', 'did anyone', "i'll find you"), '[alert, calling out]'),
        (('what was that', 'who\u2019s there', "who's there"), '[startled]'),
        (('points from', 'restricted area', 'you know the rules'), '[stern, officious]'),
        (('get back', 'go back', 'clear off', 'back to the dungeons'), '[stern]'),
        (('it\u2019ll cost', "it'll cost"), '[matter-of-fact, bargaining]'),
        (('i\u2019ll give you card', "i'll give you card"), '[eager, bargaining]'),
        (('bad luck', 'you lost', 'didn\u2019t beat', 'didn\u2019t break', 'wasn\u2019t beaten',
          'not bad', 'no wizard card'), '[sympathetic]' if 'harry' in e else '[smug]'),
        (('well done', 'you won', 'you beat', 'managed to beat', 'for winning', 'for breaking',
          'have a wizard card', 'here\u2019s a'), '[friendly, congratulating]'),
        (('race', 'record', 'have a go', 'another go', 'try again', 'fancy'), '[friendly, challenging]'),
    ]
    if e.strip() in ('ow!', 'oof!'):
        return '[pained]'
    if e.strip() in ('huh?',):
        return '[startled]'
    for words, tag in rules:
        if any(w in e for w in words):
            return tag
    if '_CFROG_' in wavs:
        return '[reading aloud, impressed]'  # Chocolate Frog cards
    if sp == 'HRY' and any(s in wavs for s in ('_AUGDF_', '_AUGDA_', '_AUGPU_', '_AUGRW_')):
        return '[thinking aloud, to himself]'  # Harry's hints to the player
    return '[excited]' if e.endswith('!') else '[curious]' if e.endswith('?') else ''


STRESS = os.path.join(HERE, 'stress.tsv')


def stress_table(which=('mark',)):
    """form -> stressed form (U+0301) for glossary rows whose status is in `which`."""
    t = {}
    if os.path.exists(STRESS):
        for r in csv.DictReader(open(STRESS, encoding='utf-8'), delimiter='\t'):
            if (r.get('status') or '') in which:
                t[r['form']] = r['stressed']
    return t


def apply_stress(text, table):
    """Put the glossary's stress mark into every listed word form, keeping its case."""
    if not table:
        return text

    def fix(m):
        w = m.group(0)
        st = table.get(w.lower())
        if not st or '́' not in st:
            return w
        pos = st.index('́')  # the mark follows the stressed vowel; case comes from w
        return w[:pos] + '́' + w[pos:]
    return re.sub(r"[А-ЯҐЄІЇа-яґєії’']+", fix, text)  # hyphen splits: Майже-Безголо́вого-Ніка


JOINS = os.path.join(HERE, 'joins.tsv')


def join_names(text):
    """Multi-word names (joins.tsv, any case form) are written with hyphens so the
    voice says them as one word, without a pause (listening test: no pause, -0.4 s)."""
    if not os.path.exists(JOINS):
        return text
    for r in csv.DictReader(open(JOINS, encoding='utf-8'), delimiter='\t'):
        text = re.sub(r['pattern'], lambda m: re.sub(r'\s+', '-', m.group(0)), text)
    return text


def with_core(tags, core):
    """Add the speaker's book character (voices.tsv core_tag) unless the line's tags
    already say it."""
    have = tags.strip('[] ')
    add = [w.strip() for w in core.split(',') if w.strip() and w.strip() not in have]
    parts = [x for x in [', '.join(add), have] if x]
    return '[%s]' % ', '.join(parts) if parts else ''


def cmd_worklist(_a):
    """recon map -> one line per (speaker, text_id); dub_uk only from dub.tsv."""
    old = {}
    if os.path.exists(LINES):
        for r in csv.DictReader(open(LINES, encoding='utf-8'), delimiter='\t'):
            old[r['key']] = r
    lines = {}
    for r in csv.DictReader(open(MAP, encoding='utf-8'), delimiter='\t'):
        if not r['text_ids'] or not r['uk'] or r['dup_of']:
            continue
        tid = r['text_ids'].split(',')[0]
        key = '%s_%s' % (r['speaker'], tid)
        e = lines.setdefault(key, dict(key=key, speaker=r['speaker'], text_id=tid, bank=r['bank'],
                                       sec=r['sec'], wavs=[], en=r['en'], uk=r['uk']))
        if r['wav'] not in e['wavs']:
            e['wavs'].append(r['wav'])
        e['sec'] = '%.3f' % min(float(e['sec']), float(r['sec']))  # fit the shortest take
    # clips without a subtitle link: extra.py decisions (game string, rule or hand
    # translation); a game string reuses the SPEAKER_textid key (and its audio)
    bank_of = {r['wav']: r['bank'] for r in csv.DictReader(open(MAP, encoding='utf-8'), delimiter='\t')}
    extra = os.path.join(WORK, 'extra.tsv')
    comm, comm_tags = {}, {}
    cpath = os.path.join(HERE, 'commentary.tsv')
    if os.path.exists(cpath):
        for c in csv.DictReader(open(cpath, encoding='utf-8'), delimiter='\t', quoting=csv.QUOTE_NONE):
            comm[c['clip']] = c['tags']
    if os.path.exists(extra):
        for r in csv.DictReader(open(extra, encoding='utf-8'), delimiter='\t', quoting=csv.QUOTE_NONE):
            if not r['uk'] or r['how'] in ('nonverbal', 'skip', 'todo'):
                continue
            if r['how'] == 'subtitle' and r['text_id']:
                key, tid = '%s_%s' % (r['speaker'], r['text_id']), r['text_id']
            else:
                key, tid = '%s_x%s' % (r['speaker'], r['wav'].split('_', 2)[-1]), ''
            e = lines.setdefault(key, dict(key=key, speaker=r['speaker'], text_id=tid,
                                           bank=bank_of.get(r['wav'], ''), sec=r['sec'], wavs=[],
                                           en=r['en'], uk=r['uk']))
            if r['how'] == 'commentary':
                comm_tags[key] = comm.get(r['wav'].split('_', 2)[-1], '')
            if r['wav'] not in e['wavs']:
                e['wavs'].append(r['wav'])
            e['sec'] = '%.3f' % min(float(e['sec']), float(r['sec']))
    dub = {}  # committed shorter spoken texts (dub.tsv: key, text)
    dpath = os.path.join(HERE, 'dub.tsv')
    if os.path.exists(dpath):
        for r in csv.DictReader(open(dpath, encoding='utf-8'), delimiter='\t', quoting=csv.QUOTE_NONE):
            dub[r['key']] = r['text']
    hand = {}
    if os.path.exists(TAGS):
        for r in csv.DictReader(open(TAGS, encoding='utf-8'), delimiter='\t'):
            hand[r['key']] = r['tags']
    os.makedirs(WORK, exist_ok=True)
    cols = ['key', 'speaker', 'text_id', 'bank', 'sec', 'wavs', 'en', 'uk', 'dub_uk', 'tags']
    with open(LINES, 'w', encoding='utf-8', newline='') as f:
        w = csv.DictWriter(f, cols, delimiter='\t', lineterminator='\n')
        w.writeheader()
        import speech_map
        for k in sorted(lines, key=lambda k: (lines[k]['bank'], k)):
            e = dict(lines[k], wavs=','.join(lines[k]['wavs']))
            # what is spoken: no button glyph codes {U1710}, \n or emptied "( )" (user
            # report: the voice said something extra in their place)
            e['uk'] = speech_map.spoken(e['uk'])
            # dub.tsv is the only source: a key taken out of it speaks its subtitle again
            e['dub_uk'] = dub.get(k, '')
            e['tags'] = (hand.get(k) or comm_tags.get(k)
                         or rule_tags(e['speaker'], e['en'], e['wavs']))  # edits go to tags.tsv
            w.writerow(e)
    print('%d lines -> %s' % (len(lines), LINES))


def selected(a):
    if not os.path.exists(LINES):
        cmd_worklist(a)
    rows = list(csv.DictReader(open(LINES, encoding='utf-8'), delimiter='\t'))
    sp = set(a.speaker.split(',')) if a.speaker else None
    ks = set(open(a.keys).read().split()) if getattr(a, 'keys', None) else None
    bk = set(a.bank.split(',')) if a.bank else None
    out = [r for r in rows if (not sp or r['speaker'] in sp) and (not bk or r['bank'] in bk)
           and (not ks or r['key'] in ks)]
    return out[:a.limit] if a.limit else out


def plan(a):
    voices = load_voices()
    stress = stress_table()
    todo, skip_have, skip_voice = [], 0, set()
    for r in selected(a):
        if os.path.exists(os.path.join(AUDIO, r['key'] + '.mp3')) and not getattr(a, 'force', False):
            skip_have += 1; continue
        if r['speaker'] not in voices:
            skip_voice.add(r['speaker']); continue
        text = apply_stress(join_names(r['dub_uk'] or r['uk']), stress)
        need = len(text) / CPS / max(float(r['sec']), 0.3)
        model = a.model or voices[r['speaker']].get('model') or DEFAULT_MODEL
        if 'v3' in model or 'v4' in model:  # audio tags, e.g. [whispers]; they cost characters
            core, line = voices[r['speaker']].get('core_tag', ''), r.get('tags', '')
            if getattr(a, 'compact', False):  # cheaper: 2 words of character + 1 line emotion
                core = ', '.join(core.split(', ')[:2])
                line = '[%s]' % ', '.join(x.strip() for x in line.strip('[]').split(',')[:2]
                                          if x.strip()) if line else ''
            tags = with_core(line, core)
            text = (tags + ' ' + text).strip()
        todo.append(dict(r, text=text, speed=round(min(1.2, max(1.0, need)), 2),
                         voice=voices[r['speaker']]))
    chars = sum(len(t['text']) for t in todo)
    print('to generate: %d lines, %d characters (already generated: %d)' % (
        len(todo), chars, skip_have))
    if skip_voice:
        print('no voice in voices.tsv, skipped: ' + ','.join(sorted(skip_voice)))
    by = {}
    for t in todo:
        by[t['speaker']] = by.get(t['speaker'], 0) + len(t['text'])
    print('by speaker: ' + ', '.join('%s %d' % kv for kv in sorted(by.items(), key=lambda kv: -kv[1])))
    return todo, chars


def cmd_gen(a):
    todo, chars = plan(a)
    if not todo:
        return
    if not a.yes:
        print('dry run: add --yes to generate (spends about %d credits on multilingual,' % chars,
              'half on flash)')
        return
    os.makedirs(AUDIO, exist_ok=True)
    import concurrent.futures, threading, time
    lock, done, failed, out_of_credits = threading.Lock(), [0], [], []

    def one(t):
        if out_of_credits:  # the plan's credits are used up: never retry past the balance
            failed.append(t['key'])
            return
        v = t['voice']
        model = a.model or v.get('model') or DEFAULT_MODEL
        body = {'text': t['text'], 'model_id': model, 'voice_settings': {
            'stability': float(v.get('stability') or 0.5),
            'similarity_boost': float(v.get('similarity') or 0.75),
            'speed': t['speed']}}
        if 'v2_5' in model or 'v3' in model or 'v4' in model:
            body['language_code'] = 'uk'
        for attempt in range(6):  # 429 (too many requests) / 5xx / network: wait and retry
            try:
                mp3 = call('POST', '/v1/text-to-speech/' + v['voice_id'], body,
                           {'output_format': 'mp3_44100_128'}, raw=True)
                break
            except SystemExit as e:
                err = str(e)
                if 'quota_exceeded' in err:
                    out_of_credits.append(t['key'])
                if not any(c in err for c in ('HTTP 429', 'HTTP 5')) or attempt == 5:
                    with lock:
                        failed.append(t['key']); print('FAILED %s: %s' % (t['key'], err[:200]), flush=True)
                    return
                time.sleep(5 * (attempt + 1))
        path = os.path.join(AUDIO, t['key'] + '.mp3')
        with open(path + '.part', 'wb') as f:
            f.write(mp3)
        os.replace(path + '.part', path)  # a killed run never leaves a half file
        with lock:
            done[0] += 1
            print('%4d/%d %s (%d chars, speed %.2f)' % (done[0], len(todo), t['key'],
                                                         len(t['text']), t['speed']), flush=True)

    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        list(ex.map(one, todo))
    if out_of_credits:
        print('OUT OF CREDITS: stopped; %d lines not generated' % len(failed))
    if failed:
        print('%d failed (run again to retry): %s' % (len(failed), ','.join(failed)))


def cmd_credits(_a):
    s = call('GET', '/v1/user/subscription')
    used, lim = s.get('character_count', 0), s.get('character_limit', 0)
    print('tier %s: used %d of %d, left %d; resets %s' % (
        s.get('tier'), used, lim, lim - used, s.get('next_character_count_reset_unix')))


def cmd_voices(_a):
    for v in call('GET', '/v1/voices').get('voices', []):
        lab = v.get('labels') or {}
        print('%s\t%s\t%s' % (v['voice_id'], v['name'], ', '.join('%s=%s' % kv for kv in lab.items())))


def cmd_library(a):
    q = {'page_size': 50, 'language': a.lang}
    if a.q:
        q['search'] = a.q
    for v in call('GET', '/v1/shared-voices', query=q).get('voices', []):
        print('%s\t%s\t%s %s %s\t%s' % (v.get('voice_id'), v.get('name'), v.get('gender'),
                                        v.get('age'), v.get('accent'),
                                        (v.get('description') or '')[:80].replace('\n', ' ')))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument('cmd', choices=['credits', 'voices', 'library', 'worklist', 'plan', 'gen'])
    ap.add_argument('--speaker'); ap.add_argument('--bank'); ap.add_argument('--limit', type=int)
    ap.add_argument('--model'); ap.add_argument('--yes', action='store_true')
    ap.add_argument('--jobs', type=int, default=4, help='parallel requests (Creator allows 5)')
    ap.add_argument('--keys', help='file with line keys to process (one per line)')
    ap.add_argument('--audio-dir', help='write mp3s here instead of .cache/HP2Cache/voice/uk/audio (A/B)')
    ap.add_argument('--force', action='store_true', help='generate even if the mp3 exists')
    ap.add_argument('--compact', action='store_true', help='short tags (character x2 + line emotion x2)')
    ap.add_argument('--lang', default='uk'); ap.add_argument('--q')
    a = ap.parse_args()
    global AUDIO
    if a.audio_dir:
        AUDIO = a.audio_dir
    {'credits': cmd_credits, 'voices': cmd_voices, 'library': cmd_library,
     'worklist': cmd_worklist, 'plan': lambda a: plan(a), 'gen': cmd_gen}[a.cmd](a)


if __name__ == '__main__':
    main()
