#!/usr/bin/env python3
"""BITCOIN IS TIME, the ship card and the boot card in the website's own
typeface: Archivo, which bwatch.html says "is the brand voice and it matches
the mark" (the mark is an Archivo 600 B). Weight 600 is the site's file and
the mark's weight; 800 is what the site's h1 asks for. Uppercase with the
headline's tight tracking, as the site sets it. The screens stay as they are
today (white on black, same words, same places); only the big type changes."""
import sys
from PIL import Image, ImageDraw, ImageFont
import screens
from screens import Face, center_small, frame, ship_body, gfx2

def opt(w):
    return dict(big='gfx/archivo%d_18.h' % w, mid='gfx/archivo%d_15.h' % w,
                small='gfx/archivo%d_9.h' % w, ghost=None, space=' ', track=-1)

def center(f, path, s, base, track):
    b = gfx2.ink_box(path, s, track)
    x = (200 - (b[2] - b[0] + 1)) // 2 - b[0]
    gfx2.draw(f, path, x, base, s, 1, track)
    return b

def resting(path, o):
    f = Face(True); frame(f)
    center(f, o['big'], "BITCOIN", 84, o['track'])
    center(f, o['big'], "IS TIME", 114, o['track'])
    center_small(f, "970,962", 134); center_small(f, "LAST BLOCK SEEN", 148)
    center_small(f, "CHARGE TO RESUME", 176)
    f.save(path)

def ship(path, o):
    f = Face(True)
    center(f, o['mid'], "BWATCH", 44, o['track'])
    f.rect(40, 54, 120, 1)
    ship_body(f)
    f.save(path)

def boot(path, o):
    f = Face(True)
    center(f, o['mid'], "BWATCH", 86, o['track'])
    f.rect(40, 96, 120, 1)
    center_small(f, "SEGMENT TEST OK", 106)
    center_small(f, "BUILT 25 NOV 2026", 124)
    b = center(f, o['small'], "BITCOIN IS TIME", 182, 0)
    assert 182 + b[3] <= 183
    f.save(path)

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'out'
    S, HEAD = 600, 70
    font = ImageFont.load_default(size=34)
    rows = [("NOW: the built-in 5x7 font", None), ("ARCHIVO 600: the site's file, the mark's weight", 600),
            ("ARCHIVO 800: the site's headline weight", 800)]
    sh = Image.new('RGB', (3 * (S + 16) + 16, HEAD + len(rows) * (S + 60) + 16), (150, 150, 150))
    d = ImageDraw.Draw(sh)
    for c, t in enumerate(["BITCOIN IS TIME (battery out)", "SHIP CARD", "BOOT CARD"]):
        d.text((16 + c * (S + 16), 18), t, fill=(0, 0, 0), font=font)
    for r, (label, w) in enumerate(rows):
        y0 = HEAD + r * (S + 60)
        d.text((16, y0 + 8), label, fill=(0, 0, 0), font=font)
        if w is None:
            screens.resting_now(out + '/b-rest-now.png'); screens.ship_now(out + '/b-ship-now.png')
            screens.boot_now(out + '/b-boot-now.png')
            cells = [out + '/b-rest-now.png', out + '/b-ship-now.png', out + '/b-boot-now.png']
        else:
            o = opt(w); cells = []
            for kind, fn in (('rest', resting), ('ship', ship), ('boot', boot)):
                p = '%s/b-%s-archivo%d.png' % (out, kind, w); fn(p, o); cells.append(p)
            print('archivo %d: 18pt %d B, 15pt %d B, 9pt %d B (A-Z + space)' % (w,
                  gfx2.cost(o['big'], ' ABCDEFGHIJKLMNOPQRSTUVWXYZ'),
                  gfx2.cost(o['mid'], ' ABCDEFGHIJKLMNOPQRSTUVWXYZ'),
                  gfx2.cost(o['small'], ' ABCDEFGHIJKLMNOPQRSTUVWXYZ')))
        for c, p in enumerate(cells):
            sh.paste(Image.open(p), (16 + c * (S + 16), y0 + 52))
    sh.save(out + '/brand-sheet.png')
