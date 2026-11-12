#!/usr/bin/env bash
# Compile firmware/BitcoinChronograph with the build settings from
# docs/ASSEMBLY.md and the libraries pinned in firmware/ci/libraries.txt.
# CI runs this on every firmware PR (.github/workflows/firmware.yml); it
# runs the same way on a desktop with arduino-cli and curl on the PATH.
#
#   firmware/ci/build.sh [output-dir]       default: firmware/ci/.build/out
#
# Besides the IDE's build files, output-dir/flash/ gets images that flash
# without the IDE; its FLASH.txt says which commit they are and where each
# one goes. IMAGE_REF names them (CI: pr<N> or main; default: the branch).
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

# Flashable images, named bwatch-bw1-<ref>-<commit>[-dirty]:
#   -full.bin    at 0x0, for a new watch or a clean start. It ERASES the
#                settings: merge_bin fills the gaps between its parts with
#                0xFF, and NVS (0x9000-0xDFFF) is one of those gaps.
#   -update.bin  at 0xe000: boot_app0 + app, what an IDE upload writes after
#                the partition table. NVS isn't touched, so settings stay.
#   -sketch.zip  the sketch folder this compiled.
# Addresses and flash settings come from the core (platform.txt's upload
# command, boards.txt), as the IDE would use them.
props="$(arduino-cli compile --fqbn "$FQBN" --show-properties "$sketch")"
prop() { sed -n "s/^$1=//p" <<<"$props" | head -n 1; }
platform="$(prop runtime.platform.path)"
mcu="$(prop build.mcu)"
esptool_bin="$(prop runtime.tools.esptool_py.path)/$(prop tools.esptool_py.cmd)"
case "$esptool_bin" in
  *.py) esptool=(python3 "$esptool_bin") ;;
  *)    esptool=("$esptool_bin") ;;
esac
app="$out/BitcoinChronograph.ino"
boot_app0="$platform/tools/partitions/boot_app0.bin"

# The fixed addresses below must match this build's partition table, or the
# image wouldn't boot. Fail here instead.
table="$(python3 "$platform/tools/gen_esp32part.py" -q "$app.partitions.bin")"
if ! grep -q '^otadata,data,ota,0xe000,' <<<"$table" ||
   ! grep -q '^app0,app,ota_0,0x10000,' <<<"$table"; then
  echo "partition table doesn't match the image addresses:" >&2
  echo "$table" >&2
  exit 1
fi

commit="$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)"
ref="${IMAGE_REF:-$(git -C "$root" rev-parse --abbrev-ref HEAD 2>/dev/null || echo local)}"
ref="${ref//[^A-Za-z0-9._-]/-}"
dirty=""
if [ -n "$(git -C "$root" status --porcelain -- firmware 2>/dev/null)" ]; then
  dirty="-dirty"
fi
name="bwatch-bw1-$ref-${commit:0:7}$dirty"

flash="$out/flash"
rm -rf "$flash"
mkdir -p "$flash"
flash="$(cd "$flash" && pwd)"
"${esptool[@]}" --chip "$mcu" merge_bin -o "$flash/$name-full.bin" \
  --flash_mode "$(prop build.flash_mode)" \
  --flash_freq "$(prop build.flash_freq)" \
  --flash_size "$(prop build.flash_size)" \
  "$(prop build.bootloader_addr)" "$app.bootloader.bin" \
  0x8000 "$app.partitions.bin" \
  0xe000 "$boot_app0" \
  0x10000 "$app.bin"
"${esptool[@]}" --chip "$mcu" merge_bin -o "$flash/$name-update.bin" \
  --target-offset 0xe000 \
  0xe000 "$boot_app0" \
  0x10000 "$app.bin"
(cd "$root/firmware" && python3 -m zipfile -c "$flash/$name-sketch.zip" BitcoinChronograph)

if command -v sha256sum >/dev/null; then sha256=(sha256sum); else sha256=(shasum -a 256); fi
(cd "$flash" && "${sha256[@]}" "$name"-* > SHA256SUMS)
{
  echo "$name"
  echo "Built from commit $commit ($ref)${dirty:+, plus uncommitted changes in firmware/}."
  echo "esp32 core $CORE_VERSION, $FQBN, libraries from firmware/ci/libraries.txt."
  echo
  echo "Update a watch that already runs BWATCH firmware, keeping its settings"
  echo "(Wi-Fi, wallet, Lightning address, time zone, battery calibration):"
  echo "  esptool.py --chip $mcu write_flash 0xe000 $name-update.bin"
  echo "New watch, or a clean start. This ERASES all of those settings:"
  echo "  esptool.py --chip $mcu write_flash 0x0 $name-full.bin"
  echo
  echo "SHA-256:"
  cat "$flash/SHA256SUMS"
} > "$flash/FLASH.txt"
cat "$flash/FLASH.txt"

if [ -n "${GITHUB_OUTPUT:-}" ]; then
  echo "image=$name" >> "$GITHUB_OUTPUT"
fi
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  { echo '```'; cat "$flash/FLASH.txt"; echo '```'; } >> "$GITHUB_STEP_SUMMARY"
fi
