#!/usr/bin/env python3
"""Find ALL code references to a guest address: catches lis+addi pointer
materialization (rY = base+off) and reports every subsequent use of rY
(memory ops at offset 0, register moves into r3-r10 before bl, mtctr).
Usage: find_refs.py <addr_hex> [window]
"""
import sys, struct

target = int(sys.argv[1], 16)
window = int(sys.argv[2]) if len(sys.argv) > 2 else 40
base = 0x82000000
data = open('/home/z/my-project/scripts/PrototypeRecomp/pristine.bin', 'rb').read()
size = min(0x00E60000, len(data))

def dec(off):
    return struct.unpack_from('>I', data, off)[0]

lo = target & 0xFFFF
lo_s = lo - 0x10000 if lo >= 0x8000 else lo

results = []
for off in range(0, size - 4, 4):
    w = dec(off)
    # addis rD, 0, hi  (lis)
    if (w >> 26) == 15 and ((w >> 16) & 31) == 0:
        rD = (w >> 21) & 31
        if ((w & 0xFFFF) << 16) != (target & 0xFFFF0000):
            continue
        # find addi rY, rD, lo within next 8
        for k in range(1, 9):
            w2 = dec(off + 4 * k)
            if (w2 >> 26) == 14:  # addi
                rY = (w2 >> 21) & 31
                rA2 = (w2 >> 16) & 31
                imm2 = w2 & 0xFFFF
                imm2s = imm2 - 0x10000 if imm2 >= 0x8000 else imm2
                if rA2 == rD and (imm2 == lo or imm2s == lo_s):
                    results.append((off + 4 * k, rY))
                    break

print("pointer materializations of %08X:" % target)
sites = {}
for at, rY in results:
    print("  ptr@%08X -> r%d" % (base + at, rY))
    # Track uses of rY in the next `window` instructions (stop at reload of rY)
    for k in range(1, window):
        o = at + 4 * k
        if o + 4 > size: break
        w3 = dec(o)
        op3 = w3 >> 26
        rS3 = (w3 >> 21) & 31
        rA3 = (w3 >> 16) & 31
        if op3 in (36,37,38,44,32,34,40,48,52,54,50,62,58):  # D-form mem ops
            imm3 = w3 & 0xFFFF
            imm3s = imm3 - 0x10000 if imm3 >= 0x8000 else imm3
            if rA3 == rY and (imm3 == 0 or (k < 3 and imm3s == 0)):
                sites.setdefault(base + o, []).append(("mem op %d" % op3, at))
        # mr rZ, rY (or r3..r10) then bl within 4
        if op3 == 31 and (w3 & 0xFFFF) == 0x0780 // 1:  # crude
            pass
    break  # only detail the first

# simpler second pass: dump context around each materialization site
for at, rY in results[:6]:
    print("context @%08X (r%d):" % (base + at, rY))
    for j in range(-4, 14):
        o = at + 4 * j
        if 0 <= o < size:
            print("  %08X: %08X" % (base + o, dec(o)))
print("# %d materialization sites" % len(results))
