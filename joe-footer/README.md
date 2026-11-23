# JOE footer mock (PR for `fw/joe-footer`, not for merging)

200x200 renders of `drawJoeFace()` at `fw/joe-footer` 6b60c74. `mock.py` repeats the face's
rectangle math (`drawStencilDigit()`, `drawTinyB()`, the cell) and draws every line of text
with the Adafruit GFX classic 5x7 font. `font.py` is copied from `glcdfont.c` in Adafruit GFX
1.13.0, the pinned version. To re-render, run `python3 mock.py` (needs Pillow).

`*-1x.png` files are at panel size. The others are at 3x. `joe-footer-sheet.png` puts them side by side,
starting with today's face (`before-dd6f002`). `strips-6x.png` is the bottom strip of each state at 6x.

This is a mock, not a photo. The wrist test decides how it looks on the glass.

`plain-*.png` and `joe-plain-sheet.png` show the version with no quote (`fw/joe-footer-plain` 7608aba): the full-size digits from dd6f002 above the same strip.

Digits (`fw/joe-footer` 9c9358a, `fw/joe-footer-plain` e596eab): `ones-sheet2.png` shows the 1s OP chose from (he picked 5), `thick-sheet.png` the stroke weights (he picked D), `seven-sheet.png` the 7's bar (F, as thick as its upright), and `digits-final-sheet.png` all ten digits as the code now draws them, with and without the quote.
