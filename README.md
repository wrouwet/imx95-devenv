# imx95-devenv

A development environment for the **NXP i.MX95 19x19 LPDDR5 EVK** (board code
`IMX95LPD5BB-19`). It covers:

- **Linux on the 6× Cortex-A55**: NXP Yocto BSP `imx-6.18.37-2.1.0` (Yocto 6.0
  "wrynose", kernel 6.18), with a custom developer image (`imx95-devenv-image`)
  that has SSH, Weston, a native toolchain and everything needed to build
  Zephyr *on the board*.
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
   PSI / PF  (MAC A)        VSI / VF1 (MAC B, 00:04:9f:..)
   Linux end0 on A55        Zephyr on M7 (DHCP, UDP echo :7)
        |                          |
        +------ RPMsg over MU7 + shared DDR (0x88000000) ------+
        |   /dev/ttyRPMSG0 = Zephyr shell, rpmsg-client-sample |
        +------------- remoteproc: load/start/stop ------------+
```

See [docs/architecture.md](docs/architecture.md) for how this works and why.

## Repository layout

| Path | What |
|---|---|
| `container/` | Ubuntu 24.04 build container (Yocto 6.0 doesn't support the Ubuntu 26.04 host) |
| `scripts/` | `container.sh`, `yocto-setup.sh`, `yocto-build.sh`, `zephyr-setup.sh`, `zephyr-build.sh`, `flash.sh` |
| `yocto/meta-imx95-devenv/` | Yocto layer: image, System Manager config, kernel DTS/config, U-Boot default DTB, M7 tools |
| `yocto/local.conf.append` | Build settings appended to `build/conf/local.conf` |
| `zephyr/west.yml` | West manifest (Zephyr v4.5.0 + only the modules the M7 needs) |
| `zephyr/apps/m7-netipc/` | Reference M7 app: RPMsg + Ethernet VF |
| `docs/` | Architecture, flashing/first boot, silicon revision, SM config diff |
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

## Security

The image is set up for a desk: root has **no password**, and SSH root login
is allowed. Set a password (`passwd`) or install an SSH key before connecting
the board to a network you don't trust.

## Licenses

Scripts, Yocto layer and docs: MIT ([LICENSE](LICENSE)). The Zephyr app is
Apache-2.0, with code adapted from Zephyr samples (see file headers).
`mx95devenv.cfg` is derived from NXP's imx-sm (BSD-3-Clause, header retained).
Building the image downloads NXP components under the NXP Software License
Agreement. No NXP binaries are stored in this repository.
