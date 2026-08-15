#!/usr/bin/env bash
#
# bench.sh -- build / flash / monitor the eub-target on an ESP32-S3 dev board over its OWN USB
#             (not through the ESP-Prog-2). The console + command loop live on that USB.
#
# Usage (runs from anywhere -- it cd's into the project):
#   ./scripts/bench.sh build
#   ./scripts/bench.sh flash   <port>      # e.g. /dev/ttyACM0
#   ./scripts/bench.sh monitor <port>      # console: type `baud <n>`, `pattern <n>`, `status`
#   ./scripts/bench.sh run     <port>      # build + flash + monitor (the usual one)
#   ./scripts/bench.sh ports               # list candidate USB serial ports
#
# ESP-IDF: if `idf.py` isn't already on PATH, set IDF_PATH (or edit the default) and this
# script sources it for you.
#
set -euo pipefail

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

cmd="${1:-run}"
port="${2:-}"
need_port() {
    [ -n "$port" ] && return 0
    echo "ERROR: '$cmd' needs a port, e.g. ./scripts/bench.sh $cmd /dev/ttyACM0" >&2
    echo "       (run './scripts/bench.sh ports' to find the dev board's own USB)" >&2
    exit 1
}

case "$cmd" in
    build)   ensure_target; idf.py build ;;
    flash)   need_port; ensure_target; idf.py -p "$port" flash ;;
    monitor) need_port; idf.py -p "$port" monitor ;;
    run)     need_port; ensure_target; idf.py -p "$port" flash monitor ;;
    ports)   ls -1 /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "(no /dev/ttyACM* or /dev/ttyUSB*)" ;;
    *)       echo "usage: $0 {build|flash <port>|monitor <port>|run <port>|ports}" >&2; exit 1 ;;
esac
