#!/usr/bin/env bash
#
# bench.sh -- build / flash / monitor the eub-target on an ESP32-S3 dev board over its OWN USB
#             (not through the ESP-Prog-2). The console + command loop live on that USB.
#
# Every build also emits an all-in-one image + UF2 at the test/ root:
#   test/eub-target.bin  (merged, flash at 0x0)   test/eub-target.uf2  (drag-drop to the probe MSC)
#
# Usage (runs from anywhere -- it cd's into the project):
#   ./scripts/bench.sh build               # build + emit test/eub-target.{bin,uf2}
#   ./scripts/bench.sh dist                # same as build (just the artifacts)
#   ./scripts/bench.sh flash   <port>      # e.g. /dev/ttyACM0
#   ./scripts/bench.sh monitor <port>      # console: type `baud <n>`, `pattern <n>`, `status`
#   ./scripts/bench.sh run     <port>      # build + dist + flash + monitor (the usual one)
#   ./scripts/bench.sh ports               # list candidate USB serial ports
#
# ESP-IDF: if `idf.py` isn't already on PATH, set IDF_PATH (or edit the default) and this
# script sources it for you.
#
set -euo pipefail

IDF_PATH="/data/beanyclaudecode/ProjectSupport/esp-idf-v5.5.5"

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJ_DIR"

if ! command -v idf.py >/dev/null 2>&1; then
    : "${IDF_PATH:=$HOME/esp/esp-idf-v5.5.5}"
    if [ -f "$IDF_PATH/export.sh" ]; then
        # shellcheck source=/dev/null
        source "$IDF_PATH/export.sh"
    else
        echo "ERROR: idf.py not on PATH and no export.sh at IDF_PATH=$IDF_PATH" >&2
        echo "       Run 'source <esp-idf>/export.sh' first, or set IDF_PATH." >&2
        exit 1
    fi
fi

ensure_target() { [ -f sdkconfig ] || idf.py set-target esp32s3; }

# All-in-one outputs land in the test/ root (parent of this project).
TEST_ROOT="$(cd "$PROJ_DIR/.." && pwd)"
dist() {
    # Merged image (flash at 0x0) + UF2 (drag-drop onto the ESP-Prog-2 MSC drive).
    idf.py merge-bin -o "$TEST_ROOT/eub-target.bin" >/dev/null
    idf.py uf2 >/dev/null
    cp -f build/uf2.bin "$TEST_ROOT/eub-target.uf2"
    echo "dist -> $TEST_ROOT/eub-target.bin  +  $TEST_ROOT/eub-target.uf2"
}

cmd="${1:-run}"
port="${2:-}"
need_port() {
    [ -n "$port" ] && return 0
    echo "ERROR: '$cmd' needs a port, e.g. ./scripts/bench.sh $cmd /dev/ttyACM0" >&2
    echo "       (run './scripts/bench.sh ports' to find the dev board's own USB)" >&2
    exit 1
}

case "$cmd" in
    build)   ensure_target; idf.py build; dist ;;
    dist)    ensure_target; idf.py build; dist ;;
    flash)   need_port; ensure_target; idf.py -p "$port" flash ;;
    monitor) need_port; idf.py -p "$port" monitor ;;
    run)     need_port; ensure_target; idf.py build; dist; idf.py -p "$port" flash monitor ;;
    ports)   ls -1 /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "(no /dev/ttyACM* or /dev/ttyUSB*)" ;;
    *)       echo "usage: $0 {build|dist|flash <port>|monitor <port>|run <port>|ports}" >&2; exit 1 ;;
esac
