from __future__ import annotations

import struct

import pytest

from census_disc_overlays import (
    SENTINEL_LBA,
    analyze_mips,
    lzss_decompress_with_status,
    map_physical_routes,
    packet_offsets,
    parse_fat_table,
)


def _entry(lba: int, size: int) -> bytes:
    return lba.to_bytes(3, "little") + struct.pack("<i", size)


def test_fat_parser_preserves_xa_special_and_file_records() -> None:
    table = b"".join(
        (
            _entry(100, -2),
            _entry(101, 0),
            _entry(102, 0),
            _entry(200, 2049),
            _entry(SENTINEL_LBA, 0),
        )
    )

    entries, xa_children = parse_fat_table(table, 1000)

    assert xa_children == 2
    assert entries[-1].lba == 200
    assert entries[-1].size == 2049


def test_fat_parser_rejects_out_of_disc_extent() -> None:
    table = b"".join(
        (_entry(1, -1), _entry(999, 4096), _entry(SENTINEL_LBA, 0))
    )

    with pytest.raises(ValueError, match="outside the disc"):
        parse_fat_table(table, 1000)


def test_directory_routes_use_one_based_encoded_starts() -> None:
    table = [0] * 64
    table[0x10] = 101
    table[0x11] = 104
    table[0x12] = 105

    routes = map_physical_routes(table, 110)

    assert routes[100] == [{"directory": "0x10", "file_id": "0x01"}]
    assert routes[102] == [{"directory": "0x10", "file_id": "0x03"}]
    assert routes[103] == [{"directory": "0x11", "file_id": "0x01"}]
    assert routes[104] == [{"directory": "0x12", "file_id": "0x01"}]


def test_lzss_reports_complete_and_guest_compatible_truncated_streams() -> None:
    complete = (3).to_bytes(4, "little") + b"\x00abc"
    truncated = (4).to_bytes(4, "little") + b"\x00a"

    complete_result = lzss_decompress_with_status(complete)
    truncated_result = lzss_decompress_with_status(truncated)

    assert complete_result.payload == b"abc"
    assert not complete_result.truncated
    assert truncated_result.payload == b"a\x00\x00\x00"
    assert truncated_result.truncated


def test_packet_offsets_require_a_complete_monotonic_layout() -> None:
    packet = struct.pack("<IIII", 2, 16, 20, 24) + b"a" * 8

    assert packet_offsets(packet) == [16, 20, 24]
    assert packet_offsets(packet[:-1]) is None


def test_mips_analysis_recovers_a_sharp_jal_prologue_base() -> None:
    base = 0x801C5000
    offsets = [0x100, 0x240, 0x540, 0x980, 0xF00, 0x1680, 0x2040, 0x2C00]
    data = bytearray(0x4000)
    for index, offset in enumerate(offsets):
        struct.pack_into("<I", data, offset, 0x27BDFFE0)
        struct.pack_into("<I", data, offset + 4, 0x03E00008)
        target = base + offset
        struct.pack_into("<I", data, 0x20 + index * 4, 0x0C000000 | ((target >> 2) & 0x03FFFFFF))

    result = analyze_mips(bytes(data))

    assert result["signal"] == "strong"
    assert result["recovered_base"] == "0x801C5000"
    assert result["base_score"] == len(offsets)


def test_mips_analysis_retains_call_sparse_code_as_a_weak_candidate() -> None:
    data = bytearray(32)
    struct.pack_into("<I", data, 0, 0x27BDFFE0)
    struct.pack_into("<I", data, 4, 0x03E00008)

    result = analyze_mips(bytes(data))

    assert result["signal"] == "weak"
    assert result["recovered_base"] is None
