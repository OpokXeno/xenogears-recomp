"""Standalone source operations lowered to native VM instructions and private data.

The buffers in this module are compiler-owned layouts, never provenance. They
are constructed from visible operands, including variable references. Source
labels describe continuations, not required physical displacements.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from functools import lru_cache

from field_instruction_codec import InstructionForm, append_named_operands, form_pattern, normalized


SCRIPT_IMAGE_MAGIC = b"XGSD\x01\xa5\x5a\x00"


def script_images(bytecode):
    """Locate compiler-framed, immutable script-as-data images."""
    result = {}
    at = 0
    while True:
        at = bytecode.find(SCRIPT_IMAGE_MAGIC, at)
        if at < 0:
            return result
        body = at + 12
        if body <= len(bytecode):
            size = int.from_bytes(bytecode[at + 8:body], "little")
            if 0 < size <= len(bytecode) - body:
                result[body] = (at, body + size)
                at = body + size
                continue
        at += 1


def recover_lowerings(bytecode, instructions, entries):
    """Recognize native operand islands so repeated decompilation stays clean.

    Only hide islands whose interior has no independent executable entry.
    Script-as-data references are lifted separately into immutable images.
    """
    incoming = set(entries) | {target for ins in instructions.values() for target in ins.targets}
    recovered = {}
    covered = set()

    def jump(at, target):
        ins = instructions.get(at)
        return ins is not None and ins.opcode == 1 and ins.targets == (target,)

    def accept(root, native, end, terminal=False):
        if not 0 <= root < end <= len(bytecode) or any(at in covered for at in range(root, end)):
            return
        if any(root < at < end and at != native for at in incoming):
            return
        recovered[root] = (native, end, terminal)
        covered.update(range(root, end))

    for pc, ins in sorted(instructions.items()):
        raw, sub = ins.raw, ins.subopcode
        length = end = None
        if sub == 0x5C and raw[2] == 1:
            length, end = 3, pc + 8
        elif sub == 0x77 and raw[2] == 0:
            length, end = 3, pc + 15
        elif sub == 0xD7:
            length, end = 7, pc + 11
        elif sub == 0x6C:
            if pc + 2 < len(bytecode) and bytecode[pc + 2] == 0 and pc + 2 not in incoming:
                accept(pc, pc, pc + 3, True)
            else:
                length, end = 2, pc + 5
        elif sub == 0x5C and raw[2] > 2:
            length, end = 4, pc + 7
        if length is not None and jump(pc + length, end):
            accept(pc, pc, end)
        if sub is None and ins.opcode == 0x11 and raw[1] == 0 and pc + 17 <= len(bytecode):
            gate = bytecode[pc + 9:pc + 17]
            if (gate[0] == 2 and gate[1] == (gate[3] ^ 1) and gate[4:6] == b"\0\xc0"
                    and int.from_bytes(gate[6:8], "little") == pc + 17):
                accept(pc, pc, pc + 17)
        if sub is None and ((ins.opcode in {0x10, 0x11} and raw[1] != 0)
                            or (ins.opcode == 0x57 and len(raw) == 2 and raw[1] != 15)):
            size = 11 if ins.opcode == 0x57 else 9
            root = pc - size - 3
            end = pc + len(raw)
            if ins.opcode == 0x11:
                end = pc + 13
                if not jump(pc + 4, end):
                    continue
            if root >= 0 and jump(root, pc):
                accept(root, pc, end)
    return recovered


@dataclass
class SemanticForm:
    kind: str
    form: InstructionForm
    seed: bytes
    terminal: bool = False
    complete: bool = False


@lru_cache(maxsize=1)
def semantic_forms() -> tuple[SemanticForm, ...]:
    forms = []

    def add(kind, name, seed, inputs=(), *, terminal=False, suffix=""):
        form = InstructionForm()
        args = [form.operand(*field) for field in inputs]
        form.template = f"{name}({', '.join(args)}{suffix});"
        item = SemanticForm(kind, form, bytes.fromhex(seed), terminal)
        forms.append(item)
        return item

    add("mecha_load", "actor.load_current_actor_mecha", "FE 5C 01 00 00 00 00 80",
        (("v80", 6),)).form.template = "actor.load_current_actor_mecha(1, {0});"
    add("image_load", "event.manage_overlay_image_asset", "FE 77 00 " + "00 " * 11 + "80",
        (("masked", 6, 14, 0x80),)).form.template = "event.manage_overlay_image_asset(0, {0});"
    add("world_marker", "movement.set_world_map_marker_position_xz", "FE D7 " + "00 " * 8 + "C0",
        (("masked", 2, 10, 0x80), ("masked", 4, 10, 0x40)))
    add("controller_stop", "event.clear_controller_enable_flag_and_stop", "FE 6C 00", terminal=True)
    add("controller_keep", "event.keep_controller_enable_flag", "FE 6C")
    add("mecha_reinterpret", "actor.load_current_actor_mecha_reinterpret", "FE 5C 03 80",
        (("v80", 2),))
    party = add("party_stage", "actor.add_immediate_party_character", "FE 18 00 00 00 00 00",
                (("u8", 2), ("label", 3), ("label", 5)), terminal=True)
    party.form.template = "actor.add_immediate_party_character({0}) staged goto {1} already_present goto {2};"
    xyz9 = (("masked", 2, 8, 0x80), ("masked", 4, 8, 0x40), ("masked", 6, 8, 0x20))
    add("move_begin", "movement.begin_actor_move", "10 00 " + "00 " * 6 + "E0", xyz9)
    add("move_finish", "movement.continue_actor_move", "10 00 " + "00 " * 6 + "E0 10 01", xyz9)
    add("bounded_begin", "movement.begin_actor_move_with_step_limit", "11 00 " + "00 " * 15,
        xyz9 + (("v80", 11),))
    add("bounded_finish", "movement.continue_actor_move_with_step_limit", "11 00 " + "00 " * 20,
        xyz9 + (("v80", 20),))
    for use_walkmesh in (False, True):
        add("ballistic_finish", "movement.finish_ballistic_actor_move",
            f"57 {'80' if use_walkmesh else '00'} " + "00 " * 9 + "57 03",
            (("masked", 2, 10, 0x80), ("masked", 4, 10, 0x40), ("masked", 6, 10, 0x20)),
            suffix=", " + str(use_walkmesh).lower())
    native_fields = {
        'move_begin': [('reserved_flags', 'bits', 8, 0, 0x1F)],
        'bounded_begin': [('reserved_flags', 'bits', 8, 0, 0x1F)],
        'move_finish': [('phase', 'u8', 10, 0, 0)],
        'bounded_finish': [('phase', 'u8', 10, 0, 0), ('initial_limit', 'v80', 11, 0, 0)],
        'ballistic_finish': [('phase', 'u8', 12, 0, 0)],
        'world_marker': [('reserved', 'u8', 6, 0, 0)],
    }
    for item in list(forms):
        if item.kind in native_fields:
            complete = InstructionForm(item.form.template, list(item.form.operands))
            append_named_operands(complete, native_fields[item.kind])
            forms.append(SemanticForm(item.kind, complete, item.seed, item.terminal, True))
    return tuple(forms)


def encode_semantic(statement, symbols, labels, *, provisional=False):
    """Return (kind, buffer, relocations, terminal), or None for a regular op."""
    variables = {symbol.reference: offset for offset, symbol in symbols.items()}
    for item in sorted(semantic_forms(), key=lambda item: not item.complete):
        match = form_pattern(item.form.template, len(item.form.operands)).fullmatch(normalized(statement))
        if match is None:
            continue
        raw = item.form.encode(statement, item.seed, variables, dict.fromkeys(labels, 0))
        if item.complete:
            if item.kind in {'move_finish', 'bounded_finish'} and raw[10] == 0:
                raise ValueError('continuation phase must be nonzero')
            if item.kind == 'ballistic_finish' and (raw[12] & 3 != 3 or raw[12] == 15):
                raise ValueError('ballistic continuation phase must select completion')
        if item.kind == "mecha_reinterpret" and raw[2] <= 2 and not provisional:
            raise ValueError("reinterpretation requires a party selector whose encoded low byte exceeds 2")
        references = {operand.offset: match[f"f{index}"] for index, operand in enumerate(item.form.operands)
                      if operand.kind == "label"}
        return item.kind, raw, references, item.terminal
    return None


def render_semantic(instruction, bytecode, symbols, branches=None):
    """Lift neighboring encoded words into independent logical arguments."""
    op, sub, pc, raw = instruction.opcode, instruction.subopcode, instruction.pc, instruction.raw
    if sub is None and op == 0x73 and raw[1] > 1:
        return "stall_forever;", "stall", True
    kind = None
    if sub == 0x18:
        kind = "party_stage"
    elif sub == 0x5C and raw[2] == 1:
        kind = "mecha_load"
    elif sub == 0x5C and raw[2] > 2:
        kind = "mecha_reinterpret"
    elif sub == 0x77 and raw[2] == 0:
        kind = "image_load"
    elif sub == 0xD7:
        kind = "world_marker"
    elif sub == 0x6C:
        if pc + 2 >= len(bytecode):
            raise ValueError("controller operation reads beyond the script")
        kind = "controller_stop" if bytecode[pc + 2] == 0 else "controller_keep"
    elif sub is None and op in {0x10, 0x11}:
        kind = ("move_" if op == 0x10 else "bounded_") + ("begin" if raw[1] == 0 else "finish")
    elif sub is None and op == 0x57 and len(raw) == 2 and raw[1] != 15:
        kind = "ballistic_finish"
    if kind is None:
        return None

    def read(offset, length):
        at = pc + offset
        if not 0 <= at <= len(bytecode) - length:
            raise ValueError(f"{instruction.name} at 0x{pc:04X} reads outside the script")
        return bytecode[at:at + length]

    item = next(item for item in semantic_forms() if item.kind == kind)
    buffer = bytearray(item.seed)
    if kind == "party_stage":
        buffer[2] = raw[2]
        struct.pack_into("<HH", buffer, 3, *(branches or (pc + 3, pc + 5)))
    elif kind == "mecha_load":
        buffer[6:8] = read(6, 2)
    elif kind == "mecha_reinterpret":
        buffer[2:4] = read(2, 2)
    elif kind == "image_load":
        buffer[6:8], buffer[14] = read(6, 2), read(14, 1)[0]
    elif kind == "world_marker":
        buffer[2:7], buffer[10] = raw[2:7], read(10, 1)[0]
    elif kind in {"move_begin", "bounded_begin"}:
        buffer[:9] = read(0, 9)
        if kind == "bounded_begin":
            buffer[11:13] = read(11, 2)
    elif kind in {"move_finish", "bounded_finish"}:
        buffer[:9] = read(-9, 9)
        buffer[9:9 + len(raw)] = raw
        if kind == "bounded_finish":
            if pc + 13 <= len(bytecode):
                buffer[20:22] = read(11, 2)
            elif buffer[:2] == b"\x11\0":
                # The normal paired initializer already read this limit from
                # continuation+2. Its continuation never takes the uninitialized
                # counter path, so no out-of-resource fallback word is needed.
                buffer[20:22] = read(2, 2)
            else:
                raise ValueError("unpaired bounded continuation has no readable fallback limit")
    elif kind == "ballistic_finish":
        buffer[:11] = read(-11, 11)
        buffer[11:13] = raw
        # Both forms share a buffer layout but expose a genuine boolean input.
        item = next(item for item in semantic_forms() if item.kind == kind
                    and bool(item.seed[1] & 0x80) == bool(buffer[1] & 0x80))
    # Add native format fields only when the concise spelling would lose them.
    # These are operands of this instruction, never bytes copied from neighbors.
    use_complete = ((kind in {'move_begin', 'bounded_begin'} and buffer[8] & 0x1F)
                    or (kind == 'world_marker' and buffer[6])
                    or (kind == 'move_finish' and buffer[10] != 1)
                    or (kind == 'bounded_finish' and buffer[9:13] != b'\x11\x01\0\0')
                    or (kind == 'ballistic_finish' and buffer[12] != 3))
    if use_complete:
        item = next(form for form in semantic_forms() if form.kind == kind and form.complete
                    and (kind != 'ballistic_finish' or bool(form.seed[1] & 0x80) == bool(buffer[1] & 0x80)))
    return item.form.render(buffer, symbols), kind, item.terminal


def _compact_encoding(source, allow_initialized=False):
    raw, kind = source.raw, source.semantic

    def word(offset, start):
        return ("bytes", offset, raw[start:start + 2], b"\xff\xff")

    def mask(offset, start, bits):
        return ("bytes", offset, bytes((raw[start] & bits,)), bytes((bits,)))

    if kind == "move_begin":
        return raw[:9], ()
    if kind == "bounded_begin":
        return raw[:9], (word(11, 11),)
    if kind in {"move_finish", "bounded_finish", "ballistic_finish"}:
        size = 11 if kind == "ballistic_finish" else 9
        control = size - 1
        requirements = [word(2 - size, 2), word(4 - size, 4), word(6 - size, 6), mask(-1, control, 0xE0)]
        opcode = {"move_finish": 0x10, "bounded_finish": 0x11, "ballistic_finish": 0x57}[kind]
        mode = raw[size + 1]
        if (kind == "ballistic_finish" and (mode & 3 != 3 or mode == 15)) or not mode:
            mode = 3 if kind == "ballistic_finish" else 1
        native = bytes((opcode, mode))
        if kind == "ballistic_finish":
            requirements.append(mask(1 - size, 1, 0x80))
        if kind == "bounded_finish":
            native += raw[11:13]
            requirements.append(("limit", 11, raw[20:22], allow_initialized))
        return native, tuple(requirements)
    if kind == "mecha_load":
        return raw[:3], (word(6, 6),)
    if kind == "image_load":
        return raw[:3], (word(6, 6), mask(14, 14, 0x80))
    if kind == "world_marker":
        return raw[:7], (mask(10, 10, 0xC0),)
    if kind == "party_stage":
        return raw[:3], (("label", 3, source.references[3]), ("label", 5, source.references[5]))
    if kind == "controller_stop":
        return raw[:2], (("bytes", 2, b"\0", b"\xff"),)
    if kind == "controller_keep":
        return raw[:2], (("nonzero", 2),)
    if kind == "mecha_reinterpret":
        return raw[:4], ()
    return None


def lower_semantic_records(records, *, disabled=frozenset(), externally_entered=frozenset()):
    """Expand compiler IR macros, retaining the native handler state machines.

    Jumps over private operand data use ordinary relocations. The two-entry
    party gate overlaps an unconditional jump with an always-taken conditional
    branch, so neither continuation has an address/alignment restriction.
    """
    from compile_field_scripts import Record

    result = []
    serial = 0
    for source in records:
        if source.kind != "semantic":
            result.append(source)
            continue
        serial += 1
        end = f"__xgs_native_{serial}_end"
        resume = f"__xgs_native_{serial}_resume"
        raw, kind = source.raw, source.semantic
        children = []

        def emit(data, category="op", *, refs=None, names=(), guard=None):
            record = Record(None, bytes(data), category, references=refs or {},
                            line=source.line, block=source.block, labels=list(names),
                            source_block=source.source_block, guard=guard, site=source.site)
            children.append(record)
            return record

        def jump(target):
            emit(b"\x01\0\0", refs={1: target})

        def mark_end():
            emit(b"", "mark", names=(end,))

        candidate = _compact_encoding(source, source.site not in externally_entered)
        if source.site not in disabled and candidate is not None:
            native, requirements = candidate
            record = emit(native)
            record.constraints = requirements
            record.labels[:0] = source.labels
            record.alignment, record.residue = source.alignment, source.residue
            result.extend(children)
            continue

        if kind == "party_stage":
            emit(raw[:3])
            # Entry +3: if (0x01xx != already_present) jump staged.
            # Entry +5: unconditional jump already_present. The guard byte is
            # selected after relocation to make the comparison always unequal.
            emit(bytes.fromhex("02 00 01 00 00 C0 00 00"), "data",
                 refs={3: source.references[5], 6: source.references[3]}, guard=source.references[5])
        elif kind == "controller_keep":
            emit(b"\x13")  # No state effect; preserve one advancing VM dispatch.
        elif kind in {"mecha_load", "image_load", "world_marker"}:
            length = {"mecha_load": 3, "image_load": 3, "world_marker": 7}[kind]
            emit(raw[:length])
            jump(end)
            if kind == "mecha_load":
                emit(raw[6:8], "data")
            elif kind == "image_load":
                emit(raw[6:15], "data")
            elif kind == "world_marker":
                emit(raw[10:11], "data")
            mark_end()
        elif kind == "controller_stop":
            emit(raw[:2])
            emit(b"\0")
        elif kind == "mecha_reinterpret":
            # The FE escape and the reinterpreted primary 5C share bytes.
            # Model their complete four-byte footprint as one physical record.
            emit(raw[:4])
            jump(end)
            mark_end()
        elif kind == "move_begin":
            emit(raw)
        elif kind == "bounded_begin":
            emit(raw[:9])
            lo, hi = raw[11:13]
            emit(bytes((2, hi ^ 1, lo, hi, 0, 0xC0, 0, 0)), "data", refs={6: end})
            mark_end()
        elif kind in {"move_finish", "bounded_finish", "ballistic_finish"}:
            jump(resume)
            size = 11 if kind == "ballistic_finish" else 9
            emit(raw[:size], "data")
            continuation = {"move_finish": b"\x10\x01", "bounded_finish": b"\x11\x01\0\0",
                            "ballistic_finish": b"\x57\x03"}[kind]
            if candidate is not None:
                continuation = candidate[0]
            emit(continuation, names=(resume,))
            if kind == "bounded_finish":
                jump(end)
                emit(bytes(4) + raw[20:22], "data")
                mark_end()
        else:
            raise ValueError(f"unknown semantic lowering: {kind}")
        children[0].labels[:0] = source.labels
        children[0].alignment, children[0].residue = source.alignment, source.residue
        result.extend(children)

    # Zero-size end marks bind the next record; they are not bytecode or NOPs.
    for index in range(len(result) - 1, -1, -1):
        mark = result[index]
        if mark.kind != "mark":
            continue
        if index + 1 < len(result) and result[index + 1].block == mark.block:
            result[index + 1].labels[:0] = mark.labels
            result.pop(index)
        else:
            # Only reachable for an explicitly unreachable block end.
            mark.kind, mark.raw = "op", b"\x13"
    return result
