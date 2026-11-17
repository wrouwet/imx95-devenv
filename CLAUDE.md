# CLAUDE.md: imx95-devenv handoff notes

Read this first, then `docs/architecture.md`. These notes cover what a fresh session can't
work out from the files: current status, decisions and the reasons for them, gotchas, and next steps.

## Status (keep this section current)

<!-- STATUS-START -->
- Last updated: 2026-11-16 by Claude (autonomous overnight session)
- **Host builds: DONE and verified** (see "Build status" below). First full image build took
  ~23 h wall clock on the ThinkPad T480, including ~7 h paused by laptop suspend.
- **Nothing has been tested on hardware yet.** The board (IMX95LPD5BB-19 rev A) has not been
  booted with this image. Silicon revision unknown (build assumes B0).
<!-- STATUS-END -->

### Build status
Verified 2026-11-16 by inspecting the build outputs (not on hardware):
- `imx-boot-imx95-19x19-lpddr5-evk-sd.bin-flash_a55`: SM `m33_image-mx95devenv.bin`, B0 AHAB
  container, LPDDR5 FW, TF-A/OP-TEE, U-Boot. No M7 image (remoteproc loads it).
- U-Boot default env: `devenv_display=auto`, `loadfdt=run devenv_pickfdt; ...`.
- Kernel: devenv / -adv7535 / -lt9611uxc dtbs decompiled and checked: `m7-ddr@80000000` 48 MiB
  no-map, `cm7` 7 memory-regions, the right HDMI bridge node per variant. Config has remoteproc,
  RPMsg tty/char/ctrl, `PCI_IOV`, ENETC4, ENETC VF (m).
- Rootfs: Zephyr ELF = host build, root `*` (locked), sshd key-only, `authorized_keys` = board key,
  serial autologin drop-in, services enabled (m7-vf-handoff, m7-autostart, imx95-console,
  rootfs-grow), Weston `[autolaunch]`, Rust/cargo/clippy/rustfmt/rg, toolchain, net/debug tools. No Qt.
- Disk image `imx95-devenv-image-...rootfs.wic.zst` (513 MiB, 6.2 GiB raw): imx-boot at 32 KiB = `flash_a55`
  byte for byte; p1 FAT 256 MiB with Image + the 3 devenv dtbs (checked with `wic ls`); p2 ext4 rootfs.
- CI (GitHub Actions `zephyr-m7.yml`) green: builds m7-netipc and checks the ELF.

Bugs caught while verifying (fixed):
1. NXP's default `EXTRA_IMAGE_FEATURES` re-added empty root password + `PermitEmptyPasswords yes`;
   now stripped with `IMAGE_FEATURES:remove` in the image recipe.
2. Devenv dtbs were built but NOT on the boot partition: `KERNEL_DEVICETREE:append` sat in the kernel
   bbappend (recipe-local), so `IMAGE_BOOT_FILES` in the image didn't see it. Moved to
   `yocto/local.conf.append`. Keep it global!

Possible slimming later: target `qemu` and Xen/jailhouse bits come from NXP `MACHINE_FEATURES`
(xen, jailhouse); clang-native and Rust dominate the build time.

## Where things live (host)

- Repo: `~/imx95-devenv` (GitHub: `wrouwet/imx95-devenv`, **public**). Commit identity is
  set repo-locally to the GitHub noreply address. Don't use the user's personal email.
- Work dir (not in git): `~/imx95-work/`
  - `yocto/` NXP repo checkout (`sources/`), `build/`, `downloads/`, `sstate-cache/`
  - `zephyr-ws/` west workspace (`manifest/west.yml` symlinks to the repo), `.venv/`, `build/`
  - `.container-home/` the container's `$HOME` (Zephyr SDK installs here)
- **Everything builds inside podman** (`scripts/container.sh`, Ubuntu 24.04), because the host is
  Ubuntu 26.04 (unsupported by Yocto 6.0) and has no `python3-venv`. The user has no
  passwordless sudo, so never assume host packages can be installed. Ask the user to run
  `! sudo ...` themselves.

## Decisions and why

- **NXP BSP `imx-6.18.37-2.1.0`** (latest at setup time), `fsl-imx-wayland` distro, custom image
  rather than `imx-image-full`: the full image (Qt/Chromium) doesn't fit in ~200 GB of disk or
  a reasonable build time. Weston is included because a monitor is attached. **No Qt** (user: "I don't need qt"): `DISTRO_FEATURES:remove = "qt"`. The NXP distro enables it, and libcamera (via pipewire) would then build Qt6.
- **Linux loads the M7 (remoteproc)**: `IMXBOOT_TARGETS = flash_a55` (no M7 in the boot
  image) and SM config based on `mx95evkrpmsg` (AP has `LMM_1 ALL`). The stock `mx95evk`
  only has `LMM_1 NOTIFY`, so remoteproc couldn't boot the M7.
- **Ethernet sharing = Linux PF + Zephyr VF**, on ENETC0 (RJ45/RTL8211). Zephyr 4.5's
  `nxp,imx-netc-vsi` driver only supports this pairing (Zephyr VSI + Linux PSI). NXP's
  `mx95netc`/`netc_share` approach is the opposite: the M7 owns the switch, it is MCUXpresso-only
  and needs a special Linux dtb. Rejected because the user wants Zephyr on the M7.
- **Reference design**: NXP NavQ95 (Zephyr `boards/nxp/mr_navq95b`, robotics-sm `mr-navq95.cfg`,
  `meta-nxp-robotics` `enetc-vsi-handoff-mcore`). `meta-nxp-robotics` is under the **NXP proprietary
  EULA**, so its files were NOT copied into this public repo. `m7-vf-handoff` is an independent
  implementation. robotics-sm is BSD-3 and was only used for reference.
- **The SM config lives as a full .cfg file in the layer** (BSD-3 header kept), with no inline
  `#` comments on resource lines, because NXP notes configtool parsing bugs with them.
  `docs/sm-mx95devenv-vs-mx95evkrpmsg.diff` shows the delta. Regenerate that diff if the cfg changes.
- **Zephyr built outside Yocto** and injected via `M7_FIRMWARE_ELF` (recipe `m7-firmware`), which
  keeps Zephyr iteration fast. The recipe checks the ELF at *parse* time, so build Zephyr
  before bitbake, or the image ships without default M7 firmware (only a warning).
- **No passwords, by user request** (2026-11-15): serial autologin as root, SSH key-only
  (`DEVENV_SSH_PUBKEY` = `~/imx95-work/ssh/id_ed25519.pub`, passphrase-less key generated on the
  host, never committed), password/empty-password SSH disabled in a rootfs postprocess, root has
  no password. Don't reintroduce `empty-root-password`/`allow-empty-password`.
- **M7 must be visible from the start, by user request**: the `imx95-console` package (tmux session
  `imx95`: windows M7 = RPMsg shell via `m7-term`, Linux = shell). Serial and SSH logins
  auto-attach, Weston autolaunches a terminal on it, and the host has `scripts/board.sh tmux`
  (prefix Ctrl-a to avoid clashing with the board's Ctrl-b).
- **Rootfs tool set, by user request** ("enough Ethernet and development tools for basic local
  enablement"): see the image recipe. Rust (rustc, cargo, clippy, rustfmt) and ripgrep are included on the board, by user request.
- **HDMI display for Linux, by user request**: the user has an HDMI monitor on an adapter in the
  MIPI DSI/CSI connector: **NXP IMX-MIPI-HDMI (ADV7535)**, confirmed by the user 2026-11-15. U-Boot autodetects LT9611UXC (i2c1 @0x2b) and
  otherwise uses ADV7535 (NXP EVK default). Console layout is one split window (M7 top, Linux bottom)
  so the monitor shows both cores at once.
- **NXP EULA**: the user explicitly accepted it in the setup session (`ACCEPT_FSL_EULA=1`).
  Don't put NXP binaries in the repo.

## Gotchas

- An i.MX95 silicon revision mismatch shows up as "no U-Boot output at all". Read
  `docs/silicon-revision.md` before debugging anything else on a dead board.
- A saved U-Boot environment on the board (e.g. from the factory image) overrides ALL default-env
  changes (`CONFIG_DEFAULT_FDT_FILE` and `devenv_pickfdt`). Symptom: the stock NXP dtb boots, and the M7
  pane shows a red warning. Fix: `env default -a; saveenv`. uuu `emmc_all` probably doesn't
  clear the env (unverified).
- U-Boot `i2c dev 1` assumes U-Boot's DM numbering matches the Linux alias `i2c1 = &lpi2c2`. If
  U-Boot numbers it differently, autodetection silently falls back to ADV7535.
- In the bbappends, `${UNPACKDIR}` (not `${WORKDIR}`) is where `file://` sources land in Yocto 6.0.
- Linux netdev naming: the PF is usually `end0` (systemd naming), but `m7-vf-handoff` finds it by
  PCI ID `1131:e101` at `*:00:00.0`, so renames don't break it.
- The VF must match three places at once: SM cfg (`NETC_VSI1`/`NETC_LDID3`), the Zephyr overlay
  (`enetc0_vsi1`) and `m7-vf-handoff` (`VF=1`). Change all three together.
- The M7 DDR region (0x80000000, 48 MiB reserved in Linux) must be in `&cm7 memory-region`, or
  remoteproc fails to load ELF segments there ("bad phdr da").
- uuu 1.5.243 is in `~/imx95-work/tools/uuu` (fetched 2026-11-16 with user permission via
  `gh release download -R nxp-imx/mfgtools`).
- FT4232 debug-USB port → UART mapping in `scripts/board.sh` (if01=SM, if02=A55, if03=M7) is
  an UNVERIFIED guess. Fix it once someone has looked at the real board.
- Host USB/serial access: `sudo scripts/host-setup.sh` (user runs it once) installs udev `uaccess`
  rules for uuu + the FT4232 and adds the user to `dialout`. After that, `flash.sh`/`board.sh` need no sudo.
- tmux on the board: Weston (user weston) attaches to root's tmux server via a 0777 socket and
  `server-access -a weston` (tmux 3.6a). If the GUI terminal shows "access not allowed", check that.
- `rpmsg_tty` allows only one sensible reader: `m7-term` in the tmux window owns `/dev/ttyRPMSG0`,
  so `m7ctl console` attaches to that window instead of opening the tty.
- The host is a laptop (ThinkPad T480, 4C/8T). A suspend pauses the build (it happened once for about 7 h).
  `scripts/yocto-build.sh` now wraps bitbake in `systemd-inhibit` (opt out with `NO_INHIBIT=1`).
- Bitbake's first parse takes about 10 minutes (~20 layers). That isn't a hang.

## Next steps (priority order)

1. Finish/verify the host builds (Yocto image + Zephyr ELF) if not done. Update Status above.
2. **On hardware** (user has the board, serial console only at first):
   - record silicon revision (`/sys/devices/soc0/revision` or the U-Boot banner) in this file
   - verify: boot, `end0` DHCP, `m7ctl status` = running, `/dev/ttyRPMSG0` shell, M7 DHCP lease,
     `ping`/UDP echo to the M7 from the LAN and from Linux on the board
   - record the FT4232 `ttyUSBn` → UART mapping in `docs/flashing.md` and fix the defaults in `scripts/board.sh`
   - check the console: serial autologin → tmux `imx95`, M7 pane shows the banner, Weston autolaunch
   - HDMI: the adapter is IMX-MIPI-HDMI (ADV7535). U-Boot should print `fdtfile=imx95-19x19-evk-devenv-adv7535.dtb`
3. Things likely to need fixing on first boot (untested assumptions): `rootfs-grow` on eMMC,
   `CONFIG_DEFAULT_FDT_FILE` actually being used by the imx95 U-Boot env, the VSI driver
   working with this exact Linux PF driver version (NavQ95 tested on a different linux-imx rev).
4. Nice to have: CI that runs `zephyr-build.sh` (GitHub Actions, Zephyr docker image);
   an `rpmsg_char` example on Linux; VLAN/PTP for the M7 VF (see the end of the architecture doc).

## Commands

```sh
scripts/container.sh scripts/zephyr-setup.sh      # once: Zephyr workspace + SDK (in container)
scripts/container.sh scripts/zephyr-build.sh      # M7 ELF -> ~/imx95-work/zephyr-ws/build/m7-netipc/zephyr/zephyr.elf
ACCEPT_FSL_EULA=1 scripts/yocto-build.sh           # image (setup on first run)
scripts/yocto-build.sh imx-boot                    # just the boot image
scripts/yocto-build.sh linux-imx -c devshell       # (needs a tty)
scripts/board.sh serial | ssh <ip> | tmux | ports  # reach the board from the host
sudo scripts/host-setup.sh                         # once, user runs it (udev + dialout)
scripts/flash.sh emmc                              # uuu in ~/imx95-work/tools
```
