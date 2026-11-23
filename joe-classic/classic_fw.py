#!/usr/bin/env python3
"""drawJoeClassic() as written in the sketch (fw/joe-classic), line for line:
fillConvex's scanline, drawSegDigit's hexagons, drawSegText's DSEG ghosts,
drawMoon with GFX's own drawCircle, drawBattery, drawWalker, the 5x7 font."""
import math
from PIL import Image
import gfxfont, mock
from font import FONT
from gshock import Face

P = 29.530588853
REF = 947182440


def fill_convex(f, px, py, step):
    y0, y1 = min(py), max(py)
    pix = f.im.load()
    for y in range(int(math.floor(y0)), int(math.ceil(y1)) + 1):
        if step > 1 and y % step:
            continue
        yc = y + 0.5; xl, xr = 1e9, -1e9
        n = len(px)
        for i in range(n):
            j = (i + 1) % n
            ya, yb = py[i], py[j]
            if (ya <= yc < yb) or (yb <= yc < ya):
                x = px[i] + (yc - ya) * (px[j] - px[i]) / (yb - ya)
                xl = min(xl, x); xr = max(xr, x)
        a, b = int(math.ceil(xl - 0.5)), int(math.floor(xr - 0.5))
        if b < a:
            continue
        for x in range(a, b + 1):
            if (step == 1 or x % step == 0) and 0 <= x < 200 and 0 <= y < 200:
                pix[x, y] = 1


SEGS = [0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F]


def seg_digit(f, x, y, w, h, t, d, slant):
    on = SEGS[d]
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
        fill_convex(f, xs, ys, 1 if (on >> k) & 1 else 2)


def seg_time(f, x, y, w, h, t, gap, cw, slant, hh, mm):
    d = [hh // 10, hh % 10, mm // 10, mm % 10]
    for i in range(4):
        seg_digit(f, x, y, w, h, t, d[i], slant)
        x += w + gap
        if i == 1:
            for k in range(2):
                cy = y + h * (0.7 if k else 0.3)
                cx = x + cw / 2.0 + slant * (y + h - cy)
                f.rect(int(cx - t / 2.0), int(cy - t / 2.0), t, t)
            x += cw + gap


def gfx_circle(f, x0, y0, r):
    """Adafruit_GFX::drawCircle"""
    pix = f.im.load()
    def p(x, y):
        if 0 <= x < 200 and 0 <= y < 200: pix[x, y] = 1
    fx, ddx, ddy, x, y = 1 - r, 1, -2 * r, 0, r
    p(x0, y0 + r); p(x0, y0 - r); p(x0 + r, y0); p(x0 - r, y0)
    while x < y:
        if fx >= 0:
            y -= 1; ddy += 2; fx += ddy
        x += 1; ddx += 2; fx += ddx
        for (a, b) in ((x, y), (-x, y), (x, -y), (-x, -y), (y, x), (-y, x), (y, -x), (-y, -x)):
            p(x0 + a, y0 + b)


def moon_lit(age, dx, dy):
    th = 2 * math.pi * age / P
    s = math.sqrt(1 - dy * dy)
    return dx > math.cos(th) * s if th < math.pi else dx < -math.cos(th) * s


def draw_moon(f, cx, cy, r, age):
    pix = f.im.load()
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            dx, dy = (x + 0.5 - cx) / r, (y + 0.5 - cy) / r
            if dx * dx + dy * dy > 1: continue
            if moon_lit(age, dx, dy) or (x % 2 == 0 and y % 2 == 0): pix[x, y] = 1
    gfx_circle(f, cx, cy, r)


def battery(f, x, y, pct):
    f.rect(x, y, 14, 1); f.rect(x, y + 7, 14, 1); f.rect(x, y, 1, 8); f.rect(x + 13, y, 1, 8)
    f.rect(x + 14, y + 2, 2, 4)
    fill = 11 if pct >= 99 else int(11 * pct / 100 + 0.5)
    if fill > 0: f.rect(x + 2, y + 2, fill, 4)
    if pct < 15:
        f.rect(x + 19, y, 1, 5); f.rect(x + 19, y + 7, 1, 1)


def walker(f, x, y):
    for r, row in enumerate(mock.WALKER):
        for c, ch in enumerate(row):
            if ch == 'X': f.rect(x + c, y + r, 1, 1)


DAYS = ["SU", "MO", "TU", "WE", "TH", "FR", "SA"]
SF = 'DSEG7_Classic_Bold_18'


def joe_classic(path, hh, mm, wday, day, month, utc, pct, k, count, frm='09:41', dark=False):
    f = Face(dark)
    f.rect(0, 0, 200, 200, 1)
    f.d.rounded_rectangle([6, 20, 6 + 188 - 1, 20 + 160 - 1], radius=10, fill=0)
    f.text(8, 6, "BWATCH", 1, 0); f.text(200 - 6 * 4 - 6, 6, "MODE", 1, 0)
    f.text(8, 186, "MENU", 1, 0); f.text(200 - 6 * 12 - 6, 186, "STEP COUNTER", 1, 0)
    f.text(16, 29, DAYS[wday - 1], 2)
    gfxfont.draw(f, SF, 184 - gfxfont.width(SF, "88-88"), 46, "%02d-%02d" % (day, month), 3)
    f.rect(12, 51, 176, 1)
    age = ((utc - REF) / 86400.0) % P
    f.text(118, 60, "MOON %2d" % int(age))
    draw_moon(f, 176, 63, 8, age)
    f.rect(12, 75, 176, 1)
    seg_time(f, 14, 81, 31, 56, 7, 6, 10, 0.08, hh, mm)
    f.rect(12, 142, 176, 1)
    battery(f, 16, 160, pct)
    f.text(39, 160, "%d%%" % pct)
    walker(f, 80, 156)
    lab = {'TODAY': 'TODAY', 'FROM': 'FROM ' + frm, 'NONE': ''}[k]
    num = "%5s" % "--" if k == 'NONE' else "%5d" % count
    f.text(185 - 6 * len(lab), 147, lab)
    gfxfont.draw(f, SF, 184 - gfxfont.width(SF, num.replace(' ', '8')), 174, num, 3)
    f.save(path)


if __name__ == '__main__':
    from PIL import ImageDraw
    import calendar, datetime
    def u(s): return calendar.timegm(datetime.datetime.strptime(s, '%Y-%m-%d %H:%M').timetuple())
    shots = [
        ('fw-1.png', dict(hh=10, mm=47, wday=2, day=23, month=11, utc=u('2026-11-23 10:47'), pct=70, k='TODAY', count=4812)),
        ('fw-2.png', dict(hh=21, mm=8, wday=2, day=23, month=11, utc=u('2026-11-23 21:08'), pct=35, k='FROM', count=1207, dark=True)),
        ('fw-3.png', dict(hh=0, mm=1, wday=4, day=2, month=12, utc=u('2026-12-02 00:01'), pct=10, k='NONE', count=0)),
        ('fw-4.png', dict(hh=19, mm=56, wday=7, day=19, month=12, utc=u('2026-12-19 19:56'), pct=100, k='TODAY', count=23456, dark=True)),
    ]
    for n, kw in shots: joe_classic(n, **kw)
    S = 600
    sheet = Image.new('RGB', (4 * (S + 16) + 16, S + 32), (150, 150, 150))
    for i, (n, kw) in enumerate(shots):
        sheet.paste(Image.open(n), (16 + i * (S + 16), 16))
    sheet.save('fw-sheet.png')
