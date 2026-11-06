#!/usr/bin/env python3
"""Ukrainian text for the speech clips that have no subtitle link (decisions).

Input: .cache/HP2Cache/voice/uk/matches.tsv (match_text.py). Per clip, in this order:
  MANUAL[wav]          hand translation (or SKIP: a sound without words stays English)
  card reading         "Number N, <name>" -> "Номер <N in words>, <card name uk>" (text 3223+N)
  Fred's bean prices   "<N> Bertie Bott's ..." -> "<N in words> драже Берті Бота на всі смаки!"
  Snape's duel rule    "First duellist to gain N points ..." -> text 843 pattern with N
  non-verbal           empty / (coughing) / *sigh* / [laughing] -> SKIP
  status 'match'       the game's own Ukrainian string
  score >= 0.78        the matched string (whisper slips: "For you" = "Phew!")
Output: .cache/HP2Cache/voice/uk/extra.tsv (wav, speaker, en, uk, how) used by tts.py worklist.
"""
import csv, glob, os, re
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))  # tools/
import hp2paths  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WORK = os.path.join(hp2paths.CACHE, 'voice/uk')
SKIP = 'SKIP'

ONES = ['', 'один', 'два', 'три', 'чотири', 'п’ять', 'шість', 'сім', 'вісім', 'дев’ять',
        'десять', 'одинадцять', 'дванадцять', 'тринадцять', 'чотирнадцять', 'п’ятнадцять',
        'шістнадцять', 'сімнадцять', 'вісімнадцять', 'дев’ятнадцять']
TENS = ['', '', 'двадцять', 'тридцять', 'сорок', 'п’ятдесят', 'шістдесят', 'сімдесят',
        'вісімдесят', 'дев’яносто']


def num(n):
    """Ukrainian cardinal (nominative), 1..999."""
    out = []
    if n >= 100:
        out.append(['', 'сто', 'двісті', 'триста', 'чотириста', 'п’ятсот', 'шістсот', 'сімсот',
                    'вісімсот', 'дев’ятсот'][n // 100]); n %= 100
    if n >= 20:
        out.append(TENS[n // 10]); n %= 10
    if n:
        out.append(ONES[n])
    return ' '.join(out)


MANUAL = {
    'SV_HRY_NIT04_0061C': 'Б-б-б-б-б!',
    'SV_HRY_AUGDF_0045': 'Треба спробувати інше закляття.',
    'SV_HRY_AUGDF_0044': '«Фліпендо» їх тільки оглушує.',
    'SV_HRY_AUGDF_0046': 'Марно. Треба спробувати щось інше.',
    'SV_HRY_AUGDF_0043': 'Треба захищатися «Експеліармусом».',
    'SV_HRY_AUGDF_0042': 'Треба застосувати «Експеліармус».',
    'SV_HRY_AUGDF_0029': 'Треба спробувати інше закляття.',
    'SV_HRY_AUGDF_0030': 'Треба взяти нове закляття.',
    'SV_HRY_AUGDF_0031': 'Нічого не виходить. Треба спробувати нове закляття.',
    'SV_HRY_AUGDF_0032': 'Треба цілитися заклинаннями в цю штуку.',
    'SV_HRY_AUGDF_0033': 'Нічого не виходить. Треба цілитися в неї заклинаннями.',
    'SV_HRY_AUGDF_0034': 'Треба навести на неї заклинання.',
    'SV_HRY_AUGDF_0035': 'Треба цілитися в них заклинаннями.',
    'SV_HRY_AUGDF_0036': 'Треба цілити в них заклинаннями.',
    'SV_HRY_AUGDF_0037': 'Треба навести заклинання.',
    'SV_BOR_AUGDA_0004': SKIP, 'SV_BOR_AUGDA_0005': SKIP,  # transcript unreliable
    'SV_HRY_NIT04_0047A': 'Геґрід, мабуть, у своїй хатині.',
    'SV_HRY_NIT01_0033B': 'Книжка закляття «Діффіндо»!',
    'SV_FRD_BUR03_0001B': 'Але-але-але-але…',
    'SV_FRD_BUR03_0024': 'Вдар «Фліпендо» по татовому Магічному вимірювачу.',
    'SV_GNM_BUR02_0001': SKIP,
    'SV_GRG_BUR03_0001E': 'Ой, мамо, я…',
    'SV_HRY_LVBUR_0001C': SKIP,
    'SV_MXD_BUR02_0005': 'О, так.',
    'SV_RON_AUGRW_0013': 'Давай, Гаррі, покажи Рейвенклову, як треба грати у квідич!',
    'SV_RON_AUGRW_0012': 'Гей, Гаррі, не забудь: у тебе матч проти Рейвенклову. Ходімо на стадіон.',
    'SV_RON_AUGRW_0014': 'Гаррі, сьогодні ідеальна погода. Усі чекають, щоб ти показав слизеринцям! Ходімо!',
    'SV_RON_AUGRW_0001': 'Ага, проти Гафелпафу, пам’ятаєш? Ходімо на стадіон, на перший матч сезону не можна запізнюватися.',
    'SV_NEV_NIT01_0005A': SKIP, 'SV_GIL_DY201_0003E': SKIP,
    'SV_SNP_DY201_0001': 'Експеліармус!',
    'SV_DRC_DY201_0006': 'Серпенсортія!',
    'SV_DRC_DY201_0012B': 'Фліпендо!',
    'SV_DRC_DY201_0013': 'Експеліармус!',
    'SV_GIL_DY201_0019B': SKIP, 'SV_GIL_DY201_0019D': SKIP,
    'SV_GIN_DIA01_0008D': 'Зустрінемося тут, коли все знайдеш.',
    'SV_MOL_DIA01_0014B': 'Гаррі!',
    'SV_HAG_DIA01_0008B': None, 'SV_HAG_DIA01_0007': None,  # take the match
    'SV_GIN_AUGDF_0013': 'Так!',
    'SV_WZ1_AUGPU_0002': None, 'SV_WZ1_AUGPU_0014': None,
    'SV_GOY_DY101_0001B': SKIP, 'SV_GOY_DY101_0002B': SKIP,
    'SV_RON_DY301_0022A': 'Агов, Гаррі, ти тут?',
    'SV_GOY_DY601_0001B': SKIP, 'SV_BAS_NIT01_0004': SKIP,
    'SV_RON_DY101_0010B': 'Ось так, є!',
    'SV_OWD_DY301_0004C': None, 'SV_OWD_DY301_0005C': 'Швидше!', 'SV_RON_DY601_0026A': 'Швидше!',
    'SV_FRD_FANDG_0054': 'Сто тридцять драже Берті Бота на всі смаки!',
    'SV_FRD_FANDG_0058': 'Сто сімдесят драже Берті Бота на всі смаки!',
    'SV_HRY_FANDG_0004A': None,
    'SV_GIL_DIA01_0003B': 'Широко всміхнися, Гаррі! Разом ми з тобою варті першої шпальти.',
    'SV_FAB_DIA01_0001C': None, 'SV_FAB_DIA01_0010A': SKIP, 'SV_GIL_DIA01_0016': None,
    'SV_GAJ_DIA01_0017': 'Ой лишенько, треба краще.',
    'SV_GAJ_DIA01_0018': 'Непогано, але треба більше сили.',
    'SV_GAJ_DIA01_0019': 'Набирай закляття довше.',
    'SV_GAJ_DIA01_0020B': 'Ти набирав силу задовго.',
    'SV_GAJ_DIA01_0021': 'Ой! Спробуй вдарити закляттям раніше.',
    'SV_GAJ_DIA01_0022B': 'Чудово! Ідеальне «Фліпендо»!',
    'SV_GAJ_DIA01_0016B': 'Щоб підняти Магічний вимірювач до самого верху, потрібне ідеальне «Фліпендо». Набирай силу в чарівній паличці якомога довше — і вдар закляттям за мить до того, як вона перебере через край.',
    'SV_HRY_GRING_0002B': None, 'SV_GR1_GRING_0009': None,
    'SV_GR1_GRING_0016': 'Триста вісімдесят сім, триста вісімдесят вісім… Іди геть, хлопче! Триста дев’яносто тисяч!',
    'SV_WZ1_GRING_0006': 'Дякую всім!',
    'SV_FP1_AUGPU_0181': 'Це коштуватиме драже Берті Бота.',
    'SV_MP1_AUGPU_0181': 'Привіт, Гаррі! А ти знаєш, що в Гоґвортсі можна змагатися в метанні гномів? Побий шкільний рекорд — і виграєш картку чарівника.',
    'SV_FP1_AUGPU_0179': 'Привіт, Гаррі! А ти знаєш, що в Гоґвортсі можна змагатися в метанні гномів? Побий шкільний рекорд — і виграєш картку чарівника.',
    'SV_MP1_AUGPU_0180': 'Гаррі, з деякими учнями в Гоґвортсі можна позмагатися в перегонах на картки чарівників. Тільки знадобиться мітла.',
    'SV_FP1_AUGPU_0178': 'Гаррі, з деякими учнями в Гоґвортсі можна позмагатися в перегонах на картки чарівників. Тільки знадобиться мітла.',
    'SV_MP1_AUGPU_0179': 'Привіт, Гаррі! А ти знаєш, що в Гоґвортсі з декотрими можна влаштувати перегони? Знайди їх, обжени — і виграєш картку чарівника! Тільки знадобиться мітла.',
    'SV_FP1_AUGPU_0177': 'Привіт, Гаррі! А ти знаєш, що в Гоґвортсі з декотрими можна влаштувати перегони? Знайди їх, обжени — і виграєш картку чарівника! Тільки знадобиться мітла.',
    'SV_MP1_AUGPU_0182': 'Гаррі, можна позмагатися в метанні гномів і спробувати побити шкільний рекорд. За рекорд дають картку чарівника. Треба тільки знайти того, хто проводить змагання.',
    'SV_FP1_AUGPU_0180': 'Гаррі, можна позмагатися в метанні гномів і спробувати побити шкільний рекорд. За рекорд дають картку чарівника. Треба тільки знайти того, хто проводить змагання.',
    'SV_HGD_DIA01_0015C': SKIP, 'SV_HGD_AUGPU_0007': SKIP,
    'SV_HER_DY201_0012B': 'Гаррі, ти тут?',
    'SV_TOM_DIA01_0004B': 'Зараз!',
    'SV_WZ1_DIA01_0007B': 'О, так!',
    'SV_WZ2_DIA01_0008B': None,
    'SV_FP1_NIT02_0002': 'Ми не Поттер!',
    'SV_WZ2_AUGPU_0005': 'Писане слово робить людину ерудованою і красномовною.',
    'SV_WZ2_AUGPU_0002': 'У бібліотеці є досить важке чтиво, особливо серед грубих томів.',
    'SV_WZ2_AUGPU_0003': 'Інформацію можна і слід черпати щонайменше з шести загальнодоступних книжок цієї бібліотеки.',
    'SV_WZ2_AUGPU_0004': 'Чи можу я порадити вам почитати, юначе?',
    'SV_WZ2_AUGPU_0001': 'Якщо шукаєте відомостей, їх можна знайти в будь-якій із наших чудових книжок.',
    'SV_WZ2_AUGPU_0006': 'Мені дуже до вподоби слово «пальцекрутство», а вам?',
    'SV_WZ2_AUGPU_0007': 'Labor omnia vincit, містере Поттер.',
    'SV_HRY_DIA01_0035B': None, 'SV_AS1_AUGDA_0004': 'Ай! Ай!',
    'SV_HRY_NIT00_0011': 'Флібертиджибет!',
    'SV_HRY_NIT00_0008': 'Ватлберд!',
    'SV_HRY_NIT02_0003': 'Бібліофіл.',
    'SV_HRY_QUSLY_0002D': 'Треба наздогнати Мелфоя.',
    'SV_HRY_QUSLY_0001C': 'Коли пролітаю крізь кільце, швидшаю.',
    'SV_HRY_QUSLY_0004D': 'Майже впіймав!',
    'SV_HRY_QUSLY_0005A': 'Ще трошки ближче…',
    'SV_HRY_QUSLY_0006E': 'Досить близько для прискорення.',
    'SV_HRY_QUSLY_0007D': 'Прискорення повне.',
    'SV_HRY_QUSLY_0008B': 'Я зовсім близько. От би ще прискорення.',
    'SV_HRY_QUSLY_0009D': 'Мелфой наздоганяє. Треба поквапитися.',
    'SV_HRY_QUSLY_0010A': 'Час майже вийшов.',
    'SV_HRY_QUSLY_0011C': 'Мелфой мене обійшов. Наступного разу треба швидше.',
    'SV_HRY_QUSLY_0012A': 'Майже є!',
    'SV_HRY_QUSLY_0013E': 'Ледь-ледь!',
    'SV_DRC_QUSLY_0002B': 'Що, вже не такий герой?',
    'SV_DRC_QUSLY_0001': 'Бувай, шрамоголовий!',
    'SV_DRC_QUSLY_0003B': 'Снич мій!',
    'SV_DRC_QUSLY_0004E': 'Ну ти й невдаха, Поттере.',
    'SV_DRC_QUSLY_0005F': 'Квідич явно не твоя гра!',
    'SV_DRC_QUSLY_0006B': 'Спіймай мене, якщо зможеш, Поттере!',
    'SV_MQ1_QUID1_0010': 'Дорогу!', 'SV_MQ1_QUID1_0001': 'Дорогу!',
    'SV_MQ1_QUID1_0011': 'Я лечу!', 'SV_MQ1_QUID1_0002': 'Я лечу!',
    'SV_MQ1_QUID1_0012': 'Не наздоженеш!', 'SV_MQ1_QUID1_0003': 'Не наздоженеш!',
    'SV_MQ1_QUID1_0013': 'Гафелпаф виграє цей матч!',
    'SV_MQ1_QUID1_0014': 'Гафелпаф найкращий!',
    'SV_MQ1_QUID1_0015': 'Ґрифіндор програє, Поттере!',
    'SV_MQ1_QUID1_0016': 'Гляди не впади!', 'SV_MQ1_QUID1_0007': 'Гляди не впади!',
    'SV_MQ1_QUID1_0017': 'Бувай!', 'SV_MQ1_QUID1_0008': 'Бувай!',
    'SV_MQ1_QUID1_0018': 'Щасти наступного разу!',
    'SV_MQ1_QUID1_0004': 'Рейвенклов виграє цей матч!',
    'SV_MQ1_QUID1_0005': 'Рейвенклов найкращий!',
    'SV_MQ1_QUID1_0006': 'Ґрифіндор вилітає, Гаррі.',
    'SV_DRC_QUID1_0002': 'Може, тобі уроки польотів узяти, Поттере?',
    'SV_DRC_QUID1_0003': 'Гей, Поттере! А як тобі таке?',
    'SV_DRC_QUID1_0004': 'Дорогу, шрамоголовий!',
    'SV_DRC_QUID1_0005': 'Стережися нас!',
    'SV_DRC_QUID1_0006': 'Ти програєш цей матч!',
    'SV_DRC_QUID1_0007': 'Що, велика голова заважає летіти, Поттере?',
    'SV_DRC_QUID1_0008': 'Гей! Розквашу тобі пику!',
    'SV_DRC_QUID1_0009': 'Ми вас розгромимо!',
    'SV_DRC_QUID1_0010': 'Як тобі програвати, Поттере?',
    'SV_DRC_QUID1_0011': 'Слизерин понад усе!',
    'SV_DRC_QUID1_0012': 'Гаррі Поттер, друг бруднокровок!',
    'SV_DRC_QUID1_0013': 'Тепер уже марно кликати мамусю, Поттере.',
    'SV_DRC_QUID1_0015': 'Бувай-бувай.',
    'SV_DRC_QUID1_0016': 'Гей, невдахо!',
    'SV_DRC_QUID1_0001': 'Ковтай пил з-під моєї мітли, Поттере!',
    'SV_OWD_QUIDT_0009': 'Неймовірний політ, Поттере! Навіть мадам Гуч поставила б тобі добру оцінку.',
    'SV_OWD_AUGPU_0011': None, 'SV_OWD_AUGPU_0021': None,
    'SV_OWD_QUIDT_0001': 'А, Поттере! Готовий продовжити нашу переможну серію з минулого року? Я певен, що ми досить добрі, але тренуватися доведеться ще більше. Не будемо гаяти часу — повторимо основи.',
    'SV_OWD_QUIDT_0002': 'Коли пролітаєш крізь кільце, магічний заряд мітли зростає, і вона летить швидше. А якщо пропускаєш кільце — заряд падає.',
    'SV_OWD_QUIDT_0003': 'Чудово, Поттере, мітла заряджена наполовину. Так тримати.',
    'SV_OWD_QUIDT_0004': 'Мітла повністю заряджена, тепер можна прискорюватися. Коли з мітли сиплються іскри — прискорення готове.',
    'SV_OWD_QUIDT_0005': 'А це бладжери, гидкі штуки, як ти пам’ятаєш з минулого року. Краще б тобі під них не потрапляти.',
    'SV_OWD_QUIDT_0006': 'Ти вже досить близько до снича, але щоб вийти на позицію і впіймати його, завжди треба прискоритися.',
    'SV_OWD_QUIDT_0007': 'Чудово, Поттере. А тепер хапай снич.',
    'SV_OWD_QUIDT_0008': 'Схоже, ти став кращим, ніж торік, Поттере. З таким складом Ґрифіндор знову візьме Кубок з квідичу — оце Снейпові допече! Гафелпаф теж тренується, тож вони погодилися зіграти з нами тренувальний матч. Тепер змагатимешся з їхнім ловцем, але я певен, для тебе це не проблема.',
    'SV_OWD_QUIDT_0010': 'Досить непогано, Поттере, але з практикою можна краще.',
    'SV_OWD_QUIDT_0011': 'Це було жахливо. Доведеться тренуватися до сьомого поту, якщо хочемо мати шанс проти Слизерину цього року. Чекаю тебе тут на нове тренування.',
    'SV_OWD_QUIDT_0012': 'Тільки щоб професорка Макґонеґел не дізналася — ось, візьми цю мітлу і тренуйся, скільки зможеш.',
    'SV_OWD_QUIDT_0013': 'Радий, що ти ставишся до цього серйозно, Поттере. Зіграймо ще один тренувальний матч.',
    'SV_OWD_QUIDT_0014': 'Не віриться, що ти зіграв гірше, ніж минулого разу. Нам треба ставати кращими. Може, відпочинь трохи і повертайся на тренування.',
    'SV_DUM_SPLRE_0001B': 'Ну-ну.',
    'SV_SNP_AUGPU_0005': 'Переможе той, хто першим набере дев’ять очок.',  # whisper: "nine"
    'SV_FRD_EMOTEOUCH_0002': 'Ой!',
    'SV_FRD_EMOTEHARRY_0001': 'Гаррі!',
    'SV_FRD_EMOTEHSTOPIT_0004': 'Гаррі, перестань!', 'SV_GRG_EMOTEHSTOPIT_0001': 'Гаррі, перестань!',
    'SV_GRG_EMOTEHSTOPIT_0008': 'Гаррі, перестань!', 'SV_RON_EMOTEHSTOPIT_0001': 'Гаррі, перестань!',
    'SV_RON_EMOTEHSTOPIT_0002': 'Гаррі, перестань!',
    'SV_GRG_SPLRE_0004': 'Відчепися!', 'SV_GRG_SPLRE_0003': SKIP,
    'SV_HER_SPLRE_0003': 'Ура!',
    'SV_RON_SPLRE_0003': 'Привіт, Гаррі!',
    'SV_OWD_SPLRE_0001': SKIP,
    'SV_RON_WHW01_0007': 'Вдар його ще раз, Гаррі!',
    'SV_RON_WHW01_0010C': 'Не здавайся, Гаррі!',
    'SV_RON_WHW01_0026C': 'Можна вже йти?',
    'SV_HER_NIT02_0018E': None,
    'SV_RON_BUR03_0020B': None, 'SV_RON_BUR03_0017B': None, 'SV_RON_BUR03_0030': 'Молодець, Фреде!',
    'SV_MOL_BUR03_0004D': None, 'SV_FRD_BUR03_0009E': None,
    'SV_GIN_DIA01_0019A': 'Швидше!',
    'SV_FRD_DIA01_0008D': None, 'SV_GRG_DIA01_0005C': None,
    'SV_HRY_DIA01_0047B': None, 'SV_HRY_DIA01_0011A': None, 'SV_HRY_AUGDF_0052': None,
    'SV_HRY_NIT04_0062A': None,
}
NONVERBAL = re.compile(r'^\W*(\(.*\)|\*.*\*|\[.*\]|a+h+!*|w+a+!*|wo+!*|whoa!*|ooh!*|aw+!*|ha!*|donk!*|uh-?uh\.?|)\W*$', re.I)


COMM = {}
_c = os.path.join(HERE, 'commentary.tsv')
if os.path.exists(_c):
    for _r in csv.DictReader(open(_c, encoding='utf-8'), delimiter='\t', quoting=csv.QUOTE_NONE):
        COMM[_r['clip']] = (_r['uk'], _r['tags'])


def main():
    text = {}
    for f in glob.glob(os.path.join(ROOT, 'mods/loc/uk/*.tsv')):
        rows = open(f, encoding='utf-8').read().splitlines()
        h = rows[0].split('\t')
        for r in rows[1:]:
            p = r.split('\t')
            uk = p[h.index('uk')] if len(p) > h.index('uk') else ''
            text.setdefault(int(p[0]), '' if uk == '{=}' else uk)  # {=}: the English string
    out, how_n = [], {}
    for r in csv.DictReader(open(os.path.join(WORK, 'matches.tsv'), encoding='utf-8'), delimiter='\t', quoting=csv.QUOTE_NONE):
        w, en, sc = r['wav'], r['transcript'], float(r['score'])
        how, uk = None, None
        m_card = re.match(r'(?:Number\s*)?(\d+)[.,]?\s', en)
        m_bean = re.match(r'(\d+)\s+\S+\s+\S+\s+every flavou?r beans', en, re.I)
        m_duel = re.search(r'(\d+) points wins the duel', en)
        if w in MANUAL and MANUAL[w] is not None:
            uk, how = MANUAL[w], 'manual'
        elif r['speaker'] == 'LEE':
            uk, how = COMM.get(w.split('_', 2)[-1], ('', ''))[0], 'commentary'
        elif m_card and '_CFROG_' in w:
            n = int(m_card.group(1))
            uk, how = 'Номер %s, %s.' % (num(n), text.get(3223 + n, '').strip()), 'card'
        elif m_bean:
            uk, how = '%s драже Берті Бота на всі смаки!' % num(int(m_bean.group(1))).capitalize(), 'beans'
        elif m_duel:
            uk, how = 'Переможе той, хто першим набере %s очок.' % num(int(m_duel.group(1))), 'duel'
        elif NONVERBAL.match(en):
            uk, how = SKIP, 'nonverbal'
        elif r['status'] == 'match' or w in MANUAL or sc >= 0.78:
            uk, how = text.get(int(r['text_id']), ''), 'subtitle'
        else:
            uk, how = '', 'todo'
        if uk == SKIP:
            how = 'nonverbal' if how != 'manual' else 'skip'
            uk = ''
        how_n[how] = how_n.get(how, 0) + 1
        out.append([w, r['speaker'], r['sec'], en, uk, how, r['text_id']])
    with open(os.path.join(WORK, 'extra.tsv'), 'w', encoding='utf-8') as f:
        f.write('wav\tspeaker\tsec\ten\tuk\thow\ttext_id\n')
        for o in out:
            f.write('\t'.join(x.replace('\t', ' ') for x in o) + '\n')
    print(how_n)
    for o in out:
        if o[5] == 'todo':
            print('TODO', o[0], o[3][:80])


if __name__ == '__main__':
    main()
