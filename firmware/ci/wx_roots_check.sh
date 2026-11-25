#!/usr/bin/env bash
# The roots in weather_roots.h, checked twice (build.sh runs this):
#  1. Offline, always: every certificate in WX_ROOTS parses, and its SHA-256
#     fingerprint is the one written above it, in the same order.
#  2. Against the live server, when this machine can reach it: the chain
#     api.open-meteo.com sends verifies against WX_ROOTS alone (no system
#     store), for that hostname; and it does NOT verify against an
#     unrelated root, or for another hostname, each refused for its own
#     certificate error (a refusal that never got that far is a failure).
#     A chain that fails is an error. A server that can't be reached is skipped, unless
#     WX_LIVE_REQUIRED is set (build.sh sets it in CI).
#     With WX_PROBE (build.sh builds it from wx_live_probe.cpp), the
#     watch's exact request goes over that checked channel and its answer
#     must pass the watch's own framing and checks (weather.h).
# The watch's own TLS (mbedTLS, core 2.0.17) is a different implementation:
# the wrist test is what shows it working there.
#
#   firmware/ci/wx_roots_check.sh [roots.h] [host]
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
roots_h="${1:-$here/../BitcoinChronograph/weather_roots.h}"
host="${2:-api.open-meteo.com}"
port="${WX_PORT:-443}"
addr="${WX_ADDR:-$host}"          # where to connect, if not the host itself
wrong_root="${WX_WRONG_ROOT:-/etc/ssl/certs/Amazon_Root_CA_1.pem}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# 1. the PEMs out of the C string, and the fingerprints out of the comments
sed -n 's/^  "\(.*\)\\n"$/\1/p' "$roots_h" > "$tmp/roots.pem"
sed -n 's|^//   SHA-256 \([0-9A-F:]*\)$|\1|p' "$roots_h" > "$tmp/want"
csplit -s -z -f "$tmp/cert" "$tmp/roots.pem" '/-----BEGIN CERTIFICATE-----/' '{*}'
: > "$tmp/have"
for c in "$tmp"/cert*; do
  openssl x509 -in "$c" -noout -fingerprint -sha256 | cut -d= -f2 >> "$tmp/have"
  openssl x509 -in "$c" -noout -subject
done
if ! diff -u "$tmp/want" "$tmp/have"; then
  echo "wx_roots_check: FAIL, the fingerprints in $roots_h don't match its certificates" >&2
  exit 1
fi
echo "wx_roots_check: $(wc -l < "$tmp/have") roots, fingerprints match"

# 2. the live chain
probe() {   # probe <CAfile> <hostname to verify>: prints the s_client log
  timeout 20 openssl s_client -connect "$addr:$port" -servername "$host" \
    -verify_hostname "$2" -verify_return_error \
    -CAfile "$1" -no-CApath -no-CAstore </dev/null 2>&1 || true
}
probe "$tmp/roots.pem" "$host" > "$tmp/good"
if ! grep -q -e '^depth=' -e 'Verify return code' -e '^verify error' "$tmp/good"; then
  if [ -n "${WX_LIVE_REQUIRED:-}" ]; then
    echo "wx_roots_check: FAIL, $addr:$port unreachable and the live check is required here:" >&2
    head -3 "$tmp/good" >&2
    exit 1
  fi
  echo "wx_roots_check: SKIPPED the live check, $addr:$port unreachable from here:"
  head -3 "$tmp/good"
  exit 0
fi
grep -e '^depth=' -e '^verify' "$tmp/good" || true
if ! grep -q 'Verify return code: 0 (ok)' "$tmp/good"; then
  echo "wx_roots_check: FAIL, $host's chain does not verify against WX_ROOTS:" >&2
  grep -e 'Verify return code' -e '^verify error' "$tmp/good" >&2 || true
  exit 1
fi
echo "wx_roots_check: $host verifies against WX_ROOTS alone"
# the watch's own request and answer handling over that checked channel
if [ -n "${WX_PROBE:-}" ]; then
  if "$WX_PROBE" request | timeout 30 openssl s_client -quiet -connect "$addr:$port" \
       -servername "$host" -verify_hostname "$host" -verify_return_error \
       -CAfile "$tmp/roots.pem" -no-CApath -no-CAstore 2>/dev/null | "$WX_PROBE" check; then
    echo "wx_roots_check: the watch's request answered and taken, over WX_ROOTS"
  else
    echo "wx_roots_check: FAIL, the watch's request or its answer handling, live" >&2
    exit 1
  fi
fi
# The two that must be refused, each for its own certificate error: a
# failure to connect, resolve or finish in time proves nothing.
fails=0
refused() {   # refused <log> <what> <verify error numbers, as an ERE>
  if grep -q 'Verify return code: 0 (ok)' "$1"; then
    echo "wx_roots_check: FAIL, $host verified $2" >&2
    fails=1
  elif grep -E -q "^verify error:num=($3):" "$1"; then
    echo "wx_roots_check: refused $2, as it should be: $(grep -E -m1 '^verify error:num=' "$1")"
  else
    echo "wx_roots_check: FAIL, the check $2 never reached that certificate error:" >&2
    head -5 "$1" >&2
    fails=1
  fi
}
# 2: unable to get issuer, 19: self-signed in the chain, 20: no local issuer
probe "$wrong_root" "$host" > "$tmp/wrongroot"
refused "$tmp/wrongroot" "against an unrelated root ($(basename "$wrong_root"))" '2|19|20'
# 62: hostname mismatch
probe "$tmp/roots.pem" "wrong.example.com" > "$tmp/wronghost"
refused "$tmp/wronghost" "for another hostname" '62'
exit $fails
