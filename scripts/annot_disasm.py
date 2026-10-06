#!/usr/bin/env python3
"""Annotate xex_dump --disasm output: convert decimal branch targets to hex."""
import sys

for line in sys.stdin:
    line = line.rstrip("\n")
    # Find decimal call targets like "bl  2191751468" / "b 2189303456" / bne/beq forms
    out = line
    # match any run of 8-10 digits >= 0x80000000-ish (guest addresses)
    import re
    def repl(m):
        v = int(m.group(0))
        if v >= 0x80000000 or (v > 100000000):
            return "0x%08X" % v
        return m.group(0)
    out = re.sub(r"\b(2[0-9]{9})\b", repl, out)
    print(out)
