from __future__ import annotations

import pytest

from finalize_static_overlays import generate_dispatch_shards, split_overlay_body
from compile_overlays import parse_game_identity


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
        {
            "addr": address,
            "symbol": f"fixture_func_{address:08X}",
            "crc": address,
            "ranges": [(address, 4)],
            "resume": 0,
        }
        for address in (0x80010000, 0x80010010, 0x80010020, 0x80010030)
    ]
    images = [{
        "load_addr": 0x80010000,
        "size": 0x40,
        "crc": 0x12345678,
        "ranges": [(0x80010000, 0x40)],
    }]

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
