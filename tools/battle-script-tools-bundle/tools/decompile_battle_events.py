"""Bytecode analysis and editable XGS rendering for Battle Event scripts."""

from __future__ import annotations

import json
import struct
from dataclasses import dataclass, field

from battle_event_codec import (
    COMPARISON_FUNCTIONS, COMPARISONS, FIELD_SIZES, SPECS, Instruction, decode, successors,
)
from extract_disc_battle_scripts import (
    ENTRIES_PER_ENTITY, SCRIPT_PREFIX_SIZE, SCRIPT_ROWS_OFFSET, SCRIPT_ROW_SIZE,
    parse_dialog_resource, parse_script_resource,
)

VARIABLE_BANK_END = 0x400
ENGINE_VARIABLES = {
    0x0000: "battle_exit_signal",
    0x0020: "party0_exit_status",
    0x0022: "party1_exit_status",
    0x0024: "party2_exit_status",
}
ENTRY_NAMES = {0: "start", 1: "idle"}
ROLE_BY_OPCODE = {0x07: "flag", 0x08: "flag", 0x0D: "counter", 0x0E: "counter", 0x14: "random", 0x15: "random"}


@dataclass
class Block:
    start: int
    end: int
    kind: str                     # "code" or "data"
    instructions: list[Instruction] = field(default_factory=list)
    owners: set[int] = field(default_factory=set)
    orphan: bool = False

    @property
    def label(self) -> str:
        return f"{'L' if self.kind == 'code' else 'D'}_{self.start:04X}"


@dataclass
class Analysis:
    prefix: bytes
    rows: list[list[int]]
    bytecode: bytes
    instructions: dict[int, Instruction]
    blocks: list[Block]
    aliases: dict[int, tuple[int, int]]      # target -> (instruction pc, delta)
    diagnostics: list[str]
    variables: dict[int, str]               # offset -> qualified name
    variable_groups: dict[str, list[tuple[str, int]]]
    used_entries: list[set[int]]


def split_script(script: bytes) -> tuple[bytes, list[list[int]], bytes]:
    metadata = parse_script_resource(script)
    rows = [list(struct.unpack_from("<8H", script, SCRIPT_ROWS_OFFSET + entity * SCRIPT_ROW_SIZE))
            for entity in range(metadata["entity_count"])]
    return script[:SCRIPT_PREFIX_SIZE], rows, script[metadata["bytecode_offset"]:]


def _follow(bytecode: bytes, starts: list[int], decoded: dict[int, Instruction]) -> tuple[set[int], list[int]]:
    """Return instruction PCs reachable from starts and targets that fail to decode."""
    reached: set[int] = set()
    failed = []
    work = [start for start in starts if start < len(bytecode)]
    while work:
        pc = work.pop()
        while pc not in reached:
            instruction = decoded.get(pc) or decode(bytecode, pc)
            if instruction is None:
                failed.append(pc)
                break
            decoded[pc] = instruction
            reached.add(pc)
            following = successors(instruction)
            if not following:
                break
            for target in following[1:]:
                work.append(target)
            pc = following[0]
            if pc >= len(bytecode):
                failed.append(pc)
                break
    return reached, failed


def analyze(script: bytes) -> Analysis:
    prefix, rows, bytecode = split_script(script)
    diagnostics: list[str] = []
    decoded: dict[int, Instruction] = {}
    entries = sorted({offset for row in rows for offset in row})
    reached, failed = _follow(bytecode, entries, decoded)
    for pc in sorted(set(failed)):
        diagnostics.append(f"control flow reaches 0x{pc:04X}, which is not a valid instruction")

    # Resolve overlapping decodes: keep instructions in address order and turn
    # targets that enter an earlier instruction into byte-relative aliases.
    instructions: dict[int, Instruction] = {}
    aliases: dict[int, tuple[int, int]] = {}
    cursor_end = -1
    owner_pc = -1
    for pc in sorted(reached):
        if pc < cursor_end:
            aliases[pc] = (owner_pc, pc - owner_pc)
            diagnostics.append(f"target 0x{pc:04X} enters instruction at 0x{owner_pc:04X}")
            continue
        instructions[pc] = decoded[pc]
        owner_pc, cursor_end = pc, pc + decoded[pc].size

    # Unreached gaps: promote complete linear decodes that end in a terminal
    # instruction and contain work beyond stop/no-op; everything else stays
    # byte-exact data (LZSS group padding commonly follows the last routine).
    covered = bytearray(len(bytecode))
    for pc, instruction in instructions.items():
        covered[pc:pc + instruction.size] = b"\1" * instruction.size
    gaps = []
    pc = 0
    while pc < len(bytecode):
        if covered[pc]:
            pc += 1
            continue
        start = pc
        while pc < len(bytecode) and not covered[pc]:
            pc += 1
        gaps.append((start, pc))
    orphan_starts = set()
    data_regions = []
    for start, end in gaps:
        linear = []
        cursor = start
        while cursor < end:
            instruction = decode(bytecode, cursor)
            if instruction is None or cursor + instruction.size > end:
                linear = None
                break
            linear.append(instruction)
            cursor += instruction.size
        if linear and linear[-1].spec.terminal and any(item.opcode not in {0x00, 0x32, 0x34} for item in linear):
            for instruction in linear:
                instructions[instruction.pc] = instruction
            orphan_starts.add(start)
        else:
            data_regions.append((start, end))

    targets = set(entries) | orphan_starts
    for instruction in instructions.values():
        targets.update(target for target in instruction.labels() if target not in aliases)
    for owner, _ in aliases.values():
        targets.add(owner)

    blocks: list[Block] = []
    data_starts = {start: end for start, end in data_regions}
    pc = 0
    current: Block | None = None
    while pc < len(bytecode):
        if pc in data_starts:
            current = None
            blocks.append(Block(pc, data_starts[pc], "data"))
            pc = data_starts[pc]
            continue
        instruction = instructions[pc]
        if current is None or pc in targets:
            current = Block(pc, pc, "code", orphan=pc in orphan_starts)
            blocks.append(current)
        current.instructions.append(instruction)
        current.end = pc + instruction.size
        pc = current.end
        if instruction.spec.terminal:
            current = None

    block_of = {}
    for block in blocks:
        for instruction in block.instructions:
            block_of[instruction.pc] = block
    used = used_entries(rows, instructions)
    for entity, row in enumerate(rows):
        reach, _ = _follow(bytecode, [row[index] for index in sorted(used[entity])], dict(decoded))
        for pc in reach:
            if pc in block_of:
                block_of[pc].owners.add(entity)

    variables, groups = _name_variables(instructions)
    return Analysis(prefix, rows, bytecode, instructions, blocks, aliases, diagnostics, variables, groups, used)


def used_entries(rows: list[list[int]], instructions: dict[int, Instruction]) -> list[set[int]]:
    """Entries the VM can select: start/idle plus those named by start opcodes."""
    used = [{0, 1} for _ in rows]
    for instruction in instructions.values():
        if instruction.opcode in {0x03, 0x04, 0x05}:
            entity, entry = instruction.values[0], instruction.values[1] & 0x1F
            if entity < len(rows) and entry < ENTRIES_PER_ENTITY:
                used[entity].add(entry)
    return used


def variable_references(instruction: Instruction) -> list[tuple[int, int]]:
    """Return (variable offset, operand index) for every variable operand."""
    spec = instruction.spec
    mask = next((value for operand, value in zip(spec.operands, instruction.values) if operand.kind == "mask"), 0)
    found = []
    for index, (operand, value) in enumerate(zip(spec.operands, instruction.values)):
        if operand.kind in {"dest", "var"} or (operand.kind == "typed" and not mask & operand.bit) \
                or (operand.kind == "v15" and not value & 0x8000):
            found.append((value, index))
    return found


def _name_variables(instructions: dict[int, Instruction]) -> tuple[dict[int, str], dict[str, list[tuple[str, int]]]]:
    """Name variables by what writes them, else by the operand they feed."""
    writes: dict[int, set[str]] = {}
    reads: dict[int, set[str]] = {}
    for instruction in instructions.values():
        operands = instruction.spec.operands
        for offset, index in variable_references(instruction):
            if offset & 1 or offset >= VARIABLE_BANK_END:
                continue
            writes.setdefault(offset, set())
            reads.setdefault(offset, set())
            operand = operands[index]
            if operand.kind == "dest" or (instruction.opcode in {0x16, 0x17} and index == 0):
                role = ROLE_BY_OPCODE.get(instruction.opcode)
                if role:
                    writes[offset].add(role)
            elif operand.kind == "v15":
                reads[offset].add(operand.name)
    names: dict[int, str] = {}
    groups: dict[str, list[tuple[str, int]]] = {"engine": [], "script": []}
    for offset in sorted(writes):
        if offset in ENGINE_VARIABLES:
            name = ENGINE_VARIABLES[offset]
            groups["engine"].append((name, offset))
            names[offset] = f"engine.{name}"
            continue
        if len(writes[offset]) == 1:
            role = next(iter(writes[offset]))
        elif not writes[offset] and len(reads[offset]) == 1:
            role = next(iter(reads[offset]))
        else:
            role = "var"
        name = f"{role}_{offset:04X}"
        groups["script"].append((name, offset))
        names[offset] = f"script.{name}"
    return names, groups


def _number(value: int, hexadecimal: bool = False) -> str:
    return f"0x{value:02X}" if hexadecimal else str(value)


def render_variable(analysis_variables: dict[int, str], offset: int) -> str:
    return analysis_variables.get(offset) or f"slot(0x{offset:04X})"


class Renderer:
    def __init__(self, analysis: Analysis, label_names: dict[int, str]):
        self.analysis = analysis
        self.label_names = label_names

    def var(self, offset: int) -> str:
        return render_variable(self.analysis.variables, offset)

    def label(self, target: int) -> str:
        if target in self.label_names:
            return self.label_names[target]
        if target in self.analysis.aliases:
            return f"L_{target:04X}"
        return f"0x{target:04X}"

    def operand(self, instruction: Instruction, index: int, mask: int) -> str:
        operand = instruction.spec.operands[index]
        value = instruction.values[index]
        if operand.kind == "v15":
            return _number(value & 0x7FFF, operand.hex) if value & 0x8000 else self.var(value)
        if operand.kind == "typed":
            if mask & operand.bit:
                return str(value - 0x10000 if value >= 0x8000 else value)
            return self.var(value)
        if operand.kind in {"dest", "var"}:
            return self.var(value)
        if operand.kind == "label":
            return self.label(value)
        if operand.kind == "s8":
            return str(value - 256 if value >= 128 else value)
        return _number(value, operand.hex)

    def statement(self, instruction: Instruction) -> str:
        spec = instruction.spec
        values = instruction.values
        mask_index = next((i for i, operand in enumerate(spec.operands) if operand.kind == "mask"), None)
        mask = values[mask_index] if mask_index is not None else 0
        derivable = sum(operand.bit for operand in spec.operands if operand.kind == "typed")
        selector_bits = 0x0F if spec.opcode == 0x02 else 0
        reserved = mask & ~derivable & ~selector_bits & 0xFF
        op = spec.opcode
        arg = lambda index: self.operand(instruction, index, mask)  # noqa: E731

        if op == 0x00:
            return "stop;"
        if op == 0x01:
            return f"goto {arg(0)};"
        if op == 0x02:
            selector = mask & 0x0F
            left, right = arg(0), arg(1)
            if reserved:
                condition = f"compare({left}, {right}, {selector}, reserved_flags=0x{reserved:02X})"
            elif selector in COMPARISONS:
                condition = f"{left} {COMPARISONS[selector]} {right}"
            elif selector in COMPARISON_FUNCTIONS:
                condition = f"{COMPARISON_FUNCTIONS[selector]}({left}, {right})"
            else:
                condition = f"compare({left}, {right}, {selector})"
            return f"if (!({condition})) goto {arg(3)};"
        if op in {0x03, 0x04, 0x05}:
            packed = values[1]
            return f"{spec.qualified}({values[0]}, {packed & 0x1F}, {packed >> 5});"
        if not reserved:
            sugar = {0x06: "=", 0x09: "+=", 0x0A: "-=", 0x0B: "|=", 0x0F: "&=", 0x11: "^="}
            if op in sugar:
                return f"{arg(0)} {sugar[op]} {arg(1)};"
            if op == 0x0C:
                return f"{arg(0)} &= ~{arg(1)};"
            if op in {0x16, 0x17} and not mask & 0x80:
                return f"{arg(0)} {'*=' if op == 0x16 else '/='} {arg(1)};"
        if op == 0x07:
            return f"{arg(0)} = true;"
        if op == 0x08:
            return f"{arg(0)} = false;"
        if op == 0x0D:
            return f"{arg(0)}++;"
        if op == 0x0E:
            return f"{arg(0)}--;"
        if op in {0x12, 0x13}:
            return f"{arg(0)} {'<<=' if op == 0x12 else '>>='} {arg(1)};"
        if op == 0x14:
            return f"{arg(0)} = random();"
        if op == 0x15:
            return f"{arg(1)} = random({values[0]});"
        args = []
        for index, operand in enumerate(spec.operands):
            if operand.kind == "mask":
                continue
            if operand.name == "reserved":
                if values[index]:
                    args.append(f"reserved={values[index]}")
                continue
            args.append(arg(index))
        if reserved:
            args.append(f"reserved_flags=0x{reserved:02X}")
        return f"{spec.qualified}({', '.join(args)});"


def _entry_name(index: int) -> str:
    return ENTRY_NAMES.get(index, f"entry[{index}]")


def _entry_runs(row: list[int]) -> list[tuple[int, int, int]]:
    runs = []
    index = 0
    while index < len(row):
        end = index
        if index >= 2:
            while end + 1 < len(row) and row[end + 1] == row[index]:
                end += 1
        runs.append((index, end, row[index]))
        index = end + 1
    return runs


def _slot_text(start: int, end: int) -> str:
    if start == end:
        return _entry_name(start)
    return f"entry[{start}..{end}]"


def render_source(event_index: int, script: bytes, dialog: bytes | None = None) -> tuple[str, dict]:
    analysis = analyze(script)
    bytecode = analysis.bytecode
    messages = {}
    if dialog is not None:
        try:
            messages = {item["id"]: item["text"] for item in parse_dialog_resource(dialog)["messages"]}
        except ValueError:
            messages = {}
    label_names = {block.start: block.label for block in analysis.blocks}

    entry_comments: dict[int, list[str]] = {}
    for entity, row in enumerate(analysis.rows):
        slots: dict[int, list[str]] = {}
        for start, end, offset in _entry_runs(row):
            names = [_slot_text(index, index) for index in range(start, end + 1) if index in analysis.used_entries[entity]]
            if names:
                slots.setdefault(offset, []).extend(names)
        for offset, names in slots.items():
            entry_comments.setdefault(offset, []).append(f"entity {entity}: {', '.join(names)}")
    renderer = Renderer(analysis, label_names)

    def render_block(block: Block, indent: str, following: Block | None) -> list[str]:
        lines = [f"{indent}{block.label}:"]
        comments = entry_comments.get(block.start)
        if comments:
            lines.append(f"{indent}// event: {', '.join(comments)}")
        if block.orphan:
            lines.append(f"{indent}// orphan: no stored entry or jump reaches this code")
        for instruction in block.instructions:
            raw = bytecode[instruction.pc:instruction.pc + instruction.size]
            trace = f"// {instruction.pc:04X}: {raw.hex(' ').upper()}"
            if instruction.opcode in {0x18, 0x19}:
                text = messages.get(instruction.operand("message_id"))
                if text is not None:
                    trace += f" | {json.dumps(text)}"
            lines.append(f"{indent}{renderer.statement(instruction)} {trace}")
        last = block.instructions[-1]
        if not last.spec.terminal:
            continuation = last.pc + last.size
            target = label_names.get(continuation, f"0x{continuation:04X}")
            lines.append(f"{indent}fallthrough {target};")
        return lines

    by_owner: dict[int | None, list[Block]] = {}
    for block in analysis.blocks:
        if block.kind != "code":
            continue
        owner = next(iter(block.owners)) if len(block.owners) == 1 else None
        by_owner.setdefault(owner, []).append(block)

    output = ["// Xenogears editable Battle Event script", f"battle_event {event_index} {{"]
    if analysis.prefix != bytes(SCRIPT_PREFIX_SIZE):
        output.append(f'  prefix "{analysis.prefix.hex(" ").upper()}";')
    if any(analysis.variable_groups.values()):
        output.append("  state {")
        for group, members in analysis.variable_groups.items():
            if not members:
                continue
            output.append(f"    {group} {{")
            output.extend(f"      signed {name} at 0x{offset:04X};" for name, offset in members)
            output.append("    }")
        output.append("  }")
        output.append("")
    for target, (owner, delta) in sorted(analysis.aliases.items()):
        output.append(f"  alias L_{target:04X} = {label_names[owner]} + {delta};")
    if analysis.aliases:
        output.append("")
    output.append("  entities {")
    for entity, row in enumerate(analysis.rows):
        output.append(f"    entity {entity} {{")
        output.append("      events {")
        runs = _entry_runs(row)
        width = max(len(_slot_text(start, end)) for start, end, _ in runs)
        for start, end, offset in runs:
            target = label_names.get(offset, f"L_{offset:04X}" if offset in analysis.aliases else f"0x{offset:04X}")
            output.append(f"        {_slot_text(start, end).ljust(width)} -> {target};")
        output.append("      }")
        owned = by_owner.get(entity, [])
        if owned:
            output.append("      code {")
            for position, block in enumerate(owned):
                if position:
                    output.append("")
                output.extend(render_block(block, "        ", None))
            output.append("      }")
        output.append("    }")
    output.append("  }")
    shared = by_owner.get(None, [])
    if shared:
        output.append("")
        output.append("  shared_code {")
        for position, block in enumerate(shared):
            if position:
                output.append("")
            output.extend(render_block(block, "    ", None))
        output.append("  }")
    data_blocks = [block for block in analysis.blocks if block.kind == "data"]
    if data_blocks:
        output.append("")
        output.append("  data {")
        for block in data_blocks:
            output.append(f"    {block.label} {{")
            payload = bytecode[block.start:block.end]
            for offset in range(0, len(payload), 16):
                output.append(f'      bytes "{payload[offset:offset + 16].hex(" ").upper()}";')
            output.append("    }")
        output.append("  }")
    if analysis.diagnostics:
        output.append("")
        output.extend(f"  // Diagnostic: {message}" for message in analysis.diagnostics)
    output.append("}")

    instruction_bytes = sum(item.size for block in analysis.blocks for item in block.instructions)
    data_bytes = sum(block.end - block.start for block in data_blocks)
    size = len(bytecode)
    report = {
        "entity_count": len(analysis.rows),
        "bytecode_size": size,
        "instruction_count": sum(len(block.instructions) for block in analysis.blocks),
        "instruction_bytes": instruction_bytes,
        "data_bytes": data_bytes,
        "classified_bytes": instruction_bytes + data_bytes,
        "orphan_blocks": sum(1 for block in analysis.blocks if block.orphan),
        "instruction_coverage_percent": round(100.0 * instruction_bytes / size, 3) if size else 100.0,
        "total_coverage_percent": round(100.0 * (instruction_bytes + data_bytes) / size, 3) if size else 100.0,
        "variables": sum(len(members) for members in analysis.variable_groups.values()),
        "diagnostics": analysis.diagnostics,
    }
    return "\n".join(output) + "\n", report
