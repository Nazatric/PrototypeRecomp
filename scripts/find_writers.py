#!/usr/bin/env python3
"""Scan pristine.bin for code that STORES to a given absolute guest address.
Handles: lis+stw/lfs/stb/lhz with offset, lis+addi+stw (reg-held base),
addis/ori pairs. Reports every store site with 6 instructions of context.
Usage: find_writers.py <addr_hex>
"""
import sys, struct

target = int(sys.argv[1], 16)
base = 0x82000000
data = open('/home/z/my-project/scripts/PrototypeRecomp/pristine.bin', 'rb').read()
size = min(0x00E60000, len(data))

def dec(off):
    return struct.unpack_from('>I', data, off)[0]

# Collect (addr_of_lis, reg) -> high value; then look for stores with the
# matching low offset within the next 12 instructions.
hits = []
for off in range(0, size - 4, 4):
    w = dec(off)
    op = w >> 26
    if op == 15:  # addis (lis when rA=0)
        rD = (w >> 21) & 31
        rA = (w >> 16) & 31
        if rA != 0:
            continue
        imm = w & 0xFFFF
        hi = (imm << 16) & 0xFFFFFFFF
        if hi != (target & 0xFFFF0000):
            continue
        # look ahead for store with offset == target low half
        lo = target & 0xFFFF
        lo_s = lo - 0x10000 if lo >= 0x8000 else lo
        for k in range(1, 13):
            w2 = dec(off + 4 * k)
            op2 = w2 >> 26
            # stw=36 stb=38 sth=44 lfs=48 stfs=52 stfd=54 stwu=37 std=62
            if op2 in (36, 37, 38, 44, 48, 52, 54, 62):
                rA2 = (w2 >> 16) & 31
                off2 = w2 & 0xFFFF
                off2s = off2 - 0x10000 if off2 >= 0x8000 else off2
                if rA2 == rD and (off2 == lo or off2s == lo_s or off2 == lo - 0x10000):
                    hits.append((base + off, base + off + 4 * k, op2))
                    break

print("stores to %08X (lis+rD, store within 12 insns):" % target)
for lis_at, st_at, op2 in hits:
    print("  lis@%08X -> store@%08X (op %d)" % (lis_at, st_at, op2))
    for j in range(-2, 6):
        o = st_at - base + 4 * j
        if 0 <= o < size:
            print("    %08X: %08X" % (base + o, dec(o)))
print("# %d sites" % len(hits))
