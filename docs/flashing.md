# Flashing and first boot (headless)

No monitor or keyboard is needed. Everything goes through the debug USB serial console and,
once Linux is up, SSH over Ethernet. The board's HDMI/LVDS outputs will work later, because
the image runs Weston.

## Connections

| Connector | Use |
|---|---|
| **Debug USB-C** (FT4232H) | 4 serial ports appear on the host (`/dev/ttyUSB0..3`, 115200 8N1). Verified mapping (by interface, stable across enumeration): **if00 = M7 console**, if01 = unused, **if02 = A55 Linux console**, **if03 = System Manager (M33) + DDR OEI**. Names: `/dev/serial/by-id/usb-FTDI_Quad_RS232-HS-if0N-port0`. |
| **USB-C download port** | `uuu` flashing in Serial Download mode |
| **RJ45 ENET1** (next to the 10G port, RTL8211 PHY) | ENETC0: Linux `end0` **and** the M7's VF. Plug it into a LAN with DHCP. |
| **MIPI DSI/CSI connector** + HDMI adapter (this board: NXP **IMX-MIPI-HDMI**, ADV7535) | Display for Linux (Weston). ADV7535 (NXP IMX-MIPI-HDMI, the EVK default) or LT9611UXC adapter, detected by U-Boot (see below) |
| Power | 12 V barrel jack + power switch |

On the host: `scripts/board.sh ports` lists the four ports, `scripts/board.sh serial [a55|m7|sm]`
opens one, and `scripts/board.sh tmux` opens all of them in one tmux dashboard. The script's default
port mapping is the verified one above. Override it in `~/.config/imx95-devenv/board.conf` if needed
(format in the `board.sh` header).

Boot switch **SW7** (1 = ON): `1010` = eMMC boot (normal), `1001` = USB serial download (flashing).
This board arrived set to `1101`, which is not a documented mode and left the board silent.
Run `sudo scripts/host-setup.sh` once: it installs udev rules (uuu + FT4232, effective immediately) and adds you to `dialout`.

## Boot mode

The boot mode is set by the BOOT MODE DIP switch on the EVK. Use the table printed on the
board's silkscreen or in NXP's *i.MX95 EVK Quick Start Guide* for **Serial Download**,
**eMMC** and **SD** settings, and change switches only with the power off.

## Flash eMMC with uuu

1. Get `uuu` from https://github.com/nxp-imx/mfgtools/releases and save it as
   `~/imx95-work/tools/uuu` (`chmod +x`). It's a single static binary.
2. Set the board to **Serial Download** and connect the USB-C download port to the host.
3. Run `scripts/flash.sh emmc` (after `sudo scripts/host-setup.sh`; otherwise prefix `sudo`).
4. Power off, set the boot mode to **eMMC**, power on.

To use an SD card instead, write the `.wic.zst` from the host:
`zstdcat imx95-devenv-image-*.wic.zst | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress`
(check `sdX` with `lsblk`). Then boot with the switches set to SD. The wic image already
contains the `flash_a55` boot image at the right offset.

## First boot checklist

The A55 console logs in as root automatically and drops you into the `imx95` tmux session.
The `M7` window shows the Zephyr shell; switch to `Linux` with `Ctrl-b l`:

```sh
cat /sys/devices/soc0/revision    # silicon revision, see docs/silicon-revision.md
ip -br addr                       # end0 gets a DHCP address -> scripts/board.sh ssh <ip>
systemctl status m7-vf-handoff m7-autostart imx95-console rootfs-grow
m7ctl status                      # state: running, firmware imx95-m7-zephyr.elf
m7ctl console                     # Zephyr shell: "m7 status", "net iface", "net ping <ip>"
```

The M7 also logs to its own serial port (LPUART3) and gets its own DHCP address.
Test the shared Ethernet path from the board or any LAN host:

```sh
ping <m7-ip>
echo hello | nc -u -w1 <m7-ip> 7     # UDP echo from the M7
```

## HDMI display

U-Boot probes I2C bus 1 just before loading the device tree. If an LT9611UXC answers at
0x2b, it boots `imx95-19x19-evk-devenv-lt9611uxc.dtb`. Otherwise it boots
`imx95-19x19-evk-devenv-adv7535.dtb`, the ADV7535 adapter NXP ships with the EVK, which is harmless
without an adapter. The U-Boot console prints `imx95-devenv: display=... fdtfile=...`.
To force a choice, at the U-Boot prompt: `setenv devenv_display adv7535` (or `lt9611uxc`, `none`,
`auto`), then `saveenv`. Weston starts on the HDMI output with a full-screen terminal showing the
M7 and Linux panes.

If U-Boot doesn't boot an `imx95-19x19-evk-devenv*.dtb` (check with `cat /proc/device-tree/model`
and the red warning in the M7 pane), interrupt U-Boot and run `env default -a; saveenv; boot`.
