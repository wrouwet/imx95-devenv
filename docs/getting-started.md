# Getting started: from a fresh PC to Linux + M7 running

This walks through the whole flow once, in order. Each step links to the detailed doc.

## 0. What you need

- The **i.MX95 19x19 LPDDR5 EVK** (`IMX95LPD5BB-19`), its 12 V supply, two USB-C cables
  (debug + download), an Ethernet cable to a LAN with DHCP, and optionally an HDMI monitor
  with the NXP **IMX-MIPI-HDMI** adapter on the MIPI DSI/CSI connector.
- A Linux x86_64 PC with `podman`, `git`, `picocom`, `tmux`, about 200 GB of free disk and as many
  cores as you can spare. The full first build takes about 8–12 h on a 4-core laptop.
- Your user in the `dialout` group, for the USB serial ports:
  `sudo usermod -aG dialout $USER`, then log out and back in.

## 1. Clone

```sh
git clone https://github.com/wrouwet/imx95-devenv ~/imx95-devenv
cd ~/imx95-devenv
```

All build output goes to `~/imx95-work/` (override with `WORK=`). Nothing big lands in the repo.

## 2. Build the M7 firmware (minutes)

```sh
scripts/container.sh scripts/zephyr-setup.sh     # west workspace + Zephyr SDK (once)
scripts/container.sh scripts/zephyr-build.sh     # -> ~/imx95-work/zephyr-ws/build/m7-netipc/zephyr/zephyr.elf
```

Do this **before** the Yocto build, so the image ships with this firmware and starts it at boot.
See [m7-development.md](m7-development.md).

## 3. Build Linux (hours)

```sh
ACCEPT_FSL_EULA=1 scripts/yocto-build.sh
```

`ACCEPT_FSL_EULA=1` means you accept the NXP Software License Agreement (read it in
`~/imx95-work/yocto/sources/meta-imx/EULA.txt`). The first run fetches the BSP, creates the build
directory and generates the board SSH key `~/imx95-work/ssh/id_ed25519`.
Outputs are written to `~/imx95-work/yocto/build/tmp/deploy/images/imx95-19x19-lpddr5-evk/`:

| File | What |
|---|---|
| `imx-boot-imx95-19x19-lpddr5-evk-sd.bin-flash_a55` | boot image: OEI + System Manager (`mx95devenv`) + TF-A + U-Boot |
| `imx95-devenv-image-imx95-19x19-lpddr5-evk.rootfs.wic.zst` | full disk image: boot partition (kernel, dtbs) + rootfs |

See [yocto.md](yocto.md) for customizing and rebuilding parts.

## 4. Flash

Get NXP's `uuu` (https://github.com/nxp-imx/mfgtools/releases) into `~/imx95-work/tools/uuu`,
set the boot switches to Serial Download, connect the download USB-C port and run:

```sh
sudo scripts/flash.sh emmc
```

Then set the boot switches to eMMC. Details, plus the SD card alternative: [flashing.md](flashing.md).

## 5. First boot

```sh
scripts/board.sh ports      # find the 4 debug UARTs
scripts/board.sh tmux       # host dashboard: A55 console, M7 UART, SM UART
```

Power on. On the A55 console you'll see the SM, U-Boot (`imx95-devenv: display=auto
fdtfile=imx95-19x19-evk-devenv-adv7535.dtb`) and Linux boot, then automatic root login straight
into the board's tmux session:

```
┌ M7 ───────────────────────────────────────────────────────────┐
│ ==== i.MX95 Cortex-M7 (Zephyr) ====                           │
│   state: running   firmware: imx95-m7-zephyr.elf              │
│ *** i.MX95 Cortex-M7 online: Zephyr 4.5.0, m7-netipc ... ***  │
│ m7:~$ m7 status                                               │
├ Linux ────────────────────────────────────────────────────────┤
│ root@imx95-19x19-lpddr5-evk:~#                                │
└ i.MX95 | M7: running | imx95-19x19-lpddr5-evk | 21:04 ────────┘
```

The HDMI monitor shows the same session in a Weston terminal.

Checks, in the Linux pane:

```sh
cat /sys/devices/soc0/revision       # silicon revision (build assumes B0)
ip -br addr                          # end0: Linux's DHCP address
systemctl --failed                   # should be empty
```

In the M7 pane: `m7 status` shows the M7's own DHCP address on the shared port.

## 6. Day-to-day loop

```sh
# on the PC
scripts/board.sh ssh <board-ip>                       # key-based, lands in the tmux session
scripts/container.sh scripts/zephyr-build.sh
scp -i ~/imx95-work/ssh/id_ed25519 ~/imx95-work/zephyr-ws/build/m7-netipc/zephyr/zephyr.elf root@<board-ip>:/tmp/
# on the board
m7ctl run /tmp/zephyr.elf                             # M7 pane reconnects by itself
```

Or build on the board itself: `imx95-zephyr-setup` (once), then `~/imx95-devenv/scripts/zephyr-build.sh`.
