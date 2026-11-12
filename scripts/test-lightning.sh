#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PIN=877542fdc16319dd92a7d2a679ea9dacce474bd2
CACHE=${XDG_CACHE_HOME:-"$HOME/.cache"}/bwatch-lightning
mkdir -p "$CACHE"
UBITCOIN_DIR=${UBITCOIN_DIR:-"$CACHE/uBitcoin-$PIN"}
if [[ ! -d "$UBITCOIN_DIR" ]]; then
  git clone https://github.com/micro-bitcoin/uBitcoin.git "$UBITCOIN_DIR"
  git -C "$UBITCOIN_DIR" checkout --detach "$PIN"
fi
[[ $(git -C "$UBITCOIN_DIR" rev-parse HEAD) == "$PIN" ]] || {
  echo "uBitcoin must be at $PIN" >&2; exit 1;
}
[[ -z $(git -C "$UBITCOIN_DIR" status --porcelain -- src) ]] || {
  echo 'uBitcoin src has local modifications' >&2; exit 1;
}
BUILD=$(mktemp -d "$CACHE/run.XXXXXX")
echo "Repository: $(git -C "$ROOT" rev-parse HEAD) (working-tree changes, if any, are included)"
echo "uBitcoin: $PIN"
echo "Build: $BUILD"
python3 - "$ROOT" "$BUILD" <<'PY'
import json, pathlib, sys
root, build = map(pathlib.Path, sys.argv[1:])
vectors = json.loads((root / 'tests/lightning/bolt11-vectors.json').read_text())
with (build / 'vectors.h').open('w') as f:
    f.write('struct Vector { const char *invoice; bool valid; };\nstatic const Vector vectors[] = {\n')
    for v in vectors:
        f.write('{%s, %s},\n' % (json.dumps(v['invoice']), str(v['valid']).lower()))
    f.write('};\n')
corpus = build / 'corpus'
corpus.mkdir()
for i, v in enumerate(vectors):
    (corpus / str(i)).write_text(v['invoice'])
PY
CC=${CC:-cc}; CXX=${CXX:-c++}
FLAGS=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all -ffunction-sections -fdata-sections)
CRYPTO="$UBITCOIN_DIR/src/utility/trezor"
for src in bignum ecdsa secp256k1 sha2 memzero rand; do
  "$CC" -std=c99 "${FLAGS[@]}" -I"$CRYPTO" -c "$CRYPTO/$src.c" -o "$BUILD/$src.o"
done
"$CXX" -std=c++11 "${FLAGS[@]}" -Wall -Wextra -Werror -Wconversion \
  -I"$UBITCOIN_DIR/src" -I"$ROOT/firmware/BitcoinChronograph" -I"$BUILD" \
  "$ROOT/tests/lightning/test.cpp" "$BUILD/"*.o -Wl,--gc-sections -o "$BUILD/test"
"$BUILD/test"
if [[ ${1:-} == --fuzz ]]; then
  # Requires clang/clang++; uses the same crypto implementation as the sketch.
  "$CXX" -std=c++11 "${FLAGS[@]}" -fsanitize=fuzzer \
    -I"$UBITCOIN_DIR/src" -I"$ROOT/firmware/BitcoinChronograph" \
    "$ROOT/tests/lightning/fuzz.cpp" "$BUILD/"*.o -Wl,--gc-sections -o "$BUILD/fuzz"
  "$BUILD/fuzz" "$BUILD/corpus" -runs=100000 -max_len=600 -timeout=10
fi
