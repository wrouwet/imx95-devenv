# M7 memory window (m7shm)

A 128 KiB window of Cortex-M7 DTCM that Linux on the A55 can map. Its first 4 KiB are now the
register page of the Wimmo virtual EHCI ([vehci.md](vehci.md)). It is
plain memory: there are no doorbells, no header and no protocol on top yet.
It is the base for a descriptor ring that comes later; the long-term goal is an
external PCIe host reaching the M7 through such a window.

## Where it is

| | Address | Size |
|---|---|---|
| M7 view (DTCM + 128 KiB) | `0x20020000` | 128 KiB |
| A55 view (M7 system-bus slave port) | `0x20420000` | 128 KiB |

DTCM is 256 KiB at `0x20000000` on the M7 and `0x20400000` on the A55
(`imx_rproc_att_imx95_m7` in the kernel). The `imx95_evk/mimx9596/m7/ddr`
board target puts nothing in DTCM (`zephyr,sram = &ddr`), so the lower 128 KiB
stays free for M7-private fast data. The SM config already lets both sides
reach it: the M7 LM owns M7MIX, and the AP domain has `M7MIX DATA
0x20380000-0x2047FFFF`.

The window is defined in two places; keep them in sync:
- Linux: node `m7-shm@20420000` in `imx95-19x19-evk-devenv.dts`
  (`compatible = "devenv,imx95-m7shm"`, `reg`, `devenv,m7-addr`, `devenv,rproc = <&cm7>`)
- Zephyr: node `m7shm@20020000` in `boards/imx95_evk_mimx9596_m7_ddr.overlay`
  (binding in the app's `dts/bindings/`)

## Memory types and cost

- M7: TCM is never cached, so there is no cache maintenance. Use
  `barrier_dmem_fence_full()` where the order of writes matters to the other side.
- A55: `ioremap_wc` / `pgprot_writecombine` (Normal non-cacheable), the same
  mapping `imx_rproc` uses for TCM. Unaligned and 8/16-bit accesses work.
  Writes can be merged and are posted; reads go all the way to the TCM.

Measured on the board (`m7shm-tool bench`, `shm bench`), 2026-11-19:

| | Write | Read |
|---|---|---|
| A55 (32-bit, sequential) | ~2.4 GB/s, 1.7 ns/store (posted) | ~31 MB/s, ~128 ns/load |
| A55 dependent loads (latency) | | ~127 ns |
| M7 (32-bit, sequential) | ~530 MB/s | ~1.6 GB/s |

So A55 reads of the window are expensive (~128 ns each) and writes are cheap.
Keep that in mind for the ring layout: have the A55 write and the M7 read
where possible, and avoid A55 polling loops on the window.

## Lifetime: the window exists only while the M7 runs

The TCM is in the M7's power domain. After `remoteproc stop` an A55 access
raises an SError and the kernel dies. `m7shm.ko` therefore registers a
remoteproc subdevice and tracks the M7:

- online: after remoteproc started the M7 (or at module load if it already runs)
- offline: before remoteproc stops the M7 (also on crash recovery and on `rmmod`)

While offline, user mappings are zapped (the next access gets SIGBUS),
`read`/`write` return `ENODEV`, and `m7shm_access_begin()` returns NULL.
Verified on the board: a process reading the mapping in a loop got SIGBUS on
`echo stop > .../state`, the kernel stayed up, and the window came back after
the next start.

The M7 zeroes the window early in boot (`PRE_KERNEL_1`), so it starts from
known contents each time. That also means anything Linux writes in the first
milliseconds after a start may be overwritten. A ready flag belongs to the
protocol on top.

## Linux side

`linux/m7shm/`: `m7shm.ko` plus `tools/m7shm-tool`. Build and load on the board
(kernel headers are in the image):

```sh
scp -r linux/m7shm root@<board>:            # from the PC
cd m7shm && make && insmod m7shm.ko         # on the board
```

First time on a board: `/lib/modules/$(uname -r)/build` needs `make modules_prepare`
once, and `modules_prepare` regenerates the release string without the git suffix.
Put it back so `vermagic` matches the running kernel:

```sh
K=/lib/modules/$(uname -r)/build; cd $K && make -j6 modules_prepare
uname -r > $K/include/config/kernel.release
echo "#define UTS_RELEASE \"$(uname -r)\"" > $K/include/generated/utsrelease.h
```

Interfaces:
- `/dev/m7shm`: `mmap(MAP_SHARED)` of the window, `pread`/`pwrite`, ioctl
  `M7SHM_IOC_INFO` (`m7shm_uapi.h`)
- sysfs `/sys/bus/platform/devices/20420000.m7-shm/{online,m7_addr,size}`
- in-kernel API for drivers (`m7shm.h`): `m7shm_get()`/`put()`,
  `m7shm_access_begin()`/`end()` around every access, and a notifier with
  `M7SHM_ONLINE`/`M7SHM_OFFLINE`

`m7shm-tool info | dump [OFF [LEN]] | write OFF VALUE | fill VALUE|seq [OFF [LEN]] | check VALUE|seq [OFF [LEN]] | bench`

## M7 side

`src/shm.c` / `src/m7shm.h`: `M7SHM_BASE`, `M7SHM_SIZE`, `m7shm_base()`.
Shell (`/dev/ttyRPMSG0` or the M7 UART): `shm info | dump | write | fill | check | bench`,
with the same arguments as `m7shm-tool`. `seq` makes each word hold its own byte
offset, so a range filled on one core can be checked on the other:

```
linux# m7shm-tool fill seq
m7:~$  shm check seq
check 0x0+0x20000: OK (0 bad words)
```

## Later: an external PCIe host

The window is defined by an address range and the M7's run state only, so a
PCIe endpoint BAR can be pointed at the same TCM (inbound ATU to `0x20420000`)
without changing the M7 side. Notification (doorbells, MSI) is deliberately
not part of this layer yet.
