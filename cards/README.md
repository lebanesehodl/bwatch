# Anniversary cards: classic JOE redesign (mock only, not for merging)

`cards-sheet.png`: each row is one card. The four columns are **now** (a line-for-line port of `drawStatCard()` at `1e2ebfc`) and **classic**, each in light and dark. `cards.py` makes all of it, and its `before()` is the port.

The cards' text is what `drawPersonCard()` / `drawWatchCard()` build. Nothing is added or dropped, only moved:

| card | `top` | title | number | `l1` | `l2` | `l3` |
|---|---|---|---|---|---|---|
| person, long name | 1 OF 3 - BACK FOR NEXT | MARGARET (8, the most a name can be) | 812,345 | BLOCKS SEEN | BORN AT BLOCK 157,616 | SEEN 84% OF THE CHAIN |
| before genesis, big count | 2 OF 3 - BACK FOR NEXT | GRANDPA | 2,601,234 | BLOCKS OLD, AT TEN MINUTES | BORN 1,628,889 BEFORE GENESIS | BITCOIN IS 37% OF YOUR LIFE |
| not born yet | 3 OF 3 - BACK FOR NEXT | BABY | 4,211 (shown without a sign, as now) | BLOCKS UNTIL BORN | BORN AT BLOCK 974,173 | (empty) |
| the watch | THIS WATCH | BWATCH | 1,234 | BLOCKS WITNESSED | UNBOXED AT BLOCK 968,728 | 9 DAYS ON YOUR WRIST |

## The classic layout
- **Frame:** the bezel and window from `drawJoeClassic()`: `fillScreen(fg)`, then `fillRoundRect(6, 20, 188, 160, 10, bg)`.
- **Bezel:**
  - `top` at the top left, in bg (22 chars at most, 132 px).
  - `l3`, the punchline, centred on the bottom bezel. The longest is the blockday card's "NEXT: 1,000,000 IN ~3.2 YEARS", 29 chars or 174 px, which fits.
- **Window, top to bottom:**
  - The name: built-in font at size 2, at (14, 28).
  - A rule at y 50.
  - `l1`, right-aligned at 185, y 58, like JOE's TODAY label.
  - **The number:** a 7-place field of JOE's hexagon segments (w 21, h 50, t 5, gap 2, slant 0.08), right edge 186, top y 70. Places before the number are ghost 8s. Unlit segments are ghosted every third pixel, which is lighter than the time's every-second: the busier ghost made seven narrow digits hard to read. The two group marks are lit only where the number reaches them. Seven places hold 9,999,999; a 100-year-old is about 5.3 million blocks old.
  - A rule at y 130.
  - `l2`, centred at y 148. The longest is 29 chars, 174 px.
- **Unchanged:** every caller (person, watch, blockday, birthday), the numbers and how they're worked out, the UP/BACK cycle, and the refreshes.

## What an implementation needs
- **`drawSegDigit()`** needs two things it lacks today: a blank place (no segment lit) and a ghost-step argument. The alternative is a small field helper next to it.
- **`drawStatCard()`** is the only function that changes. The double frame goes, and the bezel replaces it.
- **Wrist test:** legibility of the 7-place field, and whether the bezel leaves a ghost when the card leaves. Today `leaveFace()` already calls `fullOnClose()` after dark cards.
