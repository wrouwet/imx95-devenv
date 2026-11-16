# Yocto: layout, rebuilding, customizing

## Pieces

| Component | Recipe | Notes |
|---|---|---|
| BSP | NXP manifest `imx-6.18.37-2.1.0.xml` (branch `imx-linux-wrynose`), Yocto 6.0 | fetched by `repo` into `~/imx95-work/yocto/sources` |
| Machine / distro | `imx95-19x19-lpddr5-evk` / `fsl-imx-wayland`, `DISTRO_FEATURES:remove = "qt"` | `yocto/local.conf.append` |
| Image | `meta-imx95-devenv/recipes-devenv/images/imx95-devenv-image.bb` | tools, SSH hardening, M7 packages |
| System Manager | `imx-system-manager_%.bbappend` + `files/mx95devenv.cfg` | `SYSTEM_MANAGER_CONFIG = "mx95devenv"` |
| Boot image | NXP `imx-boot` | `IMXBOOT_TARGETS = "flash_a55"` (no M7 image) |
| Kernel | `linux-imx_%.bbappend` | devenv dts + adapter variants, `devenv.cfg` fragment (remoteproc/RPMsg, SR-IOV) |
| U-Boot | `u-boot-imx_%.bbappend` | default dtb, `devenv_pickfdt` display autodetect |
| Weston | `weston-init.bbappend` | `[autolaunch]` terminal on the tmux console |
| M7 tools | `m7-tools` | `m7ctl`, `m7-vf-handoff`(+service), `m7-autostart.service`, `imx95-zephyr-setup` |
| Console | `imx95-console` | tmux session, serial autologin, profile hook, Weston launcher |
| M7 firmware | `m7-firmware` | packages the Zephyr ELF from `M7_FIRMWARE_ELF` |
| First boot | `rootfs-grow` | grows the root partition to the full device |

## Common commands

```sh
scripts/yocto-build.sh                          # the image
scripts/yocto-build.sh imx-boot                 # boot image only (SM/U-Boot/TF-A changes)
scripts/yocto-build.sh linux-imx                # kernel + dtbs
scripts/yocto-build.sh -c cleansstate <recipe>  # force a recipe to rebuild from scratch
scripts/yocto-build.sh -e <recipe> | less       # expanded variables of a recipe
scripts/container.sh                            # interactive shell in the build container
#   then: cd $WORK/yocto && source setup-environment build && devtool modify linux-imx ...
```

An SDK for cross-compiling Linux apps on the PC: `scripts/yocto-build.sh imx95-devenv-image -c populate_sdk`.
The installer lands in `tmp/deploy/sdk/`. For most work the native toolchain on the board is enough.

## Adding packages

Add them to `IMAGE_INSTALL` in the image recipe (shared), or `IMAGE_INSTALL:append = " foo"` in
`build/conf/local.conf` (local only). Find the providing recipe with
`scripts/container.sh bash -c 'cd $WORK/yocto/sources && find . -name "foo*.bb"'`.

## Disk and time

- `rm_work` deletes each recipe's work directory after it builds. Recipes you're likely to hack on
  (kernel, U-Boot, SM, imx-boot, the image) are excluded in `RM_WORK_EXCLUDE`.
- `downloads/` (~10 GB) and `sstate-cache/` are shared and survive `rm -rf build/tmp`.
- First build: about 8–12 h on a 4-core/8-thread laptop with 30 GB RAM. Afterwards a kernel change is minutes.
- **Never kill bitbake hard.** A SIGKILL during compilation leaves zero-byte object files that
  `make` treats as up to date. If that happens: `scripts/yocto-build.sh -c cleansstate <recipe>`.
  To stop a build, press Ctrl-C once and wait.
