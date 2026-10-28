#!/usr/bin/env python3
"""covermaker.py -- the FZ-1 ODE Cover Maker, for any computer with Python (no browser needed).

Does what the Cover Maker web page (getart.html) does, for people whose browser can't run it (Firefox, Safari):

  python3 covermaker.py                      asks for the card, shows the plan, asks before writing anything
  python3 covermaker.py /Volumes/FZ1         the same, for that card (or a folder on it)
  python3 covermaker.py E:\\ --yes            no questions: make everything

For every game on the card, as the menu sees it, that has no cover yet:
  1. a picture of your own beside it (<game>.jpg/.png; in a game's own folder also <folder>.jpg/.png; for a game
     file among others, in _art/), else
  2. a picture from libretro's thumbnail collection (the one RetroArch uses), matched by name: the kind you choose
     (--picture box|screenshot|title, default box art), then up to two fallbacks (--then, --then2; "none" to stop),
     default box art, then in-game screenshot, then title screen.
Each picture is shrunk whole to fit 88x122 (the menu's info pane), lightly sharpened, reduced to its own 256
colours and written as the menu's .img: <game folder>/_cover.img, or _art/<game name>.img beside a game file.

It also writes each folder's index (_odemenu.idx) exactly as the menu would, so folders open at once on the
console (--no-index to skip), and, for a whole card, marks the console's first-start scan as done. An index the
console already made is left alone.

Games with no picture are listed at the end; --fix goes through them one by one: search libretro's collection
under another name, give a picture file from your computer, or open an image search in your web browser.

Options: --redo (remake covers that exist), --dry-run (show the plan, write nothing), --fix, --yes.
Needs Python 3.8+ and Pillow (pip install pillow). Downloads are cached (~/.cache/fz1-covermaker).

The pictures belong to the games' publishers; libretro's collection states no licence. Fine for your own card;
please don't share them.
"""
import argparse
import io
import json
import os
import re
import struct
import sys
import time
import urllib.parse
import urllib.request
import webbrowser

try:
    from PIL import Image
except ImportError:
    sys.exit('This needs Pillow, the Python picture library. Install it with:\n\n'
             '    python3 -m pip install pillow\n\n(on Windows: py -m pip install pillow) and run this again.')

REPO = 'libretro-thumbnails/The_3DO_Company_-_3DO'
TREE = os.environ.get('FZ1_TREE_URL', 'https://api.github.com/repos/' + REPO + '/git/trees/master?recursive=1')
RAW = os.environ.get('FZ1_RAW_URL', 'https://raw.githubusercontent.com/' + REPO + '/master/')
AGENT = 'fz1-ode-covermaker/1.0'
KINDS = ('Named_Boxarts', 'Named_Snaps', 'Named_Titles')
KIND_ARG = {'box': 'Named_Boxarts', 'screenshot': 'Named_Snaps', 'title': 'Named_Titles', 'none': None}
KIND_LABEL = {'Named_Boxarts': 'box art', 'Named_Snaps': 'screenshot', 'Named_Titles': 'title screen'}
W, H = 88, 122                     # the most a cover can be: the menu's info pane
BAD = '&*/:`<>?\\|"'               # characters libretro's names replace with "_"


# ------------------------------------------------------------------ names (as getart.html)

def stem(name):
    return re.sub(r'\.(iso|cue|bin)$', '', name, flags=re.I)


def server_name(n):
    return ''.join('_' if c in BAD else c for c in n)


def title_key(n):
    t = re.sub(r'[(\[][^)\]]*[)\]]', ' ', n).replace('_', ' ').lower()
    t = re.sub(r',\s*the\b', ' ', t)
    t = re.sub(r'^\s*the\s+', ' ', t)
    t = re.sub(r'[^a-z0-9]+', ' ', t)
    return ' '.join(t.split())


REGIONS = ('usa', 'europe', 'japan', 'world', 'korea', 'germany', 'france', 'spain', 'italy', 'brazil', 'asia')


def regions(n):
    out = set()
    for m in re.findall(r'[(\[]([^)\]]*)[)\]]', n):
        for p in re.split(r'[,\s]+', m.lower()):
            if p in REGIONS:
                out.add(p)
    return out


def disc(n):
    m = re.search(r'\b(?:disc|disk|cd)\s*(\d+)', n, re.I)
    return int(m.group(1)) if m else 0


def best_match(server, kind, game):
    names, index = server['names'][kind], server['index'][kind]
    exact = server_name(game)
    if exact in names:
        return exact
    cands = index.get(title_key(game), [])
    if not cands:
        return None
    wr, wd = regions(game), disc(game)

    def score(c):
        r = regions(c)
        bad = 2 if re.search(r'\((proto|beta|demo)', c, re.I) else 0
        return len(r & wr) * 4 - len(r - wr) + (3 if disc(c) == wd else 0) - bad
    return sorted(cands, key=lambda c: (-score(c), len(c)))[0]


# ------------------------------------------------------------------ the menu's folder index (as src/dirlist.c)
# The firmware numbers a folder's entries in the order they sit on the card, skipping what its filter hides. The
# operating system lists a FAT32/exFAT folder in that same order (os.scandir does not sort). Nothing promises it;
# the menu checks every index against the firmware and lists the folder again if it disagrees.

IDX = '_odemenu.idx'
IDX_MAGIC = 0x4f4d4333
NAME_MAX, GF_MAX_SEEN, IDX_MAX = 127, 32, 4096
GT_GAME, GT_PLAIN = 1, 2
SYS_NAMES = ('system volume information', '$recycle.bin', 'recycler', 'thumbs.db', 'desktop.ini',
             '$windows.~bt', '$windows.~ws', '$sysreset', 'found.000')


def ext3(b):
    return b[-3:].decode('latin-1').lower() if len(b) >= 4 and b[-4] == 46 else None


def is_game_bytes(b):
    return len(b) >= 5 and ext3(b) in ('iso', 'cue')


class Entry:
    def __init__(self, name, is_dir, flags):
        self.name, self.is_dir, self.flags = name, is_dir, flags
        self.bytes = name.encode('utf-8')
        self.gtype, self.inner, self.inner_toc = 0, b'', 0


def fw_entry(name, is_dir):
    """one entry as the firmware lists it (flags as in a D1 record), or None if the firmware skips it"""
    if name[:1] in ('.', '_') or name.lower() in SYS_NAMES:
        return None
    x = None if is_dir else ext3(name.encode('utf-8'))
    if x is not None and x not in ('iso', 'cue', 'nvm', 'ode'):
        return None
    return Entry(name, is_dir, 2 if is_dir else 9 if x == 'nvm' else 0x11 if x == 'ode' else 1)


def detect_game(listed):
    """the folder's one game, if the menu shows the folder as that game"""
    seen, game = 0, None
    for e in listed:
        seen += 1
        if e.is_dir or seen > GF_MAX_SEEN:
            return None
        if not (e.flags & 0x18) and is_game_bytes(e.bytes):
            if game:
                return None
            game = e
    return game


def set_folder(e, sub):
    g = detect_game(sub)
    if g:
        e.gtype, e.inner, e.inner_toc = GT_GAME, g.bytes[:NAME_MAX], sub.index(g) + 1
    else:
        e.gtype = GT_PLAIN


def name_compare(a, b):
    """case-insensitive natural order on the UTF-8 bytes (name_compare)"""
    def at(s, i):
        return s[i] if i < len(s) else 0

    def dig(c):
        return 48 <= c <= 57

    def low(c):
        return c + 32 if 65 <= c <= 90 else c
    p = q = 0
    while True:
        if dig(at(a, p)) and dig(at(b, q)):
            while at(a, p) == 48 and dig(at(a, p + 1)):
                p += 1
            while at(b, q) == 48 and dig(at(b, q + 1)):
                q += 1
            la = lb = 0
            while dig(at(a, p + la)):
                la += 1
            while dig(at(b, q + lb)):
                lb += 1
            if la != lb:
                return la - lb
            for i in range(la):
                if at(a, p + i) != at(b, q + i):
                    return at(a, p + i) - at(b, q + i)
            p += la
            q += lb
            continue
        ca, cb = low(at(a, p)), low(at(b, q))
        if ca != cb:
            return ca - cb
        if ca == 0:
            return 0
        p += 1
        q += 1


def crc32(b):
    import zlib
    return zlib.crc32(b) & 0xffffffff


def build_index(listed):
    if not listed or len(listed) > IDX_MAX:
        return None
    import functools
    ents = [(e, i + 1, e.bytes[:NAME_MAX]) for i, e in enumerate(listed)]

    def plain(x):
        return 1 if x[0].is_dir and x[0].gtype != GT_GAME else 0

    def cmp(x, y):
        return (plain(y) - plain(x)) or name_compare(x[2], y[2])
    ents.sort(key=functools.cmp_to_key(cmp))                   # stable, as the menu's merge sort
    body = bytearray()
    for e, toc, nb in ents:
        il = len(e.inner) if e.gtype == GT_GAME else 0
        it = e.inner_toc if e.gtype == GT_GAME else 0
        body += bytes([e.flags, len(nb), toc >> 8, toc & 255, e.gtype, il, it >> 8, it & 255]) + nb
        if il:
            body += e.inner
    return struct.pack('>IIII', IDX_MAGIC, len(ents), len(body), crc32(bytes(body))) + bytes(body)


def index_names(buf):
    if len(buf) < 16:
        return None
    magic, count, body, crc = struct.unpack('>IIII', buf[:16])
    if magic != IDX_MAGIC or len(buf) < 16 + body or crc32(buf[16:16 + body]) != crc:
        return None
    names, p = [], 16
    while p < 16 + body:
        ln, il = buf[p + 1], buf[p + 5]
        names.append(bytes(buf[p + 8:p + 8 + ln]))
        p += 8 + ln + il
    return sorted(names) if len(names) == count else None


# ------------------------------------------------------------------ the card, as the menu sees it

def find_pic(folder, files, bases):
    for b in bases:
        for e in ('.jpg', '.png', '.jpeg'):
            f = files.get((b + e).lower())
            if f:
                return os.path.join(folder, f)
    return None


def walk(folder, is_root, rel, jobs, idx):
    """one folder and everything below it: games to jobs (covers), its index to idx; returns its listing"""
    files, listed, paths = {}, [], {}
    try:
        it = list(os.scandir(folder))                          # in the order they sit on the card
    except OSError as e:
        print('  (could not read %s: %s)' % (folder, e))
        return []
    for d in it:
        try:
            is_dir = d.is_dir()
        except OSError:
            continue
        if not is_dir:
            files[d.name.lower()] = d.name
        e = fw_entry(d.name, is_dir)
        if e:
            listed.append(e)
            paths[id(e)] = d.path
    at = len(jobs)
    subs = sorted((e for e in listed if e.is_dir), key=lambda e: e.bytes.lower())
    for e in subs:
        set_folder(e, walk(paths[id(e)], False, (rel + '/' if rel else '') + e.name, jobs, idx))
    mine, name = [], os.path.basename(os.path.normpath(folder))
    game = None if is_root else detect_game(listed)
    if game:
        g = stem(game.name)
        dst = os.path.join(folder, '_cover.img')
        mine.append(dict(game=g, folder=name, label=rel or name, dst=dst, have=os.path.exists(dst),
                         own=find_pic(folder, files, [g, name])))
    else:
        games = sorted(e.name for e in listed if not e.is_dir and not (e.flags & 0x18) and is_game_bytes(e.bytes)
                       and not (is_root and e.name.lower() == 'boot.iso'))
        art = os.path.join(folder, '_art')
        af = {f.lower(): f for f in os.listdir(art)} if games and os.path.isdir(art) else {}
        for f in games:
            g = stem(f)
            dst = os.path.join(art, g + '.img')
            mine.append(dict(game=g, folder=None, label=(rel + '/' if rel else '') + g, dst=dst,
                             have=(g + '.img').lower() in af, own=find_pic(art, af, [g]) if af else None))
    jobs[at:at] = mine
    b = build_index(listed)
    if b:
        p = os.path.join(folder, IDX)
        state = 'new'
        if os.path.exists(p):
            old = open(p, 'rb').read()
            if old == b:
                state = 'same'
            elif index_names(old) is not None and index_names(old) == index_names(b):
                state = 'kept'                                 # the console's, same files in another order
            else:
                state = 'changed'
        idx.append(dict(path=p, rel=rel or 'the card', bytes=b, state=state))
    return listed


# ------------------------------------------------------------------ libretro's pictures

def cache_dir():
    d = os.path.join(os.path.expanduser('~'), '.cache', 'fz1-covermaker')
    os.makedirs(d, exist_ok=True)
    return d


def fetch(url, timeout=30):
    req = urllib.request.Request(url, headers={'User-Agent': AGENT})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()
    except urllib.error.URLError as e:
        if 'CERTIFICATE_VERIFY_FAILED' in str(e):
            sys.exit('Python could not check the website\'s certificate. On a Mac, run "Install Certificates.command"\n'
                     'in your Python folder (Applications > Python 3.x) once, then run this again.')
        raise


def load_server():
    path = os.path.join(cache_dir(), 'tree.json')
    if os.path.exists(path) and time.time() - os.path.getmtime(path) < 86400 and 'FZ1_TREE_URL' not in os.environ:
        tree = json.load(open(path, encoding='utf-8'))
    else:
        try:
            tree = json.loads(fetch(TREE).decode('utf-8'))
        except urllib.error.HTTPError as e:
            sys.exit('GitHub said %d%s' % (e.code, ' (too many requests: try again in an hour)' if e.code == 403 else ''))
        json.dump(tree, open(path, 'w', encoding='utf-8'))
    s = dict(names={k: set() for k in KINDS}, index={k: {} for k in KINDS}, links=set(), all=[])
    for t in tree.get('tree', []):
        m = re.match(r'^(Named_Boxarts|Named_Snaps|Named_Titles)/(.+)\.png$', t['path'], re.I)
        if not m:
            continue
        k = next(x for x in KINDS if x.lower() == m.group(1).lower())
        n = m.group(2)
        s['names'][k].add(n)
        s['index'][k].setdefault(title_key(n), []).append(n)
        if t.get('mode') == '120000':
            s['links'].add(t['path'])                          # a link: the file holds the real picture's name
    for k in KINDS:
        for n in sorted(s['names'][k]):
            s['all'].append((k, n, ' ' + title_key(n) + ' '))
    return s


def download(server, path):
    cached = os.path.join(cache_dir(), path)
    if os.path.exists(cached):
        return open(cached, 'rb').read()
    url = RAW + '/'.join(urllib.parse.quote(p) for p in path.split('/'))
    data = fetch(url)
    if path in server['links']:
        target = data.decode('utf-8', 'replace').strip()
        data = fetch(RAW + '/'.join(urllib.parse.quote(p) for p in (path.split('/')[0] + '/' + target).split('/')))
    os.makedirs(os.path.dirname(cached), exist_ok=True)
    open(cached, 'wb').write(data)
    time.sleep(0.2)
    return data


def search(server, q, order):
    words = title_key(q).split()
    if not words:
        return []
    hits = [h for h in server['all'] if all(w in h[2] for w in words)]

    def rank(h):
        whole = sum(1 for w in words if ' ' + w + ' ' in h[2])
        return (-whole, order.index(h[0]) if h[0] in order else 9, len(h[1]))
    return sorted(hits, key=rank)[:10]


# ------------------------------------------------------------------ picture -> .img (as the console and the web page)

def fit_size(sw, sh):
    if sw * H > sh * W:
        return W, max(1, (sh * W + sw // 2) // sw)
    return max(1, (sw * H + sh // 2) // sh), H


def cover(data):
    """the picture shrunk whole to fit W x H, lightly sharpened, as 15-bit pixels: (pixels, w, h)"""
    im = Image.open(io.BytesIO(data))
    if im.mode in ('RGBA', 'LA', 'P'):
        im = im.convert('RGBA')
        bg = Image.new('RGBA', im.size, (0, 0, 0, 255))
        im = Image.alpha_composite(bg, im)
    im = im.convert('RGB')
    w, h = fit_size(im.width, im.height)
    im = im.resize((w, h), Image.BOX)                          # every source pixel into the one it falls in
    p = im.load()
    out = []
    for y in range(h):
        for x in range(w):
            c = p[x, y]
            n = (p[x, max(y - 1, 0)], p[x, min(y + 1, h - 1)], p[max(x - 1, 0), y], p[min(x + 1, w - 1), y])
            v = [max(0, min(255, round(c[i] + (4 * c[i] - sum(q[i] for q in n)) / 8))) >> 3 for i in range(3)]
            out.append((v[0] << 10) | (v[1] << 5) | v[2])
    return out, w, h


def encode8(px, w, h):
    """the picture's own 256 colours by median cut (as art_encode8 in the menu), as an OAR8 .img"""
    count = {}
    cols = []
    for c in px:
        if c not in count:
            count[c] = 0
            cols.append(c)
        count[c] += 1

    def ch(c, k):
        return (c >> 10) & 31 if k == 0 else (c >> 5) & 31 if k == 1 else c & 31
    boxes = [[0, len(cols), len(px)]]
    while len(boxes) < 256 and len(boxes) < len(cols):
        b = None
        for x in boxes:
            if x[1] - x[0] > 1 and (b is None or x[2] > b[2]):
                b = x
        if b is None:
            break
        part = cols[b[0]:b[1]]
        lo = [min(ch(c, k) for c in part) for k in range(3)]
        hi = [max(ch(c, k) for c in part) for k in range(3)]
        wch = 0
        for k in (1, 2):
            if hi[k] - lo[k] > hi[wch] - lo[wch]:
                wch = k
        hist = [0] * 32
        for c in part:
            hist[ch(c, wch)] += count[c]
        acc, cut = 0, lo[wch]
        while cut < hi[wch]:
            acc += hist[cut]
            if acc >= b[2] / 2:
                break
            cut += 1
        if cut >= hi[wch]:
            cut = hi[wch] - 1
        a = [c for c in part if ch(c, wch) <= cut]
        z = [c for c in part if ch(c, wch) > cut]
        cols[b[0]:b[1]] = a + z
        nb = [b[0] + len(a), b[1], sum(count[c] for c in z)]
        b[1] = nb[0]
        b[2] -= nb[2]
        boxes.append(nb)
    out = bytearray(b'OAR8' + struct.pack('>HHH', w, h, len(boxes)))
    cmap = {}
    for i, (lo_, hi_, _) in enumerate(boxes):
        s = [0, 0, 0]
        ws = 0
        for c in cols[lo_:hi_]:
            m = count[c]
            for k in range(3):
                s[k] += ch(c, k) * m
            ws += m
            cmap[c] = i
        ws = ws or 1
        out += struct.pack('>H', (round(s[0] / ws) << 10) | (round(s[1] / ws) << 5) | round(s[2] / ws))
    out += bytes(cmap[c] for c in px)
    return bytes(out)


def write_cover(job, data):
    px, w, h = cover(data)
    os.makedirs(os.path.dirname(job['dst']), exist_ok=True)
    with open(job['dst'], 'wb') as f:
        f.write(encode8(px, w, h))
    return w, h


def mark_scanned(root):
    p = os.path.join(root, '_odemenu.opt')
    text = open(p, encoding='latin-1').read() if os.path.exists(p) else ''
    if not text.startswith('OMO1\n'):
        text = 'OMO1\n'
    if re.search(r'^scanned=1\r?$', text, re.M):
        return
    if re.search(r'^scanned=', text, re.M):
        text = re.sub(r'^scanned=.*$', 'scanned=1', text, count=1, flags=re.M)
    else:
        text += ('' if text.endswith('\n') else '\n') + 'scanned=1\n'
    with open(p, 'w', encoding='latin-1', newline='') as f:
        f.write(text)


# ------------------------------------------------------------------ the run

def ask(q, default='y'):
    try:
        a = input(q).strip().lower()
    except EOFError:
        return default == 'y'
    return (a or default).startswith('y')


def fix(server, order, missing):
    print('\nGames with no picture. For each: type words to search libretro\'s pictures, a picture file\'s path')
    print('(you can drag the file onto this window), "w" for an image search in your browser, Enter to skip,')
    print('"q" to stop.\n')
    for j in missing:
        title = re.sub(r'\s+', ' ', re.sub(r'[(\[][^)\]]*[)\]]', ' ', j['game'])).strip()
        print('== %s' % j['label'])
        hits = search(server, title, order)
        while True:
            for i, (k, n, _) in enumerate(hits):
                print('   %d. %s  [%s]' % (i + 1, n, KIND_LABEL[k]))
            try:
                a = input('   number, words, file, w, Enter, q > ').strip().strip('"').strip("'")
            except EOFError:
                return
            if a == '':
                break
            if a.lower() == 'q':
                return
            if a.lower() == 'w':
                webbrowser.open('https://www.google.com/search?tbm=isch&q=' + urllib.parse.quote(title + ' 3DO'))
                continue
            if a.isdigit() and 1 <= int(a) <= len(hits):
                k, n, _ = hits[int(a) - 1]
                try:
                    w, h = write_cover(j, download(server, k + '/' + n + '.png'))
                    print('   cover written (%dx%d) from %s' % (w, h, n))
                    break
                except Exception as e:                         # noqa: BLE001 - say so and let them try another
                    print('   that didn\'t work: %s' % e)
                    continue
            path = os.path.expanduser(a.replace('\\ ', ' '))
            if os.path.isfile(path):
                try:
                    w, h = write_cover(j, open(path, 'rb').read())
                    print('   cover written (%dx%d) from %s' % (w, h, os.path.basename(path)))
                    break
                except Exception as e:                         # noqa: BLE001
                    print('   that didn\'t work: %s' % e)
                    continue
            hits = search(server, a, order)
            if not hits:
                print('   nothing with those words; try fewer, or another name for the game')


def main():
    try:
        sys.stdout.reconfigure(errors='replace')               # game names a Windows console cannot show
    except (AttributeError, ValueError):
        pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('card', nargs='?', help='the SD card (its top folder, with boot.iso), or a folder on it')
    ap.add_argument('--picture', choices=('box', 'screenshot', 'title'), default='box')
    ap.add_argument('--then', dest='then1', choices=tuple(KIND_ARG), default=None,
                    help='if a game has none: (default: the next of box, screenshot, title)')
    ap.add_argument('--then2', choices=tuple(KIND_ARG), default=None, help='and then:')
    ap.add_argument('--redo', action='store_true', help='remake covers that already exist')
    ap.add_argument('--no-index', action='store_true', help='don\'t write folder indexes')
    ap.add_argument('--dry-run', action='store_true', help='show the plan, write nothing')
    ap.add_argument('--fix', action='store_true', help='afterwards, go through the games with no picture')
    ap.add_argument('--yes', '-y', action='store_true', help='don\'t ask before writing')
    a = ap.parse_args()

    card = a.card
    if not card:
        print('FZ-1 ODE Cover Maker\n')
        card = input('Drag the SD card (or its folder) onto this window, or type its path, then press Enter:\n> ')
        card = card.strip().strip('"').strip("'").replace('\\ ', ' ')
    card = os.path.expanduser(card)
    if not os.path.isdir(card):
        sys.exit('Not a folder: %s' % card)

    first = KIND_ARG[a.picture]
    others = [k for k in KINDS if k != first]
    t1 = KIND_ARG[a.then1] if a.then1 else others[0]
    order = [first]
    if t1 and t1 != first:
        order.append(t1)
        rest = [k for k in others if k != t1]
        t2 = KIND_ARG[a.then2] if a.then2 else (rest[0] if rest else None)
        if t2 and t2 not in order:
            order.append(t2)

    whole = os.path.exists(os.path.join(card, 'boot.iso'))
    print('Looking at %s ...' % ('the whole card' if whole else 'the folder "%s" and everything in it' %
                                  os.path.basename(os.path.normpath(card))))
    jobs, idx = [], []
    walk(card, whole, '', jobs, idx)
    print('Reading the list of pictures ...')
    server = load_server()

    n = dict(box=0, dl=0, own=0, have=0, miss=0)
    for j in jobs:
        j['src'] = None
        if j['have'] and not a.redo:
            j['what'] = 'have'
        elif j['own']:
            j['what'] = 'own'
        else:
            for k in order:
                names = [j['game']] + ([j['folder']] if j['folder'] and j['folder'] != j['game'] else [])
                for nm in names:
                    m = best_match(server, k, nm)
                    if m:
                        j['src'], j['kind'] = k + '/' + m + '.png', k
                        break
                if j['src']:
                    break
            j['what'] = 'dl' if j['src'] else 'miss'
        n[j['what']] += 1
    new_idx = [x for x in idx if x['state'] in ('new', 'changed')] if not a.no_index else []

    print('\nPicture: %s' % ', then '.join(KIND_LABEL[k] for k in order))
    print('  %4d pictures to download' % n['dl'])
    print('  %4d from your own pictures' % n['own'])
    print('  %4d already have a cover' % n['have'])
    print('  %4d no picture found' % n['miss'])
    print('  %4d folder indexes to write' % len(new_idx))
    if a.dry_run:
        print()
        for j in jobs:
            src = j['src'].split('/', 1)[1][:-4] if j['src'] else ''
            tag = {'dl': KIND_LABEL.get(j.get('kind'), ''), 'own': 'your picture', 'have': 'has cover',
                   'miss': 'NOT FOUND'}[j['what']]
            print('  %-13s %s%s' % (tag, j['label'], '  <-  ' + src if src and src != j['game'] else ''))
        return
    todo = [j for j in jobs if j['what'] in ('dl', 'own')]
    if not todo and not new_idx and not (a.fix and n['miss']):
        print('\nNothing to do.')
        return
    if not a.yes and not ask('\nMake them now? [Y/n] '):
        return

    made, failed = 0, []
    for i, j in enumerate(todo):
        print('  %d of %d: %s' % (i + 1, len(todo), j['label']))
        try:
            data = open(j['own'], 'rb').read() if j['what'] == 'own' else download(server, j['src'])
            write_cover(j, data)
            made += 1
        except Exception as e:                                 # noqa: BLE001 - report and go on
            j['what'] = 'miss'
            failed.append('%s (%s)' % (j['label'], e))
    idx_made = idx_failed = 0
    for x in new_idx:                  # last: the covers above only add names starting with "_", which it skips
        try:
            with open(x['path'], 'wb') as f:
                f.write(x['bytes'])
            idx_made += 1
        except OSError:
            idx_failed += 1
    if whole and not a.no_index and not idx_failed:
        try:
            mark_scanned(card)
        except OSError:
            pass

    miss = [j for j in jobs if j['what'] == 'miss']
    print('\n%d cover%s made.%s%s' % (made, '' if made == 1 else 's', ' %d failed.' % len(failed) if failed else '',
                                      ' %d game%s no picture yet.' % (len(miss), ' has' if len(miss) == 1 else 's have')
                                      if miss else ''))
    for f in failed:
        print('  failed: ' + f)
    if not a.no_index:
        same = sum(1 for x in idx if x['state'] == 'same')
        kept = [x for x in idx if x['state'] == 'kept']
        print('Folder indexes: %d written%s%s.' % (idx_made, ', %d failed' % idx_failed if idx_failed else '',
                                                   ', %d already right' % same if same else ''))
        if kept:
            print('%d index%s the console made list%s the same files in another order, so %s left alone: %s' %
                  (len(kept), '' if len(kept) == 1 else 'es', 's' if len(kept) == 1 else '',
                   'it was' if len(kept) == 1 else 'they were', ', '.join(x['rel'] for x in kept[:3]) +
                   (', ...' if len(kept) > 3 else '')))
    if miss and a.fix:
        fix(server, order, miss)
    elif miss:
        print('No picture for:')
        for j in miss:
            print('  ' + j['label'])
        print('Run again with --fix to give them one (search under another name, a file, or a web search).')
    print('\nEject the card safely and put it back in the ODE.')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit('\nStopped.')
