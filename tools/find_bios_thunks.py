#!/usr/bin/env python3
"""find_bios_thunks.py — inventory packed PsyQ BIOS dispatch thunks in the EXE.

Scans the main EXE text for the PsyQ libapi/libetc thunk pattern
(mirrors the recompiler's own detector, function_analysis.cpp Pass 2.56
`is_bios_dispatch_thunk`):

    addiu $tN, $zero, 0xA0|0xB0|0xC0     ; w0: opcode 0x09, rs=$zero, imm=vector
    jr    $tN                            ; w1: SPECIAL fn 0x08, rs==same $tN
    addiu $t1, $zero, index              ; w2: opcode 0x09, rs=$zero, rt=$t1(9)

Output: seeds/slus_00664_bios_thunks.txt (TombaRecomp format: grouped by
vector, `V0xINDEX  Name  0xVADDR`).

NOTE: `[recompiler] bios_thunks` is a DEAD config field at pin 678c71f (parsed
but never consumed; see docs/pin-history.md) and thunk DETECTION is already
unconditional in the recompiler. This inventory is therefore documentation
(kernel-usage map, detector cross-check) and future-proofing for a pin bump —
it is intentionally NOT wired into game.toml.

Usage: tools/find_bios_thunks.py [exe] [out]
"""

import os
import struct
import sys

EXE_VADDR = 0x80010000
EXE_HEADER = 0x800  # PS-X EXE: text starts at file offset 2048

# Well-established PSX BIOS call names (psx-spx public docs). Unknown indices
# are emitted as "-". Keep this conservative: only names that are certain.
A0_NAMES = {
    0x00: "FileOpen", 0x01: "Seek", 0x02: "Read", 0x03: "Write", 0x04: "Close",
    0x05: "Ioctl", 0x06: "exit", 0x07: "isatty", 0x08: "GetChar", 0x09: "PutChar",
    0x13: "setjmp", 0x14: "longjmp",
    0x15: "strcat", 0x16: "strncat", 0x17: "strcmp", 0x18: "strncmp",
    0x19: "strcpy", 0x1A: "strncpy", 0x1B: "strlen", 0x1E: "strchr",
    0x1F: "strrchr", 0x23: "strtok", 0x24: "strstr",
    0x25: "toupper", 0x26: "tolower", 0x27: "bcopy", 0x28: "bzero",
    0x29: "bcmp", 0x2A: "memcpy", 0x2B: "memset", 0x2C: "memmove",
    0x2D: "memcmp", 0x2E: "memchr", 0x2F: "rand", 0x30: "srand",
    0x31: "qsort", 0x33: "malloc", 0x34: "free", 0x36: "bsearch",
    0x37: "calloc", 0x38: "realloc", 0x39: "InitHeap", 0x3A: "_exit",
    0x3B: "getchar", 0x3C: "putchar", 0x3D: "gets", 0x3E: "puts",
    0x3F: "printf", 0x42: "Load", 0x43: "Exec", 0x44: "FlushCache",
    0x46: "GPU_dw", 0x47: "mem2vram", 0x48: "SendGPUStatus", 0x49: "GPU_cw",
    0x4A: "GPU_cwb", 0x4B: "SendPackets", 0x70: "_bu_init", 0x71: "_96_init",
}
B0_NAMES = {
    0x00: "SysMalloc", 0x01: "SysFree",
    0x08: "OpenEvent", 0x09: "CloseEvent", 0x0A: "WaitEvent", 0x0B: "TestEvent",
    0x0C: "EnableEvent", 0x0D: "DisableEvent",
    0x0E: "OpenTh", 0x0F: "CloseTh", 0x10: "ChangeTh",
    0x13: "StartPAD", 0x14: "StopPAD",
    0x17: "ReturnFromException", 0x18: "SetDefaultExitFromException",
    0x19: "SetCustomExitFromException",
    0x3F: "toupper?",  # rarely used; keep only certain ones below
    0x42: "printf", 0x45: "SetConf", 0x46: "GetConf",
    0x56: "_96_CdromRead",
}
B0_NAMES.pop(0x3F)  # not certain enough
C0_NAMES = {
    0x00: "EnqueueTimerAndVblankIrqs", 0x01: "EnqueueSyscallHandler",
    0x02: "SysEnqIntRP", 0x03: "SysDeqIntRP",
    0x06: "ExceptionHandler", 0x08: "SysInitMemory",
    0x09: "SysInitKernelVariables", 0x0A: "ChangeClearRCnt",
    0x0C: "InitDefInt",
}
NAMES = {0xA0: A0_NAMES, 0xB0: B0_NAMES, 0xC0: C0_NAMES}


def is_li_zero_u16(w):
    """addiu $rt, $zero, imm16 -> (rt, imm) or None."""
    if (w >> 26) != 0x09 or ((w >> 21) & 0x1F) != 0:
        return None
    return ((w >> 16) & 0x1F, w & 0xFFFF)


def main() -> int:
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = (sys.argv[1] if len(sys.argv) > 1
           else os.path.join(repo, "..", "game", "slus_006.64"))  # game.toml: exe
    out = (sys.argv[2] if len(sys.argv) > 2
           else os.path.join(repo, "seeds", "slus_00664_bios_thunks.txt"))

    with open(exe, "rb") as f:
        data = f.read()
    text = data[EXE_HEADER:]

    thunks = []  # (vaddr, vector, index)
    for off in range(0, len(text) - 12, 4):
        w0, w1, w2 = struct.unpack_from("<III", text, off)
        li = is_li_zero_u16(w0)
        if not li or li[1] not in (0xA0, 0xB0, 0xC0):
            continue
        reg, vector = li
        # w1 must be jr $reg (SPECIAL, fn=0x08, rs=reg)
        if (w1 >> 26) != 0 or (w1 & 0x3F) != 0x08 or ((w1 >> 21) & 0x1F) != reg:
            continue
        li2 = is_li_zero_u16(w2)
        if not li2 or li2[0] != 9:
            continue
        thunks.append((EXE_VADDR + off, vector, li2[1]))

    lines = [
        "# Xenogears (SLUS-00664) PSX BIOS thunk inventory",
        f"# Source: {exe}",
        "# Pattern: li tN, 0x{A0|B0|C0}; jr tN; li t1, FN  (PSY-Q libapi/libetc style)",
        f"# Total thunks: {len(thunks)}",
        "# NOTE: documentation only at pin 678c71f — [recompiler] bios_thunks is a",
        "# dead config field there; detection is unconditional (docs/pin-history.md).",
        "",
    ]
    for vector in (0xA0, 0xB0, 0xC0):
        group = [(a, i) for a, v, i in thunks if v == vector]
        names = NAMES[vector]
        uniq = sorted({i for _, i in group})
        lines.append(f"## {vector:02X} vector ({len(group)} thunks, {len(uniq)} unique)")
        for addr, idx in sorted(group):
            name = names.get(idx, "-")
            lines.append(f"  {vector:02X}:0x{idx:02X}  {name:<28} 0x{addr:08X}")
        lines.append("")

    with open(out, "w") as f:
        f.write("\n".join(lines))
    print(f"{len(thunks)} thunks -> {out}")
    for vector in (0xA0, 0xB0, 0xC0):
        group = [(a, i) for a, v, i in thunks if v == vector]
        named = sum(1 for _, i in group if i in NAMES[vector])
        print(f"  {vector:02X}: {len(group)} ({named} named)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
