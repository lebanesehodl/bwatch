"""Adafruit GFXfont headers (fontconvert output), rendered as GFX does."""
import re
_cache = {}
def load(path):
    if path in _cache: return _cache[path]
    src = open(path).read()
    bm = src[src.index('Bitmaps[]'):]; bm = bm[bm.index('{') + 1: bm.index('};')]
    bits = [int(v, 16) for v in re.findall(r'0x[0-9A-Fa-f]+', bm)]
    gl = src[src.index('Glyphs[]'):]; gl = gl[gl.index('{') + 1: gl.index('};')]
    glyphs = [tuple(int(v) for v in g) for g in re.findall(r'\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\}', gl)]
    tail = src[src.rindex('Glyphs,'):]
    first, last, ya = re.findall(r'(0x[0-9A-Fa-f]+|\d+)', tail)[:3]
    f = (bits, glyphs, int(first, 0), int(last, 0), int(ya, 0))
    _cache[path] = f
    return f
def glyph(path, ch):
    bits, glyphs, first, last, ya = load(path)
    o = ord(ch)
    if o < first or o > last: return [], 0, None
    off, w, h, xa, xo, yo = glyphs[o - first]
    out = []; bit = 0
    for yy in range(h):
        for xx in range(w):
            if bits[off + (bit >> 3)] & (0x80 >> (bit & 7)): out.append((xo + xx, yo + yy))
            bit += 1
    return out, xa, (off, w, h, xa, xo, yo)
def width(path, s, track=0):
    return sum(glyph(path, c)[1] for c in s) + track * (len(s) - 1)
def ink_box(path, s, track=0):
    """the inked pixels' box, relative to the cursor at baseline 0"""
    x = 0; xs = []; ys = []
    for c in s:
        pts, xa, _ = glyph(path, c)
        xs += [x + a for a, b in pts]; ys += [b for a, b in pts]
        x += xa + track
    return (min(xs), min(ys), max(xs), max(ys)) if xs else (0, 0, 0, 0)
def draw(face, path, x, y, s, c=1, track=0, ghost=None, gstep=3):
    """y: baseline. ghost: a character drawn every gstep-th pixel under each glyph (an LCD's unlit segments)"""
    px = face.im.load()
    for ch in s:
        pts, xa, _ = glyph(path, ch)
        if ghost and ch != ' ':
            for (dx, dy) in glyph(path, ghost)[0]:
                X, Y = x + dx, y + dy
                if 0 <= X < 200 and 0 <= Y < 200 and X % gstep == 0 and Y % gstep == 0: px[X, Y] = c
        for (dx, dy) in pts:
            X, Y = x + dx, y + dy
            if 0 <= X < 200 and 0 <= Y < 200: px[X, Y] = c
        x += xa + track
    return x
def cost(path, chars):
    """flash for a font cut to `chars`, as fontconvert would emit it for first..last:
    bitmap bytes + 7-byte glyph records (+ padding to 8 on the ESP32) + the 12-byte struct"""
    bits, glyphs, first, last, ya = load(path)
    lo, hi = min(map(ord, chars)), max(map(ord, chars))
    bm = 0
    for o in range(lo, hi + 1):
        off, w, h, xa, xo, yo = glyphs[o - first]
        bm += (w * h + 7) // 8
    return bm + 8 * (hi - lo + 1) + 12
