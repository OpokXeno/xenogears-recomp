#!/usr/bin/env python3
"""Extract Xenogears Field VM script sections from retail discs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

SCHEMA = "xenogears-disc-field-scripts/v1"
FAT_LBA = 0x18
FAT_SECTORS = 0x10
DIRECTORY_LBA = 0x28
DIRECTORY_COUNT = 64
FIELD_DIRECTORY = 0x04
FIELD_FILE_BASE = 0xB8
FIELD_SECTION = 5
USER_SECTOR = 2048
MAX_STORED_EXTENT_SIZE = 64 * 1024 * 1024
MAX_SCRIPT_SECTION_SIZE = 8 * 1024 * 1024
SECTION_COUNT = 9
SECTION_SIZE_TABLE = 0x10C
SECTION_OFFSET_TABLE = 0x130
ENTITY_COUNT_OFFSET = 0x18C
ENTITY_TABLE_OFFSET = 0x190
SCRIPT_BITMAP_SIZE = 0x80
SCRIPT_ROW_COUNT_OFFSET = 0x80
SCRIPT_ROWS_OFFSET = 0x84
SCRIPT_ROW_SIZE = 0x40
ROUTINES_PER_ENTITY = 32


@dataclass(frozen=True)
class DiscLayout:
    source_path: Path
    data_path: Path
    sector_size: int
    user_offset: int

    @property
    def sector_count(self) -> int:
        return self.data_path.stat().st_size // self.sector_size

    def read_user_data(self, lba: int, size: int, *, padded: bool = False) -> bytes:
        if lba < 0 or size < 0:
            raise ValueError("disc extent has a negative LBA or size")
        read_size = (
            (size + USER_SECTOR - 1) // USER_SECTOR * USER_SECTOR
            if padded
            else size
        )
        sectors = (read_size + USER_SECTOR - 1) // USER_SECTOR
        output = bytearray()
        with self.data_path.open("rb") as source:
            for index in range(sectors):
                source.seek((lba + index) * self.sector_size + self.user_offset)
                chunk = source.read(USER_SECTOR)
                if len(chunk) != USER_SECTOR:
                    raise ValueError(f"disc ends inside extent at LBA {lba + index}")
                output.extend(chunk)
        return bytes(output[:read_size])


@dataclass(frozen=True)
class FatEntry:
    lba: int
    size: int


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def s32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _cue_layout(path: Path) -> DiscLayout:
    text = path.read_text(encoding="utf-8-sig")
    file_match = re.search(
        r'^\s*FILE\s+"([^"]+)"\s+BINARY\s*$',
        text,
        re.MULTILINE | re.IGNORECASE,
    )
    track_match = re.search(
        r"^\s*TRACK\s+01\s+(MODE[12]/(?:2048|2352))\s*$",
        text,
        re.MULTILINE | re.IGNORECASE,
    )
    index_match = re.search(
        r"^\s*INDEX\s+01\s+00:00:00\s*$",
        text,
        re.MULTILINE | re.IGNORECASE,
    )
    if not file_match or not track_match or not index_match:
        raise ValueError(f"{path}: CUE must describe track 01 at 00:00:00")
    binary = (path.parent / file_match.group(1)).resolve()
    if not binary.is_file():
        raise ValueError(f"{path}: CUE data file does not exist: {binary}")
    mode = track_match.group(1).upper()
    if mode.endswith("/2048"):
        layout = DiscLayout(path.resolve(), binary, 2048, 0)
    else:
        layout = DiscLayout(
            path.resolve(),
            binary,
            2352,
            24 if mode.startswith("MODE2") else 16,
        )
    if binary.stat().st_size % layout.sector_size:
        raise ValueError(f"{binary}: size is not sector aligned")
    return layout


def open_disc(path: Path) -> DiscLayout:
    path = path.resolve()
    if path.suffix.lower() == ".cue":
        return _cue_layout(path)
    if not path.is_file():
        raise ValueError(f"disc image does not exist: {path}")
    size = path.stat().st_size
    candidates: list[DiscLayout] = []
    if size % 2352 == 0:
        candidates.extend(
            (DiscLayout(path, path, 2352, 24), DiscLayout(path, path, 2352, 16))
        )
    if size % 2048 == 0:
        candidates.append(DiscLayout(path, path, 2048, 0))
    for candidate in candidates:
        try:
            if candidate.read_user_data(16, 7)[1:6] == b"CD001":
                return candidate
        except ValueError:
            pass
    raise ValueError(f"{path}: cannot identify ISO or MODE1/2 BIN layout")


def parse_fat(data: bytes, sector_count: int) -> list[FatEntry]:
    entries = []
    for offset in range(0, len(data) - 6, 7):
        lba = int.from_bytes(data[offset : offset + 3], "little")
        size = s32(data, offset + 3)
        if lba == 0xFFFFFF and size == 0:
            return entries
        if lba == 0xFFFFFF:
            raise ValueError("malformed FAT sentinel")
        if size > 0:
            end = lba + (size + USER_SECTOR - 1) // USER_SECTOR
            if lba >= sector_count or end > sector_count:
                raise ValueError(f"FAT extent {len(entries)} lies outside the disc")
        entries.append(FatEntry(lba, size))
    raise ValueError("FAT sentinel not found")


def parse_directory_table(data: bytes) -> list[int]:
    if len(data) < DIRECTORY_COUNT * 2:
        raise ValueError("directory table is truncated")
    return list(struct.unpack_from(f"<{DIRECTORY_COUNT}H", data))


def map_physical_routes(directory: list[int], entry_count: int) -> dict[int, list[dict]]:
    starts: dict[int, list[int]] = {}
    for directory_id, encoded in enumerate(directory):
        if encoded and 0 <= encoded - 1 < entry_count:
            starts.setdefault(encoded - 1, []).append(directory_id)
    ordered = sorted(starts)
    routes = {}
    for position, start in enumerate(ordered):
        end = ordered[position + 1] if position + 1 < len(ordered) else entry_count
        for fat_index in range(start, end):
            routes[fat_index] = [
                {
                    "directory": f"0x{directory_id:02X}",
                    "file_id": f"0x{fat_index - start + 1:03X}",
                }
                for directory_id in starts[start]
            ]
    return routes


def lzss_decompress(
    data: bytes,
    offset: int = 0,
    max_size: int = MAX_SCRIPT_SECTION_SIZE,
) -> tuple[bytes, int]:
    if offset < 0 or offset + 4 > len(data):
        raise ValueError("missing LZSS header")
    target = u32(data, offset)
    if target == 0 or target > max_size:
        raise ValueError("LZSS expanded size exceeds the loader allocation")
    cursor = offset + 4
    output = bytearray()
    while len(output) < target:
        if cursor >= len(data):
            raise ValueError("truncated LZSS control byte")
        control = data[cursor]
        cursor += 1
        for bit in range(8):
            if control & (1 << bit):
                if cursor + 2 > len(data):
                    raise ValueError("truncated LZSS back-reference")
                low, high_length = data[cursor], data[cursor + 1]
                cursor += 2
                distance = low | ((high_length & 0x0F) << 8)
                length = (high_length >> 4) + 3
                if distance == 0 or distance > len(output):
                    raise ValueError("LZSS reference does not address produced output")
                if len(output) + length > target:
                    raise ValueError("LZSS token crosses its declared target")
                for _ in range(length):
                    output.append(output[-distance])
            else:
                if cursor >= len(data):
                    raise ValueError("truncated LZSS literal")
                if len(output) == target:
                    raise ValueError("LZSS group continues beyond its declared target")
                output.append(data[cursor])
                cursor += 1
            if len(output) == target and bit != 7:
                raise ValueError("LZSS target is reached before the control group ends")
    return bytes(output), cursor - offset


def parse_scripts_file(data: bytes) -> dict:
    if len(data) < SCRIPT_ROWS_OFFSET:
        raise ValueError("script section does not contain its fixed header")
    row_count = u32(data, SCRIPT_ROW_COUNT_OFFSET)
    bytecode_offset = SCRIPT_ROWS_OFFSET + row_count * SCRIPT_ROW_SIZE
    if bytecode_offset > len(data):
        raise ValueError("script routine rows leave the expanded section")
    bytecode_size = len(data) - bytecode_offset
    if bytecode_size == 0:
        raise ValueError("script section has no shared bytecode")

    rows = []
    all_offsets = []
    for entity_id in range(row_count):
        row_offset = SCRIPT_ROWS_OFFSET + entity_id * SCRIPT_ROW_SIZE
        offsets = list(struct.unpack_from("<32H", data, row_offset))
        if any(offset >= bytecode_size for offset in offsets):
            raise ValueError(f"entity {entity_id} has a routine entry outside bytecode")
        all_offsets.extend(offsets)
        rows.append(
            {
                "entity_id": entity_id,
                "routine_offsets": [f"0x{offset:04X}" for offset in offsets],
            }
        )

    unsigned_offsets = []
    for variable_index in range(1024):
        variable_offset = variable_index * 2
        word = u32(data, (variable_offset >> 6) * 4)
        if word & (1 << ((variable_offset >> 1) & 31)):
            unsigned_offsets.append(f"0x{variable_offset:04X}")

    return {
        "format": "xenogears-field-scripts-file/v1",
        "size": len(data),
        "variable_type_bitmap_offset": 0,
        "variable_type_bitmap_size": SCRIPT_BITMAP_SIZE,
        "unsigned_variable_offsets": unsigned_offsets,
        "signed_variable_count": 1024 - len(unsigned_offsets),
        "unsigned_variable_count": len(unsigned_offsets),
        "routine_row_count": row_count,
        "routines_per_entity": ROUTINES_PER_ENTITY,
        "routine_rows_offset": SCRIPT_ROWS_OFFSET,
        "routine_rows_size": row_count * SCRIPT_ROW_SIZE,
        "bytecode_offset": bytecode_offset,
        "bytecode_size": bytecode_size,
        "arrival_table_marker_present": data[bytecode_offset] == 0xFF,
        "unique_entry_offsets": [f"0x{offset:04X}" for offset in sorted(set(all_offsets))],
        "routine_rows": rows,
        "routine_boundary_policy": "entry_points_only_no_stored_lengths",
    }


def extract_container_scripts(container: bytes, logical_size: int | None = None) -> tuple[bytes, dict, dict]:
    logical_size = len(container) if logical_size is None else logical_size
    if logical_size > len(container):
        raise ValueError("logical container size exceeds available bytes")
    if logical_size < ENTITY_TABLE_OFFSET:
        raise ValueError("field container is shorter than its fixed header")
    entity_count = u16(container, ENTITY_COUNT_OFFSET)
    header_size = ENTITY_TABLE_OFFSET + entity_count * 0x10
    if header_size > logical_size:
        raise ValueError("field entity table leaves the container")

    sizes = [u32(container, SECTION_SIZE_TABLE + index * 4) for index in range(SECTION_COUNT)]
    offsets = [u32(container, SECTION_OFFSET_TABLE + index * 4) for index in range(SECTION_COUNT)]
    if any(left >= right for left, right in zip(offsets, offsets[1:])):
        raise ValueError("field section offsets are not strictly increasing")
    if offsets[0] < header_size or any(offset + 4 > logical_size for offset in offsets):
        raise ValueError("field section offset leaves the container")

    declared_size = sizes[FIELD_SECTION]
    stream_offset = offsets[FIELD_SECTION]
    if declared_size == 0:
        raise ValueError("field script section has zero declared size")
    allocation_size = declared_size + 0x10
    if allocation_size > MAX_SCRIPT_SECTION_SIZE:
        raise ValueError("field script allocation exceeds the safety limit")
    scripts, consumed = lzss_decompress(container, stream_offset, allocation_size)
    scripts_metadata = parse_scripts_file(scripts)
    next_offset = offsets[FIELD_SECTION + 1]
    container_metadata = {
        "entity_count": entity_count,
        "header_size": header_size,
        "section_sizes": sizes,
        "section_offsets": offsets,
        "script_section": {
            "index": FIELD_SECTION,
            "declared_expanded_size": declared_size,
            "actual_expanded_size": len(scripts),
            "within_loader_allocation": len(scripts) <= allocation_size,
            "stream_offset": stream_offset,
            "compressed_consumed_size": consumed,
            "compressed_end_offset": stream_offset + consumed,
            "next_section_offset": next_offset,
            "reads_past_next_section_offset": stream_offset + consumed > next_offset,
        },
    }
    return scripts, scripts_metadata, container_metadata


def _field_route(routes: list[dict]) -> tuple[dict, int] | None:
    for route in routes:
        directory_id = int(route["directory"], 16)
        file_id = int(route["file_id"], 16)
        if (
            directory_id == FIELD_DIRECTORY
            and file_id >= FIELD_FILE_BASE
            and (file_id - FIELD_FILE_BASE) % 2 == 0
        ):
            return route, (file_id - FIELD_FILE_BASE) // 2
    return None


def scan_disc(
    path: Path,
    disc_index: int,
    selected_fields: set[int] | None = None,
) -> tuple[dict, list[tuple[bytes, dict, dict]]]:
    disc = open_disc(path)
    entries = parse_fat(
        disc.read_user_data(FAT_LBA, FAT_SECTORS * USER_SECTOR),
        disc.sector_count,
    )
    directory = parse_directory_table(
        disc.read_user_data(DIRECTORY_LBA, DIRECTORY_COUNT * 2)
    )
    routes_by_index = map_physical_routes(directory, len(entries))
    found = []
    rejected = []
    candidate_count = 0

    for fat_index, entry in enumerate(entries):
        routes = routes_by_index.get(fat_index, [])
        selected = _field_route(routes)
        if selected is None:
            continue
        route, field_id = selected
        if selected_fields is not None and field_id not in selected_fields:
            continue
        candidate_count += 1
        reason = None
        if entry.size <= 0:
            reason = "empty_or_placeholder_extent"
        elif entry.size > MAX_STORED_EXTENT_SIZE:
            reason = "stored_extent_exceeds_safety_limit"
        elif entry.size < ENTITY_TABLE_OFFSET:
            reason = "extent_shorter_than_field_header"
        if reason is not None:
            rejected.append(
                {
                    "field_id": field_id,
                    "fat_index": fat_index,
                    "file_id": route["file_id"],
                    "stored_size": entry.size,
                    "reason": reason,
                }
            )
            continue

        padded = disc.read_user_data(entry.lba, entry.size, padded=True)
        try:
            scripts, scripts_metadata, container_metadata = extract_container_scripts(
                padded, entry.size
            )
        except (ValueError, struct.error) as error:
            rejected.append(
                {
                    "field_id": field_id,
                    "fat_index": fat_index,
                    "file_id": route["file_id"],
                    "stored_size": entry.size,
                    "reason": "invalid_field_container",
                    "detail": str(error),
                }
            )
            continue

        occurrence = {
            "disc_index": disc_index,
            "disc_name": path.name,
            "field_id": field_id,
            "fat_index": fat_index,
            "lba": entry.lba,
            "stored_size": entry.size,
            "route": route,
            "routes": routes,
            "container": container_metadata,
        }
        found.append((scripts, scripts_metadata, occurrence))

    disc_record = {
        "disc_index": disc_index,
        "input_path": str(path.resolve()),
        "disc_name": path.name,
        "data_file": disc.data_path.name,
        "sha256": file_sha256(disc.data_path),
        "layout": {
            "sector_size": disc.sector_size,
            "user_data_offset": disc.user_offset,
            "sector_count": disc.sector_count,
            "fat_entry_count": len(entries),
        },
        "candidate_field_files": candidate_count,
        "extracted_field_files": len(found),
        "rejected_candidate_count": len(rejected),
        "rejected_by_reason": dict(sorted(Counter(item["reason"] for item in rejected).items())),
        "rejected_candidates": rejected,
    }
    return disc_record, found


def write_if_changed(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and path.read_bytes() == content:
        return
    path.write_bytes(content)


def _json_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=True) + "\n").encode("ascii")


def _hardlink_or_copy(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, destination)
    except OSError:
        shutil.copy2(source, destination)


def _relative_symlink(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.symlink(os.path.relpath(source, destination.parent), destination)
    except OSError:
        _hardlink_or_copy(source, destination)


def build_catalog(output: Path, resources: list[dict], payloads: dict[str, bytes]) -> dict:
    from compile_field_scripts import disassemble
    from editable_field_scripts import render_source

    catalog_root = output / "catalog"
    if catalog_root.exists():
        shutil.rmtree(catalog_root)
    catalog_records = []
    for resource in resources:
        digest = resource["sha256"]
        payload = payloads[digest]
        metadata = resource["metadata"]
        assembly = disassemble(payload).text().encode("utf-8")
        for occurrence in resource["occurrences"]:
            relative = (
                Path("catalog")
                / f"disc-{occurrence['disc_index'] + 1:02d}"
                / f"field-{occurrence['field_id']:04d}_{digest[:10]}"
            )
            destination = output / relative
            _relative_symlink(output / resource["asset_path"], destination / "scripts.bin")
            _relative_symlink(output / resource["metadata_path"], destination / "metadata.json")
            bytecode_offset = metadata["bytecode_offset"]
            write_if_changed(destination / "bytecode.bin", payload[bytecode_offset:])
            write_if_changed(
                destination / "variable-types.bin", payload[:SCRIPT_BITMAP_SIZE]
            )
            readable_script, _ = render_source(
                occurrence["field_id"], payload, metadata
            )
            write_if_changed(
                destination / "script.xgs", readable_script.encode("utf-8")
            )
            write_if_changed(destination / "script.xga", assembly)
            write_if_changed(destination / "sources.json", _json_bytes(occurrence))
            catalog_records.append(
                {
                    "disc_index": occurrence["disc_index"],
                    "field_id": occurrence["field_id"],
                    "sha256": digest,
                    "path": relative.as_posix(),
                    "script_path": (relative / "script.xgs").as_posix(),
                    "assembly_path": (relative / "script.xga").as_posix(),
                    "routine_row_count": metadata["routine_row_count"],
                    "bytecode_size": metadata["bytecode_size"],
                    "instruction_count": metadata["readable_script"]["instruction_count"],
                    "instruction_coverage_percent": metadata["readable_script"]["instruction_coverage_percent"],
                    "total_coverage_percent": metadata["readable_script"]["total_coverage_percent"],
                }
            )
    catalog_records.sort(key=lambda item: (item["disc_index"], item["field_id"], item["sha256"]))
    catalog = {
        "schema": SCHEMA,
        "field_occurrence_count": len(catalog_records),
        "fields": catalog_records,
    }
    write_if_changed(catalog_root / "catalog.json", _json_bytes(catalog))
    return catalog


def extract_field_scripts(
    discs: Iterable[Path],
    output: Path,
    selected_fields: set[int] | None = None,
) -> dict:
    from editable_field_scripts import render_source

    disc_paths = list(discs)
    if not disc_paths:
        raise ValueError("at least one disc is required")
    if selected_fields is not None and any(field_id < 0 for field_id in selected_fields):
        raise ValueError("Field IDs must be non-negative")
    output.mkdir(parents=True, exist_ok=True)

    unique: dict[str, dict] = {}
    disc_records = []
    for disc_index, path in enumerate(disc_paths):
        disc_record, found = scan_disc(path, disc_index, selected_fields)
        disc_records.append(disc_record)
        for payload, metadata, occurrence in found:
            digest = hashlib.sha256(payload).hexdigest()
            if digest not in unique:
                unique[digest] = {
                    "payload": payload,
                    "metadata": metadata,
                    "occurrences": [],
                }
            unique[digest]["occurrences"].append(occurrence)

    found_field_ids = {
        occurrence["field_id"]
        for item in unique.values()
        for occurrence in item["occurrences"]
    }
    if selected_fields is not None:
        missing = selected_fields - found_field_ids
        if missing:
            raise ValueError(
                "requested Field IDs were not found: "
                + ", ".join(str(field_id) for field_id in sorted(missing))
            )

    resources = []
    payloads = {}
    referenced_assets = set()
    referenced_metadata = set()
    for digest, item in sorted(unique.items()):
        payload = item["payload"]
        metadata = dict(item["metadata"])
        first_field_id = item["occurrences"][0]["field_id"]
        _, readable_report = render_source(first_field_id, payload, metadata)
        metadata["readable_script"] = readable_report
        asset_relative = Path("assets") / f"{digest}.scripts.bin"
        metadata_relative = Path("metadata") / f"{digest}.json"
        write_if_changed(output / asset_relative, payload)
        write_if_changed(output / metadata_relative, _json_bytes(metadata))
        occurrences = sorted(
            item["occurrences"],
            key=lambda value: (value["disc_index"], value["field_id"], value["fat_index"]),
        )
        resources.append(
            {
                "sha256": digest,
                "size": len(payload),
                "asset_path": asset_relative.as_posix(),
                "metadata_path": metadata_relative.as_posix(),
                "routine_row_count": metadata["routine_row_count"],
                "unique_entry_count": len(metadata["unique_entry_offsets"]),
                "bytecode_size": metadata["bytecode_size"],
                "readable_script": readable_report,
                "arrival_table_marker_present": metadata["arrival_table_marker_present"],
                "metadata": metadata,
                "occurrences": occurrences,
            }
        )
        payloads[digest] = payload
        referenced_assets.add(asset_relative.name)
        referenced_metadata.add(metadata_relative.name)

    catalog = build_catalog(output, resources, payloads)
    occurrence_count = sum(len(resource["occurrences"]) for resource in resources)
    manifest_resources = [
        {key: value for key, value in resource.items() if key != "metadata"}
        for resource in resources
    ]
    manifest = {
        "schema": SCHEMA,
        "discs": disc_records,
        "summary": {
            "unique_scripts_files": len(resources),
            "field_occurrences": occurrence_count,
            "unique_bytecode_bytes": sum(resource["bytecode_size"] for resource in resources),
            "routine_rows": sum(resource["routine_row_count"] for resource in resources),
            "routine_entry_references": sum(
                resource["routine_row_count"] * ROUTINES_PER_ENTITY for resource in resources
            ),
            "instructions": sum(
                resource["readable_script"]["instruction_count"] for resource in resources
            ),
            "instruction_bytes": sum(
                resource["readable_script"]["instruction_bytes"] for resource in resources
            ),
            "classified_bytes": sum(
                resource["readable_script"]["classified_bytes"] for resource in resources
            ),
        },
        "catalog": {
            "path": "catalog/catalog.json",
            "field_occurrence_count": catalog["field_occurrence_count"],
        },
        "resources": manifest_resources,
    }
    write_if_changed(output / "manifest.json", _json_bytes(manifest))

    for directory_name, referenced in (
        ("assets", referenced_assets),
        ("metadata", referenced_metadata),
    ):
        directory = output / directory_name
        if not directory.is_dir():
            continue
        for path in directory.iterdir():
            if path.is_file() and path.name not in referenced:
                path.unlink()
    return manifest


def _field_id(value: str) -> int:
    try:
        parsed = int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid Field ID: {value}") from error
    if parsed < 0:
        raise argparse.ArgumentTypeError("Field ID must be non-negative")
    return parsed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "discs",
        nargs="+",
        type=Path,
        help="one or more retail CUE, BIN, or ISO images",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("extracted-field-scripts"),
        help="output directory (default: extracted-field-scripts)",
    )
    parser.add_argument(
        "--field",
        action="append",
        type=_field_id,
        dest="fields",
        help="extract only this Field ID; may be repeated",
    )
    args = parser.parse_args()
    manifest = extract_field_scripts(
        args.discs,
        args.output,
        set(args.fields) if args.fields is not None else None,
    )
    print(
        f"Extracted {manifest['summary']['unique_scripts_files']} unique ScriptsFiles "
        f"from {manifest['summary']['field_occurrences']} Field occurrences."
    )
    print(f"Manifest: {args.output / 'manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
