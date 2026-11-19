# USB2 on the M7 (EHCI host)

The i.MX95 USB2 controller (ChipIdea, EHCI-compatible, the EVK Type-A port) belongs
to the Cortex-M7. Linux and U-Boot never touch it. Zephyr on the M7 runs it as a USB
host with its own EHCI driver. Verified on the board on 2026-11-19: from a cold eMMC boot,
the M7 enumerates an Apple keyboard (a full-speed hub with the keyboard on port 3) and a
low-speed Logitech mouse on hub port 2. Unplugging and re-plugging is reported and
re-enumerated.

USB1 (DWC3, USB-C) still belongs to Linux.

## Who does what

| Layer | Change | Where |
|---|---|---|
| System Manager | `USB2 OWNER, kpa=0, sid=0xf` moved from the A55 non-secure LM to the M7 LM. configtool also gives the M7 SCMI access to `PD_HSIO_TOP`. USB2's bus-master domain becomes the M7's, so its DMA has the M7's memory rights (its DDR window and TCM). | `mx95devenv.cfg`, [diff](sm-mx95devenv-vs-mx95evkrpmsg.diff) |
| U-Boot | Default `bootcmd` without `bootflow scan`. Scanning USB boot devices touched USB2, faulted and reset the board. | `recipes-bsp/u-boot/files/devenv-fdt.cfg` (`CONFIG_BOOTCOMMAND`) |
| Linux dts | `&usb2`, `&usbmisc`, `&usbphynop` disabled. `m7-usb2-dma` node with only `iommus = <&smmu 0xf>`. `&reg_usb_vbus` always-on. | `imx95-19x19-evk-devenv.dts` |
| Linux boot | `m7-dma-handoff.service` sets the IOMMU group of every `devenv,m7-dma-master` node to `identity`, before `m7-autostart`. | `recipes-devenv/m7-tools` |
| Zephyr | EHCI host driver `uhc_ci_ehci` (Zephyr module `zephyr/modules/devenv`), USB host stack with hub class, `src/usb.c` (start at boot, device reports, `usb2` shell). | `zephyr/` |

Why each Linux piece is needed:
- **SMMU.** USB2's DMA goes through the A55's SMMU, which Linux owns. Since Linux 6.x the SMMU
  aborts streams that Linux has not set up. The driverless `m7-usb2-dma` node gives stream 0xf an
  IOMMU group without any USB register access, and `m7-dma-handoff` makes that group untranslated.
  The NETC VF uses the same mechanism (`m7-vf-handoff`).
- **VBUS.** The Type-A port's 5 V comes from a GPIO on the I2C7 expander (`reg_usb_vbus`), which
  Linux owns. With no USB2 consumer left, Linux would switch it off at boot.

## Zephyr driver (`zephyr/modules/devenv/drivers/usb/uhc/uhc_ci_ehci.c`)

Written for this repo from the EHCI 1.0 spec and the i.MX reference manual (Apache-2.0, no
GPL code). It implements Zephyr's UHC API, so Zephyr's `usbh` stack, hub class and shell run on top.

- One root port. Control, bulk and interrupt transfers; no isochronous; no suspend/resume.
- High, full and low speed. Full- and low-speed devices go through the ChipIdea's integrated
  transaction translator (`PORTSC.PSPD` gives the speed); behind hubs the TT is the nearest HS
  hub, or the root port's own.
- Each endpoint gets a queue head; transfers queue per endpoint, one on the bus at a time.
  Interrupt endpoints are polled every frame (1 ms).
- EHCI structures (frame list, 32 QHs, 128 qTDs, about 12 KiB) live in the first 32 KiB of DTCM:
  never cached on the M7, and the controller reaches them at `0x20400000 + offset`
  (`devenv,pool`, `devenv,pool-bus-addr`). Transfer buffers come from the UHC pool in DDR and are
  cleaned or invalidated around DMA (they are cache-line aligned).
- A driver thread handles port changes (100 ms debounce, 50 ms reset, speed) and completions.
  The ISR only collects `USBSTS` bits.
- The controller is only touched once the host stack initializes it, not at boot (with an SM that
  doesn't give USB2 to the M7, the first access is a bus fault).

Console messages (M7 UART and the RPMsg shell):
```
uhc_ci_ehci: USB2: ChipIdea EHCI 1.00, id 0xe4a1fa05, 1 port(s), port power control
uhc_ci_ehci: USB2: EHCI running (USBCMD 0x00010b31, USBSTS 0x0000c000, PORTSC 0x10001801)
m7_usb:      USB2: host started, waiting for devices
uhc_ci_ehci: USB2: device attached (line state 2), resetting port
uhc_ci_ehci: USB2: full-speed device connected
m7_usb:      USB2: device 05ac:1003 at address 1 (full-speed, root port), class 09 (hub), USB 1.10
usbh_hub:    Device ready on port 2 (speed: LOW)
m7_usb:      USB2: device 046d:c016 at address 2 (low-speed, hub 1 port 2), class 00 (per-interface), USB 2.00
m7_usb:      USB2: device 05ac:020b at address 3 (full-speed, hub 1 port 3), class 00 (per-interface), USB 1.10
uhc_ci_ehci: USB2: device disconnected
```
The `m7_usb` device lines come from a "monitor" class in `src/usb.c`. It matches every device,
prints, and declines, so the real class drivers still bind. Its name sorts before them.

Shell: `usb2 start` (when `CONFIG_APP_USB2_AUTOSTART=n`), `usb2 regs` (EHCI register dump), and
Zephyr's `usbh ...` commands. `devmem` is enabled for raw register access.

Not done yet: class drivers for the HID devices (keyboard/mouse reports), isochronous transfers,
suspend/resume, and M7-side SCMI requests for the HSIO power domain and clock. Today Linux keeps
those on for USB1 and PCIe; once USB1 also moves, the M7 has to request them itself (it already
has the permission).

## Boot image built outside Yocto (2026-11-19)

The T480 with the Yocto build was not available, so the board's boot image was changed in place:
- The SM was rebuilt from imx-sm `lf-6.18.37-2.1.0` with **Arm GNU Toolchain 15.2.rel1** (what
  imx-sm names, and what Yocto uses). A build with Ubuntu's `gcc-arm-none-eabi` 14.2 boot-loops:
  the board resets right after the OEI, before "Hello from SM".
- `scripts/bootimg/swap-sm.py` put it into the existing boot image (same slot, hash updated),
  and `scripts/bootimg/patch-bytes.py` changed U-Boot's default `bootcmd` in the image (same length,
  hash updated). See [scripts/bootimg/README.md](../scripts/bootimg/README.md).

The next Yocto build has the same content from source: the SM cfg, the U-Boot fragment, the dts,
and m7-tools. That build is not verified yet. Remember to rebuild Zephyr first, so the image ships
the USB firmware as default.

Testing a boot image without writing the eMMC: set SW7=1001 and power-cycle, then run
`uuu -b spl <image>`. U-Boot then sits in fastboot (on this PC it enumerates as `1fc9:0152`). To boot
Linux from eMMC from there: `uuu FB: ucmd env default -a`, `FB: ucmd setenv mmcdev 0`,
`FB: ucmd setenv mmcroot /dev/mmcblk0p2 rootwait rw`, `FB: acmd run bsp_bootcmd`. A Linux
`reboot` only resets the A55 LM. A new SM needs a power cycle (or `lm ap reset` on the SM
debug monitor, which only restarts the A55).
