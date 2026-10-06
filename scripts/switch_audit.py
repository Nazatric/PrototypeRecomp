#!/usr/bin/env python3
"""Phase 2D switch-table audit (v2).

Exact-instruction scanner: finds `lwzx rT, rA, rB; mtctr rT; bctr` dispatch
sites, derives the table base via lis/addi (sign-extended), reads the REAL
entry count from pristine.bin, and cross-checks switch_tables_phase2.toml for
MISSING / TRUNCATED entries.

Usage: python3 scripts/switch_audit.py
"""
import struct
import re
import bisect

PRISTINE = '/home/z/my-project/scripts/PrototypeRecomp/pristine.bin'
TOML = '/home/z/my-project/scripts/PrototypeRecomp/switch_tables_phase2.toml'
MAPPING = '/home/z/my-project/analysis/out/phase2/ppc_func_mapping.cpp'
BASE = 0x82000000
SIZE = 0xE60000

data = open(PRISTINE, 'rb').read()

starts = []
with open(MAPPING) as f:
    for line in f:
        m = re.match(r'\t\{ (0x[0-9A-Fa-f]+),', line)
        if m:
            starts.append(int(m.group(1), 16))
starts.sort()

def func_range(addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i < 0:
        return None
    return starts[i], (starts[i + 1] if i + 1 < len(starts) else BASE + SIZE)

def w(off):
    return struct.unpack_from('>I', data, off)[0]

def sx16(v):
    return v - 0x10000 if v >= 0x8000 else v

# Parse TOML
config = {}
cur_base = None
cur_labels = []
with open(TOML) as f:
    for line in f:
        m = re.match(r'base = (0x[0-9A-Fa-f]+)', line)
        if m:
            cur_base = int(m.group(1), 16)
            cur_labels = []
            continue
        m = re.match(r'\s*(0x[0-9A-Fa-f]+),?\s*$', line)
        if m and cur_base is not None:
            cur_labels.append(int(m.group(1), 16))
            continue
        if line.strip() == ']' and cur_base is not None:
            config[cur_base] = cur_labels
            cur_base = None

BCTR = 0x4E800420
MTCTR = lambda r: 0x7C0903A6 | (r << 21)      # mtspr ctr (SPR=9 field in bits 20-16)
LWZX = lambda d, a, b: 0x7C00002E | (d << 21) | (a << 16) | (b << 11)

missing, truncated, ok = [], [], []
checked = 0

for off in range(0, SIZE - 4, 4):
    if w(off) != BCTR:
        continue
    # find lwzx within 8 insns back
    lwzx = None
    for k in range(1, 9):
        w2 = w(off - 4 * k)
        if (w2 >> 26) == 31 and ((w2 >> 1) & 0x3FF) == 23:  # lwzx
            lwzx = ((w2 >> 21) & 31, (w2 >> 16) & 31, (w2 >> 11) & 31)
            lwzx_at = k
            break
    if not lwzx:
        continue
    rT, rA, rB = lwzx
    if rA == 0 and rB == 0:
        continue
    # mtctr rT between lwzx and bctr
    has_mtctr = any(w(off - 4 * k) == MTCTR(rT) for k in range(1, lwzx_at))
    if not has_mtctr:
        continue
    base_reg = rA if rA != 0 else rB
    # derive table base: lis base_reg, HI ... addi base_reg, base_reg, LO
    table_addr = None
    for k in range(lwzx_at + 1, lwzx_at + 14):
        w2 = w(off - 4 * k)
        if (w2 >> 26) == 15 and ((w2 >> 21) & 31) == base_reg and \
           ((w2 >> 16) & 31) == 0:  # lis base_reg, HI
            hi = w2 & 0xFFFF
            for j in range(k - 1, 0, -1):
                w3 = w(off - 4 * j)
                if (w3 >> 26) == 14 and ((w3 >> 21) & 31) == base_reg and \
                   ((w3 >> 16) & 31) == base_reg:  # addi base_reg, base_reg, LO
                    table_addr = ((hi << 16) + sx16(w3 & 0xFFFF)) & 0xFFFFFFFF
                    break
            if table_addr is not None:
                break
    if table_addr is None or not (0x82000000 < table_addr < BASE + SIZE):
        continue
    tbl_off = table_addr - BASE
    if tbl_off + 4 > len(data):
        continue
    fr = func_range(BASE + off)
    if not fr:
        continue
    fstart, fend = fr
    entries = []
    for n in range(512):
        v = w(tbl_off + 4 * n)
        if v == 0 or not (fstart <= v < fend):
            break
        entries.append(v)
    if len(entries) < 2:
        continue
    checked += 1
    # config lookup: entry keyed anywhere within 16 insns before the bctr
    cfg = None
    for cb, cl in config.items():
        if BASE + off - 64 <= cb <= BASE + off:
            cfg = cl
            break
    bctr_addr = BASE + off
    if cfg is None:
        missing.append((bctr_addr, table_addr, len(entries)))
    elif len(cfg) < len(entries):
        truncated.append((bctr_addr, table_addr, len(cfg), len(entries)))
    else:
        ok.append(bctr_addr)

print(f"dispatch sites checked: {checked}  ok={len(ok)}")
print(f"\nMISSING config entries ({len(missing)}):")
for a, t, n in missing[:50]:
    print(f"  bctr {a:08X} table {t:08X} entries={n}")
print(f"\nTRUNCATED entries ({len(truncated)}):")
for a, t, cn, rn in truncated[:50]:
    print(f"  bctr {a:08X} table {t:08X} config={cn} real={rn}  "
          f"<<< {rn - cn} cases UNDEFINED")
