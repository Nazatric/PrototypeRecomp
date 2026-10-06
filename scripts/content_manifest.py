#!/usr/bin/env python3
"""content_manifest.py — deterministic inventory of a supplied game-content
tree (the extracted Xbox 360 Prototype disc).

Usage:  python3 content_manifest.py <content_root> [--json out.json]

Produces a manifest of every file: relative path (lowercased match key),
size, extension, CRC32. Validates the expected disc set for Prototype
(RCF archives + args.txt) and reports what is present / missing so a run
can be verified before launching the runtime.

The game opens these (verified from XEX strings — see
analysis/PHASE2C_CONTENT_PIPELINE.md):
  game:\\00cells.rcf  game:\\01cells.rcf
  game:\\00art.rcf    game:\\01art.rcf    game:\\02art.rcf
"""
import os
import sys
import json
import zlib
import argparse

EXPECTED = [
    "00cells.rcf", "01cells.rcf", "00art.rcf", "01art.rcf", "02art.rcf",
    "args.txt",
]

# Interesting extension classes for the first-pass inventory report.
INTERESTING = {".rcf": "RCF archive (Radical Content Format)",
               ".p3d": "P3D package (game resource)",
               ".xex": "Xbox executable",
               ".xsb": "sound bank",
               ".xwb": "wave bank",
               ".xma": "XMA audio",
               ".bik": "Bink video",
               ".bin": "binary data",
               ".txt": "text"}


def crc32_of(path, chunk=1 << 20):
    c = 0
    size = 0
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            size += len(b)
            c = zlib.crc32(b, c)
    return c, size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--json", dest="json_out", default=None)
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    if not os.path.isdir(root):
        print("ERROR: not a directory: %s" % root)
        return 1

    entries = []
    ext_stats = {}
    total = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for fn in sorted(filenames):
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            try:
                crc, size = crc32_of(full)
            except OSError as e:
                print("WARN: unreadable %s: %s" % (rel, e))
                continue
            ext = os.path.splitext(fn)[1].lower()
            entries.append({"path": rel, "size": size, "crc32": "%08X" % crc,
                            "ext": ext})
            st = ext_stats.setdefault(ext, [0, 0])
            st[0] += 1
            st[1] += size
            total += size

    # Present/missing validation (case-insensitive).
    have = {e["path"].lower(): e for e in entries}
    print("== Content manifest: %s ==" % root)
    print("files: %d   total: %.2f GiB" % (len(entries), total / 2**30))
    print("\n-- Extension classes --")
    for ext in sorted(ext_stats, key=lambda k: -ext_stats[k][1]):
        n, sz = ext_stats[ext]
        note = INTERESTING.get(ext, "")
        print("  %-6s %5d files  %10.2f MiB  %s" %
              (ext or "(none)", n, sz / 2**20, note))
    print("\n-- Expected disc set --")
    ok = True
    for name in EXPECTED:
        e = have.get(name.lower())
        if e:
            print("  PRESENT  %-14s %10d bytes  crc32=%s" %
                  (name, e["size"], e["crc32"]))
        else:
            print("  MISSING  %-14s" % name)
            ok = False
    if not ok:
        print("\nThe runtime will park in the display-init retry loop until "
              "the RCF archives are present (see PHASE2C_CONTENT_PIPELINE.md).")

    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump({"root": root, "total": total, "entries": entries}, f,
                      indent=1)
        print("\nmanifest written: %s" % args.json_out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
