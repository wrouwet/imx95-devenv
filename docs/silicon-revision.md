# Silicon revision

The boot image (`imx-boot`: OEI DDR config, ELE/AHAB container, mkimage `REV=`) depends on
the i.MX95 silicon revision. An image for the wrong revision usually **doesn't boot at all**:
no U-Boot output on the serial console. The kernel, rootfs and Zephyr don't depend on it.

This build defaults to **B0**, the NXP BSP 6.18 default (`IMX_SOC_REV ??= "B0"` in meta-freescale).

## Finding the revision

- **Serial console** of the image that shipped on the board: the first lines from the SM
  (M33 debug UART) or U-Boot (`CPU: i.MX95 rev1.x` / `SoC: i.MX95 B0`) show it.
- **Chip marking**: the second line of the top marking carries the mask set/revision.
  Early boards with a `P` part-number prefix (e.g. `PIMX9596...`) are pre-production.
- From a running Linux: `cat /sys/devices/soc0/revision`.

## Switching

| Revision | What to do |
|---|---|
| B0 | default, nothing to do |
| B1 | use NXP's machine `imx95-b1-19x19-lpddr5-evk`: `MACHINE=imx95-b1-19x19-lpddr5-evk` for `yocto-setup.sh` on a fresh build dir |
| A0/A1 | BSP 6.18 is built and tested for B0/B1; imx-mkimage still has A1 code paths, but A-silicon ELE firmware and DDR timing are not set up here. Untested: try `IMX_SOC_REV=A1` for the boot image, otherwise use an older NXP BSP (LF6.6.x) for `imx-boot` only |

For B0 ↔ B1 only the boot image needs rebuilding: `scripts/yocto-build.sh imx-boot`.
