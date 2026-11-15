#!/usr/bin/env bash
# Build the imx95-devenv image (or other targets) inside the container.
#   scripts/yocto-build.sh                       # imx95-devenv-image
#   scripts/yocto-build.sh linux-imx -c menuconfig
# First run: ACCEPT_FSL_EULA=1 scripts/yocto-build.sh  (runs yocto-setup.sh)
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${WORK:-$HOME/imx95-work}"
export WORK

if [ ! -f "$WORK/yocto/build/conf/local.conf" ]; then
    "$REPO/scripts/container.sh" env ACCEPT_FSL_EULA="${ACCEPT_FSL_EULA:-0}" \
        IMX_SOC_REV="${IMX_SOC_REV:-B0}" "$REPO/scripts/yocto-setup.sh"
fi

args=("$@"); [ ${#args[@]} -eq 0 ] && args=(imx95-devenv-image)
exec "$REPO/scripts/container.sh" bash -c \
    'cd "$WORK/yocto" && set +u && source setup-environment build >/dev/null && bitbake "$@"' _ "${args[@]}"
