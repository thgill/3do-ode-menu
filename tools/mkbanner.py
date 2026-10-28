#!/usr/bin/env python3
"""mkbanner.py -- the boot banner (BannerScreen): the logo on the menu's background, in the 3DO APPSCRN format.

    mkbanner.py <logo.png> <out BannerScreen> [--preview out.png]

APPSCRN: 0x01 "APPSCRN", u32 pixel bytes, u16 height, u16 width, u32 0x10000000, u32 0, then 320x240 RGB555 in the
frame buffer's line-pair order, then a 64-byte signature that 3dt pack fills in (RSA_APPSPLASH, APP key).
The 3DO shows it while the disc boots.
"""
import struct
import sys

from PIL import Image

BG = (8, 2, 20)


def main():
    logo, out = sys.argv[1], sys.argv[2]
    src = Image.open(logo).convert('RGBA')
    src = src.crop(src.getchannel('A').point(lambda v: 255 if v > 90 else 0).getbbox())
    w = 280
    h = round(src.height * w / src.width)
    lg = src.resize((w, h), Image.LANCZOS)
    im = Image.new('RGB', (320, 240), BG)
    im.paste(lg, ((320 - w) // 2, (240 - h) // 2 - 6), lg)
    if '--preview' in sys.argv:
        im.resize((640, 480), Image.NEAREST).save(sys.argv[sys.argv.index('--preview') + 1])
    px = bytearray(320 * 240 * 2)
    for y in range(240):
        for x in range(320):
            r, g, b = im.getpixel((x, y))
            v = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
            i = ((y // 2) * 320 + x) * 2 + (y & 1)
            px[i * 2:i * 2 + 2] = struct.pack('>H', v)
    hdr = b'\x01APPSCRN' + struct.pack('>IHHII', len(px), 240, 320, 0x10000000, 0)
    with open(out, 'wb') as f:
        f.write(hdr + px + bytes(64))


if __name__ == '__main__':
    main()
