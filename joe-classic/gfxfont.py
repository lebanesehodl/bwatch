"""Render an Adafruit GFXfont header (fontconvert output) the way GFX does."""
import re
FONTDIR = '/home/bloxer/wt/joe-footer/firmware/BitcoinChronograph/'
_cache = {}
def load(name):
    if name in _cache: return _cache[name]
    src = open(FONTDIR + name + '.h').read()
    bm = src[src.index('Bitmaps[]'):]
    bm = bm[bm.index('{') + 1: bm.index('};')]
    bits = [int(v, 16) for v in re.findall(r'0x[0-9A-Fa-f]+', bm)]
    gl = src[src.index('Glyphs[]'):]
    gl = gl[gl.index('{') + 1: gl.index('};')]
    glyphs = [tuple(int(v) for v in g) for g in re.findall(r'\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\}', gl)]
    tail = src[src.rindex('Glyphs,'):]
    first, last, ya = re.findall(r'(0x[0-9A-Fa-f]+|\d+)', tail)[:3]
    f = (bits, glyphs, int(first, 0), int(last, 0), int(ya, 0))
    _cache[name] = f
    return f
def glyph_pixels(name, ch):
    bits, glyphs, first, last, ya = load(name)
    o = ord(ch)
    if o < first or o > last: return [], 0
    off, w, h, xa, xo, yo = glyphs[o - first]
    out = []; bit = 0
    for yy in range(h):
        for xx in range(w):
            if bits[off + (bit >> 3)] & (0x80 >> (bit & 7)): out.append((xo + xx, yo + yy))
            bit += 1
    return out, xa
def width(name, s):
    return sum(glyph_pixels(name, c)[1] for c in s)
def draw(face, name, x, y, s, ghost=0, c=1):
    """y is the baseline. ghost: draw an '8' under each digit, every ghost-th pixel"""
    px = face.im.load()
    for ch in s:
        pts, xa = glyph_pixels(name, ch)
        if ghost and (ch.isdigit() or ch == ' '):
            g8, _ = glyph_pixels(name, '8')
            for (dx, dy) in g8:
                X, Y = x + dx, y + dy
                if 0 <= X < 200 and 0 <= Y < 200 and X % ghost == 0 and Y % ghost == 0: px[X, Y] = c
            if ch == ' ': xa = glyph_pixels(name, '8')[1]
        for (dx, dy) in pts:
            X, Y = x + dx, y + dy
            if 0 <= X < 200 and 0 <= Y < 200: px[X, Y] = c
        x += xa
    return x
