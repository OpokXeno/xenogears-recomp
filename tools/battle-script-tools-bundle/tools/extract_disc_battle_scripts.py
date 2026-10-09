#!/usr/bin/env python3
"""Extract Xenogears Battle Event VM scripts from retail discs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

SCHEMA = "xenogears-disc-battle-event-scripts/v1"
FAT_LBA = 0x18
FAT_SECTORS = 0x10
DIRECTORY_LBA = 0x28
DIRECTORY_COUNT = 64
EVENT_DIRECTORY = 0x20
EVENT_DATA_FILE = 0x02
USER_SECTOR = 2048
MAX_STORED_EXTENT_SIZE = 16 * 1024 * 1024
MAX_RESOURCE_SIZE = 1024 * 1024
SCRIPT_PREFIX_SIZE = 0x40
SCRIPT_COUNT_OFFSET = 0x40
SCRIPT_ROWS_OFFSET = 0x44
SCRIPT_ROW_SIZE = 0x10
ENTRIES_PER_ENTITY = 8
RUNTIME_ENTITY_CAPACITY = 16
MAX_BYTECODE_SIZE = 0x10000


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
    file_match = re.search(r'^\s*FILE\s+"([^"]+)"\s+BINARY\s*$', text, re.MULTILINE | re.IGNORECASE)
    track_match = re.search(r"^\s*TRACK\s+01\s+(MODE[12]/(?:2048|2352))\s*$", text, re.MULTILINE | re.IGNORECASE)
    index_match = re.search(r"^\s*INDEX\s+01\s+00:00:00\s*$", text, re.MULTILINE | re.IGNORECASE)
    if not file_match or not track_match or not index_match:
        raise ValueError(f"{path}: CUE must describe track 01 at 00:00:00")
    binary = (path.parent / file_match.group(1)).resolve()
    if not binary.is_file():
        raise ValueError(f"{path}: CUE data file does not exist: {binary}")
    mode = track_match.group(1).upper()
    if mode.endswith("/2048"):
        layout = DiscLayout(path.resolve(), binary, 2048, 0)
    else:
        layout = DiscLayout(path.resolve(), binary, 2352, 24 if mode.startswith("MODE2") else 16)
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
        candidates.extend((DiscLayout(path, path, 2352, 24), DiscLayout(path, path, 2352, 16)))
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


def read_indexed_file(disc_path: Path, directory_id: int, file_id: int) -> tuple[bytes, int, int, DiscLayout]:
    """Return (sector-padded payload, logical size, FAT index, layout)."""
    disc = open_disc(disc_path)
    directory = parse_directory_table(disc.read_user_data(DIRECTORY_LBA, DIRECTORY_COUNT * 2))
    entries = parse_fat(disc.read_user_data(FAT_LBA, FAT_SECTORS * USER_SECTOR), disc.sector_count)
    start = directory[directory_id] - 1
    index = start + file_id - 1
    next_starts = [encoded - 1 for encoded in directory if encoded - 1 > start]
    end = min(next_starts, default=len(entries))
    if start < 0 or not start <= index < end:
        raise ValueError(f"file 0x{file_id:02X} is outside directory 0x{directory_id:02X}")
    entry = entries[index]
    if entry.size <= 0 or entry.size > MAX_STORED_EXTENT_SIZE:
        raise ValueError(f"directory 0x{directory_id:02X} file 0x{file_id:02X} is not a stored file")
    return disc.read_user_data(entry.lba, entry.size, padded=True), entry.size, index, disc


def lzss_decompress(data: bytes, offset: int = 0, max_size: int = MAX_RESOURCE_SIZE) -> tuple[bytes, int]:
    """Expand one stream exactly as the resident decoder (0x80032E88) does.

    Control bits are processed low-to-high; one selects a two-byte
    back-reference (12-bit distance, 4-bit length - 3). The decoder always
    processes complete eight-token groups and compares its output cursor with
    the declared end only between groups, so a valid stream reaches the
    declared size exactly at a group boundary. Returns the payload and the
    number of stream bytes consumed, header included.
    """
    if offset < 0 or offset + 4 > len(data):
        raise ValueError("missing LZSS header")
    target = u32(data, offset)
    if target == 0 or target > max_size:
        raise ValueError("LZSS expanded size is outside the supported range")
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
                for _ in range(length):
                    output.append(output[-distance])
            else:
                if cursor >= len(data):
                    raise ValueError("truncated LZSS literal")
                output.append(data[cursor])
                cursor += 1
            if len(output) > target:
                raise ValueError("LZSS group overruns the declared size; the resident decoder would not stop")
    if len(output) != target:
        raise ValueError("LZSS stream does not end on a group boundary")
    return bytes(output), cursor - offset


def parse_event_data_file(data: bytes, logical_size: int | None = None) -> dict:
    """Parse directory 0x20 file 2: a relocatable table of LZSS streams.

    +0x00 u32 count; +0x04 u32 offset[count]; then u32 end offset (file size).
    Stream 2*k is the script resource and 2*k+1 the dialogue bundle of Battle
    Event data index k.
    """
    logical_size = len(data) if logical_size is None else logical_size
    if logical_size < 8:
        raise ValueError("event data file is shorter than its table")
    count = u32(data, 0)
    table_end = 4 + (count + 1) * 4
    if count == 0 or count % 2 or table_end > logical_size:
        raise ValueError("event data table has an invalid stream count")
    offsets = [u32(data, 4 + index * 4) for index in range(count + 1)]
    if offsets[0] != table_end or offsets != sorted(offsets) or offsets[-1] != logical_size:
        raise ValueError("event data stream offsets are not ordered within the file")
    streams = []
    for index in range(count):
        payload, consumed = lzss_decompress(data, offsets[index])
        stored = offsets[index + 1] - offsets[index]
        streams.append({
            "index": index,
            "offset": offsets[index],
            "stored_size": stored,
            "consumed_size": consumed,
            "overrun": max(0, consumed - stored),
            "expanded_size": len(payload),
            "payload": payload,
        })
    return {"count": count, "offsets": offsets, "streams": streams}


def parse_script_resource(data: bytes) -> dict:
    if len(data) < SCRIPT_ROWS_OFFSET:
        raise ValueError("script resource does not contain its fixed header")
    entity_count = u32(data, SCRIPT_COUNT_OFFSET)
    bytecode_offset = SCRIPT_ROWS_OFFSET + entity_count * SCRIPT_ROW_SIZE
    if entity_count == 0 or bytecode_offset >= len(data):
        raise ValueError("script entity rows leave the resource or no bytecode remains")
    bytecode_size = len(data) - bytecode_offset
    if bytecode_size > MAX_BYTECODE_SIZE:
        raise ValueError("bytecode exceeds the VM's 16-bit address space")
    rows = []
    for entity_id in range(entity_count):
        offsets = list(struct.unpack_from("<8H", data, SCRIPT_ROWS_OFFSET + entity_id * SCRIPT_ROW_SIZE))
        rows.append({"entity_id": entity_id, "entry_offsets": [f"0x{offset:04X}" for offset in offsets]})
    entries = sorted({int(offset, 16) for row in rows for offset in row["entry_offsets"]})
    return {
        "format": "xenogears-battle-event-script/v1",
        "size": len(data),
        "prefix_size": SCRIPT_PREFIX_SIZE,
        "prefix_is_zero": data[:SCRIPT_PREFIX_SIZE] == bytes(SCRIPT_PREFIX_SIZE),
        "entity_count": entity_count,
        "entries_per_entity": ENTRIES_PER_ENTITY,
        "entity_rows_offset": SCRIPT_ROWS_OFFSET,
        "entity_rows_size": entity_count * SCRIPT_ROW_SIZE,
        "bytecode_offset": bytecode_offset,
        "bytecode_size": bytecode_size,
        "within_runtime_entity_capacity": entity_count <= RUNTIME_ENTITY_CAPACITY,
        "entity_rows": rows,
        "unique_entry_offsets": [f"0x{offset:04X}" for offset in entries],
        "entries_outside_bytecode": [f"0x{offset:04X}" for offset in entries if offset >= bytecode_size],
        "entry_boundary_policy": "entry_points_only_no_stored_lengths",
    }


# Narrow single-byte glyph table (docs/xenogears/graphics/03 section 9.1.1).
_TEXT = {0x10: " ", 0x11: "+", 0x12: ",", 0x13: "-", 0x14: ".", 0x15: "/", 0x57: "!",
         0x58: '"', 0x59: "#", 0x5A: "%", 0x5B: "&", 0x5C: "'", 0x5D: "(", 0x5E: ")",
         0x5F: ":", 0x60: "?"}
_TEXT.update({0x16 + index: str(index) for index in range(10)})
_TEXT.update({0x20 + index: chr(ord("A") + index) for index in range(26)})
_TEXT.update({0x3D + index: chr(ord("a") + index) for index in range(26)})


def decode_text(data: bytes, offset: int, limit: int) -> str:
    """Render one encoded message for comments; unknown codes stay as {XX}."""
    parts = []
    cursor = offset
    while cursor < limit:
        code = data[cursor]
        cursor += 1
        if code == 0x00:
            break
        if code == 0x01:
            parts.append(" / ")
        elif code == 0x0F and cursor + 2 <= limit:
            parts.append(f"{{0F {data[cursor]:02X} {data[cursor + 1]:02X}}}")
            cursor += 2
        elif code in _TEXT:
            parts.append(_TEXT[code])
        else:
            parts.append(f"{{{code:02X}}}")
    return "".join(parts)


def parse_dialog_resource(data: bytes) -> dict:
    """Parse the common message bundle used by Battle Event dialogue."""
    if len(data) < 4:
        raise ValueError("dialogue bundle is shorter than its header")
    highest = u16(data, 0)
    count = 0 if highest == 0xFFFF else highest + 1
    if 4 + count * 2 > len(data):
        raise ValueError("dialogue offset table leaves the bundle")
    messages = []
    for message_id in range(count):
        offset = u16(data, 4 + message_id * 2)
        if offset >= len(data):
            messages.append({"id": message_id, "offset": offset, "text": None})
            continue
        messages.append({"id": message_id, "offset": offset, "text": decode_text(data, offset, len(data))})
    return {"format": "xenogears-battle-event-dialogue/v1", "size": len(data), "message_count": count,
            "messages": messages}


def scan_disc(path: Path, disc_index: int, selected: set[int] | None = None) -> tuple[dict, list[dict]]:
    padded, logical_size, fat_index, disc = read_indexed_file(path, EVENT_DIRECTORY, EVENT_DATA_FILE)
    container = parse_event_data_file(padded, logical_size)
    found = []
    rejected = []
    for event_index in range(container["count"] // 2):
        if selected is not None and event_index not in selected:
            continue
        script_stream = container["streams"][event_index * 2]
        dialog_stream = container["streams"][event_index * 2 + 1]
        try:
            metadata = parse_script_resource(script_stream["payload"])
            dialog = parse_dialog_resource(dialog_stream["payload"])
        except (ValueError, struct.error) as error:
            rejected.append({"event_index": event_index, "reason": "invalid_script_resource", "detail": str(error)})
            continue
        stream_info = {
            key: {name: value for name, value in stream.items() if name != "payload"}
            for key, stream in (("script_stream", script_stream), ("dialog_stream", dialog_stream))
        }
        found.append({
            "script": script_stream["payload"],
            "dialog": dialog_stream["payload"],
            "metadata": metadata,
            "dialog_metadata": dialog,
            "occurrence": {
                "disc_index": disc_index,
                "disc_name": path.name,
                "event_index": event_index,
                "directory": f"0x{EVENT_DIRECTORY:02X}",
                "file_id": f"0x{EVENT_DATA_FILE:02X}",
                "fat_index": fat_index,
                "container_size": logical_size,
                **stream_info,
            },
        })
    record = {
        "disc_index": disc_index,
        "input_path": str(path.resolve()),
        "disc_name": path.name,
        "data_file": disc.data_path.name,
        "sha256": file_sha256(disc.data_path),
        "layout": {"sector_size": disc.sector_size, "user_data_offset": disc.user_offset,
                   "sector_count": disc.sector_count},
        "event_data_file": {
            "directory": f"0x{EVENT_DIRECTORY:02X}",
            "file_id": f"0x{EVENT_DATA_FILE:02X}",
            "fat_index": fat_index,
            "size": logical_size,
            "sha256": hashlib.sha256(padded[:logical_size]).hexdigest(),
            "stream_count": container["count"],
            "event_count": container["count"] // 2,
            "streams_reading_past_their_extent": sum(1 for stream in container["streams"] if stream["overrun"]),
        },
        "extracted_events": len(found),
        "rejected_events": rejected,
    }
    return record, found


def write_if_changed(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and path.read_bytes() == content:
        return
    path.write_bytes(content)


def _json_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=True) + "\n").encode("ascii")


def _relative_symlink(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink() or destination.exists():
        destination.unlink()
    try:
        os.symlink(os.path.relpath(source, destination.parent), destination)
    except OSError:
        try:
            os.link(source, destination)
        except OSError:
            shutil.copy2(source, destination)


def dialog_listing(dialog_metadata: dict) -> str:
    lines = ["// Battle Event dialogue (informational; edit dialog.bin with a text tool)"]
    for message in dialog_metadata["messages"]:
        text = message["text"]
        lines.append(f"{message['id']:3d}: " + ("<outside bundle>" if text is None else json.dumps(text)))
    return "\n".join(lines) + "\n"


def build_catalog(output: Path, resources: list[dict], payloads: dict[str, tuple[bytes, bytes]]) -> dict:
    from compile_battle_events import disassemble
    from decompile_battle_events import render_source

    catalog_root = output / "catalog"
    if catalog_root.exists():
        shutil.rmtree(catalog_root)
    records = []
    for resource in resources:
        digest = resource["sha1"]
        script, dialog = payloads[digest]
        metadata = resource["metadata"]
        assembly = disassemble(script).text().encode("utf-8")
        for occurrence in resource["occurrences"]:
            relative = (Path("catalog") / f"disc-{occurrence['disc_index'] + 1:02d}"
                        / f"event-{occurrence['event_index']:02d}_{digest[:10]}")
            destination = output / relative
            _relative_symlink(output / resource["script_path"], destination / "script.bin")
            _relative_symlink(output / resource["dialog_path"], destination / "dialog.bin")
            _relative_symlink(output / resource["metadata_path"], destination / "metadata.json")
            write_if_changed(destination / "bytecode.bin", script[metadata["bytecode_offset"]:])
            text, _ = render_source(occurrence["event_index"], script, dialog)
            write_if_changed(destination / "script.xgs", text.encode("utf-8"))
            write_if_changed(destination / "script.xga", assembly)
            write_if_changed(destination / "dialog.txt", dialog_listing(resource["dialog_metadata"]).encode("utf-8"))
            write_if_changed(destination / "sources.json", _json_bytes(occurrence))
            report = metadata["readable_script"]
            records.append({
                "disc_index": occurrence["disc_index"],
                "event_index": occurrence["event_index"],
                "sha1": digest,
                "path": relative.as_posix(),
                "script_path": (relative / "script.xgs").as_posix(),
                "assembly_path": (relative / "script.xga").as_posix(),
                "entity_count": metadata["entity_count"],
                "bytecode_size": metadata["bytecode_size"],
                "message_count": resource["dialog_metadata"]["message_count"],
                "instruction_count": report["instruction_count"],
                "instruction_coverage_percent": report["instruction_coverage_percent"],
                "total_coverage_percent": report["total_coverage_percent"],
            })
    records.sort(key=lambda item: (item["disc_index"], item["event_index"], item["sha1"]))
    catalog = {"schema": SCHEMA, "event_occurrence_count": len(records), "events": records}
    write_if_changed(catalog_root / "catalog.json", _json_bytes(catalog))
    return catalog


def extract_battle_scripts(discs: Iterable[Path], output: Path, selected: set[int] | None = None) -> dict:
    from decompile_battle_events import render_source

    disc_paths = list(discs)
    if not disc_paths:
        raise ValueError("at least one disc is required")
    output.mkdir(parents=True, exist_ok=True)
    unique: dict[str, dict] = {}
    disc_records = []
    for disc_index, path in enumerate(disc_paths):
        record, found = scan_disc(path, disc_index, selected)
        disc_records.append(record)
        for item in found:
            digest = hashlib.sha1(item["script"] + b"\0dialog\0" + item["dialog"]).hexdigest()
            entry = unique.setdefault(digest, {**{key: item[key] for key in ("script", "dialog", "metadata", "dialog_metadata")}, "occurrences": []})
            entry["occurrences"].append(item["occurrence"])
    if selected is not None:
        missing = selected - {occ["event_index"] for item in unique.values() for occ in item["occurrences"]}
        if missing:
            raise ValueError("requested Battle Event indices were not found: " + ", ".join(map(str, sorted(missing))))

    resources = []
    payloads = {}
    referenced = {"assets": set(), "metadata": set()}
    for digest, item in sorted(unique.items()):
        occurrences = sorted(item["occurrences"], key=lambda value: (value["disc_index"], value["event_index"]))
        first = occurrences[0]
        metadata = dict(item["metadata"])
        _, report = render_source(first["event_index"], item["script"], item["dialog"])
        metadata["readable_script"] = report
        stem = f"{first['fat_index']}_event-{first['event_index']:02d}_{digest}"
        script_relative = Path("assets") / f"{stem}.script.bin"
        dialog_relative = Path("assets") / f"{stem}.dialog.bin"
        metadata_relative = Path("metadata") / f"{stem}.json"
        write_if_changed(output / script_relative, item["script"])
        write_if_changed(output / dialog_relative, item["dialog"])
        write_if_changed(output / metadata_relative, _json_bytes({**metadata, "dialog": item["dialog_metadata"]}))
        resources.append({
            "sha1": digest,
            "script_sha1": hashlib.sha1(item["script"]).hexdigest(),
            "dialog_sha1": hashlib.sha1(item["dialog"]).hexdigest(),
            "event_index": first["event_index"],
            "script_size": len(item["script"]),
            "dialog_size": len(item["dialog"]),
            "script_path": script_relative.as_posix(),
            "dialog_path": dialog_relative.as_posix(),
            "metadata_path": metadata_relative.as_posix(),
            "entity_count": metadata["entity_count"],
            "bytecode_size": metadata["bytecode_size"],
            "readable_script": report,
            "metadata": metadata,
            "dialog_metadata": item["dialog_metadata"],
            "occurrences": occurrences,
        })
        payloads[digest] = (item["script"], item["dialog"])
        referenced["assets"].update((script_relative.name, dialog_relative.name))
        referenced["metadata"].add(metadata_relative.name)

    catalog = build_catalog(output, resources, payloads)
    manifest = {
        "schema": SCHEMA,
        "discs": disc_records,
        "summary": {
            "unique_events": len(resources),
            "event_occurrences": sum(len(resource["occurrences"]) for resource in resources),
            "unique_bytecode_bytes": sum(resource["bytecode_size"] for resource in resources),
            "entities": sum(resource["entity_count"] for resource in resources),
            "instructions": sum(resource["readable_script"]["instruction_count"] for resource in resources),
            "instruction_bytes": sum(resource["readable_script"]["instruction_bytes"] for resource in resources),
            "classified_bytes": sum(resource["readable_script"]["classified_bytes"] for resource in resources),
        },
        "catalog": {"path": "catalog/catalog.json", "event_occurrence_count": catalog["event_occurrence_count"]},
        "resources": [{key: value for key, value in resource.items() if key not in {"metadata", "dialog_metadata"}}
                      for resource in resources],
    }
    write_if_changed(output / "manifest.json", _json_bytes(manifest))
    for directory_name, names in referenced.items():
        directory = output / directory_name
        if directory.is_dir():
            for path in directory.iterdir():
                if path.is_file() and path.name not in names:
                    path.unlink()
    return manifest


def _event_index(value: str) -> int:
    try:
        parsed = int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid Battle Event index: {value}") from error
    if parsed < 0:
        raise argparse.ArgumentTypeError("Battle Event index must be non-negative")
    return parsed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("discs", nargs="+", type=Path, help="one or more retail CUE, BIN, or ISO images")
    parser.add_argument("--output", type=Path, default=Path("extracted-battle-scripts"),
                        help="output directory (default: extracted-battle-scripts)")
    parser.add_argument("--event", action="append", type=_event_index, dest="events",
                        help="extract only this Battle Event data index; may be repeated")
    args = parser.parse_args()
    try:
        manifest = extract_battle_scripts(args.discs, args.output, set(args.events) if args.events else None)
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")
    print(f"Extracted {manifest['summary']['unique_events']} unique Battle Event scripts "
          f"from {manifest['summary']['event_occurrences']} occurrences.")
    print(f"Manifest: {args.output / 'manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
