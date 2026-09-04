# Per-unit build checklist

One pass per watch. The battery calibration is the step most easily forgotten and
the one that makes the percentage meaningful, so it is not optional.

- [ ] Flash `firmware/BitcoinChronograph`
- [ ] Charge to full on a wall adapter (corner reads `CHG`, then `DOCK` above 95%)
- [ ] About → **hold UP** to store this board's full-charge reading
- [ ] Setup WiFi — confirm `Keys 1 of 3` in About
- [ ] Setup Wallet with a test zpub, confirm a balance, then clear it
- [ ] Set Timezone
- [ ] Walk every mode: HGT PRC SAT CAP FEE SUP HLV WAL JOE
- [ ] Turn every dial: HGT has seven stops until block 1,000,000, six after
- [ ] Long-press UP — corner should read `FTCH`, then the face refreshes
- [ ] Time Travel: forward, back, and **hold BACK** to come home
- [ ] Wear it one day; note the percentage before and after
- [ ] Engrave the caseback with the block that confirmed payment
- [ ] Pack: watch, hex key, two spare M2 screws, cable, card, manual

## Known-good build settings

| | |
|---|---|
| Board | ESP32S3 Dev Module |
| Flash | 8MB |
| Partition | 8M with spiffs |
| Core | 2.0.17 |
| Upload speed | 115200 |

`Rtc_Pcf8563 claims to run on avr architecture` is expected and harmless — the
Watchy library bundles that driver for earlier revisions. v3 does not use it.
