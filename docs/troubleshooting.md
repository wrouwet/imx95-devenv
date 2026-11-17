# Troubleshooting

## Boot

| Symptom | Check / fix |
|---|---|
| Nothing at all on any debug UART | Silicon revision mismatch (build assumes B0), see [silicon-revision.md](silicon-revision.md). Also check the boot switches and that the boot image was flashed |
| SM/OEI output, then nothing from U-Boot | Wrong DDR config for the board, or a flashing problem. Reflash with `uuu` |
| U-Boot boots `imx95-19x19-evk-adv7535-ap1302.dtb` or another stock dtb | Saved environment from the factory image. At the U-Boot prompt: `env default -a; saveenv; boot` |
| `WARNING: not booted with an imx95-devenv dtb` in the M7 pane | Same as above |
| Kernel panic: cannot mount root | Root device numbering. U-Boot's `mmcautodetect=yes` normally handles eMMC vs SD; check `mmcroot` in U-Boot |

## M7

| Symptom | Check / fix |
|---|---|
| No `/sys/class/remoteproc/*` | Wrong dtb (see above), or `imx_rproc` didn't probe: `dmesg \| grep -i rproc` |
| `lmm(1) not under Linux Control` in dmesg | The boot image doesn't use SM config `mx95devenv`/rpmsg. Rebuild `imx-boot`, check `SYSTEM_MANAGER_CONFIG` |
| `bad phdr da 0x80000000` when starting | `m7-ddr` region missing from the dtb, or the ELF was built for another load address |
| M7 `running` but no `/dev/ttyRPMSG0` | Firmware without a resource table / OpenAMP, or `rpmsg_tty` not loaded (`modprobe rpmsg_tty`). Check the M7 UART for errors |
| M7 pane says "Waiting for the M7 RPMsg shell" | M7 not started: `m7ctl run /lib/firmware/imx95-m7-zephyr.elf` |

## Network

See the table at the end of [ethernet.md](ethernet.md).

## Access

| Symptom | Check / fix |
|---|---|
| `board.sh serial` / `flash.sh`: no access | `sudo scripts/host-setup.sh` (udev uaccess rules + dialout) |
| `board.sh ports`: no FTDI ports | Debug USB-C not connected, or the board is unpowered (the FT4232 is powered by the board) |
| SSH: `Permission denied (publickey)` | Use `scripts/board.sh ssh <ip>` (passes `~/imx95-work/ssh/id_ed25519`). If the image was built with a different key, set `DEVENV_SSH_PUBKEY` and rebuild, or add your key via the serial console |
| Wrong UART in `board.sh` | Set `IMX95_A55_TTY`/`IMX95_M7_TTY`/`IMX95_SM_TTY` in `~/.config/imx95-devenv/board.conf` |
| Serial console garbled in tmux | Resize the host terminal, detach and re-attach (`Ctrl-b d`, then `imx95-console attach`) so the size is queried again |
| No picture on HDMI | U-Boot console should print `fdtfile=...-adv7535.dtb`. Check `weston` with `journalctl -u weston`. Force with `setenv devenv_display adv7535; saveenv` |

## Build

| Symptom | Check / fix |
|---|---|
| `undefined reference` / `file format not recognized` in a native recipe after an interrupted build | Truncated objects: `scripts/yocto-build.sh -c cleansstate <recipe>` |
| `m7-firmware: M7_FIRMWARE_ELF ... not found` warning | Build Zephyr first (`scripts/container.sh scripts/zephyr-build.sh`), then rebuild the image |
| `DEVENV_SSH_PUBKEY not set/found` warning | Run `scripts/yocto-setup.sh` once (it creates the key), or point the variable at a public key |
