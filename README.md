# imx95-devenv

[![Zephyr M7 firmware](https://github.com/wrouwet/imx95-devenv/actions/workflows/zephyr-m7.yml/badge.svg)](https://github.com/wrouwet/imx95-devenv/actions/workflows/zephyr-m7.yml)

A development environment for the **NXP i.MX95 19x19 LPDDR5 EVK** (board code
`IMX95LPD5BB-19`). It covers:

- **Linux on the 6× Cortex-A55**: NXP Yocto BSP `imx-6.18.37-2.1.0` (Yocto 6.0
  "wrynose", kernel 6.18), with a custom developer image (`imx95-devenv-image`).
  It has key-only SSH, Weston, native gcc/g++/make/cmake/meson/ninja, kernel
  headers/sources for out-of-tree modules, gdb/strace/ltrace/valgrind/perf,
  Rust (rustc/cargo/clippy/rustfmt), vim/nano/tmux, ripgrep, and networking tools (ethtool, iproute2+tc, tcpdump, nmap, iperf2/3,
  linuxptp, lldpd, phytool, mdio-tools, socat/netcat). It also has everything needed
  to build Zephyr *on the board*.
- **Zephyr on the Cortex-M7**: Zephyr v4.5.0, board `imx95_evk/mimx9596/m7/ddr`,
  with a reference app (`m7-netipc`). You can build it on the host or on the board.
- **Loading the M7 from Linux**: remoteproc. `m7ctl run app.elf` loads, starts and stops it.
- **IPC between the M7 and A55**: OpenAMP/RPMsg. The M7's Zephyr shell appears
  as `/dev/ttyRPMSG0` on Linux, alongside an echo endpoint.
- **Ethernet to both cores on one RJ45 port.** Linux owns the ENETC0 physical
  function. Zephyr on the M7 drives **ENETC0 VF1** (a virtual function on the
  same port), with its own MAC and IP. The NETC hardware sends each frame to the
  right core.

```
            RJ45 (ENET1, RTL8211 PHY)
                     |
              ENETC0 port (NETC)
            /                    \
   PSI / PF  (MAC A)        VSI / VF1 (02:04:9f:95:00:07)
   Linux end0 on A55        Zephyr on M7 (DHCP, UDP echo :7, L2 reflector)
        |                          |
        +------ RPMsg over MU7 + shared DDR (0x88000000) ------+
        |   /dev/ttyRPMSG0 = Zephyr shell, rpmsg-client-sample |
        +------------- remoteproc: load/start/stop ------------+
```

## Status

- **Built and verified on the host (2026-11-16):** boot image (`flash_a55`, SM `mx95devenv`), kernel and
  devenv device trees, U-Boot display autodetect, and the full disk image with M7 firmware, key-only SSH,
  tmux console and the toolchain/tools. The M7 firmware also builds in CI.
- **Not yet run on hardware.** First boot, RPMsg, the shared Ethernet VF and HDMI still need checking on
  the real board. The checklist is in [docs/flashing.md](docs/flashing.md), and the live status is in [CLAUDE.md](CLAUDE.md).

## Documentation

| Doc | For |
|---|---|
| [docs/getting-started.md](docs/getting-started.md) | The whole flow once: build, flash, first boot, daily loop |
| [docs/architecture.md](docs/architecture.md) | How it fits together and why: SM config, device tree, remoteproc, RPMsg, consoles |
| [docs/m7-development.md](docs/m7-development.md) | Zephyr workflow (PC and on-board), memory map, adding RPMsg channels, debugging |
| [docs/vehci.md](docs/vehci.md) | "Wimmo" virtual EHCI emulated by the M7, driven by Linux's stock ehci-platform (PCIe endpoint later) |
| [docs/m7-usb2.md](docs/m7-usb2.md) | USB2 owned by the M7: SM/U-Boot/Linux changes, the Zephyr EHCI host driver, console output, boot image built outside Yocto |
| [docs/m7shm.md](docs/m7shm.md) | Memory window in the M7 DTCM shared with Linux: m7shm.ko, /dev/m7shm, M7 shell, measurements |
| [docs/ethernet.md](docs/ethernet.md) | The shared Ethernet port: data path, setup, tests, tweaks (VLAN, fixed MAC, other ports) |
| [docs/yocto.md](docs/yocto.md) | Layer contents, rebuilding pieces, adding packages, SDK, disk/time |
| [docs/flashing.md](docs/flashing.md) | Connectors, debug UARTs, uuu/SD flashing, HDMI, first-boot checklist |
| [docs/silicon-revision.md](docs/silicon-revision.md) | Finding and handling the i.MX95 silicon revision |
| [docs/troubleshooting.md](docs/troubleshooting.md) | Symptom → cause tables for boot, M7, network, access, build |
| [CLAUDE.md](CLAUDE.md) | Project status, decisions and gotchas (handoff notes for AI sessions and humans) |

## Repository layout

| Path | What |
|---|---|
| `container/` | Ubuntu 24.04 build container (Yocto 6.0 doesn't support the Ubuntu 26.04 host) |
| `scripts/` | `container.sh`, `yocto-setup.sh`, `yocto-build.sh`, `zephyr-setup.sh`, `zephyr-build.sh`, `flash.sh`, `board.sh` (serial/SSH/tmux access from the host) |
| `yocto/meta-imx95-devenv/` | Yocto layer: image, System Manager config, kernel DTS/config, U-Boot default DTB, M7 tools |
| `yocto/local.conf.append` | Build settings appended to `build/conf/local.conf` |
| `zephyr/west.yml` | West manifest (Zephyr v4.5.0 + only the modules the M7 needs) |
| `zephyr/apps/m7-netipc/` | Reference M7 app: RPMsg + Ethernet VF |
| `docs/` | See the documentation table above |
| `.github/workflows/` | CI: builds and checks the M7 Zephyr firmware on every push |
| `CLAUDE.md` | Status and handoff notes for AI coding sessions (and humans) |

## Quick start (host)

Requirements: Linux x86_64, `podman` (rootless is fine), about 200 GB free disk, and preferably 8+ cores and 32 GB RAM.
All builds run inside an Ubuntu 24.04 container (`scripts/container.sh`), so the host needs no other packages.

```sh
git clone https://github.com/wrouwet/imx95-devenv && cd imx95-devenv

# 1. Zephyr workspace + M7 firmware (minutes; runs in the build container)
scripts/container.sh scripts/zephyr-setup.sh
scripts/container.sh scripts/zephyr-build.sh   # -> ~/imx95-work/zephyr-ws/build/m7-netipc/zephyr/zephyr.elf

# 2. Linux BSP + image (hours on the first run; you accept the NXP EULA here)
ACCEPT_FSL_EULA=1 scripts/yocto-build.sh
```

Outputs are in `~/imx95-work/yocto/build/tmp/deploy/images/imx95-19x19-lpddr5-evk/`:
`imx-boot-*-sd.bin-flash_a55` and `imx95-devenv-image-*.wic.zst`.

To flash and boot the board, see [docs/flashing.md](docs/flashing.md). It covers
the USB serial console, `uuu` to eMMC or an SD card, and the first SSH login.

## Consoles: M7 and Linux side by side

At boot the board starts a tmux session, **`imx95`**, showing both cores at once:
the top pane **`M7`** has the Zephyr shell and log of the Cortex-M7 over RPMsg,
reconnecting whenever the M7 restarts, and the bottom pane **`Linux`** is a root
shell on the A55. The status bar shows the M7's remoteproc state. You end up in that session automatically,
with no password, from:

| From | How |
|---|---|
| This machine, debug USB | `scripts/board.sh serial`: serial console, automatic root login, then the tmux session |
| This machine, Ethernet | `scripts/board.sh ssh <ip>`: SSH with the key from `~/imx95-work/ssh/` |
| This machine, everything | `scripts/board.sh tmux`: host dashboard with windows A55 / M7-UART / SM-UART (/ SSH). Host prefix is `Ctrl-a`; `Ctrl-b` goes to the board |
| HDMI monitor on the board | Weston desktop with a full-screen terminal on the session. Works with the ADV7535 (NXP IMX-MIPI-HDMI) or LT9611UXC adapter on the MIPI DSI/CSI connector, detected automatically at boot |
| On the board | `imx95-console attach` or `m7ctl console` |

On the board, `Ctrl-b m` jumps to the M7 pane, `Ctrl-b l` to Linux, `Ctrl-b z` zooms a pane, and `Ctrl-b d` detaches
to a plain shell. Run `touch ~/.no-imx95-console` to stop logins from auto-attaching.

## On the board

```sh
m7ctl status                      # remoteproc state, firmware, RPMsg ttys
m7ctl run /lib/firmware/imx95-m7-zephyr.elf
m7ctl console                     # Zephyr shell over RPMsg: try "m7 status", "net iface"
journalctl -u m7-vf-handoff       # ENETC0 VF1 handed to the M7?

# Build Zephyr natively on the A55 and run it on the M7
imx95-zephyr-setup                # clones this repo, west workspace + SDK into ~/zephyr-ws
~/imx95-devenv/scripts/zephyr-build.sh
m7ctl run ~/zephyr-ws/build/m7-netipc/zephyr/zephyr.elf
```

## Access and security (no passwords)

- Root has **no password** in the image, and no password is ever asked for.
- **Serial console** (physical USB access): automatic root login.
- **SSH: public key only.** Password and empty-password logins are disabled in `sshd_config`.
  `scripts/yocto-setup.sh` creates `~/imx95-work/ssh/id_ed25519` (no passphrase, never committed)
  on the build host and bakes its public key into the image. To use another key, set
  `DEVENV_SSH_PUBKEY` in `build/conf/local.conf`.
- A monitor on the board gets a root terminal through Weston, which is equivalent to physical access.
- For the M7, the UART and RPMsg shell have no authentication. Treat the board as a lab device.

## Licenses

Scripts, Yocto layer and docs: MIT ([LICENSE](LICENSE)). The Zephyr app is
Apache-2.0, with code adapted from Zephyr samples (see file headers).
`mx95devenv.cfg` is derived from NXP's imx-sm (BSD-3-Clause, header retained).
Building the image downloads NXP components under the NXP Software License
Agreement. No NXP binaries are stored in this repository.
