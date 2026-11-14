#include "ln_validation.h"
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const char *s = reinterpret_cast<const char *>(data);
  uint8_t metadata[32] = {};
  bwatch_ln::Invoice invoice{};
  auto result = bwatch_ln::validateInvoice(s, size, 250000000, 1496314658, metadata, invoice);
  if (result != bwatch_ln::Error::OK && (invoice.amountMsat || invoice.timestamp || invoice.expiresAt))
    std::abort();
  bwatch_ln::httpsUrl(s, size);
  bwatch_ln::settlementProof(invoice, s, size, s, size, s, size);
  // Also fuzz behind the checksum gate: keep the amount/timestamp usable,
  // map mutations to Bech32 words, and repair the checksum (not the signature).
  if (size >= 127 && size <= bwatch_ln::MAX_INVOICE_CHARS) {
    const char *alphabet = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    char repaired[bwatch_ln::MAX_INVOICE_CHARS];
    memcpy(repaired, "lnbc2500u1pvjluez", 17);
    for (size_t i = 17; i < size - 6; ++i) {
      int w = bwatch_ln::detail::word(s[i]);
      repaired[i] = alphabet[w < 0 ? data[i] & 31 : w];
    }
    uint32_t chk = 1;
    for (size_t i = 0; i < 9; ++i) chk = bwatch_ln::detail::polymod(chk, repaired[i] >> 5);
    chk = bwatch_ln::detail::polymod(chk, 0);
    for (size_t i = 0; i < 9; ++i) chk = bwatch_ln::detail::polymod(chk, repaired[i] & 31);
    for (size_t i = 10; i < size - 6; ++i)
      chk = bwatch_ln::detail::polymod(chk, bwatch_ln::detail::word(repaired[i]));
    for (unsigned i = 0; i < 6; ++i) chk = bwatch_ln::detail::polymod(chk, 0);
    chk ^= 1;
    for (size_t i = 0; i < 6; ++i) repaired[size - 6 + i] = alphabet[(chk >> ((5 - i) * 5)) & 31];
    bwatch_ln::validateInvoice(repaired, size, 250000000, 1496314658, metadata, invoice);
  }
  return 0;
}
