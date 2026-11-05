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


def year_of(s):
    m = re.search(r'\b(19|20)\d\d\b', s)
    return int(m.group(0)) if m else None


def parse_list_table(wikitext):
    """the 'List of 3DO games' sortable wikitable -> rows {title, dev, pub, na, pal, jp}."""
    start = wikitext.find('{|')
    body = wikitext[start:wikitext.find('|}', start)] if start >= 0 else ''
    rows = []
    for chunk in re.split(r'\n\|-', body):
        cells = []
        for raw in re.split(r'\n\s*[|!]', chunk):
            raw = raw.strip()
            if not raw or raw.startswith(('{|', 'class=', 'style=', 'scope')):
                continue
            # a cell may carry attributes before a single | ; keep what follows the last leading pipe
            cells.append(clean_cell(raw.split('||')[-1] if '||' in raw else raw))
        cells = [c for c in cells if c]
        if len(cells) >= 2 and year_of(' '.join(cells)) or (len(cells) >= 4):
            # heuristic columns: title, developer, publisher, then year columns (NA, PAL, JP)
            title, dev, pub = (cells + ['', '', ''])[:3]
            yrs = [year_of(c) for c in cells[3:6]] + [None, None, None]
            rows.append({'title': title, 'dev': dev, 'pub': pub,
                         'na': yrs[0], 'pal': yrs[1], 'jp': yrs[2]})
    return rows


def parse_list_cache(path):
    rows = []
    for line in open(path, encoding='utf-8'):
        p = [x.strip() for x in line.split('|')]
        if len(p) < 6:
            continue
        rows.append({'title': p[0], 'dev': p[1], 'pub': p[2],
                     'na': year_of(p[3]), 'pal': year_of(p[4]), 'jp': year_of(p[5])})
    return rows


def get_infobox(title, cache):
    if title in cache:
        return cache[title]
    out = {'genre': '', 'mode': ''}
    try:
        wt = wp_get({'action': 'parse', 'page': title, 'prop': 'wikitext', 'section': '0'})
        wt = wt['parse']['wikitext']['*']
        g = re.search(r'\|\s*genre\s*=\s*(.+)', wt)
        m = re.search(r'\|\s*modes?\s*=\s*(.+)', wt)
        if g:
            out['genre'] = clean_cell(g.group(1).split('\n')[0]).rstrip(',')
        if m:
            out['mode'] = clean_cell(m.group(1).split('\n')[0]).rstrip(',')
    except Exception as e:
        sys.stderr.write('  infobox miss %s: %s\n' % (title, e))
    cache[title] = out
    time.sleep(0.1)
    return out


# ---- region-aware field resolution ----
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
    lead = re.split(r'\((?:NA|PAL|JP|EU|US|Japan|Europe)\)', value)[0]
    return lead.strip(' ,') or value.strip()


def build(redump, rows, want_infobox, verbose):
    index = {}
    for r in rows:
        for k in alt_keys(r['title']):
            index.setdefault(k, r)
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
        pub = region_field(row['pub'], slot)
        dev = region_field(row['dev'], slot)
        ib = get_infobox(row['title'], ib_cache) if want_infobox else {'genre': '', 'mode': ''}
        e = out.setdefault(key, {'regions': []})
        e.update({'year': year, 'publisher': pub, 'developer': dev,
                  'genre': ib['genre'], 'mode': ib['mode'], 'wikipedia': row['title']})
        if g['region'] not in e['regions']:
            e['regions'].append(g['region'])
        if verbose:
            sys.stderr.write('  %-40s -> %s %s / %s\n' % (g['name'], year, pub, ib['genre']))
    sys.stderr.write('matched %d / %d redump names\n' % (matched, len(redump)))
    return out


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
    sys.stderr.write('%d table rows\n' % len(rows))
    data = build(redump, rows, not a.no_infobox, a.verbose)
    with open(a.out, 'w', encoding='utf-8') as f:
        json.dump({'_license': 'CC BY-SA 4.0, derived from Wikipedia (List of 3DO games + article infoboxes)',
                   'games': data}, f, ensure_ascii=False, indent=0, sort_keys=True)
    sys.stderr.write('wrote %s (%d games)\n' % (a.out, len(data)))


if __name__ == '__main__':
    main()
