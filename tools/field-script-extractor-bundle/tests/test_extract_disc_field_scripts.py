from __future__ import annotations

import importlib.util
import struct
import sys
from pathlib import Path

import pytest


MODULE_PATH = (
    Path(__file__).resolve().parents[1] / "tools" / "extract_disc_field_scripts.py"
)
sys.path.insert(0, str(MODULE_PATH.parent))
SPEC = importlib.util.spec_from_file_location("extract_disc_field_scripts", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
extractor = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = extractor
SPEC.loader.exec_module(extractor)
from decompile_field_scripts import (
    EXTENDED,
    PRIMARY,
    VariableSymbol,
    analyze_bytecode,
    decode_instruction,
    render_high_level_script,
    render_instruction,
)
from build_dsl_documentation import render_catalog


def literal_lzss(payload: bytes) -> bytes:
    assert len(payload) % 8 == 0
    encoded = bytearray(struct.pack("<I", len(payload)))
    for offset in range(0, len(payload), 8):
        encoded.append(0)
        encoded.extend(payload[offset : offset + 8])
    return bytes(encoded)


def scripts_file(*, invalid_entry: bool = False) -> bytes:
    bitmap = bytes(range(0x80))
    bytecode = bytes((0x10, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B))
    offsets = [index % len(bytecode) for index in range(32)]
    if invalid_entry:
        offsets[-1] = len(bytecode)
    return bitmap + struct.pack("<I", 1) + struct.pack("<32H", *offsets) + bytecode


def field_container() -> tuple[bytes, bytes]:
    scripts = scripts_file()
    stream = literal_lzss(scripts)
    offsets = [0x1A0, 0x1A4, 0x1A8, 0x1AC, 0x1B0, 0x1B4]
    offsets.extend((0x1B4 + len(stream) + 4, 0x1B4 + len(stream) + 8, 0x1B4 + len(stream) + 12))
    container = bytearray(offsets[-1] + 4)
    struct.pack_into("<9I", container, 0x10C, 1, 1, 1, 1, 1, len(scripts), 1, 1, 1)
    struct.pack_into("<9I", container, 0x130, *offsets)
    struct.pack_into("<H", container, 0x18C, 1)
    container[offsets[5] : offsets[5] + len(stream)] = stream
    return bytes(container), scripts


def fat_record(lba: int, size: int) -> bytes:
    return lba.to_bytes(3, "little") + struct.pack("<i", size)


def synthetic_disc(path: Path) -> bytes:
    container, scripts = field_container()
    image = bytearray(0x40 * extractor.USER_SECTOR)
    pvd = 16 * extractor.USER_SECTOR
    image[pvd + 1 : pvd + 6] = b"CD001"

    fat = bytearray()
    fat.extend(fat_record(0, 0))
    for _ in range(1, 184):
        fat.extend(fat_record(0, 0))
    fat.extend(fat_record(0x30, len(container)))
    fat.extend(fat_record(0xFFFFFF, 0))
    fat_offset = extractor.FAT_LBA * extractor.USER_SECTOR
    image[fat_offset : fat_offset + len(fat)] = fat

    directory = [0] * extractor.DIRECTORY_COUNT
    directory[extractor.FIELD_DIRECTORY] = 2
    directory_bytes = struct.pack(f"<{extractor.DIRECTORY_COUNT}H", *directory)
    directory_offset = extractor.DIRECTORY_LBA * extractor.USER_SECTOR
    image[directory_offset : directory_offset + len(directory_bytes)] = directory_bytes
    container_offset = 0x30 * extractor.USER_SECTOR
    image[container_offset : container_offset + len(container)] = container
    path.write_bytes(image)
    return scripts


def test_lzss_uses_low_to_high_zero_literal_groups() -> None:
    payload = b"12345678ABCDEFGH"
    expanded, consumed = extractor.lzss_decompress(literal_lzss(payload))
    assert expanded == payload
    assert consumed == 4 + 2 * 9


def test_lzss_rejects_target_reached_before_group_end() -> None:
    encoded = struct.pack("<I", 1) + bytes((0, ord("x")))
    with pytest.raises(ValueError, match="before the control group ends"):
        extractor.lzss_decompress(encoded)


def test_container_extracts_complete_scripts_file_and_entry_table() -> None:
    container, expected = field_container()
    scripts, metadata, container_metadata = extractor.extract_container_scripts(container)
    assert scripts == expected
    assert metadata["routine_row_count"] == 1
    assert metadata["bytecode_offset"] == 0xC4
    assert metadata["bytecode_size"] == 12
    assert metadata["routine_rows"][0]["routine_offsets"][11] == "0x000B"
    assert container_metadata["script_section"]["declared_expanded_size"] == len(expected)


def test_scripts_file_rejects_entry_at_bytecode_end() -> None:
    with pytest.raises(ValueError, match="outside bytecode"):
        extractor.parse_scripts_file(scripts_file(invalid_entry=True))


def test_opcode_table_and_high_level_control_flow() -> None:
    assert len(PRIMARY) == 256
    assert len(EXTENDED) == 227
    bytecode = bytes((
        0x02, 0x00, 0x04, 0x01, 0x00, 0x40, 0x09, 0x00,
        0x00,
        0x22,
        0x01, 0x0E, 0x00,
        0xFF,
        0x0D,
    ))
    metadata = {
        "bytecode_offset": 0,
        "bytecode_size": len(bytecode),
        "arrival_table_marker_present": False,
        "unsigned_variable_offsets": [],
        "routine_rows": [
            {
                "entity_id": 0,
                "routine_offsets": ["0x0000"] * 32,
            }
        ],
    }
    analysis = analyze_bytecode(bytecode, metadata)
    rendered, report = render_high_level_script(7, bytecode, metadata)
    assert analysis["instructions"][0].name == "ConditionalJmp"
    assert "signed entity_0_actor_sequence_gate @offset(0x0400);" in rendered
    assert "if (!(scene.entity_0_actor_sequence_gate == 1)) goto L_0009;" in rendered
    assert "documented " not in rendered
    assert "inferred " not in rendered
    assert "@evidence" not in rendered
    assert "event_state" not in rendered
    assert "event_flag" not in rendered
    assert "vm[" not in rendered
    assert "state.unresolved" not in rendered
    assert "actor[0].visible" not in rendered
    assert "actor.self.visible = true;" in rendered
    assert "routine[04..31] ->" in rendered
    assert "goto L_000E;" in rendered
    assert report["diagnostics"] == []
    assert report["instruction_bytes"] + report["data_bytes"] == len(bytecode)
    assert report["total_coverage_percent"] == 100.0


def test_actor_script_start_splits_routine_and_priority() -> None:
    instruction = decode_instruction(bytes((0x09, 0x01, 0x65)), 0)
    assert render_instruction(instruction).startswith(
        "start actor[1].routine[5] priority 3 wait_extended;"
    )


def test_playable_binding_does_not_claim_player_control() -> None:
    bind = render_instruction(decode_instruction(bytes((0x16, 0x00, 0x80)), 0))
    update = render_instruction(decode_instruction(bytes((0xA7,)), 0))
    assert bind.startswith("actor.bind_playable_character(character: 0);")
    assert update.startswith("actor.process_player_control_if_owned();")


@pytest.mark.parametrize(
    ("encoded", "statement"),
    (
        ("FE 4F", "inventory.enable_field_menu();"),
        ("FE 50", "inventory.disable_field_menu();"),
        ("FE 53", "world.enable_encounters_field_menu_and_compass();"),
        ("FE 54", "world.disable_encounters_field_menu_and_compass();"),
    ),
)
def test_field_menu_control_opcodes_render_semantically(
    encoded: str, statement: str
) -> None:
    instruction = decode_instruction(bytes.fromhex(encoded), 0)
    assert render_instruction(instruction).startswith(statement)


def test_proven_variable_writes_and_mutations_render_semantically() -> None:
    symbols = {
        offset: VariableSymbol(
            offset=offset,
            name=name,
            scope=scope,
            value_type="signed",
            evidence="test",
            confidence="documented",
        )
        for offset, name, scope in (
            (0x0002, "map_entry_point", "persistent"),
            (0x000A, "event_timer", "persistent"),
            (0x0400, "result_x", "scene"),
            (0x0402, "result_z", "scene"),
            (0x0404, "result_y", "scene"),
            (0x0406, "other", "scene"),
        )
    }

    position = render_instruction(
        decode_instruction(bytes((0x2D, 0xFB, 0x00, 0x04, 0x02, 0x04, 0x04, 0x04)), 0),
        symbols,
    )
    assert "movement.get_actor_position(actor: self)" in position
    assert "-> (scene.result_x, scene.result_z, scene.result_y)" in position

    actor_rotation = render_instruction(
        decode_instruction(bytes((0xFE, 0x75, 0xFB, 0x06, 0x04)), 0),
        symbols,
    )
    assert "actor: self" in actor_rotation
    assert "-> (scene.other)" in actor_rotation

    timer = render_instruction(
        decode_instruction(bytes((0x94, 0x0A, 0x80, 0x1E, 0x80)), 0), symbols
    )
    assert "-> (persistent.event_timer)" in timer

    shift = render_instruction(
        decode_instruction(bytes((0x41, 0x00, 0x04, 0x02, 0x80)), 0), symbols
    )
    assert "scene.result_x <<= 2;" in shift

    indexed_bit = render_instruction(
        decode_instruction(bytes((0xFE, 0x0A, 0x05, 0x40)), 0), symbols
    )
    assert "scene.result_x |= (1 << 5);" in indexed_bit


def test_camera_read_write_mode_changes_the_rendered_effect() -> None:
    symbol = VariableSymbol(
        offset=0x0400,
        name="camera_yaw",
        scope="scene",
        value_type="signed",
        evidence="test",
        confidence="documented",
    )
    read_mode = render_instruction(
        decode_instruction(bytes((0xAF, 0x00, 0x04, 0x00)), 0), {0x0400: symbol}
    )
    write_mode = render_instruction(
        decode_instruction(bytes((0xAF, 0x00, 0x02, 0x01)), 0), {0x0400: symbol}
    )

    assert "camera.read_or_write_camera_yaw() -> (scene.camera_yaw);" in read_mode
    assert "camera.yaw = 512;" in write_mode


def test_timer_configuration_does_not_invent_a_vm_write() -> None:
    bytecode = bytes((0x95, 0x00, 0x96, 0x00))
    metadata = {
        "bytecode_offset": 0,
        "bytecode_size": len(bytecode),
        "arrival_table_marker_present": False,
        "unsigned_variable_offsets": [],
        "routine_rows": [{"entity_id": 0, "routine_offsets": ["0x0000"] * 32}],
    }

    rendered, _ = render_high_level_script(0, bytecode, metadata)

    assert "event_timer @offset(0x000A)" not in rendered


def test_generated_operation_catalog_is_complete_and_current() -> None:
    catalog = render_catalog()
    catalog_path = MODULE_PATH.parents[1] / "docs" / "OPERATION_CATALOG.md"
    assert catalog_path.read_text(encoding="utf-8") == catalog
    assert catalog.count("| `FE ") == 227
    assert sum(f"| `{opcode:02X}` |" in catalog for opcode in range(256)) == 256
    assert "actor.bind_playable_character(character: value)" in catalog


def test_variable_skip_triplets_discovers_computed_jump_table() -> None:
    bytecode = bytes((
        0xA6, 0x00, 0x04,
        0x01, 0x09, 0x00,
        0x01, 0x0A, 0x00,
        0x22,
        0x23,
    ))
    metadata = {
        "bytecode_offset": 0,
        "bytecode_size": len(bytecode),
        "arrival_table_marker_present": False,
        "unsigned_variable_offsets": [],
        "routine_rows": [{"entity_id": 0, "routine_offsets": ["0x0000"] * 32}],
    }
    analysis = analyze_bytecode(bytecode, metadata)
    rendered, report = render_high_level_script(9, bytecode, metadata)
    assert sorted(analysis["instructions"]) == [0, 3, 6, 9, 10]
    assert "dispatch_index @offset(0x0400)" in rendered
    assert "flow.dispatch_triplet_table(index: scene.dispatch_index);" in rendered
    assert report["instruction_coverage_percent"] == 100.0
    assert report["total_coverage_percent"] == 100.0


def test_end_to_end_iso_extraction(tmp_path: Path) -> None:
    disc = tmp_path / "disc.iso"
    expected = synthetic_disc(disc)
    output = tmp_path / "output"
    manifest = extractor.extract_field_scripts([disc], output)

    assert manifest["summary"]["unique_scripts_files"] == 1
    assert manifest["summary"]["field_occurrences"] == 1
    resource = manifest["resources"][0]
    assert (output / resource["asset_path"]).read_bytes() == expected
    catalog = output / "catalog" / "disc-01" / f"field-0000_{resource['sha256'][:10]}"
    assert (catalog / "scripts.bin").read_bytes() == expected
    assert (catalog / "bytecode.bin").read_bytes() == expected[0xC4:]
    assert (catalog / "variable-types.bin").read_bytes() == expected[:0x80]
    readable = (catalog / "script.xgs").read_text(encoding="utf-8")
    assert "field 0 {" in readable
    assert "initialize     ->" in readable
    assert "movement.move_actor_to_position(" in readable
    assert "vm[" not in readable
    assert "byte(" not in readable
    assert resource["readable_script"]["total_coverage_percent"] == 100.0
