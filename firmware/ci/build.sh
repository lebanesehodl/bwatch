#!/usr/bin/env bash
# Compile firmware/BitcoinChronograph with the build settings from
# docs/ASSEMBLY.md and the libraries pinned in firmware/ci/libraries.txt.
# CI runs this on every firmware PR (.github/workflows/firmware.yml); it
# runs the same way on a desktop with arduino-cli and curl on the PATH.
#
#   firmware/ci/build.sh [output-dir]       default: firmware/ci/.build/out
#
# Your own Arduino setup is left alone. The core and its tools go into a
# private data folder (firmware/ci/.build/arduino) and the libraries into a
# private sketchbook that is emptied and refilled every run, so neither the
# libraries in your Arduino folder nor your IDE's core can leak into the
# result. To reuse a core you already downloaded, point
# ARDUINO_DIRECTORIES_DATA at it; if that folder holds another esp32
# version, 2.0.17 replaces it.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
sketch="$root/firmware/BitcoinChronograph"
out="${1:-$here/.build/out}"

# docs/ASSEMBLY.md, "Known-good build settings": ESP32S3 Dev Module,
# Flash 8MB, Partition "8M with spiffs", core 2.0.17. Every other menu
# option is left at the board's default, as the IDE leaves it.
CORE="esp32:esp32"
CORE_VERSION="2.0.17"
CORE_INDEX="https://espressif.github.io/arduino-esp32/package_esp32_index.json"
FQBN="esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB"

export ARDUINO_DIRECTORIES_DATA="${ARDUINO_DIRECTORIES_DATA:-$here/.build/arduino}"
export ARDUINO_DIRECTORIES_USER="$here/.build/sketchbook"   # emptied below
export ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS="${ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS:-$CORE_INDEX}"

installed="$(arduino-cli core list)"
if grep -Eq "^$CORE +$CORE_VERSION( |$)" <<<"$installed"; then
  echo "core $CORE@$CORE_VERSION already installed"
else
  arduino-cli core update-index
  arduino-cli core install "$CORE@$CORE_VERSION"
fi

# Fresh library set every run: exactly the pinned tags, nothing else.
libs="$ARDUINO_DIRECTORIES_USER/libraries"
rm -rf "$libs"
mkdir -p "$libs"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
while read -r repo tag _name; do
  case "$repo" in ''|'#'*) continue ;; esac
  dir="${repo#*/}"
  echo "library $repo@$tag"
  curl -fsSL --retry 3 -o "$tmp/$dir.zip" \
    "https://github.com/$repo/archive/refs/tags/$tag.zip"
  unzip -q "$tmp/$dir.zip" -d "$tmp/$dir"
  mv "$tmp/$dir"/* "$libs/$dir"
done < "$here/libraries.txt"

arduino-cli compile \
  --fqbn "$FQBN" \
  --warnings default \
  --output-dir "$out" \
  "$sketch"
