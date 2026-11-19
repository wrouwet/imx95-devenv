#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""List the AHAB containers of an i.MX95 boot image (as written at eMMC 32 KiB)."""
import struct, sys, hashlib

HDR = struct.Struct('<BHBIHBBHH')            # version, length, tag, flags, sw_ver, fuse_ver, num_images, sig_off, reserved
IMG = struct.Struct('<IIQQII64s32s')         # offset, size, dst, entry, hab_flags, meta, hash, iv

found_any = False
def main(path):
    global found_any
    data = open(path, 'rb').read()
    off = 0
    seen = []
    # Containers live at 1 KiB boundaries; scan the first few MiB
    for off in range(0, min(len(data), 8 << 20), 0x400):
        ver, length, tag, flags, swv, fusev, n, sig, _ = HDR.unpack_from(data, off)
        if tag != 0x87 or ver not in (0, 2) or not (1 <= n <= 8):
            continue
        found_any = True
        print(f"container @0x{off:06x}: ver {ver} len 0x{length:x} flags 0x{flags:08x} images {n} sig@0x{sig:x}")
        for i in range(n):
            o, sz, dst, entry, hf, meta, h, iv = IMG.unpack_from(data, off + HDR.size + i * IMG.size)
            img = data[off + o: off + o + sz]
            htype = (hf >> 8) & 7      # 0=sha256 1=sha384 2=sha512
            hfn = {0: hashlib.sha256, 1: hashlib.sha384, 2: hashlib.sha512}.get(htype)
            hl = {0: 32, 1: 48, 2: 64}.get(htype, 64)
            ok = hfn and hfn(img).digest() == h[:hl]
            print(f"  img{i}: off 0x{o:06x} (abs 0x{off+o:06x}) size 0x{sz:06x} dst 0x{dst:010x} entry 0x{entry:010x} "
                  f"flags 0x{hf:08x} (type {hf&0xf}, core {(hf>>4)&0xf}, hash {htype}) meta 0x{meta:08x} hash {'OK' if ok else 'MISMATCH'}")

main(sys.argv[1])
