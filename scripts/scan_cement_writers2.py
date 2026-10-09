#!/usr/bin/env python3
"""Find all stores into the cement-global region 0x82DCF578..0x82DCF678,
via ANY base materialization (lis+addi) whose base+store_off lands inside.
This catches writes through moved/derived bases the direct scan misses."""
import struct

data = open('/home/z/my-project/scripts/PrototypeRecomp/pristine.bin', 'rb').read()
size = min(0x00E60000, len(data))
BASE = 0x82000000
LO, HI = 0x82DCF578, 0x82DCF678  # struct + slack

def dec(off):
    return struct.unpack_from('>I', data, off)[0]

# 1) collect all lis(rA=0)+addi pairs -> (materialized_value, use_site)
mats = []
for off in range(0, size - 4, 4):
    w = dec(off)
    if (w >> 26) == 15 and ((w >> 16) & 31) == 0:
        rD = (w >> 21) & 31
        hi = (w & 0xFFFF) << 16
        for k in range(1, 10):
            w2 = dec(off + 4 * k)
            if (w2 >> 26) == 14 and ((w2 >> 16) & 31) == rD:
                imm = w2 & 0xFFFF
                lo = imm - 0x10000 if imm >= 0x8000 else imm
                mats.append((hi + lo, off + 4 * k, (w2 >> 21) & 31))
                break

# 2) for each materialization whose value is within [LO-0x7FFF, HI+0x7FFF],
#    scan forward 30 insns for stores via that register landing in [LO,HI]
for val, site, reg in mats:
    if not (LO - 0x7FFF <= val <= HI + 0x7FFF):
        continue
    for k in range(0, 30):
        a = site + 4 * k
        if a + 4 > size:
            break
        w = dec(a)
        op = w >> 26
        if op in (36, 44, 38, 47):  # stw stfd stb stmw... 36=stw,44=stfd? (44=stfd is 54?) keep stw(36),stb(38),sth(44),std(62?)
            rA = (w >> 16) & 31
            imm = w & 0xFFFF
            off_s = imm - 0x10000 if imm >= 0x8000 else imm
            if rA == reg:
                ea = val + off_s
                if LO <= ea <= HI:
                    print(f"STORE {ea:08X} (struct+{ea-LO:X}) at {BASE+a:08X} op={op} (base {val:08X} mat@{BASE+site:08X} r{reg})")
        # also track mr reg->reg3 for calls? (skip)
