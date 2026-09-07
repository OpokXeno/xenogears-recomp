from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile

import pytest

from finalize_static_overlays import generate_dispatch_shards, split_overlay_body
from compile_overlays import parse_game_identity


CAPABILITY_ID = 0x1020304050607080
AUTHORITY_PROVENANCE = "authenticated-static-image-v1"
ARTIFACT_SHA256 = "ab" * 32


def authorized_variant(address: int, *, symbol: str | None = None) -> dict:
    return {
        "addr": address,
        "symbol": symbol or f"fixture_func_{address:08X}",
        "code_sha256": f"{address:064x}",
        "ranges": [(address, 4)],
        "resume": 0,
        "producer_entry": address,
        "artifact_base": 0x00010000,
        "artifact_size": 0x40,
        "artifact_sha256": ARTIFACT_SHA256,
        "capability_id": CAPABILITY_ID,
        "authority_provenance": AUTHORITY_PROVENANCE,
    }


def authorized_image() -> dict:
    return {
        "image_id": "fixture-image",
        "load_addr": 0x80010000,
        "size": 0x40,
        "code_sha256": "cd" * 32,
        "artifact_sha256": ARTIFACT_SHA256,
        "artifact_ranges": [(0x80010000, 0x40)],
        "capability_id": CAPABILITY_ID,
        "authority_provenance": AUTHORITY_PROVENANCE,
        "ranges": [(0x80010000, 0x40)],
    }


def test_overlay_body_is_split_only_at_function_boundaries() -> None:
    namespace = "ov_fixture_deadbeef"
    prefix = (
        '#include "psx_runtime.h"\n'
        f"static void {namespace}_helper(void) {{}}\n"
        f"void {namespace}_func_80010000(CPUState* cpu);\n"
        f"void {namespace}_func_80010010(CPUState* cpu);\n\n"
    )
    functions = [
        f"void {namespace}_func_{address:08X}(CPUState* cpu)\n"
        "{\n"
        "    (void)cpu;\n"
        "}\n\n"
        for address in (0x80010000, 0x80010010, 0x80010020)
    ]

    shards = split_overlay_body(
        prefix + "".join(functions), namespace, shard_count=3, line_budget=6
    )

    assert len(shards) == 3
    assert all('#include "psx_runtime.h"' in shard for shard in shards)
    combined = "".join(shards)
    for address in (0x80010000, 0x80010010, 0x80010020):
        definition = f"void {namespace}_func_{address:08X}(CPUState* cpu)\n{{"
        assert combined.count(definition) == 1


def test_overlay_body_rejects_insufficient_declared_slots() -> None:
    namespace = "ov_fixture_deadbeef"
    source = '#include "psx_runtime.h"\n' + "".join(
        f"void {namespace}_func_{address:08X}(CPUState* cpu)\n"
        "{\n"
        "    (void)cpu;\n"
        "}\n\n"
        for address in (0x80010000, 0x80010010, 0x80010020)
    )

    with pytest.raises(ValueError, match="only 2 slots"):
        split_overlay_body(source, namespace, shard_count=2, line_budget=1)


def test_dispatch_entries_are_partitioned_once_across_shards() -> None:
    identity = parse_game_identity("0" * 64, "1" * 64)
    variants = [
        authorized_variant(address)
        for address in (0x80010000, 0x80010010, 0x80010020, 0x80010030)
    ]
    images = [authorized_image()]

    main, shards = generate_dispatch_shards(
        variants, identity, images, shard_count=2
    )

    assert len(shards) == 2
    assert "psx_overlay_dispatch_shard_00" in main
    assert "psx_overlay_dispatch_shard_01" in main
    combined = "".join(shards)
    for address in (0x80010000, 0x80010010, 0x80010020, 0x80010030):
        assert combined.count(f"case 0x{address:08X}u:") == 1
    assert "psx_overlay_static_image_known" in main


def test_static_candidate_is_noted_with_exact_authority_before_native_call() -> None:
    identity = parse_game_identity("12" * 32, "34" * 32)
    variant = authorized_variant(0x80010010, symbol="fixture_native")
    variant["producer_entry"] = 0x80010000

    main, shards = generate_dispatch_shards(
        [variant], identity, [authorized_image()], shard_count=1
    )
    shard = shards[0]

    function_match = shard.index("0x00010010u, 0x4u")
    artifact_match = shard.index("0x00010000u, 0x40u")
    note = shard.index("psx_overlay_static_note_candidate_dispatch(")
    native_call = shard.index("fixture_native(cpu)")
    assert function_match < note
    assert artifact_match < note
    assert note < native_call
    assert "artifact_sha256" in shard
    assert "UINT64_C(0x1020304050607080)" in shard
    assert "0x80010000u, dispatch_pc" in shard
    assert "cpu, key, addr, &k_psx_overlay_static_identity" in main
    assert "0x12u" in main
    assert "0x34u" in main


def test_static_dispatch_rejects_ambiguous_overlapping_variants() -> None:
    identity = parse_game_identity("12" * 32, "34" * 32)
    first = authorized_variant(0x80010000, symbol="fixture_first")
    second = authorized_variant(0x80010000, symbol="fixture_second")
    second["code_sha256"] = "cd" * 32

    _main, shards = generate_dispatch_shards(
        [first, second], identity, [authorized_image()], shard_count=1
    )
    shard = shards[0]

    assert shard.count("if (selected != 0u)") == 2
    overlap_rejection = shard.index("if (selected != 0u)")
    overlap_return = shard.index("return 0;", overlap_rejection)
    notification = shard.index("psx_overlay_static_note_candidate_dispatch(")
    assert overlap_return < notification


@pytest.mark.parametrize("field", ["capability_id", "authority_provenance"])
def test_static_dispatch_rejects_unbound_artifact_authority(field: str) -> None:
    identity = parse_game_identity("12" * 32, "34" * 32)
    variant = authorized_variant(0x80010000)
    variant[field] = 0 if field == "capability_id" else "filename-inferred"

    with pytest.raises(ValueError, match="invalid artifact authority"):
        generate_dispatch_shards(
            [variant], identity, [authorized_image()], shard_count=1
        )


def test_generated_dispatch_executes_only_after_exact_candidate_notification() -> None:
    compiler = shutil.which("cc") or shutil.which("gcc")
    if compiler is None:
        pytest.skip("C compiler is required")
    identity = parse_game_identity("12" * 32, "34" * 32)
    direct = authorized_variant(0x80010000, symbol="fixture_native")
    overlap_a = authorized_variant(0x80010010, symbol="fixture_overlap_a")
    overlap_b = authorized_variant(0x80010010, symbol="fixture_overlap_b")
    overlap_a["code_sha256"] = "cd" * 32
    overlap_b["code_sha256"] = "ef" * 32
    main_source, shards = generate_dispatch_shards(
        [direct, overlap_a, overlap_b], identity, [authorized_image()], shard_count=1
    )
    harness = r'''
#include "overlay_loader.h"
#include "game_identity.h"
#include <stdint.h>
#include <string.h>

static int identity_ok = 1;
static int code_ok = 1;
static int note_ok = 1;
static int note_calls;
static int native_calls;
static int sequence;
static int note_sequence;
static int native_sequence;

int psx_game_identity_bind_static(const PsxGameIdentity *identity) {
    (void)identity;
    return identity_ok;
}
int psx_game_identity_gate(const PsxGameIdentity *identity) {
    return identity_ok && identity != 0;
}
int psx_overlay_static_code_matches(const uint32_t *ranges, uint32_t count,
                                    const uint8_t sha256[32]) {
    (void)count;
    (void)sha256;
    return ranges[0] == UINT32_C(0x00010000) ? code_ok : 1;
}
int psx_overlay_static_note_candidate_dispatch(
        const uint32_t *code_ranges, uint32_t code_count,
        const uint8_t code_sha256[32],
        const uint32_t *artifact_ranges, uint32_t artifact_count,
        const uint8_t artifact_sha256[32],
        const PsxGameIdentity *identity, uint64_t capability_id,
        uint32_t producer_entry, uint32_t dispatch_pc) {
    note_calls++;
    note_sequence = ++sequence;
    if (code_ranges[0] != UINT32_C(0x00010000) || code_ranges[1] != 4u ||
        code_count != 1u || code_sha256 == 0 ||
        artifact_ranges[0] != UINT32_C(0x00010000) ||
        artifact_ranges[1] != 0x40u || artifact_count != 1u ||
        artifact_sha256 == 0 || artifact_sha256[0] != 0xabu ||
        capability_id != UINT64_C(0x1020304050607080) ||
        producer_entry != UINT32_C(0x80010000) ||
        dispatch_pc != UINT32_C(0xA0010000))
        return 0;
    for (uint32_t index = 0; index < 32u; index++)
        if (identity->game_sha256[index] != 0x12u ||
            identity->manifest_sha256[index] != 0x34u)
            return 0;
    return note_ok;
}
void fixture_native(CPUState *cpu) {
    (void)cpu;
    native_calls++;
    native_sequence = ++sequence;
}
void fixture_overlap_a(CPUState *cpu) { (void)cpu; native_calls++; }
void fixture_overlap_b(CPUState *cpu) { (void)cpu; native_calls++; }
int psx_overlay_dispatch(CPUState *cpu, uint32_t addr);

static void reset_counts(void) {
    note_calls = native_calls = sequence = note_sequence = native_sequence = 0;
}
int main(void) {
    CPUState cpu = {0};
    if (!psx_overlay_dispatch(&cpu, UINT32_C(0xA0010000)) ||
        note_calls != 1 || native_calls != 1 ||
        note_sequence == 0 || note_sequence >= native_sequence)
        return 1;
    reset_counts();
    code_ok = 0;
    if (psx_overlay_dispatch(&cpu, UINT32_C(0x80010000)) ||
        note_calls != 0 || native_calls != 0)
        return 2;
    code_ok = 1;
    note_ok = 0;
    if (psx_overlay_dispatch(&cpu, UINT32_C(0xA0010000)) || native_calls != 0)
        return 3;
    reset_counts();
    note_ok = 1;
    identity_ok = 0;
    if (psx_overlay_dispatch(&cpu, UINT32_C(0x80010000)) ||
        note_calls != 0 || native_calls != 0)
        return 4;
    identity_ok = 1;
    reset_counts();
    if (psx_overlay_dispatch(&cpu, UINT32_C(0x80010010)) ||
        note_calls != 0 || native_calls != 0)
        return 5;
    return 0;
}
'''
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        main_c = temporary / "dispatch.c"
        shard_c = temporary / "dispatch_00.c"
        harness_c = temporary / "harness.c"
        executable = temporary / "dispatch-test"
        main_c.write_text(main_source, encoding="ascii")
        shard_c.write_text(shards[0], encoding="ascii")
        harness_c.write_text(harness, encoding="ascii")
        subprocess.run(
            [
                compiler,
                "-std=c11",
                "-DPSX_HAS_OVERLAY_DISPATCH=1",
                f"-I{root / 'psxrecomp' / 'runtime' / 'include'}",
                f"-I{root / 'native_renderer' / 'include'}",
                str(main_c),
                str(shard_c),
                str(harness_c),
                "-o",
                str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)
