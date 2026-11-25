# BITCOIN IS TIME: typefaces (mock only, not for merging)

**OP, Nov 25:** keep BITCOIN IS TIME and the ship card as they are for now. OP didn't like options A–C and asked for something closer to the brand and website. That is `brand-sheet.png` (`brand.py`), shown first below.

## The brand's typeface: Archivo
- **Why Archivo:** `bwatch.html` and `index.html` set every heading in Archivo ("it is the brand voice and it matches the mark"). The mark is an Archivo 600 B in a rounded square. The site serves its own `fonts/archivo-600.woff2` and `-500`, and its h1 asks for weight 800.
- **Source:** google/fonts `ofl/archivo/Archivo[wdth,wght].ttf`, SIL OFL 1.1, with no Reserved Font Name. It's instanced at wght 600 (the font's default and the site's file) and at 800, both at wdth 100, with fontTools, then run through `fontconvert`.
- **What changes:** only the big type. The screens keep today's white on black and the same words in the same places. The tracking is -1 px, after the site's `letter-spacing: -.045em`.
- **Sizes:** 18 pt for BITCOIN / IS TIME, 15 pt for BWATCH, and 9 pt for the boot card's bottom line, which stays above y 184.
- **Flash, A–Z and space:**
  - 600: 3.5 KB + 2.6 KB + 1.2 KB, about 7.3 KB for all three sizes.
  - 800: 3.7 KB + 2.8 KB + 1.3 KB.
  - Fewer sizes cost less.
- The small labels stay in the built-in 5×7. The site's labels are SF Mono, which is Apple's and can't ship, so the 5×7 stays in its place.

## The first three options (not chosen)

`type-sheet.png` has four columns: the resting screen and the boot card, each in light and dark. The first row is **now** (always white on black), followed by one row per option. `screens.py` draws it from real `fontconvert` output (Adafruit-GFX 1.13.0's, the tool behind every font in the sketch), using GFX's own drawing rules. Small text stays in the built-in 5×7, as it is everywhere else.

## The screens
- **Resting screen:** `restingScreen()` (`.ino:5402` at `1e2ebfc`). It's the screen the watch leaves up when the cell runs out (`battVoltsTrue() < 3.36f`, `:8349`). The words stay the same: BITCOIN / IS TIME, the last block seen, CHARGE TO RESUME.
- **Boot card:** the card that ends SEGMENT TEST (`bootSegmentTest()`, `:6160–6170`). It has BWATCH, SEGMENT TEST OK, the build stamp, and BITCOIN IS TIME, with that last line kept above y 184 for `bootStatus()`.

## The ship card
`ship-sheet.png` shows `drawShipCard()` (`.ino:11058`), the card left on the glass in the box, today and with the headline in each typeface. Its text and the sealed block (`drawGrouped()` in DSEG7 25) are unchanged. The block's hash tail here is example data. Today the card is always dark, by design ("the card is always dark"), so the light versions are only for comparison. When there's no block to show, the card has `drawTinyB()` instead of the height, which isn't mocked.

## The options
| | typeface | source and licence | sizes | flash, A–Z + space |
|---|---|---|---|---|
| **A** | DSEG14 Classic Bold: 14-segment LCD letters, like JOE's digits. The height uses JOE's DSEG7 18 with ghost 8s, a font that's already in the sketch. | keshikan/DSEG v0.46, SIL OFL 1.1, **Reserved Font Name "DSEG"** | 14 pt (27 px caps), 7 pt | ~2.7 KB + ~1.1 KB |
| **B** | Michroma: a wide, square sans like the type on a watch dial | googlefonts/Michroma, SIL OFL 1.1, no reserved name | 12 pt (19 px caps), 7 pt | ~2.8 KB + ~1.3 KB |
| **C** | Cinzel (regular weight): Roman capitals like a caseback engraving | NDISCOVER/Cinzel, SIL OFL 1.1, no reserved name | 16 pt (23 px caps), 10 pt | ~2.9 KB + ~1.4 KB |

- **Flash cost:** bitmaps plus 8-byte glyph records, measured from the generated headers (`gfx/`). The full printable ASCII set would be about 4.0–4.5 KB for the headline size plus 1.8–2.2 KB for the small size. For comparison, the sketch is 1.52 MB of a 3.34 MB partition.
- **DSEG's reserved name:** the sketch already ships `DSEG7_Classic_Bold_*` conversions under that name. Whether a converted header may keep the "DSEG" name is an OFL question that DSEG14 would share with them, so settle it before adding more.
- **Ghosting on A:** the mock draws DSEG14 without ghost segments. Ghosting all 14 segments behind letters (`~`) turned them into noise at one bit per pixel.

## Following the theme: what else it touches
- **`restingScreen()`** doesn't use `bootClear()`. It fills black itself and uses `GxEPD_WHITE` 4 times, and `paintedDark = true` (`:5433`) would become `themeDark`. It doesn't touch anything else, so it could follow the theme on its own.
- **`bootClear()` (`:6012`)** is shared. Its callers are SEGMENT TEST (`:6125`, `:6160`), GENESIS (`:6179`), HALVINGS (`:6255`, which forces `themeDark` on for its numbers) and the ship card (`drawShipCard()`, `:11060`, "the card is always dark").
- **Hard-coded colours:** the boot code between `:6005` and `:6345` uses `GxEPD_WHITE` / `GxEPD_BLACK` directly 34 times. That covers `bootStatus()`, `syncBegin()` / `syncStep()`, the segment frames, GENESIS's hash rows and `bootRing()`. Changing `bootClear()` alone would leave white on white in the light theme.
- **"Every boot sequence ends on black":** `paintedDark = true` at `:8428` and `:10014` assumes it.
- **So:** theming the boot is a separate, larger change. The resting screen can follow the theme by itself, with no extra delay or refresh: it already ends on one `displayFull()`.
