#!/usr/bin/env bash
# Build a Zephyr app for the i.MX95 Cortex-M7 (host container or on the board).
# Host: scripts/container.sh scripts/zephyr-build.sh   Board: zephyr-build.sh
#   zephyr-build.sh [APP_DIR] [extra west build args...]
# Defaults: APP_DIR = <repo>/zephyr/apps/m7-netipc, BOARD = imx95_evk/mimx9596/m7/ddr
# Output:   $ZEPHYR_WS/build/<app>/zephyr/zephyr.elf  (load with: m7ctl run <elf>)
set -euo pipefail
REPO="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
WS="${ZEPHYR_WS:-${WORK:+$WORK/zephyr-ws}}"; WS="${WS:-$HOME/zephyr-ws}"
BOARD="${BOARD:-imx95_evk/mimx9596/m7/ddr}"

APP="${1:-$REPO/zephyr/apps/m7-netipc}"; [ $# -gt 0 ] && shift
APP="$(cd "$APP" && pwd)"
[ -d "$WS/.west" ] || { echo "no workspace at $WS - run scripts/zephyr-setup.sh first" >&2; exit 1; }

# shellcheck disable=SC1091
. "$WS/.venv/bin/activate"
cd "$WS"
west build -p auto -b "$BOARD" -d "build/$(basename "$APP")" "$APP" "$@"
echo "ELF: $WS/build/$(basename "$APP")/zephyr/zephyr.elf"
