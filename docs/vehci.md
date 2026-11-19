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

## Not done yet

- The translator to the physical USB2 controller (the next step).
- Full/low-speed devices. A PCI EHCI has no integrated TT and `ehci-pci` would hand them to
  companion controllers. The plan is a virtual high-speed hub with a TT on the root port, with the
  physical devices behind it; then split transactions have to be emulated.
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
