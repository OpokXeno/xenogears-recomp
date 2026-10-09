#!/usr/bin/env python3
"""Build the self-contained Battle Event opcode table from the Battle documentation."""

from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
OUTPUT = Path(__file__).with_name("battle_event_opcode_table.json")
SOURCE = "docs/xenogears/battle/07-battle-event-vm.md"
ROW = re.compile(
    r"^\| `(?P<opcode>[0-9A-F]{2})` "
    r"\| (?P<size>[^|]+?) "
    r"\| `(?P<handler>0x[0-9A-F]+)` "
    r"\| `(?P<function>[^`]+)` "
    r"\| (?P<behavior>.+) \|$"
)


def main() -> int:
    records = {}
    for line in (ROOT / SOURCE).read_text(encoding="utf-8").splitlines():
        match = ROW.match(line)
        if match is None:
            continue
        records[match.group("opcode")] = {
            "bytes": match.group("size").strip(),
            "handler": match.group("handler"),
            "function": match.group("function"),
            "behavior": match.group("behavior").replace("\\|", "|"),
        }
    if sorted(records) != [f"{opcode:02X}" for opcode in range(0x4C)]:
        raise ValueError(f"expected dispatch entries 00..4B, got {len(records)}")
    document = {
        "schema": "xenogears-battle-event-opcodes/v1",
        "source": [SOURCE],
        "dispatch_range": "0x00..0x4B",
        "opcodes": records,
    }
    OUTPUT.write_text(json.dumps(document, indent=2, ensure_ascii=True) + "\n", encoding="ascii")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
