#!/usr/bin/env python3
"""mkart.py -- make the info pane's pictures for the FZ-1 ODE menu.

  mkart.py --card <SD card>     the whole card at once, as the console's card scan would (seconds instead of
                                minutes: the console writes the card slowly). For every game in its own folder
                                with a <game file name>.jpg/.png or <folder name>.jpg/.png beside it, writes
                                <folder>/_cover.img; for every other game with _art/<game name>.jpg/.png, writes
                                _art/<game name>.img. Existing .img files are kept (--force remakes them).


The menu shows <game folder>/_art/<game name>.img beside a game (or <game folder>/_cover.img): the picture
shrunk to fit 88 x 122 with its shape kept (box art: 63 x 122 for a US long box; a 4:3 screenshot: 88 x 66),
with its own 256 colours (15-bit each). "<game name>" is the file name without .iso/.cue/.bin, so
"Doom (USA).iso" uses "_art/Doom (USA).img". tools/covermaker.py (or the web page) downloads box art for a whole card.
(The firmware hides names starting with "_", so the _art folder does not show up in the menu.)

  mkart.py picture.png out.img                 one picture
  mkart.py --folder <game folder> <pictures>   every picture in <pictures> whose name matches a game in
                                               <game folder> (same name, any image extension) goes to
                                               <game folder>/_art/

Needs Python 3 and Pillow (pip install pillow). Game screenshots belong to their publishers: fine for your own
card, not for sharing.
"""
import argparse
import os
import struct
import sys

from PIL import Image

W, H = 88, 122           # the most a cover can be: the info pane
GAME_EXT = ('.iso', '.cue', '.bin')
PIC_EXT = ('.png', '.jpg', '.jpeg', '.bmp', '.gif', '.webp')


def sharpen(im):
    """the console scan's light sharpen: each pixel moves away from its 4 neighbours' average by (4c - n) / 8"""
    w, h = im.size
    p = im.load()
    out = im.copy()
    q = out.load()
    for y in range(h):
        for x in range(w):
            c = p[x, y]
            n = [p[x, max(y - 1, 0)], p[x, min(y + 1, h - 1)], p[max(x - 1, 0), y], p[min(x + 1, w - 1), y]]
            q[x, y] = tuple(max(0, min(255, c[i] + int((4 * c[i] - sum(v[i] for v in n)) / 8))) for i in range(3))
    return out


def fit_size(sw, sh):
    """the size the menu shows a sw x sh picture at (as fit_size in src/artdec.c)"""
    if sw * H > sh * W:
        return W, max(1, (sh * W + sw // 2) // sw)
    return max(1, (sw * H + sh // 2) // sh), H


def convert(src, dst, box=None):
    im = Image.open(src).convert('RGB')
    if box:
        im = im.crop(box)
    out = sharpen(im.resize(fit_size(im.width, im.height), Image.BOX))
    w, h = out.size
    # OAR8: the picture's own 256 colours (as the console's scan writes): "OAR8", w, h, n, n RGB555 colours, w*h
    # one-byte indices. 6.2 KB at 88x66, 8.4 KB for a long box.
    out = out.point(lambda v: (v >> 3) << 3)
    q = out.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[:3 * 256]
    idx = q.tobytes()
    n = max(idx) + 1
    data = bytearray(b'OAR8' + struct.pack('>HHH', w, h, n))
    for i in range(n):
        r, g, b = pal[3 * i:3 * i + 3]
        data += struct.pack('>H', ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3))
    data += idx
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(dst, 'wb') as f:
        f.write(data)


def stem(name):
    base, ext = os.path.splitext(name)
    return base if ext.lower() in GAME_EXT else name


def card_games(root):
    """every game on the card, as the menu sees them: dicts with game (file name without extension), folder (a
    game folder's name, or None), dst (the .img the menu opens) and src (a local .jpg/.png for it, or None)"""
    for d, dirs, files in os.walk(root):
        dirs[:] = sorted(x for x in dirs if not x.startswith(('_', '.')) and x != 'System Volume Information')
        games = sorted(f for f in files if os.path.splitext(f)[1].lower() in ('.iso', '.cue')
                       and not f.startswith(('_', '.')) and not (d == root and f.lower() == 'boot.iso'))
        lower = {f.lower(): f for f in files}

        def pic(base):
            for e in ('.jpg', '.png'):
                if (base + e).lower() in lower:
                    return os.path.join(d, lower[(base + e).lower()])
            return None
        if len(games) == 1 and not dirs and d != root:
            g = stem(games[0])
            yield dict(game=g, folder=os.path.basename(d), dst=os.path.join(d, '_cover.img'),
                       src=pic(g) or pic(os.path.basename(d)))
            continue
        art = os.path.join(d, '_art')
        al = {f.lower(): f for f in os.listdir(art)} if os.path.isdir(art) else {}
        for f in games:
            g = stem(f)
            src = next((os.path.join(art, al[(g + e).lower()]) for e in ('.jpg', '.png') if (g + e).lower() in al), None)
            yield dict(game=g, folder=None, dst=os.path.join(art, g + '.img'), src=src)


def card(root, force):
    made = kept = 0
    for j in card_games(root):
        if not j['src']:
            continue
        if os.path.exists(j['dst']) and not force:
            kept += 1
            continue
        convert(j['src'], j['dst'])
        made += 1
        print('made', j['dst'])
    print('%d covers made, %d kept' % (made, kept))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--card', help='SD card root: make every cover on it')
    ap.add_argument('--force', action='store_true', help='with --card: remake existing .img files')
    ap.add_argument('src', nargs='?')
    ap.add_argument('dst', nargs='?')
    ap.add_argument('--folder', help='game folder: match pictures in SRC to the games in it')
    ap.add_argument('--size', help='WxH: the most a picture can be (must match the menu build)')
    ap.add_argument('--box', help='x0,y0,x1,y1: use only this part of the source picture')
    a = ap.parse_args()
    global W, H
    if a.size:
        W, H = (int(v) for v in a.size.lower().split('x'))
    box = tuple(int(v) for v in a.box.split(',')) if a.box else None
    if a.card:
        card(a.card, a.force)
        return
    if not a.src:
        ap.error('give a picture, --folder or --card')
    if not a.folder:
        if not a.dst:
            ap.error('give an output file, or --folder')
        convert(a.src, a.dst, box)
        return
    games = {stem(n).lower(): stem(n) for n in os.listdir(a.folder)
             if os.path.splitext(n)[1].lower() in GAME_EXT}
    done = 0
    for n in sorted(os.listdir(a.src)):
        base, ext = os.path.splitext(n)
        if ext.lower() not in PIC_EXT:
            continue
        g = games.get(base.lower())
        if g is None:
            print('no game for', n)
            continue
        convert(os.path.join(a.src, n), os.path.join(a.folder, '_art', g + '.img'), box=box)
        done += 1
    print('%d pictures written to %s' % (done, os.path.join(a.folder, '_art')))


if __name__ == '__main__':
    sys.exit(main())
