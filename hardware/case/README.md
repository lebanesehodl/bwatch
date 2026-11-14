# Case

BWATCH uses the **Armadillonium**, the official CNC anodized aluminium case for
Watchy, designed by SQFMI and published at
[sqfmi/watchy-cases](https://github.com/sqfmi/watchy-cases) under the MIT licence.

Buy it from [SQFMI](https://shop.sqfmi.com/products/watchy-cnc-anodized-aluminum-case)
unless you need a colour they do not sell. Everything here exists because the
Genesis edition is orange, which meant machining it.

## What is here

```
armadillonium-bottom.step   the thick shell, 10.10 mm, tapped, carries the board
armadillonium-top.step      the front bezel, 5.25 mm, open for the display
armadillonium-button.step   the pusher

armadillonium-bottom-shell-drawing.pdf
armadillonium-top-bezel-drawing.pdf
armadillonium-button-drawing.pdf
```

The STEP files are the original assembly split into its three parts, because
shops quote and machine one part per order. The drawings are ours.

## What the drawings say, and why

The published case files give you the shape. They do not tell you which
dimensions are critical, and that gap is what these sheets fill.

**Threads.** Four M2 × 0.4 – 6H, 2.35 min full thread, in the **bottom shell**
only. The screws pass through the bezel's ⌀1.62 clearance holes, which are
counterbored ⌀4.00 × 0.80 on the outer face.

**Button bores.** ⌀2.50, and each one is formed **half in the shell and half in
the bezel** — the bore only exists once the case is assembled. Hold ±0.05 on the
half-bore profile in each part.

**The fit is already designed.** The button post is ⌀2.30 in a ⌀2.50 bore:
0.20 mm of clearance. Do not tighten it. A tighter fit costs more and binds
once anodising adds its 5–15 µm per surface.

**Mask before anodising.** Threads and half-bores. Coating thickness lands
directly on the fit.

**The button flange is 0.80 mm.** That is below the 1.5 mm minimum wall some
shops enforce. It is the part as designed and it is in volume production, so
ask for an exception rather than thickening it.

## Verify before you spend

Every dimension here was measured from the STEP file with a CAD kernel, not
taken from a datasheet — there isn't one. Check them against a physical case
before committing to a run. The drawings say the same thing in their footer.
