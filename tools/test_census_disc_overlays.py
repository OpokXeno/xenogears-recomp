from __future__ import annotations

import struct
import tomllib
from pathlib import Path

import pytest

import census_disc_overlays as census
from census_disc_overlays import (
    DISC2_MANIFEST,
    KNOWN_DISC1_SHA256,
    KNOWN_DISC2_SHA256,
    SENTINEL_LBA,
    _manifest_images,
    _zero_extent_hashes,
    analyze_mips,
    lzss_decompress_with_status,
    map_physical_routes,
    packet_offsets,
    parse_fat_table,
    recognize_disc,
    select_source_manifest,
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


def test_disc_identity_selects_its_manifest_unless_explicitly_overridden(
    tmp_path: Path,
) -> None:
    disc1 = recognize_disc(KNOWN_DISC1_SHA256)
    disc2 = recognize_disc(KNOWN_DISC2_SHA256)

    assert disc1 is not None and disc1.id == "disc1" and disc1.number == 1
    assert disc2 is not None and disc2.id == "disc2" and disc2.number == 2
    assert select_source_manifest(disc2, None) == DISC2_MANIFEST
    assert select_source_manifest(disc2, tmp_path / "explicit.toml") == (
        tmp_path / "explicit.toml"
    )
    assert recognize_disc("0" * 64) is None
    assert select_source_manifest(None, None) is None


def test_disc2_source_manifest_reuses_disc1_byte_identities() -> None:
    expected_sectors = {
        "movie-str-lib-image": 172873,
        "battling-image": 173205,
        "field-image": 173245,
        "world-image": 173307,
        "battle-image": 173353,
        "menu-image": 173435,
        "movie-image": 173470,
        "field-runtime-diagnostics-image": 184997,
        "battle-result-image": 226551,
        "member-change-menu-image": 226640,
        "enter-name-menu-image": 226729,
        "shop-menu-image": 226744,
        "gear-shop-menu-image": 226771,
        "gear-helper-image": 226888,
        "battle-debug-setup-menu-image": 226919,
        "battle-runtime-debug-image": 226972,
        "battle-loader-image": 227006,
        "battle-event-image": 244380,
        "battle-green-framebuffer-grid-image": 254102,
        "battle-curved-sprite-ribbon-image": 254104,
        "battle-polygon-shatter-image": 254105,
        "battle-velocity-sprite-clone-strip-image": 254107,
        "battle-fixed-origin-sprite-marquee-image": 254109,
        "battle-framebuffer-ripple-dissolve-image": 254110,
    }
    source_document = tomllib.loads(DISC2_MANIFEST.read_text(encoding="utf-8"))
    identity_path = DISC2_MANIFEST.parent / source_document["identity_manifest"]
    disc1_images = {image["id"]: image for image in _manifest_images(identity_path)}
    disc2_images = _manifest_images(DISC2_MANIFEST, KNOWN_DISC2_SHA256)

    assert source_document["disc_sha256"] == KNOWN_DISC2_SHA256
    assert all(
        set(source) == {"id", "source_sector"}
        for source in source_document["sources"]
    )
    assert {
        image["id"]: image["source_sector"] for image in disc2_images
    } == expected_sectors
    for image in disc2_images:
        shared_identity = {
            key: value for key, value in image.items() if key != "source_sector"
        }
        disc1_identity = {
            key: value
            for key, value in disc1_images[image["id"]].items()
            if key != "source_sector"
        }
        assert shared_identity == disc1_identity

    with pytest.raises(ValueError, match="does not match the disc SHA-256"):
        _manifest_images(DISC2_MANIFEST, KNOWN_DISC1_SHA256)


def test_build_census_reports_recognized_disc2_and_automatic_manifest(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    disc_path = tmp_path / "disc2.bin"
    disc_path.write_bytes(bytes(2048))

    class SyntheticDisc:
        path = disc_path
        sector_size = 2048
        user_offset = 0

        @staticmethod
        def read_user_data(lba: int, size: int) -> bytes:
            if lba == census.FAT_LBA:
                return b"".join(
                    (
                        _entry(0, -1),
                        _entry(0, 0),
                        _entry(SENTINEL_LBA, 0),
                    )
                )
            if lba == census.DIRECTORY_LBA:
                return bytes(census.DIRECTORY_COUNT * 2)
            raise AssertionError(f"unexpected synthetic disc read at {lba} for {size}")

    loaded_manifests: list[tuple[Path, str | None]] = []
    monkeypatch.setattr(census, "open_disc", lambda _path: SyntheticDisc())
    monkeypatch.setattr(census, "_file_sha256", lambda _path: KNOWN_DISC2_SHA256)
    monkeypatch.setattr(
        census,
        "_manifest_images",
        lambda path, sha256=None: loaded_manifests.append((path, sha256)) or [],
    )

    result = census.build_census(disc_path)

    assert result["recognized_disc"] == "disc2"
    assert result["source_manifest"] == "annotations/overlays/disc2-images.toml"
    assert "known_disc1" not in result
    assert loaded_manifests == [(DISC2_MANIFEST, KNOWN_DISC2_SHA256)]


def test_large_zero_extent_is_classified_without_code_analysis() -> None:
    class ZeroDisc:
        @staticmethod
        def read_user_data(_lba: int, size: int) -> bytes:
            return bytes(size)

    class NonzeroDisc:
        @staticmethod
        def read_user_data(_lba: int, size: int) -> bytes:
            return bytes(size - 1) + b"X"

    assert _zero_extent_hashes(ZeroDisc(), 100, 2 * 1024 * 1024 + 17) == (
        "0b5f645725e6aa767bcaa0838f4e22a623d0f308b45127aaf5c1c7d20b51eb14",
        "4107AAB4",
    )
    assert _zero_extent_hashes(NonzeroDisc(), 100, 4096) is None
