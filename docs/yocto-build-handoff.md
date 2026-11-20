# Handoff: first Yocto build of the hand-made board changes (T480)

For a Claude Code session (or a human) on the T480, 2026-11-20. Goal: build the image from branch
`m7shm-usb2` so that all changes now running on the board by hand come from Yocto: SM config (USB2 to
the M7), U-Boot bootcmd, dts (m7shm, vEHCI, vxHCI), `m7-tools` (m7-vehci, m7-dma-handoff), the
`m7shm` module and tool, and the default M7 firmware. **Build only. Do not flash the board.** The
outputs go back to the .224 PC first and are compared there with the hand-made boot image.

## Before starting

- The image installs `~/imx95-work/ssh/id_ed25519.pub` as root's SSH key. If it is missing,
  `yocto-setup.sh` makes a new key, and the .224 PC can no longer log in to the flashed board.
  **Ask the user to copy `~/imx95-work/ssh/` from the .224 PC first** (both files; `chmod 700` on the
  directory, `chmod 600` on the private key). Never commit it.
- Needs about 200 GB free disk (`df -h ~`), `podman` and `git`. Prefer 8+ cores and 32 GB RAM. The
  host needs nothing else, because everything runs in the Ubuntu 24.04 container
  (`scripts/container.sh`).

## Steps

```sh
git clone -b m7shm-usb2 https://github.com/wrouwet/imx95-devenv
cd imx95-devenv

# 1. M7 firmware first: the m7-firmware recipe reads this ELF when bitbake parses
scripts/container.sh scripts/zephyr-setup.sh
scripts/container.sh scripts/zephyr-build.sh
ls -l ~/imx95-work/zephyr-ws/build/m7-netipc/zephyr/zephyr.elf

# 2. Image (hours; the first parse is ~10 min with no output, which isn't a hang)
ACCEPT_FSL_EULA=1 scripts/yocto-build.sh 2>&1 | tee ~/imx95-yocto-build.log
```

Outputs are in `~/imx95-work/yocto/build/tmp/deploy/images/imx95-19x19-lpddr5-evk/`:
`imx-boot-*-sd.bin-flash_a55` and `imx95-devenv-image-*.wic.zst`.

## Things to watch

- **System Manager.** The hand-made SM was built with Arm GNU 15.2.rel1, the toolchain imx-sm
  names and the one Yocto uses. Ubuntu's 14.2 build boot-loops on the board. If
  `imx-system-manager` fails in Yocto, record the error. Don't work around it by changing the SM cfg
  or the toolchain.
- **m7shm** (`recipes-kernel/m7shm`) has never been built in Yocto before. Check that `m7shm.ko`
  and `m7shm-tool` end up in the rootfs.
- **Check the built dtb** (`imx95-19x19-evk-devenv*.dtb` in deploy) with `fdtdump` or `dtc -I dtb`.
  It needs these nodes:
  - an `m7shm` node (`devenv,imx95-m7shm`)
  - `usb@20420000` (`wimmo,vehci`) and `usb@20421000` (`wimmo,vxhci`) under `vehci-bus`, both on SPI 236
  - no USB2 controller node
  - `m7-usb2-dma`

## Report back

Leave a short note for the user to bring to the .224 PC. Include:

- success or failure
- the first `ERROR:` lines from the log
- the output file names and sizes
- the dtb check

The .224 PC session compares the boot image with the hand-made one (backup
`~/imx95-work/boot-backup/emmc-32k-8m.bin` there) before anything is flashed.
