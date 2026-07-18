#!/usr/bin/env python3
"""Parse a PS-X EXE header (A1 step 4).

Layout (offsets from file start):
  0x00  8 bytes  magic "PS-X EXE"
  0x10  u32      initial PC
  0x14  u32      initial GP (r28)
  0x18  u32      text load address (taddr)
  0x1C  u32      text size in bytes (tsize) = file size - 2048
  0x30  u32      initial SP base (usually 0x801ffff0)
  0x34  u32      SP offset (added to base; 0 => sp = base)

Usage: psx_exe_info.py FILE...   (prints a Markdown table)
"""

import struct
import sys

HEADER_SIZE = 2048


def parse(path):
    with open(path, "rb") as fh:
        data = fh.read(HEADER_SIZE)
    if len(data) < HEADER_SIZE:
        raise ValueError(f"{path}: too small for a PS-X EXE header")
    magic = data[0x00:0x08]
    pc, gp, taddr, tsize = struct.unpack_from("<4I", data, 0x10)
    sp_base, sp_off = struct.unpack_from("<2I", data, 0x30)
    return {
        "path": path,
        "magic_ok": magic == b"PS-X EXE",
        "pc": pc,
        "gp": gp,
        "taddr": taddr,
        "tsize": tsize,
        "sp": (sp_base + sp_off) if sp_off else sp_base,
    }


def main(argv):
    rows = [parse(p) for p in argv]
    print("| file | magic | initial PC | initial GP | load addr | text size | initial SP |")
    print("|---|---|---|---|---|---|---|")
    for r in rows:
        print(
            f"| {r['path']} | {'OK' if r['magic_ok'] else 'BAD'} "
            f"| 0x{r['pc']:08X} | 0x{r['gp']:08X} | 0x{r['taddr']:08X} "
            f"| {r['tsize']} (0x{r['tsize']:X}) | 0x{r['sp']:08X} |"
        )
    return 0 if all(r["magic_ok"] for r in rows) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
