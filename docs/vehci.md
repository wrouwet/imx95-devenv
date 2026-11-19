# "Wimmo" virtual EHCI

The M7 emulates an EHCI host controller, and Linux on the A55 drives it with its **stock
`ehci-platform` driver**. There is no glue module and no kernel change, only device tree. Linux sees
EHCI registers, builds its own queue heads, qTDs and periodic frame list in its own memory, and gets
level interrupts. Behind the root port sits a built-in test device ("Wimmo", `1209:0001`). It is a
stand-in until a translator connects the virtual controller to the physical USB2 controller.

The target is a PCIe endpoint, where a remote host runs its stock `ehci-pci`. Everything here is
built to carry over: EHCI semantics live entirely on the M7, and the transport has three hooks.

Verified on the board, 2026-11-19:
```
ehci-platform 20420000.usb: EHCI Host Controller
ehci-platform 20420000.usb: irq 290, io mem 0x20420000
ehci-platform 20420000.usb: USB 2.0 started, EHCI 1.00
usb 1-1: new high-speed USB device number 2 using ehci-platform
$ lsusb
Bus 001 Device 002: ID 1209:0001 Generic pid.codes Test PID     (Wimmo / Virtual EHCI test device)
$ grep ehci /proc/interrupts
290:  48  0  0  0  0  0  GICv3 268 Level ehci_hcd:usb1
```
`vehci unplug` / `vehci plug` on the M7 shell gives `USB disconnect` / re-enumeration on Linux.
`m7ctl restart` unbinds, restarts the M7 and rebinds.

## Pieces

| | Today (A55) | PCIe endpoint (later) |
|---|---|---|
| Register page | first 4 KiB of the m7shm window: M7 `0x20020000`, Linux `0x20420000` | BAR0 on the same TCM |
| Host memory | Linux DDR `0x90000000`–`0xBFFFFFFF`, accessed directly | outbound ATU window |
| Interrupt | MSGINTR1 channel 0 → A55 GIC SPI 236, level. Raised and cleared by the M7. | MSI |
| Host driver | `ehci-platform` (generic EHCI), bound by `m7-vehci` | `ehci-pci` |

M7 code: `zephyr/modules/devenv/subsys/vehci` (core and test device, independent of the transport,
API in `include/vehci/vehci.h`) and `zephyr/apps/m7-netipc/src/vehci_a55.c` (A55 transport and the
`vehci` shell).

### Linux side, device tree only

```dts
vehci-bus {
	compatible = "simple-bus";
	#address-cells = <2>; #size-cells = <2>;
	ranges;
	dma-ranges = <0 0 0 0 0 0xc0000000>;
	usb@20420000 {
		compatible = "wimmo,vehci";		/* bound to ehci-platform by m7-vehci */
		reg = <0 0x20420000 0 0x1000>;
		interrupts = <GIC_SPI 236 IRQ_TYPE_LEVEL_HIGH>;
	};
};
```
- **Binding.** The registers are M7 TCM, which only exists while the M7 runs (a Linux access with
  the M7 off is an SError). So the compatible matches no driver. `m7-vehci bind` waits for the
  "Wimmo" signature in the vendor block, then binds `ehci-platform` with `driver_override`.
  `m7ctl` (start/run/restart) binds it after starting the M7 and unbinds it before stopping.
- **DMA.** `dma-ranges` keeps Linux's EHCI allocations and bounce buffers below `0xC0000000`. On
  arm64 the lowest `dma-ranges` in the tree also sets `ZONE_DMA`, now `0x90000000`–`0xBFFFFFFF`. The
  limit must stay below the CMA area (`0xC3000000`+): a first try with `0xE0000000` split CMA across
  two zones, CMA was not activated, and Weston failed to allocate buffers. swiotlb lands below the
  limit too (`0xBBFFF000`–`0xBFFFF000`).
- **SM config.** The M7 LM gets `DDR DATA 0x90000000-0xBFFFFFFF` and `0xD0000000-0xDFFFFFFF`
  (it already had `0xC0000000-0xCFFFFFFF`). The M7 MPU maps that as normal non-cacheable memory
  (overlay `linux_ddr*` nodes).
- **Interrupt.** MSGINTR2 is taken by Zephyr's NETC driver (`CONFIG_ETH_NXP_IMX_MSGINTR=2`). MSGINTR1
  is free, M7-owned, and reaches the A55 as SPI 236. That is not SPI 246: the M7 NVIC calls it IRQ 246.
  Found by triggering it and reading `GICD_ISPENDR8` from Linux.

### Register emulation without a write trap

The register page is plain memory. A thread on the M7 (lowest priority, polling continuously)
infers host writes:
- **USBCMD, PORTSC:** a difference from the last published value is a host write.
- **USBSTS:** the EHCI driver only writes write-1-to-clear masks of the interrupt bits. Reserved
  bit 31 is kept set in every published value, so a value without it is a host write, even one
  that repeats the same bits.
- **PORTSC CSC:** a host write that only clears CSC doesn't change the value and can't be seen.
  CSC clears itself 300 ms after it was set, or at the next port reset. That covers Linux's
  100 ms connect debounce.
- **Port reset:** the port is enabled 10 ms into the reset, while PR is still set. Linux reads the
  port right after it ends the reset, faster than any poll.
- **Interrupt:** level = `USBSTS & USBINTR` (interrupt bits). The M7 raises or clears the line
  when that level changes. After a W1C write Linux re-reads USBSTS and may spin briefly until
  the M7 publishes the cleared value (it polls in the microsecond range).

Schedules: when USBCMD.RS and ASE/PSE are set, the M7 walks the async ring and the current frame of
the periodic list in Linux's memory. It loads a queue head's overlay from its next qTD (EHCI 4.10),
runs the whole qTD stage against the device, writes the token back to the qTD and the overlay, and
follows the alternate pointer on a short IN packet. It raises USBINT (IOC or short packet),
USBERRINT (halt) and IAA after a pass that followed the doorbell. A device NAK (-EAGAIN) leaves the
qTD active for the next pass, so a device can be asynchronous. Transactions to an address nobody
answers halt with a transaction error.

Capability registers: EHCI 1.00, one port without port power control, 32-bit, fixed 1024-entry
frame list, no extended capabilities. Vendor block at `0x100`: "Wimmo" signature, version, ready
flag, and the name "Wimmo Virtual EHCI".

## Virtual hub and translator (verified 2026-11-19)

The root port carries the **Wimmo hub** (`1209:0002`, USB 2.0 high-speed, single TT, one port),
emulated by the M7 (`vehci_hub.c`): descriptors, hub class requests, the status change endpoint on the
periodic schedule, remote wakeup from autosuspend. A plain PCI EHCI has no integrated TT, so with
this hub `ehci-pci` reaches full/low-speed devices with split transactions; the emulation simply
executes them.

The **translator** (`vehci_xlat.c`, `CONFIG_APP_VEHCI_XLAT`, default on) puts the physical USB2 port
behind the hub's port. Linux's EHCI driver enumerates and drives the real devices; the M7 sits in the
middle and touches every descriptor:
- each qTD stage Linux queues becomes one physical qTD (`uhc_ci_ehci` raw interface,
  `devenv/ci_ehci_raw.h`), keyed by the Linux qTD's address; the stage NAKs until the physical one
  completes, then status and byte count go back into Linux's qTD
- USB addresses pass through (Linux's SET_ADDRESS reaches the device); the data toggle is Linux's
  (the physical qTD carries it, DTC=1)
- TT routing: Linux addresses FS/LS devices through the virtual hub's TT; physically they use the
  controller's root TT (hub 0, port 1). TTs of real high-speed hubs pass through.
- data: zero-copy by default (the physical controller DMAs into Linux's buffers; `vehci_dma_addr`,
  identity today, the outbound window on PCIe) or bounce buffers in M7 DDR (`vehci bounce on`,
  `CONFIG_VEHCI_XLAT_BOUNCE`)
- a stage Linux stops asking for (URB unlinked) is cancelled after 500 ms
- port: physical attach/detach = hub port connect change; a reset of the hub port resets the
  physical port (work queue)
- snoop for the console: SET_ADDRESS, VID:PID, SET_CONFIGURATION, and key presses of boot keyboards
  found in the configuration descriptors (`CONFIG_VEHCI_XLAT_KEYS`)

Zephyr's own USB host stack is off on USB2 in this mode (`src/usb.c` only with
`CONFIG_USB_HOST_STACK`).

Seen on the board with an Apple keyboard (built-in FS hub) and a Logitech mouse:
```
$ lsusb -t
/:  Bus 001.Port 001: Dev 001, Class=root_hub, Driver=ehci-platform/1p, 480M
    |__ Port 001: Dev 002, If 0, Class=Hub, Driver=hub/1p, 480M              <- Wimmo hub (M7)
        |__ Port 001: Dev 003, If 0, Class=Hub, Driver=hub/3p, 12M           <- keyboard's hub
            |__ Port 001: Dev 004, If 0, Class=Human Interface Device, Driver=usbhid, 1.5M  <- mouse
            |__ Port 003: Dev 005, If 0, Class=Human Interface Device, Driver=usbhid, 12M   <- keyboard
m7: xlat: host gives the device address 5 (SET_ADDRESS passed through)
m7: xlat: address 5 is 05ac:020b (device descriptor, passed through)
m7: xlat: address 5 is a boot keyboard (EP 0x81)
m7: xlat: key 'o' (keyboard at address 5, passing to the host)
```
Typed keys arrive in Linux (Weston, the tmux Linux pane).

## Throughput and M7 load (measured 2026-11-19)

USB 2.0 high-speed stick (SMI "USB DISK", 2 GB) on the physical port, Linux `dd` reading 64 MiB
with `iflag=direct`; `vehci perf 4` on the M7 during the read. Checksums identical in both modes.

| Data path | Throughput | M7 work (vEHCI + USB2 driver thread) | Cost | Rate at 100 % M7 work |
|---|---|---|---|---|
| zero-copy | 14.4 MB/s | 8.5 % + 3.1 % = 11.6 % | 6.4 cycles/byte | ~1.0 Gbit/s |
| bounce | 14.3 MB/s | 18.8 % + 3.1 % = 21.9 % | 12.3 cycles/byte | ~0.5 Gbit/s |

- About 3,700 stages/s at about 3.9 KB each: Linux's EHCI driver builds one qTD per 4 KiB
  scatter-gather page. Zero-copy work is therefore per stage, not per byte: about 25,000 M7 cycles
  (about 31 us at 800 MHz) per stage, mostly uncached reads and writes of Linux's QHs and qTDs.
  Bounce mode adds the memcpy (about 6 cycles/byte).
- "M7 work" counts only polls that retired a stage, plus the driver thread. The vEHCI thread
  polls whenever nothing else runs (69 % of the core idle, about 90 % under load). That is
  spare time, not load; an event-driven design (or a doorbell on PCIe) would free it.
- 14.4 MB/s is latency-bound, not CPU-bound: one stage per endpoint at a time, about 270 us
  per 4 KB stage round trip (USB 2 bus time + relay). Pipelining several qTDs of a queue head
  would raise it towards the link rate.

Rough projection, if the link weren't the limit (zero-copy, data never touches the M7):
- at today's 4 KB stages: ~32,000 stages/s at 100 % M7, about 1 Gbit/s
- with 16–20 KB stages (what one EHCI qTD can carry with page-aligned buffers): about 4–5 Gbit/s
- larger transfers per descriptor (xHCI TRBs up to 64 KB) and cheaper per-stage handling (fewer
  uncached accesses, no polling): on the order of 10 Gbit/s before memory bandwidth and DMA
  latency take over.
SuperSpeed itself needs a virtual xHCI: EHCI's registers and descriptors cannot express USB 3.
The per-stage cost above is the number to scale.

## Not done yet

- Throughput tuning (the translator handles one stage per endpoint at a time; bulk devices such
  as mass storage are untested), isochronous, measured latency.
- Isochronous (iTD/siTD), suspend/resume details, the FLR/frame-list-size options.
- The root hub's product string is Linux's generic "EHCI Host Controller". The name "Wimmo" is in
  the vendor block (and on PCIe it would be in the vendor/device IDs).

## Commands

M7: `vehci status` (registers, port, counters), `vehci plug`, `vehci unplug`.
Linux: `m7-vehci bind|unbind|status`, `lsusb`, `grep ehci /proc/interrupts`.

Linux announces the controller and its devices: a udev rule (`90-wimmo-vehci.rules`) runs
`m7-vehci-event` for USB devices below `20420000.usb`. It prints to every login terminal (`wall -n`:
the serial console, SSH), to the imx95 tmux Linux panes (the HDMI screen) and to the journal
(`journalctl -t wimmo-vehci`). The kernel's own messages don't reach the console (loglevel 1).
```
[Wimmo vEHCI] controller up: Linux ehci-platform drives the M7's virtual EHCI (bus 001)
[Wimmo vEHCI] USB device detected: 1209:0001 "Wimmo Virtual EHCI test device", 480 Mb/s, bus 001 device 002
[Wimmo vEHCI] USB device removed: 1209:0001 (bus 001 device 002)
[Wimmo vEHCI] controller gone (bus 001)
```

The m7shm window's first page is the register page: `m7shm-tool fill` or `shm fill` over the whole
window overwrites it while Linux is bound.
