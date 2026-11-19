# CLAUDE.md: imx95-devenv handoff notes

Read this first, then `docs/architecture.md`. These notes cover what a fresh session can't
work out from the files: current status, decisions and the reasons for them, gotchas, and next steps.

## Status (keep this section current)

<!-- STATUS-START -->
- Last updated: 2026-11-19 by Claude (m7shm + USB2 + vEHCI session, on the 192.168.1.224 PC, see "Where things live")
- **"Wimmo" virtual EHCI (2026-11-19, verified on hardware):** M7 emulates an EHCI controller
  (register page = first 4 KiB of the m7shm window); Linux uses ONLY its stock ehci-platform (user's
  hard requirement: no glue module), bound by `m7-vehci` via driver_override after the M7 is up
  (m7ctl unbinds before stop). IRQ = MSGINTR1 -> GIC SPI 236; DMA kept below 0xC0000000 by
  dma-ranges (0xE0000000 broke CMA/Weston, see docs/vehci.md); SM grants M7 DDR 0x90000000-0xDFFFFFFF.
  Root port carries the emulated Wimmo hub (1209:0002); the TRANSLATOR (vehci_xlat.c, default on)
  relays the physical USB2 port behind it: Linux enumerates the real keyboard hub/keyboard/mouse,
  usbhid binds, typed keys reach Linux (verified 2026-11-19); M7 snoops addresses/VID:PID/keys.
  Zero-copy default, bounce optional. Goal: same core behind a PCIe endpoint (host's stock ehci-pci). On the board by hand: V7 boot image (SM with the DDR
  grant), dtb with vehci node, /usr/sbin/m7-vehci(-event), /etc/udev/rules.d/90-wimmo-vehci.rules (console
  announcements), /usr/bin/m7ctl, firmware m7-netipc-usb2.elf.
- **"Wimmo" virtual xHCI + virtual USB 3 drive (2026-11-19, verified on hardware):** same rules as
  the vEHCI (Linux uses ONLY stock xhci-plat/"xhci-hcd", later the host's stock xhci-pci over PCIe).
  User decision: the xHCI only ever talks to a VIRTUAL drive (no physical USB3/DWC3/Type-C takeover).
  Registers = second 4 KiB of the m7shm window (Linux 0x20421000), IRQ shares SPI 236 with the vEHCI
  (src/a55_irq.c), drive store = 64 MiB M7 DDR at 0x83000000. Linux enumerates it at 5000M, usb-storage
  + sd bind, data verified. 96 MB/s read / 59 MB/s write (M7 memcpy-bound); null drive 382 MB/s with
  628 M7 cycles per TRB (EHCI: 18,000 per stage). docs/vxhci.md. On the board: dtb node added with
  fdtput (backup `.dtb.pre-vxhci`), m7-vehci binds both controllers, default firmware = this build.
  The Yocto build of all hand-made board changes moves to the T480 (disk on the .224 PC too tight).
- Every cold boot logs `Reset LM 1, reason=fccu, errId=38` on the SM UART once (seen since the first
  hand-made boot image); the M7 then runs normally. Cause not investigated.
- **USB2 belongs to the M7 (2026-11-19, verified on hardware from a cold eMMC boot):** SM cfg moves
  `USB2` to LM1; Linux dts has no USB2 (only `m7-usb2-dma` for SMMU stream 0xf, made identity by
  the new `m7-dma-handoff.service`) and keeps `reg_usb_vbus` on; U-Boot bootcmd without `bootflow
  scan` (it probed USB2 and crashed). Zephyr EHCI host driver `zephyr/modules/devenv`
  (uhc_ci_ehci) + usbh hub class: enumerates FS hub, LS mouse behind it, FS keyboard; hot-plug OK.
  docs/m7-usb2.md. **The eMMC boot image was built OUTSIDE Yocto**: SM rebuilt with Arm GNU 15.2.rel1
  and swapped into the old image, U-Boot default bootcmd byte-patched (scripts/bootimg). The repo
  has the source equivalents (SM cfg, u-boot `devenv-fdt.cfg`, dts, m7-tools) but no Yocto build
  of them yet. On the board by hand: dtb with all overlays (m7shm, USB2 off, VBUS always-on;
  original `.dtb.orig`), `/usr/sbin/m7-dma-handoff` + service, `/etc/default/m7`
  M7_FIRMWARE=m7-netipc-usb2.elf. Original boot area backup on that PC:
  `~/imx95-work/boot-backup/emmc-32k-8m.bin` (crc32 e4a56726).
- **M7 memory window (m7shm, 2026-11-19, verified on hardware):** upper 128 KiB of M7 DTCM
  (M7 0x20020000 = A55 0x20420000), plain memory, no doorbells/protocol yet (user: descriptor ring
  comes later, eventually from an external PCIe host). `linux/m7shm` (m7shm.ko, /dev/m7shm,
  in-kernel API, m7shm-tool), Zephyr `src/shm.c` + `shm` shell, DT node in the devenv dts.
  Cross-core fill/check OK both ways; M7 stop under a live mapping gives SIGBUS, no SError.
  A55 reads ~128 ns/word, writes posted. docs/m7shm.md. On the board NOW (not in an image):
  the adv7535 dtb on mmcblk0p1 has the node added by fdtoverlay (original saved as
  `imx95-19x19-evk-devenv-adv7535.dtb.orig`), m7shm.ko built in /root/m7shm (insmod by hand, not
  autoloaded), firmware `/lib/firmware/m7-netipc-m7shm.elf` loaded by hand (autostart default is
  still the old ELF). No Yocto recipe for m7shm yet.
- **Board runs the image from eMMC (SW7=1010, normal boot), verified on hardware:**
  U-Boot picks `imx95-19x19-evk-devenv-adv7535.dtb`, Linux 6.18 boots with no failed units,
  serial autologin, rootfs-grow (59 GiB), key-only SSH (password refused), M7 auto-started by
  remoteproc with the Zephyr ELF, RPMsg up (`/dev/ttyRPMSG0` shell in tmux pane `imx95:main.0`,
  rpmsg-client-sample 100-message echo OK), Weston running (no monitor yet; ADV7535 `HDMI-A-1`
  detected, disconnected), USB NCM network to the host (board 192.168.95.2, host gets DHCP).
- **Ethernet verified** (cable to a laptop, link-local): laptop pings Linux AND the M7 on the one
  RJ45, UDP echo from the M7 works. Linux names the ports `eth0`/`eth1` (not `end0`). Board-internal
  Linux<->M7 over Ethernet does not work (no PF/VF loopback); use RPMsg. Key fix: VF1's IOMMU group
  must be `identity` (m7-vf-handoff), otherwise SMMU faults kill Linux Ethernet too.
- **HDMI verified**: IMX-MIPI-HDMI (ADV7535) at 1920x1080@60 (PLL2410W monitor), Weston shows the
  tmux console (M7 top, Linux bottom).
- **L2 reflector (2026-11-17, stage 1 working):** the M7 firmware reflects EtherType 0x88B5 frames
  (`src/reflector.c`, docs/reflector.md). The M7 MAC is now FIXED at 02:04:9f:95:00:07 (overlay
  `local-mac-address` + `VF_MAC` in m7-vf-handoff). On the board: firmware loaded by hand as
  `/tmp/m7-netipc-reflector.elf` (the autostart default `imx95-m7-zephyr.elf` is still the old
  build), and m7-vf-handoff copied to /usr/sbin. The RJ45 cable currently goes to an RT1060-EVK
  (100 Mb/s) driven by another Claude session on 192.168.1.224 (user wimmo). It measures the round
  trip with IEEE 1588 MAC timestamps. Mailbox: `ssh -i ~/.ssh/id_ed25519_claude_link
  wimmo@192.168.1.224`, `~/claude-link/to-224/` (we write) and `to-193/` (it writes).
  `host/l2rtt` is the Linux-PC measuring tool (hw/sw/user timestamps; needs setcap, see docs).
- **Reflector verified** with the RT1060 peer: 78-byte RTT min 33.0 / avg 35.5 us, 1514-byte 195-198 us
  (docs/reflector.md "Measured"); 0 loss, 0 errors.
- **Not yet verified:** M7 DHCP from a real LAN; uuu flashing (cable).
- The eMMC holds the image with all fixes (built 2026-11-16 21:20), flashed with
  `scripts/flash-from-linux.sh`'s method and verified chunk by chunk against the .wic.
- **Silicon = B0** (factory image: `SOC MIMX95(B0)`, U-Boot `rev2.0`, Linux soc0 revision `2.0`).
- Factory image (gone now): NXP LF 6.12.20, U-Boot 2025.04, SM build 633.
<!-- STATUS-END -->

### Hardware bring-up findings (2026-11-16)
Fixed in the repo:
1. **SError kernel panic** in `fsl_edma_chan_mux`: SM gives eDMA1 ch6 / eDMA2 ch2-3 to the M7.
   devenv dts masks them (`dma-channel-mask` 0x40 / 0xf).
2. **M7 started at address 0**: imx_rproc reads the boot address from ELF section `.interrupts`;
   Zephyr calls it `rom_start`. m7-netipc's CMakeLists renames it (custom ALL target; Zephyr's
   `extra_post_build_commands` is read before the app CMakeLists runs, so it doesn't work there).
3. **RPMsg vring setup failed**: `CONFIG_OPENAMP_COPY_RSC_TABLE` overwrote Linux's resource table at
   0x88220000; now off, the app uses the table in place.
4. **m7ctl** looked for "m7" in the remoteproc name; it's `imx-rproc` (device `imx95-cm7`).
5. **Weston exited with no monitor** and systemd gave up; `require-outputs=none` added.
6. **uuu fastboot never enumerated**: U-Boot's dwc3 gadget runs SuperSpeed, which failed with
   this cable/laptop (ROM SDP and SPL work). Forcing high-speed works in Linux; that's how the
   `usb-gadget-net` recipe sets it. Flashing workaround used: boot Linux, `zstd -dc | dd` to
   `/dev/mmcblk0` from a copy in RAM (see docs/flashing.md). Do NOT write the eMMC under a mounted
   rootfs: the old ext4 wrote its superblock back and had to be repaired with XMODEM + `mmc write`.
   Root cause (2nd time): kernel.sysrq mask ignored the `u`/`b` requests. `scripts/flash-from-linux.sh`
   now sets `kernel.sysrq=1` first.
7. **SMMU blocked the M7's VF DMA** (SID 0x22): faults also stalled the PF TX rings. m7-vf-handoff
   sets the VF's IOMMU group to `identity`. The VSI->PSI MAC registration also needs this.
8. Console UART showed `oe:12032` RX overruns early on (bursty input lost ~16-char blocks); not
   reproducible later. If it comes back, type slowly or use SSH over USB.

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

- Since 2026-11-19 the board is on a second PC (192.168.1.224); the T480 is not at hand. There:
  repo `~/imx95-workspace/imx95-devenv`, Zephyr workspace `~/imx95-work/zephyr-ws` set up natively
  (no podman on that PC; python3-venv works), board SSH key `~/imx95-work/ssh/id_ed25519` (its own key,
  added by hand to the board's authorized_keys; an image built on the T480 only has the T480 key),
  `~/.config/imx95-devenv/board.conf` = USB NCM 192.168.95.2. Kernel modules are built on the board
  (see docs/m7shm.md for the one-time `modules_prepare` + release-string fix). The board has no RTC
  sync: set its clock (`date -s`) before builds.

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

- **SM builds need Arm GNU Toolchain 15.2.rel1.** Ubuntu's gcc-arm-none-eabi 14.2 builds an SM
  that boot-loops right after the OEI (no "Hello from SM").
- A Linux or U-Boot `reset` only restarts the A55 LM; the SM keeps running its old config. A new
  SM (and a new SW7 setting) needs a power cycle. The SM debug monitor (SM UART) has
  `lm ap reset`.
- USB2 now faults on any A55 access: no `usb start`/`bootflow scan` in U-Boot, no USB2 node in
  Linux. Linux's `dmesg` won't show it; U-Boot dies with a synchronous abort at `0x4c200000`.
- The A55 must not touch the M7's DDR (0x80000000-0x89FFFFFF): in U-Boot that caused a POR.

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
- FT4232 port mapping (VERIFIED 2026-11-16): if00=M7, if01=unused, if02=A55 Linux, if03=SM. The
  by-id names are `usb-FTDI_Quad_RS232-HS-if0N-port0` (there's no "FT4232" in them).
- SW7 boot switch: 1010 = eMMC, 1001 = USB serial download. It came set to 1101 (silent board).
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

State at the end of the 2026-11-16 session: everything the user asked for is verified on hardware
(boot, M7 load + RPMsg, tmux consoles, key-only SSH, Ethernet to both cores, HDMI at 1080p).
The board's eMMC has the first image plus fixes copied by hand; the repo has all of them.

1. **Image with all fixes is FLASHED and verified on the board** (2026-11-16, image built 21:20):
   boot via U-Boot defaults (adv7535 dtb), no failed units, rootfs grown to 58G, serial autologin,
   M7 auto-started + RPMsg shell + 100/100 echo, tmux `main`(2 panes)+`shell`, VF1 IOMMU identity,
   0 SMMU faults, laptop -> Linux 0.5 ms and -> M7 0.2 ms + UDP echo, SSH key-only (password
   refused), root `*`, Weston up without monitor. HDMI was unplugged for this run (verified earlier).
2. eth0 IPv4 link-local fallback: added (recipe `devenv-network`, drop-in
   `/etc/systemd/network/80-wired.network.d/10-link-local.conf`, Linux tries 169.254.95.2 first).
   Verified on the board (installed by hand, survives reboot: eth0 = 169.254.95.2, laptop ping OK).
   In the image built 2026-11-16 21:52 (`...rootfs-20261117034502.wic.zst`, checked). The board's
   eMMC has the 21:20 image + this one drop-in, i.e. the same content; reflash with
   `scripts/flash-from-linux.sh` whenever convenient.
3. Reflector: get numbers (RT1060 peer and/or `host/l2rtt`), then stage 2 (busy-poll the VSI
   rings without the Zephyr stack, zero-copy, buffers in OCRAM/TCM; see docs/reflector.md). Rebuild
   the image so the fixed VF_MAC handoff and the new firmware become the defaults.
4. m7shm: Yocto recipe (module + tool, autoload), then the user's descriptor ring on top.
   USB2: HID class on the M7 (keyboard/mouse reports), M7-side SCMI for PD_HSIO_TOP/clock
   (needed once USB1 also leaves Linux), then USB3/USB1 to the M7 (user's next goal). Build a
   Yocto image with the SM/U-Boot/dts changes and compare with the hand-made boot image.
5. M7 DHCP on a real LAN (so far only link-local on a direct laptop cable).
6. Nice to have: an `rpmsg_char` example on Linux; VLAN/PTP for the M7 VF; an iperf server on the M7;
   a smaller terminal font on HDMI if the user wants it.

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
