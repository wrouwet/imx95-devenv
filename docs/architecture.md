# Architecture

## Cores and who owns what

| Core | Software | Role |
|---|---|---|
| Cortex-M33 | NXP System Manager (SM) firmware, config `mx95devenv` | Boot core. Partitions the SoC into *logical machines* (LMs), provides SCMI (clocks, power, pins, LM control) |
| Cortex-M7 (LM1 "M7") | Zephyr v4.5.0, `imx95_evk/mimx9596/m7/ddr` | Real-time core, loaded by Linux |
| 6× Cortex-A55 (LM2 "AP") | TF-A, U-Boot, Linux 6.18 (NXP BSP) | Main OS, owns NETC, loads/controls the M7 |

The boot image is built with the `flash_a55` target: it contains OEI, SM, TF-A and U-Boot,
but **no M7 image**. The M7 stays off until Linux starts it.

## System Manager config (`mx95devenv`)

`yocto/meta-imx95-devenv/recipes-bsp/imx-system-manager/files/mx95devenv.cfg` is
NXP's `configs/other/mx95evkrpmsg.cfg` (imx-sm `lf-6.18.37-2.1.0`) plus a small delta
([diff](sm-mx95devenv-vs-mx95evkrpmsg.diff)):

- From `mx95evkrpmsg`: the AP LM has `LMM_1 ALL`, `CPU_M7P ALL` and `PD_M7 ALL`, so Linux's
  `imx_rproc` can power on, boot, reset and shut down the M7 LM. The stock `mx95evk` config
  only gives `LMM_1 NOTIFY`, which would make remoteproc fall back to "attach only".
- Added: the M7 LM owns `NETC_VSI1` (ENETC0 VF1) and `NETC_LDID3` (that VF's MSI/stream ID,
  `sid=0x22`). It gets `READONLY` access to `NETC_ECAM` (to see the PF's SR-IOV VF-enable bits)
  and `NETC0` (ENETC0 capability registers). The AP LM loses `NETC_VSI1` and `NETC_LDID3`.
- The M7 already owns `MSGINTR1/2` in the stock config. The Zephyr NETC driver uses MSGINTR1
  for the VF's MSI-X interrupts.

The same split was used and tested upstream by NXP on the NavQ95 board
(Zephyr `boards/nxp/mr_navq95b`, robotics-sm `mr-navq95.cfg`).

## Linux device tree (`imx95-19x19-evk-devenv*.dtb`)

Three dtbs are built: `imx95-19x19-evk-devenv.dtb` (no display) and `-adv7535` / `-lt9611uxc`,
each of which is the devenv dtb plus NXP's adapter overlay (`imx95-19x19-evk-<bridge>.dtbo`), merged
by Kbuild. U-Boot's `loadfdt` runs `devenv_pickfdt` first. That probes `i2c dev 1` (lpi2c2) for
the LT9611UXC at 0x2b and otherwise uses ADV7535 (at 0x3d, NXP's EVK default). Override with the
`devenv_display` env var. The patch is applied to `board/nxp/imx95_evk/imx95_evk.env` in the
u-boot-imx bbappend.

The devenv dtb itself is:

`imx95-19x19-evk.dts` plus:
- a `no-map` reserved region `m7-ddr@80000000`, 48 MiB, where the Zephyr `m7/ddr` image
  is linked (Zephyr uses 0x80000000 + 42 MiB)
- that region appended to `&cm7 { memory-region }`, so remoteproc's ELF loader may write there.

The base EVK dts already has the `fsl,imx95-cm7` remoteproc node (MU7 tx/rx/rxdb, `fsl,lmm-id = <1>`,
`fsl,cpu-id = <1>`), vrings at `0x88000000..0x8801ffff`, `vdevbuffer` at `0x88020000` (1 MiB) and the
resource table at `0x88220000`. The Zephyr overlay uses exactly those addresses.

U-Boot boots this dtb by default (`CONFIG_DEFAULT_FDT_FILE`). If the board already has a
saved U-Boot environment from an older image, run `env default -a; saveenv` once.

The NXP `imx95-19x19-evk-rpmsg.dtb` was deliberately **not** used. It belongs to NXP's MCUXpresso
audio/I2C-over-RPMsg demos and disables SAI, eDMA, LPI2C4 etc. on Linux.

## Loading the M7 (remoteproc)

```
echo imx95-m7-zephyr.elf > /sys/class/remoteproc/remoteprocN/firmware
echo start > /sys/class/remoteproc/remoteprocN/state
```
`m7ctl` wraps this (`status | load | start | stop | restart | run | console`).
`m7-autostart.service` runs `/lib/firmware/imx95-m7-zephyr.elf` at boot, if present.
The image includes the `m7-netipc` build that was current when the image was made.
Set `M7_AUTOSTART=no` in `/etc/default/m7` to turn this off.

Use the `imx95_evk/mimx9596/m7/ddr` board target for remoteproc. The plain `m7` (TCM) target
also works for small apps, since the remoteproc address table maps the TCMs too. Ethernet
needs the DDR target, though: the NETC driver needs non-cacheable buffers and more RAM.

## IPC (RPMsg)

Zephyr is the OpenAMP *remote* with a resource table (`CONFIG_OPENAMP_RSC_TABLE`,
copied to `0x88220000` by `CONFIG_OPENAMP_COPY_RSC_TABLE`). On Linux, `virtio_rpmsg_bus`
exposes these endpoints:

| Endpoint name | Zephyr side | Linux side |
|---|---|---|
| `rpmsg-tty` | Zephyr shell backend (`CONFIG_SHELL_BACKEND_RPMSG`) | `rpmsg_tty` → `/dev/ttyRPMSG0` |
| `rpmsg-client-sample` | echo, replies `M7 echo: <msg>` | `modprobe rpmsg_client_sample` |

To add your own channel, create another endpoint in `src/rpmsg.c` and bind Linux to it with
`rpmsg_char` (`/dev/rpmsg_ctrl0` → `rpmsg-export-ept`) or a small kernel driver.

## Ethernet shared by both cores

The 19x19 EVK has three NETC Ethernet ports: ENETC0 (RJ45, RTL8211 RGMII PHY), ENETC1 (60-pin
PHY daughter-card connector) and ENETC2 (10G, AQR113C). This setup uses **ENETC0**.

- Linux binds `fsl-enetc4` to the ENETC0 **PF** (PSI). It owns the PHY, the link and its own MAC.
  The netdev is typically named `end0` under systemd; the scripts locate it by PCI ID, not by name.
- At boot, `m7-vf-handoff.service`:
  1. sets `sriov_drivers_autoprobe=0`, so Linux's `fsl_enetc_vf` won't take the VFs
  2. creates 2 VFs (`sriov_numvfs=2`)
  3. sets Bus Master and MSI-X Enable in VF1's PCI config space (the M7 can't write these itself)
  4. sets `ip link set <pf> vf 1 trust on`, so the PF accepts the M7's MAC/multicast filter
     requests sent over the VSI→PSI mailbox.
- Zephyr's `nxp,imx-netc-vsi` driver on `enetc0_vsi1` waits until the PF's SR-IOV VF-enable
  bits are set, then brings up the VSI. It uses the fixed MAC `02:04:9f:95:00:07`, which
  Linux also assigns to VF1 (`VF_MAC` in m7-vf-handoff), and gets an address via DHCPv4. A UDP
  echo service listens on port 7, and an L2 reflector returns EtherType 0x88B5 frames
  ([reflector.md](reflector.md)).
- The ENETC hardware sends each incoming frame to the PF or the VF by destination MAC.
  Broadcast/multicast go to both. Frames between Linux and the M7 are switched internally,
  so Linux on the board can reach the M7 by its IP address.

Because the VF's DMA comes from an unbound PCI function, the SMMU leaves its stream untranslated.
No IOMMU setup is needed, which matches the NavQ95 reference.

Ideas for going further: a dedicated VLAN for M7 traffic (`ip link set end0 vf 1 vlan N`), PTP
time sync (the NETC timer is owned by Linux; `ptp4l` on `end0`), and TSN/Qbv on the PF.

## Consoles (`imx95-console` package)

- `imx95-console.service` (after `m7-autostart`) starts tmux session `imx95` on socket
  `/run/imx95-console/tmux.sock` with config `/etc/imx95-console/tmux.conf`. Window `main` is split:
  the top pane `M7` runs `m7-term`, a loop that waits for `/dev/ttyRPMSG0`, runs `picocom` on it, and
  reconnects after the M7 restarts; it warns in red if the booted dtb lacks the `m7-ddr` reservation.
  The bottom pane `Linux` is a root shell. Window `shell` is a spare root shell. The status bar polls `m7ctl state`.
- `serial-getty@ttyLP0` gets an `--autologin root` drop-in. `/etc/profile.d/zz-imx95-console.sh`
  attaches interactive serial and SSH logins with `imx95-console attach`, which creates a grouped
  session per client (independent current window, shared windows). It first queries the serial
  terminal's size, since serial lines have none.
- Weston (user `weston`) runs `[autolaunch] imx95-console-gui` → `weston-terminal --shell=imx95-console-attach`.
  tmux `server-access -a weston` and a 0777 socket let it attach to root's server.
- Zephyr side: RPMsg shell prompt `m7:~$ `, UART prompt `m7-uart:~$ `, a banner when the RPMsg link
  comes up, and Zephyr logs go to both shells.
