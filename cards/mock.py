#!/usr/bin/env python3
"""JOE face mock, 200x200. The same rectangle math as drawJoeFace() and
drawStencilDigit(), and the Adafruit GFX classic 5x7 font (glcdfont.c,
Adafruit GFX 1.13.0, the pinned version) for every line of text."""
import sys
from PIL import Image
from font import FONT


class Panel:
    def __init__(self, dark=False):
        self.dark = dark
        self.px = [[0] * 200 for _ in range(200)]   # 1 = ink (fg)
        self.fill_rect(0, 0, 200, 200, 0)

    # fg = 1, bg = 0; colours are resolved at save time
    def fill_rect(self, x, y, w, h, c):
        for yy in range(max(0, y), min(200, y + h)):
            for xx in range(max(0, x), min(200, x + w)):
                self.px[yy][xx] = c

    def text(self, x, y, s):
        for ch in s:
            o = ord(ch) * 5
            for i in range(5):
                line = FONT[o + i]
                for j in range(8):
                    if line & 1 and 0 <= x + i < 200 and 0 <= y + j < 200:
                        self.px[y + j][x + i] = 1
                    line >>= 1
            x += 6

    def save(self, path, scale=1):
        ink, paper = ((255, 255, 255), (0, 0, 0)) if self.dark else ((0, 0, 0), (255, 255, 255))
        im = Image.new('RGB', (200, 200))
        for y in range(200):
            for x in range(200):
                im.putpixel((x, y), ink if self.px[y][x] else paper)
        if scale != 1:
            im = im.resize((200 * scale, 200 * scale), Image.NEAREST)
        im.save(path)


def r(v):                       # (int)(v + 0.5f)
    return int(v + 0.5)


def stencil(p, X, Y, W, H, d):
    T = 0.24
    UH = (1.0 - 3.0 * T) / 2.0
    UY, LY = T, (1.0 + T) / 2.0

    def fill(x, y, w, h, c):
        p.fill_rect(X + r(x * W), Y + r(y * H), r(w * W), r(h * H), c)
    slabs = {2: (0, 1 - T, T, 1 - T), 3: (0, 1 - T, 0, 1 - T), 5: (T, 1 - T, 0, 1 - T),
             6: (T, 1 - T, T, 1 - 2 * T), 8: (T, 1 - 2 * T, T, 1 - 2 * T),
             9: (T, 1 - 2 * T, 0, 1 - T)}
    if d == 0:
        fill(0, 0, 1, 1, 1); fill(T, T, 1 - 2 * T, 1 - 2 * T, 0); return
    if d in slabs:
        ux, uw, lx, lw = slabs[d]
        fill(0, 0, 1, 1, 1); fill(ux, UY, uw, UH, 0); fill(lx, LY, lw, UH, 0); return
    if d == 1:
        fill(1 - T, 0, T, 1, 1); fill(1 - 2.1 * T, 0, 1.1 * T, T, 1)
    elif d == 4:
        fill(0, 0, T, (1 + T) / 2, 1); fill(1 - T, 0, T, 1, 1); fill(0, (1 - T) / 2, 1, T, 1)
    elif d == 7:
        fill(0, 0, 1, T, 1); fill(1 - T, 0, T, 1, 1)


def draw_rect(p, x, y, w, h):          # GFX drawRect: the outline
    p.fill_rect(x, y, w, 1, 1); p.fill_rect(x, y + h - 1, w, 1, 1)
    p.fill_rect(x, y, 1, h, 1); p.fill_rect(x + w - 1, y, 1, h, 1)


def battery(p, x, y, pct, charging=False):
    """drawBattery(): outline, nub, proportional fill, '!' under 15%."""
    draw_rect(p, x, y, 14, 8)
    p.fill_rect(x + 14, y + 2, 2, 4, 1)
    if not charging:
        fill = 11 if pct >= 99 else r(11 * pct / 100.0)
        if fill > 0:
            p.fill_rect(x + 2, y + 2, fill, 4, 1)
    if not charging and pct < 15:
        p.fill_rect(x + 19, y, 1, 5, 1); p.fill_rect(x + 19, y + 7, 1, 1, 1)


WALKER = ["....XX.",
          "....XX.",
          "...XX..",
          "..XXXX.",
          ".X.XX.X",
          "X..XX..",
          "...XXX.",
          "..X..X.",
          "..X...X",
          ".X....X",
          "X......"]


def walker(p, x, y):
    for rr, row in enumerate(WALKER):
        for cc, ch in enumerate(row):
            if ch == 'X':
                p.fill_rect(x + cc, y + rr, 1, 1, 1)


def tiny_b(p, x, y):
    for (a, b, w, h) in [(1, 1, 2, 9), (1, 1, 4, 2), (4, 2, 2, 2), (1, 4, 4, 2),
                         (4, 6, 2, 2), (1, 8, 4, 2), (2, -1, 1, 2), (2, 10, 1, 2)]:
        p.fill_rect(x + a, y + b, w, h, 1)


def joe(path, hh, mm, day, mon, batt, steps, quote, dark=False, scale=3,
        QH=90, DH=81, DW=86, QY=None, cell=(78, 46)):
    p = Panel(dark)
    QW, GUT = 98, 4
    digits = [hh // 10, hh % 10, mm // 10, mm % 10]
    for q in range(4):
        ox = (q % 2) * (QW + GUT) + (QW - DW) // 2
        oy = (q // 2) * (QH + GUT) + (QH - DH) // 2
        stencil(p, ox, oy, DW, DH, digits[q])
    SY = 188
    tiny_b(p, 1, SY)
    p.text(12, SY + 3, "%02d %02d" % (day, mon))
    if cell == 'glyph':
        battery(p, 46, SY + 3, batt)
        pc = '%d%%' % batt
        px = 46 + 23
        if steps is not None:
            sx = 201 - 6 * len(steps)
            wx = sx - 2 - 7
            if px + 6 * len(pc) + 2 > wx:
                pc = ''
            walker(p, wx, SY)
            p.text(sx, SY + 3, steps)
        p.text(px, SY + 3, pc)
    else:
        cx, cw = cell
        p.fill_rect(cx, SY, cw, 3, 1)
        fw = r(cw * batt / 100.0) if batt >= 0 else 0
        if fw > 0:
            p.fill_rect(cx, SY + 5, fw, 5, 1)
        if steps is not None:
            p.text(201 - 6 * len(steps), SY + 3, steps)
    if quote is not None and QY is not None:
        for i, line in enumerate(quote):
            p.text((200 - len(line) * 6) // 2, QY[i], line)
    p.save(path, scale)
    p.save(path.replace('.png', '-1x.png'), 1)


if __name__ == '__main__':
    # the lines, exactly as JOE_SAYS has them (BitcoinChronograph.ino)
    Q1 = ('"A NEW KEY PAIR IS USED FOR', 'EVERY TRANSACTION." - SATOSHI')
    Q2 = ('"...IT\'S BEST TO USE BITCOIN', 'ADDRESSES ONLY ONCE." - SATOSHI')
    Q3 = ('"...BY KEEPING PUBLIC KEYS', 'ANONYMOUS." - SATOSHI')
    for q in Q1 + Q2 + Q3:
        assert len(q) <= 33, q
    # drawJoeFace() at fw/joe-footer (the walker strip): quarters 81, digits 86 x 73, Satoshi at
    # y 167 and 176; the strip: mark, date, the regular faces' battery glyph
    # and its percentage, then the walker and the steps to the right edge
    new = dict(QH=81, DH=73, DW=86, QY=(167, 176), cell='glyph')
    joe('before-dd6f002.png', 14, 32, 23, 11, 72, None, None, cell=(78, 46))
    joe('light-today.png', 14, 32, 23, 11, 70, '4,812 TODAY', Q1, **new)
    joe('light-since.png', 9, 58, 23, 11, 85, '1,207 FROM 09:41', Q2, **new)
    joe('light-none.png', 14, 32, 23, 11, 10, '--', Q3, **new)
    joe('dark-today.png', 22, 8, 23, 11, 35, '12,345 TODAY', Q2, dark=True, **new)
    joe('light-widest.png', 18, 47, 23, 11, 100, '12,345 FROM 09:41', Q1, **new)
