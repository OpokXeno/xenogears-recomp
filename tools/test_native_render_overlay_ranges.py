from pathlib import Path
import hashlib
import struct

import pytest

from native_render_overlay_ranges import (
    OverlayRangeError,
    emit_cold_cutover_table,
    emit_source_plan,
    merge_source_plans,
    load_overlay_range_variants,
    source_plan_for_overlay_ranges,
)


EXACT_ARTIFACT_BASE = 0x8006FAF0


def contract_text(digest: str) -> str:
    return f'''schema = "xg-render-overlay-ranges/v1"
[[variants]]
id = "fixture"
base_address = "0x80001000"
required_ranges = [{{ start = "0x80001004", size = 4, sha256 = "{digest}" }}]
cutovers = [{{ pc = "0x80001004", instruction = "0x12345678", transfer = "return", continuation = "0x00000000" }}]
'''


def exact_artifact_contract(
    data: bytes,
    *,
    required_start: int = EXACT_ARTIFACT_BASE,
    required_size: int | None = None,
    cutover_pc: int = EXACT_ARTIFACT_BASE,
    transfer: str = "return",
    continuation: int = 0,
) -> str:
    base = EXACT_ARTIFACT_BASE
    size = len(data) if required_size is None else required_size
    offset = required_start - base
    range_digest = hashlib.sha256(data[offset:offset + size]).hexdigest()
    artifact_digest = hashlib.sha256(data).hexdigest()
    instruction = struct.unpack_from("<I", data)[0]
    return f'''schema = "xg-render-overlay-ranges/v1"
[[variants]]
id = "exact-fixture"
base_address = "0x{base:08x}"
artifact_size = {len(data)}
artifact_sha256 = "{artifact_digest}"
required_ranges = [{{ start = "0x{required_start:08x}", size = {size}, sha256 = "{range_digest}" }}]
cutovers = [{{ pc = "0x{cutover_pc:08x}", instruction = "0x{instruction:08x}", transfer = "{transfer}", continuation = "0x{continuation:08x}" }}]
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


@pytest.mark.parametrize("artifact_size", [29779, 260862])
def test_terminal_partial_word_is_accepted_and_hashed_byte_exactly(
    tmp_path: Path, artifact_size: int,
) -> None:
    data = struct.pack("<I", 0x12345678) + bytes(artifact_size - 4)
    path = tmp_path / "ranges.toml"
    path.write_text(exact_artifact_contract(data), encoding="utf-8")

    variants = load_overlay_range_variants(path)
    assert variants[0].artifact_size == artifact_size
    assert variants[0].required_ranges[0].size == artifact_size
    assert source_plan_for_overlay_ranges(
        variants, data, EXACT_ARTIFACT_BASE
    ) == (
        "psxrecomp-source-observation-plan-v5\n"
        "cutover 8006FAF0 12345678 return 00000000\n"
    )

    mutated = bytearray(data)
    mutated[-1] ^= 1
    assert source_plan_for_overlay_ranges(
        variants, bytes(mutated), EXACT_ARTIFACT_BASE
    ) is None


def test_unaligned_required_range_start_is_rejected(tmp_path: Path) -> None:
    data = struct.pack("<I", 0x12345678) + b"abc"
    path = tmp_path / "ranges.toml"
    path.write_text(
        exact_artifact_contract(
            data, required_start=EXACT_ARTIFACT_BASE + 1,
            required_size=6),
        encoding="utf-8",
    )

    with pytest.raises(OverlayRangeError, match="required range is not aligned"):
        load_overlay_range_variants(path)


def test_nonterminal_partial_word_range_is_rejected(tmp_path: Path) -> None:
    data = struct.pack("<I", 0x12345678) + b"abcd"
    path = tmp_path / "ranges.toml"
    path.write_text(
        exact_artifact_contract(data, required_size=7), encoding="utf-8")

    with pytest.raises(OverlayRangeError, match="required range is not aligned"):
        load_overlay_range_variants(path)


def test_unaligned_cutover_pc_is_rejected(tmp_path: Path) -> None:
    data = struct.pack("<I", 0x12345678) + b"abcd"
    path = tmp_path / "ranges.toml"
    path.write_text(
        exact_artifact_contract(
            data, cutover_pc=EXACT_ARTIFACT_BASE + 1),
        encoding="utf-8",
    )

    with pytest.raises(OverlayRangeError, match="cutover address is not aligned"):
        load_overlay_range_variants(path)


def test_cutover_cannot_overrun_terminal_partial_word(tmp_path: Path) -> None:
    data = struct.pack("<I", 0x12345678) + b"abc"
    path = tmp_path / "ranges.toml"
    path.write_text(
        exact_artifact_contract(
            data, cutover_pc=EXACT_ARTIFACT_BASE + 4),
        encoding="utf-8",
    )
    variants = load_overlay_range_variants(path)

    with pytest.raises(
        OverlayRangeError,
        match="authenticated range cutover overruns artifact",
    ):
        source_plan_for_overlay_ranges(variants, data, EXACT_ARTIFACT_BASE)


def test_emit_source_plan_materializes_codegen_input(tmp_path: Path) -> None:
    data = bytearray(16)
    struct.pack_into("<I", data, 4, 0x12345678)
    manifest = tmp_path / "ranges.toml"
    manifest.write_text(
        contract_text(hashlib.sha256(data[4:8]).hexdigest()),
        encoding="utf-8",
    )
    artifact = tmp_path / "overlay.bin"
    artifact.write_bytes(data)
    output = tmp_path / "plans" / "overlay.plan"

    emit_source_plan(
        load_overlay_range_variants(manifest), artifact, 0x80001000, output)

    assert output.read_text(encoding="ascii") == (
        "psxrecomp-source-observation-plan-v5\n"
        "cutover 80001004 12345678 return 00000000\n"
    )


def test_static_aot_codegen_consumes_authenticated_source_plan() -> None:
    root = Path(__file__).resolve().parents[1]
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")

    assert "emit-source-plan" in cmake
    assert "run_with_source_observation_plan.py" in cmake
    assert '--plan "${XG_OVERLAY_SOURCE_PLAN}"' in cmake


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


def test_retail_world_overlay_emits_authenticated_plan() -> None:
    root = Path(__file__).resolve().parents[1]
    variants = load_overlay_range_variants(
        root / "native_renderer" / "xg_render_overlay_ranges.toml"
    )
    artifact = (root / "overlays" / "worldmap_module.bin").read_bytes()
    assert hashlib.sha256(artifact).hexdigest() == (
        "4c15fd32b3a03d7cd5ea4403dcaabc70abaf99b6aaca65d63d6866803edaac70"
    )
    plan = source_plan_for_overlay_ranges(variants, artifact, 0x8006FAF0)
    assert plan is not None
    for seam in (
        "cutover 800979CC 3C04800A observe 00000000",
        "cutover 80097AC8 0C011225 observe 00000000",
        "cutover 80097ADC 87A50012 observe 00000000",
    ):
        assert seam in plan


def test_world_contract_has_independent_family_authority() -> None:
    root = Path(__file__).resolve().parents[1]
    variants = load_overlay_range_variants(
        root / "native_renderer" / "xg_render_overlay_ranges.toml"
    )
    identifiers = {variant.identifier for variant in variants}
    assert identifiers == {
        "ft4-2c-callers-v1",
        "ft4-2e-projected-v1",
        "field-sprite-xy-override-v1",
        "field-target-polylines-v1",
        "movie-field-owner-v1",
        "movie-standalone-owner-v1",
        "movie-str-frame-complete-v1",
        "world-full-sky-v1",
        "world-full-animated-textures-v1",
        "world-full-shared-textures-v1",
        "world-full-terrain-water-v1",
        "world-partial-terrain-water-v1",
        "world-full-models-v1",
        "world-full-actor-sprites-v1",
        "world-full-entity-shadows-v1",
        "world-full-clouds-v1",
        "world-full-effects-v1",
        "world-full-decorations-v1",
        "world-full-horizon-v1",
        "world-full-minimap-v1",
    }
    field_polylines = next(
        variant for variant in variants
        if variant.identifier == "field-target-polylines-v1"
    )
    assert field_polylines.base_address == 0x801CD000
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
    xy_override = next(variant for variant in variants
                       if variant.identifier == "field-sprite-xy-override-v1")
    assert xy_override.artifact_size is None
    assert {(required.start, required.size, required.sha256.hex())
            for required in xy_override.required_ranges} == {
        (0x801C93A8, 2084,
         "ea5ce2d39eb6086053103582c85b3f3f715baf14e7e6200d4dcd73027500fa2c"),
    }
    assert [(cutover.pc, cutover.instruction, cutover.transfer)
            for cutover in xy_override.cutovers] == [
        (0x801C9984, 0xA4620008, "observe"),
        (0x801C9B80, 0x02801021, "observe"),
    ]
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
            "world-partial-terrain-water-v1",
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
    partial_terrain = next(
        variant for variant in variants
        if variant.identifier == "world-partial-terrain-water-v1"
    )
    assert partial_terrain.base_address == 0x80090000
    assert partial_terrain.artifact_size == 40964
    assert partial_terrain.artifact_sha256.hex() == (
        "6d4d865dea73b55b4decaa9027701ca65f15f2375c4f08890a92e0c3f449f45d"
    )
    assert {(required.start, required.size, required.sha256.hex())
            for required in partial_terrain.required_ranges} == {
        (0x80090000, 40964,
         "6d4d865dea73b55b4decaa9027701ca65f15f2375c4f08890a92e0c3f449f45d"),
    }
    assert [(cutover.pc, cutover.instruction, cutover.transfer)
            for cutover in partial_terrain.cutovers] == [
        (0x8009932C, 0x27BDFFC8, "return"),
    ]
    full_world = [variant for variant in variants
                  if variant.identifier.startswith("world-full-")]
    assert full_world
    assert all(variant.base_address == 0x8006F000
               for variant in full_world)
    assert all(variant.load_address == 0x8006FAF0
               for variant in full_world)
    terrain = next(
        variant for variant in variants
        if variant.identifier == "world-full-terrain-water-v1"
    )
    assert (0x800979C8, 504,
            "192f5c3be6870f95277ee0b5f8cd4e21b999ea9a0ce7897e8e1a821dac32d2fa") in {
        (required.start, required.size, required.sha256.hex())
        for required in terrain.required_ranges
    }
    assert {
        (0x800979CC, 0x3C04800A, "observe"),
        (0x80097AC8, 0x0C011225, "observe"),
        (0x80097ADC, 0x87A50012, "observe"),
    } <= {
        (cutover.pc, cutover.instruction, cutover.transfer)
        for cutover in terrain.cutovers
    }
    shared_textures = next(
        variant for variant in variants
        if variant.identifier == "world-full-shared-textures-v1"
    )
    assert {(required.start, required.size, required.sha256.hex())
            for required in shared_textures.required_ranges} == {
        (0x8008440C, 372,
         "b906f63b893759181a6c6c6253f667f0d351cb79f1567f308405443e803e3202"),
    }
    assert [(cutover.pc, cutover.instruction, cutover.transfer)
            for cutover in shared_textures.cutovers] == [
        (0x80084410, 0xAFBF0038, "observe"),
        (0x80084508, 0x0C011225, "observe"),
        (0x8008451C, 0x87A50012, "observe"),
    ]
    animated_textures = next(
        variant for variant in variants
        if variant.identifier == "world-full-animated-textures-v1"
    )
    assert {(required.start, required.size, required.sha256.hex())
            for required in animated_textures.required_ranges} == {
        (0x80074F2C, 260,
         "71f94afc28d28891b18ff24eae1bde26a33c128fa90e93d65550dade7e061150"),
        (0x80075104, 292,
         "ef57b7be41c85093c460590ef60b7bf8cde3bcf0b9724b27fd523d4a4e6e53dc"),
    }
    assert [(cutover.pc, cutover.instruction, cutover.transfer)
            for cutover in animated_textures.cutovers] == [
        (0x80074F30, 0x8C42CC9C, "observe"),
        (0x80074FEC, 0x0C011225, "observe"),
        (0x80075018, 0x8FB20020, "observe"),
        (0x80075108, 0x8C42CD64, "observe"),
        (0x800751E4, 0x0C011225, "observe"),
        (0x80075210, 0x8FB20020, "observe"),
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
