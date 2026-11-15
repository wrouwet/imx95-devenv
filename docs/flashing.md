# Flashing and first boot (headless)

No monitor or keyboard is needed. Everything goes through the debug USB serial console and,
once Linux is up, SSH over Ethernet. The board's HDMI/LVDS outputs will work later, because
the image runs Weston.

## Connections

| Connector | Use |
|---|---|
| **Debug USB-C** (FT4232H) | 4 serial ports appear on the host (`/dev/ttyUSB0..3`, 115200 8N1). They carry the A55 Linux console (LPUART1), the System Manager log on the M33 (LPUART2) and the **M7 Zephyr console** (LPUART3). Which `ttyUSBn` is which depends on enumeration, so open them all once and note the mapping. |
| **USB-C download port** | `uuu` flashing in Serial Download mode |
| **RJ45 ENET1** (next to the 10G port, RTL8211 PHY) | ENETC0: Linux `end0` **and** the M7's VF. Plug it into a LAN with DHCP. |
| Power | 12 V barrel jack + power switch |

Serial terminal on the host: `picocom -b 115200 /dev/ttyUSB2`, or `screen /dev/ttyUSB2 115200`.
Your user must be in the `dialout` group. Run `groups` to check; adding it needs sudo and a fresh login.

## Boot mode

The boot mode is set by the BOOT MODE DIP switch on the EVK. Use the table printed on the
board's silkscreen or in NXP's *i.MX95 EVK Quick Start Guide* for **Serial Download**,
**eMMC** and **SD** settings, and change switches only with the power off.

## Flash eMMC with uuu

1. Get `uuu` from https://github.com/nxp-imx/mfgtools/releases and save it as
   `~/imx95-work/tools/uuu` (`chmod +x`). It's a single static binary.
2. Set the board to **Serial Download** and connect the USB-C download port to the host.
3. Run `sudo scripts/flash.sh emmc`, or install NXP's udev rules first and drop `sudo`.
4. Power off, set the boot mode to **eMMC**, power on.

To use an SD card instead, write the `.wic.zst` from the host:
`zstdcat imx95-devenv-image-*.wic.zst | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress`
(check `sdX` with `lsblk`). Then boot with the switches set to SD. The wic image already
contains the `flash_a55` boot image at the right offset.

## First boot checklist

On the A55 console (log in as `root`, no password):

```sh
cat /sys/devices/soc0/revision    # silicon revision, see docs/silicon-revision.md
ip -br addr                       # end0 gets a DHCP address -> use it for SSH
passwd                            # set a root password before exposing the board
systemctl status m7-vf-handoff m7-autostart rootfs-grow
m7ctl status                      # state: running, firmware imx95-m7-zephyr.elf
m7ctl console                     # Zephyr shell: "m7 status", "net iface", "net ping <ip>"
```

The M7 also logs to its own serial port (LPUART3) and gets its own DHCP address.
Test the shared Ethernet path from the board or any LAN host:

```sh
ping <m7-ip>
echo hello | nc -u -w1 <m7-ip> 7     # UDP echo from the M7
```

If U-Boot doesn't boot `imx95-19x19-evk-devenv.dtb` (check with `cat /proc/device-tree/model`
and `ls /sys/class/remoteproc`), interrupt U-Boot and run `env default -a; saveenv; boot`.
