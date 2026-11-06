#!/usr/bin/env python3
"""build_metadata.py -- build the 3DO game metadata file the Cover Maker bundles.

Joins three permissively-licensed sources into one data file keyed by game name:
  * Redump names (the canonical dump names), from libretro-database's 3DO dat -- the set of games and their regions.
  * Wikipedia "List of 3DO games" table (CC BY-SA) -- developer, publisher and the per-region release year.
  * each game's Wikipedia article infobox (CC BY-SA) -- genre and single-/multiplayer mode.

Run it on a normal machine with internet (it talks to the Wikipedia API and reads the dat from libretro-database):

    python3 build_metadata.py --dat "The 3DO Company - 3DO.dat" --out 3do_meta.json

The result is a JSON object keyed by the game's title key (see title_key, shared with covermaker.py), each value:
    {"year": 1996, "publisher": "Art Data Interactive", "developer": "id Software",
     "genre": "First-person shooter", "mode": "Single player", "regions": ["USA"]}
Year/publisher are resolved per region where the table splits them. The data is CC BY-SA (Wikipedia-derived): ship
the file with that notice, separate from the all-rights-reserved menu and the MIT tools.

Testing offline: --list-cache FILE reads the table from a pipe-delimited cache (TITLE|DEV|PUB|NA|PAL|JP per line)
instead of the live page, and --no-infobox skips the per-game genre/mode fetches. The join logic is then exercised
with no network.
"""
import argparse
import json
import re
import sys
import time
import unicodedata
import urllib.parse
import urllib.request

WP_API = 'https://en.wikipedia.org/w/api.php'
AGENT = '3do-ode-metadata/1.0 (https://github.com/thgill/3do-ode-menu)'
REGION_TAGS = ('USA', 'Europe', 'Japan', 'Korea', 'Germany', 'France', 'World', 'Spain', 'Italy', 'Australia')
REGION_YEAR = {'USA': 'na', 'World': 'na', 'Europe': 'pal', 'Germany': 'pal', 'France': 'pal', 'Spain': 'pal',
               'Italy': 'pal', 'Australia': 'pal', 'Japan': 'jp', 'Korea': 'jp'}
REGION_TAG = {'na': ('NA', 'USA', 'US', 'North America'), 'pal': ('PAL', 'EU', 'Europe', 'UK'),
              'jp': ('JP', 'Japan', 'JPN')}


# ---- name handling (title_key kept identical to covermaker.py so the keys line up) ----
def strip_accents(s):
    return ''.join(c for c in unicodedata.normalize('NFKD', s) if not unicodedata.combining(c))


def title_key(n):
    t = re.sub(r'[(\[][^)\]]*[)\]]', ' ', n).replace('_', ' ').lower()
    t = re.sub(r',\s*the\b', ' ', t)
    t = re.sub(r'^\s*the\s+', ' ', t)
    t = re.sub(r'[^a-z0-9]+', ' ', t)
    return t.strip()


def alt_keys(n):
    """keys to try when the exact title key misses: hyphen-joined, de-subtitled, a few known aliases."""
    base = title_key(n)
    keys = {base, base.replace(' ', '')}
    stem = re.split(r'\s[-:]\s| tetsujin| combat in', base)[0].strip()
    keys.add(stem)
    keys.add(stem.replace(' ', ''))
    ALIAS = {'out of this world': 'another world', 'braindead 13': 'brain dead 13',
             'snowjob': 'snow job', 'fatty bears funpack': 'fatty bears fun pack'}
    if base in ALIAS:
        keys.add(ALIAS[base])
    return {k for k in keys if k}


# ---- Redump names (the game set) ----
def load_redump(dat_path):
    games = []
    for line in open(dat_path, encoding='utf-8', errors='replace'):
        m = re.match(r'\s*name "(.+?)"', line)
        if not m:
            continue
        name = m.group(1)
        if name.lower().endswith(('.cue', '.bin')):
            continue
        pm = re.search(r'\(([^)]*)\)', name)
        region = 'USA'
        if pm:
            first = pm.group(1).split(',')[0].strip()
            if first in REGION_TAGS:
                region = first
        base = re.sub(r'\s*\(.*$', '', name)
        games.append({'name': name, 'base': base, 'region': region})
    return games


# ---- Wikipedia ----
def wp_get(params):
    params = dict(params, format='json')
    url = WP_API + '?' + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={'User-Agent': AGENT})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def wp_wikitext(page):
    d = wp_get({'action': 'parse', 'page': page, 'prop': 'wikitext'})
    return d['parse']['wikitext']['*']


def clean_cell(s):
    s = re.sub(r'<ref[^>]*>.*?</ref>', '', s, flags=re.S)
    s = re.sub(r'<ref[^>]*/>', '', s)
    s = re.sub(r'\{\{[^{}]*\}\}', '', s)
    s = re.sub(r'\[\[(?:[^|\]]*\|)?([^\]]*)\]\]', r'\1', s)   # [[a|b]] -> b, [[a]] -> a
    s = re.sub(r"'''?", '', s)
    s = re.sub(r'<[^>]+>', ' ', s)
    return re.sub(r'\s+', ' ', s).strip()


def cell_primary(s):
    """A publisher/developer cell often lists one value per region, separated by <br>, newlines or bullets. Keep the
    first (primary) one, so the field stays a single name instead of several regions' names run together."""
    first = re.split(r'<br\s*/?>|\n|&bull;|•|;', s, maxsplit=1)[0]
    return clean_cell(first)


def year_of(s):
    m = re.search(r'\b(19|20)\d\d\b', s)
    return int(m.group(0)) if m else None


def _tables(wikitext):
    """each wikitable body on the page, in order (non-nested; the list page's game tables don't nest)."""
    out, i = [], 0
    while True:
        s = wikitext.find('{|', i)
        if s < 0:
            break
        e = wikitext.find('\n|}', s)
        if e < 0:
            e = wikitext.find('|}', s)
        if e < 0:
            break
        out.append(wikitext[s:e])
        i = e + 2
    return out


def _parse_table(body):
    rows = []
    for chunk in re.split(r'\n\|-', body):
        pairs = []
        for raw in re.split(r'\n\s*[|!]', chunk):
            raw = raw.strip()
            if not raw or raw.startswith(('{|', 'class=', 'style=', 'scope', '|+')):
                continue
            # a cell may carry attributes before a single | ; keep what follows the last leading pipe
            raw = raw.split('||')[-1] if '||' in raw else raw
            c = clean_cell(raw)
            if c:
                pairs.append((c, raw))
        cells = [c for c, _ in pairs]
        if (len(cells) >= 2 and year_of(' '.join(cells))) or len(cells) >= 4:
            # heuristic columns: title, developer, publisher, then the "First released" / NA / JP / PAL year columns.
            # developer and publisher take only the primary (first) value when a cell lists several by region.
            title = cells[0]
            dev = cell_primary(pairs[1][1]) if len(pairs) > 1 else ''
            pub = cell_primary(pairs[2][1]) if len(pairs) > 2 else ''
            yrs = [year_of(c) for c in cells[3:7]] + [None, None, None]
            rows.append({'title': title, 'dev': dev, 'pub': pub,
                         'na': yrs[0], 'pal': yrs[1], 'jp': yrs[2]})
    return rows


def parse_list_table(wikitext):
    """the main 'officially released' games wikitable -> rows {title, dev, pub, na, pal, jp}. The page opens with a
    small region-key table (and later has smaller non-game / homebrew / arcade tables); the games table is the
    largest one that carries both Developer and Publisher columns, so parse every table and keep that one."""
    best = []
    for body in _tables(wikitext):
        if 'Developer' not in body or 'Publisher' not in body:
            continue
        rows = _parse_table(body)
        if len(rows) > len(best):
            best = rows
    return best


def parse_list_lines(lines):
    rows = []
    for line in lines:
        p = [x.strip() for x in line.split('|')]
        if len(p) < 6:
            continue
        rows.append({'title': p[0], 'dev': p[1], 'pub': p[2],
                     'na': year_of(p[3]), 'pal': year_of(p[4]), 'jp': year_of(p[5])})
    return rows


def parse_list_cache(path):
    with open(path, encoding='utf-8') as f:
        return parse_list_lines(f)


LIST_TMPL = r'\{\{\s*(?:ubl|unbulleted list|hlist|flatlist|plainlist|plain list|cslist|collapsible list|nowrap|sortname)\s*\|?'


def ib_value(wt, names):
    """an infobox field's value, with wiki-list templates ({{ubl|..}}, {{plainlist|* ..}}) unwrapped to a
    comma-separated list. Genre/mode are often stored that way, so taking just the first line misses them."""
    m = re.search(r'\n\s*\|\s*(?:' + names + r')\s*=\s*', wt, re.I)
    if not m:
        return ''
    rest = wt[m.end():]
    nxt = re.search(r'\n\s*\|\s*[\w ]+=|\n\}\}', rest)       # the next field, or the end of the infobox
    val = rest[:nxt.start()] if nxt else rest
    val = re.sub(r'<ref[^>]*>.*?</ref>', '', val, flags=re.S)
    val = re.sub(r'<ref[^>]*/>', '', val)
    val = re.sub(r'\[\[(?:[^|\]]*\|)?([^\]]*)\]\]', r'\1', val)   # resolve links before splitting on |
    val = re.sub(LIST_TMPL, '', val, flags=re.I).replace('}}', '')   # drop the list-template wrapper, keep items
    items = []
    for part in re.split(r'[\*\|\n]', val):                  # bullets, template args, lines
        part = re.sub(r"'''?", '', part).strip(" ,;")
        if part and '=' not in part and not part.startswith(('{{', '<')):
            items.append(part)
    seen, out = set(), []
    for it in items:                                          # dedupe, keep order
        k = it.lower()
        if k not in seen:
            seen.add(k)
            out.append(it)
    return re.sub(r'\s+', ' ', ', '.join(out)).strip()


def get_infobox(title, cache):
    if title in cache:
        return cache[title]
    out = {'genre': '', 'mode': ''}
    try:
        wt = wp_get({'action': 'parse', 'page': title, 'prop': 'wikitext', 'section': '0'})
        wt = wt['parse']['wikitext']['*']
        out['genre'] = ib_value(wt, 'genres?')
        out['mode'] = ib_value(wt, 'modes?')
    except Exception as e:
        sys.stderr.write('  infobox miss %s: %s\n' % (title, e))
    cache[title] = out
    time.sleep(0.1)
    return out


# ---- region-aware field resolution ----
_ANY_TAG = r'\((?:NA|PAL|JP|EU|US|Japan|Europe)\)'


def region_field(value, region_slot):
    """a table cell like 'Interplay (NA) Infogrames (PAL)' -> the clause for this region, else the whole thing."""
    if not value:
        return ''
    tags = REGION_TAG.get(region_slot, ())
    # split into clauses by the region markers; keep the clause tagged for this region if any
    for tag in tags:
        m = re.search(r'([^()]+)\(' + re.escape(tag) + r'\)', value)
        if m:
            return m.group(1).strip(' ,')
    # no region split -> strip any region tags and return the lead clause
    lead = re.split(_ANY_TAG, value)[0]
    return lead.strip(' ,') or value.strip()


def region_pub_names(rows):
    """The set of per-region publisher/developer names in the table: whatever follows a region tag is a cleanly
    bounded single name (e.g. '... Infogrames (PAL) Pony Canyon (JP)' -> {Infogrames, Pony Canyon}). Used to strip a
    trailing Japanese/PAL name off a cell whose primary (NA) value wasn't tagged."""
    names = set()
    for r in rows:
        for cell in (r.get('pub', ''), r.get('dev', '')):
            for cl in re.split(_ANY_TAG, cell)[1:]:
                nm = cl.strip(' ,')
                if nm and 2 <= len(nm.split()) <= 4 and re.match(r"^[A-Za-z0-9&.'\- ]+$", nm):
                    names.add(nm)
    return sorted(names, key=lambda s: -len(s))


def primary_name(value, suffixes):
    """One name, not several regions' names run together: the first comma-segment, with any trailing region-specific
    name (from `suffixes`) stripped, and an immediately repeated lead ('Electronic Arts Electronic Arts') collapsed."""
    if not value:
        return value
    s = value.split(',')[0].strip()
    changed = True
    while changed:
        changed = False
        for suf in suffixes:
            if s.lower().endswith(' ' + suf.lower()) and len(s) > len(suf) + 1:
                s = s[:-(len(suf) + 1)].strip(' ,')
                changed = True
                break
    w = s.split()
    for n in range(1, len(w) // 2 + 1):
        if w[:n] == w[n:2 * n]:
            return ' '.join(w[n:]).strip(' ,')
    return s.strip(' ,')


def build(redump, rows, want_infobox, verbose):
    index = {}
    for r in rows:
        for k in alt_keys(r['title']):
            index.setdefault(k, r)
    suffixes = region_pub_names(rows)       # per-region names to strip off an untagged primary (keeps fields short)
    ib_cache = {}
    out = {}
    matched = 0
    for g in redump:
        row = None
        for k in alt_keys(g['base']):
            if k in index:
                row = index[k]
                break
        key = title_key(g['name'])
        if not row:
            out.setdefault(key, {'regions': []})
            if g['region'] not in out[key]['regions']:
                out[key]['regions'].append(g['region'])
            continue
        matched += 1
        slot = REGION_YEAR.get(g['region'], 'na')
        year = row.get(slot) or row.get('na') or row.get('pal') or row.get('jp')
        pub = primary_name(region_field(row['pub'], slot), suffixes)
        dev = primary_name(region_field(row['dev'], slot), suffixes)
        ib = get_infobox(row['title'], ib_cache) if want_infobox else {'genre': '', 'mode': ''}
        genre = (ib['genre'] or '').split(',')[0].strip()   # one primary genre keeps the info pane from overflowing
        e = out.setdefault(key, {'regions': []})
        e.update({'year': year, 'publisher': pub, 'developer': dev,
                  'genre': genre, 'mode': ib['mode'], 'wikipedia': row['title']})
        if g['region'] not in e['regions']:
            e['regions'].append(g['region'])
        if verbose:
            sys.stderr.write('  %-40s -> %s %s / %s\n' % (g['name'], year, pub, ib['genre']))
    sys.stderr.write('matched %d / %d redump names\n' % (matched, len(redump)))
    return out


# The "List of 3DO games" table, cached (TITLE|DEV|PUB|NA|PAL|JP). Used when the live page parse comes up
# short, so the build still works offline-ish (it still fetches each game's infobox for genre/mode).
EMBEDDED_LIST = """\
Advanced Dungeons & Dragons: DeathKeep | Lion Entertainment | Strategic Simulations, Inc. | 1995 | 1995 | -
Advanced Dungeons & Dragons: Slayer | Lion Entertainment | Strategic Simulations, Inc. (NA) Mindscape (PAL) T&E Soft (JP) | 1994 | 1995 | 1995
AI Shogi | Taito | Taito | - | - | 1995
Akagi Tōhaiden | Micronet | Micronet | - | - | 1996
Alfred Hitchcock Presents | Toyota | Panasonic | - | - | 1994
Alone in the Dark | I-Motion, Krisalis Software | Interplay Productions (NA) Infogrames (PAL) Pony Canyon (JP) | 1994 | 1994 | 1994
Alone in the Dark 2 | I-Motion, Krisalis Software | Interplay Productions (NA) Infogrames Multimedia (PAL) Electronic Arts Victor (JP) | 1995 | 1995 | 1995
Another World | Interplay Productions, Lil' Gangsters Entertainment | Interplay Productions Electronic Arts Victor (JP) | 1994 | 1994 | 1994
Aqua World: Umibi Monogatari | Mizuki | Mizuki | - | - | 1995
Armageddon | GoldStar / LG Electronics | GoldStar / LG Electronics | - | - | 1996
AutoBahn Tokio | Sanai Enterprise, Sanyei Shobou | Panasonic | - | - | 1995
Bakushō!! All Yoshimoto Quiz-Ou Ketteisen | Tose | Yoshimoto Kogyo | - | - | 1995
Ballz: The Director's Cut | Cave Logic Studios, PF.Magic | Panasonic BMG Interactive (JP) | 1995 | 1995 | 1995
Battle Blues | Shin's Deco | GoldStar / LG Electronics | - | - | 1996
Battle Chess | Krisalis Software | Interplay Productions Electronic Arts Victor (JP) | 1994 | 1994 | 1994
Battle Pinball | Japan DataWorks | Panasonic | - | - | 1994
BattleSport | Cyclone Studios | The 3DO Company | 1995 | 1996 | -
BC Racers | Core Design | GoldStar / LG Electronics | 1995 | 1995 | -
Belzerion | Human Entertainment | Human Entertainment | - | - | 1994
Blade Force | Studio 3DO | The 3DO Company | 1995 | 1995 | 1995
Blue Forest Story: Kaze no Fūin | Right Stuff | Panasonic | - | - | 1996
Bonogurashi | Amuse Productions, Bandai Visual | Amuse Productions | - | - | 1995
Brain Dead 13 | ReadySoft | ReadySoft | 1995 | - | -
Burning Soldier | Genki | Panasonic Pack-In-Video (JP) | 1994 | 1994 | 1994
Bust-a-Move | Microcabin | Taito (JP) Panasonic (NA) | 1995 | - | 1995
Cannon Fodder | Sensible Software | Virgin Interactive | 1994 | 1994 | -
Captain Quazar | Cyclone Studios | The 3DO Company | 1996 | 1996 | 1996
Carrier: Fortress at Sea | RMG Dentsu USA | Panasonic | 1995 | - | -
Casper | Logicware | Interplay Productions | 1996 | - | -
Chiki Chiki Machine Mō Race | Future Pirates | Future Pirates | - | - | 1994
Chiki Chiki Machine Mō Race 2: In Space | Future Pirates | Future Pirates | - | - | 1995
Club 3DO: Station Invasion | Studio 3DO | The 3DO Company | 1994 | 1994 | -
Corpse Killer | Digital Pictures | Acclaim Distribution (NA) Acclaim Japan (JP) | 1995 | 1994 | 1995
Cowboy Casino | The Computer Studio, Intellimedia Sports | IntelliPlay | 1995 | - | -
Crash 'n Burn | Crystal Dynamics | Crystal Dynamics | 1993 | - | 1994
Crayon Shin-chan: Puzzle Daimaou no Nazo | Tose | Bandai | - | - | 1995
Creature Shock | Argonaut Software | Virgin Interactive | 1996 | - | -
Crime Patrol | American Laser Games | American Laser Games | 1994 | - | -
Cyberdillo | Pixel Technologies | Panasonic | 1996 | - | -
Cyberia | Xatrix Entertainment | Interplay Productions Electronic Arts Victor (JP) | 1996 | 1994 | 1996
D •D no Shokutaku: Director's Cut | Warp | Panasonic Sanei Shobo Publishing (JP) | 1995 | 1995 | 1995
The Daedalus Encounter | Life Like Productions, Palmsoft | Panasonic Hatnet (JP) | 1995 | 1995 | 1995
Defcon 5 | Millennium Interactive | GoldStar / LG Electronics (PAL) Multisoft (JP) | - | 1995 | 1996
Demolition Man | Alexandria, Inc. | Virgin Interactive | 1994 | 1995 | 1994
DinoPark Tycoon | Manley & Associates | MECC | 1994 | - | -
Doctor Hauzer | Riverhillsoft | Panasonic | - | - | 1994
Doom | Art Data Interactive, id Software, Logicware | Art Data Interactive Basho House (JP) | 1996 | 1996 | 1996
Doraemon Yūjō Densetsu | Riverhillsoft | Shogakukan | - | - | 1995
Dragon Lore: The Legend Begins | Cryo Interactive | Mindscape | 1995 | 1995 | -
Dragon Tycoon Edge | Sala International | Sanyo | - | - | 1995
Dragon's Lair | ReadySoft | ReadySoft T&E Soft (JP) | 1994 | 1994 | 1994
Drug Wars | American Laser Games | American Laser Games | 1994 | - | -
Eigo de Go! | Gakken | Gakken | - | - | 1996
Emit Vol. 1: Toki no Maigo | Koei | Koei | - | - | 1995
Emit Vol. 2: Inochigake no Tabi | Koei | Koei | - | - | 1995
Emit Vol. 3: Watashi ni Sayonara o | Koei | Koei | - | - | 1995
Escape from Monster Manor | Studio 3DO | Electronic Arts Electronic Arts Victor (JP) | 1994 | 1994 | 1994
The Eye of Typhoon | Viccom | GoldStar / LG Electronics | - | - | 1996
F1GP | Dynamite, Opera House | Pony Canyon | - | - | 1995
Family Feud | Eurocom | GameTek | 1994 | - | -
Fatty Bear's Birthday Surprise | Humongous Entertainment | Humongous Entertainment (NA) Marubeni, Media Vision (JP) | 1994 | - | 1995
Fatty Bear's Fun Pack | Humongous Entertainment | Humongous Entertainment (NA) Marubeni, Media Vision (JP) | 1994 | - | 1994
FIFA International Soccer | Extended Play Productions | EA Sports | 1994 | 1994 | -
Firewall: Man vs. Machine | Visionary Media, Inc. | GoldStar / LG Electronics | - | - | 1996
Flashback: The Quest for Identity | Delphine Software International, Tiertex Design Studios | U.S. Gold Electronic Arts Victor, Interplay Productions (JP) | 1995 | 1995 | 1995
Flying Nightmares | Life Like Productions, Simis Limited | Domark Software | 1995 | 1995 | -
Foes of Ali | Gray Matter | EA Sports | 1995 | 1995 | -
Fun 'n Games | Williams Entertainment | Panasonic (NA) Virgin Interactive (JP) | 1995 | - | 1995
Furopon World | Warp | Warp | - | - | 1995
Gakkō no Kowai Uwasa: Hanako-San ga Kita!! | JASPAC | Amuse Productions | - | - | 1995
Game no Tatsujin | Sunsoft | Sunsoft | - | - | 1995
Gex | Crystal Dynamics | Crystal Dynamics (NA) BMG Interactive | 1994 | 1995 | 1995
Goal FH | JASPAC, NHK | Carrozzeria Japan | - | - | 1995
Gridders | Tetragon | The 3DO Company | 1994 | 1994 | 1994
Guardian War | Microcabin | Panasonic | 1994 | 1994 | 1994
Hell: A Cyberpunk Thriller | Take-Two Interactive, Tetragon | GameTek | 1994 | 1994 | -
Hello Kitty: Asobi no Omochabako | Mizuki | Mizuki, Sanrio | - | - | 1995
The Horde | Toys For Bob | Crystal Dynamics (NA) BMG Interactive (PAL) Byse, Inc. (JP) | 1994 | 1994 | 1994
Icebreaker | Magnet Interactive Studios | Panasonic | 1995 | - | -
Ide Yōsuke Meijin no Shin Jissen Mahjong | Capcom | Capcom | - | - | 1996
Idol Janshi Suchie-Pai Special | Jaleco | Jaleco | - | - | 1995
Idol Mahjong Final Romance 2: Hyper Edition | Infini Entertainment Technology, Video System | ASK Kodansha | - | - | 1996
Igo Time Trial: Shikatsu Daihyakka | Ematec | Ematec | - | - | 1995
Igo Time Trial: Thumego 1 | Ematec | Ematec | - | - | 1994
Īda Jōji Nightmare Interactive: Moon Cradle - Igyō no Hanayome | Tose | Pack-In-Video | - | - | 1995
Immercenary | Five Miles Out | Electronic Arts Electronic Arts Victor (JP) | 1995 | 1995 | 1995
The Incredible Machine | Dynamix | Dynamix Sierra On-Line, T&E Soft (JP) | 1994 | 1994 | 1994
Insect War | Riverhillsoft | Panasonic | - | - | 1994
Iron Angel of the Apocalypse | Synergy, Inc. | Panasonic (JP) Synergy Interactive | 1995 | 1995 | 1994
Iron Angel of the Apocalypse: The Return | Synergy, Inc. | Synergy, Inc (JP) Synergy Interactive (NA) | 1995 | - | 1995
Ishida Yoshio Kudan no Igo Seiha | G.A.M. | Electronic Arts Victor | - | - | 1995
J.League Virtual Stadium | JASPAC | Electronic Arts Victor | - | - | 1994
J.League Virtual Stadium '95 | EA Sports | Electronic Arts Victor | - | - | 1995
J.B. Harold: Blue Chicago Blues | Riverhillsoft | Riverhillsoft | - | - | 1995
Jammit | GTE Vantage | GTE Entertainment | 1994 | - | -
Jikki Pachi-Slot Simulator Vol. 1 | Nexton | Electronic Arts Victor | - | - | 1995
John Madden Football | High Score Productions | EA Sports Electronic Arts Victor (JP) | 1994 | 1994 | 1994
Johnny Bazookatone | Arc Developments | U.S. Gold | 1996 | 1995 | -
Jurassic Park Interactive | Studio 3DO | Universal Interactive Studios Panasonic (JP) | 1994 | 1994 | 1994
Kakinoki Shogi | ASCII Corporation | Panasonic | - | - | 1994
Kero Kero Keroppi to Origami no Tabibito | JASPAC, Mizuki | Mizuki, Sanrio | - | - | 1995
Killing Time | Studio 3DO | The 3DO Company | 1995 | 1995 | 1995
Kingdom: The Far Reaches | Interplay Productions, Virtual Image Productions | Interplay Productions | 1995 | 1996 | -
Konpeki no Kantai | Microcabin | Tokuma Shoten | - | - | 1995
Kurokishi no Kamen | HummingBirdSoft | Panasonic | - | - | 1994
The Last Bounty Hunter | American Laser Games | American Laser Games | 1995 | - | -
Lemmings | DMA Design | Psygnosis (NA) Electronic Arts Victor (JP) | 1994 | - | 1994
Lost Eden | Cryo Interactive | Virgin Interactive | 1995 | - | -
The Lost Files of Sherlock Holmes | Mythos Software | Electronic Arts | 1994 | 1994 | -
Lucienne's Quest | Microcabin | Panasonic (NA) Microcabin (JP) | 1996 | - | 1995
Macaroni Hōren Shō Interactive | Future Pirates | Toshiba EMI | - | - | 1995
Mad Dog II: The Lost Gold | American Laser Games | American Laser Games | 1994 | - | -
Mad Dog McCree | American Laser Games | American Laser Games | 1994 | - | -
Mahjong Gokū Tenjiku | Chat Noir | ASCII Corporation | - | - | 1994
Mahjong-kyo Jidai: AV Gal Seifukuhen | Micronet | Micronet | - | - | 1994
Mahjong-kyo Jidai: Ko Gal Hokagohen | Micronet | Micronet | - | - | 1995
Masters: Harukanaru Augusta 3 | T&E Soft | T&E Soft | - | - | 1994
Mazer | American Laser Games | American Laser Games | 1995 | - | -
MegaRace | Cryo Interactive | Mindscape Multisoft (JP) | 1994 | 1994 | 1994
Microcosm | Psygnosis | Psygnosis (NA) T&E Soft (JP) | 1994 | - | 1994
Mind Teazzer | Vivid Interactive | Vivid Interactive | 1994 | 1994 | -
Mirai Shōnen Konan Digital Library | Bandai Visual | Bandai Visual | - | - | 1995
Monoshiri Koro Yūgaku | Shinko Human Create | Shinko Human Create | - | - | 1995
Montana Jones | Future Pirates | Future Pirates | - | - | 1995
Murphy da yo Zenin Shūgō | Vantan International | Vantan International | - | - | 1994
Myst | Microcabin | Panasonic Microcabin (JP) | 1995 | 1995 | 1995
N.O.B.: Neo Organic Bioform | Octagon Entertainment | Sanyo | - | - | 1995
Nemurenu Yoru no Chīsana Ohanashi | Amuse Productions | Amuse Productions | - | - | 1994
NeuroDancer: Journey Into the Neuronet! | Electric Dreams Inc. | PIXIS Interactive | 1994 | - | -
Nice Body All-Star Suiei Taikai | BrainBusters, East Wind Corporation | Fuji Television | - | - | 1995
Night Trap | Digital Pictures | Virgin Interactive Virgin Games (JP) | 1994 | 1994 | 1994
Nishimura Kyōtarō Travel Mystery: Akugyaku no Kisetsu - Tokyo~Nanki Shirahama Renzoku Satsujin Jiken | Tose | Pack-In-Video, Panasonic | - | - | 1994
Nobunaga no Yabō: Haōden | Koei | Koei | - | - | 1994
Nontan to Issho: Hoshino Okurimono | Victor Entertainment | Panasonic | - | - | 1995
Nontan to Issho: Nohara de Asobo | Victor Entertainment | Panasonic | - | - | 1994
Novastorm | Psygnosis | Psygnosis (NA) T&E Soft (JP) | 1994 | - | 1994
Off-World Interceptor | Crystal Dynamics | Crystal Dynamics BMG Interactive (JP) | 1994 | 1994 | 1994
Olympic Soccer | Silicon Dreams, Tiertex Design Studios | Eidos Interactive, Panasonic, U.S. Gold | 1996 | - | -
Olympic Summer Games | Silicon Dreams, Tiertex Design Studios | Eidos Interactive, Panasonic, U.S. Gold | 1996 | - | -
Oneesan to Issho! Janken Paradise | V.I.P. Corporation | Intarus | - | - | 1994
Oneesan to Issho! Kisekae Paradise | B.S.S. Corporation | B.S.S. Corporation | - | - | 1995
Ōkoku no Grand Chef | Sala International | Sala International | - | - | 1996
Oyaji Hunter Mahjong | Warp | Warp | - | - | 1995
Panzer General | Strategic Simulations, Inc. | Strategic Simulations, Inc. (NA) Mindscape (PAL) | 1995 | 1995 | -
PaTaank | PF.Magic | PF.Magic BMG Interactive (JP) | 1994 | 1994 | 1994
Pebble Beach Golf Links | T&E Soft | Panasonic T&E Soft (JP) | 1994 | 1994 | 1994
The Perfect General | Game Guild | Kirin Entertainment | 1996 | - | -
Peter Frankl: Puzzle no Tō | TBS Multimedia | Hamlet | - | - | 1995
PGA Tour 96 | Hitmen Productions, NuFX | EA Sports | 1995 | 1995 | -
Phoenix 3 | Gray Matter | The 3DO Company | 1995 | 1995 | -
Plumbers Don't Wear Ties | United Pixtures | Kirin Entertainment | 1994 | - | -
PO'ed | Any Channel | Any Channel (NA) The 3DO Company (PAL) | 1995 | 1995 | -
Policenauts | Konami | Konami | - | - | 1995
Pretty Soldier Sailor Moon S | Tose | Bandai | - | - | 1995
Primal Rage | Probe Entertainment | GoldStar / LG Electronics | 1995 | 1995 | -
Princess Maker 2 | Gainax | Microcabin | - | - | 1995
Pro Stadium | Workman | Sanyo | - | - | 1995
Pro Yakyū Virtual Stadium: Professional Baseball | EA Sports | Electronic Arts Victor | - | - | 1995
Psychic Detective | Colossal Pictures, Electronic Arts | Electronic Arts | 1995 | 1995 | -
Puppet Tale | Microcabin | Panasonic | - | - | 1994
Putt-Putt Goes to the Moon | Humongous Entertainment | Humongous Entertainment | 1994 | - | -
Putt-Putt Joins the Parade | Humongous Entertainment | Humongous Entertainment (NA) Marubeni, Media Vision (JP) | 1994 | - | 1994
Putt-Putt's Fun Pack | Humongous Entertainment | Humongous Entertainment (NA) Marubeni, Media Vision (JP) | 1994 | - | 1994
Pyramid Intruder | HighTech Lab. Japan | Taito | - | - | 1995
Quarantine | Imagexcel | GameTek Imagineer (JP) | 1995 | 1995 | 1995
Quarterback Attack with Mike Ditka | Digital Pictures | Acclaim Distribution | 1995 | - | -
Real Pinball | Japan DataWorks | Panasonic | 1994 | 1994 | 1994
Return Fire | Silent Software | Prolific Publishing The 3DO Company (JP) | 1995 | 1995 | 1995
Return Fire: Maps O' Death (expansion) | Silent Software | Prolific Publishing (NA) The 3DO Company (PAL) | 1995 | 1996 | -
Rise of the Robots | Art Data Interactive | Absolute Entertainment (NA) Mirage (PAL) | 1995 | 1995 | -
Road & Track Presents: The Need for Speed | Electronic Arts Canada | Electronic Arts Electronic Arts Victor (JP) | 1994 | 1994 | 1994
Road Rash | Monkey Do Productions, New Level Software | Electronic Arts Electronic Arts Victor (JP) | 1994 | 1994 | 1994
Robinson's Requiem | Silmarils | ReadySoft | 1996 | - | -
Royal Pro Wrestling: Jikkyō Live!! | Natsume Co., Ltd. | Natsume Co., Ltd. | - | - | 1996
Samurai Shodown | Crystal Dynamics | Crystal Dynamics (NA) BMG Interactive | 1994 | 1995 | 1995
Sangokushi IV | Koei | Koei | - | - | 1995
Scramble Cobra | Genki | Panasonic Pack-In-Video (JP) | 1995 | 1995 | 1995
Seal of the Pharaoh | System Sacom | Panasonic (NA) ASK Kodansha (JP) | 1994 | - | 1994
Sesame Street: Numbers | Viridis Corporation | EA Kids (NA) Electronic Arts Victor (JP) | 1994 | - | 1995
Sewer Shark | Digital Pictures | Hasbro (NA) Virgin Interactive (PAL) | 1994 | 1994 | -
Shadow: War of Succession | Tribeca Digital Studios | Tribeca Digital Studios (NA) T&E Soft (JP) | 1994 | - | 1994
Shanghai: Triple-Threat | Activision, Success | Activision (NA) Electronic Arts Victor (JP) | 1995 | - | 1994
Shelley Duvall's It's a Bird's Life | Sanctuary Woods | Sanctuary Woods | 1994 | - | -
Shock Wave | The Advanced Technology Group | Electronic Arts Electronic Arts Victor (JP) | 1994 | 1994 | 1994
Shock Wave 2: Beyond the Gate | Advanced Technology Group 2 | Electronic Arts Studios (NA) Electronic Arts (PAL) | 1995 | 1996 | -
Shock Wave: Operation JumpGate (expansion) | The Advanced Technology Group | Electronic Arts Electronic Arts Victor (JP) | 1994 | 1995 | 1995
Short Warp | Warp | Warp | - | - | 1996
Slam 'N Jam '95 | Left Field Productions | Crystal Dynamics (NA) BMG Interactive | 1995 | 1995 | 1995
Snow Job | iXL Interactive Excellence | The 3DO Company | 1996 | 1996 | -
Soccer Kid | Team17 | The 3DO Company | 1994 | 1994 | 1994
Sotsugyō II: Neo Generation Special | Fill-in-Cafe | Shar Rock | - | - | 1995
Sotsugyō: Graduation Final | Fupac | Shar Rock | - | - | 1994
Space Ace | ReadySoft | ReadySoft | 1995 | - | -
Space Hulk: Vengeance of the Blood Angels | Key Game | Electronic Arts Electronic Arts Victor (JP) | 1995 | 1995 | 1995
Space Pirates | American Laser Games | American Laser Games | 1995 | - | -
Space Shuttle | Amazing Media | The Software Toolworks | 1994 | 1994 | -
Star Control II | Toys For Bob | Crystal Dynamics (NA) BMG Interactive | 1994 | 1994 | 1995
Star Fighter | Krisalis Software | The 3DO Company | 1995 | 1995 | -
Star Wars: Rebel Assault | LucasArts | LucasArts (NA) Electronic Arts (PAL) Electronic Arts Victor (JP) | 1994 | 1994 | 1995
Starblade | HighTech Lab. Japan | Panasonic Namco (JP) | 1994 | 1994 | 1994
Stellar 7: Draxon's Revenge | Dynamix | Dynamix (NA) T&E Soft (JP) | 1994 | - | 1994
Strahl | Media Entertainment | Panasonic (NA) Media Entertainment (JP) | 1995 | - | 1994
Striker: World Cup Special | Rage Software | Coconuts Japan Entertainment JP Panasonic PAL | - | 1995 | 1995
Super Real Mahjong P V | SETA Corporation | SETA Corporation | - | - | 1995
Super Real Mahjong PIV + Aishō Shindan | Opus Studio | SETA Corporation | - | - | 1995
Super Street Fighter II Turbo | Capcom | Panasonic Capcom JP | 1994 | 1994 | 1994
Super Wing Commander | Origin Systems | Origin Systems (NA) Electronic Arts (PAL) Electronic Arts Victor (JP) | 1994 | 1994 | 1994
Supreme Warrior | Digital Pictures | Acclaim Distribution (NA) Acclaim Japan (JP) | 1994 | - | 1994
Syndicate | Bullfrog Productions | Electronic Arts Electronic Arts Victor (JP) | 1995 | 1995 | 1995
Taiketsu! Rooms | Sanyei Shobou | Sanyo | - | - | 1995
Tanjō: Debut Pure | Fill-in-Cafe | Shar Rock | - | - | 1996
Terasawa Takeichi no Takeru | Fun Project | Panasonic | - | - | 1994
Theatre Wars: Goraku no Dendō | Scitron & Art | Hamlet | - | - | 1994
Theme Park | Bullfrog Productions | Electronic Arts Electronic Arts Victor (JP) | 1995 | 1995 | 1995
Toki o Koeta Tegami: The Letter That Over Came Time | Thinking Rabbit | Panasonic | - | - | 1994
Tokimeki Mahjong Paradise Special | Sonnet Computer Entertainment | Sonnet Computer Entertainment | - | - | 1996
Total Eclipse | Crystal Dynamics | Crystal Dynamics Byse, Inc. (JP) | 1994 | 1994 | 1994
Totsugeki Kikan (Karakuri) Megadasu!! | Warp | Warp | - | - | 1994
The Tower | OpenBook Co., Ltd. | OpenBook Co., Ltd. | - | - | 1996
Trip'd | Warp | Panasonic Sanei Shobo Publishing (JP) | 1995 | 1995 | 1994
Twinkle Knights | Axes Art Amuse | Intarus | - | - | 1995
Twisted: The Game Show | Studio 3DO | Electronic Arts Electronic Arts Victor (JP) | 1994 | 1994 | 1994
Uchuu Seibutsu Flopon-kun | WARP | Sanyei Shobou | - | - | 1994
Ultraman Powered | Tose | Bandai | - | - | 1994
V-Goal Soccer '96 | Tecmo | Tecmo | - | - | 1996
Virtual Cameraman Part 1: Sawada Naomi and Juri Anna | Naxat Soft | Trans-Pegasus Limited | - | - | 1995
Virtual Cameraman Part 2: Kawai Natsumi and Tachihara Kimi | Naxat Soft | Trans-Pegasus Limited | - | - | 1995
Virtual Cameraman Part 3: Sugimoto Yumika | Naxat Soft | Trans-Pegasus Limited | - | - | 1995
Virtual Cameraman Part 4: Fujitani Shiori | Naxat Soft | Trans-Pegasus Limited | - | - | 1995
Virtual Cameraman Part 5: Ando Ari | Naxat Soft | Trans-Pegasus Limited | - | - | 1995
Virtuoso | MotiveTime Ltd. | Data East (NA) Elite Systems (PAL) Imagineer (JP) | 1995 | 1995 | 1995
VR Stalker | Morpheus Interactive | American Laser Games | 1994 | 1995 | -
Waialae Country Club | T&E Soft | Panasonic(NA) T&E Soft (JP) | 1994 | 1994 | 1994
Way of the Warrior | Naughty Dog | Universal Interactive Studios | 1994 | 1994 | 1995
Who Shot Johnny Rock? | American Laser Games | American Laser Games | 1994 | - | -
Wicked 18 | T&E Soft | Panasonic | 1995 | - | 1995
Wing Commander III: Heart of the Tiger | Origin Systems | Origin Systems | 1995 | 1995 | -
Winning Post | Koei | Koei | - | - | 1994
Wolfenstein 3D | Logicware | Interplay Productions Electronic Arts Victor (JP) | 1995 | - | 1995
World Cup Golf: Hyatt Dorado Beach | Arc Developments | U.S. Gold | 1994 | 1994 | -
The Yakyūken Special | Societa | Societa Daikanyama | - | - | 1994
Yamamura Misa Suspense: Kyōto Anba Sansō Satsujin Jiken | Tose | Pack-In-Video | - | - | 1994
Yū Yū Hakusho | Hudson Soft, Tose | Tomy | - | - | 1994
Zhadnost: The People's Party | Studio 3DO | The 3DO Company | 1995 | 1995 | -
"""


DAT_URL = ('https://raw.githubusercontent.com/libretro/libretro-database/master/'
           'metadat/redump/The%203DO%20Company%20-%203DO.dat')


def fetch_dat():
    """the Redump 3DO dat from libretro-database, to a temp file, when --dat isn't given."""
    import tempfile
    req = urllib.request.Request(DAT_URL, headers={'User-Agent': AGENT})
    with urllib.request.urlopen(req, timeout=30) as r:
        data = r.read()
    fd, path = tempfile.mkstemp(suffix='.dat')
    import os as _os
    _os.write(fd, data)
    _os.close(fd)
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dat', help='the libretro-database 3DO Redump .dat (fetched from libretro if omitted)')
    ap.add_argument('--out', default='3do_meta.json')
    ap.add_argument('--list-page', default='List of 3DO games')
    ap.add_argument('--list-cache', help='read the table from a TITLE|DEV|PUB|NA|PAL|JP file instead of the live page')
    ap.add_argument('--no-infobox', action='store_true', help='skip the per-game genre/mode fetches')
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()

    dat = a.dat or fetch_dat()
    redump = load_redump(dat)
    sys.stderr.write('%d Redump names\n' % len(redump))
    if a.list_cache:
        rows = parse_list_cache(a.list_cache)
    else:
        rows = parse_list_table(wp_wikitext(a.list_page))
        if len(rows) < 50:      # the live page layout changed under us: fall back to the built-in list
            sys.stderr.write('live list parse found only %d rows; using the built-in game list\n' % len(rows))
            rows = parse_list_lines(EMBEDDED_LIST.strip('\n').splitlines())
    sys.stderr.write('%d table rows\n' % len(rows))
    data = build(redump, rows, not a.no_infobox, a.verbose)
    with open(a.out, 'w', encoding='utf-8') as f:
        json.dump({'_license': 'CC BY-SA 4.0, derived from Wikipedia (List of 3DO games + article infoboxes)',
                   'games': data}, f, ensure_ascii=False, indent=0, sort_keys=True)
    sys.stderr.write('wrote %s (%d games)\n' % (a.out, len(data)))


if __name__ == '__main__':
    main()
