# Wimmo virtual xHCI and virtual USB 3 drive

The M7 emulates an xHCI host controller with a SuperSpeed mass-storage device behind it. Linux
on the A55 drives both with its stock drivers: `xhci-plat` (driver `xhci-hcd`), `usb-storage`
and `sd`. Nothing on the host side is changed. The same M7 code is meant to sit behind a PCIe
endpoint later, where the host's stock `xhci-pci` would drive it. Unlike the virtual EHCI
([vehci.md](vehci.md)), the xHCI only ever talks to a virtual drive, never to a physical USB 3
device.

```
Linux (A55)                                  M7 (Zephyr)
xhci-hcd (xhci-plat)  --registers-->  vxhci.c: register page, doorbells, command ring,
usb-storage, sd                       event ring, contexts, transfer rings
rings/contexts/buffers <--memory-->       |
in Linux DDR                          vxhci_drive.c: SuperSpeed BOT/SCSI drive, 64 MiB RAM
                 <--SPI 236 (shared with the virtual EHCI)--
```

## Where things are

| | |
|---|---|
| Core | `zephyr/modules/devenv/subsys/vxhci/vxhci.c`, API `include/vxhci/vxhci.h` |
| Drive | `zephyr/modules/devenv/subsys/vxhci/vxhci_drive.c` (1209:0003, "Wimmo Virtual USB 3 drive") |
| A55 transport and shell | `zephyr/apps/m7-netipc/src/vxhci_a55.c`, shared IRQ `src/a55_irq.c` |
| Registers | second 4 KiB of the m7shm window: M7 0x20021000, Linux 0x20421000 |
| Drive store | M7 DDR 0x83000000, 64 MiB (`vxhci_drive` region in the overlay; cached on the M7, outside Linux RAM) |
| Linux dts | `vehci-bus { usb@20421000 { compatible = "wimmo,vxhci"; interrupts = SPI 236 } }` |
| Binding | `m7-vehci bind` binds both virtual controllers (`driver_override`), called by `m7ctl` |
| Kconfig | `VXHCI`, `VXHCI_ADAPTIVE_POLL` (module); `APP_VXHCI` (app, default y) |

## How the emulation works

The register page is plain memory, and nothing traps the host's accesses. One thread at the
lowest priority polls the page:

- **Doorbells** read 0xffffffff while idle. Any other value is a host write; the command doorbell
  writes 0. The thread puts the sentinel back first and then walks the ring, so a doorbell
  written in between still finds its TRBs. While idle the thread reads only the M7 TCM. Host
  memory is touched only after a doorbell, unlike EHCI, which has to walk its schedules.
- **USBCMD, CRCR, ERSTBA, ERDP and IMAN** are compared with what was last published or loaded.
  A USBCMD reset completes only after every other register is back to its default, because
  Linux reads PAGESIZE as soon as HCRST clears.
- **PORTSC:** reserved bit 2 stays set in every published value. Linux writes back only RO and
  RWS bits (`xhci_port_state_to_neutral`) plus the bits it acts on, so a value without bit 2 is
  a host write, even a write-1-to-clear write of an unchanged value.
- **Interrupt acknowledge:** Linux clears USBSTS.EINT and IMAN.IP by writing back the value it
  read, which the M7 can't see. So the interrupt stays pending until ERDP has caught up with
  every posted event; then EINT and IP clear and the line drops. IMOD spaces interrupts as the
  host asked.

Commands: Enable/Disable Slot, Address Device (the controller assigns the address, which is the
slot id), Configure Endpoint, Evaluate Context, Reset/Stop Endpoint, Set TR Dequeue Pointer,
Reset Device and No Op. TRBs: Normal, Setup/Data/Status, Link, Event Data and No Op. A short
packet ends the TD with one event, which is what Linux's bulk path expects. A STALL halts the
endpoint and puts the TRB pointer in the output context. Not supported: isochronous, streams,
LPM, more than one interrupter.

The capabilities are xHCI 1.0, 8 slots, 2 ports (1 = USB 3, 2 = USB 2, each with a Supported
Protocol capability), 32-bit addressing for the A55 stand-in, 32-byte contexts, no scratchpad,
and an ERST of up to 16 segments.

## Virtual drive

The drive is a SuperSpeed device (bcdUSB 3.20, EP0 512 bytes, bulk 1024 bytes with burst 16)
with a BOS descriptor that has no U1/U2. It uses bulk-only transport with SCSI: INQUIRY (with
VPD pages 0 and 0x80), READ CAPACITY 10/16, READ/WRITE 10/12/16, MODE SENSE 6/10, REQUEST
SENSE, TEST UNIT READY and the no-data commands. Each bulk TRB is served at once. Get Max LUN
returns 0, and the class reset is supported. The 64 MiB store is zeroed at boot.

`vxhci null on` skips the copy between the store and the host buffer, so only the protocol path
is measured.

## Measured (2026-11-19)

Linux `dd` with `O_DIRECT` and 1 MiB requests on the drive. The M7 numbers come from
`vxhci perf 5` (cycles of polls that did work, DWT). Data was verified with 48 MiB of random data
written and read back, identical.

| Test | Linux | M7 work | M7 cost | M7-bound ideal |
|---|---|---|---|---|
| Read, RAM drive | 96 MB/s | 64 % | 5.45 cycles/B, 21,900 cycles per 4 KiB TRB | ~1.2 Gbit/s |
| Write, RAM drive | 59 MB/s | 76 % | 10 cycles/B | ~0.64 Gbit/s |
| Read, null drive | 382 MB/s | 5.9 % | 0.12 cycles/B, 628 cycles/TRB | ~53 Gbit/s |
| Write, null drive | 346 MB/s | 6.6 % | 0.15 cycles/B, 658 cycles/TRB | ~43 Gbit/s |
| Virtual EHCI + physical stick, for comparison | 16 MB/s | 10 % | 4.8 cycles/B, 18,000 cycles/stage | ~1.3 Gbit/s |

"M7-bound ideal" is what the data rate would be if the M7 were the only limit.

- The xHCI protocol cost is about **29× lower per descriptor** than EHCI's (628 against 18,000
  cycles). That's because of doorbells (no schedule walking), contiguous TRBs, a single 16-byte
  event per completion, and no QH overlay or qTD token write-back.
- **RAM drive:** the cost is the M7 copying data between its cached store and Linux's
  uncached DDR, at about 150–190 MB/s (writes are slower because uncached reads from Linux DDR
  stall). On PCIe the endpoint's DMA engine would do this copy. On the A55 stand-in, an M7-side
  DMA engine, or cacheable access with explicit cache maintenance, could replace `memcpy`.
- **Null drive:** the M7 is about 94 % free; the limit is on the Linux side. Every byte is
  bounced through swiotlb (32-bit DMA window, as with the EHCI), and bulk-only transport runs
  one command at a time (CBW, data, CSW, one interrupt each).
- TRBs average about 4 KiB, because Linux's scatter-gather segments are single pages (swiotlb).
- 1 interrupt per event: bulk-only transport is serial, so IMOD (40 µs) has nothing to
  coalesce.
