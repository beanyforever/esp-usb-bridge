#!/usr/bin/env bash
#
# Flash the ESP-Prog-2 high-throughput firmware (see docs/THROUGHPUT.md).
#
# Flashes the MERGED image at 0x0 with a full erase -- the foolproof path.
# (A merged image at 0x10000, or an app-only image at 0x0, bricks the probe:
#  "No bootable app partitions" boot loop. Offset matters.)
#
# Usage:
#   ./flash.sh <port> [esptool]
#     <port>     e.g. /dev/ttyACM1
#     [esptool]  path to esptool binary (default: 'esptool' on PATH)
#
# Put the ESP-Prog-2 in download mode first: hold BOOT, tap RESET.
# After flashing: release BOOT and power-cycle to run.
#
set -euo pipefail

PORT="${1:?usage: ./flash.sh <port> [esptool-binary]}"
ESPTOOL="${2:-esptool}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMG="$SCRIPT_DIR/build/esp-prog2-throughput.bin"

if [ ! -f "$IMG" ]; then
    echo "ERROR: merged image not found: $IMG" >&2
    echo "       Run ./build.sh first." >&2
    exit 1
fi

echo ">>> Flashing MERGED image at 0x0 (full erase): $IMG"
"$ESPTOOL" --port "$PORT" --chip esp32s3 write-flash --erase-all 0x0 "$IMG"

echo ">>> Done. Release BOOT and power-cycle."
echo "    Confirm this firmware is running: boot log shows 'uart: queue free spaces: 40'."
