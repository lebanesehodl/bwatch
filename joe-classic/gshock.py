#!/usr/bin/env python3
"""Classic-digital JOE concepts, 200x200, 1 bit. Segment digits are hexagons
(a rectangle and two triangles each, which GFX can draw), unlit segments are
'ghosts': a sparse dot pattern, the way an LCD shows its unlit segments.
Text is the GFX 5x7 font (glcdfont.c, Adafruit GFX 1.13.0)."""
from PIL import Image, ImageDraw
from font import FONT
import mock
import gfxfont

SEG = {0: 'abcdef', 1: 'bc', 2: 'abged', 3: 'abgcd', 4: 'fgbc', 5: 'afgcd',
       6: 'afgedc', 7: 'abc', 8: 'abcdefg', 9: 'abcdfg'}


class Face:
    def __init__(self, dark=False):
        self.dark = dark
        self.im = Image.new('1', (200, 200), 0)     # 1 = ink
        self.d = ImageDraw.Draw(self.im)

    def rect(self, x, y, w, h, c=1):
        if w > 0 and h > 0:
            self.d.rectangle([x, y, x + w - 1, y + h - 1], fill=c)

    def poly(self, pts, c=1):
        self.d.polygon(pts, fill=c)

    def ghost_poly(self, pts, step=2):
        """unlit segment: the polygon, every `step`th pixel both ways"""
        m = Image.new('1', (200, 200), 0)
        ImageDraw.Draw(m).polygon(pts, fill=1)
        px, mp = self.im.load(), m.load()
        for y in range(200):
            for x in range(200):
                if mp[x, y] and x % step == 0 and y % step == 0:
                    px[x, y] = 1

    def text(self, x, y, s, size=1, c=1):
        px = self.im.load()
        for ch in s:
            o = ord(ch) * 5
            for i in range(5):
                line = FONT[o + i]
                for j in range(8):
                    if line & 1:
                        for a in range(size):
                            for b in range(size):
                                xx, yy = x + i * size + a, y + j * size + b
                                if 0 <= xx < 200 and 0 <= yy < 200:
                                    px[xx, yy] = c
                    line >>= 1
            x += 6 * size

    def save(self, path, scale=3):
        ink, paper = (255, 255, 255) if self.dark else (0, 0, 0), \
                     (0, 0, 0) if self.dark else (255, 255, 255)
        rgb = Image.new('RGB', (200, 200), paper)
        src = self.im.load(); dst = rgb.load()
        for y in range(200):
            for x in range(200):
                if src[x, y]:
                    dst[x, y] = ink
        rgb.resize((200 * scale, 200 * scale), Image.NEAREST).save(path)
        rgb.save(path.replace('.png', '-1x.png'))


def seg_polys(x, y, w, h, t, slant=0.0, gap=1):
    """the seven segments of a digit in the box (x, y, w, h), stroke t;
    slant shears the top to the right by slant * h"""
    def sh(px, py):
        return (px + slant * (y + h - py), py)
    def hseg(x0, x1, cy):
        a = t / 2
        pts = [(x0 + gap, cy), (x0 + gap + a, cy - a), (x1 - gap - a, cy - a),
               (x1 - gap, cy), (x1 - gap - a, cy + a), (x0 + gap + a, cy + a)]
        return [sh(*p) for p in pts]
    def vseg(cx, y0, y1):
        a = t / 2
        pts = [(cx, y0 + gap), (cx + a, y0 + gap + a), (cx + a, y1 - gap - a),
               (cx, y1 - gap), (cx - a, y1 - gap - a), (cx - a, y0 + gap + a)]
        return [sh(*p) for p in pts]
    L, R = x + t / 2, x + w - t / 2
    T, M, B = y + t / 2, y + h / 2, y + h - t / 2
    return {'a': hseg(L, R, T), 'g': hseg(L, R, M), 'd': hseg(L, R, B),
            'f': vseg(L, T, M), 'e': vseg(L, M, B),
            'b': vseg(R, T, M), 'c': vseg(R, M, B)}


def digit(f, x, y, w, h, t, d, slant=0.0, ghost=2, blank=False):
    polys = seg_polys(x, y, w, h, t, slant)
    on = '' if blank else SEG[d]
    for k, p in polys.items():
        if k in on:
            f.poly(p)
        elif ghost:
            f.ghost_poly(p, ghost)


def number(f, x, y, w, h, t, s, gap, slant=0.0, ghost=2, colon=None):
    """digits right to left would be the LCD way; here left to right. ' ' is
    an unlit digit (ghost only). ':' takes `colon` px."""
    for ch in s:
        if ch == ':':
            cw = colon or t * 2
            for cy in (y + h * 0.3, y + h * 0.7):
                cx = x + cw / 2 + slant * (y + h - cy)
                f.rect(int(cx - t / 2), int(cy - t / 2), t, t)
            x += cw + gap
            continue
        if ch in '-.':
            if ch == '-':
                p = seg_polys(x, y, w, h, t, slant)['g']
                f.poly(p)
            x += w + gap
            continue
        digit(f, x, y, w, h, t, 0 if ch == ' ' else int(ch), slant, ghost, blank=(ch == ' '))
        x += w + gap
    return x


def battery(f, x, y, pct):
    f.rect(x, y, 14, 1); f.rect(x, y + 7, 14, 1); f.rect(x, y, 1, 8); f.rect(x + 13, y, 1, 8)
    f.rect(x + 14, y + 2, 2, 4)
    fill = 11 if pct >= 99 else int(11 * pct / 100 + 0.5)
    f.rect(x + 2, y + 2, fill, 4)


def walker(f, x, y):
    for r, row in enumerate(mock.WALKER):
        for c, ch in enumerate(row):
            if ch == 'X':
                f.rect(x + c, y + r, 1, 1)


BEZEL = 'STEP COUNTER'
DAYS = ['SU', 'MO', 'TU', 'WE', 'TH', 'FR', 'SA']


def small(f, font, right, base, digits, field, ghost=3):
    """a right-aligned LCD field: `field` digit places, the unused ones ghosts"""
    s = digits.rjust(field)
    x = right - gfxfont.width(font, s.replace(' ', '8'))
    return gfxfont.draw(f, font, x, base, s, ghost)


# ---- concept 1: a bezel and a square LCD window (DW-5600-like) ----
def concept1(path, hh, mm, dow, dd, mo, pct, steps, label, dark=False, slant=0.08):
    f = Face(dark)
    f.rect(0, 0, 200, 200, 1)                        # the bezel, in ink
    f.d.rounded_rectangle([6, 20, 193, 179], radius=10, fill=0)   # the window
    # the bezel's words sit by the buttons they name: UP (top right) is the
    # next face, MENU is bottom left
    f.text(8, 6, 'BWATCH', 1, 0)
    f.text(200 - 6 * 4 - 6, 6, 'MODE', 1, 0)
    f.text(8, 186, 'MENU', 1, 0)
    f.text(200 - 6 * len(BEZEL) - 6, 186, BEZEL, 1, 0)
    # top row: the day, and the date in segments
    f.text(16, 31, DAYS[dow], 2)
    gfxfont.draw(f, 'DSEG7_Classic_Bold_18', 184 - gfxfont.width('DSEG7_Classic_Bold_18', '88-88'), 48,
                 '%02d-%02d' % (dd, mo), 3)
    f.rect(12, 54, 176, 1)                           # the LCD's own rule
    # the time
    number(f, 12, 62, 32, 72, 8, '%02d:%02d' % (hh, mm), 6, slant, colon=10)
    f.rect(12, 142, 176, 1)
    # bottom row: battery and steps
    battery(f, 16, 160, pct)
    f.text(36, 160, '%d%%' % pct)
    walker(f, 80, 156)
    f.text(184 - 6 * len(label) + 1, 147, label)     # TODAY / FROM 09:41
    small(f, 'DSEG7_Classic_Bold_18', 184, 174, steps, 5)
    f.save(path)


# ---- concept 2: the whole glass is the LCD, F-91W-like ----
def concept2(path, hh, mm, dow, dd, mo, pct, steps, label, dark=False, slant=0.12):
    f = Face(dark)
    f.text(6, 4, 'BWATCH', 1)
    f.text(200 - 6 * 19 - 4, 4, 'BITCOIN CHRONOGRAPH', 1)
    f.rect(0, 14, 200, 2)
    # upper: day in big letters, date in segments
    f.text(10, 26, DAYS[dow], 3)
    gfxfont.draw(f, 'DSEG7_Classic_Bold_25', 194 - gfxfont.width('DSEG7_Classic_Bold_25', '88-88'), 50,
                 '%02d-%02d' % (dd, mo), 3)
    # the time, big
    number(f, 6, 62, 36, 84, 9, '%02d:%02d' % (hh, mm), 6, slant, colon=10)
    f.rect(0, 156, 200, 2)
    battery(f, 6, 178, pct)
    f.text(26, 178, '%d%%' % pct)
    walker(f, 64, 174)
    f.text(194 - 6 * len(label) + 1, 162, label)
    small(f, 'DSEG7_Classic_Bold_18', 194, 196, steps, 5)
    f.save(path)


if __name__ == '__main__':
    S = 600
    shots = [
        ('g1-light.png', concept1, dict(hh=10, mm=47, dow=0, dd=23, mo=11, pct=70, steps='4812', label='TODAY')),
        ('g1-dark.png', concept1, dict(hh=21, mm=8, dow=0, dd=23, mo=11, pct=35, steps='1207', label='FROM 09:41', dark=True)),
        ('g2-light.png', concept2, dict(hh=10, mm=47, dow=0, dd=23, mo=11, pct=70, steps='4812', label='TODAY')),
        ('g2-dark.png', concept2, dict(hh=21, mm=8, dow=0, dd=23, mo=11, pct=35, steps='12345', label='TODAY', dark=True)),
    ]
    for name, fn, kw in shots:
        fn(name, **kw)
    sheet = Image.new('RGB', (2 * S + 48, 2 * S + 64), (150, 150, 150))
    dr = ImageDraw.Draw(sheet)
    for i, (name, fn, kw) in enumerate(shots):
        x = 16 + (i % 2) * (S + 16); y = 20 + (i // 2) * (S + 20)
        dr.text((x, y - 14), name, fill=(0, 0, 0))
        sheet.paste(Image.open(name), (x, y))
    sheet.save('gshock-sheet.png')
