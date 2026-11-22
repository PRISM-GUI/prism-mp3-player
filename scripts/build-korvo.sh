#!/bin/bash
# Build the Korvo-1 firmware: build/player.bin, one image merged at address 0 (bootloader,
# partition table, app). Flash it with `esptool.py write_flash 0x0 build/player.bin`.
# Needs ESP-IDF in the environment (. esp-idf/export.sh) and subprojects/prism-esp32 with its
# own PRISM checkout (git submodule update --init --recursive).
set -euo pipefail
cd "$(dirname "$0")/.."

[[ -n ${IDF_PATH:-} ]] || { echo "ESP-IDF is not in the environment: source esp-idf/export.sh" >&2; exit 1; }
[[ -d subprojects/prism-esp32/boards/korvo1 ]] || {
    echo "subprojects/prism-esp32 is missing: git submodule update --init --recursive" >&2
    exit 1
}

export IDF_TARGET=esp32s31
if command -v ccache >/dev/null; then
    export IDF_CCACHE_ENABLE=1 CCACHE_BASEDIR=$PWD CCACHE_NOHASHDIR=true
fi
idf.py -C boards/korvo1 -B build/korvo1 build
(cd build/korvo1 && python -m esptool --chip esp32s31 merge-bin -o ../player.bin @flash_args)
echo "build/player.bin: $(wc -c < build/player.bin | tr -d " ") bytes"
