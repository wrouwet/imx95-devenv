#!/usr/bin/env bash
# Flash the imx95-devenv image to the EVK's eMMC (or an SD card) over USB with
# NXP's uuu (https://github.com/nxp-imx/mfgtools/releases - single static binary).
#
#   scripts/flash.sh [emmc|sd]        default: emmc
#
# Board must be in Serial Download boot mode, USB-C download port connected.
# USB access: run `sudo scripts/host-setup.sh` once (udev rules), then no sudo needed.
set -euo pipefail
WORK="${WORK:-$HOME/imx95-work}"
MACHINE="${MACHINE:-imx95-19x19-lpddr5-evk}"
D="$WORK/yocto/build/tmp/deploy/images/$MACHINE"
TARGET="${1:-emmc}"
UUU="${UUU:-$(command -v uuu || echo "$WORK/tools/uuu")}"

[ -x "$UUU" ] || { echo "uuu not found. Download it from https://github.com/nxp-imx/mfgtools/releases to $WORK/tools/uuu (chmod +x) or set UUU=" >&2; exit 1; }
BOOT="$(ls "$D"/imx-boot-*-sd.bin-flash_a55 2>/dev/null | head -1)"
IMG="$D/imx95-devenv-image-$MACHINE.rootfs.wic.zst"
[ -f "$BOOT" ] || { echo "no boot image in $D (build first)" >&2; exit 1; }
[ -f "$IMG" ]  || { echo "no $IMG (build first)" >&2; exit 1; }

case "$TARGET" in
emmc) exec "$UUU" -b emmc_all "$BOOT" "$IMG" ;;
sd)   exec "$UUU" -b sd_all "$BOOT" "$IMG" ;;
*)    echo "usage: $0 [emmc|sd]" >&2; exit 2 ;;
esac
