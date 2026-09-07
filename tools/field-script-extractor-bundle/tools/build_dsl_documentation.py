#!/usr/bin/env python3
"""Generate the complete Field event DSL operation catalog."""

from __future__ import annotations

from collections import defaultdict
from pathlib import Path

from decompile_field_scripts import (
    CONDITIONAL_OUTPUT_VARIABLES,
    EXTENDED,
    EXTENDED_CONDITIONAL_BRANCHES,
    IMPLICIT_OUTPUT_VARIABLES,
    OUTPUT_VARIABLES,
    PRIMARY,
    PRIMARY_CONDITIONAL_BRANCHES,
    operation_dsl_name,
    operation_namespace,
)


OUTPUT = Path(__file__).resolve().parents[1] / "docs" / "OPERATION_CATALOG.md"

CATEGORY_ORDER = (
    "actor",
    "movement",
    "world",
    "camera",
    "dialogue",
    "audio",
    "visual",
    "battle",
    "inventory",
    "input",
    "state",
    "flow",
    "event",
)

CATEGORY_DESCRIPTIONS = {
    "actor": "Entities, characters, party members, sprites, and actor control.",
    "movement": "Position, rotation, walkmesh, movement, and collision.",
    "world": "Maps, encounters, triggers, transitions, and scene state.",
    "camera": "Camera position, projection, tracking, and view geometry.",
    "dialogue": "Text, portraits, windows, and dialogue choices.",
    "audio": "Music, sound, channels, volume, and tempo.",
    "visual": "Fades, lighting, models, particles, effects, and video.",
    "battle": "Battle handoffs, Battling results, and return destinations.",
    "inventory": "Inventory, items, currency, and menus.",
    "input": "Button state and accumulated input history.",
    "state": "Script-variable reads, writes, arithmetic, and bit operations.",
    "flow": "Jumps, calls, waits, yields, returns, and termination.",
    "event": "Operations that do not belong exclusively to another subsystem.",
}

SPECIAL_FORMS = {
    "P:00": ("flow", "stop"),
    "P:01": ("flow", "goto label"),
    "P:02": ("flow", "if (!(condition)) goto label"),
    "P:05": ("flow", "call label"),
    "P:06": ("flow", "call label"),
    "P:07": ("flow", "start actor.routine priority async"),
    "P:08": ("flow", "start actor.routine priority wait"),
    "P:09": ("flow", "start actor.routine priority wait_extended"),
    "P:0A": ("world", "if (inside_trigger_2d(...)) call label"),
    "P:0C": ("actor", "actor.process_player_control_if_owned_preserve_ip()"),
    "P:0D": ("flow", "return"),
    "P:13": ("flow", "nop"),
    "P:14": ("world", "world.encounters.enabled = false"),
    "P:15": ("world", "world.encounters.enabled = true"),
    "P:16": ("actor", "actor.bind_playable_character(character: value)"),
    "P:22": ("actor", "actor.self.visible = true"),
    "P:23": ("actor", "actor.self.visible = false"),
    "P:24": ("actor", "actor.visible = true"),
    "P:25": ("actor", "actor.visible = false"),
    "P:26": ("flow", "flow.sleep(duration)"),
    "P:2A": ("actor", "actor.self.dialogue_enabled = false"),
    "P:2B": ("actor", "actor.self.dialogue_enabled = true"),
    "P:31": ("input", "if ((input.held & mask) == 0) goto label"),
    "P:32": ("input", "if ((input.accumulated & mask) == 0) goto label"),
    "P:33": ("input", "input.accumulated = 0"),
    "P:36": ("state", "state = true"),
    "P:37": ("state", "state = false"),
    "P:3C": ("state", "state++"),
    "P:3D": ("state", "state--"),
    "P:41": ("state", "state <<= value"),
    "P:42": ("state", "state >>= value"),
    "P:5B": ("flow", "stall_forever"),
    "P:5C": ("actor", "actor.bind_party_slot(slot: value)"),
    "P:A6": ("flow", "flow.dispatch_triplet_table(index: value)"),
    "P:A7": ("actor", "actor.process_player_control_if_owned()"),
    "P:C9": ("world", "if (!inside_trigger_2d(...)) goto label"),
    "P:CB": ("world", "if (!inside_trigger_3d(...)) goto label"),
    "P:CC": ("world", "if (inside_trigger_3d(...)) call label"),
    "P:DC": ("state", "state.swap(left, right)"),
    "P:D1": ("flow", "stall_forever"),
    "P:E4": ("flow", "stall_forever"),
    "P:FD": ("flow", "nop"),
    "P:FF": ("flow", "nop"),
    "E:0A": ("state", "state |= (1 << bit)"),
    "E:0B": ("state", "state &= ~(1 << bit)"),
    "E:0D": ("dialogue", "dialogue.set_portrait(character: value)"),
}

STATE_OPERATORS = {
    0x35: "=",
    0x38: "+=",
    0x39: "-=",
    0x3A: "|= 1 <<",
    0x3B: "&= ~(1 << ...)",
    0x3E: "&=",
    0x3F: "|=",
    0x40: "^=",
    0xDE: "*=",
    0xDF: "/=",
}


def operation_form(opcode: int, subopcode: int | None, record: dict) -> tuple[str, str]:
    table = "E" if subopcode is not None else "P"
    value = subopcode if subopcode is not None else opcode
    special = SPECIAL_FORMS.get(f"{table}:{value:02X}")
    if special is not None:
        return special
    if subopcode is None and opcode in STATE_OPERATORS:
        return "state", f"state {STATE_OPERATORS[opcode]} value"
    dsl_name = operation_dsl_name(record["name"])
    key = (opcode, subopcode)
    if key in OUTPUT_VARIABLES or key in IMPLICIT_OUTPUT_VARIABLES:
        return operation_namespace(record["name"]), f"{dsl_name}(...) -> state"
    if key in CONDITIONAL_OUTPUT_VARIABLES:
        return operation_namespace(record["name"]), f"{dsl_name}(...) -> state in read mode"
    if subopcode is not None and subopcode in EXTENDED_CONDITIONAL_BRANCHES:
        return operation_namespace(record["name"]), f"{dsl_name}_or_goto(label)"
    if subopcode is None and opcode in PRIMARY_CONDITIONAL_BRANCHES:
        return operation_namespace(record["name"]), f"{dsl_name}_or_goto(label)"
    return operation_namespace(record["name"]), f"{dsl_name}(...)"


def _escape(value: str) -> str:
    return value.replace("|", "\\|")


def render_catalog() -> str:
    grouped = defaultdict(list)
    for opcode, record in sorted(PRIMARY.items()):
        namespace, form = operation_form(opcode, None, record)
        grouped[namespace].append((f"{opcode:02X}", record, form))
    for subopcode, record in sorted(EXTENDED.items()):
        namespace, form = operation_form(0xFE, subopcode, record)
        grouped[namespace].append((f"FE {subopcode:02X}", record, form))

    lines = [
        "# Complete Operation Catalog",
        "",
        "This file is generated from `tools/field_opcode_table.json`. It lists all",
        "256 primary opcodes and 227 extended opcodes known to the dispatcher.",
        "The DSL column shows the usual rendering. Instructions with special syntax",
        "may appear as assignments, conditions, or control-flow statements.",
        "",
        "Opcodes and handler addresses remain hexadecimal because they are technical",
        "identities. Arguments emitted in `script.xgs` are displayed in decimal.",
        "",
        "## Summary By Object",
        "",
        "| Object | Operations | Responsibility |",
        "|---|---:|---|",
    ]
    for category in CATEGORY_ORDER:
        records = grouped.get(category, [])
        if records:
            lines.append(
                f"| [`{category}`](#{category}) | {len(records)} | {CATEGORY_DESCRIPTIONS[category]} |"
            )

    for category in CATEGORY_ORDER:
        records = grouped.get(category, [])
        if not records:
            continue
        lines.extend(
            (
                "",
                f"## `{category}`",
                "",
                CATEGORY_DESCRIPTIONS[category],
                "",
                "| Opcode | Bytes | DSL form | Handler | Original function | Behavior |",
                "|---|---:|---|---|---|---|",
            )
        )
        for opcode, record, form in records:
            lines.append(
                f"| `{opcode}` | {_escape(record['bytes'])} | `{_escape(form)}` | "
                f"`{record['handler']}` | `{record['name']}` | {_escape(record['behavior'])} |"
            )

    lines.extend(
        (
            "",
            "## Regeneration",
            "",
            "```bash",
            "python3 tools/build_dsl_documentation.py",
            "```",
            "",
            "When the Field source documentation changes, regenerate the opcode table first:",
            "",
            "```bash",
            "python3 tools/build_opcode_table.py",
            "python3 tools/build_dsl_documentation.py",
            "```",
            "",
        )
    )
    return "\n".join(lines)


def main() -> int:
    OUTPUT.write_text(render_catalog(), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
