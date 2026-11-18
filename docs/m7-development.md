# Developing for the Cortex-M7 (Zephyr)

## Workspace

`zephyr/west.yml` pins **Zephyr v4.5.0** and only the modules the M7 needs (`cmsis`, `cmsis_6`,
`hal_nxp`, `libmetal`, `open-amp`). `scripts/zephyr-setup.sh` creates:

```
<ws>/                     host: ~/imx95-work/zephyr-ws   board: ~/zephyr-ws
├── .venv/                Python venv with west + Zephyr requirements
├── manifest/west.yml  -> <repo>/zephyr/west.yml
├── zephyr/  modules/     sources (shallow clones)
└── build/<app>/          build directories
```

The Zephyr SDK `arm-zephyr-eabi` toolchain is installed with `west sdk install` into `$HOME`
(host container: `~/imx95-work/.container-home/zephyr-sdk-1.0.1`; board: `~/zephyr-sdk-1.0.1`,
the aarch64 build of the SDK).

| Where | Setup | Build |
|---|---|---|
| PC | `scripts/container.sh scripts/zephyr-setup.sh` | `scripts/container.sh scripts/zephyr-build.sh [app] [west args]` |
| Board | `imx95-zephyr-setup` (clones this repo to `~/imx95-devenv`) | `~/imx95-devenv/scripts/zephyr-build.sh [app]` |

The default board target is `imx95_evk/mimx9596/m7/ddr`. Override with `BOARD=...`, for example
`BOARD=imx95_evk/mimx9596/m7` for a TCM-only image.

## Running on the M7

```sh
m7ctl run path/to/zephyr.elf   # stop, copy to /lib/firmware, select, start
m7ctl status | state | stop | start | restart
m7ctl console                  # jump to the M7 pane of the tmux console
```

The firmware is an ordinary Zephyr ELF with a `.resource_table` section. Linux's `imx_rproc` loads
its segments: DDR segments into the reserved `m7-ddr@80000000` region (48 MiB), TCM segments
through the TCM windows. Then it boots the M7 logical machine through the System Manager (SCMI LMM).

To make your own app the boot default, copy it to `/lib/firmware/imx95-m7-zephyr.elf`, or set
`M7_FIRMWARE=` in `/etc/default/m7`. To set it at build time, point `M7_FIRMWARE_ELF` in
`build/conf/local.conf` at your ELF.

## Memory map (M7 view = physical, DDR identity-mapped)

| Range | Use |
|---|---|
| `0x80000000`–`0x82A00000` | Zephyr `m7/ddr` image (code, data, `nocache` NETC buffers), 42 MiB of the 48 MiB Linux reservation |
| `0x88000000`–`0x8801FFFF` | vrings (vdev0/1 × 2) |
| `0x88020000`–`0x8811FFFF` | RPMsg buffers (`vdevbuffer`) |
| `0x88220000` | resource table (copied there by `CONFIG_OPENAMP_COPY_RSC_TABLE`) |
| `0x4cd30000` | ENETC0 VF1 registers (VSI1), granted to the M7 by the SM config |

## The reference app: `zephyr/apps/m7-netipc`

| File | What |
|---|---|
| `src/rpmsg.c` | OpenAMP remote: vdev setup, Zephyr shell over RPMsg (`rpmsg-tty`), echo endpoint (`rpmsg-client-sample`), online banner |
| `src/net.c` | DHCPv4 on the VF, logs the address, UDP echo on port 7 |
| `src/main.c` | `m7 status` shell command |
| `boards/imx95_evk_mimx9596_m7_ddr.overlay` | IPC chosen nodes + shared memory, NETC ownership split, `enetc0_vsi1` enabled |
| `boards/imx95_evk_mimx9596_m7_ddr.conf` | SCMI, D-cache handling for OpenAMP, resource-table copy |

Shell commands worth knowing: `m7 status`, `net iface`, `net ipv4`, `net ping <ip>`, `net stats`,
`kernel threads`, `kernel uptime`, `device list`, `log enable dbg net_dhcpv4`.

## Adding your own RPMsg channel

Zephyr side, in `rpmsg.c` after the link comes up:

```c
static int my_cb(struct rpmsg_endpoint *ept, void *data, size_t len, uint32_t src, void *priv)
{
	/* handle a message from Linux; reply with rpmsg_send(ept, buf, n) */
	return RPMSG_SUCCESS;
}
static struct rpmsg_endpoint my_ept;
rpmsg_create_ept(&my_ept, rpdev, "my-channel", RPMSG_ADDR_ANY, RPMSG_ADDR_ANY, my_cb, NULL);
```

The name is announced to Linux. Without a kernel driver bound to it, use `rpmsg_char`:

```sh
modprobe rpmsg_char rpmsg_ctrl
ls /sys/bus/rpmsg/devices/                 # virtio0.my-channel.-1.<addr>
# bind it to rpmsg_chrdev, or create an endpoint device from user space:
#   rpmsg_ctrl ioctl RPMSG_CREATE_EPT_IOCTL on /dev/rpmsg_ctrl0 -> /dev/rpmsgN
```

A raw TTY is simpler: name the endpoint `rpmsg-tty`, and Linux's `rpmsg_tty` creates another
`/dev/ttyRPMSGn` for it.

## Networking from the M7

The VF is a normal Zephyr `net_if` (the default interface), so BSD sockets work (`zsock_*`, or
POSIX names with `CONFIG_POSIX_API`). It shares the port with Linux, but it has its own MAC
(`02:04:9f:95:00:07`, fixed) and IP. An L2 reflector for latency measurements answers
EtherType 0x88B5 frames ([reflector.md](reflector.md)). See [ethernet.md](ethernet.md) for how frames are split
and how to test.

Random numbers come from the timer-based test generator (`CONFIG_TEST_RANDOM_GENERATOR`). The M7
has no entropy driver because the TRNG belongs to the ELE. That's fine for DHCP/TCP, but **not**
for crypto.

## Debugging

- **Logs**: Zephyr logs go to the RPMsg shell (the M7 pane) *and* to the M7's own UART (LPUART3,
  one of the debug-USB ports: `scripts/board.sh serial m7`). If the M7 crashes before RPMsg comes
  up, the UART is where you'll see it.
- **Crash or hang**: `m7ctl status` shows `crashed` or `running`. `m7ctl restart` recovers. The SM
  resets the M7 logical machine on M7 lockup (`FAULT_M7_LOCKUP reaction=lm_reset`).
- **JTAG**: Zephyr supports `west debug -r jlink` for this board (J-Link on the EVK JTAG header).
  Load with remoteproc first, then attach.
- **Linux side**: `dmesg | grep -i -E 'rproc|rpmsg|virtio'`, `cat /sys/kernel/debug/remoteproc/remoteproc*/resource_table`.
