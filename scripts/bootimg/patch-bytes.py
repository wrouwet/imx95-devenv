#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Patch a byte string inside one image of an i.MX95 boot image and fix that image's hash.

patch-bytes.py IN.bin OUT.bin OLD NEW      (NEW must be the same length as OLD; OLD must occur once)
"""
import hashlib, struct, sys

HDR = struct.Struct('<BHBIHBBHH')
IMG = struct.Struct('<IIQQII64s32s')

src, dst, old, new = sys.argv[1], sys.argv[2], sys.argv[3].encode(), sys.argv[4].encode()
assert len(old) == len(new), "NEW must have the same length as OLD"
d = bytearray(open(src, 'rb').read())
assert d.count(old) == 1, f"OLD found {d.count(old)} times"
pos = d.find(old)
d[pos:pos + len(old)] = new

done = False
for c in range(0, min(len(d), 8 << 20), 0x400):
    ver, length, tag, flags, swv, fusev, n, sig, _ = HDR.unpack_from(d, c)
    if tag != 0x87 or ver not in (0, 2) or not 1 <= n <= 8:
        continue
    for i in range(n):
        e = list(IMG.unpack_from(d, c + HDR.size + i * IMG.size))
        o, sz = c + e[0], e[1]
        if o <= pos < o + sz:
            htype = (e[4] >> 8) & 7
            h = {0: hashlib.sha256, 1: hashlib.sha384, 2: hashlib.sha512}[htype](bytes(d[o:o + sz])).digest()
            e[6] = h + b'\0' * (64 - len(h))
            IMG.pack_into(d, c + HDR.size + i * IMG.size, *e)
            print(f"patched 0x{pos:x} in container @0x{c:x} img{i} (dst 0x{e[2]:x}), hash updated")
            done = True
assert done, "offset not inside any container image"
open(dst, 'wb').write(d)
