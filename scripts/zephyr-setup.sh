#!/usr/bin/env bash
# Set up a Zephyr workspace for the i.MX95 Cortex-M7. Works on the x86_64 host
# and natively on the board (aarch64, installed there as imx95-zephyr-setup).
#
#   zephyr-setup.sh [WORKSPACE]      default: $ZEPHYR_WS, $WORK/zephyr-ws (container) or ~/zephyr-ws
# On the x86 host run it in the build container:  scripts/container.sh scripts/zephyr-setup.sh
#
# Creates WORKSPACE/{.venv, manifest/west.yml -> repo, zephyr/, modules/} and
# installs the Zephyr SDK arm-zephyr-eabi toolchain (west sdk install).
# Repo location: $IMX95_DEVENV_REPO, else the repo this script lives in, else
# ~/imx95-devenv (cloned from GitHub if missing).
set -euo pipefail

WS="${1:-${ZEPHYR_WS:-${WORK:+$WORK/zephyr-ws}}}"; WS="${WS:-$HOME/zephyr-ws}"
REPO_URL="${IMX95_DEVENV_URL:-https://github.com/wrouwet/imx95-devenv.git}"
SDK_VERSION="${ZEPHYR_SDK_VERSION:-1.0.1}"

here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
if [ -n "${IMX95_DEVENV_REPO:-}" ]; then
    REPO="$IMX95_DEVENV_REPO"
elif [ -f "$here/../zephyr/west.yml" ]; then
    REPO="$(cd "$here/.." && pwd)"
else
    REPO="$HOME/imx95-devenv"
    [ -d "$REPO/.git" ] || git clone --depth 1 "$REPO_URL" "$REPO"
fi
echo "repo:      $REPO"
echo "workspace: $WS"

mkdir -p "$WS/manifest"
ln -sfn "$REPO/zephyr/west.yml" "$WS/manifest/west.yml"
[ -d "$WS/manifest/.git" ] || git -C "$WS/manifest" init -q

# Python venv with west and Zephyr's Python requirements
[ -d "$WS/.venv" ] || python3 -m venv "$WS/.venv"
# shellcheck disable=SC1091
. "$WS/.venv/bin/activate"
pip install -q --upgrade pip wheel
pip install -q west
command -v cmake >/dev/null || pip install -q cmake
command -v ninja >/dev/null || pip install -q ninja

cd "$WS"
[ -d .west ] || west init -l manifest
west update --narrow -o=--depth=1
pip install -q -r zephyr/scripts/requirements-base.txt

# Toolchain: only the Arm (Cortex-M) GNU toolchain is needed for the M7
if ! ls -d "$HOME"/zephyr-sdk-"$SDK_VERSION"*/gnu/arm-zephyr-eabi >/dev/null 2>&1 &&
   ! ls -d "$HOME"/zephyr-sdk-"$SDK_VERSION"*/arm-zephyr-eabi >/dev/null 2>&1; then
    west sdk install --version "$SDK_VERSION" -t arm-zephyr-eabi
fi

cat <<EOF

Zephyr workspace ready. Build the M7 app with:
  $REPO/scripts/zephyr-build.sh            # -> $WS/build/m7-netipc/zephyr/zephyr.elf
EOF
