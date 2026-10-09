#!/usr/bin/env python3
"""Find all lis(0x82DD)+addi(-0xA88) materializations of 0x82DCF578,
plus direct lis-relative accesses, with disasm context.
Usage: scan_cement_global.py [window]"""
import sys, struct

WIN = int(sys.argv[1]) if len(sys.argv) > 1 else 24
base = 0x82000000
data = open('/home/z/my-project/scripts/PrototypeRecomp/pristine.bin', 'rb').read()
size = min(0x00E60000, len(data))

def dec(off):
    return struct.unpack_from('>I', data, off)[0]

def fields(w):
    op = w >> 26
    rD = (w >> 21) & 31
    rA = (w >> 16) & 31
    imm = w & 0xFFFF
    return op, rD, rA, imm

hits = []
for off in range(0, size - 4, 4):
    op, rD, rA, imm = fields(dec(off))
    if op == 15 and rA == 0 and imm == 0x82DD:
        # look ahead for addi rY, rD, 0xF578 (-0xA88)
        for k in range(1, WIN):
            w2 = dec(off + 4 * k)
            op2, rD2, rA2, imm2 = fields(w2)
            if op2 == 14 and rA2 == rD and (imm2 & 0xFFFF) == 0xF578:
                hits.append((off, rD, off + 4 * k, rD2))
                break

print(f"materializations of 0x82DCF578: {len(hits)}")
for site, rD, use, rD2 in hits:
    print(f"\n== lis@{base+site:X} (r{rD}) + addi@{base+use:X} (r{rD2}) ==")
    # print 10 insns after the use site, decoded minimal (raw hex)
    for j in range(-2, 14):
        a = use + 4 * j
        if 0 <= a < size:
            w = dec(a)
            print(f"  {base+a:X}: {w:08X}")
