"""Physical layout and relocation of Field VM code, tables and data.

Source ranges order blocks, not their edited contents. Within a block, source
order is authoritative. Original PCs are anchors for numeric references; new
instructions need no address. The output is ordinary retail Field bytecode.
"""

from __future__ import annotations

import bisect
import copy
import struct

from decompile_field_scripts import address_operand_offsets, decode_instruction
from field_instruction_codec import checked


def link_assembly(source, layout: str):
    from compile_field_scripts import Assembly, Record

    if layout not in {"exact", "relocate"}:
        raise ValueError(f"unknown layout: {layout}")
    if layout == "exact":
        if source.link_report.get("source_stage") == "clean":
            raise ValueError("exact layout requires XGA address assertions; XGS uses symbolic layout")
        result = copy.deepcopy(source)
        result.build("exact")
        result.link_report = {"layout": "exact", "source_size": source.size, "bytecode_size": source.size}
        return result

    grouped = {name: [] for name in source.blocks}
    free = []
    for record in source.records:
        if record.kind == "padding":
            if any(record.raw):
                raise ValueError("generated alignment padding must be zero; use data for payload bytes")
            continue
        if record.block is None:
            free.append(record)
        elif record.block not in grouped:
            raise ValueError(f"line {record.line}: unknown source block {record.block}")
        else:
            grouped[record.block].append(record)
    units = [(start, end, name, grouped[name]) for name, (start, end) in source.blocks.items()]
    free_units = []
    next_pc = source.size
    for record in reversed(free):
        if record.pc is not None:
            next_pc = record.pc
            free_units.append((record.pc, record.pc + len(record.original or record.raw), None, [record]))
        else:
            free_units.append((next_pc, next_pc, None, [record]))
    units.extend(reversed(free_units))
    if source.link_report.get("grouped_source"):
        def symbol_order(unit):
            _, _, name, items = unit
            labels = [label for item in items for label in item.labels if not label.startswith("__xgs_")]
            preferred = [label for label in labels if label.startswith("L_")]
            return (0 if name == "arrivals" else 1, min(preferred or labels, default="~"))
        units.sort(key=symbol_order)
        # Placement ranks are internal, not addresses inferred from label names.
        units = [(index, index, name, items) for index, (_, _, name, items) in enumerate(units)]
    else:
        units.sort(key=lambda unit: unit[0])
    records = []
    spans = {}
    previous_end = 0
    for start, end, name, items in units:
        if start < 0 or end < start or (name is not None and start < previous_end):
            raise ValueError("overlapping or invalid original block ranges")
        previous_end = max(previous_end, end)
        anchored = [r.pc for r in items if r.pc is not None]
        if anchored != sorted(set(anchored)):
            raise ValueError(f"block {name}: original instruction anchors must be unique and ordered")
        if name is not None and any(not start <= pc < end for pc in anchored):
            raise ValueError(f"block {name}: anchor leaves its original source range")
        bounds = dict(zip(anchored, anchored[1:] + [end]))
        for item in items:
            record = copy.deepcopy(item)
            record.source_block = record.block
            record.block = None
            records.append(record)
            if record.pc is not None:
                if record.pc in spans:
                    raise ValueError(f"duplicate source anchor 0x{record.pc:04X}")
                spans[record.pc] = (record, bounds.get(record.pc, record.pc + len(record.original or record.raw)))

    # The source labels are numeric provenance; .bind / code labels additionally
    # attach to record identity, so labels before newly inserted code follow it.
    bindings = {}
    for record in records:
        for name in record.labels:
            if name in bindings:
                raise ValueError(f"duplicate label binding: {name}")
            bindings[name] = record

    def binding(name, trail=()):
        if name in trail:
            raise ValueError(f"cyclic label alias: {' -> '.join(trail + (name,))}")
        if name in source.aliases:
            base, offset = source.aliases[name]
            item, addend = binding(base, trail + (name,))
            return item, addend + offset
        if name not in bindings:
            raise ValueError(f"unresolved label: {name}")
        return bindings[name], 0

    # Semantic fallthroughs make block placement independent of presentation
    # order. Only these explicit zero-cost edges may disappear when adjacent.
    for index in range(len(records) - 1, -1, -1):
        record = records[index]
        if record.kind != "edge":
            continue
        target_name = record.references[1]
        target, offset = binding(target_name)
        if index + 1 < len(records) and target is records[index + 1] and offset == 0 and record.alignment == target.alignment == 1:
            for name in record.labels:
                target.labels.append(name)
                bindings[name] = target
            records.pop(index)
        else:
            record.raw = b"\x01\0\0"
            record.kind = "op"

    original_records = {r.pc: r for r in records if r.pc is not None}
    extra_labels = {}

    def anchor_name(pc):
        name = next((name for name, value in source.labels.items() if value == pc), None)
        if name is None:
            name = f"__address_{pc:04X}"
            while name in source.labels or name in bindings:
                name += "_"
            extra_labels[name] = pc
        return name

    # Promote every known encoded address, including raw instructions and data
    # read bases. Positional numeric operands never evade relocation.
    immediate_dispatch = []
    relative_skips = []
    for record in records:
        if record.kind != "op" or record.raw == b"\xfe":
            continue
        instruction = decode_instruction(record.raw, 0)
        for offset in address_operand_offsets(instruction):
            if offset not in record.references:
                target = struct.unpack_from("<H", record.raw, offset)[0]
                record.references[offset] = anchor_name(target)
        original_opcode = (record.original or record.raw)[0]
        if record.jump_target is not None:
            if instruction.opcode != 0xA6 or not record.raw[2] & 0x80:
                raise ValueError("symbolic triplet skip requires immediate A6")
            immediate_dispatch.append((record, record.jump_target))
        elif record.pc is not None and original_opcode == instruction.opcode == 0xA6 and record.raw[2] & 0x80:
            count = struct.unpack_from("<H", record.raw, 1)[0] & 0x7FFF
            target = (record.pc + 3 + count * 3) & 0xFFFF
            immediate_dispatch.append((record, anchor_name(target)))
        if record.alternate is not None:
            if instruction.opcode not in {0x9A, 0xD4, 0xFC}:
                raise ValueError("otherwise target is only valid on a handler with an alternate landing")
            relative_skips.append((record, record.alternate))
        elif record.pc is not None and original_opcode == instruction.opcode and instruction.opcode in {0x9A, 0xD4, 0xFC}:
            # ResolveActorSelector (8009CDB4) returns a literal actor ID or
            # current actor for FB. Only FD..FF can resolve to absent (FF).
            may_skip = record.raw[1] in {0xFD, 0xFE, 0xFF}
            if instruction.opcode == 0x9A:
                may_skip = struct.unpack_from("<H", record.raw, 1)[0] <= 0x8000
            elif record.raw[1] == 0xFB and len(source.rows) > 255:
                may_skip = True
            if may_skip:
                relative_skips.append((record, record.pc + 6))

    # A6 always indexes physical three-byte slots. If the author edits a slot's
    # body or inserts code in it, move the body out of line and retain a jump
    # veneer in that slot. Unchanged tables require no extra instructions.
    islands = []
    table_reports = []
    for dispatch, entries in source.tables.items():
        owner = original_records.get(dispatch)
        if owner is None or owner.raw[:1] != b"\xa6" or owner.raw[2] & 0x80:
            continue
        if entries != list(range(dispatch + 3, dispatch + 3 + len(entries) * 3, 3)):
            raise ValueError(f"A6 at 0x{dispatch:04X}: table slots must be consecutive triplets")
        begin = records.index(owner) + 1
        end_pc = dispatch + 3 + len(entries) * 3
        end = begin
        next_anchors = {}
        next_pc = source.size
        for item in reversed(records):
            if item.pc is not None:
                next_pc = item.pc
            next_anchors[id(item)] = next_pc
        while end < len(records) and next_anchors[id(records[end])] < end_pc:
            end += 1
        table_records = records[begin:end]
        if len(table_records) == len(entries) and all(r.pc == pc and len(r.raw) == 3 for r, pc in zip(table_records, entries)):
            continue
        groups = [[] for _ in entries]
        slot = len(entries) - 1
        for record in reversed(table_records):
            if record.pc is not None:
                slot = (record.pc - dispatch - 3) // 3
            if not 0 <= slot < len(entries):
                raise ValueError("record outside computed table")
            groups[slot].insert(0, record)
        slots = []
        for index, (pc, body) in enumerate(zip(entries, groups)):
            if not body:
                # Removing a slot body makes that case continue at the next case.
                body = [Record(None, b"\x01\0\0", references={1: anchor_name(pc + 3)})]
                spans[pc] = (body[0], pc + 3)
                for name, value in source.labels.items():
                    if value == pc and name not in bindings:
                        bindings[name] = body[0]
            if len(body) == 1 and len(body[0].raw) == 3:
                slots.append(body[0])
                continue
            name = f"__table_{dispatch:04X}_{index}"
            while name in source.labels or name in bindings:
                name += "_"
            bindings[name] = body[0]
            slot_record = Record(None, b"\x01\0\0", references={1: name})
            slots.append(slot_record)
            # Entry labels must enter the body (including inserted instructions),
            # while the dynamic dispatcher enters its fixed-width veneer.
            last = body[-1]
            if last.kind == "op" and last.raw[0] not in {0x00, 0x01, 0x04, 0x0D, 0x5B, 0xD1, 0xE4}:
                body.append(Record(None, b"\x01\0\0", references={1: anchor_name(pc + 3)}))
            islands.extend(body)
        records[begin:end] = slots
        table_reports.append({"dispatch": dispatch, "slots": len(slots), "out_of_line_bytes": sum(len(r.raw) for r in islands)})

    records.extend(islands)
    prefix_noops = []
    for index, record in enumerate(records[:-1]):
        if record.kind == "op" and record.raw == b"\xfe" and records[index + 1].raw[0] not in {0, *range(0x78, 0x7F)}:
            # These FE slots consume one byte and do nothing else. If an edit
            # separates the prefix from its lookahead, use the equivalent primary
            # one-byte NOP instead of accidentally selecting a different FE op.
            record.raw = b"\x13"
            prefix_noops.append(record.pc)
    positions = {}

    def allocate():
        nonlocal records
        real = [record for record in records if record.kind != "padding"]
        estimates = {}
        cursor = 0
        for record in real:
            estimates[id(record)] = cursor
            cursor += len(record.raw)
        visited = set()
        for _ in range(258):
            cursor = 0
            placed = []
            positions.clear()
            forward = False
            for record in real:
                residue = record.residue
                if isinstance(residue, str):
                    anchor = bindings.get(residue)
                    if anchor is None:
                        anchor = original_records.get(source.labels.get(residue))
                    if anchor is None or id(anchor) not in estimates:
                        raise ValueError(f"unresolved alignment label: {residue}")
                    if id(anchor) not in positions:
                        forward = True
                    residue = positions.get(id(anchor), estimates[id(anchor)]) >> 8
                padding = (residue - cursor) % record.alignment
                if padding:
                    pad = Record(None, bytes(padding), "padding")
                    positions[id(pad)] = cursor
                    placed.append(pad)
                    cursor += padding
                positions[id(record)] = cursor
                placed.append(record)
                cursor += len(record.raw)
            updated = {id(record): positions[id(record)] for record in real}
            if not forward or updated == estimates:
                records = placed
                return checked(cursor, 1, 65536, "Field VM bytecode size")
            state = tuple(updated.values())
            if state in visited:
                break
            visited.add(state)
            estimates = updated
        raise ValueError("landing alignment constraints do not have a convergent layout")

    cursor = allocate()
    anchors = sorted(spans)

    def translate(pc):
        if pc == source.size:
            return cursor
        index = bisect.bisect_right(anchors, pc) - 1
        if index < 0:
            raise ValueError(f"reference to deleted source address 0x{pc:04X}")
        start = anchors[index]
        record, end = spans[start]
        offset = pc - start
        if not start <= pc < end or offset >= len(record.raw) or id(record) not in positions:
            raise ValueError(f"reference to deleted bytes at 0x{pc:04X}; bind the label to its replacement")
        return positions[id(record)] + offset

    skip_reports = []
    for record, target in relative_skips:
        if isinstance(target, str):
            item, offset = binding(target)
            target_pc = positions[id(item)] + offset
        else:
            target_pc = translate(target) if target < source.size else None
        if target_pc is None or target_pc == positions[id(record)] + 6:
            continue
        index = next(i for i, item in enumerate(records) if item is record)
        continuation = records[index + 1]
        normal_name = f"__continue_{positions[id(record)]:04X}"
        while normal_name in source.labels or normal_name in bindings:
            normal_name += "_"
        bindings[normal_name] = continuation
        skip_name = target if isinstance(target, str) else anchor_name(target)
        if record.raw[0] == 0x9A:
            records[index + 1:index + 1] = [
                Record(None, b"\x01\0\0", references={1: normal_name}),
                Record(None, b"\x01\0\0", references={1: skip_name}),
            ]
        else:
            # D4/FC have entries one byte apart (+5 and +6). Two overlapping
            # jumps encode both paths without scratch VM variables or call-stack
            # changes: 01 01 HH LL -> normal HH01 / alternate LLHH.
            normal_pad = normal_name + "_pad"
            alternate_pad = normal_name + "_alternate_pad"
            first = Record(None, b"\x01\0\0", references={1: normal_name}, alignment=256, residue=1)
            second = Record(None, b"\x01\0\0", references={1: skip_name}, alignment=256, residue=normal_pad)
            bindings[normal_pad] = first
            bindings[alternate_pad] = second
            records.insert(index + 1, Record(None, bytes([1, 1, 0, 0]), "data", references={1: normal_pad, 2: alternate_pad}))
            records.extend((first, second))
        skip_reports.append({"source": record.pc, "block": record.source_block, "line": record.line, "opcode": record.raw[0], "alternate_source": target})
        cursor = allocate()

    labels = {}
    for name, pc in (source.labels | extra_labels).items():
        if name in source.aliases:
            item, offset = binding(name)
            labels[name] = positions[id(item)] + offset
        elif name in bindings:
            labels[name] = positions[id(bindings[name])]
        elif pc is not None:
            labels[name] = translate(pc)
        else:
            raise ValueError(f"unresolved label: {name}")
    for name, record in bindings.items():
        labels[name] = positions[id(record)]
    for record, name in immediate_dispatch:
        if name not in labels:
            raise ValueError(f"unresolved triplet skip label: {name}")
        count = ((labels[name] - positions[id(record)] - 3) * 43691) & 0xFFFF
        if count <= 32767:
            record.raw = b"\xa6" + struct.pack("<H", 0x8000 | count)
        else:
            # Evaluated immediate A6 has exactly the same effect as one jump.
            record.raw = b"\x01\0\0"
            record.references = {1: name}

    result = Assembly(source.bitmap, cursor, copy.deepcopy(source.rows), labels)
    address_map = []
    for record in records:
        new_pc = positions[id(record)]
        if record.pc is not None:
            address_map.append({"source": record.pc, "linked": new_pc, "size": len(record.raw)})
        record.pc = new_pc
        record.labels = []
        record.original = None
        record.alternate = None
        record.jump_target = None
        result.records.append(record)
    result.link_report = source.link_report | {"source_stage": "linked", "layout": "relocate", "source_size": None if source.link_report.get("source_stage") == "clean" else source.size, "bytecode_size": cursor, "address_map": address_map, "symbols": labels, "source_map": [{"block": record.source_block, "line": record.line, "pc": record.pc, "size": len(record.raw)} for record in result.records if record.line], "computed_tables": table_reports, "relative_landing_pads": skip_reports, "prefix_noops": prefix_noops}
    # Validate before exposing a partially linked result or writing any output.
    result.build("exact")
    return result
