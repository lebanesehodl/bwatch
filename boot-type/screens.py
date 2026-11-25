#!/usr/bin/env python3
"""The resting screen (restingScreen(): BITCOIN IS TIME before the cell runs
out) and the boot card that ends SEGMENT TEST, as they are at 1e2ebfc and in
three typefaces. Fonts are real fontconvert output (Adafruit-GFX 1.13.0's
fontconvert, as every font in the sketch was made), drawn as GFX draws them;
everything else is the built-in 5x7 font, as now. Light and dark: the screens
would follow the watch's theme."""
import sys
sys.path.insert(0, '../cards')
from PIL import Image
from gshock import Face
import gfx2

SEG18 = __import__('os').environ.get('FONTDIR', '/home/bloxer/wt/joe-footer/firmware/BitcoinChronograph/') + 'DSEG7_Classic_Bold_18.h'

OPTIONS = {
  'lcd':      dict(big='gfx/dseg14_14.h', small='gfx/dseg14_7.h', ghost=None, space='!', track=0,
                   label='A  DSEG14 Classic Bold: the LCD, like JOE'),
  'dial':     dict(big='gfx/michroma_12.h', small='gfx/michroma_7.h', ghost=None, space=' ', track=1,
                   label='B  Michroma: a watch dial\'s wide sans'),
  'engraved': dict(big='gfx/cinzel_16.h', small='gfx/cinzel_10.h', ghost=None, space=' ', track=2,
                   label='C  Cinzel: caseback engraving'),
}

def center_small(f, s, y, c=1):
    f.text((200 - len(s) * 6) // 2, y, s, 1, c)

def center_font(f, path, s, base, o, small=False):
    s = s.replace(' ', o['space'])
    tr = o['track'] if not small else min(o['track'], 1)
    w = gfx2.width(path, s, tr)
    b = gfx2.ink_box(path, s, tr)
    x = (200 - (b[2] - b[0] + 1)) // 2 - b[0]
    gfx2.draw(f, path, x, base, s, 1, tr, o['ghost'])
    return b

def frame(f):
    for k in (0, 2):
        f.rect(k, k, 200 - 2 * k, 1); f.rect(k, 199 - k, 200 - 2 * k, 1)
        f.rect(k, k, 1, 200 - 2 * k); f.rect(199 - k, k, 1, 200 - 2 * k)

# ---------- restingScreen() ----------
def resting_now(path):
    f = Face(True)                         # always white on black today
    frame(f)
    f.text((200 - 7 * 12) // 2, 72, "BITCOIN", 2); f.text((200 - 7 * 12) // 2, 96, "IS TIME", 2)
    center_small(f, "970,962", 128); center_small(f, "LAST BLOCK SEEN", 142)
    center_small(f, "CHARGE TO RESUME", 176)
    f.save(path)

def resting(path, o, dark):
    f = Face(dark)
    frame(f)
    center_font(f, o['big'], "BITCOIN", 82, o)
    center_font(f, o['big'], "IS TIME", 114, o)
    if o is OPTIONS['lcd']:                 # the LCD option: the height as JOE's numbers
        s = "970,962"
        w = gfx2.width(SEG18, "970962") + 6
        x = (200 - w) // 2
        for i, ch in enumerate(s):
            if ch == ',':
                f.rect(x + 1, 140, 3, 4); x += 6; continue
            gfx2.draw(f, SEG18, x, 144, ch, 1, 0, '8', 3)
            x += gfx2.glyph(SEG18, ch)[1]
        center_small(f, "LAST BLOCK SEEN", 150)
    else:
        center_small(f, "970,962", 134); center_small(f, "LAST BLOCK SEEN", 148)
    center_small(f, "CHARGE TO RESUME", 176)
    f.save(path)

# ---------- the boot card SEGMENT TEST ends on ----------
def boot_now(path):
    f = Face(True)
    f.text((200 - 6 * 18) // 2, 64, "BWATCH", 3)
    f.rect(40, 96, 120, 1)
    center_small(f, "SEGMENT TEST OK", 106)
    center_small(f, "BUILT 25 NOV 2026", 124)
    center_small(f, "BITCOIN IS TIME", 176)
    f.rect(0, 184, 200, 16, 0)
    f.save(path)

def boot(path, o, dark):
    f = Face(dark)
    center_font(f, o['big'], "BWATCH", 90, o)
    f.rect(40, 96, 120, 1)
    center_small(f, "SEGMENT TEST OK", 106)
    center_small(f, "BUILT 25 NOV 2026", 124)
    b = center_font(f, o['small'], "BITCOIN IS TIME", 180, o, small=True)
    assert 180 + b[3] <= 183, "into the status line"
    f.save(path)


# ---------- drawShipCard() (.ino:11058 at 1e2ebfc): what the box shows ----------
SEG25 = SEG18.replace('18', '25')

def grouped(f, s, y):
    """drawGrouped() for a whole number in DSEG7 25, line for line"""
    p = SEG25
    digits = sum(c.isdigit() for c in s)
    commas = (digits - 1) // 3 if digits >= 5 else 0
    commaW = 4
    adv = gfx2.glyph(p, '0')[1]
    g0 = gfx2.glyph(p, s[0])[2]; gl = gfx2.glyph(p, s[-1])[2]
    lead = g0[4]; trail = gl[3] - (gl[4] + gl[1])
    x = (200 - (len(s) * adv + commas * commaW - lead - trail)) // 2 - lead
    for i, ch in enumerate(s):
        gfx2.draw(f, p, x, y, ch)
        x += adv
        left = digits - i - 1
        if left > 0 and left % 3 == 0:
            cw = (commaW - 2) // 2
            f.rect(x + cw, y - 3, 2, 3); f.rect(x + cw, y, 1, 1); f.rect(x + cw - 1, y + 1, 1, 1)
            x += commaW

def tiny_b(f, x, y):
    for (a, b, w, h) in [(1,1,2,9),(1,1,4,2),(4,2,2,2),(1,4,4,2),(4,6,2,2),(1,8,4,2),(2,-1,1,2),(2,10,1,2)]:
        f.rect(x + a, y + b, w, h)

SHIP = ["HOLD ANY BUTTON", "FOR 3 SECONDS TO START", "OR PLUG IN TO CHARGE"]

def ship_body(f, top_rule=54):
    center_small(f, SHIP[0], 68); center_small(f, SHIP[1], 82); center_small(f, SHIP[2], 102)
    f.rect(40, 120, 120, 1)
    center_small(f, "SEALED AT BLOCK", 130)
    grouped(f, "970962", 166)
    center_small(f, "...3f9a1c2e", 178)

def ship_now(path):
    f = Face(True)                         # "the card is always dark"
    f.text((200 - 6 * 18) // 2, 22, "BWATCH", 3)
    f.rect(40, 54, 120, 1)
    ship_body(f)
    f.save(path)

def ship(path, o, dark):
    f = Face(dark)
    center_font(f, o['big'], "BWATCH", 48, o)
    f.rect(40, 54, 120, 1)
    ship_body(f)
    f.save(path)

if __name__ == '__main__':
    from PIL import ImageDraw, ImageFont
    out = sys.argv[1] if len(sys.argv) > 1 else 'out'
    S, HEAD = 600, 70
    font = ImageFont.load_default(size=34)
    rows = [('NOW (always white on black)', None)] + [(o['label'], o) for o in OPTIONS.values()]
    sheet = Image.new('RGB', (4 * (S + 16) + 16, HEAD + len(rows) * (S + 60) + 16), (150, 150, 150))
    d = ImageDraw.Draw(sheet)
    for col, t in enumerate(["RESTING, light", "RESTING, dark", "BOOT CARD, light", "BOOT CARD, dark"]):
        d.text((16 + col * (S + 16), 18), t, fill=(0, 0, 0), font=font)
    names = ['now'] + list(OPTIONS)
    for r, ((label, o), name) in enumerate(zip(rows, names)):
        y0 = HEAD + r * (S + 60)
        d.text((16, y0 + 8), label, fill=(0, 0, 0), font=font)
        if o is None:
            resting_now('%s/rest-now.png' % out); boot_now('%s/boot-now.png' % out)
            cells = [None, '%s/rest-now.png' % out, None, '%s/boot-now.png' % out]
        else:
            cells = []
            for kind, fn in (('rest', resting), ('boot', boot)):
                for dark in (False, True):
                    p = '%s/%s-%s-%s.png' % (out, kind, name, 'dark' if dark else 'light')
                    fn(p, o, dark); cells.append(p)
        for c, p in enumerate(cells):
            if p: sheet.paste(Image.open(p), (16 + c * (S + 16), y0 + 52))
    sheet.save('%s/type-sheet.png' % out)
    sh = Image.new('RGB', (3 * (S + 16) + 16, HEAD + len(rows) * (S + 60) + 16), (150, 150, 150))
    d = ImageDraw.Draw(sh)
    for col, t in enumerate(["SHIP CARD, light", "SHIP CARD, dark"]):
        d.text((16 + col * (S + 16), 18), t, fill=(0, 0, 0), font=font)
    for r, ((label, o), name) in enumerate(zip(rows, names)):
        y0 = HEAD + r * (S + 60)
        d.text((16, y0 + 8), label, fill=(0, 0, 0), font=font)
        if o is None:
            ship_now('%s/ship-now.png' % out); cells = [None, '%s/ship-now.png' % out]
        else:
            cells = []
            for dark in (False, True):
                p = '%s/ship-%s-%s.png' % (out, name, 'dark' if dark else 'light'); ship(p, o, dark); cells.append(p)
        for c, p in enumerate(cells):
            if p: sh.paste(Image.open(p), (16 + c * (S + 16), y0 + 52))
    sh = sh.crop((0, 0, 2 * (S + 16) + 16, sh.size[1]))
    sh.save('%s/ship-sheet.png' % out)
    for n, o in OPTIONS.items():
        bigc = gfx2.cost(o['big'], ' !ABCDEFGHIJKLMNOPQRSTUVWXYZ')
        small = gfx2.cost(o['small'], ' !ABCDEFGHIJKLMNOPQRSTUVWXYZ')
        full_big = gfx2.cost(o['big'], ''.join(chr(c) for c in range(32, 127)))
        full_small = gfx2.cost(o['small'], ''.join(chr(c) for c in range(32, 127)))
        print('%-9s headline %5d B (A-Z), small %5d B (A-Z); full ASCII %5d + %5d B' % (n, bigc, small, full_big, full_small))
