from pathlib import Path
import hashlib
import struct

import pytest

from native_render_overlay_ranges import (
    OverlayRangeError,
    emit_cold_cutover_table,
    merge_source_plans,
    load_overlay_range_variants,
    source_plan_for_overlay_ranges,
)


def contract_text(digest: str) -> str:
    return f'''schema = "xg-render-overlay-ranges/v1"
[[variants]]
id = "fixture"
base_address = "0x80001000"
required_ranges = [{{ start = "0x80001004", size = 4, sha256 = "{digest}" }}]
cutovers = [{{ pc = "0x80001004", instruction = "0x12345678", transfer = "return", continuation = "0x00000000" }}]
'''


def test_range_match_and_mutation(tmp_path: Path) -> None:
    data = bytearray(16)
    struct.pack_into("<I", data, 4, 0x12345678)
    digest = hashlib.sha256(data[4:8]).hexdigest()
    path = tmp_path / "ranges.toml"
    path.write_text(contract_text(digest), encoding="utf-8")
    variants = load_overlay_range_variants(path)
    plan = source_plan_for_overlay_ranges(variants, bytes(data), 0x80001000)
    assert plan == (
        "psxrecomp-source-observation-plan-v5\n"
        "cutover 80001004 12345678 return 00000000\n"
    )
    data[4] ^= 1
    assert source_plan_for_overlay_ranges(
        variants, bytes(data), 0x80001000
    ) is None


def test_full_artifact_identity_rejects_mutation_outside_required_range(
    tmp_path: Path,
) -> None:
    data = bytearray(16)
    struct.pack_into("<I", data, 4, 0x12345678)
    range_digest = hashlib.sha256(data[4:8]).hexdigest()
    artifact_digest = hashlib.sha256(data).hexdigest()
    path = tmp_path / "ranges.toml"
    path.write_text(
        contract_text(range_digest).replace(
            'base_address = "0x80001000"',
            'base_address = "0x80001000"\n'
            f'artifact_size = {len(data)}\n'
            f'artifact_sha256 = "{artifact_digest}"',
        ),
        encoding="utf-8",
    )
    variants = load_overlay_range_variants(path)
    assert source_plan_for_overlay_ranges(
        variants, bytes(data), 0x80001000
    ) is not None
    data[12] ^= 1
    assert source_plan_for_overlay_ranges(
        variants, bytes(data), 0x80001000
    ) is None


def test_plan_merge_deduplicates() -> None:
    plan = (
        "psxrecomp-source-observation-plan-v5\n"
        "cutover 80001004 12345678 return 00000000\n"
    )
    assert merge_source_plans(plan, plan) == plan
    with pytest.raises(OverlayRangeError):
        merge_source_plans("wrong\n")


def test_cold_cutover_table_contains_non_control_overlay_seams(
    tmp_path: Path,
) -> None:
    root = Path(__file__).resolve().parents[1]
    variants = load_overlay_range_variants(
        root / "native_renderer" / "xg_render_overlay_ranges.toml"
    )
    output = tmp_path / "cutovers.inc"
    emit_cold_cutover_table(variants, output)
    table = output.read_text(encoding="ascii")
    for pc in (0x001E927C, 0x001E92C4, 0x001E920C, 0x001E7C50,
               0x001E76E0):
        assert f"0x{pc:08x}" in table
    assert "0x001cf85c" not in table


def test_world_contract_has_independent_family_authority() -> None:
    root = Path(__file__).resolve().parents[1]
    variants = load_overlay_range_variants(
        root / "native_renderer" / "xg_render_overlay_ranges.toml"
    )
    identifiers = {variant.identifier for variant in variants}
    assert identifiers == {
        "ft4-2c-callers-v1",
        "ft4-2e-projected-v1",
        "field-target-polylines-v1",
        "world-full-sky-v1",
        "world-full-terrain-water-v1",
        "world-full-models-v1",
        "world-full-actor-sprites-v1",
        "world-full-entity-shadows-v1",
        "world-full-clouds-v1",
        "world-full-effects-v1",
        "world-full-decorations-v1",
        "world-full-horizon-v1",
        "world-full-minimap-v1",
    }
    ft4 = next(variant for variant in variants
               if variant.identifier == "ft4-2c-callers-v1")
    assert ft4.artifact_size == 270340
    assert ft4.artifact_sha256.hex() == "6b9f505b5ea77f3bb7222e78d2b2550f038fb319db399b7d862b4bd236bb2dbe"
    assert ft4.producer_scope is not None
    assert (ft4.producer_scope.entry, ft4.producer_scope.return_pc,
            ft4.producer_scope.writer, ft4.producer_scope.opcode) == (
        0x801E927C, 0x801E92C4, 0x801E92B4, 0x2C,
    )
    assert len(ft4.producer_callers) == 10
    assert {caller.semantic_family for caller in ft4.producer_callers} == {
        "rectangle-helper", "static-quad", "dynamic-uv-template",
    }
    assert all(caller.semantic_source == "caller-state"
               for caller in ft4.producer_callers)
    projected = next(variant for variant in variants
                     if variant.identifier == "ft4-2e-projected-v1")
    assert projected.artifact_size == 270340
    assert projected.artifact_sha256.hex() == (
        "75c675f9736365dded5373bbd851b4a8c763ba34c167ef223c47032e8068f69f"
    )
    assert projected.producer_scope is not None
    assert (projected.producer_scope.entry,
            projected.producer_scope.return_pc,
            projected.producer_scope.writer,
            projected.producer_scope.opcode) == (
        0x801E91C4, 0x801E9204, 0x801E91F4, 0x2E,
    )
    assert {caller.semantic_family for caller in projected.producer_callers} == {
        "alternate-template-uv-material", "template-uv-material",
        "narrow-template-uv-material", "projected-xy-addprim",
        "projected-ft4-driver", "zero-u-template-material",
        "descriptor-template-material",
    }
    for variant in variants:
        assert variant.required_ranges
        if variant.identifier not in {
            "world-full-sky-v1",
            "world-full-effects-v1",
            "world-full-horizon-v1",
            "world-full-minimap-v1",
            "world-full-terrain-water-v1",
            "world-full-models-v1",
            "world-full-entity-shadows-v1",
            "world-full-decorations-v1",
        }:
            assert all(cutover.transfer == "observe"
                       for cutover in variant.cutovers)
    horizon = next(variant for variant in variants
                   if variant.identifier == "world-full-horizon-v1")
    assert {(required.start, required.size) for required in horizon.required_ranges} == {
        (0x80071A58, 16),
        (0x80071B50, 24),
        (0x800739B8, 332),
        (0x80073B04, 812),
        (0x8009A300, 64),
    }
    assert [(cutover.pc, cutover.instruction, cutover.transfer)
            for cutover in horizon.cutovers] == [
        (0x80073B04, 0x27BDFFC0, "return"),
        (0x80073E0C, 0x8FBF003C, "observe"),
    ]
    effects = next(variant for variant in variants
                   if variant.identifier == "world-full-effects-v1")
    assert {(required.start, required.size) for required in effects.required_ranges} == {
        (0x80071A58, 16),
        (0x80071A90, 32),
        (0x8008901C, 268),
        (0x80089748, 1328),
        (0x80089C78, 1616),
        (0x8009AFF0, 80),
        (0x8009B040, 320),
    }
    assert [(cutover.pc, cutover.instruction, cutover.transfer)
            for cutover in effects.cutovers] == [
        (0x80089C78, 0x27BDFFB0, "return"),
        (0x8008A294, 0x8FBF004C, "observe"),
    ]
    actor_sprites = next(
        variant for variant in variants
        if variant.identifier == "world-full-actor-sprites-v1"
    )
    assert {(required.start, required.size)
            for required in actor_sprites.required_ranges} == {
        (0x80071A58, 16),
        (0x80071AA8, 16),
        (0x80085CDC, 636),
    }
    assert [(cutover.pc, cutover.instruction, cutover.transfer)
            for cutover in actor_sprites.cutovers] == [
        (0x80085CDC, 0x3C02800A, "observe"),
        (0x80085F38, 0x8FBF0020, "observe"),
    ]
    minimap = next(variant for variant in variants
                   if variant.identifier == "world-full-minimap-v1")
    assert {(required.start, required.size)
            for required in minimap.required_ranges} == {
        (0x80071A58, 16),
        (0x80071B68, 32),
        (0x800740B8, 1244),
    }
    assert [(cutover.pc, cutover.instruction, cutover.transfer,
             cutover.continuation) for cutover in minimap.cutovers] == [
        (0x800740B8, 0x27BDFFC8, "observe", 0),
        (0x8007412C, 0x266400B8, "local", 0x80074298),
        (0x80074564, 0x8FBF0030, "observe", 0),
    ]
    shadow_contracts = {
        "world-full-terrain-water-v1": (
            (0x80071B1C, 28),
            (0x800996D4, 0x8FBF0034),
        ),
        "world-full-entity-shadows-v1": (
            (0x80071AB8, 12),
            (0x80074E24, 0x8FBF0064),
        ),
        "world-full-clouds-v1": (
            (0x80071B50, 24),
            (0x800876DC, 0x8FBF004C),
        ),
        "world-full-decorations-v1": (
            (0x80071AA8, 16),
            (0x80099E78, 0x8FB10008),
        ),
        "world-full-minimap-v1": (
            (0x80071B68, 32),
            (0x80074564, 0x8FBF0030),
        ),
    }
    for identifier, (caller_range, finish) in shadow_contracts.items():
        variant = next(item for item in variants
                       if item.identifier == identifier)
        assert caller_range in {
            (required.start, required.size)
            for required in variant.required_ranges
        }
        assert (*finish, "observe") in {
            (cutover.pc, cutover.instruction, cutover.transfer)
            for cutover in variant.cutovers
        }
