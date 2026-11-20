#!/usr/bin/env bash
# Reflash the board's eMMC from the development host while the board runs Linux,
# over the USB network (or any SSH path). For when uuu can't be used: uuu's
# fastboot step needs a USB-C cable that works at SuperSpeed.
#
#   scripts/flash-from-linux.sh [BOARD_IP]        default 192.168.95.2 (USB network)
#
# The image is copied to the board's RAM, the tools that run during the write are
# copied to tmpfs too, the root fs is forced read-only, the eMMC is overwritten
# and the board resets at once. kernel.sysrq must allow remount/reboot: the
# image's default doesn't, which left the old ext4 mounted read-write during the
# write, and its journal corrupted the new image twice (2026-11-16). Afterwards
# check the result before trusting it (docs/flashing.md).
# UNRELIABLE (2026-11-20: corrupted the new root fs again despite the SysRq fix).
# Prefer U-Boot "ums 0 mmc 0" and dd from the host, see docs/flashing.md.
set -euo pipefail
WORK="${WORK:-$HOME/imx95-work}"
MACHINE="${MACHINE:-imx95-19x19-lpddr5-evk}"
HOST="${1:-${IMX95_BOARD_IP:-192.168.95.2}}"
IMG="$WORK/yocto/build/tmp/deploy/images/$MACHINE/imx95-devenv-image-$MACHINE.rootfs.wic.zst"
SSH=(ssh -i "$WORK/ssh/id_ed25519" -o UserKnownHostsFile="$WORK/ssh/known_hosts" -o BatchMode=yes)
SCP=(scp -q -i "$WORK/ssh/id_ed25519" -o UserKnownHostsFile="$WORK/ssh/known_hosts" -o BatchMode=yes)

[ -f "$IMG" ] || { echo "no image: $IMG" >&2; exit 1; }
src=$(readlink -f "$IMG")
want=$(sha256sum "$src" | cut -c1-64)
echo "copying $(basename "$src") to $HOST:/dev/shm"
"${SCP[@]}" "$src" "root@$HOST:/dev/shm/img.wic.zst"
got=$("${SSH[@]}" "root@$HOST" 'sha256sum /dev/shm/img.wic.zst | cut -c1-64')
[ "$want" = "$got" ] || { echo "checksum mismatch after copy" >&2; exit 1; }

# Runs on the board. The write itself is done by run.sh, started from tmpfs.
read -r -d '' BOARD_SCRIPT <<'END_OF_BOARD_SCRIPT' || true
set -e
E=/dev/shm/e
mkdir -p $E
cp /usr/lib/ld-linux-aarch64.so.1 /usr/lib/libc.so.6 /usr/lib/libtinfo.so.5 \
   /usr/bin/zstd /usr/bin/dd /usr/bin/bash.bash $E/
zstd -tq /dev/shm/img.wic.zst
echo 1 > /proc/sys/kernel/sysrq
{
  echo 'E=/dev/shm/e'
  echo 'L="$E/ld-linux-aarch64.so.1 --library-path $E"'
  echo 'echo s > /proc/sysrq-trigger'
  echo 'echo u > /proc/sysrq-trigger'
  echo 'sleep 2'
  echo '$L $E/zstd -dcq /dev/shm/img.wic.zst | $L $E/dd of=/dev/mmcblk0 bs=4M iflag=fullblock oflag=direct conv=fsync status=none'
  echo 'echo "imx95-devenv flash: dd exit ${PIPESTATUS[0]} ${PIPESTATUS[1]}, resetting" > /dev/kmsg'
  echo 'echo b > /proc/sysrq-trigger'
} > $E/run.sh
echo "writing the eMMC; the board resets when done (a few minutes, direct I/O)"
setsid $E/ld-linux-aarch64.so.1 --library-path $E $E/bash.bash $E/run.sh </dev/null >/dev/kmsg 2>&1 &
END_OF_BOARD_SCRIPT

"${SSH[@]}" "root@$HOST" "$BOARD_SCRIPT"
echo "started. Watch the A55 console (scripts/board.sh serial a55)."
echo "The new image has new SSH host keys: ssh-keygen -f $WORK/ssh/known_hosts -R $HOST"
