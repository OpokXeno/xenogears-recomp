"""Field assembly, editable event DSL compilation and linkable source records."""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass, field

from decompile_field_scripts import (
    VariableSymbol, address_operand_offsets, analyze_bytecode, decode_instruction,
)
from field_instruction_codec import checked, encode_statement, instruction_dependencies, integer


IDENT = r"[A-Za-z_][A-Za-z_0-9]*"
NUMBER = r"(?:0x[0-9A-Fa-f]+|[0-9]+)"
HEX = r"[0-9A-Fa-f\s]*"


@dataclass
class Record:
    pc: int | None
    raw: bytes
    kind: str = "op"
    references: dict[int, str] = field(default_factory=dict)
    line: int = 0
    block: str | None = None
    labels: list[str] = field(default_factory=list)
    original: bytes | None = None
    alignment: int = 1
    residue: int | str = 0
    alternate: str | None = None
    jump_target: str | None = None
    source_block: str | None = None
    relative: dict[int, str] = field(default_factory=dict)
    semantic: str | None = None
    terminal: bool = False
    guard: str | None = None
    constraints: tuple = ()
    site: int | None = None


@dataclass
class Assembly:
    bitmap: bytes
    size: int
    rows: dict[int, list[str]] = field(default_factory=dict)
    labels: dict[str, int | None] = field(default_factory=dict)
    records: list[Record] = field(default_factory=list)
    blocks: dict[str, tuple[int, int]] = field(default_factory=dict)
    tables: dict[int, list[int]] = field(default_factory=dict)
    link_report: dict = field(default_factory=dict)
    aliases: dict[str, tuple[str, int]] = field(default_factory=dict)
    alias_fallbacks: dict[str, bytes] = field(default_factory=dict)
    script_images: set[str] = field(default_factory=set)
    absolute_symbols: dict[str, int] = field(default_factory=dict)

    def label(self, name: str, pc: int | None) -> None:
        if name in self.labels:
            raise ValueError(f"duplicate label: {name}")
        self.labels[name] = pc

    def link(self, layout: str = "relocate") -> Assembly:
        from field_linker import link_assembly

        return link_assembly(self, layout)

    def encode_bytecode(self) -> bytes:
        """Serialize current records and relocations; also used during relaxation."""
        bytecode = bytearray(self.size)
        covered = bytearray(self.size)
        for record in self.records:
            prefix = f"line {record.line}: " if record.line else ""
            if record.pc is None:
                raise ValueError(f"{prefix}new records require relocating layout")
            if not record.raw or not 0 <= record.pc < record.pc + len(record.raw) <= self.size:
                raise ValueError(f"{prefix}record at 0x{record.pc:X} leaves exact layout")
            end = record.pc + len(record.raw)
            if any(covered[record.pc:end]):
                raise ValueError(f"{prefix}overlapping record at 0x{record.pc:X}; exact layout cannot grow over adjacent bytes")
            raw = bytearray(record.raw)
            reference_bytes = {}
            for offset, name in record.references.items():
                if name not in self.labels:
                    raise ValueError(f"{prefix}unresolved label: {name}")
                if not 0 <= offset <= len(raw) - 2:
                    raise ValueError(f"{prefix}reference leaves its record")
                encoded = struct.pack("<H", checked(self.labels[name], 0, 65535, "target offset"))
                for position, value in enumerate(encoded, offset):
                    if position in reference_bytes and reference_bytes[position] != value:
                        raise ValueError(f"{prefix}conflicting overlapping reference fields")
                    reference_bytes[position] = value
                    raw[position] = value
            if record.guard is not None:
                if record.guard not in self.labels or len(raw) != 8 or raw[0] != 2 or raw[2] != 1 or raw[5] != 0xC0:
                    raise ValueError("invalid party continuation gate")
                if (0x100 | raw[1]) == self.labels[record.guard]:
                    raw[1] ^= 1  # Keep original bytes unless relocation creates equality.
            bytecode[record.pc:end] = raw
            covered[record.pc:end] = b"\1" * len(raw)
        if 0 in covered:
            raise ValueError(f"uncovered byte at 0x{covered.index(0):04X}; preserve data and padding explicitly")
        return bytes(bytecode)

    def build(self, layout: str = "relocate") -> bytes:
        from extract_disc_field_scripts import MAX_SCRIPT_SECTION_SIZE, parse_scripts_file

        if layout != "exact":
            return self.link(layout).build("exact")
        if any(record.kind == "semantic" for record in self.records):
            raise ValueError("semantic operations must be lowered before exact assembly")
        if len(self.bitmap) != 0x80:
            raise ValueError("variable bitmap must contain exactly 128 bytes")
        checked(self.size, 1, MAX_SCRIPT_SECTION_SIZE - 0x84, "bytecode size")
        if sorted(self.rows) != list(range(len(self.rows))):
            raise ValueError("entity IDs must be contiguous from zero")
        if 0x84 + 0x40 * len(self.rows) + self.size > MAX_SCRIPT_SECTION_SIZE:
            raise ValueError("ScriptsFile exceeds the loader allocation limit")
        for name, pc in self.labels.items():
            if pc is None:
                raise ValueError(f"new label {name} requires relocating layout")
            checked(pc, 0, self.size - 1, f"label {name}")
        bytecode = self.encode_bytecode()
        for record in self.records:
            if record.kind != "op":
                continue
            for displacement, name in record.relative.items():
                if name not in self.labels or self.labels[name] != record.pc + displacement:
                    raise ValueError(f"line {record.line}: script-byte dependency {displacement:+d} -> {name} moved; "
                                     "keep the dependent instructions contiguous")
            try:
                instruction = decode_instruction(bytecode, record.pc)
            except (ValueError, struct.error) as error:
                raise ValueError(f"line {record.line}: invalid instruction at 0x{record.pc:X}: {error}") from error
            if instruction.size != len(record.raw):
                raise ValueError(f"line {record.line}: instruction size disagrees with opcode/mode at 0x{record.pc:X}")
            if any(not 0 <= record.pc + offset < self.size for offset in instruction_dependencies(instruction)):
                raise ValueError(f"line {record.line}: shared-byte operation reads outside the script")
            for target in instruction.targets:
                checked(target, 0, self.size - 1, "branch target")
        result = bytearray(self.bitmap + struct.pack("<I", len(self.rows)))
        for entity, row in sorted(self.rows.items()):
            if len(row) != 32 or any(name not in self.labels for name in row):
                raise ValueError(f"entity {entity}: all 32 routines must resolve to declared labels")
            offsets = [checked(self.labels[name], 0, 65535, "routine offset") for name in row]
            result.extend(struct.pack("<32H", *offsets))
        result.extend(bytecode)
        parse_scripts_file(result)
        return bytes(result)

    def text(self) -> str:
        if any(record.kind == "semantic" for record in self.records):
            raise ValueError("link semantic operations before exporting XGA")
        lines = [".xga 1", f'.bitmap "{self.bitmap.hex(" ").upper()}"', f".size {self.size}"]
        for name, pc in sorted(self.labels.items(), key=lambda pair: (pair[1] if pair[1] is not None else self.size, pair[0])):
            if pc is not None:
                lines.append(f".label {name} = 0x{pc:04X}")
        for entity, row in sorted(self.rows.items()):
            lines.append(f".row {entity} = {', '.join(row)}")
        for name, (base, offset) in self.aliases.items():
            lines.append(f".alias {name} = {base} + {offset}")
        for dispatch, entries in sorted(self.tables.items()):
            lines.append(f".triplets 0x{dispatch:04X} = " + ", ".join(f"0x{pc:04X}" for pc in entries))
        current_block = None
        for record in self.records:
            if record.block != current_block:
                if current_block is not None:
                    lines.append(".end")
                current_block = record.block
                if current_block is not None:
                    start, end = self.blocks[current_block]
                    lines.append(f".block {current_block} 0x{start:04X} 0x{end:04X}")
            for label in record.labels:
                lines.append(f".bind {label}")
            references = "".join(f" @{offset}={name}" for offset, name in sorted(record.references.items()))
            if record.alternate is not None:
                references += f" !alternate={record.alternate}"
            if record.jump_target is not None:
                references += f" !jump={record.jump_target}"
            for displacement, name in sorted(record.relative.items()):
                references += f" !relative={displacement}:{name}"
            if record.guard is not None:
                references += f" !guard={record.guard}"
            if record.alignment != 1:
                residue = str(record.residue) if isinstance(record.residue, int) else f"high({record.residue})"
                references += f" !align={record.alignment}:{residue}"
            pc = "auto" if record.pc is None else f"0x{record.pc:04X}"
            lines.append(f'.at {pc} {record.kind} "{record.raw.hex(" ").upper()}"{references}')
        if current_block is not None:
            lines.append(".end")
        return "\n".join(lines) + "\n"


def disassemble(scripts: bytes) -> Assembly:
    from extract_disc_field_scripts import parse_scripts_file

    metadata = parse_scripts_file(scripts)
    bytecode = scripts[metadata["bytecode_offset"]:]
    analysis = analyze_bytecode(bytecode, metadata)
    assembly = Assembly(scripts[:0x80], len(bytecode))
    # Keep the independent lossless XGA fully navigable at instruction starts.
    assembly.labels = {f"L_{pc:04X}": pc for pc in analysis["labels"] | analysis["instructions"].keys()}
    assembly.rows = {row["entity_id"]: [f"L_{int(value, 16):04X}" for value in row["routine_offsets"]] for row in metadata["routine_rows"]}
    assembly.tables = {pc: list(entries) for pc, entries in analysis["computed_successors"].items() if entries}
    block = None
    end = -1
    for pc, instruction in sorted(analysis["instructions"].items()):
        if pc != end:
            block = f"code_{pc:04X}"
            assembly.blocks[block] = (pc, pc)
        end = pc + instruction.size
        assembly.blocks[block] = (assembly.blocks[block][0], end)
        references = {offset: f"L_{struct.unpack_from('<H', instruction.raw, offset)[0]:04X}" for offset in address_operand_offsets(instruction)}
        assembly.records.append(Record(pc, instruction.raw, references=references, block=block, original=instruction.raw))
        for displacement in instruction_dependencies(instruction):
            address = pc + displacement
            if not 0 <= address < len(bytecode):
                raise ValueError(f"{instruction.name} at 0x{pc:04X} reads outside the script")
            name = f"L_{address:04X}"
            assembly.labels[name] = address
            assembly.records[-1].relative[displacement] = name
        if pc in analysis["alignments"]:
            assembly.records[-1].alignment, assembly.records[-1].residue = analysis["alignments"][pc]
    for start, end in analysis["data_ranges"]:
        block = f"data_{start:04X}"
        assembly.blocks[block] = (start, end)
        for pc in range(start, end, 16):
            references = {offset: f"L_{target:04X}" for offset, target in analysis["landing_data"].get(pc, {}).items()}
            guard = None
            if pc - 3 in analysis.get("semantic_branches", {}):
                staged, present = analysis["semantic_branches"][pc - 3]
                references = {3: f"L_{present:04X}", 6: f"L_{staged:04X}"}
                guard = references[3]
            assembly.records.append(Record(pc, bytecode[pc:min(pc + 16, end)], "data", references=references, block=block, guard=guard))
    assembly.records.sort(key=lambda record: record.pc)
    labels_by_pc = {}
    for name, pc in assembly.labels.items():
        labels_by_pc.setdefault(pc, []).append(name)
    for record in assembly.records:
        record.labels = labels_by_pc.get(record.pc, [])
    return assembly


def parse_assembly(text: str) -> Assembly:
    assembly = Assembly(b"", 0)
    seen = set()
    block = None
    bindings = []
    for line_number, original in enumerate(text.splitlines(), 1):
        line = original.split("//", 1)[0].strip()
        if not line:
            continue
        try:
            if line == ".xga 1":
                key = "version"
            elif match := re.fullmatch(rf"\.block ({IDENT}) ({NUMBER}) ({NUMBER})", line):
                if block is not None or match[1] in assembly.blocks:
                    raise ValueError("nested or duplicate block")
                block = match[1]
                assembly.blocks[block] = (integer(match[2]), integer(match[3]))
                continue
            elif line == ".end":
                if block is None or bindings:
                    raise ValueError("unexpected .end or unfinished binding")
                block = None
                continue
            elif match := re.fullmatch(rf"\.bind ({IDENT})", line):
                bindings.append(match[1])
                assembly.labels.setdefault(match[1], None)
                continue
            elif match := re.fullmatch(rf"\.triplets ({NUMBER}) = (.+)", line):
                pc = integer(match[1])
                if pc in assembly.tables:
                    raise ValueError("duplicate triplet table")
                assembly.tables[pc] = [integer(value.strip()) for value in match[2].split(",")]
                continue
            elif match := re.fullmatch(rf'\.bitmap "({HEX})"', line):
                key = "bitmap"
                assembly.bitmap = bytes.fromhex(match[1])
            elif match := re.fullmatch(rf"\.size ({NUMBER})", line):
                key = "size"
                assembly.size = integer(match[1])
            elif match := re.fullmatch(rf"\.label ({IDENT}) = ({NUMBER})", line):
                assembly.label(match[1], integer(match[2]))
                continue
            elif match := re.fullmatch(rf"\.alias ({IDENT}) = ({IDENT}) \+ ({NUMBER})", line):
                assembly.label(match[1], None)
                assembly.aliases[match[1]] = (match[2], integer(match[3]))
                continue
            elif match := re.fullmatch(r"\.row ([0-9]+) = (.+)", line):
                entity = int(match[1])
                if entity in assembly.rows:
                    raise ValueError(f"duplicate entity: {entity}")
                assembly.rows[entity] = [name.strip() for name in match[2].split(",")]
                continue
            elif match := re.fullmatch(rf'\.at ({NUMBER}|auto) (op|data|padding|edge) "({HEX})"(.*)', line):
                pc = None if match[1] == "auto" else integer(match[1])
                record = Record(pc, bytes.fromhex(match[3]), match[2], line=line_number, block=block, labels=bindings)
                bindings = []
                for token in match[4].split():
                    guard = re.fullmatch(rf"!guard=({IDENT})", token)
                    if guard:
                        record.guard = guard[1]
                        continue
                    relative = re.fullmatch(rf"!relative=(-?[0-9]+):({IDENT})", token)
                    if relative:
                        displacement = int(relative[1])
                        if displacement in record.relative:
                            raise ValueError("duplicate relative dependency")
                        record.relative[displacement] = relative[2]
                        continue
                    flow = re.fullmatch(rf"!(alternate|jump)=({IDENT})", token)
                    if flow:
                        setattr(record, "alternate" if flow[1] == "alternate" else "jump_target", flow[2])
                        continue
                    alignment = re.fullmatch(rf"!align=([0-9]+):(?:([0-9]+)|high\(({IDENT})\))", token)
                    if alignment:
                        record.alignment = checked(int(alignment[1]), 1, 65536, "alignment")
                        record.residue = int(alignment[2]) if alignment[2] is not None else alignment[3]
                        continue
                    ref = re.fullmatch(rf"@([0-9]+)=({IDENT})", token)
                    if not ref or int(ref[1]) in record.references:
                        raise ValueError(f"invalid or duplicate reference: {token}")
                    record.references[int(ref[1])] = ref[2]
                assembly.records.append(record)
                continue
            else:
                raise ValueError("unknown assembly syntax")
            if key in seen:
                raise ValueError(f"duplicate {key}")
            seen.add(key)
        except ValueError as error:
            raise ValueError(f"line {line_number}: {error}") from error
    if seen != {"version", "bitmap", "size"}:
        raise ValueError("assembly requires .xga 1, .bitmap and .size")
    if block is not None or bindings:
        raise ValueError("unclosed block or unfinished binding")
    return assembly


def compile_xgs(text: str) -> Assembly:
    """Parse the self-contained editable DSL.

    The grammar is deliberately line-oriented: one declaration, annotation or
    instruction per line, with the same nested sections emitted by the renderer.
    Unknown text is always an error, including text in diagnostics sections.
    """
    # Discard comments before even selecting the grammar. Preserve line numbers
    # for diagnostics; no downstream compiler stage receives trace payloads.
    text = "\n".join(line.split("//", 1)[0] for line in text.splitlines())
    if not re.search(r'^\s*variable_types\s*=', text, re.MULTILINE):
        from editable_field_scripts import compile_source

        return compile_source(text)
    assembly = Assembly(b"", 0)
    symbols = {}
    names = set()
    stack: list[tuple[str, object]] = []
    sections = set()
    pending_labels = []
    annotation = None
    alignment = (1, 0)
    statements = []
    arrival_indices = set()
    arrival_records = []
    bitmap_seen = size_seen = field_seen = False

    def context() -> str:
        return stack[-1][0] if stack else ""

    def push(kind: str, value=None) -> None:
        stack.append((kind, value))

    for line_number, original in enumerate(text.splitlines(), 1):
        line = original.split("//", 1)[0].strip()
        if not line:
            continue
        try:
            if line == "}":
                if not stack or annotation is not None or pending_labels or alignment != (1, 0):
                    raise ValueError("unexpected closing brace or unfinished instruction/label")
                kind, value = stack.pop()
                if kind == "arrivals":
                    assembly.records = [r for r in assembly.records if r.block != value[2]] + [r for _, r in sorted(arrival_records, key=lambda pair: pair[0])]
                continue
            if match := re.fullmatch(r"field ([0-9]+) \{", line):
                if stack or field_seen:
                    raise ValueError("duplicate or nested field")
                field_seen = True
                push("field")
            elif context() == "field" and (match := re.fullmatch(rf'variable_types = "({HEX})";', line)):
                if bitmap_seen:
                    raise ValueError("duplicate variable_types")
                bitmap_seen = True
                assembly.bitmap = bytes.fromhex(match[1])
            elif context() == "field" and (match := re.fullmatch(rf"bytecode_size = ({NUMBER});", line)):
                if size_seen:
                    raise ValueError("duplicate bytecode_size")
                size_seen = True
                assembly.size = integer(match[1])
            elif context() == "field" and line in {"state {", "labels {", "entities {", "shared_code {", "non_instruction_regions {", "diagnostics {", "triplet_tables {"}:
                section = line[:-2]
                if section in sections:
                    raise ValueError(f"duplicate section: {section}")
                sections.add(section)
                push(section)
            elif context() == "state" and line in {"persistent {", "scene {", "out_of_range {"}:
                scope = line[:-2]
                if scope in sections:
                    raise ValueError(f"duplicate scope: {scope}")
                sections.add(scope)
                push(scope)
            elif context() in {"persistent", "scene", "out_of_range"} and (match := re.fullmatch(rf"(signed|unsigned) ({IDENT}) @offset\(({NUMBER})\);", line)):
                value_type, name, offset_text = match.groups()
                offset = checked(integer(offset_text), 0, 65535, "variable offset")
                scope = context()
                if offset in symbols or (scope, name) in names:
                    raise ValueError("duplicate variable name or offset")
                names.add((scope, name))
                symbols[offset] = VariableSymbol(offset, name, scope, value_type, "source", "declared")
            elif context() == "labels" and (match := re.fullmatch(rf"({IDENT}) @offset\(({NUMBER})\);", line)):
                assembly.label(match[1], integer(match[2]))
            elif context() == "triplet_tables" and (match := re.fullmatch(rf"table @at\(({NUMBER})\) = \[(.*)\];", line)):
                pc = integer(match[1])
                if pc in assembly.tables:
                    raise ValueError("duplicate triplet table")
                assembly.tables[pc] = [integer(value.strip()) for value in match[2].split(",") if value.strip()]
            elif context() == "entities" and (match := re.fullmatch(r"entity ([0-9]+) \{", line)):
                entity = int(match[1])
                if entity in assembly.rows:
                    raise ValueError(f"duplicate entity: {entity}")
                assembly.rows[entity] = [""] * 32
                push("entity", entity)
            elif context() == "entity" and line in {"events {", "code {"}:
                key = (stack[-1][1], line[:-2])
                if key in sections:
                    raise ValueError(f"duplicate entity section: {line[:-2]}")
                sections.add(key)
                push(line[:-2], stack[-1][1])
            elif context() == "events" and (match := re.fullmatch(rf"(initialize|update|interact|contact|routine\[([0-9]+)(?:\.\.([0-9]+))?\])\s*->\s*({IDENT});", line)):
                roles = {"initialize": 0, "update": 1, "interact": 2, "contact": 3}
                first = roles[match[1]] if match[1] in roles else int(match[2])
                last = first if match[3] is None else int(match[3])
                checked(first, 0, 31, "routine ID")
                checked(last, first, 31, "routine range")
                row = assembly.rows[stack[-1][1]]
                for index in range(first, last + 1):
                    if row[index]:
                        raise ValueError(f"duplicate routine binding: {index}")
                    row[index] = match[4]
            elif context() in {"code", "shared_code"} and (match := re.fullmatch(rf"block @source\(({NUMBER})\.\.({NUMBER})\) \{{", line)):
                start, end = integer(match[1]), integer(match[2])
                block = f"code_{start:04X}"
                if block in assembly.blocks:
                    raise ValueError("duplicate code block")
                assembly.blocks[block] = (start, end)
                push("block", block)
            elif context() in {"code", "shared_code", "block"} and (match := re.fullmatch(rf"({IDENT}):", line)):
                if annotation is not None:
                    raise ValueError("label between annotation and statement")
                pending_labels.append(match[1])
            elif context() in {"code", "shared_code", "block"} and (match := re.fullmatch(rf'(?:@at\(({NUMBER})\) )?@encoding\("({HEX})"\)', line)):
                if annotation is not None:
                    raise ValueError("duplicate instruction annotation")
                annotation = (integer(match[1]) if match[1] else None, bytes.fromhex(match[2]))
            elif context() in {"code", "shared_code", "block"} and (match := re.fullmatch(rf"@align\(([0-9]+), (?:([0-9]+)|high\(({IDENT})\))\)", line)):
                alignment = (checked(int(match[1]), 1, 65536, "alignment"), int(match[2]) if match[2] is not None else match[3])
            elif context() in {"code", "shared_code", "block"} and line.endswith(";"):
                pc, seed = annotation if annotation is not None else (None, None)
                if seed == b"":
                    raise ValueError("empty instruction encoding")
                for label in pending_labels:
                    assembly.label(label, pc)
                block = stack[-1][1] if context() == "block" else None
                statements.append((pc, seed, line, line_number, block, pending_labels, alignment))
                alignment = (1, 0)
                pending_labels = []
                annotation = None
            elif context() == "field" and line == "arrivals unknown;":
                if "arrivals" in sections:
                    raise ValueError("duplicate arrivals")
                sections.add("arrivals")
            elif context() in {"field", "non_instruction_regions"} and (match := re.fullmatch(rf"(arrivals|region {IDENT}) @source\(({NUMBER})\.\.({NUMBER})\) \{{", line)):
                kind = "arrivals" if match[1] == "arrivals" else "region"
                if (kind == "arrivals") != (context() == "field"):
                    raise ValueError("misplaced data section")
                if kind == "arrivals":
                    if kind in sections:
                        raise ValueError("duplicate arrivals")
                    sections.add(kind)
                    arrival_indices.clear()
                    arrival_records = []
                start, end = integer(match[2]), integer(match[3])
                block = f"data_{start:04X}"
                if block in assembly.blocks:
                    raise ValueError("duplicate data block")
                assembly.blocks[block] = (start, end)
                push(kind, (start, end, block))
            elif context() == "arrivals" and line == "marker: 255;":
                record = Record(stack[-1][1][0], b"\xff", "data", line=line_number, block=stack[-1][1][2])
                assembly.records.append(record)
                arrival_records.append((-1, record))
            elif context() == "arrivals" and (match := re.fullmatch(r"arrival ([0-9]+) \{ x: (-?[0-9]+), z: (-?[0-9]+), walkmesh: ([0-9]+), camera: ([0-9]+|restore), actor: ([0-9]+|restore) \}", line)):
                index = int(match[1])
                if index in arrival_indices:
                    raise ValueError("duplicate arrival index")
                arrival_indices.add(index)
                x, z = (checked(int(match[i]), -32768, 32767, "arrival coordinate") for i in (2, 3))
                small = [255 if match[i] == "restore" else checked(int(match[i]), 0, 255, "arrival byte") for i in (4, 5, 6)]
                pc = stack[-1][1][0] + 1 + index * 7
                record = Record(pc if pc < stack[-1][1][1] else None, struct.pack("<hhBBB", x, z, *small), "data", line=line_number, block=stack[-1][1][2])
                assembly.records.append(record)
                arrival_records.append((index, record))
            elif context() == "region" and (match := re.fullmatch(rf'bytes @offset\(({NUMBER})\) = "({HEX})"(.*);', line)):
                references = {}
                for token in match[3].split():
                    reference = re.fullmatch(rf"@([0-9]+)=({IDENT})", token)
                    if not reference or int(reference[1]) in references:
                        raise ValueError("invalid or duplicate data reference")
                    references[int(reference[1])] = reference[2]
                assembly.records.append(Record(integer(match[1]), bytes.fromhex(match[2]), "data", references=references, line=line_number, block=stack[-1][1][2]))
            else:
                raise ValueError(f"unexpected syntax in {context() or 'document'}: {line}")
        except (ValueError, struct.error) as error:
            raise ValueError(f"line {line_number}: {error}") from error
    if stack or not field_seen:
        raise ValueError("unclosed or missing field block")
    if len(assembly.bitmap) != 128:
        raise ValueError("source requires a complete variable_types bitmap")
    if not size_seen:
        raise ValueError("source requires bytecode_size")
    bitmap = bytearray(assembly.bitmap)
    for offset, symbol in symbols.items():
        if offset < 0x800 and offset % 2 == 0:
            index = offset // 2
            bit = 1 << (index % 8)
            bitmap[index // 8] = (bitmap[index // 8] & ~bit) | (bit if symbol.value_type == "unsigned" else 0)
        elif symbol.value_type != ("unsigned" if offset >= 0x800 else "signed"):
            raise ValueError(f"{symbol.reference}: signedness cannot be encoded for this anomalous offset")
    assembly.bitmap = bytes(bitmap)
    for pc, seed, statement, line_number, block, labels, alignment in statements:
        try:
            references = {}
            relative = {}
            if pc is not None and seed is not None:
                offsets = (1,) if seed == b"\xfe" else instruction_dependencies(decode_instruction(seed, 0))
                for offset in offsets:
                    name = f"L_{pc + offset:04X}"
                    assembly.labels.setdefault(name, pc + offset)
                    relative[offset] = name
            raw = encode_statement(statement, seed, symbols, assembly.labels,
                                   references=references, layout_references=relative)
            assembly.records.append(Record(pc, raw, references=references, line=line_number, block=block,
                                           labels=labels, original=seed, alignment=alignment[0],
                                           residue=alignment[1], relative=relative))
        except (ValueError, struct.error) as error:
            raise ValueError(f"line {line_number}, PC {hex(pc) if pc is not None else 'new'}: {error}") from error
    return assembly
