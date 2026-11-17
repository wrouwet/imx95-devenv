#!/usr/bin/env bash
# One-time host setup (needs root once):  sudo scripts/host-setup.sh
#
# - udev rules so the logged-in user can use uuu (NXP serial download) and the
#   EVK's FT4232 debug UARTs without sudo. "uaccess" grants access to the
#   active desktop session immediately: no re-login needed.
# - adds the invoking user to "dialout" (covers SSH/non-seat sessions after the
#   next login).
# Undo: sudo rm /etc/udev/rules.d/70-imx95-devenv.rules && sudo udevadm control --reload
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "run as: sudo $0" >&2; exit 1; }
USER_NAME="${SUDO_USER:-}"
WORK="${WORK:-$(getent passwd "${USER_NAME:-root}" | cut -d: -f6)/imx95-work}"
UUU="${UUU:-$(command -v uuu || echo "$WORK/tools/uuu")}"
RULES=/etc/udev/rules.d/70-imx95-devenv.rules

{
    echo "# imx95-devenv: uuu + FT4232 debug UART access for the seat user"
    echo 'SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6011", TAG+="uaccess"'
    if [ -x "$UUU" ]; then
        "$UUU" -udev 2>/dev/null | grep '^SUBSYSTEM'
    else
        # i.MX95 serial download (ROM) and fastboot/uuu in U-Boot
        echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="1fc9", ATTRS{idProduct}=="015d", TAG+="uaccess"'
        echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="1fc9", ATTRS{idProduct}=="0152", TAG+="uaccess"'
        echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="1fc9", ATTRS{idProduct}=="0146", TAG+="uaccess"'
        echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="0525", ATTRS{idProduct}=="a4a5", TAG+="uaccess"'
        echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="1fc9", ATTRS{idProduct}=="0129", TAG+="uaccess"'
    fi
} > "$RULES"
echo "wrote $RULES ($(grep -c '^SUBSYSTEM' "$RULES") rules)"
udevadm control --reload-rules
udevadm trigger --subsystem-match=usb --subsystem-match=tty

if [ -n "$USER_NAME" ] && ! id -nG "$USER_NAME" | grep -qw dialout; then
    usermod -aG dialout "$USER_NAME"
    echo "added $USER_NAME to dialout (effective on next login; uaccess works now)"
fi
echo "done: scripts/flash.sh and scripts/board.sh now work without sudo"
