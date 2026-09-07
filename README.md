# BWATCH bw–1

A wristwatch that keeps Bitcoin's time. No app, no account, no custodian.

Inspired by the digital watches of my youth, bwatch shows the height of the Bitcoin timechain, difficulty epochs, halvings, derives
issued supply on-device from the block height. It also carries a watch-only wallet that holds
no key, and vibrates once for every block the network finds, roughly one hundred and
forty-four times a day. The Bitcoin price is available in 5 different currencies along with SATS/USD (MSCW Time)

**Site:** [bwatch.xyz](https://bwatch.xyz) · **Manual:** [bwatch.xyz/bwatch-manual.html](https://bwatch.xyz/bwatch-manual.html)

---

## What's here

```
index.html              the site's front door
bwatch.html             the long document: claims, editions, specs, field log
bwatch-manual.html      the manual: every mode, every button
img/                    photographs and schematics
fonts/                  Archivo, self-hosted
brand/                  the mark and lockup, SVG and PDF
favicon.svg
404.html  robots.txt  sitemap.xml  CNAME

firmware/
  BitcoinChronograph/   the Arduino sketch and its fonts
hardware/
  case/                 the case split into parts, with drawings
```

The site is served from the repository root by GitHub Pages. The firmware sits
alongside it because they are the same project. The page makes claims and the code
is where you check them.

The case is the **Armadillonium**, SQFMI's official CNC anodized aluminium case for
Watchy. Buy it from them unless you need a colour they do not sell — `hardware/case`
holds the assembly split into its three parts and the manufacturing drawings that go
with it, which exist because the Genesis edition is orange and had to be machined.
The geometry is theirs, under MIT; the drawings are ours.

- Case: [shop.sqfmi.com](https://shop.sqfmi.com/products/watchy-cnc-anodized-aluminum-case)
  — top and bottom, four buttons, four M2×6 hex socket screws and a hex wrench, 30 g
- Files and other designs: [sqfmi/watchy-cases](https://github.com/sqfmi/watchy-cases)
- Watchy itself is MIT licensed and OSHWA certified

---

## Firmware

Built on [Watchy](https://watchy.sqfmi.com) hardware (**v3**, ESP32-S3, 200×200 e-paper).
The sketch subclasses the Watchy library rather than patching it, so it survives a
library update.

### Building

1. Arduino IDE with the ESP32 board package, core **2.0.17**
2. Board: **ESP32S3 Dev Module** · Flash **8MB** · Partition **8M with spiffs**
3. Upload speed **115200** — 921600 drops the connection on this board
4. Install the Watchy library, then open `firmware/BitcoinChronograph/BitcoinChronograph.ino`

The DSEG7 font headers and a local copy of `qrcodegen` are included; nothing else is
vendored.

### First run

| Step | Where |
|---|---|
| Charge to full | Plug in. Below 95% the watch declines the live dock and sleeps so the cell fills. |
| WiFi | MENU → Setup WiFi. Holds three networks — add a phone hotspot as one if you'd like. |
| Wallet | MENU → Setup Wallet. Paste a **zpub**, never a seed. |
| Timezone | MENU → Set Timezone. |
| Battery | Fully charged, open About and **hold UP**. Each board learns its own full-charge reading. |

iPhone hotspots default to 5 GHz; the watch is 2.4 GHz only, so turn on *Maximize
Compatibility* or it will never see the network.

---

## Design notes

Two rules run through the firmware and are worth knowing before changing it.

**Never show a number you cannot defend.** Block height is fetched, never inferred,
while the network is reachable. Once a fetch has actually failed the face switches to
dead reckoning and says so — `EST +36 BLK +/-6` — rather than presenting a guess as a
reading. The corner tag carries the age of everything on screen, so individual
readouts do not each need their own disclaimer.

**One fact, one place.** Most bugs in this build came from the same quantity living in
two places: the dial stop count, the DOWN handler, the LIVE/OLD threshold, the width
arithmetic, the label corner conventions. Where you see a helper that looks like
overkill for one caller — `cellStops()`, `backPressed()`, `battVoltsTrue()`,
`supplyCapBTC()` — it exists because the duplicated version shipped a bug.

Supply is measured against **20,999,999.9769 BTC**, not a round 21 million: each
subsidy is an integer number of satoshis and halving truncates.

---

## Building one yourself

| Part | Where |
|---|---|
| Watchy v3 | [SQFMI](https://shop.sqfmi.com) · Mouser · The Pi Hut |
| Armadillonium aluminium case | [SQFMI](https://shop.sqfmi.com/products/watchy-cnc-anodized-aluminum-case) |
| 20 mm strap | any |
| This firmware | `firmware/BitcoinChronograph` |

The case ships with its own screws and hex wrench, so nothing else is needed. Flash,
set WiFi, paste a zpub, calibrate the battery once, and it is the same watch.

## The mark

`brand/` holds the mark and the lockup as SVG and PDF, letterforms converted to
paths. The field is not a generic rounded square: it is the bw-1 case at its true
proportion — 40.40 × 50.36, corner radius 5.4 — traced from the same STEP file the
shop machines from, so the mark and the object cannot drift apart.

Orange `#FF3B12` · ink `#14120F` · paper `#F2EFE6` · Archivo SemiBold.

## Licence

MIT for the firmware. Build your own — the sketch is here, free, forever, and runs
on stock Watchy hardware. The editions exist for people who would rather have one made.

Photographs are © BTC Technologies and not covered by the MIT licence.
