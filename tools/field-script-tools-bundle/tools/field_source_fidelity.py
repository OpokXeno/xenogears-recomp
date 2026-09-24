"""Decompiler-only diagnostic traces. The compiler never consumes this module."""

from __future__ import annotations

import hashlib
import re

ORIGIN = re.compile(r"^\s*// ScriptsFile SHA-256: ([0-9a-fA-F]{64})\s*$")
TRACE = re.compile(r"// ([0-9a-fA-F]{4,}): ((?:[0-9a-fA-F]{2})(?: [0-9a-fA-F]{2})*)\s*$")


def add_fidelity_traces(text, scripts, metadata):
    """Show input bytes for inspection; no trace is required for compilation."""
    bytecode = scripts[metadata["bytecode_offset"]:]
    covered = bytearray(len(bytecode))
    lines = [line for line in text.splitlines() if not ORIGIN.fullmatch(line)]
    for line in lines:
        match = TRACE.search(line)
        if match is None:
            continue
        at, raw = int(match[1], 16), bytes.fromhex(match[2])
        if bytecode[at:at + len(raw)] != raw:
            raise ValueError("generated trace disagrees with the input bytecode")
        covered[at:at + len(raw)] = b"\1" * len(raw)
    lines.insert(1, f"// ScriptsFile SHA-256: {hashlib.sha256(scripts).hexdigest()}")
    extra = []
    at = 0
    while at < len(bytecode):
        if covered[at]:
            at += 1
            continue
        end = at + 1
        while end < len(bytecode) and not covered[end] and end - at < 16:
            end += 1
        extra.append(f"// {at:04X}: {bytecode[at:end].hex(' ').upper()}")
        at = end
    if extra:
        lines.extend(("", "// Additional diagnostic bytes:", *extra))
    return "\n".join(lines) + "\n"
