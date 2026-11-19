# Boot image tools (i.MX95 AHAB containers)

Small tools for working on a finished i.MX95 boot image (the `flash.bin` / `imx-boot` that sits
at eMMC offset 32 KiB) without a full Yocto/mkimage build. They are plain Python 3 with no
dependencies.

| Tool | What it does |
|---|---|
| `ahab.py IMG` | List the containers and images, and check each image's SHA hash |
| `swap-sm.py IMG NEW_SM.bin OUT` | Replace the System Manager image (M33, loaded at `0x1ffc0000`), shift later images if it grew, and update its hash |
| `patch-bytes.py IN OUT OLD NEW` | Replace a same-length byte string inside one image (e.g. a U-Boot default env variable) and update that image's hash |

Layout of the devenv image (`flash_a55`, B0): container 1 at `0x0` holds the ELE firmware (NXP signed,
left alone). Container 2 at `0x8000` holds the OEI with DDR firmware, the SM, and U-Boot SPL.
Container 3 follows container 2's last image (U-Boot SPL finds it there, `get_imageset_end()`)
and holds TF-A, U-Boot and OP-TEE. The image hashes are SHA-384. Nothing is signed (open lifecycle).

Read the current image from the board and keep a copy before writing anything:
```sh
ssh root@<board> 'dd if=/dev/mmcblk0 bs=1k skip=32 count=8160' > emmc-32k-8m.bin
```
Write (from Linux on the board, nothing mounted there): `dd if=img of=/dev/mmcblk0 bs=1k seek=32 conv=fsync`.
Test first over USB without writing anything (SW7=1001, `uuu -b spl img`), see
[docs/m7-usb2.md](../../docs/m7-usb2.md).

Build the SM with Arm GNU Toolchain 15.2.rel1 (`make config=mx95devenv TOOLS=<dir with the
toolchain> all`). A build with Ubuntu's `gcc-arm-none-eabi` 14.2 did not boot.
