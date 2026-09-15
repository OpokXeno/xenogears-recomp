"""Reversible operand forms shared by the Field DSL renderer and compiler.

An encoding selects the opcode, mode and otherwise invisible control bits.
Forms describe only fields actually displayed by the DSL. Unclassified fields
are encoded bytes, not guessed semantic arguments.
"""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass, field
from functools import lru_cache


# Complete input schemas verified against FieldReadEvaluatedOperand (800ACDEC)
# call sites in field-overlay.bin. In these words, bit 15 selects an immediate;
# it is not another argument. Roles are also used for variable discovery.
EVALUATED_OPERATION_INPUTS = {
    (0x0B, None): ((1, "field_graphic"),),                # 800A1624
    (0x21, None): ((1, "actor_movement_speed"),),         # 8009E094
    (0x69, None): ((1, "actor_rotation_sector"),),        # 8009AC7C
    (0x71, None): ((1, "battle_configuration"),),         # 80093568
    (0x72, None): ((1, "music_id"),),                     # 8008F724 -> 8008F7B8
    (0x74, None): ((1, "sound_effect_id"),),              # 8008F668
    (0x75, None): ((1, "music_id"),),                     # 8008F76C -> 8008F7B8
    (0x8C, None): ((1, "inventory_object"),),             # 8009631C
    (0x8D, None): ((1, "inventory_object"),),             # 8009640C
    (0x9A, None): ((1, "camera_reacquisition_duration"),), # 8008FC4C
    (0xA0, None): (                                      # 8009BA7C
        (1, "camera_direction"),
        (3, "screen_geometry_parameter_2"),
        (5, "screen_geometry_parameter_3"),
    ),
}

# All these handlers read the destination with 800ACDB8(1) and the source
# through 8009CFBC(3, control). Only source-control bit 0x40 is observed.
# Verified: 8009D9A4, 8009D890, 8009D804, 8009D644, 8009D408, 8009D5B8,
# 8009D52C, 8009D4A0, 8009D6D8 and 8009D768.
MASKED_VARIABLE_OPCODES = frozenset({0x35, 0x38, 0x39, 0x3A, 0x3B, 0x3E, 0x3F, 0x40, 0xDE, 0xDF})


def integer(text: str) -> int:
    return int(text, 16 if text.lower().lstrip("-").startswith("0x") else 10)


def checked(value: int, low: int, high: int, description: str) -> int:
    if not low <= value <= high:
        raise ValueError(f"{description} must be in {low}..{high}, got {value}")
    return value


def normalized(text: str) -> str:
    return re.sub(r"\s+", "", text)


def equivalent_instruction_encoding(left: bytes | None, right: bytes) -> bool:
    """Allow only independently verified, behavior-neutral encoding differences.

    The masked variable family reads its destination directly and passes its
    control byte to 8009CFBC. That helper tests only bit 0x40; every other bit
    is ignored. Opcode, operand values and immediate/variable mode must match.
    """
    if left == right:
        return True
    return (
        left is not None
        and len(left) == len(right) == 6
        and left[0] == right[0]
        and left[0] in MASKED_VARIABLE_OPCODES
        and left[:5] == right[:5]
        and (left[5] & 0x40) == (right[5] & 0x40)
    )


@lru_cache(maxsize=2048)
def form_pattern(template: str, count: int) -> re.Pattern:
    pattern = re.escape(normalized(template))
    for index in range(count):
        pattern = pattern.replace(re.escape("{" + str(index) + "}"), f"(?P<f{index}>.+?)")
    return re.compile(pattern)


@dataclass(frozen=True)
class Operand:
    kind: str
    offset: int
    control: int = 0
    mask: int = 0

    def read(self, raw: bytes, symbols: dict) -> str:
        from decompile_field_scripts import _actor, _variable

        if self.kind == "fixedvar":
            return _variable(self.offset, symbols)
        value = raw[self.offset] if self.kind in {"u8", "actor", "routine", "priority"} else struct.unpack_from("<H", raw, self.offset)[0]
        if self.kind == "label":
            return f"L_{value:04X}"
        if self.kind == "actor":
            return _actor(value)
        if self.kind == "routine":
            return str(value & 31)
        if self.kind == "priority":
            return str(value >> 5)
        if self.kind == "packedvar":
            return _variable(value >> 4, symbols)
        if self.kind == "bit":
            return str(value & 15)
        if self.kind == "var":
            return _variable(value, symbols)
        if self.kind == "v80":
            return str(value & 0x7FFF) if value & 0x8000 else _variable(value, symbols)
        if self.kind == "masked" and not raw[self.control] & self.mask:
            return _variable(value, symbols)
        if self.kind in {"s16", "masked"} and value & 0x8000:
            value -= 0x10000
        return str(value)

    def write(self, raw: bytearray, text: str, variables: dict[str, int], labels: dict[str, int]) -> None:
        kind = self.kind
        if kind == "fixedvar":
            if variables.get(text) != self.offset:
                raise ValueError(f"implicit output must name the declared VM slot at 0x{self.offset:04X}")
            return
        if kind == "label":
            if text not in labels:
                raise ValueError(f"unresolved label: {text}")
            value = checked(labels[text], 0, 65535, "target offset")
        elif kind == "actor":
            special = {"self": 0xFB, "party[0]": 0xFF, "party[1]": 0xFE, "party[2]": 0xFD}
            if text in special:
                value = special[text]
            else:
                match = re.fullmatch(r"actor\[(\d+)\]", text)
                if not match:
                    raise ValueError(f"invalid actor selector: {text}")
                value = checked(int(match[1]), 0, 255, "actor selector")
        elif kind in {"var", "packedvar"} or (kind in {"v80", "masked"} and text in variables):
            if text not in variables:
                raise ValueError(f"unresolved state symbol: {text}")
            value = checked(variables[text], 0, 0xFFFF, "variable offset")
            if kind == "v80":
                checked(value, 0, 0x7FFF, "v80 variable offset")
            if kind == "masked":
                raw[self.control] &= ~self.mask
            if kind == "packedvar":
                checked(value, 0, 0xFFF, "packed variable offset")
                value = (value << 4) | (raw[self.offset] & 15)
        else:
            try:
                value = integer(text)
            except ValueError as error:
                raise ValueError(f"expected integer or declared state symbol, got {text}") from error
            if kind == "v80":
                value = checked(value, 0, 32767, "v80 immediate") | 0x8000
            elif kind in {"masked", "s16"}:
                value = checked(value, -32768, 32767, "signed immediate") & 0xFFFF
                if kind == "masked":
                    raw[self.control] |= self.mask
            elif kind == "bit":
                value = (struct.unpack_from("<H", raw, self.offset)[0] & 0xFFF0) | checked(value, 0, 15, "bit index")
            elif kind == "routine":
                value = (raw[self.offset] & 0xE0) | checked(value, 0, 31, "routine ID")
            elif kind == "priority":
                value = (raw[self.offset] & 31) | (checked(value, 0, 7, "priority") << 5)
            else:
                checked(value, 0, 255 if kind == "u8" else 65535, kind)
        if kind in {"u8", "actor", "routine", "priority"}:
            raw[self.offset] = value
        else:
            struct.pack_into("<H", raw, self.offset, value)


@dataclass
class InstructionForm:
    template: str = ""
    operands: list[Operand] = field(default_factory=list)

    def operand(self, kind: str, offset: int, control: int = 0, mask: int = 0) -> str:
        self.operands.append(Operand(kind, offset, control, mask))
        return "{" + str(len(self.operands) - 1) + "}"

    def render(self, raw: bytes, symbols: dict) -> str:
        return self.template.format(*(operand.read(raw, symbols) for operand in self.operands))

    def encode(self, statement: str, seed: bytes, variables: dict, labels: dict) -> bytes | None:
        match = form_pattern(self.template, len(self.operands)).fullmatch(normalized(statement))
        if match is None:
            return None
        raw = bytearray(seed)
        for index, operand in enumerate(self.operands):
            operand.write(raw, match[f"f{index}"], variables, labels)
        return bytes(raw)


def instruction_form(instruction, symbols: dict) -> InstructionForm:
    # Imports are local so that decoding, symbol discovery and this codec can
    # share the existing dispatch inventory without an import cycle.
    from decompile_field_scripts import (
        CONDITION_OPERATORS, CONDITIONAL_OUTPUT_VARIABLES, IMPLICIT_OUTPUT_VARIABLES,
        _encoded_outputs, _operation_name, target_operand_offset,
    )

    raw, op, sub = instruction.raw, instruction.opcode, instruction.subopcode
    form = InstructionForm()
    arg = form.operand
    name = _operation_name(instruction)
    start = 2 if sub is not None else 1
    key = (op, sub)
    outputs = _encoded_outputs(instruction)
    implicit = IMPLICIT_OUTPUT_VARIABLES.get(key, ())

    def generic() -> str:
        target = target_operand_offset(op, sub, len(raw))
        arguments = []
        for i in range(start, len(raw)):
            if i == target:
                arguments.append(arg("label", i))
            elif target is None or i != target + 1:
                arguments.append(arg("u8", i))
        return ", ".join(arguments)

    if sub is not None and len(raw) == 1:
        # These dispatch slots advance over FE only. Their identity comes from
        # the following byte, which belongs to a different instruction/region.
        form.template = 'raw("FE");'
    elif outputs or implicit:
        destinations = [arg("var", offset) for offset, _ in outputs]
        destinations.extend(arg("fixedvar", offset) for offset, _ in implicit)
        if sub == 0xC1:
            arguments = arg("v80", 6)
        elif sub == 0x38:
            arguments = f"[{arg('actor', 4)}, {arg('actor', 5)}]"
        elif sub == 0x69:
            arguments = arg("v80", 4)
        elif sub == 0xC7:
            arguments = arg("v80", 2)
        elif sub == 0x75:
            arguments = arg("actor", 2)
        elif op == 0x34:
            arguments = arg("v80", 1)
        elif op in {0x48, 0x49}:
            if op == 0x49:
                name = "state.read_script_s16" if raw[7] else "state.read_script_u16"
            arguments = f"{arg('label', 1)}, {arg('v80', 5)}"
        elif op == 0x2D:
            arguments = arg("actor", 1)
        elif op == 0xA8:
            arguments = arg("v80", 3)
        elif op == 0x87:
            arguments = arg("v80", 1)
        elif op == 0x94:
            arguments = f"{arg('v80', 1)}, {arg('v80', 3)}"
        elif sub == 0x56:
            arguments = arg("v80", 2)
        elif key in CONDITIONAL_OUTPUT_VARIABLES:
            arguments = ""
        else:
            positions = {p for offset, _ in outputs for p in (offset, offset + 1)}
            arguments = ", ".join(arg("u8", i) for i in range(start, len(raw)) if i not in positions)
        form.template = f"{name}({arguments}) -> ({', '.join(destinations)});"
    elif sub == 0x0D:
        form.template = f"dialogue.set_portrait({arg('v80', 2)});"
    elif sub in {0x0A, 0x0B}:
        operator = "|= " if sub == 0x0A else "&= ~"
        form.template = f"{arg('packedvar', 2)} {operator}(1 << {arg('bit', 2)});"
    elif key in EVALUATED_OPERATION_INPUTS:
        arguments = ", ".join(arg("v80", offset) for offset, _ in EVALUATED_OPERATION_INPUTS[key])
        form.template = f"{name}({arguments});"
    elif sub is not None:
        form.template = f"{name}({generic()});"
    elif op == 0x5B:
        form.template = "movement.park_actor_movement_update();"
    elif op in {0x00, 0x0D, 0x13, 0xFD, 0xFF, 0xD1, 0xE4}:
        form.template = {0x00: "stop;", 0x0D: "return;", 0x13: "nop;", 0xFD: "nop;", 0xFF: "nop;"}.get(op, "stall_forever;")
    elif op in {0x01, 0x05, 0x06}:
        verb = "goto" if op == 0x01 else "call"
        inline = f" inline {arg('u16', 3)}" if op == 0x06 else ""
        form.template = f"{verb} {arg('label', 1)}{inline};"
    elif op == 0x02:
        lhs, rhs = arg("masked", 1, 5, 0x80), arg("masked", 3, 5, 0x40)
        condition = raw[5] & 15
        if condition in {6, 9}:
            test = f"({lhs} & {rhs}) != 0"
        elif condition == 8:
            test = f"({lhs} | {rhs}) != 0"
        elif condition == 10:
            test = f"((~{lhs}) & {rhs}) != 0"
        else:
            test = f"{lhs} {CONDITION_OPERATORS.get(condition, f'cond_{condition:X}')} {rhs}"
        form.template = f"if (!({test})) goto {arg('label', 6)};"
    elif op in {0x07, 0x08, 0x09}:
        mode = {7: "async", 8: "wait", 9: "wait_extended"}[op]
        form.template = f"start {arg('actor', 1)}.routine[{arg('routine', 2)}] priority {arg('priority', 2)} {mode};"
    elif op in {0x0A, 0xCC, 0xC9, 0xCB}:
        dimension = "3d" if op in {0xCC, 0xCB} else "2d"
        negation, verb = ("!", "goto") if op in {0xC9, 0xCB} else ("", "call")
        form.template = f"if ({negation}inside_trigger_{dimension}({arg('u8', 1)})) {verb} {arg('label', 2)};"
    elif op in {0x0C, 0xA7}:
        suffix = "_preserve_ip" if op == 0x0C else ""
        form.template = f"actor.process_player_control_if_owned{suffix}();"
    elif op in {0x14, 0x15, 0x22, 0x23, 0x2A, 0x2B}:
        prop = "world.encounters.enabled" if op in {0x14, 0x15} else "actor.self.visible" if op in {0x22, 0x23} else "actor.self.dialogue_enabled"
        form.template = f"{prop} = {'true' if op in {0x15, 0x22, 0x2B} else 'false'};"
    elif op in {0x16, 0x5C}:
        form.template = f"actor.bind_{'playable_character' if op == 0x16 else 'party_slot'}({arg('v80', 1)});"
    elif op in {0x24, 0x25}:
        form.template = f"{arg('actor', 1)}.visible = {'true' if op == 0x24 else 'false'};"
    elif op == 0x26:
        form.template = f"flow.sleep({arg('v80', 1)});"
    elif op in {0x31, 0x32}:
        prop = "held" if op == 0x31 else "accumulated"
        form.template = f"if ((input.{prop} & {arg('u16', 1)}) == 0) goto {arg('label', 3)};"
    elif op == 0x33:
        form.template = "input.accumulated = 0;"
    elif op in MASKED_VARIABLE_OPCODES:
        operator = {0x35: "=", 0x38: "+=", 0x39: "-=", 0x3A: "|= 1 <<", 0x3B: "&= ~(1 <<", 0x3E: "&=", 0x3F: "|=", 0x40: "^=", 0xDE: "*=", 0xDF: "/="}[op]
        form.template = f"{arg('var', 1)} {operator} {arg('masked', 3, 5, 0x40)}{')' if op == 0x3B else ''};"
    elif op in {0x36, 0x37}:
        form.template = f"{arg('var', 1)} = {'true' if op == 0x36 else 'false'};"
    elif op in {0x3C, 0x3D}:
        form.template = f"{arg('var', 1)}{'++' if op == 0x3C else '--'};"
    elif op in {0x41, 0x42}:
        form.template = f"{arg('var', 1)} {'<<=' if op == 0x41 else '>>='} {arg('v80', 3)};"
    elif op == 0xDC:
        form.template = f"state.swap({arg('var', 1)}, {arg('var', 3)});"
    elif op in {0xAF, 0xB0, 0xB1}:
        prop = {0xAF: "yaw", 0xB0: "projection_dip", 0xB1: "projection_depth"}[op]
        form.template = f"camera.{prop} = {arg('u16' if op == 0xB1 else 's16', 1)};"
    elif op == 0xA6:
        form.template = f"flow.dispatch_triplet_table({arg('v80', 1)});"
    else:
        form.template = f"{name}({generic()});"
    return form


@lru_cache(maxsize=1)
def instruction_seeds() -> tuple[bytes, ...]:
    """Canonical encodings for every distinct opcode/mode form, not name guesses."""
    from decompile_field_scripts import decode_instruction

    result = []
    for opcode in range(256):
        if opcode == 0xFE:
            continue
        for mode in range(4) if opcode in {0x10, 0x49, 0x57, 0x73, 0xAF, 0xB0, 0xB1} else (0,):
            raw = bytearray(bytes([opcode]) + bytes(31))
            raw[7 if opcode == 0x49 else 3 if opcode in {0xAF, 0xB0, 0xB1} else 1] = mode
            if opcode in MASKED_VARIABLE_OPCODES:
                # Retail's destination-immediate marker. These handlers read
                # the destination directly and test only source bit 0x40.
                raw[5] = 0x80
            result.append(bytes(decode_instruction(raw, 0).raw))
    for sub in range(227):
        if sub == 0 or 0x78 <= sub <= 0x7E:
            continue
        for mode in range(4) if sub in {0x27, 0x5C, 0x77, 0xB0, 0xD4, 0xDD} else (0,):
            raw = bytes([0xFE, sub, mode]) + bytes(29)
            result.append(decode_instruction(raw, 0).raw)
    return tuple(dict.fromkeys(result))


def _form_key(text: str) -> str:
    match = re.match(r"([a-z_][a-z_0-9]*\.[a-z_][a-z_0-9]*)\(", text.strip())
    if match:
        return match[1]
    match = re.match(r"(if|goto|call|start|stop|return|nop|stall_forever)\b", text.strip())
    return match[1] if match else "*"


@lru_cache(maxsize=64)
def _indexed_seeds(seeds: tuple[bytes, ...]) -> dict[str, tuple[bytes, ...]]:
    from decompile_field_scripts import decode_instruction

    grouped = {}
    for seed in seeds:
        if seed == b"\xfe":
            continue
        form = instruction_form(decode_instruction(seed, 0), {})
        grouped.setdefault(_form_key(form.template), []).append(seed)
    return {key: tuple(values) for key, values in grouped.items()}


def encode_statement(statement: str, seed: bytes | None, symbols: dict, labels: dict[str, int], *, references: dict[int, str] | None = None) -> bytes:
    from decompile_field_scripts import decode_instruction

    match = re.fullmatch(r'raw\("([0-9A-Fa-f\s]+)"(?:,\s*([A-Za-z_0-9,\s]+))?\);', statement.strip())
    if match:
        from decompile_field_scripts import address_operand_offsets

        raw = bytearray(bytes.fromhex(match[1]))
        if match[2] is not None:
            names = [name.strip() for name in match[2].split(",")]
            offsets = address_operand_offsets(decode_instruction(raw, 0))
            if len(names) != len(offsets):
                raise ValueError("raw instruction label count does not match its address fields")
            for offset, name in zip(offsets, names):
                if name not in labels:
                    raise ValueError(f"unresolved label: {name}")
                value = labels[name] if references is None else 0
                struct.pack_into("<H", raw, offset, checked(value, 0, 65535, "target offset"))
                if references is not None:
                    references[offset] = name
        return bytes(raw)
    if seed is not None:
        instruction = decode_instruction(seed, 0)
        if instruction.size != len(seed):
            raise ValueError("@encoding must contain exactly one complete instruction")
    variables = {symbol.reference: offset for offset, symbol in symbols.items()}
    # Prefer the original opcode and its corresponding operator families before
    # canonical forms, retaining invisible control bits wherever possible.
    families = (
        (0x14, 0x15), (0x22, 0x23), (0x24, 0x25), (0x2A, 0x2B),
        (0x36, 0x37), (0x3C, 0x3D), (0x41, 0x42), (7, 8, 9),
        (0x35, 0x38, 0x39, 0x3A, 0x3B, 0x3E, 0x3F, 0x40, 0xDE, 0xDF),
    )
    candidates = [] if seed is None else [seed]
    for family in families:
        if seed is not None and seed[0] in family:
            candidates.extend(bytes([op]) + seed[1:] for op in family if op != seed[0])
    if seed is not None and seed[0] == 0x02:
        candidates.extend(seed[:5] + bytes([(seed[5] & 0xF0) | mode]) + seed[6:] for mode in range(11) if mode != seed[5] & 15)
    # Explicit one-instruction forms allow insertion and replacement with a
    # different-size opcode. Try provenance first to preserve invisible bits.
    key = _form_key(statement)
    candidates.extend(_indexed_seeds(instruction_seeds()).get(key, ()))
    if key == "if" and (seed is None or seed[0] != 0x02):
        candidates.extend(bytes([2, 0, 0, 0, 0, mode, 0, 0]) for mode in range(1, 11))
    errors = []
    for candidate in candidates:
        instruction = decode_instruction(candidate, 0)
        form = instruction_form(instruction, symbols)
        matched = form_pattern(form.template, len(form.operands)).fullmatch(normalized(statement))
        if matched is None:
            continue
        try:
            encoding_labels = labels if references is None else dict.fromkeys(labels, 0)
            result = form.encode(statement, candidate, variables, encoding_labels)
        except ValueError as error:
            errors.append(str(error))
            continue
        if result is not None:
            try:
                decoded = decode_instruction(result, 0)
            except ValueError:
                continue
            if decoded.size != len(result):
                continue
            # A second rendering detects overlapping fields and mode-dependent
            # interpretations that cannot represent the requested statement.
            rendered = instruction_form(decoded, symbols).render(result, symbols)
            values = [matched[f"f{i}"] for i in range(len(form.operands))]
            for i, operand in enumerate(form.operands):
                if operand.kind == "label":
                    values[i] = f"L_{encoding_labels[values[i]]:04X}"
            expected = form.template.format(*values)
            if normalized(rendered) != normalized(expected):
                errors.append("operands do not round-trip through the selected encoding")
                continue
            if references is not None:
                references.update({operand.offset: matched[f"f{i}"] for i, operand in enumerate(form.operands) if operand.kind == "label"})
            return result
    raise ValueError(errors[0] if errors else "statement does not match a supported instruction form; use raw(...) for an explicit encoding")
