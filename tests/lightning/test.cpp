#include "ln_validation.h"
#include "vectors.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace bwatch_ln;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
static const char *alphabet = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
static const uint64_t TS = 1496314658;

static std::vector<uint8_t> unhex(const std::string &s) {
  std::vector<uint8_t> result;
  for (size_t i = 0; i < s.size(); i += 2)
    result.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
  return result;
}
static std::vector<uint8_t> metadataHash = unhex(
    "3925b6f67e2c340036ed12093dd44e0368df1b6ea26c53dbe4811f58fd5db8c1");

// Independent encoder for structural mutations. It never signs or takes keys.
// Without an n tag, signature recovery may yield a DIFFERENT valid payee after
// changing the signed data: TLS, not a self-recovered key, binds the LN address.
struct Parts { std::string hrp; std::vector<uint8_t> data; };
static Parts split(std::string invoice) {
  std::transform(invoice.begin(), invoice.end(), invoice.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  size_t sep = invoice.rfind('1');
  Parts p{invoice.substr(0, sep), {}};
  for (size_t i = sep + 1; i < invoice.size() - 6; ++i) {
    const char *w = strchr(alphabet, invoice[i]); CHECK(w);
    p.data.push_back(static_cast<uint8_t>(w - alphabet));
  }
  return p;
}
static uint32_t referencePolymod(const std::vector<uint8_t> &values) {
  uint32_t c = 1;
  for (uint8_t v : values) {
    uint32_t top = c >> 25;
    c = ((c & 0x1ffffff) << 5) ^ v;
    if (top & 1) c ^= 0x3b6a57b2;
    if (top & 2) c ^= 0x26508e6d;
    if (top & 4) c ^= 0x1ea119fa;
    if (top & 8) c ^= 0x3d4233dd;
    if (top & 16) c ^= 0x2a1462b3;
  }
  return c;
}
static std::string encode(const Parts &p) {
  std::vector<uint8_t> expanded;
  for (char c : p.hrp) expanded.push_back(static_cast<uint8_t>(c >> 5));
  expanded.push_back(0);
  for (char c : p.hrp) expanded.push_back(static_cast<uint8_t>(c & 31));
  expanded.insert(expanded.end(), p.data.begin(), p.data.end());
  expanded.resize(expanded.size() + 6);
  uint32_t checksum = referencePolymod(expanded) ^ 1;
  std::string result = p.hrp + '1';
  for (uint8_t w : p.data) result += alphabet[w];
  for (int i = 5; i >= 0; --i) result += alphabet[(checksum >> (5 * i)) & 31];
  return result;
}
static size_t field(const Parts &p, unsigned tag) {
  for (size_t i = 7; i + 3 <= p.data.size() - 104;) {
    if (p.data[i] == tag) return i;
    i += 3 + size_t(p.data[i + 1]) * 32 + p.data[i + 2];
  }
  return 0;
}
static void replace(Parts &p, unsigned tag, const std::vector<uint8_t> &data) {
  size_t pos = field(p, tag);
  if (pos) {
    size_t len = 3 + size_t(p.data[pos + 1]) * 32 + p.data[pos + 2];
    p.data.erase(p.data.begin() + static_cast<ptrdiff_t>(pos),
                 p.data.begin() + static_cast<ptrdiff_t>(pos + len));
  } else pos = p.data.size() - 104;
  std::vector<uint8_t> f{static_cast<uint8_t>(tag), static_cast<uint8_t>(data.size() >> 5),
                         static_cast<uint8_t>(data.size() & 31)};
  f.insert(f.end(), data.begin(), data.end());
  p.data.insert(p.data.begin() + static_cast<ptrdiff_t>(pos), f.begin(), f.end());
}
static void remove(Parts &p, unsigned tag) {
  size_t pos = field(p, tag); CHECK(pos);
  size_t len = 3 + size_t(p.data[pos + 1]) * 32 + p.data[pos + 2];
  p.data.erase(p.data.begin() + static_cast<ptrdiff_t>(pos),
               p.data.begin() + static_cast<ptrdiff_t>(pos + len));
}
static std::vector<uint8_t> words(const uint8_t *bytes, size_t n) {
  std::vector<uint8_t> out; uint32_t acc = 0; unsigned bits = 0;
  for (size_t i = 0; i < n; ++i) {
    acc = (acc << 8) | bytes[i]; bits += 8;
    while (bits >= 5) { bits -= 5; out.push_back(static_cast<uint8_t>((acc >> bits) & 31)); }
  }
  if (bits) out.push_back(static_cast<uint8_t>((acc << (5 - bits)) & 31));
  return out;
}
static uint64_t timestamp(const Parts &p) {
  uint64_t ts = 0;
  for (size_t i = 0; i < 7; ++i) ts = ts * 32 + p.data[i];
  return ts;
}
static Error check(const std::string &s, uint64_t msat = 250000000,
                   uint64_t now = TS, const uint8_t *metadata = metadataHash.data()) {
  Invoice out; memset(&out, 0x55, sizeof(out));
  Error e = validateInvoice(s.data(), s.size(), msat, now, metadata, out);
  if (e != Error::OK) {
    CHECK(out.amountMsat == 0 && out.expiresAt == 0 && out.timestamp == 0);
    for (uint8_t b : out.paymentHash) CHECK(b == 0);
  }
  return e;
}

static void protocolVectors() {
  const uint64_t requests[] = {1000,250000000,250000000,2000000000,2000000000,
    2000000000,2000000000,2000000000,2000000000,2000000000,967878534,2500000000,
    2500000000,2500000000,1000000000,1000,2500000000,2500000000,2500000000,1000,
    1000,2500000000,250000000,250000000,250000000,250000000,1000,250000000,
    250000000,2000000000,2500000000};
  static_assert(sizeof(requests)/sizeof(*requests) == sizeof(vectors)/sizeof(*vectors), "vector count");
  for (size_t i = 0; i < sizeof(vectors)/sizeof(*vectors); ++i) {
    bool allowed = vectors[i].valid && i != 0 && i != 4 && i != 5 && i != 10 && i != 15;
    uint64_t now = i == 30 ? timestamp(split(vectors[i].invoice)) : TS;
    Error e = check(vectors[i].invoice, requests[i], now);
    if ((e == Error::OK) != allowed) std::cerr << "vector " << i << " error " << int(e) << '\n';
    CHECK((e == Error::OK) == allowed);
  }
  Invoice invoice{};
  CHECK(validateInvoice(vectors[1].invoice, strlen(vectors[1].invoice), 250000000,
                        TS, nullptr, invoice) == Error::OK);
  CHECK(invoice.amountMsat == 250000000 && invoice.timestamp == TS && invoice.expiresAt == TS + 60);
  const auto hash = unhex("0001020304050607080900010203040506070809000102030405060708090102");
  CHECK(memcmp(invoice.paymentHash, hash.data(), 32) == 0);
  CHECK(current(invoice, TS) && current(invoice, TS + 59));
  CHECK(!current(invoice, 0) && !current(invoice, TS - 1) && !current(invoice, TS + 60));
  CHECK(check(vectors[1].invoice, 250000000, TS + 60) == Error::EXPIRED);
  CHECK(check(vectors[1].invoice, 250000000, 0) == Error::CLOCK);
  CHECK(check(vectors[1].invoice, 250000000, TS - 1) == Error::CLOCK);
  CHECK(check(vectors[1].invoice, 1) == Error::AMOUNT);
  CHECK(check(vectors[3].invoice, 2000000000, TS, nullptr) == Error::DESCRIPTION);
  uint8_t badHash[32] = {};
  CHECK(check(vectors[3].invoice, 2000000000, TS, badHash) == Error::DESCRIPTION);
}
static void boundaries() {
  std::string coffee = vectors[1].invoice;
  for (size_t n = 0; n < coffee.size(); ++n) CHECK(check(coffee.substr(0, n)) != Error::OK);
  std::string changed = coffee; changed[50] = '\0'; CHECK(check(changed) == Error::ENCODING);
  changed = coffee; changed[0] = 'L'; CHECK(check(changed) == Error::ENCODING);
  changed = coffee; changed.back() = changed.back() == 'q' ? 'p' : 'q';
  CHECK(check(changed) == Error::ENCODING);
  Parts p = split(coffee);
  for (const char *hrp : {"lntb2500u", "lntbs2500u", "lnbcrt2500u", "lno2500u"}) {
    p.hrp = hrp; CHECK(check(encode(p)) == Error::NETWORK);
  }
  p = split(coffee); p.data[8] = 31; p.data[9] = 31;
  CHECK(check(encode(p)) == Error::FIELDS);
  p = split(coffee); size_t fp = field(p, 1); p.data[fp + 3 + 51] |= 1;
  CHECK(check(encode(p)) == Error::FIELDS); // non-zero hash padding
  p = split(coffee); remove(p, 1); CHECK(check(encode(p)) == Error::FIELDS);
  p = split(coffee); remove(p, 16); CHECK(check(encode(p)) == Error::FIELDS);
  p = split(coffee); remove(p, 13); CHECK(check(encode(p)) == Error::FIELDS);
  p = split(coffee); fp = field(p, 1);
  std::vector<uint8_t> duplicate(p.data.begin() + static_cast<ptrdiff_t>(fp),
                               p.data.begin() + static_cast<ptrdiff_t>(fp + 55));
  p.data.insert(p.data.begin() + 7, duplicate.begin(), duplicate.end());
  CHECK(check(encode(p)) == Error::FIELDS);
  p = split(coffee); replace(p, 23, words(metadataHash.data(), 32));
  CHECK(check(encode(p)) == Error::FIELDS); // d AND h
  p = split(coffee); const uint8_t invalidUtf8[] = {0xc0, 0x80};
  replace(p, 13, words(invalidUtf8, sizeof(invalidUtf8)));
  CHECK(check(encode(p)) == Error::DESCRIPTION);
  p = split(coffee); replace(p, 6, {0, 1}); CHECK(check(encode(p)) == Error::FIELDS);
  p = split(coffee); replace(p, 6, {}); CHECK(check(encode(p)) == Error::EXPIRED);
  p = split(coffee); replace(p, 6, std::vector<uint8_t>(13, 31));
  CHECK(check(encode(p)) == Error::FIELDS); // uint64 overflow
  p = split(coffee); remove(p, 6);
  CHECK(check(encode(p), 250000000, TS + 3599) == Error::OK);
  CHECK(check(encode(p), 250000000, TS + 3600) == Error::EXPIRED);
  p = split(coffee); replace(p, 0, std::vector<uint8_t>(145, 0));
  CHECK(encode(p).size() == MAX_INVOICE_CHARS);
  CHECK(check(encode(p)) == Error::OK);
  CHECK(check(encode(p) + 'q') == Error::LENGTH);
  p = split(coffee); std::fill(p.data.end() - 104, p.data.end(), 0);
  CHECK(check(encode(p)) == Error::SIGNATURE);
  p = split(coffee); p.data.back() = 4; CHECK(check(encode(p)) == Error::SIGNATURE);
}
static void signatures() {
  // Direct high-S recovery vector, which the amount-bound public API refuses
  // only because this particular published invoice has no amount.
  Parts high = split(vectors[15].invoice);
  uint8_t signature[65], digest[32];
  CHECK(detail::bytes(high.data.data() + high.data.size() - 104, 104, signature, 65));
  detail::signatureHash(high.hrp.data(), high.hrp.size(), high.data.data(), high.data.size() - 104, digest);
  const auto expected = unhex("6daf4d488be41ce7cbb487cab1ef2975e5efcea879b20d421f0ef86b07cbb987");
  CHECK(memcmp(digest, expected.data(), 32) == 0);
  CHECK(detail::signature(signature, digest, nullptr));
  // The upstream high-S/n fixture ALSO repeats s. The parser rejects that
  // first, so isolate its signature to test the named-payee low-S rule.
  // Normalize (r,s) to (r,n-s) without any signing or private key.
  Parts named = split(vectors[30].invoice);
  CHECK(check(encode(named), 2500000000, timestamp(named)) == Error::FIELDS);
  CHECK(detail::bytes(named.data.data() + named.data.size() - 104, 104, signature, 65));
  uint8_t pubkey[33];
  CHECK(detail::bytes(named.data.data() + field(named, 19) + 3, 53, pubkey, 33));
  detail::signatureHash(named.hrp.data(), named.hrp.size(), named.data.data(), named.data.size() - 104, digest);
  CHECK(!detail::signature(signature, digest, pubkey));
  auto order = unhex("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");
  int borrow = 0;
  for (int i = 31; i >= 0; --i) {
    int v = order[static_cast<size_t>(i)] - signature[32 + i] - borrow;
    borrow = v < 0; signature[32 + i] = static_cast<uint8_t>(v);
  }
  signature[64] ^= 1;
  CHECK(detail::signature(signature, digest, pubkey));
  digest[0] ^= 1; CHECK(!detail::signature(signature, digest, pubkey));
}
static void amountsAndUrls() {
  uint64_t n = 0;
  for (const char *hrp : {"lnbc1", "lnbc1000m", "lnbc1000000u", "lnbc1000000000n", "lnbc1000000000000p"}) {
    CHECK(detail::amount(hrp, strlen(hrp), n)); CHECK(n == 100000000000ULL);
  }
  for (const char *hrp : {"lnbc", "lnbc0", "lnbc01", "lnbc1x", "lnbc10pp", "lnbc11p", "lnbc1.0",
       "lnbc18446744073709551616p", "lnbc18446744073709551615n", "lnbc-1"})
    CHECK(!detail::amount(hrp, strlen(hrp), n));
  CHECK(requestedMsat(2147484, n) && n == 2147484000ULL);
  char decimalOut[21]; CHECK(decimal(n, decimalOut, sizeof(decimalOut)));
  CHECK(std::string(decimalOut) == "2147484000");
  CHECK(decimal(UINT64_MAX, decimalOut, sizeof(decimalOut)));
  CHECK(std::string(decimalOut) == "18446744073709551615");
  CHECK(!decimal(UINT64_MAX, decimalOut, 20) && decimalOut[0] == 0);
  CHECK(!requestedMsat(UINT64_MAX / 1000 + 1, n) && n == 0);
  CHECK(!requestedMsat(0, n));
  CHECK(withinRange(1000, 1000, 1000));
  CHECK(!withinRange(1000, 1001, 2000) && !withinRange(1000, 1, 999));
  CHECK(!withinRange(1000, 0, 1000) && !withinRange(1000, 2000, 1000));
  for (const char *s : {"https://example.org", "https://ln.example.org/p?amount=1000", "https://example.org:443/v/a%20b", "https://127.0.0.1:8443/"})
    CHECK(httpsUrl(s, strlen(s)));
  for (const char *s : {"http://example.org", "//example.org", "https://", "https://user@example.org/", "https://example.org/#x",
       "https://example.org\\@evil.org", "https://example.org\n/", "https://example.org:0/", "https://example.org:65536/",
       "https://example.org:999999999999999/", "https://example.org:/", "https://example.org:443:1/", "https://-x.org/", "https://x-.org/",
       "https://example..org/", "https://example.org./", "https://%65xample.org/", "https://[::1]/", "https://example.org/%gg", "https://example.org/%"})
    CHECK(!httpsUrl(s, strlen(s)));
  std::string embedded("https://example.org/x\0evil", 26);
  CHECK(!httpsUrl(embedded.data(), embedded.size()));
}
static void settlement() {
  Invoice invoice{};
  auto hash = unhex("66687aadf862bd776c8fc18b8e9f8e20089714856ee233b3902a591d0d5f2925");
  memcpy(invoice.paymentHash, hash.data(), 32); // SHA256(32 zero bytes)
  std::string pr = vectors[1].invoice, proof(64, '0');
  CHECK(settlementProof(invoice, pr.data(), pr.size(), pr.data(), pr.size(), proof.data(), proof.size()));
  CHECK(!settlementProof(invoice, pr.data(), pr.size(), vectors[2].invoice, strlen(vectors[2].invoice), proof.data(), proof.size()));
  CHECK(!settlementProof(invoice, pr.data(), pr.size(), pr.data(), pr.size(), proof.data(), 63));
  proof[0] = 'g'; CHECK(!settlementProof(invoice, pr.data(), pr.size(), pr.data(), pr.size(), proof.data(), 64));
  proof[0] = '1'; CHECK(!settlementProof(invoice, pr.data(), pr.size(), pr.data(), pr.size(), proof.data(), 64));
  proof[0] = '\0'; CHECK(!settlementProof(invoice, pr.data(), pr.size(), pr.data(), pr.size(), proof.data(), 64));
}
int main() {
  protocolVectors(); boundaries(); signatures(); amountsAndUrls(); settlement();
  std::cout << "PASS: " << checks << " checks, 31 upstream vectors plus adversarial boundaries\n";
}
