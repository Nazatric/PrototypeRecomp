#!/usr/bin/env python3
"""Find all bl/b call sites targeting a given guest address in pristine.bin.
Usage: find_callers.py <target_hex> [image_base_hex] [image_size_hex]
pristine.bin is the decompressed image loaded at 0x82000000.
"""
import sys, struct

target = int(sys.argv[1], 16)
base = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x82000000
size = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x00E60000

data = open('/home/z/my-project/scripts/PrototypeRecomp/pristine.bin', 'rb').read()
n = 0
for off in range(0, min(size, len(data)) - 4, 4):
    w = struct.unpack_from('>I', data, off)[0]
    if (w & 0xFC000003) == 0x48000001:  # bl with AA=0 LK=1
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        site = base + off
        if site + li == target:
            print("bl  at %08X -> %08X" % (site, target))
            n += 1
print("# %d callers found" % n)
