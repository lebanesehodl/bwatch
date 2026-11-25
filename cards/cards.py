#!/usr/bin/env python3
"""Anniversary cards: drawStatCard() as it is at 1e2ebfc ("before"), line for
line, and a classic-JOE redesign ("after"), both at 200x200, light and dark.
The text on every card is what drawPersonCard()/drawWatchCard() would build
for that person or watch; nothing is added and nothing is left out."""
import sys, math
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from PIL import Image
import gfxfont
from gshock import Face
from classic_fw import fill_convex, SEGS

# ---------- before: drawStatCard() at 1e2ebfc ----------
def center_small(f, s, y):
    f.text((200 - len(s) * 6) // 2, y, s)

def before(path, top, title, num, l1, l2, l3, dark):
    f = Face(dark)
    center_small(f, top, 14)
    tl = len(title); sz = 3 if tl <= 10 else 2
    f.text((200 - tl * 6 * sz) // 2, 34, title, sz)
    s = "%d" % abs(num)
    F = 'DSEG7_Classic_Bold_25' if len(s) > 7 else 'DSEG7_Classic_Bold_32'
    n, cw = len(s), 6
    w = 0
    for i in range(n):
        w += gfxfont.glyph_pixels(F, s[i])[1]
        if (n - 1 - i) % 3 == 0 and i < n - 1: w += cw
    x = (200 - w) // 2
    for i in range(n):
        gfxfont.draw(f, F, x, 104, s[i])
        x += gfxfont.glyph_pixels(F, s[i])[1]
        if (n - 1 - i) % 3 == 0 and i < n - 1:
            f.rect(x + cw // 2 - 1, 103, 3, 5); x += cw
    center_small(f, l1, 114)
    f.rect(30, 134, 140, 1)
    center_small(f, l2, 146)
    center_small(f, l3, 160)
    for o in (0, 2):                      # the double frame
        f.rect(o, o, 200 - 2 * o, 1); f.rect(o, 199 - o, 200 - 2 * o, 1)
        f.rect(o, o, 1, 200 - 2 * o); f.rect(199 - o, o, 1, 200 - 2 * o)
    f.save(path)

# ---------- after: the classic JOE frame ----------
def seg_digit(f, x, y, w, h, t, on, slant):
    """drawSegDigit's hexagons; `on` is the segment mask (0: a blank place, all ghost)"""
    a, g = t / 2.0, 1.0
    L, R, T, M, B = x + a, x + w - a, y + a, y + h / 2.0, y + h - a
    ends = [(L, T, R, T), (R, T, R, M), (R, M, R, B), (L, B, R, B), (L, M, L, B), (L, T, L, M), (L, M, R, M)]
    for k, e in enumerate(ends):
        if e[1] == e[3]:
            xs = [e[0] + g, e[0] + g + a, e[2] - g - a, e[2] - g, e[2] - g - a, e[0] + g + a]
            ys = [e[1], e[1] - a, e[1] - a, e[1], e[1] + a, e[1] + a]
        else:
            xs = [e[0], e[0] + a, e[0] + a, e[0], e[0] - a, e[0] - a]
            ys = [e[1] + g, e[1] + g + a, e[3] - g - a, e[3] - g, e[3] - g - a, e[1] + g + a]
        xs = [xs[i] + slant * (y + h - ys[i]) for i in range(6)]
        fill_convex(f, xs, ys, 1 if (on >> k) & 1 else 3)   # unlit: every third pixel

def ghost_rect(f, x, y, w, h):
    px = f.im.load()
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            if xx % 3 == 0 and yy % 3 == 0: px[xx, yy] = 1

PLACES, DW, DH, DT, GAP, SEP, SL = 7, 21, 50, 5, 2, 4, 0.08

def seg_number(f, right, y, num):
    """a 7-place LCD field, right-aligned at `right`: lit digits, ghost 8s in
    the places before them, and the two group marks (x,xxx,xxx) lit only
    where the number reaches them"""
    s = "%d" % abs(num)
    w = PLACES * DW + (PLACES - 1) * GAP + 2 * (SEP + GAP)
    x = right - w
    for p in range(PLACES):
        k = p - (PLACES - len(s))            # index into s, < 0: a blank place
        seg_digit(f, x, y, DW, DH, DT, SEGS[int(s[k])] if k >= 0 else 0, SL)
        x += DW + GAP
        left = PLACES - 1 - p                # places after this one
        if left in (6, 3):
            lit = len(s) > left
            cx = x + SEP / 2.0 + SL * 2
            if lit: f.rect(int(cx - 2), y + DH - 6, 4, 4)
            else: ghost_rect(f, int(cx - 2), y + DH - 6, 4, 4)
            x += SEP + GAP
    return right - w

def after(path, top, title, num, l1, l2, l3, dark):
    f = Face(dark)
    f.rect(0, 0, 200, 200, 1)                                       # the bezel
    f.d.rounded_rectangle([6, 20, 193, 179], radius=10, fill=0)     # the window
    f.text(8, 6, top, 1, 0)                                         # bezel: when / which
    f.text((200 - 6 * len(l3)) // 2, 186, l3, 1, 0)                 # bezel: the punchline
    f.text(14, 28, title, 2)                                        # the name, dot matrix
    f.rect(12, 50, 176, 1)
    f.text(185 - 6 * len(l1), 58, l1)                               # what the number counts
    x0 = seg_number(f, 186, 70, num)
    f.rect(12, 130, 176, 1)
    f.text((200 - 6 * len(l2)) // 2, 148, l2)                       # where it started
    f.save(path)

CASES = [
    ("person: long name", "1 OF 3 - BACK FOR NEXT", "MARGARET", 812345,
     "BLOCKS SEEN", "BORN AT BLOCK 157,616", "SEEN 84% OF THE CHAIN"),
    ("person: before genesis", "2 OF 3 - BACK FOR NEXT", "GRANDPA", 2601234,
     "BLOCKS OLD, AT TEN MINUTES", "BORN 1,628,889 BEFORE GENESIS", "BITCOIN IS 37% OF YOUR LIFE"),
    ("person: not born yet", "3 OF 3 - BACK FOR NEXT", "BABY", -4211,
     "BLOCKS UNTIL BORN", "BORN AT BLOCK 974,173", ""),
    ("the watch", "THIS WATCH", "BWATCH", 1234,
     "BLOCKS WITNESSED", "UNBOXED AT BLOCK 968,728", "9 DAYS ON YOUR WRIST"),
]

if __name__ == '__main__':
    from PIL import ImageDraw, ImageFont
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    S, HEAD, SIDE = 600, 70, 16
    font = ImageFont.load_default(size=34)
    sheet = Image.new('RGB', (4 * (S + 16) + 16, HEAD + len(CASES) * (S + 60) + 16), (150, 150, 150))
    d = ImageDraw.Draw(sheet)
    for col, t in enumerate(["NOW, light", "CLASSIC, light", "NOW, dark", "CLASSIC, dark"]):
        d.text((16 + col * (S + 16), 18), t, fill=(0, 0, 0), font=font)
    for r, (name, *c) in enumerate(CASES):
        y0 = HEAD + r * (S + 60)
        d.text((16, y0 + 8), name, fill=(0, 0, 0), font=font)
        for col, (fn, dark) in enumerate([(before, False), (after, False), (before, True), (after, True)]):
            p = '%s/card-%d-%s-%s.png' % (out, r + 1, fn.__name__, 'dark' if dark else 'light')
            fn(p, *c, dark)
            sheet.paste(Image.open(p), (16 + col * (S + 16), y0 + 52))
    sheet.save(out + '/cards-sheet.png')
    print('ok')
