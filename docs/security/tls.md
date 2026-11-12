# TLS on the bw–1

Status: proposed · 2026-11-11 · reviewed at `511ed7a` by three models independently (Opus, Astra, Sol) and checked against the repo by Lead.

This note exists because the firmware does not authenticate any HTTPS server it talks to.
It says what that costs, where, and what the fix is. It is a design note, not a change:
the code work is split into the two PRs at the end.

---

## The problem in one paragraph

Every HTTPS connection in `firmware/BitcoinChronograph/BitcoinChronograph.ino` calls
`WiFiClientSecure::setInsecure()`. That keeps the traffic encrypted but accepts any
certificate, so nothing proves the far end is who it claims to be. An attacker who
can get on-path — a malicious access point, or ARP or DNS spoofing on a network that
allows it — can answer any request with their own certificate. Passively sharing the
Wi-Fi is not enough to read TLS; being on-path is.

The watch itself holds no spending keys, so this cannot move existing coins. What it
can do is redirect an incoming Lightning payment, convince the wearer that a payment
arrived when it did not, and falsify every reading on the face.

## What an attacker on-path can do

| Attack | Where | Effect |
|---|---|---|
| **Invoice substitution** | `fetchLnInvoice()`, ~2448–2491 | The lookup (`/.well-known/lnurlp/<user>`) and its callback are both unauthenticated. The only check on the returned invoice is `pr.startsWith("ln")`. The attacker returns their own valid BOLT11 invoice and their own `verify` URL; the wearer shows the QR and the payer pays the attacker. A valid BOLT11 signature does not bind an invoice to the configured Lightning address. The label "N SATS INVOICE" comes from `LN_REQUEST_SATS`, not from the invoice, so the amount can differ too. The forged invoice stays on screen for up to 10 minutes (~4777). |
| **Fake settlement** | `lnSettled()`, ~2498 | The `{"settled":true}` answer from the unauthenticated verify URL is trusted. It is not checked against the outstanding invoice or its payment hash. Someone can walk away with goods while the watch buzzes PAID. |
| **Fake on-chain payment** | ~2616, ~11294, ~12130 | A forged `funded_txo_sum` shows "payment received +N sats", adds to `walletSats`, and advances and persists `recvIndex` (2641, 11309, 12160). The receive QR itself cannot be substituted: the watch derives that address itself. |
| **Forged block height** | ~1588–1591, ~12046–12048 | Any height ≥ 100,000 is accepted and height only moves up. `blockHeight` lives in RTC memory, so one forged `2000000000` sticks until reboot and the supply and halving faces are wrong. `2147483647` overflows `h + 1` in `supplyBTC()` and supply reads 0. |
| **Falsified readings** | every client | Prices, fees, market cap, fear and greed, mempool, block reward, pool, retarget estimate. |
| **Privacy** | ~2775 and the gap scan | An on-path attacker reads every address the wallet scans (both chains, up to a gap of 5, hard cap 60) and therefore the whole wallet and its balance. Even with TLS fixed, `blockstream.info` still sees those addresses and the watch's IP; that is a separate privacy decision (open item #3 in the Team Brief). |

**Default-build qualification.** `LN_REQUEST_SATS` is `0` (line 329), so the
invoice-fetch path is disabled by default and the wallet face shows a
`lightning:user@domain` address for the payer's own wallet to resolve. That does not
fix the other insecure requests, and a customer who sets an amount turns the
invoice path on.

## The 14 call sites

All line numbers in the reviewed `.ino` at `511ed7a`. Helpers that receive one of
these clients inherit the same hole.

| Line | Path |
|---|---|
| 1241 | Coinbase rates and gold |
| 1540 | Shared main-fetch client: `api.exchange.coinbase.com`, `api.alternative.me`, `mempool.space`, `blockstream.info`; candles, sentiment, chain, fee, mining, transactions |
| 2456 | Lightning address lookup **and** invoice callback |
| 2498 | Lightning settlement verify |
| 2616 | On-chain payment vigil |
| 2775 | Wallet balance and address scan (`blockstream.info`) |
| 2976 | Historical block and price lookup |
| 7268 | FRED and Stooq market and basket data (follows redirects) |
| 10563 | Shipping seal height and hash |
| 11294 | Late on-chain payment detection |
| 12043 | Docked tip polling |
| 12065 | Docked rates and mempool polling |
| 12130 | Docked on-chain payment detection |
| 12175 | Docked block reward and miner data |

`PRICE_URL` (CoinGecko) is defined at line 335 and never used, so the watch never
contacts CoinGecko.

## The fix

### One shared helper, no `setInsecure()`

```cpp
static void tlsFixed(WiFiClientSecure &c) {   // hosts baked into the firmware
  c.setCACert(ROOTS_FIXED);                   // one PEM, roots back to back
  c.setHandshakeTimeout(10);                  // seconds; the default is 120
}
static void tlsAny(WiFiClientSecure &c) {     // customer Lightning domains
  c.setCACertBundle(x509_crt_bundle);
  c.setHandshakeTimeout(10);
}
```

- Keep `HTTPClient.begin(client, httpsUrl)`: core 2.0.17 verifies the URL's hostname
  against the certificate when validation is on.
- `setHandshakeTimeout()` takes **seconds**. At the default of 120 s a stalled
  handshake keeps the radio on for two minutes — real battery on this watch.
- `setCACertBundle()` in 2.0.17 does **not** clear a previously set insecure flag.
  Remove `setInsecure()`; do not add a bundle after it. Simply deleting
  `setInsecure()` without trust material makes every connection fail, so both land
  together.
- Only ever pin **roots**, never intermediates or leaf keys. Hosts re-issue
  certificates freely; they rarely change CA.
- On failure: log `lastError()`, show `CERT` in the corner, keep stale data, and
  **never** fall back to insecure. The Lightning view already degrades safely to the
  plain address QR.

### Which roots

Fixed hosts — one PEM containing:

| Root | Covers |
|---|---|
| ISRG Root X1, X2, Root YE, Root YR | `blockstream.info` (Let's Encrypt chain) |
| Sectigo Public Server Authentication Root R46, USERTrust RSA | `mempool.space` (Sectigo OV R36) |
| GTS Root R1, R4 | `*.coinbase.com` (Google Trust Services) |
| Amazon Root CA 1, 3 | `*.coinbase.com` (Amazon, parallel certificates) |

The bundle (Mozilla's) covers every customer Lightning domain, the callback and
verify URLs an LNURL provider returns (LUD-06/21 allow any host), and any host not
listed above until it is confirmed: `api.exchange.coinbase.com`, `api.alternative.me`,
`fred.stlouisfed.org`, `stooq.com`.

Arduino core 2.0.17 ships no ready-made bundle for sketches. Generate it with the
core's `tools/gen_crt_bundle.py` and include it as a `static const uint8_t[]`
(`xxd -i`). It must stay `static`: the bundle code keeps pointers into it. About
64 KB of flash.

**Refresh dates.** ISRG Root X1 stops being trusted on 2030-06-04; X2 to 2035-09-04;
GTS R1–R4 to 2036-06-22. A host moving to a **different CA** is the likely break, not
an expiry.

### Expiry is *not* checked, and that is deliberate

Core 2.0.17's ESP32-S3 SDK is built with `CONFIG_MBEDTLS_HAVE_TIME=y` and
`# CONFIG_MBEDTLS_HAVE_TIME_DATE is not set`
(`tools/sdk/esp32s3/sdkconfig`, lines 1652–1653). `mbedtls_x509_time_is_past()`
therefore always returns 0: certificate dates are never validated. Verification
still covers the chain to a trusted root and the hostname, which is what closes this
hole.

- Setting the system clock (NTP or otherwise) before connecting is **unnecessary**.
- The residual risk is an expired certificate whose key leaked: it stays accepted.
  Enable `MBEDTLS_HAVE_TIME_DATE` only with a trustworthy clock, and note that a dead
  clock then takes the watch off the network.

## Application-level checks the TLS fix does not cover

Do these in the Lightning PR; TLS stops substitution, not a hostile provider
response.

- **BOLT11:** require `lnbc` (refuse `lntb`, and the other networks), check the
  amount equals the requested msat, check expiry, reject oversized invoices instead
  of truncating into the 420-byte buffer.
- **Amount:** `String((long)msat)` is 32-bit here; a `minSendable` above
  2,147,483,647 msat wraps. Raising the request up to `minSendable` silently asks for
  more than the wearer configured — refuse instead.
- **LUD-21:** require that `pr` equals the invoice on screen and that
  `sha256(preimage)` equals its payment hash before believing `settled`.
- **Callback and verify URLs:** require HTTPS, authenticate the host, and do not
  follow redirects to plain HTTP.
- **Height input:** bound it. Reject anything above a sane ceiling
  (e.g. `blockHeight + 200`) and anything that would overflow `h + 1`; let a bad
  answer be a non-answer.
- **Fiat / secondary reads:** label anything derived from an unchecked host as such
  until it is confirmed, per the "never show a number you cannot defend" rule.

## Supply maths: correct, with qualifications

`supplyBTC()` / `supplyCapBTC()` (3452–3476) accumulate integer satoshis, count
`h + 1` blocks including genesis, and halve by right-shift. The schedule cap is

```
210,000 × Σ(e=0..32) (5,000,000,000 >> e) = 2,099,999,997,690,000 sats
                                          = 20,999,999.9769 BTC
```

The last paying block is 6,929,999; the subsidy is zero from 6,930,000. This matches
Bitcoin Core's `GetBlockSubsidy`. Checked at every height 1 → 13,860,000 against an
independent integer accumulator: no mismatch. Spot checks: height 1 = 100 BTC,
209,999 = 10,500,000, 210,000 = 10,500,025, 6,929,999 = 20,999,999.9769.

Small things worth knowing:

- `supplyBTC(0)` returns 0, though its own counting makes genesis 50 BTC. Zero also
  doubles as "no height", so separating the two is a careful change, not a
  one-line one.
- Integer accumulation is exact; the `double` return and the two-decimal face are
  not. Keep integer sats authoritative where exactness matters.
- The face calls `supplyBTC(estHeight())`, so a dead-reckoned height gives an
  estimated supply. That must stay labelled.
- `Halving heldHalving()` (~1474) uses `50.0 / pow(2,n)` rather than
  `subsidySats(n)` and drifts from Bitcoin Core from the 10th halving.
- "Issued" is not "circulating": unclaimed rewards, the unspendable genesis output,
  the two duplicate coinbases (91842, 91880) and burns all separate the two. Say
  "issued".

## Work split

**PR 1 — TLS on fixed hosts, and the height check.** No invoice fetching, so it works
on the default build.
- `tlsFixed()` on 12 fixed-host clients; bundle on 1540 and 7268 until those hosts are
  confirmed.
- Height input bounds; halving-card subsidy source.
- Files: `firmware/…/BitcoinChronograph.ino` (+ `docs/ASSEMBLY.md` if setup text changes).
- Reviewers: Bitcoin (security), Lead.
- Wrist test: every fetch still works; a deliberately wrong root fails closed with
  `CERT`.

**PR 2 — the Lightning path.**
- Bundle on the lookup, callback and verify clients.
- BOLT11 network/amount/expiry checks; `(long)msat`; refusal over `minSendable`;
  LUD-21 preimage check; HTTPS-only callback and verify.
- Reviewers: Bitcoin, Firmware, Lead.
- Wrist test: set `LN_REQUEST_SATS`, fetch an invoice, pay it, watch the buzz; try a
  hostile `minSendable` and a `lntb` invoice.

## What is still unproven

- The root list above comes from Certificate Transparency logs and CA pages, not
  from the chains the servers actually send. Test each host **on the watch** before
  merging; nothing in a sandbox can settle it.
- No ESP32 compile and no hardware interception test was run for any of the three
  reviews.
- Heap and flash cost of the bundle on this board is unmeasured.

---

Sources: three independent reviews of `511ed7a` (2026-11-11), cross-checked against
the repo and against `arduino-esp32` 2.0.17 (`WiFiClientSecure.cpp`,
`ssl_client.cpp`, `esp_config.h`, `tools/sdk/esp32s3/sdkconfig`) and Bitcoin Core
`v28` `validation.cpp` / `validation_tests.cpp`.
