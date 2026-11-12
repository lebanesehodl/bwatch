// Receive-only Lightning checks. No networking, allocation or private keys.
// Protocol sources and integration contract: docs/security/lightning-validation.md
#ifndef BWATCH_LN_VALIDATION_H
#define BWATCH_LN_VALIDATION_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <utility/trezor/ecdsa.h>
#include <utility/trezor/secp256k1.h>
#include <utility/trezor/sha2.h>

namespace bwatch_ln {

// Existing RTC invoice storage is 420 bytes, including the terminating NUL.
// Reject longer invoices; never truncate a payment request into that buffer.
static const size_t MAX_INVOICE_CHARS = 419;

struct Invoice {
  uint64_t amountMsat;
  uint64_t timestamp;
  uint64_t expiresAt;
  uint8_t paymentHash[32];
};

enum class Error { OK, LENGTH, ENCODING, NETWORK, AMOUNT, FIELDS,
                   DESCRIPTION, FEATURES, SIGNATURE, CLOCK, EXPIRED };

namespace detail {
inline char lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
inline bool digit(char c) { return c >= '0' && c <= '9'; }
inline bool alpha(char c) { c = lower(c); return c >= 'a' && c <= 'z'; }
inline int hex(char c) {
  if (digit(c)) return c - '0';
  c = lower(c);
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
inline int word(char c) {
  const char *alphabet = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
  const char *p = c ? strchr(alphabet, lower(c)) : nullptr;
  return p ? static_cast<int>(p - alphabet) : -1;
}
inline uint32_t polymod(uint32_t chk, uint8_t v) {
  static const uint32_t gen[] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa,
                                 0x3d4233dd, 0x2a1462b3};
  uint32_t top = chk >> 25;
  chk = ((chk & 0x1ffffff) << 5) ^ v;
  for (unsigned i = 0; i < 5; ++i) if ((top >> i) & 1) chk ^= gen[i];
  return chk;
}
// Five-bit field decoding, with canonical zero padding (no extra full byte).
inline bool bytes(const uint8_t *words, size_t n, uint8_t *out, size_t count) {
  if (n != (count * 8 + 4) / 5) return false;
  uint32_t acc = 0; unsigned bits = 0; size_t written = 0;
  for (size_t i = 0; i < n; ++i) {
    acc = (acc << 5) | words[i]; bits += 5;
    if (bits >= 8) { bits -= 8; out[written++] = (acc >> bits) & 255; }
  }
  return written == count && (acc & ((1u << bits) - 1)) == 0;
}
inline bool integer(const uint8_t *words, size_t n, uint64_t &out) {
  out = 0;
  for (size_t i = 0; i < n; ++i) {
    if (out > (UINT64_MAX - words[i]) / 32) return false;
    out = out * 32 + words[i];
  }
  return true;
}
inline bool amount(const char *hrp, size_t n, uint64_t &out) {
  // The caller checks lnbc. A missing/zero amount is not an amount-bound invoice.
  if (n <= 4 || hrp[4] < '1' || hrp[4] > '9') return false;
  uint64_t value = 0; size_t i = 4;
  while (i < n && digit(hrp[i])) {
    unsigned d = static_cast<unsigned>(hrp[i++] - '0');
    if (value > (UINT64_MAX - d) / 10) return false;
    value = value * 10 + d;
  }
  uint64_t scale = 100000000000ULL;  // BTC -> millisatoshis (BOLT11 HRP)
  if (i < n) {
    if (i + 1 != n) return false;
    switch (lower(hrp[i])) {
      case 'm': scale = 100000000ULL; break;
      case 'u': scale = 100000ULL; break;
      case 'n': scale = 100ULL; break;
      case 'p': if (value % 10) return false; out = value / 10; return out != 0;
      default: return false;
    }
  }
  if (value > UINT64_MAX / scale) return false;
  out = value * scale;
  return out != 0;
}
inline bool utf8(const uint8_t *s, size_t n) {
  for (size_t i = 0; i < n;) {
    uint32_t cp = s[i++];
    if (cp < 0x80) continue;
    unsigned more; uint32_t minimum;
    if (cp >= 0xc2 && cp <= 0xdf) { more = 1; minimum = 0x80; cp &= 31; }
    else if (cp >= 0xe0 && cp <= 0xef) { more = 2; minimum = 0x800; cp &= 15; }
    else if (cp >= 0xf0 && cp <= 0xf4) { more = 3; minimum = 0x10000; cp &= 7; }
    else return false;
    if (more > n - i) return false;
    while (more--) {
      if ((s[i] & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (s[i++] & 63);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
  }
  return true;
}
inline bool features(const uint8_t *words, size_t n) {
  if (!n || !words[0]) return false;  // non-minimal feature vector
  for (size_t i = 0; i < n; ++i) for (unsigned j = 0; j < 5; ++j) {
    if (!(words[n - i - 1] & (1u << j))) continue;
    size_t bit = i * 5 + j;
    // Known invoice semantics: variable onion, payment secret, basic MPP,
    // payment metadata. Unknown optional (odd) bits are safe to ignore.
    if (!(bit & 1) && bit != 8 && bit != 14 && bit != 16 && bit != 48) return false;
  }
  return true;
}
inline void signatureHash(const char *hrp, size_t hlen, const uint8_t *words,
                          size_t n, uint8_t hash[32]) {
  SHA256_CTX ctx; sha256_Init(&ctx);
  for (size_t i = 0; i < hlen; ++i) {
    uint8_t c = static_cast<uint8_t>(lower(hrp[i])); sha256_Update(&ctx, &c, 1);
  }
  uint32_t acc = 0; unsigned bits = 0;
  for (size_t i = 0; i < n; ++i) {
    acc = (acc << 5) | words[i]; bits += 5;
    if (bits >= 8) {
      bits -= 8; uint8_t b = (acc >> bits) & 255; sha256_Update(&ctx, &b, 1);
    }
  }
  if (bits) { uint8_t b = (acc << (8 - bits)) & 255; sha256_Update(&ctx, &b, 1); }
  sha256_Final(&ctx, hash);
}
inline bool signature(const uint8_t sig[65], const uint8_t hash[32],
                      const uint8_t *namedPayee) {
  if (sig[64] > 3) return false;
  uint8_t recovered[65];
  if (namedPayee) {
    bignum256 s; bn_read_be(sig + 32, &s);
    if (bn_is_less(&secp256k1.order_half, &s)) return false; // n tag requires low S
  } else {
    if (ecdsa_recover_pub_from_sig(&secp256k1, recovered, sig, hash, sig[64]) != 0)
      return false;
    namedPayee = recovered; // Without n, BOLT11 permits high S as well as low S.
  }
  return ecdsa_verify_digest(&secp256k1, namedPayee, sig, hash) == 0;
}
} // namespace detail

inline bool current(const Invoice &invoice, uint64_t nowUtc) {
  return nowUtc != 0 && invoice.timestamp <= nowUtc && nowUtc < invoice.expiresAt;
}

// metadataHash is SHA256 of the exact decoded LNURL metadata string, not JSON
// re-serialization. If the invoice uses h, it must match; no hash means reject h.
// Invoices using d are also supported (current LUD-06 does not require h).
// nowUtc must come from the watch's usable UTC clock, NOT a server Date header.
// Output is cleared on every failure so stale payment hashes cannot escape.
inline Error validateInvoice(const char *pr, size_t len, uint64_t expectedMsat,
                             uint64_t nowUtc, const uint8_t *metadataHash,
                             Invoice &out) {
  out = Invoice{};
  if (!pr || len > MAX_INVOICE_CHARS || len < 4 + 1 + 7 + 104 + 6) return Error::LENGTH;
  bool lo = false, up = false; size_t sep = 0;
  for (size_t i = 0; i < len; ++i) {
    unsigned char c = static_cast<unsigned char>(pr[i]);
    if (c < 33 || c > 126) return Error::ENCODING;
    lo |= c >= 'a' && c <= 'z'; up |= c >= 'A' && c <= 'Z';
    if (c == '1') sep = i;
  }
  if ((lo && up) || sep < 4 || len - sep - 1 < 7 + 104 + 6) return Error::ENCODING;
  if (detail::lower(pr[0]) != 'l' || detail::lower(pr[1]) != 'n' ||
      detail::lower(pr[2]) != 'b' || detail::lower(pr[3]) != 'c' ||
      (sep > 4 && !detail::digit(pr[4]))) return Error::NETWORK;
  Invoice invoice{};
  if (!detail::amount(pr, sep, invoice.amountMsat) || !expectedMsat ||
      invoice.amountMsat != expectedMsat) return Error::AMOUNT;

  uint32_t chk = 1;
  for (size_t i = 0; i < sep; ++i) chk = detail::polymod(chk, static_cast<uint8_t>(detail::lower(pr[i]) >> 5));
  chk = detail::polymod(chk, 0);
  for (size_t i = 0; i < sep; ++i) chk = detail::polymod(chk, detail::lower(pr[i]) & 31);
  uint8_t words[MAX_INVOICE_CHARS]; size_t count = len - sep - 1;
  for (size_t i = 0; i < count; ++i) {
    int w = detail::word(pr[sep + 1 + i]);
    if (w < 0) return Error::ENCODING;
    words[i] = static_cast<uint8_t>(w); chk = detail::polymod(chk, words[i]);
  }
  if (chk != 1) return Error::ENCODING; // Bech32, not Bech32m
  const size_t end = count - 104 - 6;
  detail::integer(words, 7, invoice.timestamp);
  uint64_t expiry = 3600; uint32_t seen = 0;
  uint8_t payee[33] = {}, scratch[(MAX_INVOICE_CHARS * 5) / 8 + 1];
  for (size_t pos = 7; pos < end;) {
    if (end - pos < 3) return Error::FIELDS;
    unsigned tag = words[pos++];
    size_t n = static_cast<size_t>(words[pos]) * 32 + words[pos + 1]; pos += 2;
    if (n > end - pos) return Error::FIELDS;
    const uint8_t *data = words + pos;
    const uint32_t mask = uint32_t(1) << tag;
    // Repeated route/fallback/unknown fields are allowed, critical fields aren't.
    if (tag == 1 || tag == 16 || tag == 13 || tag == 23 || tag == 19 ||
        tag == 6 || tag == 24 || tag == 5 || tag == 27) {
      if (seen & mask) return Error::FIELDS;
      seen |= mask;
    }
    switch (tag) {
      case 1: // p: payment hash
        if (!detail::bytes(data, n, invoice.paymentHash, 32)) return Error::FIELDS;
        break;
      case 16: // s: payment secret, mandatory even though we never pay
        if (!detail::bytes(data, n, scratch, 32)) return Error::FIELDS;
        break;
      case 23: // h: hashed description
        if (!detail::bytes(data, n, scratch, 32)) return Error::FIELDS;
        if (!metadataHash || memcmp(scratch, metadataHash, 32)) return Error::DESCRIPTION;
        break;
      case 19: // n: named payee
        if (!detail::bytes(data, n, payee, 33) || (payee[0] != 2 && payee[0] != 3))
          return Error::FIELDS;
        break;
      case 13: // d: UTF-8 description (never rendered or logged here)
        if (!detail::bytes(data, n, scratch, n * 5 / 8) ||
            !detail::utf8(scratch, n * 5 / 8)) return Error::DESCRIPTION;
        break;
      case 6: case 24: { // x / c: bounded, minimally encoded integers
        uint64_t v;
        if ((n && data[0] == 0) || !detail::integer(data, n, v)) return Error::FIELDS;
        if (tag == 6) expiry = v;
        break;
      }
      case 5: if (!detail::features(data, n)) return Error::FEATURES; break;
      case 27: // m: byte-aligned payment metadata, forwarded by the payer
        if (!detail::bytes(data, n, scratch, n * 5 / 8)) return Error::FIELDS;
        break;
      default: break; // payer handles routes and fallback address semantics
    }
    pos += n;
  }
  if (!(seen & (1u << 1)) || !(seen & (1u << 16)) ||
      !!(seen & (1u << 13)) == !!(seen & (1u << 23))) return Error::FIELDS;
  if (expiry > UINT64_MAX - invoice.timestamp) return Error::FIELDS;
  invoice.expiresAt = invoice.timestamp + expiry;
  if (!nowUtc || nowUtc < invoice.timestamp) return Error::CLOCK;
  if (!current(invoice, nowUtc)) return Error::EXPIRED;
  uint8_t sig[65], hash[32];
  if (!detail::bytes(words + end, 104, sig, sizeof(sig))) return Error::SIGNATURE;
  detail::signatureHash(pr, sep, words, end, hash);
  if (!detail::signature(sig, hash, (seen & (1u << 19)) ? payee : nullptr))
    return Error::SIGNATURE;
  out = invoice;
  return Error::OK;
}

inline bool requestedMsat(uint64_t sats, uint64_t &out) {
  out = 0;
  if (!sats || sats > UINT64_MAX / 1000) return false;
  out = sats * 1000;
  return true;
}
inline bool withinRange(uint64_t requested, uint64_t minimum, uint64_t maximum) {
  return minimum > 0 && minimum <= maximum && requested >= minimum && requested <= maximum;
}
// Full uint64_t conversion. In particular: never String((long)msat) on ESP32.
inline bool decimal(uint64_t value, char *out, size_t capacity) {
  char reversed[20]; size_t n = 0;
  do { reversed[n++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
  if (!out || capacity <= n) { if (out && capacity) out[0] = 0; return false; }
  for (size_t i = 0; i < n; ++i) out[i] = reversed[n - i - 1];
  out[n] = 0; return true;
}

// A deliberately narrow HTTPS URL grammar, compatible with HTTPClient:
// DNS/IPv4 authority and optional numeric port; no userinfo, fragments, spaces,
// controls, backslashes, percent-encoded host or IPv6 literal ambiguity.
// TLS must ALSO verify the URL host; this function is not authentication.
inline bool httpsUrl(const char *url, size_t n) {
  if (!url || n <= 8 || memcmp(url, "https://", 8)) return false;
  for (size_t i = 8; i < n; ++i) {
    unsigned char c = static_cast<unsigned char>(url[i]);
    if (c <= 32 || c >= 127 || c == '\\' || c == '#') return false;
    if (c == '%' && (i + 2 >= n || detail::hex(url[i + 1]) < 0 || detail::hex(url[i + 2]) < 0))
      return false;
  }
  size_t end = 8;
  while (end < n && url[end] != '/' && url[end] != '?') ++end;
  size_t hostEnd = 8;
  while (hostEnd < end && url[hostEnd] != ':') ++hostEnd;
  if (hostEnd == 8 || hostEnd - 8 > 253) return false;
  size_t label = 0;
  for (size_t i = 8; i < hostEnd; ++i) {
    char c = url[i];
    if (c == '.') {
      if (!label || url[i - 1] == '-') return false;
      label = 0;
    } else {
      if (!detail::alpha(c) && !detail::digit(c) && c != '-') return false;
      if ((!label && c == '-') || ++label > 63) return false;
    }
  }
  if (!label || url[hostEnd - 1] == '-') return false;
  if (hostEnd < end) {
    if (hostEnd + 1 == end) return false;
    uint32_t port = 0;
    for (size_t i = hostEnd + 1; i < end; ++i) {
      if (!detail::digit(url[i]) || port > 6553) return false;
      port = port * 10 + static_cast<uint32_t>(url[i] - '0');
    }
    if (!port || port > 65535) return false;
  }
  return true;
}

// Call only after strict JSON checks: status == "OK", settled is boolean true.
// LUD-21 proves knowledge of the invoice's preimage, NOT that a dishonest
// custodian credited an account. TLS/provider trust remain essential.
inline bool settlementProof(const Invoice &invoice, const char *outstanding, size_t n,
                            const char *returned, size_t returnedLen,
                            const char *preimageHex, size_t preimageLen) {
  if (!outstanding || !returned || !preimageHex || !n || n > MAX_INVOICE_CHARS ||
      n != returnedLen || memcmp(outstanding, returned, n) || preimageLen != 64)
    return false;
  uint8_t preimage[32], hash[32];
  for (size_t i = 0; i < 32; ++i) {
    int hi = detail::hex(preimageHex[i * 2]), lo = detail::hex(preimageHex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    preimage[i] = static_cast<uint8_t>(hi * 16 + lo);
  }
  sha256_Raw(preimage, sizeof(preimage), hash);
  return memcmp(hash, invoice.paymentHash, sizeof(hash)) == 0;
}

} // namespace bwatch_ln
#endif
