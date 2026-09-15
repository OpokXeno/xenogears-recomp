"""Conservative high-level rendering for Xenogears Field bytecode."""

from __future__ import annotations

import json
import re
import struct
from collections import deque
from dataclasses import dataclass, replace
from pathlib import Path


TABLE_PATH = Path(__file__).with_name("field_opcode_table.json")
TABLE_SCHEMA = "xenogears-field-opcodes/v1"

PRIMARY_TERMINALS = {0x00, 0x04, 0x0D, 0x5B, 0xD1, 0xE4}
PRIMARY_UNCONDITIONAL_JUMPS = {0x01}
PRIMARY_CALLS = {0x05, 0x06}
PRIMARY_CONDITIONAL_CALLS = {0x0A, 0xCC}
PRIMARY_CONDITIONAL_BRANCHES = {
    0x02,
    0x31,
    0x32,
    0x84,
    0x85,
    0x86,
    0x89,
    0x8A,
    0x8B,
    0x8E,
    0x91,
    0xB9,
    0xC9,
    0xCB,
    0xE2,
    0xE3,
    0xFB,
}
EXTENDED_CONDITIONAL_BRANCHES = {
    0x02,
    0x05,
    0x06,
    0x30,
    0x31,
    0x32,
    0x33,
    0x34,
    0x35,
    0x36,
    0x37,
}
CONDITION_OPERATORS = {
    0x0: "==",
    0x1: "!=",
    0x2: ">",
    0x3: "<",
    0x4: ">=",
    0x5: "<=",
    0x6: "bit_intersects",
    0x7: "!=",
    0x8: "either_nonzero",
    0x9: "bit_intersects",
    0xA: "masked_bits_clear",
}


@dataclass(frozen=True)
class Instruction:
    pc: int
    opcode: int
    subopcode: int | None
    size: int
    raw: bytes
    name: str
    behavior: str
    successors: tuple[int, ...]
    targets: tuple[int, ...]


@dataclass(frozen=True)
class VariableReference:
    offset: int
    role: str
    evidence: str
    confidence: str = "inferred"


@dataclass(frozen=True)
class VariableSymbol:
    offset: int
    name: str
    scope: str
    value_type: str
    evidence: str
    confidence: str

    @property
    def reference(self) -> str:
        return f"{self.scope}.{self.name}"


DOCUMENTED_VARIABLES = {
    0x0000: ("story_progress", "main story progress"),
    0x0002: ("map_entry_point", "entry point in the destination map"),
    0x0004: ("previous_map_id", "previous map ID"),
    0x0006: ("player_direction", "player direction"),
    0x0008: ("camera_direction", "camera direction"),
    0x000A: ("event_timer", "event timer"),
    0x000C: ("minute_second_state", "minutes/seconds state"),
    0x000E: ("hour_state", "hours state"),
    0x0014: ("dialogue_choice_line", "confirmed dialogue choice line"),
    0x003E: ("party_slot_0_character_id", "party slot 0 character ID"),
    0x0040: ("party_slot_1_character_id", "party slot 1 character ID"),
    0x0042: ("party_slot_2_character_id", "party slot 2 character ID"),
}

VARIABLE_DOC = "docs/xenogears/field/02-resource-and-script-formats.md"

# Direct output operands proven from the documented handlers. Offsets are into
# the complete encoded instruction, including FE for extended instructions.
OUTPUT_VARIABLES = {
    (0x2D, None): (
        (2, "actor_position_x"),
        (4, "actor_position_z"),
        (6, "actor_position_y"),
    ),
    (0x2E, None): ((1, "actor_direction"),),
    (0x2F, None): ((1, "current_character_id"),),
    (0x30, None): ((1, "party_leader_character_id"),),
    (0x34, None): ((3, "inventory_object_quantity"),),
    (0x43, None): ((1, "random_value"),),
    (0x48, None): ((3, "script_byte"),),
    (0x49, None): ((3, "script_halfword"),),
    (0x6D, None): ((1, "cosine_result"),),
    (0x6E, None): ((1, "sine_result"),),
    (0x82, None): ((3, "walkmesh_normal_byte"),),
    (0x88, None): ((1, "scenario_flags"),),
    (0xA5, None): ((1, "camera_direction"),),
    (0xA8, None): ((1, "scaled_random_value"),),
    (0xAD, None): (
        (1, "camera_tween_target_x"),
        (3, "camera_tween_target_z"),
        (5, "camera_tween_target_y"),
    ),
    (0xAE, None): (
        (1, "camera_tween_position_x"),
        (3, "camera_tween_position_z"),
        (5, "camera_tween_position_y"),
    ),
    (0xCA, None): ((1, "atan2_result"),),
    (0xEB, None): (
        (14, "orbit_position_x"),
        (16, "orbit_position_z"),
        (18, "orbit_position_y"),
    ),
    (0xEC, None): (
        (9, "camera_orbit_position_x"),
        (11, "camera_orbit_position_z"),
        (13, "camera_orbit_position_y"),
    ),
    (0xF0, None): (
        (1, "camera_yaw"),
        (3, "camera_projection_dip"),
        (5, "camera_projection_depth"),
    ),
    (0xF3, None): (
        (1, "camera_orbit_yaw"),
        (3, "camera_orbit_pitch"),
        (5, "camera_orbit_half_distance"),
    ),
    (0xFE, 0x22): ((2, "camera_projection_depth"),),
    (0xFE, 0x28): ((2, "actor_flags_1"),),
    (0xFE, 0x29): ((2, "actor_flags_2"),),
    (0xFE, 0x2A): ((2, "actor_flags_3"),),
    (0xFE, 0x2B): ((2, "actor_flags_4"),),
    (0xFE, 0x38): ((2, "actor_distance"),),
    (0xFE, 0x69): ((2, "party_progress_total"),),
    (0xFE, 0x71): ((2, "current_actor_rotation_angle"),),
    (0xFE, 0x72): ((2, "interpolated_angle"),),
    (0xFE, 0x73): ((2, "distance_between_2d_points"),),
    (0xFE, 0x75): ((3, "actor_rotation_angle"),),
    (0xFE, 0x76): ((2, "distance_between_3d_points"),),
    (0xFE, 0x85): ((2, "video_playback_frame"),),
    (0xFE, 0x8B): ((2, "current_actor_party_slot"),),
    (0xFE, 0xA8): (
        (2, "camera_target_x"),
        (4, "camera_target_z"),
        (6, "camera_target_y"),
    ),
    (0xFE, 0xA9): (
        (2, "camera_position_x"),
        (4, "camera_position_z"),
        (6, "camera_position_y"),
    ),
    (0xFE, 0xAD): ((2, "party_member_hp"),),
    (0xFE, 0xAF): (
        (13, "transformed_joint_x"),
        (15, "transformed_joint_y"),
        (17, "transformed_joint_z"),
    ),
    (0xFE, 0xB4): ((2, "party_member_mp"),),
    (0xFE, 0xB9): (
        (2, "world_map_position_x"),
        (4, "world_map_position_y"),
        (6, "world_map_position_z"),
        (8, "world_map_position_direction"),
    ),
    (0xFE, 0xBB): ((2, "world_map_vehicle_state"),),
    (0xFE, 0xC0): ((2, "battling_match_result"),),
    (0xFE, 0xC1): (
        (2, "party_sprite_animation_status"),
        (4, "party_actor_index"),
    ),
    (0xFE, 0xC7): ((4, "actor_character_gear_id"),),
    (0xFE, 0xCD): ((2, "current_disc_number"),),
    (0xFE, 0xD3): (
        (14, "scaled_ratio_1"),
        (16, "scaled_ratio_2"),
    ),
    (0xFE, 0xD5): (
        (2, "world_map_position_x"),
        (4, "world_map_position_y"),
    ),
    (0xFE, 0xD6): (
        (2, "game_state_184e"),
        (4, "game_state_1852"),
    ),
}

# These handlers write fixed VM slots rather than destinations encoded in the
# instruction. Some writes occur only after an asynchronous operation commits.
IMPLICIT_OUTPUT_VARIABLES = {
    (0x47, None): ((0x0002, "map_entry_point"),),
    (0x87, None): ((0x0000, "story_progress"),),
    (0x94, None): ((0x000A, "event_timer"),),
    (0x98, None): ((0x0002, "map_entry_point"),),
    (0x9C, None): ((0x0014, "dialogue_choice_line"),),
    (0xEA, None): ((0x0002, "map_entry_point"),),
    (0xFE, 0x56): ((0x0002, "menu_selection"),),
    (0xFE, 0x84): ((0x0002, "map_entry_point"),),
    (0xFE, 0xCF): ((0x0002, "map_entry_point"),),
}

CONDITIONAL_OUTPUT_VARIABLES = {
    (0xAF, None): ((1, "camera_yaw"),),
    (0xB0, None): ((1, "camera_projection_dip"),),
    (0xB1, None): ((1, "camera_projection_depth"),),
}


def load_opcode_table(path: Path = TABLE_PATH) -> tuple[dict[int, dict], dict[int, dict]]:
    document = json.loads(path.read_text(encoding="ascii"))
    if document.get("schema") != TABLE_SCHEMA:
        raise ValueError(f"{path}: unsupported opcode table schema")
    primary = {int(key, 16): value for key, value in document["primary"].items()}
    extended = {int(key, 16): value for key, value in document["extended"].items()}
    if len(primary) != 256 or len(extended) != 227:
        raise ValueError(f"{path}: incomplete opcode table")
    return primary, extended


PRIMARY, EXTENDED = load_opcode_table()


def _u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _s16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<h", data, offset)[0]


def _fixed_size(record: dict) -> int:
    value = record["bytes"]
    if value.isdigit():
        return int(value)
    if value == "stall":
        return 1
    raise ValueError(f"opcode requires mode-dependent sizing: {value}")


def instruction_size(bytecode: bytes, pc: int) -> tuple[int, int | None, dict]:
    if not 0 <= pc < len(bytecode):
        raise ValueError("instruction PC leaves bytecode")
    opcode = bytecode[pc]
    record = PRIMARY[opcode]
    if opcode != 0xFE:
        if opcode == 0x10:
            if pc + 2 > len(bytecode):
                raise ValueError("truncated MoveActorToPosition mode")
            return (9 if bytecode[pc + 1] == 0 else 2), None, record
        if opcode == 0x57:
            if pc + 2 > len(bytecode):
                raise ValueError("truncated ContinueBallisticActorMove mode")
            return (2 if bytecode[pc + 1] & 3 == 3 else 11), None, record
        if opcode == 0x73:
            if pc + 2 > len(bytecode):
                raise ValueError("truncated InitializeParticleSystemCommand mode")
            return (2 if bytecode[pc + 1] == 0 else 8), None, record
        return _fixed_size(record), None, record

    if pc + 2 > len(bytecode):
        raise ValueError("truncated extended opcode")
    subopcode = bytecode[pc + 1]
    if subopcode > 0xE2:
        raise ValueError(f"invalid extended opcode FE {subopcode:02X}")
    extended = EXTENDED[subopcode]
    if subopcode == 0x00 or 0x78 <= subopcode <= 0x7E:
        return 1, subopcode, extended
    if subopcode in {0x27, 0x5C, 0x77, 0xB0, 0xD4, 0xDD}:
        if pc + 3 > len(bytecode):
            raise ValueError(f"truncated {extended['name']} mode")
        mode = bytecode[pc + 2]
        if subopcode == 0x27:
            size = 5 if mode == 0 else 3
        elif subopcode == 0x5C:
            size = 5 if mode == 2 else 3
        elif subopcode == 0x77:
            size = 12 if mode == 1 else 3
        elif subopcode == 0xB0:
            size = 3 if mode == 1 else 7
        elif subopcode == 0xD4:
            size = 11 if mode in {1, 3} else 3
        else:
            size = 7 if mode in {0, 1} else 3
        return size, subopcode, extended
    return _fixed_size(extended), subopcode, extended


def target_operand_offset(opcode: int, subopcode: int | None, size: int) -> int | None:
    """Position of an absolute u16 control-flow reference in its encoding."""
    if subopcode is not None:
        if subopcode in EXTENDED_CONDITIONAL_BRANCHES:
            return size - 2
        return None
    if opcode in {0x01, 0x05, 0x06}:
        return 1
    if opcode in {0x0A, 0xC9, 0xCB, 0xCC}:
        return 2
    if opcode == 0x02:
        return 6
    if opcode in PRIMARY_CONDITIONAL_BRANCHES:
        return size - 2
    return None


def address_operand_offsets(instruction: Instruction) -> tuple[int, ...]:
    """Bytecode-base-relative u16 fields, including the bases of 48/49 reads.

    Checked against handlers 80093CD0/80093D48: base(+1) + evaluated byte
    index(+5), masked to 16 bits. The index itself is not a pointer.
    """
    target = target_operand_offset(instruction.opcode, instruction.subopcode, instruction.size)
    if target is not None:
        return (target,)
    if instruction.subopcode is None and instruction.opcode in {0x48, 0x49}:
        return (1,)
    return ()


def decode_instruction(bytecode: bytes, pc: int) -> Instruction:
    size, subopcode, record = instruction_size(bytecode, pc)
    if pc + size > len(bytecode):
        raise ValueError(f"{record['name']} leaves bytecode")
    opcode = bytecode[pc]
    target_position = target_operand_offset(opcode, subopcode, size)
    target = None if target_position is None else _u16(bytecode, pc + target_position)
    targets = () if target is None else (target,)
    fallthrough = pc + size
    successors: list[int] = []
    if subopcode is None and opcode == 0xA6:
        count = _u16(bytecode, pc + 1)
        if count & 0x8000:
            successors.append(pc + size + 3 * (count & 0x7FFF))
    elif subopcode is None and opcode in PRIMARY_UNCONDITIONAL_JUMPS:
        successors.extend(targets)
    elif subopcode is None and opcode in PRIMARY_TERMINALS:
        pass
    else:
        if fallthrough < len(bytecode):
            successors.append(fallthrough)
        if target is not None and target not in successors:
            successors.append(target)
    return Instruction(
        pc=pc,
        opcode=opcode,
        subopcode=subopcode,
        size=size,
        raw=bytecode[pc : pc + size],
        name=record["name"],
        behavior=record["behavior"],
        successors=tuple(successors),
        targets=targets,
    )


def _entry_map(metadata: dict) -> dict[int, list[tuple[int, int]]]:
    entries: dict[int, list[tuple[int, int]]] = {}
    for row in metadata["routine_rows"]:
        entity_id = row["entity_id"]
        for routine_id, value in enumerate(row["routine_offsets"]):
            entries.setdefault(int(value, 16), []).append((entity_id, routine_id))
    return entries


def _arrival_records_from_entries(
    bytecode: bytes,
    entries: dict[int, list[tuple[int, int]]],
    metadata: dict,
) -> list[dict] | None:
    if not metadata["arrival_table_marker_present"]:
        return []
    positive_entries = sorted(offset for offset in entries if offset > 0)
    if not positive_entries:
        return None
    end = positive_entries[0]
    if end < 1:
        return None
    if (end - 1) % 7:
        inferred_end = None
        for candidate_start in range(8, end, 7):
            pc = candidate_start
            candidate = {}
            try:
                while pc < end:
                    instruction = decode_instruction(bytecode, pc)
                    if pc + instruction.size > end:
                        raise ValueError("instruction crosses first routine entry")
                    candidate[pc] = instruction
                    pc += instruction.size
            except (KeyError, ValueError, struct.error):
                continue
            targets = {
                target for instruction in candidate.values() for target in instruction.targets
            }
            if targets and all(
                target in candidate or end <= target < len(bytecode)
                for target in targets
            ):
                inferred_end = candidate_start
                break
        if inferred_end is None:
            return None
        end = inferred_end
    return [
        {
            "offset": offset,
            "x": _s16(bytecode, offset),
            "z": _s16(bytecode, offset + 2),
            "walkmesh": bytecode[offset + 4],
            "camera_direction": bytecode[offset + 5],
            "actor_direction": bytecode[offset + 6],
        }
        for offset in range(1, end, 7)
    ]


def analyze_bytecode(bytecode: bytes, metadata: dict) -> dict:
    entries = _entry_map(metadata)
    seeds = set(entries)
    if metadata["arrival_table_marker_present"]:
        seeds.discard(0)
    queue = deque(sorted(seeds))
    instructions: dict[int, Instruction] = {}
    occupied: dict[int, int] = {}
    labels = set(entries)
    diagnostics = []
    computed_successors: dict[int, tuple[int, ...]] = {}
    landing_data = {}
    alignments = {}
    reserved = set()

    while queue:
        pc = queue.popleft()
        if pc in reserved:
            continue
        if pc in instructions:
            continue
        if not 0 <= pc < len(bytecode):
            diagnostics.append(f"target 0x{pc:04X} leaves bytecode")
            continue
        owner = occupied.get(pc)
        if owner is not None and owner != pc:
            diagnostics.append(
                f"target 0x{pc:04X} enters instruction at 0x{owner:04X}"
            )
            continue
        try:
            instruction = decode_instruction(bytecode, pc)
        except (KeyError, ValueError, struct.error) as error:
            diagnostics.append(f"0x{pc:04X}: {error}")
            continue
        conflict = next(
            (
                occupied[position]
                for position in range(pc, pc + instruction.size)
                if position in occupied and occupied[position] != pc
            ),
            None,
        )
        if conflict is not None:
            diagnostics.append(
                f"instruction at 0x{pc:04X} overlaps instruction at 0x{conflict:04X}"
            )
            continue
        if instruction.opcode in {0xD4, 0xFC} and pc + 9 <= len(bytecode) and bytecode[pc + 5:pc + 7] == b"\x01\x01":
            normal, alternate = _u16(bytecode, pc + 6), _u16(bytecode, pc + 7)
            if normal + 3 <= alternate and alternate + 3 <= len(bytecode) and bytecode[normal] == bytecode[alternate] == 1:
                # Recover the shared-byte routing form emitted by the linker.
                # This makes decompile/edit/compile of a modified binary just as
                # relocatable as editing its exported assembly.
                landing_data[pc + 5] = {1: normal, 2: alternate}
                reserved.update(range(pc + 5, pc + 9))
                alignments[normal] = (256, 1)
                alignments[alternate] = (256, f"L_{normal:04X}")
                labels.update((normal, alternate))
                instruction = replace(instruction, successors=(normal, alternate))
        instructions[pc] = instruction
        for position in range(pc, pc + instruction.size):
            occupied[position] = pc
        labels.update(instruction.targets)
        for operand in address_operand_offsets(instruction):
            labels.add(_u16(instruction.raw, operand))
        for successor in instruction.successors:
            if 0 <= successor < len(bytecode):
                queue.append(successor)
            elif successor != len(bytecode):
                diagnostics.append(
                    f"successor 0x{successor:04X} from 0x{pc:04X} leaves bytecode"
                )

        if instruction.opcode == 0xA6:
            count = _u16(instruction.raw, 1)
            if not count & 0x8000:
                # Variable A6 operands conventionally select a contiguous table
                # of three-byte unconditional jumps immediately after A6.
                table_pc = pc + 3
                table_entries = []
                while table_pc + 3 <= len(bytecode) and bytecode[table_pc] == 0x01:
                    queue.append(table_pc)
                    labels.add(table_pc)
                    table_entries.append(table_pc)
                    table_pc += 3
                computed_successors[pc] = tuple(table_entries)

    arrivals = _arrival_records_from_entries(bytecode, entries, metadata)
    arrival_end = 0
    if metadata["arrival_table_marker_present"] and arrivals is not None:
        arrival_end = 1 + len(arrivals) * 7

    def uncovered_ranges() -> list[tuple[int, int]]:
        ranges = []
        start = None
        for position in range(len(bytecode) + 1):
            is_data = position < len(bytecode) and position not in occupied
            if is_data and start is None:
                start = position
            elif not is_data and start is not None:
                ranges.append((start, position))
                start = None
        boundaries = sorted({point for start in landing_data for point in (start, start + 4)})
        split = []
        for start, end in ranges:
            points = [start] + [point for point in boundaries if start < point < end] + [end]
            split.extend(zip(points, points[1:]))
        return split

    orphan_instruction_pcs = set()
    candidate_regions = []
    for range_start, range_end in uncovered_ranges():
        range_start = max(range_start, arrival_end)
        if range_start in landing_data:
            continue
        if range_end - range_start < 3:
            continue
        candidate = {}
        pc = range_start
        while pc < range_end:
            try:
                instruction = decode_instruction(bytecode, pc)
                if pc + instruction.size > range_end:
                    raise ValueError("instruction crosses candidate region")
            except (KeyError, ValueError, struct.error):
                break
            candidate[pc] = instruction
            pc += instruction.size
        if not candidate:
            continue
        candidate_regions.append((range_start, pc, range_end, candidate))

    candidate_starts = {
        pc for _, _, _, candidate in candidate_regions for pc in candidate
    }
    all_candidate_targets = {
        target
        for _, _, _, candidate in candidate_regions
        for instruction in candidate.values()
        for target in instruction.targets
    }
    for range_start, candidate_end, range_end, candidate in candidate_regions:
        candidate_targets = {
            target for instruction in candidate.values() for target in instruction.targets
        }
        targets_align = all(
            target in candidate_starts or target in instructions
            for target in candidate_targets
        )
        last_instruction = candidate[max(candidate)]
        clean_terminal_region = (
            candidate_end == range_end
            and len(candidate) >= 4
            and last_instruction.subopcode is None
            and last_instruction.opcode in PRIMARY_TERMINALS
        )
        is_valid_root = (
            candidate_end - range_start >= 16
            and targets_align
            and (candidate_targets or clean_terminal_region)
        )
        is_referenced_fragment = range_start in all_candidate_targets and targets_align
        if not (is_valid_root or is_referenced_fragment):
            continue
        instructions.update(candidate)
        orphan_instruction_pcs.update(candidate)
        labels.update(candidate_targets)
        for instruction in candidate.values():
            for operand in address_operand_offsets(instruction):
                labels.add(_u16(instruction.raw, operand))
        labels.add(range_start)
        for pc, instruction in candidate.items():
            for position in range(pc, pc + instruction.size):
                occupied[position] = pc

    # A proven arrival table can share one uncovered range with trailing
    # payload. Split that range so the renderer never emits the arrivals twice.
    data_ranges = []
    for start, end in uncovered_ranges():
        if start < arrival_end < end:
            data_ranges.extend(((start, arrival_end), (arrival_end, end)))
        else:
            data_ranges.append((start, end))
    data_regions = []
    for range_start, range_end in data_ranges:
        payload = bytecode[range_start:range_end]
        if range_start in landing_data:
            classification = "dual_landing"
        elif range_start < arrival_end and range_end <= arrival_end:
            classification = "arrival_table"
        elif payload and not any(payload):
            classification = "zero_padding"
        elif len(payload) <= 8:
            classification = "separator_or_padding_bytes"
        else:
            pc = range_start
            while pc < range_end:
                try:
                    instruction = decode_instruction(bytecode, pc)
                except (KeyError, ValueError, struct.error):
                    break
                if pc + instruction.size > range_end:
                    break
                pc += instruction.size
            decoded_ratio = (pc - range_start) / len(payload)
            classification = (
                "unreferenced_code_candidate"
                if pc - range_start >= 16 and decoded_ratio >= 0.75
                else "embedded_table_or_payload"
            )
        data_regions.append(
            {
                "start": range_start,
                "end": range_end,
                "size": range_end - range_start,
                "classification": classification,
            }
        )
    data_bytes = sum(region["size"] for region in data_regions)
    classified_bytes = len(occupied) + data_bytes
    return {
        "instructions": instructions,
        "entries": entries,
        "labels": labels,
        "data_ranges": data_ranges,
        "data_regions": data_regions,
        "orphan_instruction_pcs": orphan_instruction_pcs,
        "computed_successors": computed_successors,
        "landing_data": landing_data,
        "alignments": alignments,
        "diagnostics": sorted(set(diagnostics)),
        "instruction_count": len(instructions),
        "instruction_bytes": len(occupied),
        "data_bytes": data_bytes,
        "classified_bytes": classified_bytes,
        "bytecode_size": len(bytecode),
        "instruction_coverage_percent": round(len(occupied) * 100 / len(bytecode), 2) if bytecode else 0.0,
        "total_coverage_percent": round(classified_bytes * 100 / len(bytecode), 2) if bytecode else 100.0,
    }


def _snake_case(name: str) -> str:
    value = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", name)
    return re.sub(r"([a-z])([A-Z])", r"\1_\2", value).lower()


def _label(target: int) -> str:
    return f"L_{target:04X}"


def _encoded_outputs(instruction: Instruction) -> tuple[tuple[int, str], ...]:
    key = (instruction.opcode, instruction.subopcode)
    outputs = OUTPUT_VARIABLES.get(key, ())
    if key in CONDITIONAL_OUTPUT_VARIABLES and instruction.raw[3] == 0:
        outputs += CONDITIONAL_OUTPUT_VARIABLES[key]
    return outputs


def _instruction_variable_references(instruction: Instruction) -> list[VariableReference]:
    from field_instruction_codec import EVALUATED_OPERATION_INPUTS

    raw = instruction.raw
    opcode = instruction.opcode
    references = []
    output_key = (opcode, instruction.subopcode)
    for offset, role in _encoded_outputs(instruction):
        references.append(
            VariableReference(
                _u16(raw, offset),
                role,
                f"opcode {instruction.name} output",
            )
        )
    for offset, role in IMPLICIT_OUTPUT_VARIABLES.get(output_key, ()):
        references.append(
            VariableReference(
                offset,
                role,
                f"opcode {instruction.name} implicit output",
                "documented",
            )
        )

    if instruction.subopcode is None and opcode == 0x02:
        control = raw[5]
        if not control & 0x80:
            references.append(VariableReference(_u16(raw, 1), "event_condition", "conditional read"))
        if not control & 0x40:
            references.append(VariableReference(_u16(raw, 3), "event_condition", "conditional read"))
    elif instruction.subopcode is None and opcode == 0x26:
        value = _u16(raw, 1)
        if not value & 0x8000:
            references.append(VariableReference(value, "delay_duration", "timed wait operand"))
    elif instruction.subopcode is None and opcode in {0x35, 0x38, 0x39, 0x3A, 0x3B, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0xDE, 0xDF}:
        role = "event_flags" if opcode in {0x3A, 0x3B, 0x3E, 0x3F, 0x40} else "event_state"
        references.append(VariableReference(_u16(raw, 1), role, f"opcode {instruction.name} destination"))
        if opcode in {0x41, 0x42}:
            value = _u16(raw, 3)
            if not value & 0x8000:
                references.append(VariableReference(value, "shift_count", f"opcode {instruction.name} source"))
        elif not raw[5] & 0x40:
            references.append(VariableReference(_u16(raw, 3), "event_value", f"opcode {instruction.name} source"))
    elif instruction.subopcode is None and opcode in {0x36, 0x37}:
        references.append(VariableReference(_u16(raw, 1), "event_flag", f"opcode {instruction.name} destination"))
    elif instruction.subopcode is None and opcode in {0x3C, 0x3D}:
        references.append(VariableReference(_u16(raw, 1), "event_counter", f"opcode {instruction.name} destination"))
    elif instruction.subopcode is None and opcode == 0xA6:
        value = _u16(raw, 1)
        if not value & 0x8000:
            references.append(VariableReference(value, "dispatch_index", "computed jump-table selector"))
    elif instruction.subopcode is None and opcode == 0xDC:
        references.extend(
            VariableReference(_u16(raw, offset), "event_state", "opcode SwapVariables operand")
            for offset in (1, 3)
        )
    elif instruction.subopcode in {0x0A, 0x0B}:
        packed = _u16(raw, 2)
        references.append(
            VariableReference(
                packed >> 4,
                "indexed_flags",
                f"opcode {instruction.name} packed destination",
            )
        )

    v80_operands = list(EVALUATED_OPERATION_INPUTS.get(output_key, ()))
    if instruction.subopcode is None and opcode in {0x16, 0x5C}:
        v80_operands.append((1, "character_selector" if opcode == 0x16 else "party_slot"))
    elif instruction.subopcode is None and opcode == 0x34:
        v80_operands.append((1, "inventory_object"))
    elif instruction.subopcode is None and opcode in {0x48, 0x49}:
        v80_operands.append((5, "script_index"))
    elif instruction.subopcode is None and opcode == 0x87:
        v80_operands.append((1, "scenario_flags"))
    elif instruction.subopcode is None and opcode == 0x94:
        v80_operands.extend(((1, "event_timer_high_byte"), (3, "event_timer_low_byte")))
    elif instruction.subopcode is None and opcode == 0xA8:
        v80_operands.append((3, "random_maximum"))
    elif instruction.subopcode == 0x0D:
        v80_operands.append((2, "dialogue_portrait_character"))
    elif instruction.subopcode == 0x56:
        v80_operands.append((2, "menu_selection"))
    elif instruction.subopcode == 0x69:
        v80_operands.append((4, "character_selector"))
    elif instruction.subopcode == 0xC1:
        v80_operands.append((6, "party_slot"))
    elif instruction.subopcode == 0xC7:
        v80_operands.append((2, "actor_selector"))
    for offset, role in v80_operands:
        value = _u16(raw, offset)
        if not value & 0x8000:
            references.append(VariableReference(value, role, f"opcode {instruction.name} input"))

    return references


def build_variable_symbols(analysis: dict, metadata: dict) -> dict[int, VariableSymbol]:
    references: dict[int, list[VariableReference]] = {}
    reference_pcs: dict[int, set[int]] = {}
    for instruction in analysis["instructions"].values():
        for reference in _instruction_variable_references(instruction):
            references.setdefault(reference.offset, []).append(reference)
            reference_pcs.setdefault(reference.offset, set()).add(instruction.pc)

    unsigned = {int(value, 16) for value in metadata["unsigned_variable_offsets"]}
    symbols = {}
    used_names: dict[tuple[str, str], int] = {}
    owners = _instruction_owners(analysis, metadata)
    for offset, candidates in sorted(references.items()):
        if offset < 0x400:
            scope = "persistent"
        elif offset < 0x800:
            scope = "scene"
        else:
            scope = "out_of_range"
        if offset in DOCUMENTED_VARIABLES:
            name, meaning = DOCUMENTED_VARIABLES[offset]
            evidence = f"{VARIABLE_DOC}: {meaning}"
            confidence = "documented"
        else:
            candidate = next(
                (item for item in candidates if item.evidence.startswith("opcode ") and "output" in item.evidence),
                candidates[0],
            )
            name = candidate.role
            evidence = candidate.evidence
            confidence = candidate.confidence
            generic_roles = {
                "event_state": "sequence_step",
                "event_value": "sequence_value",
                "event_condition": "sequence_gate",
                "event_flag": "sequence_enabled",
                "event_flags": "sequence_flags",
                "event_counter": "sequence_counter",
            }
            if name in generic_roles:
                instruction_pcs = sorted(analysis["instructions"])
                pc_indexes = {pc: index for index, pc in enumerate(instruction_pcs)}
                domains = []
                for pc in reference_pcs[offset]:
                    index = pc_indexes[pc]
                    for neighbor_pc in instruction_pcs[max(0, index - 2) : index + 3]:
                        if abs(neighbor_pc - pc) > 32:
                            continue
                        namespace = _operation_namespace(analysis["instructions"][neighbor_pc])
                        if namespace not in {"state", "flow", "event"}:
                            domains.append(namespace)
                if domains:
                    domain = max(sorted(set(domains)), key=domains.count)
                else:
                    owner_entities = {
                        entity_id
                        for pc in reference_pcs[offset]
                        for entity_id in owners.get(pc, ())
                    }
                    if scope == "scene" and len(owner_entities) == 1:
                        domain = f"entity_{next(iter(owner_entities))}"
                    else:
                        domain = "field" if scope == "persistent" else "shared"
                owner_entities = {
                    entity_id
                    for pc in reference_pcs[offset]
                    for entity_id in owners.get(pc, ())
                }
                event_roles = set()
                if scope == "scene" and len(owner_entities) == 1:
                    entity_id = next(iter(owner_entities))
                    for pc in reference_pcs[offset]:
                        candidate_entries = [
                            entry_pc
                            for entry_pc, aliases in analysis["entries"].items()
                            if entry_pc <= pc
                            and any(alias_entity == entity_id for alias_entity, _ in aliases)
                        ]
                        if not candidate_entries:
                            continue
                        entry_pc = max(candidate_entries)
                        routines = {
                            routine_id
                            for alias_entity, routine_id in analysis["entries"][entry_pc]
                            if alias_entity == entity_id
                        }
                        if routines == {0}:
                            event_roles.add("initialize")
                        elif routines == {1}:
                            event_roles.add("update")
                        elif routines and routines <= {2, 3}:
                            event_roles.add("interaction")
                    context = f"entity_{entity_id}"
                    if len(event_roles) == 1:
                        context += f"_{next(iter(event_roles))}"
                    if not domain.startswith("entity_"):
                        context += f"_{domain}"
                    name = f"{context}_{generic_roles[name]}"
                else:
                    name = f"{domain}_{generic_roles[name]}"
            if scope == "out_of_range" or offset % 2:
                confidence = "anomalous"
        key = (scope, name)
        used_names[key] = used_names.get(key, 0) + 1
        if used_names[key] > 1:
            name = f"{name}_{used_names[key]}"
        symbols[offset] = VariableSymbol(
            offset=offset,
            name=name,
            scope=scope,
            value_type="unsigned" if offset in unsigned or scope == "out_of_range" else "signed",
            evidence=evidence,
            confidence=confidence,
        )
    return symbols


def _variable(value: int, symbols: dict[int, VariableSymbol]) -> str:
    symbol = symbols.get(value)
    return symbol.reference if symbol is not None else "state.unresolved"


def _actor(value: int) -> str:
    special = {
        0xFF: "party[0]",
        0xFE: "party[1]",
        0xFD: "party[2]",
        0xFB: "self",
    }
    return special.get(value, f"actor[{value}]")


def _raw_comment(instruction: Instruction) -> str:
    return " ".join(f"{value:02X}" for value in instruction.raw)


def _decimalize_hex_literals(text: str) -> str:
    return re.sub(r"0x[0-9A-Fa-f]+", lambda match: str(int(match.group(), 16)), text)


def operation_namespace(name: str) -> str:
    lowered = name.lower()
    if name in {
        "EnableEncountersFieldMenuAndCompass",
        "DisableEncountersFieldMenuAndCompass",
    }:
        return "world"
    if lowered.startswith(("yield", "wait", "sleep", "return", "call", "jump", "skip")):
        return "flow"
    categories = (
        ("input", ("input", "button", "controller")),
        ("dialogue", ("dialog", "text", "portrait", "message")),
        ("camera", ("camera", "projection")),
        ("audio", ("music", "sound", "audio")),
        ("battle", ("battle", "combat")),
        ("inventory", ("inventory", "item", "gold", "menu")),
        ("visual", ("fade", "light", "color", "render", "model", "effect", "particle", "video")),
        ("movement", ("move", "position", "coordinate", "walkmesh", "collision", "rotation", "direction")),
        ("actor", ("actor", "sprite", "party", "character", "gear")),
        ("world", ("field", "encounter", "compass", "trigger", "scene", "map")),
        ("state", ("variable", "flag", "random")),
    )
    return next(
        (category for category, terms in categories if any(term in lowered for term in terms)),
        "event",
    )


def operation_dsl_name(name: str) -> str:
    return f"{operation_namespace(name)}.{_snake_case(name)}"


def _operation_namespace(instruction: Instruction) -> str:
    return operation_namespace(instruction.name)


def _operation_name(instruction: Instruction) -> str:
    return operation_dsl_name(instruction.name)


def render_instruction(
    instruction: Instruction,
    symbols: dict[int, VariableSymbol] | None = None,
) -> str:
    from field_instruction_codec import instruction_form

    symbols = {} if symbols is None else symbols
    statement = instruction_form(instruction, symbols).render(instruction.raw, symbols)
    return f"{statement:<62} // {instruction.pc:04X}: {_raw_comment(instruction)}"


def _arrival_records(bytecode: bytes, analysis: dict, metadata: dict) -> list[dict] | None:
    return _arrival_records_from_entries(bytecode, analysis["entries"], metadata)


def _routine_lines(row: dict) -> list[str]:
    role_names = {0: "initialize", 1: "update", 2: "interact", 3: "contact"}
    offsets = row["routine_offsets"]
    lines = []
    for routine_id in range(min(4, len(offsets))):
        role = role_names[routine_id]
        lines.append(
            f"        {role:<14} -> {_label(int(offsets[routine_id], 16))};"
        )
    start = 4
    while start < len(offsets):
        end = start
        while end + 1 < len(offsets) and offsets[end + 1] == offsets[start]:
            end += 1
        selector = f"{start:02d}" if start == end else f"{start:02d}..{end:02d}"
        selector = f"routine[{selector}]"
        lines.append(
            f"        {selector:<14} -> {_label(int(offsets[start], 16))};"
        )
        start = end + 1
    return lines


def _entry_aliases(aliases: list[tuple[int, int]]) -> str:
    by_entity: dict[int, list[int]] = {}
    for entity_id, routine_id in aliases:
        by_entity.setdefault(entity_id, []).append(routine_id)
    rendered = []
    for entity_id, routines in sorted(by_entity.items()):
        named_roles = {
            0: "initialize",
            1: "update",
            2: "interact",
            3: "contact",
        }
        named = [named_roles[routine_id] for routine_id in routines if routine_id in named_roles]
        extra_routines = [routine_id for routine_id in routines if routine_id >= 4]
        ranges = []
        if extra_routines:
            start = previous = extra_routines[0]
            for routine_id in extra_routines[1:] + [extra_routines[-1] + 2]:
                if routine_id == previous + 1:
                    previous = routine_id
                    continue
                ranges.append(
                    f"routine[{start}]" if start == previous else f"routine[{start}..{previous}]"
                )
                start = previous = routine_id
        rendered.append(f"entity {entity_id}: {', '.join(named + ranges)}")
    return ", ".join(rendered)


def _instruction_owners(analysis: dict, metadata: dict) -> dict[int, set[int]]:
    instructions = analysis["instructions"]
    owners = {pc: set() for pc in instructions}
    for row in metadata["routine_rows"]:
        entity_id = row["entity_id"]
        queue = deque(int(value, 16) for value in row["routine_offsets"])
        visited = set()
        while queue:
            pc = queue.popleft()
            if pc in visited or pc not in instructions:
                continue
            visited.add(pc)
            owners[pc].add(entity_id)
            instruction = instructions[pc]
            queue.extend(instruction.successors)
            queue.extend(analysis["computed_successors"].get(pc, ()))
    return owners


def _render_code_lines(
    pcs: list[int],
    analysis: dict,
    symbols: dict[int, VariableSymbol],
    indent: str,
) -> list[str]:
    instructions = analysis["instructions"]
    entries = analysis["entries"]
    labels = analysis["labels"]
    lines = []
    pc_set = set(pcs)
    previous_instruction = None
    for pc in pcs:
        if previous_instruction is None or previous_instruction.pc + previous_instruction.size != pc:
            if previous_instruction is not None:
                lines.append(f"{indent}}}")
            end = pc + instructions[pc].size
            while end in pc_set:
                end += instructions[end].size
            lines.append(f"{indent}block @source(0x{pc:04X}..0x{end:04X}) {{")
        if pc in labels:
            if pc in entries and previous_instruction is not None:
                lines.append("")
            lines.append(f"{indent}{_label(pc)}:")
        if pc in analysis["orphan_instruction_pcs"] and (
            previous_instruction is None
            or previous_instruction.pc not in analysis["orphan_instruction_pcs"]
            or previous_instruction.pc + previous_instruction.size != pc
        ):
            lines.append(
                f"{indent}  // Orphan event recovered by exact decode and aligned control flow."
            )
        if pc in entries:
            lines.append(f"{indent}  // event: {_entry_aliases(entries[pc])}")
        instruction = instructions[pc]
        if pc in analysis["alignments"]:
            modulus, residue = analysis["alignments"][pc]
            residue = str(residue) if isinstance(residue, int) else f"high({residue})"
            lines.append(f"{indent}  @align({modulus}, {residue})")
        lines.append(f'{indent}  @at(0x{pc:04X}) @encoding("{_raw_comment(instruction)}")')
        lines.append(f"{indent}  {render_instruction(instruction, symbols)}")
        if instruction.behavior:
            lines.append(f"{indent}  // {_decimalize_hex_literals(instruction.behavior)}")
        previous_instruction = instruction
    if previous_instruction is not None:
        lines.append(f"{indent}}}")
    return lines


def render_high_level_script(field_id: int, scripts_file: bytes, metadata: dict) -> tuple[str, dict]:
    bytecode = scripts_file[metadata["bytecode_offset"] :]
    analysis = analyze_bytecode(bytecode, metadata)
    symbols = build_variable_symbols(analysis, metadata)
    lines = [
        "// Xenogears Field script DSL",
        "// Events are grouped by entity; shared labels and source offsets are authoritative.",
        "// Exact byte classifications prevent invented source details.",
        f"field {field_id} {{",
        f'  variable_types = "{scripts_file[:0x80].hex(" ").upper()}";',
        f"  bytecode_size = {len(bytecode)};",
        "  state {",
    ]
    for scope in ("persistent", "scene", "out_of_range"):
        scoped_symbols = [symbol for symbol in symbols.values() if symbol.scope == scope]
        if not scoped_symbols:
            continue
        lines.append(f"    {scope} {{")
        for symbol in scoped_symbols:
            lines.append(
                f"      {symbol.value_type} {symbol.name} @offset(0x{symbol.offset:04X});"
            )
        lines.append("    }")
    lines.extend(("  }", ""))

    data_labels = sorted(analysis["labels"] - analysis["instructions"].keys())
    if data_labels:
        lines.append("  labels {")
        lines.extend(f"    L_{pc:04X} @offset(0x{pc:04X});" for pc in data_labels)
        lines.extend(("  }", ""))

    if analysis["computed_successors"]:
        lines.append("  triplet_tables {")
        for pc, entries in sorted(analysis["computed_successors"].items()):
            if entries:
                targets = ", ".join(f"0x{entry:04X}" for entry in entries)
                lines.append(f"    table @at(0x{pc:04X}) = [{targets}];")
        lines.extend(("  }", ""))

    arrivals = _arrival_records(bytecode, analysis, metadata)
    if arrivals is None:
        lines.extend(
            (
                "  // Arrival marker found, but its count cannot be proven from entry alignment.",
                "  arrivals unknown;",
                "",
            )
        )
    elif arrivals or (metadata["arrival_table_marker_present"] and arrivals is not None):
        end = 1 + len(arrivals) * 7
        lines.append(f"  arrivals @source(0x0000..0x{end:04X}) {{")
        lines.append("    marker: 255;")
        for index, record in enumerate(arrivals):
            camera = "restore" if record["camera_direction"] == 0xFF else record["camera_direction"]
            actor = "restore" if record["actor_direction"] == 0xFF else record["actor_direction"]
            raw = bytecode[record["offset"] : record["offset"] + 7].hex(" ").upper()
            lines.append(
                f"    arrival {index} {{ x: {record['x']}, z: {record['z']}, "
                f"walkmesh: {record['walkmesh']}, camera: {camera}, actor: {actor} }} "
                f"// {record['offset']:04X}: {raw}"
            )
        lines.extend(("  }", ""))

    instructions: dict[int, Instruction] = analysis["instructions"]
    owners = _instruction_owners(analysis, metadata)
    lines.append("  entities {")
    for row in metadata["routine_rows"]:
        entity_id = row["entity_id"]
        entity_pcs = sorted(pc for pc, values in owners.items() if values == {entity_id})
        lines.append(f"    entity {entity_id} {{")
        lines.append("      events {")
        lines.extend(_routine_lines(row))
        lines.append("      }")
        if entity_pcs:
            lines.append("")
            lines.append("      code {")
            lines.extend(_render_code_lines(entity_pcs, analysis, symbols, "      "))
            lines.append("      }")
        lines.append("    }")
        lines.append("")
    lines.append("  }")

    shared_pcs = sorted(pc for pc, values in owners.items() if len(values) != 1)
    if shared_pcs:
        lines.extend(("", "  shared_code {"))
        lines.extend(_render_code_lines(shared_pcs, analysis, symbols, "  "))
        lines.append("  }")

    non_executable = [
        region for region in analysis["data_regions"]
        if region["classification"] != "arrival_table"
    ]
    if non_executable:
        lines.extend(("", "  non_instruction_regions {"))
        for region in non_executable:
            start = region["start"]
            end = region["end"]
            lines.append(
                f"    region {region['classification']} @source(0x{start:04X}..0x{end:04X}) {{"
            )
            for offset in range(start, end, 16):
                chunk = bytecode[offset : min(offset + 16, end)]
                references = "".join(f" @{position}=L_{target:04X}" for position, target in analysis["landing_data"].get(offset, {}).items())
                lines.append(f"      bytes @offset(0x{offset:04X}) = \"{chunk.hex(' ').upper()}\"{references};")
            lines.append("    }")
        lines.append("  }")
    if analysis["diagnostics"]:
        lines.append("")
        lines.append("  diagnostics {")
        for diagnostic in analysis["diagnostics"]:
            lines.append(f"    // {diagnostic}")
        lines.append("  }")
    lines.extend(("}", ""))
    report = {
        key: analysis[key]
        for key in (
            "instruction_count",
            "instruction_bytes",
            "data_bytes",
            "classified_bytes",
            "bytecode_size",
            "instruction_coverage_percent",
            "total_coverage_percent",
            "diagnostics",
        )
    }
    report["data_range_count"] = len(analysis["data_ranges"])
    report["data_classifications"] = dict(
        sorted(
            (classification, sum(region["size"] for region in analysis["data_regions"] if region["classification"] == classification))
            for classification in {region["classification"] for region in analysis["data_regions"]}
        )
    )
    report["semantic_symbol_count"] = len(symbols)
    report["orphan_instruction_count"] = len(analysis["orphan_instruction_pcs"])
    report["orphan_instruction_bytes"] = sum(
        analysis["instructions"][pc].size for pc in analysis["orphan_instruction_pcs"]
    )
    report["language"] = "xenogears-field-event-dsl/v3"
    return "\n".join(lines), report
