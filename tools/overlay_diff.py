#!/usr/bin/env python3
"""overlay_diff.py — A3 Step 2 relocation test.

Byte-compares overlay captures (dirty-RAM .bin dumps keyed by whole-region
CRC32, or pairs of explicit files) to answer: are Xenogears' streamed code
overlays byte-stable across loads (statically compilable), or does the
loader relocate/patch code per load?

Classification per differing WORD (4-byte aligned):
  - RAM-POINTER  : both old and new values look like PS1 RAM pointers
                    (0x80000000-0x801FFFFF) or one is 0 -> dynamic data the
                    game installs at runtime (vtable/struct ptr). NOT code
                    relocation; per-function code-hash keying absorbs it.
  - CODE-CHANGE  : the differing bytes alter instruction encodings ->
                    genuine code patching/relocation. Those overlays stay
                    interpreted unless upstream keyed-by-relocation lands.
  - DATA-OTHER   : anything else (counters, flags, IDs) -> volatile data.

Usage:
  overlay_diff.py A.bin B.bin          # diff two explicit files
  overlay_diff.py overlays/            # group by size, diff near-identical pairs
"""

import os
import struct
import sys
import collections

RAM_LO, RAM_HI = 0x80000000, 0x80200000


def words(data, off):
    return struct.unpack_from("<I", data, off)[0]


def is_ram_ptr(v):
    return RAM_LO <= v < RAM_HI


def classify(wa, wb):
    if (is_ram_ptr(wa) or wa == 0) and (is_ram_ptr(wb) or wb == 0):
        return "RAM-POINTER"
    return "OTHER/CODE?"


def diff_words(da, db):
    """Yield (word_off, wa, wb) for every differing aligned word."""
    n = min(len(da), len(db)) & ~3
    for off in range(0, n, 4):
        wa = struct.unpack_from("<I", da, off)[0]
        wb = struct.unpack_from("<I", db, off)[0]
        if wa != wb:
            yield off, wa, wb


def report_pair(pa, pb, verbose=False):
    da, db = open(pa, "rb").read(), open(pb, "rb").read()
    nbytes = sum(1 for x, y in zip(da, db) if x != y)
    dws = list(diff_words(da, db))
    cats = collections.Counter(classify(wa, wb) for _, wa, wb in dws)
    pct = nbytes / max(len(da), len(db)) * 100
    print(f"{os.path.basename(pa)} vs {os.path.basename(pb)}: "
          f"{nbytes} bytes ({pct:.1f}%), {len(dws)} words; "
          f"{dict(cats)}")
    if verbose:
        for off, wa, wb in dws[:40]:
            print(f"    0x{off:06X}: {wa:08X} -> {wb:08X}  {classify(wa, wb)}")
    return nbytes, dws, cats


def scan_dir(d):
    bins = {os.path.basename(p): open(p, "rb").read() for p in
            sorted(glob_join(d, "*.bin"))}
    by_size = collections.defaultdict(list)
    for name, data in bins.items():
        by_size[len(data)].append(name)
    print(f"{len(bins)} bins, {len(by_size)} sizes; near-identical pairs:")
    for size, names in sorted(by_size.items()):
        if len(names) < 2:
            continue
        for i in range(len(names)):
            for j in range(i + 1, len(names)):
                da, db = bins[names[i]], bins[names[j]]
                nbytes = sum(1 for x, y in zip(da, db) if x != y)
                pct = nbytes / size * 100
                if pct < 1.0:  # near-identical: same module, volatile bytes only
                    print(f"  [{size}] ", end="")
                    report_pair(os.path.join(d, names[i]),
                                os.path.join(d, names[j]))


def glob_join(d, pat):
    import glob
    return glob.glob(os.path.join(d, pat))


def main():
    args = sys.argv[1:]
    verbose = "-v" in args
    args = [a for a in args if a != "-v"]
    if len(args) == 2:
        report_pair(args[0], args[1], verbose)
    elif len(args) == 1 and os.path.isdir(args[0]):
        scan_dir(args[0])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
