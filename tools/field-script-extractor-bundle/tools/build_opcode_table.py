#!/usr/bin/env python3
"""Build the self-contained Field opcode table from the Field documentation."""

from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
OUTPUT = Path(__file__).with_name("field_opcode_table.json")
ROW = re.compile(
    r"^\| `(?P<opcode>(?:FE )?[0-9A-F]{2})` "
    r"\| (?P<size>[^|]+?) "
    r"\| `(?P<handler>0x[0-9A-F]+)` "
    r"\| `(?P<name>[^`]+)` "
    r"\| (?P<behavior>.+) \|$"
)


def parse_table(path: Path, *, extended: bool) -> dict[str, dict]:
    records = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = ROW.match(line)
        if match is None:
            continue
        opcode = match.group("opcode")
        if extended != opcode.startswith("FE "):
            continue
        key = opcode.removeprefix("FE ")
        records[key] = {
            "bytes": match.group("size").strip(),
            "handler": match.group("handler"),
            "name": match.group("name"),
            "behavior": match.group("behavior"),
        }
    return records


def main() -> int:
    docs = ROOT / "docs" / "xenogears" / "field"
    primary = parse_table(docs / "05-primary-opcodes.md", extended=False)
    extended = parse_table(docs / "06-extended-opcodes.md", extended=True)
    if len(primary) != 256 or len(extended) != 227:
        raise ValueError(
            f"expected 256 primary and 227 extended entries, got {len(primary)} and {len(extended)}"
        )
    document = {
        "schema": "xenogears-field-opcodes/v1",
        "source": [
            "docs/xenogears/field/05-primary-opcodes.md",
            "docs/xenogears/field/06-extended-opcodes.md",
        ],
        "primary": primary,
        "extended": extended,
    }
    OUTPUT.write_text(
        json.dumps(document, indent=2, ensure_ascii=True) + "\n",
        encoding="ascii",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
