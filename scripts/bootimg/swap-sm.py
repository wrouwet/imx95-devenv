#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replace the System Manager image in an i.MX95 boot image (eMMC copy starting at 32 KiB).

swap-sm.py IN.bin NEW_SM.bin OUT.bin

Container 2 holds OEI, SM, SPL, ...; the SM is the M33 executable image loaded at
0x1ffc0000 (type 3, core 1). Images after it in container 2 and the whole third
container move up by the size difference (rounded to 1 KiB). U-Boot SPL finds the
third container from the end of container 2 (get_imageset_end), so no other
offsets need to change. The image hash (SHA-384 for these containers) is updated.
"""
import hashlib, struct, sys

HDR = struct.Struct('<BHBIHBBHH')
IMG = struct.Struct('<IIQQII64s32s')
ALIGN = 0x400

def rnd(x, a=ALIGN):
    return (x + a - 1) // a * a

def hdr(d, off):
    v = HDR.unpack_from(d, off)
    assert v[2] == 0x87, f"no container at 0x{off:x}"
    return v

def cont_end(d, off):
    """Same as U-Boot get_container_size(): header length, image ends, signature block."""
    ver, length, tag, flags, swv, fusev, n, sig, _ = hdr(d, off)
    end = length
    for i in range(n):
        o, sz = IMG.unpack_from(d, off + HDR.size + i * IMG.size)[:2]
        end = max(end, o + sz)
    if sig:
        sl = struct.unpack_from('<BHB', d, off + sig)[1]
        end = max(end, sig + sl)
    return end

src, smf, dst = sys.argv[1:4]
d = bytearray(open(src, 'rb').read())
sm = open(smf, 'rb').read()

# Container 2 follows container 1's header (aligned like U-Boot's container_hdr_alignment(), 0x8000 here)
c2 = 0x8000
c3_old = rnd(c2 + cont_end(d, c2))
n2 = hdr(d, c2)[6]
c3_len = cont_end(d, c3_old)

ents = [list(IMG.unpack_from(d, c2 + HDR.size + i * IMG.size)) for i in range(n2)]
smi = [i for i, e in enumerate(ents) if (e[4] & 0xf) == 3 and ((e[4] >> 4) & 0xf) == 1 and e[2] == 0x1ffc0000]
assert len(smi) == 1, "SM image not found"
smi = smi[0]
sm_off, sm_size = ents[smi][0], ents[smi][1]
new_size = rnd(len(sm))
shift = new_size - sm_size
assert shift % ALIGN == 0
print(f"SM: offset 0x{sm_off:x}, size 0x{sm_size:x} -> 0x{new_size:x} (shift 0x{shift:x})")

out = bytearray(d[:c2 + sm_off])
out += sm + b'\0' * (new_size - len(sm))
rest_start = c2 + sm_off + sm_size
out += d[rest_start:c3_old]                       # remaining container-2 images (SPL, ...)
assert len(out) == c3_old + shift
out += d[c3_old:c3_old + c3_len]                  # third container, unchanged (offsets are relative)
out += b'\0' * (len(d) - len(out)) if len(out) < len(d) else b''

# Update container-2 entries: SM size + hash, later images shift
for i, e in enumerate(ents):
    if i == smi:
        e[1] = new_size
        htype = (e[4] >> 8) & 7
        h = {0: hashlib.sha256, 1: hashlib.sha384, 2: hashlib.sha512}[htype](bytes(out[c2 + e[0]:c2 + e[0] + new_size])).digest()
        e[6] = h + b'\0' * (64 - len(h))
    elif e[0] > sm_off:
        e[0] += shift
    IMG.pack_into(out, c2 + HDR.size + i * IMG.size, *e)

c3_new = rnd(c2 + cont_end(out, c2))
assert c3_new == c3_old + shift, (hex(c3_new), hex(c3_old + shift))
print(f"third container: 0x{c3_old:x} -> 0x{c3_new:x}; image end 0x{c3_new + c3_len:x}")
assert c3_new + c3_len <= (8 << 20) - 0x8000, "does not fit before the first partition"
open(dst, 'wb').write(out[:rnd(c3_new + c3_len, 0x200)])
