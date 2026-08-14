#!/usr/bin/env bash
#
# Build the ESP-Prog-2 high-throughput UART bridge firmware (see docs/THROUGHPUT.md).
#
# Produces:
#   build/bridge.bin                  - app only  (flash at 0x10000)
#   build/esp-prog2-throughput.bin    - merged    (flash at 0x0, recommended)
#
# Usage:
#   ./build.sh                        # uses IDF_PATH, else ~/esp/esp-idf-v5.5.5
#   IDF_PATH=/opt/esp-idf ./build.sh  # explicit toolchain
#
set -euo pipefail

# --- locate ESP-IDF (validated on v5.5.5; upstream CI covers v5.0-v6.0) ---
: "${IDF_PATH:=$HOME/esp/esp-idf-v5.5.5}"
if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ERROR: ESP-IDF not found at IDF_PATH=$IDF_PATH" >&2
    echo "       Point IDF_PATH at an ESP-IDF v5.0+ checkout and re-run." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo ">>> ESP-IDF: $IDF_PATH"
# shellcheck source=/dev/null
source "$IDF_PATH/export.sh"

# ESP-Prog-2 == ESP32-S3. Layer the product + chip defaults (matches Launchpad CI).
export SDKCONFIG_DEFAULTS="sdkconfig.defaults.esp_prog2;sdkconfig.defaults.esp32s3"
echo ">>> SDKCONFIG_DEFAULTS=$SDKCONFIG_DEFAULTS"

idf.py build

# Single-file merged image so flashing is a foolproof 'write at 0x0' (avoids the
# app-vs-merged offset trap that can brick the probe -- see docs/THROUGHPUT.md).
idf.py merge-bin -o esp-prog2-throughput.bin

cat <<EOF

>>> Build complete.
    app    (flash at 0x10000): $SCRIPT_DIR/build/bridge.bin
    merged (flash at 0x0):     $SCRIPT_DIR/build/esp-prog2-throughput.bin

    Flash:  ./flash.sh <port>          e.g. ./flash.sh /dev/ttyACM1
EOF
