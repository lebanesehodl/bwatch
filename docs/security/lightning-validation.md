# Lightning receive validation

Status: **portable helpers only; not yet wired into the sketch**. This is the
validation portion of PR B (`fw/ln-hardening`). Firmware owns the transport,
RTC/display integration and full ESP32 compile. Do not merge as a completed TLS fix.

## Scope and trust

`firmware/BitcoinChronograph/ln_validation.h` has no networking, heap allocation,
private-key input, seed input or signing. Its crypto operations reuse the SHA256
and secp256k1 verification/recovery code already in uBitcoin.

A BOLT11 signature does **not** bind a payment request to a Lightning address.
Without a named payee (`n`), changing signed data can recover a different valid
payee key. Authenticate the configured LNURL lookup and its supplied callback
using TLS; a checksum and signature alone cannot stop invoice substitution.

Similarly, a valid LUD-21 preimage proves knowledge of that invoice's secret,
not that a dishonest provider credited the customer. The invoice issuer already
knows the preimage. Do not describe this as trustless settlement verification.

The payer's wallet remains responsible for routing, fallback-address semantics
and payment execution. This is a bounded **receive-display validator**, not a
Lightning wallet or a complete replacement for the payer's invoice decoder.

## What the helpers enforce

- `validateInvoice(pr, length, expectedMsat, nowUtc, metadataHash, out)`:
  mainnet only, an explicit positive amount exactly equal to the request, Bech32
  checksum/case rules, bounded tags, one payment hash and secret, one description
  or description hash, valid UTF-8 for a description, canonical bit padding,
  bounded/minimal expiry, and a valid secp256k1 signature. A named payee requires
  low S; recovered payees accept either high S or low S per BOLT11.
- A hashed description must match `metadataHash`: SHA256 of the **exact decoded
  metadata string bytes**, not a JSON re-serialization. No supplied hash means
  reject an `h` invoice. Plain `d` descriptions are also accepted. The pinned
  current LUD-06 does **not** mandate an `h` field; do not cite it as requiring
  every provider to use hashed descriptions.
- Requests exceeding **419 characters** are rejected, not truncated. This matches
  the current `lnInvoice[420]` storage. Some legitimate invoices with routing
  hints are longer; the safe fallback is the plain address, not a shortened QR.
  On integration, add a `static_assert` tying this limit to the RTC buffer size.
- Unknown required feature bits are refused; optional unknown bits are ignored.
  The known required bits are 8, 14, 16 and 48 (variable onion, payment secret,
  basic MPP and payment metadata). Other mandatory extensions fall back safely.
- `current(invoice, nowUtc)` requires `timestamp <= nowUtc < expiresAt` and a
  nonzero clock. The boundary is exclusive; a future-dated invoice is refused.
  Default BOLT11 expiry is 3600 seconds. Validation clears its output on failure.
- `requestedMsat`, `withinRange`, `decimal`: checked 64-bit sats-to-msat
  conversion, a positive ordered min/max range with **no clamping**, and decimal
  serialization without a 32-bit `long`. A minimum above the user's request or
  a maximum below it is failure, not permission to change the amount.
- `httpsUrl`: strict HTTPS with a DNS/IPv4 authority and optional numeric port.
  Rejects userinfo, fragments, spaces/controls, backslashes, invalid percent
  escapes and malformed hosts/ports. Unicode hosts must be punycode. IPv6
  literals and trailing-dot names are deliberately unsupported. Hostname and
  certificate verification are still the TLS client's job.
- `settlementProof`: exact equality with the **stored original** invoice string,
  exactly 64 hex preimage characters, and SHA256(preimage) equal to the accepted
  invoice's payment hash. QR uppercase rendering must not overwrite the stored
  original. JSON status/boolean validation belongs at the integration boundary.

## Firmware integration contract — required before ready for review

1. Clear old invoice, verify URL and validated invoice state before a new request.
   Validate the configured address using LUD-16's username/domain grammar before
   forming the lookup URL. Never log invoice bodies, addresses, metadata, verify
   URLs or preimages. Error categories and TLS error codes are sufficient.
2. Create fresh clients with the maintained root bundle and a **10-second**
   handshake timeout for lookup, callback and verify. No `setInsecure()` and no
   insecure retry. Explicitly disable redirects for these requests. Validate
   callback and verify URLs before beginning a connection; allowing any provider
   host does not mean allowing HTTP or userinfo in a URL.
3. Bound response bytes, read time and JSON nesting **before** allocating an
   unrestricted `getString()`. Set explicit budgets for lookup metadata and
   invoice/verify responses. Require a JSON object, reject `status: ERROR`,
   require `tag: payRequest`, string callback/metadata, and unsigned integer
   min/max fields. Reject floats, strings pretending to be numbers, negatives,
   missing bounds and invalid metadata arrays. Do not coerce them to defaults.
4. Convert the configured positive sats amount to msat once, check it against
   min/max, and serialize it with `decimal` into a 21-byte temporary. Preserve
   the configured amount through callback, QR label and eventual payment event.
5. Validate the callback's complete string `pr` before copying it into RTC
   storage. Pass explicit JSON string lengths (embedded NUL must not turn a
   hostile value into a shorter accepted string). Hash the exact metadata bytes
   from the lookup. An absent optional verify URL means no settlement detection;
   a present but invalid/overlong URL must never be truncated or requested.
6. Obtain **fresh UTC** from the watch clock. Check the actual RTC/NTP/timezone
   behavior rather than copying a helper that may hold local civil time. An
   unset/unusable clock fails invoice mode closed. Certificate-date behavior in
   ESP32 core 2.0.17 is a separate issue; it does not remove BOLT11's expiry rule.
   Existing unauthenticated NTP does not provide cryptographic clock provenance.
7. Persist the validated payment hash and expiry/state if needed across sleep,
   and bump `rtcMagic` for every added/reordered RTC field. Clear the new state
   at every existing wallet reset, invoice replacement and consumption site.
   Recheck `current` on every cached QR draw, not only at fetch: the old 10-minute
   fetch-age check can display a 60-second invoice after expiry. E-paper can
   physically retain an expired QR while asleep; the payer must still enforce
   expiry. Measure whether an earlier wake/refresh is needed.
8. Before `settlementProof`, require `status` to be the string `OK`, `settled` to
   be an actual boolean `true`, and `pr`/`preimage` to be strings. The proof must
   refer to the still-outstanding accepted invoice; no proof means no PAID/buzz.
   Late settlement of an already-issued invoice may be checked after its display
   expiry; do not discard the hash solely because it is too old to show again.
9. Any TLS, parser, amount, size or clock failure falls back to the plain
   `lightning:user@domain` address QR with the existing failed-invoice message.
   That fallback is any-amount and **must not promise settlement detection**.
10. Compile and exercise both default `LN_REQUEST_SATS=0` and an enabled invoice
    build. Add the host test command to the established CI workflow after CI lands.

## Tests

```sh
# C/C++ compiler, Python 3, git; compiler ASan/UBSan runtimes required.
scripts/test-lightning.sh
# Optional existing checkout at the exact pinned uBitcoin commit:
UBITCOIN_DIR=/path/to/uBitcoin scripts/test-lightning.sh
# Clang libFuzzer, 100,000 bounded inputs, raw and checksum-repaired paths:
CC=clang CXX=clang++ scripts/test-lightning.sh --fuzz
```

The script pins uBitcoin to `877542fdc16319dd92a7d2a679ea9dacce474bd2`, checks that
its `src` is unmodified, and keeps builds outside the repository under the user's
cache. Both host tests and firmware use the same crypto implementation.

`tests/lightning/bolt11-vectors.json` is the unmodified 31-vector upstream fixture
at BOLTs commit `191462825ae341f0a27098765c075f01ca9bc6c2`. Of its valid examples,
11 fit this watch's policy; amountless, testnet and >419-character examples are
intentionally rejected. All 15 upstream invalid examples are rejected. Additional
tests cover amount units/overflow, 32-bit wrap, exact expiry boundaries, malformed
fields/padding/UTF-8, duplicate fields, missing secret/hash, output clearing,
419/420-byte limits, URL attacks and wrong/missing/malformed settlement proofs.

The upstream invalid high-S/n example also duplicates its `s` field. Tests reject
that structure and separately test its signature, including algebraic low-S
normalization. No test signs anything or imports a private key or seed.

Host sanitizers and fuzzing do not establish TLS interoperability, reliable UTC,
ESP32 stack/heap/flash use, display timing or payment-provider behavior.

## Privacy and needs wrist test

Once integrated, the configured Lightning identifier and watch IP go to its
LNURL host. The callback host receives the chosen msat amount and IP; the verify
host sees the invoice-specific URL and polling timing/IP. Callback/verify may
legitimately be on a different host. No xpub, private key or seed is needed by
these helpers. This patch alone sends nothing and changes no running sketch path.

Before OP merges the completed PR B:

- Test a real provider with amount mode enabled: correct QR amount/network,
  authenticated lookup/callback/verify, payment and exactly one settlement buzz.
- Test wrong/untrusted CA and hostname, HTTP/redirect callback or verify, truncated
  responses, wrong JSON types, min above request, max below request, wrong-network
  and wrong-amount invoices, >419-character invoices, and a bogus preimage/pr.
  Every rejection must leave no payable substituted invoice or false PAID event.
- Test a short invoice expiry on wrist and dock, stale/unset clock and timezone
  changes, deep sleep/reboot/reset, and late settlement/replacement races.
- Measure bundle + crypto stack/heap/flash cost and handshake/verification latency.
  The complete firmware must pass the pinned ESP32 2.0.17 CI build.

## Primary sources

- [BOLT11 encoding, units, signature, fields, expiry and examples](https://github.com/lightning/bolts/blob/191462825ae341f0a27098765c075f01ca9bc6c2/11-payment-encoding.md)
- [Upstream machine-readable fixtures](https://github.com/lightning/bolts/blob/191462825ae341f0a27098765c075f01ca9bc6c2/bolt11/invoice-test.json)
- [BOLT9 feature definitions](https://github.com/lightning/bolts/blob/191462825ae341f0a27098765c075f01ca9bc6c2/09-features.md)
- [LUD-06 pay request](https://github.com/lnurl/luds/blob/913c49e5b65081473eed7889eee5f806dd30f049/06.md)
- [LUD-16 address mapping](https://github.com/lnurl/luds/blob/913c49e5b65081473eed7889eee5f806dd30f049/16.md)
- [LUD-21 verify response](https://github.com/lnurl/luds/blob/913c49e5b65081473eed7889eee5f806dd30f049/21.md)
- [uBitcoin recovery and verification implementation](https://github.com/micro-bitcoin/uBitcoin/blob/877542fdc16319dd92a7d2a679ea9dacce474bd2/src/utility/trezor/ecdsa.c#L1026-L1129)

The copied BOLT fixtures are attributed to the Lightning BOLTs contributors and
licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), as stated
in BOLT11. They are unchanged; watch-specific policy expectations live in `test.cpp`.
