# JOE footer mock (PR for `fw/joe-footer`, not for merging)

200x200 renders of `drawJoeFace()` at `fw/joe-footer` 69eb762. `mock.py` repeats the face's
rectangle math (`drawStencilDigit()`, `drawTinyB()`, the cell) and draws every line of text
with the Adafruit GFX classic 5x7 font. `font.py` is copied from `glcdfont.c` in Adafruit GFX
1.13.0, the pinned version. To re-render, run `python3 mock.py` (needs Pillow).

`*-1x.png` files are at panel size. The others are at 3x. `joe-footer-sheet.png` puts them side by side,
starting with today's face (`before-dd6f002`).

This is a mock, not a photo. The wrist test decides how it looks on the glass.
