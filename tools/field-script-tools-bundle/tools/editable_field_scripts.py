"""Standalone Field source, native lowering and validated same-file fidelity."""

from __future__ import annotations

import re
import struct

from compile_field_scripts import Assembly, IDENT, Record
from decompile_field_scripts import (
    DOCUMENTED_VARIABLES, PRIMARY_TERMINALS, VariableSymbol, _arrival_records, _entry_aliases,
    _instruction_owners, _routine_lines, address_operand_offsets, analyze_bytecode,
    build_variable_symbols, decode_instruction, render_instruction,
)
from field_instruction_codec import (
    checked, encode_statement, equivalent_instruction_encoding, instruction_dependencies,
    instruction_form, integer, normalized, render_lossless_instruction,
)
from field_semantic_ops import SCRIPT_IMAGE_MAGIC, encode_semantic, render_semantic


def _slot_ranges(values):
    ranges = []
    for value in sorted(values):
        if ranges and value == ranges[-1][1] + 2:
            ranges[-1][1] = value
        else:
            ranges.append([value, value])
    return ", ".join(f"0x{start:04X}" if start == end else f"0x{start:04X}..0x{end:04X}" for start, end in ranges)


def _alternate(instruction) -> bool:
    if instruction.subopcode is not None:
        return False
    if instruction.opcode == 0x9A:
        return int.from_bytes(instruction.raw[1:3], "little") <= 0x8000
    return instruction.opcode in {0xD4, 0xFC} and instruction.raw[1] in {0xFD, 0xFE, 0xFF}


def render_source(field_id: int, scripts: bytes, metadata: dict) -> tuple[str, dict]:
    bytecode = scripts[metadata["bytecode_offset"]:]
    analysis = analyze_bytecode(bytecode, metadata)
    instructions = dict(analysis["instructions"])
    symbols = build_variable_symbols(analysis, metadata)
    owners = _instruction_owners(analysis | {"instructions": instructions}, metadata)
    tables = {pc: list(entries) for pc, entries in analysis["computed_successors"].items() if entries}
    slots = {pc for entries in tables.values() for pc in entries}
    labels = set(analysis["labels"])
    statements = {}
    terminal_pcs = set()
    script_reads = {}
    read_images = {}
    for pc, instruction in instructions.items():
        if instruction.subopcode is None and instruction.opcode in {0x48, 0x49}:
            index = int.from_bytes(instruction.raw[5:7], "little")
            base = int.from_bytes(instruction.raw[1:3], "little")
            destination = symbols[int.from_bytes(instruction.raw[3:5], "little")].reference
            image_body = next((body for body, (_, end) in analysis["script_images"].items()
                               if body <= base < end), 0)
            if image_body not in read_images:
                image_end = analysis["script_images"][image_body][1] if image_body else len(bytecode)
                read_images[image_body] = (f"script_read_image_{len(read_images)}", bytecode[image_body:image_end])
            image_name = read_images[image_body][0]
            script_reads[pc] = (base, image_name, base - image_body)
            name = "state.write_script_u8_to_variable" if instruction.opcode == 0x48 else (
                "state.read_script_s16" if instruction.raw[7] else "state.read_script_u16")
            index_value = str(index & 0x7FFF) if index & 0x8000 else symbols[index].reference
            statements[pc] = f"{name}(script_data_{base:04X}, {index_value}) -> ({destination});"
            continue
        semantic = render_semantic(instruction, bytecode, symbols)
        if semantic is not None:
            statements[pc], kind, terminal = semantic
            if terminal:
                terminal_pcs.add(pc)
            if kind == "party_stage":
                labels.update((pc + 3, pc + 5))
        elif instruction.raw == b"\xfe":
            statements[pc] = "flow.extended_prefix();"
        else:
            statements[pc] = render_lossless_instruction(instruction, symbols)
    data_bases = {int.from_bytes(ins.raw[1:3], "little") for ins in instructions.values()
                  if ins.subopcode is None and ins.opcode in {0x48, 0x49}}
    control_labels = set(analysis["entries"]) | {target for ins in instructions.values() for target in ins.targets}
    control_labels.update(pc for entries in analysis["computed_successors"].values() for pc in entries)
    labels.difference_update(data_bases - control_labels)
    relative = {}
    skips = {}
    for pc, instruction in instructions.items():
        if _alternate(instruction) and pc + 6 < len(bytecode):
            relative[pc] = pc + 6
            labels.add(pc + 6)
        if instruction.opcode == 0xA6 and instruction.raw[2] & 0x80:
            skips[pc] = (pc + 3 + (int.from_bytes(instruction.raw[1:3], "little") & 0x7FFF) * 3) & 0xFFFF
            labels.add(skips[pc])
    # An interior code address must be explicit as a byte-relative symbolic
    # alias, rather than silently attaching to a nearby instruction.
    aliases = {}
    alias_fallbacks = {}
    for base, image_name, offset in script_reads.values():
        aliases[f"script_data_{base:04X}"] = (image_name, offset)
    for pc, (staged, present) in analysis["semantic_branches"].items():
        for target in labels:
            if pc + 3 <= target < pc + 11:
                aliases[f"L_{target:04X}"] = (f"D_{pc + 3:04X}", target - pc - 3)
    for target in list(labels):
        if f"L_{target:04X}" in aliases:
            continue
        if target in instructions:
            continue
        containing = next((pc for pc, ins in instructions.items() if pc < target < pc + ins.size), None)
        if containing is not None:
            labels.add(containing)
            aliases[f"L_{target:04X}"] = (f"L_{containing:04X}", target - containing)
            if target in analysis.get('interior_terminals', {}):
                alias_fallbacks[f'L_{target:04X}'] = render_lossless_instruction(analysis['interior_terminals'][target], symbols)
    groups = []
    current = None
    for pc, instruction in sorted(instructions.items()):
        if pc in slots:
            continue
        owner_set = owners[pc]
        owner = next(iter(owner_set)) if len(owner_set) == 1 else None
        end = pc + instruction.size + 3 * len(tables.get(pc, ()))
        if current is None or current["end"] != pc or current["owner"] != owner or pc in labels or pc in analysis["alignments"]:
            current = {"name": f"B_{pc:04X}", "start": pc, "end": end, "owner": owner, "pcs": []}
            groups.append(current)
        current["pcs"].append(pc)
        current["end"] = end
        if instruction.targets or instruction.opcode in PRIMARY_TERMINALS | {0xA6} or pc in relative or pc in terminal_pcs:
            current = None
    for group in groups:
        labels.add(group["start"])
        if group["end"] < len(bytecode):
            labels.add(group["end"])

    arrivals = _arrival_records(bytecode, analysis, metadata)
    arrival_end = 1 + len(arrivals) * 7 if arrivals is not None and metadata["arrival_table_marker_present"] else 0
    for start in analysis["landing_data"]:
        for target in labels:
            if start < target < start + 4:
                aliases[f"L_{target:04X}"] = (f"D_{start:04X}", target - start)
    source = ["// Xenogears editable Field script", f"field {field_id} {{", "  state {"]
    undeclared_unsigned = {int(offset, 16) for offset in metadata["unsigned_variable_offsets"]} - symbols.keys()
    if undeclared_unsigned:
        source.append(f"    unsigned slots[{_slot_ranges(undeclared_unsigned)}];")
    for scope in ("persistent", "scene", "out_of_range"):
        scoped = [symbol for symbol in symbols.values() if symbol.scope == scope]
        if scoped:
            source.append(f"    {scope} {{")
            source.extend(f"      {symbol.value_type} {symbol.name} at 0x{symbol.offset:04X};" for symbol in scoped)
            source.append("    }")
    source.extend(("  }", ""))
    if arrival_end:
        for pc in labels:
            if pc < arrival_end:
                base = "arrivals" if pc == 0 else f"arrival_{(pc - 1) // 7}"
                aliases[f"L_{pc:04X}"] = (base, 0 if pc == 0 else (pc - 1) % 7)
    for name, (base, offset) in sorted(aliases.items()):
        fallback = f' fallback {alias_fallbacks[name][:-1]}' if name in alias_fallbacks else ''
        source.append(f"  alias {name} = {base} + {offset}{fallback};")
    if arrival_end:
        source.extend(("", "  arrivals {"))
        for index, arrival in enumerate(arrivals):
            camera = "restore" if arrival["camera_direction"] == 255 else arrival["camera_direction"]
            actor = "restore" if arrival["actor_direction"] == 255 else arrival["actor_direction"]
            source.append(f"    arrival {index} {{ x: {arrival['x']}, z: {arrival['z']}, walkmesh: {arrival['walkmesh']}, camera: {camera}, actor: {actor} }}")
        source.extend(("  }", ""))

    def code_lines(group, indent):
        result = [f"{indent}block {group['name']} {{"]
        for pc in group["pcs"]:
            instruction = instructions[pc]
            if pc in labels:
                result.append(f"{indent}  L_{pc:04X}:")
            if pc in analysis["entries"]:
                result.append(f"{indent}  // event: {_entry_aliases(analysis['entries'][pc])}")
            statement = statements[pc]
            if pc in tables:
                pairs = ", ".join(f"L_{entry:04X} -> L_{instructions[entry].targets[0]:04X}" for entry in tables[pc])
                statement = statement[:-2] + f", [{pairs}]);"
            elif pc in skips:
                statement = f"flow.skip_triplets_to(L_{skips[pc]:04X});"
            if pc in relative:
                statement = statement[:-1] + f" otherwise goto L_{relative[pc]:04X};"
            result.append(f"{indent}  {statement} // {pc:04X}: {instruction.raw.hex(' ').upper()}")
            for entry in tables.get(pc, ()):
                if entry in analysis["entries"]:
                    result.append(f"{indent}  // event: {_entry_aliases(analysis['entries'][entry])}")
                result.append(f"{indent}  // {entry:04X}: {instructions[entry].raw.hex(' ').upper()}")
        last = instructions[group["pcs"][-1]]
        if group["pcs"][-1] in terminal_pcs:
            pass
        elif last.opcode not in PRIMARY_TERMINALS | {0x01} and group["pcs"][-1] not in skips and group["end"] < len(bytecode):
            result.append(f"{indent}  fallthrough L_{group['end']:04X};")
        elif last.opcode not in PRIMARY_TERMINALS | {0x01, 0xA6} and group["end"] == len(bytecode):
            result.append(f"{indent}  unreachable;")
        result.append(f"{indent}}}")
        return result

    source.append("  entities {")
    for row in metadata["routine_rows"]:
        source.extend((f"    entity {row['entity_id']} {{", "      events {"))
        source.extend(_routine_lines(row, trace_entries=True))
        source.append("      }")
        source.append("    }")
    source.append("  }")
    data_regions = []
    for region in analysis["data_regions"]:
        if region["classification"] == "arrival_table" or any(start <= region["start"] < end for start, end in analysis["script_images"].values()):
            continue
        fragments = [(region["start"], region["end"])]
        data_regions.extend(region | {"start": start, "end": end, "size": end - start}
                            for start, end in fragments)
    source.extend(("", "  program {"))
    units = [(group["start"], "code", group) for group in groups] + [(region["start"], "data", region) for region in data_regions]
    for _, kind, unit in sorted(units, key=lambda unit: unit[0]):
        if kind == "code":
            source.extend(code_lines(unit, "    "))
        else:
            region = unit
            start, end = region["start"], region["end"]
            name = "arrivals" if start == 0 and metadata["arrival_table_marker_present"] and not arrival_end else f"D_{start:04X}"
            source.append(f"    region {name} {{")
            cuts = sorted({start, end, *(pc for pc in labels if start <= pc < end)})
            if start in analysis["landing_data"] or start - 3 in analysis["semantic_branches"]:
                cuts = [start, end]
            for left, right in zip(cuts, cuts[1:]):
                if left in labels:
                    source.append(f"      L_{left:04X}:")
                for pc in range(left, right, 16):
                    raw = bytecode[pc:min(pc + 16, right)]
                    refs = analysis["landing_data"].get(pc, {})
                    trace = f" // {pc:04X}: {raw.hex(' ').upper()}"
                    if pc - 3 in analysis["semantic_branches"]:
                        staged, present = analysis["semantic_branches"][pc - 3]
                        source.append(f"      party_landing(L_{staged:04X}, L_{present:04X});" + trace)
                    elif refs:
                        source.append(f"      landing(L_{refs[1]:04X}, L_{refs[2]:04X});" + trace)
                    else:
                        source.append(f'      bytes "{raw.hex(" ").upper()}";' + trace)
            source.append("    }")
    for view, (image_name, image) in read_images.items():
        source.append(f"    region {image_name} {{")
        source.append(f"      // view: {view:04X}")
        for at in range(0, len(image), 16):
            source.append(f'      bytes "{image[at:at + 16].hex(" ").upper()}";')
        source.append("    }")
    source.append("  }")
    if analysis["diagnostics"]:
        source.append("")
        source.extend(f"  // Diagnostic: {message}" for message in analysis["diagnostics"])
    source.extend(("}", ""))
    report = {key: analysis[key] for key in ("instruction_count", "instruction_bytes", "data_bytes", "classified_bytes", "bytecode_size", "instruction_coverage_percent", "total_coverage_percent", "diagnostics")}
    report.update({
        "data_range_count": len(analysis["data_ranges"]),
        "data_classifications": {kind: sum(r["size"] for r in analysis["data_regions"] if r["classification"] == kind)
                                 for kind in {r["classification"] for r in analysis["data_regions"]}},
        "semantic_symbol_count": len(symbols),
        "orphan_instruction_count": len(analysis["orphan_instruction_pcs"]),
        "orphan_instruction_bytes": sum(analysis["instructions"][pc].size for pc in analysis["orphan_instruction_pcs"]),
        "language": "xenogears-field-event-dsl/v10",
        "script_data_image_bytes": sum(len(image) for _, image in read_images.values()),
        "recovered_native_islands": len(analysis["recovered_lowerings"]),
        "cloned_interior_terminals": len(analysis.get("interior_terminals", {})),
    })
    from field_source_fidelity import add_fidelity_traces

    text = group_source_by_entity("\n".join(source), scripts, analysis=analysis)
    return add_fidelity_traces(text, scripts, metadata), report


def group_source_by_entity(text: str, scripts: bytes, *, analysis: dict | None = None) -> str:
    """Present generated code with its owning entity, without physical blocks."""
    from extract_disc_field_scripts import parse_scripts_file

    lines = text.splitlines()
    start = next((i for i, line in enumerate(lines) if line.strip() == "program {"), None)
    if start is None:
        return text
    metadata = parse_scripts_file(scripts)
    if analysis is None:
        analysis = analyze_bytecode(scripts[metadata["bytecode_offset"]:], metadata)
    owners = _instruction_owners(analysis, metadata)
    owned = {}
    shared = []
    data = []
    index = start + 1
    while lines[index].strip() != "}":
        match = re.fullmatch(r"\s*(block|region) (\w+) \{", lines[index])
        if not match:
            raise ValueError("unexpected generated program layout")
        kind, name = match.groups()
        index += 1
        body = []
        while lines[index].strip() != "}":
            body.append(lines[index][6:])
            index += 1
        index += 1
        if kind == "region":
            if name.startswith("D_") and not any(line.strip() == f"L_{name[2:]}:" for line in body):
                body.insert(0, f"L_{name[2:]}:")
            declaration = f"script_data {name}" if name.startswith("script_read_image_") else name
            data.extend([f"    {declaration} {{", *("      " + line if line else "" for line in body), "    }"])
            continue
        pc = int(name[2:], 16)
        if not any(line.strip() == f"L_{pc:04X}:" for line in body):
            body.insert(0, f"L_{pc:04X}:")
        entities = owners.get(pc, set())
        destination = owned.setdefault(next(iter(entities)), []) if len(entities) == 1 else shared
        if destination:
            destination.append("")
        destination.extend(body)
    output = []
    entity = None
    for line in lines[:start]:
        if match := re.fullmatch(r"    entity (\d+) \{", line):
            entity = int(match[1])
        elif line == "    }" and entity is not None:
            if entity in owned:
                output.extend(["      code {", *("        " + item if item else "" for item in owned[entity]), "      }"])
            entity = None
        output.append(line)
    if shared:
        output.extend(["  shared_code {", *("    " + line if line else "" for line in shared), "  }", ""])
    if data:
        output.extend(["  data {", *data, "  }"])
    output.extend(lines[index + 1:])
    return "\n".join(output) + "\n"


def restore_source_comments(text: str, scripts: bytes) -> str:
    """Restore traces from the semantic renderer without compiling the source.

    Native instruction counts no longer correspond one-to-one with statements.
    Compare source tokens, preserve authored comments, and refuse to discard edits.
    """
    from extract_disc_field_scripts import parse_scripts_file
    from field_source_fidelity import ORIGIN

    metadata = parse_scripts_file(scripts)
    field = re.search(r"^\s*field (\d+) \{", text, re.MULTILINE)
    if field is None:
        raise ValueError("missing field declaration")
    rendered, _ = render_source(int(field[1]), scripts, metadata)

    def is_trace(line):
        return (ORIGIN.fullmatch(line) is not None or line.strip().startswith("// event:")
                or re.fullmatch(r"\s*// [0-9A-F]{4,}: [0-9A-F ]+", line) is not None)

    expected = []
    pending = []
    for line in rendered.splitlines():
        if is_trace(line):
            pending.append(line)
        elif line.split("//", 1)[0].strip():
            code, _, comment = line.partition("//")
            trace = " //" + comment if re.match(r" (?:[0-9A-F]{4,}: |entry: )", comment) else ""
            expected.append((normalized(code), pending, trace))
            pending = []
    output = []
    index = 0
    for original in text.splitlines():
        if is_trace(original):
            continue
        original = re.sub(r"\s+// [0-9A-F]{4,}: [0-9A-F ]+$", "", original)
        original = re.sub(r"\s+// entry: [0-9A-F]{4}$", "", original)
        code = original.split("//", 1)[0].strip()
        if not code:
            output.append(original)
            continue
        if index >= len(expected) or normalized(code) != expected[index][0]:
            raise ValueError("source differs from the current decompilation; decompile the edited binary to retain changes")
        _, traces, suffix = expected[index]
        output.extend(traces)
        output.append(original + suffix)
        index += 1
    if index != len(expected):
        raise ValueError("source is incomplete; refusing to overwrite edits")
    output.extend(pending)
    return "\n".join(output) + "\n"


def equivalent_scripts(compiled: bytes, original: bytes) -> bool:
    """Check exact reconstruction except verified instruction-only differences.

    Headers, bitmap, event rows, targets and data remain byte-exact. Never scan
    arbitrary data for opcode-looking byte patterns to relax the comparison.
    """
    if compiled == original:
        return True
    if len(compiled) != len(original):
        return False
    from extract_disc_field_scripts import parse_scripts_file

    metadata = parse_scripts_file(original)
    start = metadata["bytecode_offset"]
    expected = bytearray(original)
    analysis = analyze_bytecode(original[start:], metadata)
    shared_bytes = {pc + offset for pc, instruction in analysis["instructions"].items()
                    for offset in instruction_dependencies(instruction)}
    for pc, instruction in analysis["instructions"].items():
        emitted = compiled[start + pc:start + pc + instruction.size]
        if any(pc + offset in shared_bytes and value != instruction.raw[offset]
               for offset, value in enumerate(emitted)):
            continue  # An otherwise unused bit may be an operand of a neighbor.
        if equivalent_instruction_encoding(emitted, instruction.raw):
            expected[start + pc:start + pc + instruction.size] = emitted
    return bytes(expected) == compiled


def compile_source(text: str) -> Assembly:
    """Encode only semantic information explicitly present in the source."""
    assembly = Assembly(bytes(128), 0)
    stack = []
    seen_sections = set()
    declarations = []
    statements = []
    pending = []
    seen_blocks = set()
    seen_names = set()
    field_seen = False
    unsigned_slots = set()
    unreachable_blocks = set()
    landing_records = []
    fragment_has_code = False
    current_block = None
    arrival_records = []

    def context():
        return stack[-1][0] if stack else ""

    def label(name):
        if name.startswith("__xgs_") or name in seen_names:
            raise ValueError(f"reserved or duplicate label: {name}")
        seen_names.add(name)
        assembly.labels[name] = None

    def start_block(name, kind):
        nonlocal current_block
        if name in seen_blocks:
            raise ValueError(f"duplicate block: {name}")
        seen_blocks.add(name)
        rank = 0 if name == "arrivals" else len(seen_blocks)
        assembly.blocks[name] = (rank, rank)
        assembly.size = len(seen_blocks) + 1
        label(name)
        pending.append(name)
        current_block = name

    def start_fragment():
        nonlocal current_block, fragment_has_code
        name = f"__xgs_fragment_{len(seen_blocks)}"
        seen_blocks.add(name)
        assembly.blocks[name] = (len(seen_blocks), len(seen_blocks))
        assembly.size = len(seen_blocks) + 1
        current_block = name
        fragment_has_code = False

    for line_number, original in enumerate(text.splitlines(), 1):
        line = original.split("//", 1)[0].strip()
        if not line:
            continue
        try:
            if line == "}":
                if not stack or pending:
                    raise ValueError("unexpected closing brace or label without a following record")
                kind, value = stack.pop()
                if kind in {"block", "region", "arrivals", "code", "shared_code"}:
                    current_block = None
                    fragment_has_code = False
                if kind == "arrivals":
                    indices = sorted(index for index, _ in arrival_records)
                    if indices != list(range(len(indices))):
                        raise ValueError("arrival indices must be contiguous from zero")
                    assembly.records.extend(record for _, record in sorted(arrival_records))
                if kind == "script_data":
                    image_records = [record for record in assembly.records if record.block == value]
                    length = sum(len(record.raw) for record in image_records)
                    if not image_records or not length:
                        raise ValueError("script_data image cannot be empty")
                    header = Record(None, SCRIPT_IMAGE_MAGIC + struct.pack("<I", length), "data", block=value)
                    at = next(i for i, record in enumerate(assembly.records) if record is image_records[0])
                    assembly.records.insert(at, header)
                    current_block = None
                continue
            if match := re.fullmatch(r"field ([0-9]+) \{", line):
                if stack or field_seen:
                    raise ValueError("duplicate or nested field")
                field_seen = True
                stack.append(("field", None))
            elif context() == "field" and line in {"state {", "entities {", "program {", "shared_code {", "data {"}:
                kind = line[:-2]
                if kind in seen_sections:
                    raise ValueError(f"duplicate section: {kind}")
                seen_sections.add(kind)
                stack.append((kind, None))
            elif context() == "state" and (match := re.fullmatch(r"unsigned slots\[(.*)\];", line)):
                if "unsigned_slots" in seen_sections:
                    raise ValueError("duplicate unsigned slot declaration")
                seen_sections.add("unsigned_slots")
                for value in match[1].split(","):
                    bounds = [integer(part.strip()) for part in value.split("..")]
                    if len(bounds) not in {1, 2}:
                        raise ValueError("invalid unsigned slot range")
                    start, end = bounds[0], bounds[-1]
                    checked(start, 0, 0x7FE, "VM slot")
                    checked(end, start, 0x7FE, "VM slot range")
                    if start % 2 or end % 2:
                        raise ValueError("VM slots must be word-aligned")
                    unsigned_slots.update(range(start, end + 1, 2))
            elif context() == "state" and line in {"persistent {", "scene {", "out_of_range {"}:
                kind = line[:-2]
                if kind in seen_sections:
                    raise ValueError(f"duplicate scope: {kind}")
                seen_sections.add(kind)
                stack.append((kind, None))
            elif context() in {"persistent", "scene", "out_of_range"} and (match := re.fullmatch(rf"(new )?(signed|unsigned) ({IDENT})(?: at (0x[0-9A-Fa-f]+|[0-9]+))?;", line)):
                declarations.append((context(), match[3], match[2], integer(match[4]) if match[4] else None, line_number, bool(match[1])))
            elif context() == "field" and (match := re.fullmatch(rf"alias ({IDENT}) = ({IDENT})(?: \+ ([0-9]+))?(?: fallback (.+))?;", line)):
                label(match[1])
                assembly.aliases[match[1]] = (match[2], int(match[3] or 0))
                if match[4]:
                    terminal = encode_statement(match[4] + ';', None, {}, {})
                    if len(terminal) != 1 or terminal[0] not in PRIMARY_TERMINALS:
                        raise ValueError('alias fallback must be a single-byte terminal operation')
                    assembly.alias_fallbacks[match[1]] = terminal
            elif context() == "entities" and (match := re.fullmatch(r"entity ([0-9]+) \{", line)):
                entity = int(match[1])
                if entity in assembly.rows:
                    raise ValueError(f"duplicate entity: {entity}")
                assembly.rows[entity] = [""] * 32
                stack.append(("entity", entity))
            elif context() == "entity" and line in {"events {", "code {"}:
                kind = line[:-2]
                key = (stack[-1][1], kind)
                if key in seen_sections:
                    raise ValueError(f"duplicate entity section: {kind}")
                seen_sections.add(key)
                stack.append((kind, stack[-1][1]))
                if kind == "code":
                    current_block = None
                    fragment_has_code = False
            elif context() == "events" and (match := re.fullmatch(rf"(initialize|update|interact|contact|routine\[([0-9]+)(?:\.\.([0-9]+))?\])\s*->\s*({IDENT});", line)):
                roles = {"initialize": 0, "update": 1, "interact": 2, "contact": 3}
                first = roles[match[1]] if match[1] in roles else int(match[2])
                last = int(match[3]) if match[3] else first
                checked(first, 0, 31, "routine ID")
                checked(last, first, 31, "routine range")
                row = assembly.rows[stack[-1][1]]
                for index in range(first, last + 1):
                    if row[index]:
                        raise ValueError(f"duplicate routine binding: {index}")
                    row[index] = match[4]
            elif context() in {"program", "code", "shared_code"} and (match := re.fullmatch(rf"block ({IDENT}) \{{", line)):
                start_block(match[1], "code")
                stack.append(("block", current_block))
            elif context() in {"program", "data"} and (match := re.fullmatch(rf"region ({IDENT}) \{{", line)):
                start_block(match[1], "data")
                stack.append(("region", current_block))
            elif context() == "data" and (match := re.fullmatch(rf"script_data ({IDENT}) \{{", line)):
                start_block(match[1], "data")
                assembly.script_images.add(current_block)
                stack.append(("script_data", current_block))
            elif context() == "data" and (match := re.fullmatch(rf"({IDENT}) \{{", line)):
                start_block(match[1], "data")
                stack.append(("region", current_block))
            elif context() in {"code", "shared_code"} and (match := re.fullmatch(rf"({IDENT}):", line)):
                if current_block is None:
                    start_fragment()
                elif fragment_has_code:
                    statements.append((f"__xgs_sequence {match[1]};", 0, current_block, []))
                    start_fragment()
                label(match[1])
                pending.append(match[1])
            elif context() in {"block", "region", "script_data"} and (match := re.fullmatch(rf"({IDENT}):", line)):
                label(match[1])
                pending.append(match[1])
            elif context() in {"block", "code", "shared_code"} and line.endswith(";"):
                if current_block is None:
                    start_fragment()
                statements.append((line, line_number, current_block, pending))
                pending = []
                fragment_has_code = True
                if context() in {"code", "shared_code"} and line.startswith("fallthrough "):
                    current_block = None
                    fragment_has_code = False
            elif context() in {"region", "script_data"} and (match := re.fullmatch(r'bytes "([0-9A-Fa-f\s]+)"(?: refs\((.*)\))?;', line)):
                references = {}
                fields = [part.strip() for part in match[2].split(",")] if match[2] else []
                if len(fields) % 2:
                    raise ValueError("refs requires byte-offset/label pairs")
                for offset, name in zip(fields[::2], fields[1::2]):
                    at = integer(offset)
                    if at in references or not re.fullmatch(IDENT, name):
                        raise ValueError("invalid or duplicate data reference")
                    references[at] = name
                assembly.records.append(Record(None, bytes.fromhex(match[1]), "data", references, line_number,
                                               current_block, pending))
                pending = []
            elif context() == "region" and (match := re.fullmatch(rf"landing\(({IDENT}), ({IDENT})\);", line)):
                record = Record(None, b"\x01\x01\0\0", "data", {1: match[1], 2: match[2]}, line_number, current_block, pending)
                assembly.records.append(record)
                landing_records.append(record)
                pending = []
            elif context() == "region" and (match := re.fullmatch(rf"party_landing\(({IDENT}), ({IDENT})\);", line)):
                raw = bytes.fromhex("02 00 01 00 00 C0 00 00")
                assembly.records.append(Record(None, raw, "data", {3: match[2], 6: match[1]},
                                               line_number, current_block, pending, guard=match[2]))
                pending = []
            elif context() == "field" and line == "arrivals {":
                start_block("arrivals", "arrivals")
                assembly.records.append(Record(None, b"\xff", "data", line=line_number, block=current_block, labels=pending))
                pending = []
                stack.append(("arrivals", current_block))
            elif context() == "arrivals" and (match := re.fullmatch(r"arrival ([0-9]+) \{ x: (-?[0-9]+), z: (-?[0-9]+), walkmesh: ([0-9]+), camera: ([0-9]+|restore), actor: ([0-9]+|restore) \}", line)):
                index = int(match[1])
                name = f"arrival_{index}"
                label(name)
                x, z = (checked(int(match[i]), -32768, 32767, "arrival coordinate") for i in (2, 3))
                small = [255 if match[i] == "restore" else checked(int(match[i]), 0, 255, "arrival byte") for i in (4, 5, 6)]
                arrival_records.append((index, Record(None, struct.pack("<hhBBB", x, z, *small), "data", line=line_number,
                                                       block=current_block, labels=[name])))
            else:
                raise ValueError(f"unexpected syntax in {context() or 'document'}: {line}")
        except (ValueError, TypeError, struct.error) as error:
            raise ValueError(f"line {line_number}: {error}") from error
    if stack or not field_seen:
        raise ValueError("unclosed or missing field declaration")

    # Collect table case labels before encoding forward references.
    dispatches = {}
    for index, (statement, line, block, names) in enumerate(statements):
        match = re.fullmatch(r"flow\.dispatch_triplet_table\((.+), \[(.*)\]\);", statement)
        if match:
            cases = []
            for value in match[2].split(","):
                case = re.fullmatch(rf"\s*({IDENT})\s*->\s*({IDENT})\s*", value)
                if not case:
                    raise ValueError(f"line {line}: invalid dispatch case")
                label(case[1])
                cases.append((case[1], case[2]))
            dispatches[index] = (match[1], cases)

    symbols = {}
    variable_names = set()
    used = set(unsigned_slots)
    documented = {f"persistent.{name}": offset for offset, (name, _) in DOCUMENTED_VARIABLES.items()}
    resolved = []
    for scope, name, value_type, explicit, line, is_new in declarations:
        reference = f"{scope}.{name}"
        if reference in variable_names:
            raise ValueError(f"line {line}: duplicate variable {reference}")
        variable_names.add(reference)
        offset = explicit if explicit is not None else documented.get(reference)
        if offset is None and not is_new:
            raise ValueError(f"line {line}: {reference} requires an 'at' binding or an explicit 'new' declaration")
        if offset is not None:
            checked(offset, 0, 65535, "variable offset")
            expected_scope = "persistent" if offset < 0x400 else "scene" if offset < 0x800 else "out_of_range"
            if scope != expected_scope:
                raise ValueError(f"line {line}: {reference} belongs to the {expected_scope} VM range")
            used.add(offset)
        resolved.append([scope, name, value_type, offset, line])

    # Discover reservations from the actual source, including opaque operand
    # bytes. Temporary addresses for new variables are outside the scene bank;
    # they are never emitted in the linked program.
    provisional = {}
    restricted_variables = set()
    temporary = 0x800
    for scope, name, value_type, offset, line in resolved:
        if offset is None:
            while temporary in used:
                temporary += 2
            offset = temporary
            temporary += 2
        provisional[offset] = VariableSymbol(offset, name, scope, value_type, "source", "declared")
    for index, (statement, line, _, names) in enumerate(statements):
        if statement.startswith(("fallthrough ", "__xgs_sequence ", "flow.skip_triplets_to(")) or statement == "unreachable;":
            continue
        if match := re.fullmatch(rf"(.+) otherwise goto ({IDENT});", statement):
            statement = match[1] + ";"
        if index in dispatches:
            statement = f"flow.dispatch_triplet_table({dispatches[index][0]});"
        try:
            semantic = encode_semantic(statement, provisional, assembly.labels, provisional=True)
            if semantic is not None and semantic[0] == "mecha_reinterpret":
                match = re.fullmatch(r"actor\.load_current_actor_mecha_reinterpret\(([^()]+)\);", normalized(statement))
                restricted_variables.add(match[1])
            raw = semantic[1] if semantic is not None else encode_statement(statement, None, provisional, assembly.labels, references={})
        except ValueError as error:
            raise ValueError(f"line {line}: {error}") from error
        for at in range(2 if raw[0] == 0xFE else 1, len(raw) - 1):
            word = int.from_bytes(raw[at:at + 2], "little")
            for value in (word, word >> 4):
                if 0x400 <= value < 0x800:
                    used.add(value & ~1)
    for item in sorted(resolved, key=lambda item: (item[0], item[1])):
        scope, name, value_type, offset, line = item
        if offset is None:
            if scope != "scene":
                raise ValueError(f"line {line}: new {scope} variable {name} requires an explicit 'at' binding")
            offset = next((slot for slot in range(0x400, 0x800, 2) if slot not in used
                           and (f"{scope}.{name}" not in restricted_variables or (slot & 0xFF) > 2)), None)
            if offset is None:
                raise ValueError("no unreserved scene VM slots remain")
            used.add(offset)
        if offset in symbols:
            raise ValueError(f"line {line}: duplicate VM variable binding at 0x{offset:04X}")
        symbols[offset] = VariableSymbol(offset, name, scope, value_type, "source", "declared")
    bitmap = bytearray(assembly.bitmap)
    for offset in unsigned_slots:
        bitmap[offset // 16] |= 1 << ((offset // 2) % 8)
    for offset, symbol in symbols.items():
        if offset < 0x800 and offset % 2 == 0:
            index, bit = offset // 16, 1 << ((offset // 2) % 8)
            bitmap[index] = (bitmap[index] & ~bit) | (bit if symbol.value_type == "unsigned" else 0)
        elif symbol.value_type != ("unsigned" if offset >= 0x800 else "signed"):
            raise ValueError(f"{symbol.reference}: signedness cannot be encoded for this offset")
    assembly.bitmap = bytes(bitmap)

    for index, (statement, line, block, names) in enumerate(statements):
        try:
            if statement == "unreachable;":
                if names:
                    raise ValueError("unreachable must follow an instruction")
                unreachable_blocks.add(block)
                continue
            elif match := re.fullmatch(rf"__xgs_sequence ({IDENT});", statement):
                previous = next((r for r in reversed(assembly.records) if r.block == block), None)
                if previous is None or previous.kind == "edge" or previous.terminal or block in unreachable_blocks or previous.jump_target is not None or previous.raw[0] in PRIMARY_TERMINALS | {0x01}:
                    continue
                record = Record(None, b"", "edge", {1: match[1]}, 0, block)
            elif match := re.fullmatch(rf"fallthrough ({IDENT});", statement):
                record = Record(None, b"", "edge", {1: match[1]}, line, block, names)
            elif match := re.fullmatch(rf"flow\.skip_triplets_to\(({IDENT})\);", statement):
                record = Record(None, b"\xa6\0\x80", line=line, block=block, labels=names, jump_target=match[1])
            else:
                alternate = None
                if match := re.fullmatch(rf"(.+) otherwise goto ({IDENT});", statement):
                    statement, alternate = match[1] + ";", match[2]
                if index in dispatches:
                    statement = f"flow.dispatch_triplet_table({dispatches[index][0]});"
                references = {}
                relative = {}
                semantic = encode_semantic(statement, symbols, assembly.labels)
                if semantic is not None:
                    kind, raw, references, terminal = semantic
                    if alternate is not None:
                        raise ValueError("semantic operation already defines its continuations")
                    assembly.records.append(Record(None, raw, "semantic", references=references,
                                                   line=line, block=block, labels=names,
                                                   semantic=kind, terminal=terminal))
                    continue
                raw = encode_statement(statement, None, symbols, assembly.labels,
                                       references=references, layout_references=relative)
                if relative:
                    raise ValueError("legacy script-byte operands require re-decompilation into standalone semantic operations")
                if raw != b"\xfe" and instruction_dependencies(decode_instruction(raw, 0)):
                    raise ValueError("this native opcode requires its standalone semantic operation, not a neighboring-byte encoding")
                if raw != b"\xfe" and any(offset not in references for offset in address_operand_offsets(decode_instruction(raw, 0))):
                    raise ValueError("raw address operands require symbolic label arguments")
                # The codec derives physical relationships from script.* reads
                # and named continuations. No user-authored layout is needed.
                record = Record(None, raw, references=references, line=line, block=block,
                                labels=names, alternate=alternate, relative=relative)
                if raw[0] == 0xA6 and index not in dispatches and not statement.startswith("raw("):
                    raise ValueError("dispatch_triplet_table requires an explicit case list")
                if raw != b"\xfe" and _alternate(decode_instruction(raw, 0)) and alternate is None:
                    raise ValueError("this operation needs an 'otherwise goto' target for its alternate path")
            assembly.records.append(record)
            if index in dispatches:
                if record.raw[2] & 0x80:
                    case_index = int.from_bytes(record.raw[1:3], "little") & 0x7FFF
                    if case_index >= len(dispatches[index][1]):
                        raise ValueError("constant dispatch index leaves the case table")
                    record.jump_target = dispatches[index][1][case_index][0]
                for name, target in dispatches[index][1]:
                    assembly.records.append(Record(None, b"\x01\0\0", references={1: target}, line=line, block=block, labels=[name]))
        except (ValueError, struct.error) as error:
            raise ValueError(f"line {line}: {error}") from error
    # A landing pair is a semantic two-entry branch. Its shared-byte constraints
    # derive alignment from its targets; no saved addresses are consulted.
    bound = {name: record for record in assembly.records for name in record.labels}
    for landing in landing_records:
        normal, alternate = landing.references[1], landing.references[2]
        if normal not in bound or alternate not in bound:
            raise ValueError(f"line {landing.line}: unresolved landing destination")
        bound[normal].alignment, bound[normal].residue = 256, 1
        bound[alternate].alignment, bound[alternate].residue = 256, normal
    # Keep fallthrough explicit before an aligned target inside a block.
    for target_name in {name for landing in landing_records for name in landing.references.values()}:
        target = bound[target_name]
        at = next(i for i, item in enumerate(assembly.records) if item is target)
        if at and assembly.records[at - 1].block == target.block:
            previous = assembly.records[at - 1]
            if previous.kind == "op" and previous.raw[0] not in PRIMARY_TERMINALS | {0x01}:
                assembly.records.insert(at, Record(None, b"", "edge", {1: target_name}, block=target.block))
    first_records = {}
    for record in assembly.records:
        if record.block is not None:
            first_records.setdefault(record.block, record)
    for name, first in first_records.items():
        if first.kind == "data":
            continue
        body = [record for record in assembly.records if record.block == name]
        if any(record.kind == "edge" and record.line for record in body[:-1]):
            raise ValueError(f"block {name}: fallthrough must be its last statement")
        last = body[-1]
        if last.kind != "edge" and not last.terminal and last.jump_target is None and last.raw[0] not in PRIMARY_TERMINALS | {0x01}:
            if name not in unreachable_blocks:
                raise ValueError(f"block {name}: add stop, return, goto or an explicit fallthrough")
    # New or deliberately unbound events have a defined empty implementation.
    missing = [(entity, index) for entity, row in assembly.rows.items() for index, target in enumerate(row) if not target]
    if missing:
        empty = "__xgs_empty_event"
        assembly.labels[empty] = None
        assembly.records.append(Record(None, b"\0", labels=[empty]))
        for entity, index in missing:
            assembly.rows[entity][index] = empty
    for entity, row in assembly.rows.items():
        for index, target in enumerate(row):
            if target not in assembly.labels:
                raise ValueError(f"entity {entity}, routine {index}: unresolved label {target}")
    for record in assembly.records:
        for target in (*record.references.values(), *record.relative.values(), record.alternate, record.jump_target):
            if target is not None and target not in assembly.labels:
                raise ValueError(f"line {record.line}: unresolved label {target}")
    for name, (base, _) in assembly.aliases.items():
        if base not in assembly.labels:
            raise ValueError(f"alias {name}: unresolved label {base}")
    assembly.link_report["variables"] = {symbol.reference: offset for offset, symbol in symbols.items()}
    assembly.link_report["defaulted_events"] = missing
    assembly.link_report["source_stage"] = "clean"
    assembly.link_report["grouped_source"] = "program" not in seen_sections
    assembly.link_report["unreachable_assertions"] = sorted(unreachable_blocks)
    for site, record in enumerate(assembly.records):
        record.site = site
    return assembly
