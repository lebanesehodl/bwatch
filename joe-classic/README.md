# JOE, classic digital face (`fw/joe-footer` c68463f), mocks, not for merging

`classic_fw.py` is `drawJoeClassic()` as written in the sketch, ported line for line: the scanline fill, the hexagon segments, the DSEG glyphs with ghosts, the moon with GFX's own `drawCircle`, the battery and the walker. `fw-sheet.png` shows four faces it rendered: today in light, a FROM count in dark, a dead sensor (`--`) on a low cell, and a five-figure day at 100%. `fw-*-1x.png` are the same at panel size.

How OP chose: `gshock-sheet.png` (two directions; he picked 1), `c1-sheet.png` (the bezel word; he picked STEP COUNTER), `classic2-sheet.png` (where weather and moon go; layout B).

To re-render, run `python3 classic_fw.py` (needs Pillow; it reads the DSEG7 headers from a checkout at ~/wt/joe-footer).
