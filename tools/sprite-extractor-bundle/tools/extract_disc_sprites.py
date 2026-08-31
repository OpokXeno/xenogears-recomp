#!/usr/bin/env python3
"""Extract and render Xenogears sprite pixels from retail discs."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import struct
import unicodedata
import zlib
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


SCHEMA = "xenogears-disc-sprites/v2"
FAT_LBA = 0x18
FAT_SECTORS = 0x10
DIRECTORY_LBA = 0x28
DIRECTORY_COUNT = 64
USER_SECTOR = 2048
MAX_STORED_EXTENT_SIZE = 64 * 1024 * 1024
MAX_BUNDLE_SIZE = 8 * 1024 * 1024
MAX_ENTRIES = 64
MAX_PACKET_MEMBERS = 4096
MAX_FRAMES = 0x1FF
MAX_TILES = 0x3F
MAX_PALETTE_BANKS = 64
MAX_PALETTE_VARIANTS = 16


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
        read_size = ((size + USER_SECTOR - 1) // USER_SECTOR) * USER_SECTOR if padded else size
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


@dataclass(frozen=True)
class Candidate:
    payload: bytes
    metadata: dict
    decoded_offset: int


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def s16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<h", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def s32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def s8(value: int) -> int:
    return value - 256 if value & 0x80 else value


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
                {"directory": f"0x{directory_id:02X}", "file_id": f"0x{fat_index - start + 1:02X}"}
                for directory_id in starts[start]
            ]
    return routes


def lzss_decompress(data: bytes, offset: int = 0, max_size: int = MAX_BUNDLE_SIZE) -> tuple[bytes, int]:
    if offset < 0 or offset + 4 > len(data):
        raise ValueError("missing LZSS header")
    target = u32(data, offset)
    if target == 0 or target > max_size:
        raise ValueError("implausible LZSS expanded size")
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
                low, high = data[cursor], data[cursor + 1]
                cursor += 2
                distance = low | ((high & 0x0F) << 8)
                length = (high >> 4) + 3
                if distance == 0 or distance > len(output):
                    raise ValueError("invalid LZSS history distance")
                for _ in range(length):
                    output.append(output[-distance])
                    if len(output) == target:
                        break
            else:
                if cursor >= len(data):
                    raise ValueError("truncated LZSS literal")
                output.append(data[cursor])
                cursor += 1
            if len(output) == target:
                break
    return bytes(output), cursor - offset


def packet_members(data: bytes) -> list[tuple[int, int]] | None:
    if len(data) < 12:
        return None
    count = u32(data, 0)
    header = count * 4 + 8
    if count == 0 or count > MAX_PACKET_MEMBERS or header > len(data):
        return None
    offsets = [u32(data, 4 + index * 4) for index in range(count + 1)]
    if offsets[0] != header or offsets[-1] != len(data):
        return None
    if any(left > right or left < header or right > len(data) for left, right in zip(offsets, offsets[1:])):
        return None
    return list(zip(offsets, offsets[1:]))


def _parse_animations(entry: bytes) -> dict:
    if len(entry) < 4:
        raise ValueError("animation entry is too short")
    header = u16(entry, 0)
    count = header & 0x3F
    table_end = 2 + count * 2
    if header >> 6 or count == 0 or table_end > len(entry):
        raise ValueError("invalid animation header")
    offsets = [u16(entry, 2 + index * 2) for index in range(count)]
    if any(offset < table_end or offset + 6 > len(entry) for offset in offsets):
        raise ValueError("animation offset is outside entry 0")
    animations = []
    for index, offset in enumerate(offsets):
        bytecode = offset + 2 + u16(entry, offset + 2)
        directional = offset + 4 + u16(entry, offset + 4)
        if not offset + 6 <= bytecode < len(entry) or not offset + 6 <= directional < len(entry):
            raise ValueError("animation child offset is outside entry 0")
        animations.append({
            "index": index,
            "offset": f"0x{offset:X}",
            "flags": f"0x{u16(entry, offset):04X}",
            "bytecode_offset": f"0x{bytecode:X}",
            "directional_offset": f"0x{directional:X}",
        })
    return {"animation_count": count, "animation_offsets": [f"0x{x:X}" for x in offsets], "animations": animations}


def _parse_prefix_stream(
    entry: bytes,
    cursor: int,
    tile_flags: int,
    tile_index: int,
    subgroup: int,
    transformed_subgroups: set[int],
    subgroup_transforms: list[dict],
) -> tuple[dict, int, int]:
    prefixes = []
    vertical_flip = False
    width_adjustment = 0
    height_adjustment = 0
    subgroup_translation = None
    subgroup_rotation = None
    while True:
        if cursor >= len(entry):
            raise ValueError("tile command stream leaves frame entry")
        command = entry[cursor]
        if not command & 0x80:
            break
        cursor += 1
        prefix = {"raw": f"0x{command:02X}"}
        if command & 0x40:
            subgroup = command & 7
            prefix.update({"kind": "subgroup", "subgroup": subgroup})
            if command & 0x20:
                if cursor + 2 > len(entry):
                    raise ValueError("truncated subgroup translation")
                subgroup_translation = [s8(entry[cursor]), s8(entry[cursor + 1])]
                prefix["translation"] = subgroup_translation
                subgroup_transforms[subgroup]["translation"] = subgroup_translation
                transformed_subgroups.add(subgroup)
                cursor += 2
            if command & 0x10:
                if cursor >= len(entry):
                    raise ValueError("truncated subgroup rotation")
                subgroup_rotation = entry[cursor] << 4
                prefix["rotation"] = subgroup_rotation
                subgroup_transforms[subgroup]["rotation"] = subgroup_rotation
                transformed_subgroups.add(subgroup)
                cursor += 1
            elif command & 0x40:
                subgroup_rotation = 0
                subgroup_transforms[subgroup]["rotation"] = 0
                transformed_subgroups.add(subgroup)
        else:
            prefix["kind"] = "tile"
            vertical_flip |= bool(command & 4)
            prefix["vertical_flip"] = bool(command & 4)
            if command & 1:
                if cursor >= len(entry):
                    raise ValueError("truncated width adjustment")
                width_adjustment = s8(entry[cursor])
                prefix["width_adjustment"] = width_adjustment
                cursor += 1
            if command & 2:
                if cursor >= len(entry):
                    raise ValueError("truncated height adjustment")
                height_adjustment = s8(entry[cursor])
                prefix["height_adjustment"] = height_adjustment
                cursor += 1
        prefixes.append(prefix)
        if len(prefixes) > 64:
            raise ValueError("excessive tile prefix count")
    position_size = 4 if tile_flags & 0x80 else 2
    if cursor + 1 + position_size > len(entry):
        raise ValueError("truncated tile material or position")
    material = entry[cursor]
    if material & 0x80:
        raise ValueError("prefix loop did not terminate at material")
    if tile_flags & 0x80:
        x, y = s16(entry, cursor + 1), s16(entry, cursor + 3)
    else:
        x, y = s8(entry[cursor + 1]), s8(entry[cursor + 2])
    cursor += 1 + position_size
    return {
        "index": tile_index,
        "prefixes": prefixes,
        "prefix_count": len(prefixes),
        "material": f"0x{material:02X}",
        "palette_bank": material & 0x0F,
        "blend": (material >> 4) & 3,
        "horizontal_flip": bool(material & 0x40),
        "vertical_flip": vertical_flip,
        "local_x": x,
        "local_y": y,
        "screen_width_adjustment": width_adjustment,
        "screen_height_adjustment": height_adjustment,
        "subgroup": subgroup,
        "subgroup_translation": subgroup_translation,
        "subgroup_rotation": subgroup_rotation,
        "effective_subgroup_translation": list(subgroup_transforms[subgroup]["translation"]),
        "effective_subgroup_rotation": subgroup_transforms[subgroup]["rotation"],
        "subgroup_state_dependent": subgroup != 4 or subgroup in transformed_subgroups,
    }, cursor, subgroup


def _parse_palette(entry: bytes) -> dict:
    if len(entry) < 4:
        raise ValueError("palette entry is shorter than its header")
    banks = u16(entry, 0)
    declared = u16(entry, 2) + 1
    if banks > MAX_PALETTE_BANKS:
        raise ValueError("palette bank count exceeds the VRAM-safe limit")
    stride = banks * 32
    if banks == 0:
        if len(entry) != 4:
            raise ValueError("zero-bank palette has trailing data")
        variants = 0
    else:
        if (len(entry) - 4) % stride:
            raise ValueError("palette matrices are not entry-aligned")
        variants = (len(entry) - 4) // stride
        if variants == 0:
            raise ValueError("nonzero palette bank count has no matrix")
        if variants > MAX_PALETTE_VARIANTS:
            raise ValueError("palette variant count exceeds the runtime selector")
    matrices = []
    cursor = 4
    for variant in range(variants):
        matrix = []
        for bank in range(banks):
            colors = [u16(entry, cursor + index * 2) for index in range(16)]
            cursor += 32
            matrix.append({
                "bank": bank,
                "rgb555": [f"0x{color:04X}" for color in colors],
                "stp": [bool(color & 0x8000) for color in colors],
            })
        matrices.append({"variant": variant, "banks": matrix})
    return {
        "bank_count": banks,
        "declared_variant_count": declared,
        "payload_variant_count": variants,
        "variant_count_status": (
            "not_applicable_no_banks" if banks == 0 else
            "matches" if declared == variants else
            "underdeclared" if declared < variants else
            "overdeclared"
        ),
        "declared_variant_count_matches_payload": None if banks == 0 else declared == variants,
        "variants": matrices,
    }


def _parse_frames(entry: bytes, palette_banks: int) -> tuple[dict, list[dict]]:
    if len(entry) == 0:
        return {"kind": "animation_only", "frame_count": 0, "tile_capacity": 0, "vram_prebacked": False}, []
    if len(entry) == 4 and u16(entry, 0) & 0x7FFF == 0 and u16(entry, 2) == 0x7777:
        return {
            "kind": "animation_sentinel",
            "frame_count": 0,
            "tile_capacity": 0,
            "vram_prebacked": bool(u16(entry, 0) & 0x8000),
            "sentinel": "0x7777",
        }, []
    if len(entry) < 4:
        raise ValueError("frame entry is neither empty nor a valid sentinel")
    header = u16(entry, 0)
    frame_count = header & 0x1FF
    tile_capacity = (header >> 9) & 0x3F
    static = bool(header & 0x8000)
    table_end = 2 + frame_count * 2
    if not 1 <= frame_count <= MAX_FRAMES or not 1 <= tile_capacity <= MAX_TILES or table_end > len(entry):
        raise ValueError("invalid frame table header")
    offsets = [u16(entry, frame_id * 2) for frame_id in range(1, frame_count + 1)]
    if any(offset < table_end or offset + 4 > len(entry) for offset in offsets):
        raise ValueError("frame offset is outside entry 1")
    frames = []
    observed_capacity = 0
    for frame_id, offset in enumerate(offsets, 1):
        flags = entry[offset]
        if flags & 0x40:
            raise ValueError("reserved frame flag bit 6 is set")
        tile_count = flags & 0x3F
        if tile_count > tile_capacity:
            raise ValueError("frame tile count exceeds capacity")
        observed_capacity = max(observed_capacity, tile_count)
        fixed = 4 if static else 6
        stride = 2 if static else 4
        references_at = offset + fixed
        cursor = references_at + tile_count * stride
        if cursor > len(entry):
            raise ValueError("frame reference array leaves entry 1")
        tiles = []
        subgroup = 4
        transformed_subgroups: set[int] = set()
        subgroup_transforms = [{"translation": [0, 0], "rotation": 0} for _ in range(8)]
        for tile_index in range(tile_count):
            reference = u16(entry, references_at + tile_index * stride)
            tile, cursor, subgroup = _parse_prefix_stream(
                entry,
                cursor,
                flags,
                tile_index,
                subgroup,
                transformed_subgroups,
                subgroup_transforms,
            )
            tile["reference"] = f"0x{reference:X}"
            if static:
                record = reference
                if record + 5 > len(entry):
                    raise ValueError("static atlas record leaves entry 1")
                format_page = entry[record]
                extended = bool(format_page & 0x10)
                record_size = 6 if extended else 5
                if record + record_size > len(entry):
                    raise ValueError("extended static atlas record leaves entry 1")
                raw_format = u16(entry, record) if extended else format_page
                tile["atlas"] = {
                    "record_offset": f"0x{record:X}",
                    "record_size": record_size,
                    "format_page": f"0x{raw_format:04X}" if extended else f"0x{raw_format:02X}",
                    "bpp": 8 if raw_format & 1 else 4,
                    "page": (raw_format >> 1) & 7,
                    "embedded_clut": extended,
                    "u": entry[record + (2 if extended else 1)],
                    "v": entry[record + (3 if extended else 2)],
                    "width": entry[record + (4 if extended else 3)],
                    "height": entry[record + (5 if extended else 4)],
                }
            else:
                source = reference * 4
                packed = u16(entry, references_at + tile_index * stride + 2)
                if source + 4 > len(entry):
                    raise ValueError("dynamic source descriptor leaves entry 1")
                width = entry[source]
                height = entry[source + 1]
                source_flags = u16(entry, source + 2)
                bpp = 8 if source_flags & 1 else 4
                divisor = 2 if bpp == 8 else 4
                if width % divisor:
                    raise ValueError("dynamic source width is not word-aligned")
                width_words = width // divisor
                source_size = 4 + width_words * height * 2
                if bool(width) != bool(height) or source + source_size > len(entry):
                    raise ValueError("dynamic source pixels leave entry 1")
                tile["packed_vram_offset"] = f"0x{packed:04X}"
                tile["source"] = {
                    "record_offset": f"0x{source:X}",
                    "flags": f"0x{source_flags:04X}",
                    "bpp": bpp,
                    "width": width,
                    "width_words": width_words,
                    "height": height,
                    "data_offset": f"0x{source + 4:X}",
                    "data_size": source_size - 4,
                    "scratch_x_words": packed & 0x1F,
                    "scratch_y": (packed >> 5) & 0x3F,
                }
                if bpp == 4 and width and tile["palette_bank"] >= palette_banks:
                    raise ValueError("dynamic tile references a missing palette bank")
            tiles.append(tile)
        dependencies = []
        if static:
            dependencies.append("external_vram_texture")
            if palette_banks == 0 or any(tile["atlas"]["embedded_clut"] for tile in tiles):
                dependencies.append("external_clut")
        if any(tile.get("source", {}).get("bpp") == 8 for tile in tiles):
            dependencies.append("external_or_specialized_8bpp_clut")
        if any(tile["subgroup_state_dependent"] for tile in tiles):
            dependencies.append("persistent_subgroup_transform_state")
        if any(
            tile["screen_width_adjustment"] or tile["screen_height_adjustment"]
            for tile in tiles
        ):
            dependencies.append("screen_size_adjustment")
        if any(tile["blend"] for tile in tiles):
            dependencies.append("ps1_blend_semantics")
        frames.append({
            "id": frame_id,
            "offset": f"0x{offset:X}",
            "flags": f"0x{flags:02X}",
            "tile_count": tile_count,
            "position_bits": 16 if flags & 0x80 else 8,
            "extent_y": entry[offset + 1],
            "serialized_02": entry[offset + 2],
            "extent_x": entry[offset + 3],
            "upload_width": None if static else entry[offset + 4],
            "upload_height": None if static else entry[offset + 5],
            "command_end_offset": f"0x{cursor:X}",
            "tiles": tiles,
            "dependencies": sorted(set(dependencies)),
        })
    if observed_capacity != tile_capacity:
        raise ValueError("declared tile capacity is not observed")
    return {
        "kind": "actor",
        "frame_count": frame_count,
        "frame_offsets": [f"0x{x:X}" for x in offsets],
        "tile_capacity": tile_capacity,
        "vram_prebacked": static,
    }, frames


def parse_sprite_bundle(data: bytes, offset: int = 0) -> tuple[int, dict]:
    if offset < 0 or offset + 24 > len(data):
        raise ValueError("candidate is too short")
    count = u32(data, offset)
    header = 4 * count + 8
    if count < 3 or count > MAX_ENTRIES or header > len(data) - offset:
        raise ValueError("invalid sprite entry count")
    offsets = [u32(data, offset + 4 + index * 4) for index in range(count + 1)]
    if offsets[0] != header or offsets[-1] > min(MAX_BUNDLE_SIZE, len(data) - offset):
        raise ValueError("invalid sprite entry boundary")
    if any(left > right for left, right in zip(offsets, offsets[1:])):
        raise ValueError("sprite entry offsets are not monotonic")
    payload = data[offset : offset + offsets[-1]]
    parts = [payload[left:right] for left, right in zip(offsets, offsets[1:])]
    animation = _parse_animations(parts[0])
    non_actor = len(parts[1]) == 0 or (
        len(parts[1]) == 4
        and u16(parts[1], 0) & 0x7FFF == 0
        and u16(parts[1], 2) == 0x7777
    )
    if non_actor:
        palette = None
        frames_meta, frames = _parse_frames(parts[1], 0)
    else:
        if len(parts[2]) < 4:
            raise ValueError("actor bundle has no palette header")
        palette = _parse_palette(parts[2])
        frames_meta, frames = _parse_frames(parts[1], palette["bank_count"])
    if frames_meta["kind"] == "actor":
        roles = ["animations", "frames", "palettes"] + ["subsystem_specific"] * (count - 3)
    else:
        roles = ["animations", frames_meta["kind"]] + ["opaque"] * (count - 2)
    return offsets[-1], {
        "entry_count": count,
        "entry_offsets": [f"0x{x:X}" for x in offsets],
        "entry_roles": roles,
        **animation,
        **frames_meta,
        "palette": palette,
        "frames": frames,
    }


def scan_bundles(data: bytes, alignment: int = 4) -> list[Candidate]:
    found = []
    offset = 0
    while offset + 24 <= len(data):
        try:
            size, metadata = parse_sprite_bundle(data, offset)
        except (ValueError, struct.error):
            offset += alignment
            continue
        found.append(Candidate(data[offset : offset + size], metadata, offset))
        offset += max(alignment, size - size % alignment)
    return found


def _rgba(color: int, index: int) -> tuple[int, int, int, int]:
    return (
        (color & 0x1F) * 255 // 31,
        ((color >> 5) & 0x1F) * 255 // 31,
        ((color >> 10) & 0x1F) * 255 // 31,
        0 if index == 0 else 255,
    )


def encode_png_rgba(width: int, height: int, pixels: bytes) -> bytes:
    if width <= 0 or height <= 0 or len(pixels) != width * height * 4:
        raise ValueError("invalid RGBA raster")
    scanlines = b"".join(b"\x00" + pixels[y * width * 4 : (y + 1) * width * 4] for y in range(height))

    def chunk(kind: bytes, payload: bytes) -> bytes:
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(scanlines, 9)) + chunk(b"IEND", b"")


def _decode_4bpp(data: bytes, width: int, height: int, colors: list[int]) -> tuple[int, int, bytes]:
    required = width * height // 2
    if width % 4 or len(data) < required or len(colors) != 16:
        raise ValueError("invalid 4-bpp source")
    palette = [_rgba(color, index) for index, color in enumerate(colors)]
    output = bytearray()
    for value in data[:required]:
        output.extend(palette[value & 0x0F])
        output.extend(palette[value >> 4])
    return width, height, bytes(output)


def _flip_rgba(width: int, height: int, pixels: bytes, horizontal: bool, vertical: bool) -> bytes:
    if not horizontal and not vertical:
        return pixels
    output = bytearray(len(pixels))
    for y in range(height):
        for x in range(width):
            source_x = width - 1 - x if horizontal else x
            source_y = height - 1 - y if vertical else y
            output[(y * width + x) * 4 : (y * width + x + 1) * 4] = pixels[(source_y * width + source_x) * 4 : (source_y * width + source_x + 1) * 4]
    return bytes(output)


def _rasterize_transformed_tile(tile: dict) -> dict:
    source_width = tile["width"]
    source_height = tile["height"]
    screen_width = source_width + tile["width_adjustment"]
    screen_height = source_height + tile["height_adjustment"]
    if screen_width <= 0 or screen_height <= 0:
        raise ValueError("tile screen adjustment produces a non-positive extent")
    translation_x, translation_y = tile["translation"]
    angle = tile["rotation"] * math.tau / 0x1000
    cosine = math.cos(angle)
    sine = math.sin(angle)
    if abs(cosine) < 1e-12:
        cosine = 0.0
    if abs(sine) < 1e-12:
        sine = 0.0

    def transform(x: float, y: float) -> tuple[float, float]:
        return (
            x * cosine - y * sine + translation_x,
            x * sine + y * cosine + translation_y,
        )

    x = tile["x"]
    y = tile["y"]
    corners = [
        transform(x, y),
        transform(x + screen_width, y),
        transform(x, y + screen_height),
        transform(x + screen_width, y + screen_height),
    ]
    left = math.floor(min(point[0] for point in corners))
    top = math.floor(min(point[1] for point in corners))
    right = math.ceil(max(point[0] for point in corners))
    bottom = math.ceil(max(point[1] for point in corners))
    width = right - left
    height = bottom - top
    output = bytearray(width * height * 4)
    for output_y in range(height):
        transformed_y = top + output_y + 0.5 - translation_y
        for output_x in range(width):
            transformed_x = left + output_x + 0.5 - translation_x
            local_x = transformed_x * cosine + transformed_y * sine - x
            local_y = -transformed_x * sine + transformed_y * cosine - y
            if not 0 <= local_x < screen_width or not 0 <= local_y < screen_height:
                continue
            source_x = min(source_width - 1, int(local_x * source_width / screen_width))
            source_y = min(source_height - 1, int(local_y * source_height / screen_height))
            source_at = (source_y * source_width + source_x) * 4
            destination_at = (output_y * width + output_x) * 4
            output[destination_at : destination_at + 4] = tile["pixels"][source_at : source_at + 4]
    return {"x": left, "y": top, "width": width, "height": height, "pixels": bytes(output)}


def _compose_tiles(tiles: list[dict]) -> tuple[int, int, int, int, bytes]:
    rendered = [_rasterize_transformed_tile(tile) for tile in tiles]
    if not rendered:
        raise ValueError("frame has no visual tiles")
    min_x = min(tile["x"] for tile in rendered)
    min_y = min(tile["y"] for tile in rendered)
    max_x = max(tile["x"] + tile["width"] for tile in rendered)
    max_y = max(tile["y"] + tile["height"] for tile in rendered)
    width, height = max_x - min_x, max_y - min_y
    if width <= 0 or height <= 0 or width * height > 16 * 1024 * 1024:
        raise ValueError("composed preview dimensions are unsafe")
    raster = bytearray(width * height * 4)
    # Runtime inserts each tile at the head of the draw list, so lower indices
    # are submitted last and appear above later tiles at the same depth.
    for tile in reversed(rendered):
        for tile_y in range(tile["height"]):
            for tile_x in range(tile["width"]):
                source_at = (tile_y * tile["width"] + tile_x) * 4
                destination_at = (
                    ((tile["y"] - min_y + tile_y) * width)
                    + tile["x"] - min_x + tile_x
                ) * 4
                _alpha_over(raster, destination_at, tile["pixels"][source_at : source_at + 4])
    return width, height, min_x, min_y, bytes(raster)


def _alpha_over(destination: bytearray, at: int, source: bytes) -> None:
    alpha = source[3]
    if alpha == 0:
        return
    if alpha == 255:
        destination[at : at + 4] = source
        return
    inverse = 255 - alpha
    destination[at] = (source[0] * alpha + destination[at] * inverse) // 255
    destination[at + 1] = (source[1] * alpha + destination[at + 1] * inverse) // 255
    destination[at + 2] = (source[2] * alpha + destination[at + 2] * inverse) // 255
    destination[at + 3] = alpha + destination[at + 3] * inverse // 255


def render_dynamic_frame(
    bundle: bytes,
    metadata: dict,
    frame: dict,
    variant: int,
    external_colors_by_bank: dict[int, list[int]] | None = None,
) -> tuple[bytes, dict]:
    offsets = [int(value, 16) for value in metadata["entry_offsets"]]
    entry = bundle[offsets[1] : offsets[2]]
    palette_entry = metadata["palette"]["variants"][variant]
    colors_by_bank = [[int(value, 16) for value in bank["rgb555"]] for bank in palette_entry["banks"]]
    decoded = []
    for tile in frame["tiles"]:
        source = tile["source"]
        if source["width"] == 0 or source["height"] == 0:
            continue
        data_at = int(source["data_offset"], 16)
        if source["bpp"] == 4:
            width, height, pixels = _decode_4bpp(
                entry[data_at : data_at + source["data_size"]],
                source["width"],
                source["height"],
                colors_by_bank[tile["palette_bank"]],
            )
        else:
            if external_colors_by_bank is None or tile["palette_bank"] not in external_colors_by_bank:
                raise ValueError("8-bpp source needs an external or specialized CLUT")
            colors = external_colors_by_bank[tile["palette_bank"]]
            palette = [_rgba(color, index) for index, color in enumerate(colors)]
            indexed = entry[data_at : data_at + source["data_size"]]
            if len(indexed) != source["width"] * source["height"]:
                raise ValueError("invalid 8-bpp source size")
            width, height = source["width"], source["height"]
            pixels = b"".join(bytes(palette[value]) for value in indexed)
        pixels = _flip_rgba(width, height, pixels, tile["horizontal_flip"], tile["vertical_flip"])
        decoded.append({
            "x": tile["local_x"],
            "y": tile["local_y"],
            "width": width,
            "height": height,
            "width_adjustment": tile["screen_width_adjustment"],
            "height_adjustment": tile["screen_height_adjustment"],
            "translation": tile["effective_subgroup_translation"],
            "rotation": tile["effective_subgroup_rotation"],
            "pixels": pixels,
        })
    width, height, min_x, min_y, raster = _compose_tiles(decoded)
    geometry_dependencies = {
        "persistent_subgroup_transform_state",
    }
    exact_geometry = not geometry_dependencies.intersection(frame["dependencies"])
    blend_faithful = "ps1_blend_semantics" not in frame["dependencies"]
    preview_dependencies = [
        dependency for dependency in frame["dependencies"]
        if dependency in geometry_dependencies | {"ps1_blend_semantics"}
    ]
    if exact_geometry and blend_faithful:
        completeness = "complete_preview"
    elif not exact_geometry and not blend_faithful:
        completeness = "source_pixels_complete_geometry_and_blend_approximated"
    elif not exact_geometry:
        completeness = "source_pixels_complete_geometry_state_dependent"
    else:
        completeness = "source_pixels_complete_ps1_blend_approximated"
    return encode_png_rgba(width, height, raster), {
        "preview_type": "rendered_frame",
        "width": width,
        "height": height,
        "origin_x": min_x,
        "origin_y": min_y,
        "decoded_tiles": len(decoded),
        "expected_tiles": frame["tile_count"],
        "pixels_complete": True,
        "geometry_exact_without_prior_state": exact_geometry,
        "ps1_blend_faithful": blend_faithful,
        "preview_dependencies": preview_dependencies,
        "completeness": completeness,
    }


def parse_offset_bundle(data: bytes, offset: int = 0) -> list[tuple[int, int]]:
    if offset < 0 or offset + 4 > len(data):
        raise ValueError("offset bundle header is missing")
    count = s32(data, offset)
    if count <= 0:
        return []
    if count > MAX_PACKET_MEMBERS or offset + 4 + count * 4 > len(data):
        raise ValueError("offset bundle count is invalid")
    starts = [u32(data, offset + 4 + index * 4) for index in range(count)]
    table_end = 4 + count * 4
    if starts[0] < table_end or any(left > right for left, right in zip(starts, starts[1:])):
        raise ValueError("offset bundle member offsets are invalid")
    if starts[-1] > len(data) - offset:
        raise ValueError("offset bundle member leaves its container")
    ends = starts[1:] + [len(data) - offset]
    return [(offset + start, offset + end) for start, end in zip(starts, ends)]


def parse_raw_vram_images(data: bytes, offset: int = 0) -> tuple[list[dict], int]:
    spans = parse_offset_bundle(data, offset)
    images = []
    container_end = offset + 4 + max(0, s32(data, offset)) * 4
    for start, declared_end in spans:
        if start + 4 > len(data):
            raise ValueError("raw VRAM image header is truncated")
        width_words = u16(data, start)
        height = u16(data, start + 2)
        size = 4 + width_words * height * 2
        if width_words == 0 or height == 0 or start + size > declared_end:
            raise ValueError("raw VRAM image dimensions leave the member")
        images.append({
            "width_words": width_words,
            "height": height,
            "data": data[start + 4 : start + size],
        })
        container_end = max(container_end, start + size)
    return images, container_end


def parse_battle_enemy_visual_pairs(data: bytes) -> list[dict]:
    if len(data) < 8:
        raise ValueError("Battle enemy visual header is truncated")
    record_count = data[0]
    if not 1 <= record_count <= 8 or 8 + record_count * 12 > len(data):
        raise ValueError("Battle enemy visual record count is invalid")
    pairs = []
    for record_index in range(record_count):
        record_at = 8 + record_index * 12
        actor_offset = u32(data, record_at)
        texture_ref = u32(data, record_at + 4)
        visual_type = data[record_at + 8]
        if visual_type != 0 or texture_ref < 8:
            continue
        bundle_size, metadata = parse_sprite_bundle(data, actor_offset)
        images, pack_end = parse_raw_vram_images(data, texture_ref)
        pairs.append({
            "record_index": record_index,
            "initial_vram_page": data[1],
            "actor_offset": actor_offset,
            "texture_offset": texture_ref,
            "texture_end": pack_end,
            "bundle": data[actor_offset : actor_offset + bundle_size],
            "metadata": metadata,
            "images": images,
            "serialized_record_tail": data[record_at + 9 : record_at + 12].hex(),
        })
    return pairs


def parse_battle_action_sprite(data: bytes) -> dict | None:
    try:
        outer = parse_offset_bundle(data)
        if len(outer) <= 1:
            return None
        auxiliary_start, auxiliary_end = outer[1]
        auxiliary = data[auxiliary_start:auxiliary_end]
        members = parse_offset_bundle(auxiliary)
        if len(members) <= 4:
            return None
        actor_start, actor_end = members[0]
        texture_start, texture_end = members[4]
        bundle_size, metadata = parse_sprite_bundle(auxiliary, actor_start)
        if actor_start + bundle_size > actor_end:
            raise ValueError("Battle action actor leaves its member")
        if metadata["kind"] != "actor" or not metadata["vram_prebacked"]:
            return None
        if texture_start == texture_end:
            raise ValueError("Battle action static actor has no packed pixels")
        texture = auxiliary[texture_start:texture_end]
        descriptors = parse_packed_image_set(texture, coordinate_mode=1)
        if not descriptors:
            raise ValueError("Battle action static actor has no image descriptors")
        return {
            "bundle": auxiliary[actor_start : actor_start + bundle_size],
            "metadata": metadata,
            "descriptors": descriptors,
            "texture": texture,
            "actor_offset": auxiliary_start + actor_start,
            "texture_offset": auxiliary_start + texture_start,
        }
    except (ValueError, struct.error):
        return None


def parse_sector_graphics(data: bytes) -> list[dict]:
    descriptors = []
    offset = 0
    while offset + USER_SECTOR <= len(data):
        values = struct.unpack_from("<14H", data, offset)
        tag = values[0]
        if tag not in {0x1200, 0x1201}:
            break
        stored_x, stored_y, relative_x, relative_y = values[2:6]
        width_words, rows_per_sector, unit_sectors, chunk_count = values[6:10]
        descriptor_count, reserved, chunk_copy = values[10:13]
        if (
            not width_words or not rows_per_sector or unit_sectors != 1 or not chunk_count
            or descriptor_count == 0 or reserved != 0 or chunk_copy != chunk_count
            or offset + (1 + chunk_count) * USER_SECTOR > len(data)
        ):
            raise ValueError("invalid sector graphics descriptor")
        heights = list(struct.unpack_from(f"<{chunk_count}H", data, offset + 28))
        rows = sum(heights)
        pixels = bytearray()
        for chunk_index, chunk_height in enumerate(heights):
            byte_count = width_words * chunk_height * 2
            if chunk_height == 0 or byte_count > USER_SECTOR:
                raise ValueError("invalid sector graphics chunk dimensions")
            chunk_at = offset + (chunk_index + 1) * USER_SECTOR
            pixels.extend(data[chunk_at : chunk_at + byte_count])
        descriptors.append({
            "tag": tag,
            "x": stored_x + relative_x,
            "y": stored_y + relative_y,
            "width_words": width_words,
            "height": rows,
            "data": bytes(pixels),
        })
        offset += (1 + chunk_count) * USER_SECTOR
    if offset != len(data):
        raise ValueError("sector graphics file has trailing or unparsed bytes")
    return descriptors


def parse_packed_image_set(data: bytes, coordinate_mode: int = 0) -> list[dict]:
    if coordinate_mode not in {0, 1}:
        raise ValueError("unsupported packed image coordinate mode")
    descriptors = []
    for start, end in parse_offset_bundle(data):
        if start + 16 > end:
            raise ValueError("packed image record header is truncated")
        tag, stored_x, stored_y, relative_x, relative_y, width_words, height = struct.unpack_from(
            "<Ihhhhhh", data, start
        )
        if tag not in {0x1100, 0x1101} or width_words <= 0 or height <= 0:
            raise ValueError("packed image record is invalid")
        size = width_words * height * 2
        if start + 16 + size > end:
            raise ValueError("packed image record pixels leave the member")
        if coordinate_mode == 0:
            x, y = stored_x + relative_x, stored_y + relative_y
        elif tag == 0x1100:
            x, y = 896 + relative_x, 256 + relative_y
        else:
            x, y = relative_x, 464 + relative_y
        descriptors.append({
            "tag": 0x1200 if tag == 0x1100 else 0x1201,
            "x": x,
            "y": y,
            "width_words": width_words,
            "height": height,
            "data": data[start + 16 : start + 16 + size],
        })
    return descriptors


def load_battle_common_clut(path: Path) -> list[int]:
    disc = open_disc(path)
    entries = parse_fat(disc.read_user_data(FAT_LBA, FAT_SECTORS * USER_SECTOR), disc.sector_count)
    directory = parse_directory_table(disc.read_user_data(DIRECTORY_LBA, DIRECTORY_COUNT * 2))
    routes = map_physical_routes(directory, len(entries))
    fat_index = next((
        index for index, values in routes.items()
        if any(route["directory"] == "0x0C" and int(route["file_id"], 16) == 3 for route in values)
    ), None)
    if fat_index is None:
        raise ValueError("Battle common archive route is missing")
    entry = entries[fat_index]
    padded = disc.read_user_data(entry.lba, entry.size, padded=True)
    members = packet_members(padded[: entry.size])
    if members is None or len(members) < 2:
        raise ValueError("Battle common archive is not a packet")
    expanded, _ = lzss_decompress(padded, members[1][0])
    descriptors = parse_packed_image_set(expanded)
    vram, coverage = create_vram()
    upload_sector_graphics(vram, coverage, descriptors)
    colors = []
    for x in range(1024):
        colors.append(u16(vram, (500 * 1024 + x) * 2) if coverage[500 * 1024 + x] else 0)
    return colors


def create_vram() -> tuple[bytearray, bytearray]:
    return bytearray(1024 * 512 * 2), bytearray(1024 * 512)


def upload_vram(
    vram: bytearray,
    coverage: bytearray,
    x: int,
    y: int,
    width_words: int,
    height: int,
    pixels: bytes,
) -> None:
    if x < 0 or y < 0 or x + width_words > 1024 or y + height > 512:
        raise ValueError("VRAM upload rectangle is outside VRAM")
    if len(pixels) != width_words * height * 2:
        raise ValueError("VRAM upload payload size does not match its rectangle")
    for row in range(height):
        source = row * width_words * 2
        destination_word = (y + row) * 1024 + x
        vram[destination_word * 2 : (destination_word + width_words) * 2] = pixels[source : source + width_words * 2]
        coverage[destination_word : destination_word + width_words] = b"\x01" * width_words


def upload_sector_graphics(vram: bytearray, coverage: bytearray, descriptors: list[dict]) -> None:
    for descriptor in descriptors:
        upload_vram(
            vram,
            coverage,
            descriptor["x"],
            descriptor["y"],
            descriptor["width_words"],
            descriptor["height"],
            descriptor["data"],
        )


def _read_vram_word(vram: bytearray, coverage: bytearray, x: int, y: int) -> int:
    if x < 0 or y < 0 or x >= 1024 or y >= 512 or not coverage[y * 1024 + x]:
        raise ValueError(f"sprite samples unloaded VRAM at ({x},{y})")
    return u16(vram, (y * 1024 + x) * 2)


def _static_page_x(texture_x: int, page: int, texture_page_mode: str) -> int:
    if texture_page_mode == "linear":
        return texture_x + page * 64
    if texture_page_mode == "field_ring":
        if texture_x < 320 or texture_x > 960 or (texture_x - 320) % 64:
            raise ValueError("invalid Field texture-page base")
        return 320 + (((texture_x - 320) // 64 + page) % 11) * 64
    raise ValueError(f"unsupported static texture-page mode: {texture_page_mode}")


def render_static_frame_from_vram(
    vram: bytearray,
    coverage: bytearray,
    texture_x: int,
    texture_y: int,
    metadata: dict,
    frame: dict,
    variant: int,
    texture_page_mode: str = "linear",
) -> tuple[bytes, dict]:
    local_palettes = metadata["palette"]["variants"] if metadata["palette"] else []
    decoded = []
    for tile in frame["tiles"]:
        atlas = tile["atlas"]
        if atlas["width"] == 0 or atlas["height"] == 0:
            continue
        raw_format = int(atlas["format_page"], 16)
        bpp = atlas["bpp"]
        pixels_per_word = 4 if bpp == 4 else 2
        if atlas["embedded_clut"]:
            page = atlas["page"]
            page_x = 0x300 + (page & 3) * 0x40
            page_y = 0x100 if page & 4 else 0
            clut_x = (raw_format >> 1) & 0xF0
            clut_y = ((raw_format >> 9) & 0x0F) + 0x1CC
            color_count = 16 if bpp == 4 else 256
            colors = [_read_vram_word(vram, coverage, clut_x + index, clut_y) for index in range(color_count)]
        else:
            page_x = _static_page_x(texture_x, atlas["page"], texture_page_mode)
            page_y = texture_y
            if bpp != 4 or not local_palettes:
                raise ValueError("static sprite has no resolvable local CLUT")
            colors = [int(value, 16) for value in local_palettes[variant]["banks"][tile["palette_bank"]]["rgb555"]]
        palette = [_rgba(color, index) for index, color in enumerate(colors)]
        pixels = bytearray()
        for source_y in range(atlas["height"]):
            for source_x in range(atlas["width"]):
                packed_x = (atlas["u"] + source_x) & 0xFF
                word = _read_vram_word(
                    vram,
                    coverage,
                    page_x + packed_x // pixels_per_word,
                    page_y + ((atlas["v"] + source_y) & 0xFF),
                )
                if bpp == 4:
                    color_index = word >> ((packed_x & 3) * 4) & 0x0F
                else:
                    color_index = word >> ((packed_x & 1) * 8) & 0xFF
                pixels.extend(palette[color_index])
        pixels = bytearray(_flip_rgba(
            atlas["width"],
            atlas["height"],
            bytes(pixels),
            tile["horizontal_flip"],
            tile["vertical_flip"],
        ))
        decoded.append({
            "x": tile["local_x"],
            "y": tile["local_y"],
            "width": atlas["width"],
            "height": atlas["height"],
            "width_adjustment": tile["screen_width_adjustment"],
            "height_adjustment": tile["screen_height_adjustment"],
            "translation": tile["effective_subgroup_translation"],
            "rotation": tile["effective_subgroup_rotation"],
            "pixels": pixels,
        })
    width, height, min_x, min_y, raster = _compose_tiles(decoded)
    geometry_dependencies = {"persistent_subgroup_transform_state"}
    exact_geometry = not geometry_dependencies.intersection(frame["dependencies"])
    blend_faithful = "ps1_blend_semantics" not in frame["dependencies"]
    return encode_png_rgba(width, height, raster), {
        "preview_type": "rendered_external_frame",
        "width": width,
        "height": height,
        "origin_x": min_x,
        "origin_y": min_y,
        "decoded_tiles": len(decoded),
        "expected_tiles": frame["tile_count"],
        "zero_sized_tiles": frame["tile_count"] - len(decoded),
        "pixels_complete": True,
        "geometry_exact_without_prior_state": exact_geometry,
        "ps1_blend_faithful": blend_faithful,
        "preview_dependencies": [
            dependency for dependency in frame["dependencies"]
            if dependency in geometry_dependencies | {"ps1_blend_semantics"}
        ],
        "completeness": (
            "complete_preview" if exact_geometry and blend_faithful else
            "source_pixels_complete_geometry_and_blend_approximated" if not exact_geometry and not blend_faithful else
            "source_pixels_complete_geometry_state_dependent" if not exact_geometry else
            "source_pixels_complete_ps1_blend_approximated"
        ),
        "texture_source": "reconstructed_vram",
    }


def static_texture_fingerprint(
    vram: bytearray,
    coverage: bytearray,
    texture_x: int,
    texture_y: int,
    metadata: dict,
    texture_page_mode: str = "linear",
) -> bytes:
    digest = hashlib.sha256()
    atlases = {
        (
            int(tile["atlas"]["format_page"], 16),
            tile["atlas"]["u"],
            tile["atlas"]["v"],
            tile["atlas"]["width"],
            tile["atlas"]["height"],
        )
        for frame in metadata["frames"]
        for tile in frame["tiles"]
    }
    for raw_format, u, v, width, height in sorted(atlases):
        if width == 0 or height == 0:
            continue
        bpp = 8 if raw_format & 1 else 4
        pixels_per_word = 2 if bpp == 8 else 4
        page = (raw_format >> 1) & 7
        if raw_format & 0x10:
            page_x = 0x300 + (page & 3) * 0x40
            page_y = 0x100 if page & 4 else 0
        else:
            page_x = _static_page_x(texture_x, page, texture_page_mode)
            page_y = texture_y
        digest.update(struct.pack("<HBBBB", raw_format, u, v, width & 0xFF, height & 0xFF))
        for source_y in range(height):
            for source_x in range(width):
                packed_x = (u + source_x) & 0xFF
                word = _read_vram_word(
                    vram,
                    coverage,
                    page_x + packed_x // pixels_per_word,
                    page_y + ((v + source_y) & 0xFF),
                )
                color_index = (
                    word >> ((packed_x & 3) * 4) & 0x0F
                    if bpp == 4 else
                    word >> ((packed_x & 1) * 8) & 0xFF
                )
                digest.update(bytes((color_index,)))
    return digest.digest()


def render_raw_sprite_sheet(images: list[dict], colors: list[int], bpp: int) -> tuple[bytes, dict]:
    pixels_per_word = 4 if bpp == 4 else 2
    if len(colors) != (16 if bpp == 4 else 256):
        raise ValueError("sprite-sheet palette size does not match texture depth")
    stride = 64 * pixels_per_word
    width = max(index * stride + image["width_words"] * pixels_per_word for index, image in enumerate(images))
    height = max(image["height"] for image in images)
    if width <= 0 or height <= 0 or width * height > 32 * 1024 * 1024:
        raise ValueError("sprite-sheet preview dimensions are unsafe")
    palette = [_rgba(color, index) for index, color in enumerate(colors)]
    raster = bytearray(width * height * 4)
    for image_index, image in enumerate(images):
        image_width = image["width_words"] * pixels_per_word
        for y in range(image["height"]):
            for x in range(image_width):
                word = u16(image["data"], (y * image["width_words"] + x // pixels_per_word) * 2)
                if bpp == 4:
                    color_index = word >> ((x & 3) * 4) & 0x0F
                else:
                    color_index = word >> ((x & 1) * 8) & 0xFF
                destination = (y * width + image_index * stride + x) * 4
                raster[destination : destination + 4] = bytes(palette[color_index])
    return encode_png_rgba(width, height, bytes(raster)), {
        "preview_type": "rendered_sheet",
        "width": width,
        "height": height,
        "origin_x": 0,
        "origin_y": 0,
        "decoded_tiles": len(images),
        "expected_tiles": len(images),
        "pixels_complete": True,
        "geometry_exact_without_prior_state": True,
        "ps1_blend_faithful": True,
        "preview_dependencies": [],
        "completeness": "complete_sprite_sheet",
        "texture_source": "raw_vram_image",
    }


def scan_raw_vram_image_packs(data: bytes, start_offset: int = 0) -> list[tuple[int, int, list[dict]]]:
    packs = []
    offset = (start_offset + 3) & ~3
    while offset + 12 <= len(data):
        try:
            count = s32(data, offset)
            if not 1 <= count <= 32:
                raise ValueError
            images, end = parse_raw_vram_images(data, offset)
        except (ValueError, struct.error):
            offset += 4
            continue
        packs.append((offset, end, images))
        offset = (end + 3) & ~3
    return packs


def _subsystem(routes: list[dict]) -> str:
    directories = {int(route["directory"], 16) for route in routes}
    if 0x04 in directories:
        return "field"
    if 0x24 in directories:
        return "world"
    if directories & {0x0C, 0x0D, 0x0E, 0x0F, 0x20, 0x29, 0x2A, 0x2C, 0x2D}:
        return "battle"
    return "other"


def scan_disc(
    path: Path,
    disc_index: int,
) -> tuple[dict, list[tuple[Candidate, dict]]]:
    disc = open_disc(path)
    entries = parse_fat(disc.read_user_data(FAT_LBA, FAT_SECTORS * USER_SECTOR), disc.sector_count)
    xa_children = -entries[0].size if entries and entries[0].size < 0 else 0
    if xa_children > len(entries) - 1:
        raise ValueError("XA streaming root exceeds the FAT")
    directory = parse_directory_table(disc.read_user_data(DIRECTORY_LBA, DIRECTORY_COUNT * 2))
    routes_by_index = map_physical_routes(directory, len(entries))
    found: list[tuple[Candidate, dict]] = []
    occurrence_keys = set()
    skipped_extents = []

    def add(container: bytes, entry: FatEntry, fat_index: int, routes: list[dict], chain: list[dict], container_size: int) -> None:
        for bundle_index, candidate in enumerate(scan_bundles(container)):
            digest = hashlib.sha256(candidate.payload).hexdigest()
            chain_key = tuple((step["type"], step.get("member_index"), step.get("stored_offset")) for step in chain)
            key = (fat_index, chain_key, candidate.decoded_offset, digest)
            if key in occurrence_keys:
                continue
            occurrence_keys.add(key)
            found.append((candidate, {
                "disc_index": disc_index,
                "disc_name": path.name,
                "fat_index": fat_index,
                "lba": entry.lba,
                "stored_size": entry.size,
                "container_size": container_size,
                "container_bundle_index": bundle_index,
                "decoded_container_offset": candidate.decoded_offset,
                "embedded": candidate.decoded_offset != 0 or bool(chain),
                "subsystem": _subsystem(routes),
                "transform_chain": chain,
                "routes": routes,
            }))

    for fat_index, entry in enumerate(entries):
        if fat_index <= xa_children:
            continue
        if entry.size <= 0:
            continue
        if entry.size > MAX_STORED_EXTENT_SIZE:
            skipped_extents.append({
                "fat_index": fat_index,
                "lba": entry.lba,
                "stored_size": entry.size,
                "reason": "stored_extent_exceeds_safety_limit",
                "routes": routes_by_index.get(fat_index, []),
            })
            continue
        routes = routes_by_index.get(fat_index, [])
        padded = disc.read_user_data(entry.lba, entry.size, padded=True)
        raw = padded[: entry.size]
        add(raw, entry, fat_index, routes, [], len(raw))
        members = packet_members(raw)
        if members:
            for member_index, (start, end) in enumerate(members):
                if end - start < 4:
                    continue
                try:
                    expanded, consumed = lzss_decompress(padded, start)
                except ValueError:
                    continue
                add(expanded, entry, fat_index, routes, [{
                    "type": "packet_member_lzss",
                    "member_index": member_index,
                    "stored_offset": start,
                    "logical_stored_size": end - start,
                    "consumed_size": consumed,
                }], len(expanded))
        try:
            expanded, consumed = lzss_decompress(padded)
        except ValueError:
            expanded = None
        if expanded is not None:
            add(expanded, entry, fat_index, routes, [{"type": "whole_file_lzss", "stored_offset": 0, "consumed_size": consumed}], len(expanded))
            expanded_members = packet_members(expanded)
            if expanded_members:
                expanded_with_guard = expanded + bytes(USER_SECTOR)
                for member_index, (start, end) in enumerate(expanded_members):
                    if end - start < 4:
                        continue
                    try:
                        member, member_consumed = lzss_decompress(expanded_with_guard, start)
                    except ValueError:
                        continue
                    add(member, entry, fat_index, routes, [
                        {"type": "whole_file_lzss", "stored_offset": 0, "consumed_size": consumed},
                        {
                            "type": "packet_member_lzss",
                            "member_index": member_index,
                            "stored_offset": start,
                            "logical_stored_size": end - start,
                            "consumed_size": member_consumed,
                        },
                    ], len(member))
        route_ids = {(int(route["directory"], 16), int(route["file_id"], 16)) for route in routes}
        actor_route = next(((directory_id, file_id) for directory_id, file_id in route_ids if directory_id == 0x04 and file_id >= 0xB8 and (file_id - 0xB8) % 2 == 0), None)
        if actor_route and len(raw) >= 0x154:
            section_at = u32(raw, 0x13C)
            declared = u32(raw, 0x118)
            if section_at and declared and section_at < len(raw):
                try:
                    section, consumed = lzss_decompress(padded, section_at)
                except ValueError:
                    pass
                else:
                    if len(section) > declared + 0x10:
                        continue
                    add(section, entry, fat_index, routes, [{
                        "type": "field_actor_section3_lzss",
                        "section": 3,
                        "stored_offset": section_at,
                        "consumed_size": consumed,
                        "declared_expanded_size": declared,
                        "actual_expanded_size": len(section),
                        "declared_size_matches": declared == len(section),
                        "within_loader_allocation": len(section) <= declared + 0x10,
                    }], len(section))
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
            "xa_stream_extent_count": xa_children,
            "xa_stream_bytes": sum(
                max(0, entry.size) for entry in entries[1 : xa_children + 1]
            ),
        },
        "occurrences": len(found),
        "skipped_extent_count": len(skipped_extents),
        "skipped_extents": skipped_extents,
    }
    return disc_record, found


def write_if_changed(path: Path, content: bytes) -> None:
    if path.is_file() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(content)
    temporary.replace(path)


def _sheet_occurrence(
    disc_index: int,
    disc_path: Path,
    entry: FatEntry,
    fat_index: int,
    routes: list[dict],
    member_index: int,
    decoded_offset: int,
    source_type: str,
) -> dict:
    return {
        "disc_index": disc_index,
        "disc_name": disc_path.name,
        "fat_index": fat_index,
        "lba": entry.lba,
        "stored_size": entry.size,
        "container_size": entry.size,
        "container_bundle_index": member_index,
        "decoded_container_offset": decoded_offset,
        "embedded": True,
        "subsystem": _subsystem(routes),
        "transform_chain": [{"type": source_type, "member_index": member_index}],
        "routes": routes,
    }


def _create_sheet_record(
    output: Path,
    resource_id: str,
    payload: bytes,
    images: list[dict],
    actor_metadata: dict,
    actor_sha256: str,
    occurrence: dict,
    source_type: str,
    previews: bool,
) -> dict:
    payload_digest = hashlib.sha256(payload).hexdigest()
    asset_relative = Path("assets") / f"{payload_digest}.bin"
    write_if_changed(output / asset_relative, payload)
    entries = []
    for index, image in enumerate(images):
        image_payload = struct.pack("<HH", image["width_words"], image["height"]) + image["data"]
        image_digest = hashlib.sha256(image_payload).hexdigest()
        image_relative = Path("assets") / f"{image_digest}.bin"
        write_if_changed(output / image_relative, image_payload)
        entries.append({
            "index": index,
            "role": "raw_vram_image",
            "sha256": image_digest,
            "size": len(image_payload),
            "path": image_relative.as_posix(),
        })
    bpps = sorted({
        tile["atlas"]["bpp"]
        for frame in actor_metadata["frames"]
        for tile in frame["tiles"]
    })
    if bpps != [4]:
        raise ValueError("raw sprite sheet is not exclusively 4-bpp")
    preview_records = []
    if previews:
        for variant in actor_metadata["palette"]["variants"]:
            for bank in variant["banks"]:
                colors = [int(value, 16) for value in bank["rgb555"]]
                png, render_metadata = render_raw_sprite_sheet(images, colors, 4)
                png_digest = hashlib.sha256(png).hexdigest()
                png_relative = Path("assets") / f"{png_digest}.png"
                write_if_changed(output / png_relative, png)
                preview_records.append({
                    "frame_id": None,
                    "palette_variant": variant["variant"],
                    "palette_bank": bank["bank"],
                    "sha256": png_digest,
                    "size": len(png),
                    "path": png_relative.as_posix(),
                    **render_metadata,
                })
    detailed_metadata = {
        "kind": "sprite_sheet",
        "source_type": source_type,
        "paired_actor_sha256": actor_sha256,
        "bpp": 4,
        "image_count": len(images),
        "images": [
            {"index": index, "width_words": image["width_words"], "height": image["height"]}
            for index, image in enumerate(images)
        ],
        "palette": actor_metadata["palette"],
    }
    metadata_relative = Path("metadata") / f"{resource_id}.json"
    write_if_changed(
        output / metadata_relative,
        (json.dumps(detailed_metadata, indent=2, ensure_ascii=True) + "\n").encode("ascii"),
    )
    return {
        "sha256": resource_id,
        "content_sha256": payload_digest,
        "size": len(payload),
        "path": asset_relative.as_posix(),
        "metadata_path": metadata_relative.as_posix(),
        "metadata": {
            "source_type": source_type,
            "entry_count": len(images),
            "animation_count": 0,
            "kind": "sprite_sheet",
            "frame_count": 0,
            "tile_capacity": 0,
            "vram_prebacked": True,
            "palette_bank_count": actor_metadata["palette"]["bank_count"],
            "palette_variant_count": actor_metadata["palette"]["payload_variant_count"],
        },
        "entries": entries,
        "previews": preview_records,
        "preview_incomplete_reasons": [] if previews else ["preview_generation_disabled"],
        "occurrences": [occurrence],
    }


def _create_external_actor_record(
    output: Path,
    resource_id: str,
    payload: bytes,
    metadata: dict,
    occurrence: dict,
    source_type: str,
    vram: bytearray,
    coverage: bytearray,
    texture_x: int,
    texture_y: int,
    previews: bool,
    texture_page_mode: str = "linear",
) -> dict | None:
    bundle_digest = hashlib.sha256(payload).hexdigest()
    asset_relative = Path("assets") / f"{bundle_digest}.bin"
    write_if_changed(output / asset_relative, payload)
    offsets = [int(value, 16) for value in metadata["entry_offsets"]]
    entries = []
    for index, (start, end) in enumerate(zip(offsets, offsets[1:])):
        entry = payload[start:end]
        entry_digest = hashlib.sha256(entry).hexdigest()
        entry_relative = Path("assets") / f"{entry_digest}.bin"
        write_if_changed(output / entry_relative, entry)
        entries.append({
            "index": index,
            "role": metadata["entry_roles"][index],
            "sha256": entry_digest,
            "size": len(entry),
            "path": entry_relative.as_posix(),
        })
    preview_records = []
    preview_failures = []
    if previews:
        variant_count = max(1, metadata["palette"]["payload_variant_count"])
        for frame in metadata["frames"]:
            if not any(tile["atlas"]["width"] and tile["atlas"]["height"] for tile in frame["tiles"]):
                continue
            for variant in range(variant_count):
                try:
                    png, render_metadata = render_static_frame_from_vram(
                        vram, coverage, texture_x, texture_y, metadata, frame, variant,
                        texture_page_mode,
                    )
                except ValueError as error:
                    preview_failures.append(f"frame_{frame['id']}_palette_{variant}:{error}")
                    continue
                png_digest = hashlib.sha256(png).hexdigest()
                png_relative = Path("assets") / f"{png_digest}.png"
                write_if_changed(output / png_relative, png)
                preview_records.append({
                    "frame_id": frame["id"],
                    "palette_variant": variant,
                    "sha256": png_digest,
                    "size": len(png),
                    "path": png_relative.as_posix(),
                    **render_metadata,
                })
    else:
        preview_failures.append("preview_generation_disabled")
    if previews and not preview_records:
        return None
    detailed_metadata = dict(metadata)
    detailed_metadata["external_texture_context"] = {
        "source_type": source_type,
        "texture_x": texture_x,
        "texture_y": texture_y,
        "texture_page_mode": texture_page_mode,
    }
    metadata_relative = Path("metadata") / f"{resource_id}.json"
    write_if_changed(
        output / metadata_relative,
        (json.dumps(detailed_metadata, indent=2, ensure_ascii=True) + "\n").encode("ascii"),
    )
    return {
        "sha256": resource_id,
        "bundle_sha256": bundle_digest,
        "size": len(payload),
        "path": asset_relative.as_posix(),
        "metadata_path": metadata_relative.as_posix(),
        "metadata": {
            "source_type": source_type,
            "entry_count": metadata["entry_count"],
            "animation_count": metadata["animation_count"],
            "kind": "actor",
            "frame_count": metadata["frame_count"],
            "tile_capacity": metadata["tile_capacity"],
            "vram_prebacked": True,
            "palette_bank_count": metadata["palette"]["bank_count"],
            "palette_variant_count": metadata["palette"]["payload_variant_count"],
        },
        "entries": entries,
        "previews": preview_records,
        "preview_incomplete_reasons": sorted(set(preview_failures)),
        "occurrences": [occurrence],
    }


def extract_external_actor_records(
    disc_paths: list[Path],
    output: Path,
    *,
    previews: bool,
) -> list[dict]:
    unique: dict[str, dict] = {}
    for disc_index, path in enumerate(disc_paths):
        disc = open_disc(path)
        entries = parse_fat(disc.read_user_data(FAT_LBA, FAT_SECTORS * USER_SECTOR), disc.sector_count)
        directory = parse_directory_table(disc.read_user_data(DIRECTORY_LBA, DIRECTORY_COUNT * 2))
        routes_by_index = map_physical_routes(directory, len(entries))
        route_to_fat = {
            (int(route["directory"], 16), int(route["file_id"], 16)): fat_index
            for fat_index, routes in routes_by_index.items()
            for route in routes
        }
        for fat_index, entry in enumerate(entries):
            routes = routes_by_index.get(fat_index, [])
            route_ids = {(int(route["directory"], 16), int(route["file_id"], 16)) for route in routes}
            field_route = next((
                (directory_id, file_id)
                for directory_id, file_id in route_ids
                if directory_id == 0x04 and file_id >= 0xB8 and (file_id - 0xB8) % 2 == 0
            ), None)
            if field_route and entry.size >= 0x154:
                companion_fat = route_to_fat.get((0x04, field_route[1] + 1))
                if companion_fat is None or entries[companion_fat].size <= 0:
                    continue
                actor_file = disc.read_user_data(entry.lba, entry.size, padded=True)
                companion_entry = entries[companion_fat]
                companion = disc.read_user_data(companion_entry.lba, companion_entry.size)
                try:
                    section3, _ = lzss_decompress(actor_file, u32(actor_file, 0x13C))
                    section4, _ = lzss_decompress(actor_file, u32(actor_file, 0x140))
                    bundle_spans = parse_offset_bundle(section3)
                    sheet_spans = parse_offset_bundle(section4)
                    descriptors = parse_sector_graphics(companion)
                except (ValueError, struct.error):
                    continue
                vram, coverage = create_vram()
                upload_sector_graphics(vram, coverage, descriptors)
                for sheet_index, (sheet_start, sheet_end) in enumerate(sheet_spans):
                    mapping = struct.unpack_from("<hhhh", actor_file, sheet_index * 8)
                    if mapping[3] != 0:
                        continue
                    try:
                        images, _ = parse_raw_vram_images(section4[sheet_start:sheet_end])
                    except (ValueError, struct.error):
                        continue
                    for image_index, image in enumerate(images):
                        upload_vram(
                            vram, coverage, mapping[0] + image_index * 64, mapping[1],
                            image["width_words"], image["height"], image["data"],
                        )
                for actor_index, (bundle_start, bundle_end) in enumerate(bundle_spans):
                    try:
                        bundle_size, metadata = parse_sprite_bundle(section3, bundle_start)
                    except (ValueError, struct.error):
                        continue
                    if metadata["kind"] != "actor" or not metadata["vram_prebacked"]:
                        continue
                    mapping = struct.unpack_from("<hhhh", actor_file, actor_index * 8)
                    try:
                        texture_digest = static_texture_fingerprint(
                            vram, coverage, mapping[0], mapping[1], metadata, "field_ring"
                        )
                    except ValueError:
                        continue
                    bundle = section3[bundle_start : bundle_start + bundle_size]
                    bundle_digest = hashlib.sha256(bundle).digest()
                    resource_id = hashlib.sha256(
                        b"field-static-sprite-v1\0" + bundle_digest + texture_digest
                    ).hexdigest()
                    occurrence = _sheet_occurrence(
                        disc_index, path, entry, fat_index, routes,
                        actor_index, bundle_start, "field_reconstructed_vram",
                    )
                    occurrence["texture_source"] = {
                        "fat_index": companion_fat,
                        "lba": companion_entry.lba,
                        "stored_size": companion_entry.size,
                        "routes": routes_by_index.get(companion_fat, []),
                    }
                    if resource_id in unique:
                        unique[resource_id]["occurrences"].append(occurrence)
                        continue
                    record = _create_external_actor_record(
                        output, resource_id, bundle, metadata, occurrence,
                        "field_reconstructed_vram", vram, coverage,
                        mapping[0], mapping[1], previews, "field_ring",
                    )
                    if record is not None:
                        unique[resource_id] = record
                continue
            enemy_route = next((
                (directory_id, file_id)
                for directory_id, file_id in route_ids
                if directory_id == 0x0D and file_id >= 3 and file_id % 2 == 1
            ), None)
            if enemy_route and entry.size > 0:
                visual_file = disc.read_user_data(entry.lba, entry.size)
                for pair in parse_battle_enemy_visual_pairs(visual_file):
                    metadata = pair["metadata"]
                    if metadata["kind"] != "actor" or not metadata["vram_prebacked"]:
                        continue
                    vram, coverage = create_vram()
                    for image_index, image in enumerate(pair["images"]):
                        upload_vram(
                            vram, coverage, image_index * 64, 0,
                            image["width_words"], image["height"], image["data"],
                        )
                    texture_digest = static_texture_fingerprint(vram, coverage, 0, 0, metadata)
                    bundle = pair["bundle"]
                    resource_id = hashlib.sha256(
                        b"battle-enemy-static-sprite-v1\0"
                        + hashlib.sha256(bundle).digest()
                        + texture_digest
                    ).hexdigest()
                    occurrence = _sheet_occurrence(
                        disc_index, path, entry, fat_index, routes,
                        pair["record_index"], pair["actor_offset"],
                        "battle_enemy_reconstructed_vram",
                    )
                    occurrence["texture_source"] = {
                        "fat_index": fat_index,
                        "lba": entry.lba,
                        "stored_size": entry.size,
                        "texture_pack_offset": pair["texture_offset"],
                        "image_count": len(pair["images"]),
                        "initial_vram_page": pair["initial_vram_page"],
                    }
                    if resource_id in unique:
                        unique[resource_id]["occurrences"].append(occurrence)
                        continue
                    record = _create_external_actor_record(
                        output, resource_id, bundle, metadata, occurrence,
                        "battle_enemy_reconstructed_vram", vram, coverage, 0, 0, previews,
                    )
                    if record is not None:
                        unique[resource_id] = record
                continue
            action_route = next((
                (directory_id, file_id)
                for directory_id, file_id in route_ids
                if directory_id == 0x2A
            ), None)
            if action_route and entry.size > 0:
                action_file = disc.read_user_data(entry.lba, entry.size)
                action = parse_battle_action_sprite(action_file)
                if action is None:
                    continue
                vram, coverage = create_vram()
                upload_sector_graphics(vram, coverage, action["descriptors"])
                texture_digest = hashlib.sha256(action["texture"]).digest()
                bundle = action["bundle"]
                resource_id = hashlib.sha256(
                    b"battle-action-static-sprite-v1\0"
                    + hashlib.sha256(bundle).digest()
                    + texture_digest
                ).hexdigest()
                static_texture_fingerprint(vram, coverage, 896, 256, action["metadata"])
                occurrence = _sheet_occurrence(
                    disc_index, path, entry, fat_index, routes,
                    0, action["actor_offset"], "battle_action_reconstructed_vram",
                )
                occurrence["texture_source"] = {
                    "fat_index": fat_index,
                    "lba": entry.lba,
                    "stored_size": entry.size,
                    "packed_image_offset": action["texture_offset"],
                }
                if resource_id in unique:
                    unique[resource_id]["occurrences"].append(occurrence)
                    continue
                record = _create_external_actor_record(
                    output, resource_id, bundle, action["metadata"], occurrence,
                    "battle_action_reconstructed_vram", vram, coverage, 896, 256, previews,
                )
                if record is not None:
                    unique[resource_id] = record
                continue
            common_route = next((
                (directory_id, file_id)
                for directory_id, file_id in route_ids
                if directory_id == 0x2C and file_id == 2
            ), None)
            if common_route and entry.size > 0:
                texture_fat = route_to_fat.get((0x2C, 1))
                if texture_fat is None or entries[texture_fat].size <= 0:
                    raise ValueError("Battle common static texture file is missing")
                bundle = disc.read_user_data(entry.lba, entry.size)
                bundle_size, metadata = parse_sprite_bundle(bundle)
                if metadata["kind"] != "actor" or not metadata["vram_prebacked"]:
                    raise ValueError("Battle common sprite file is not a static actor")
                bundle = bundle[:bundle_size]
                texture_entry = entries[texture_fat]
                texture = disc.read_user_data(texture_entry.lba, texture_entry.size)
                descriptors = parse_packed_image_set(texture, coordinate_mode=0)
                vram, coverage = create_vram()
                upload_sector_graphics(vram, coverage, descriptors)
                static_texture_fingerprint(vram, coverage, 896, 0, metadata)
                resource_id = hashlib.sha256(
                    b"battle-common-static-sprite-v1\0"
                    + hashlib.sha256(bundle).digest()
                    + hashlib.sha256(texture).digest()
                ).hexdigest()
                occurrence = _sheet_occurrence(
                    disc_index, path, entry, fat_index, routes,
                    0, 0, "battle_common_reconstructed_vram",
                )
                occurrence["texture_source"] = {
                    "fat_index": texture_fat,
                    "lba": texture_entry.lba,
                    "stored_size": texture_entry.size,
                    "routes": routes_by_index.get(texture_fat, []),
                }
                if resource_id in unique:
                    unique[resource_id]["occurrences"].append(occurrence)
                    continue
                record = _create_external_actor_record(
                    output, resource_id, bundle, metadata, occurrence,
                    "battle_common_reconstructed_vram", vram, coverage, 896, 0, previews,
                )
                if record is not None:
                    unique[resource_id] = record
                continue
            effect_route = next((
                (int(route["directory"], 16), int(route["file_id"], 16))
                for route in routes
                if int(route["directory"], 16) == 0x0E
            ), None)
            if effect_route is None or entry.size <= 0:
                continue
            effect = disc.read_user_data(entry.lba, entry.size)
            actors = [
                candidate for candidate in scan_bundles(effect)
                if candidate.metadata["kind"] == "actor" and candidate.metadata["vram_prebacked"]
            ]
            if not actors:
                continue
            texture_fat = route_to_fat.get((0x0E, effect_route[1] + 1))
            if texture_fat is None or entries[texture_fat].size <= 0:
                continue
            texture_entry = entries[texture_fat]
            texture = disc.read_user_data(texture_entry.lba, texture_entry.size)
            try:
                descriptors = parse_sector_graphics(texture)
            except (ValueError, struct.error):
                continue
            if not descriptors or not any(item["tag"] == 0x1200 for item in descriptors):
                continue
            vram, coverage = create_vram()
            upload_sector_graphics(vram, coverage, descriptors)
            texture_digest = hashlib.sha256(texture).digest()
            for actor_index, candidate in enumerate(actors):
                actor_digest = hashlib.sha256(candidate.payload).digest()
                resource_id = hashlib.sha256(b"battle-vfx-sprite-v1\0" + actor_digest + texture_digest).hexdigest()
                occurrence = _sheet_occurrence(
                    disc_index, path, entry, fat_index, routes,
                    actor_index, candidate.decoded_offset, "battle_vfx_descriptor_pair",
                )
                occurrence["texture_source"] = {
                    "fat_index": texture_fat,
                    "lba": texture_entry.lba,
                    "stored_size": texture_entry.size,
                    "routes": routes_by_index.get(texture_fat, []),
                }
                if resource_id in unique:
                    unique[resource_id]["occurrences"].append(occurrence)
                    continue
                record = _create_external_actor_record(
                    output, resource_id, candidate.payload, candidate.metadata, occurrence,
                    "battle_vfx_descriptor_pair", vram, coverage, 896, 256, previews,
                )
                if record is not None:
                    unique[resource_id] = record
    for record in unique.values():
        record["occurrences"].sort(key=lambda value: (
            value["disc_index"], value["fat_index"], value["container_bundle_index"],
        ))
    return [unique[key] for key in sorted(unique)]


def extract_sprite_sheet_records(
    disc_paths: list[Path],
    output: Path,
    *,
    previews: bool,
) -> list[dict]:
    unique: dict[str, dict] = {}
    for disc_index, path in enumerate(disc_paths):
        disc = open_disc(path)
        entries = parse_fat(disc.read_user_data(FAT_LBA, FAT_SECTORS * USER_SECTOR), disc.sector_count)
        directory = parse_directory_table(disc.read_user_data(DIRECTORY_LBA, DIRECTORY_COUNT * 2))
        routes_by_index = map_physical_routes(directory, len(entries))
        for fat_index, entry in enumerate(entries):
            routes = routes_by_index.get(fat_index, [])
            route_ids = {(int(route["directory"], 16), int(route["file_id"], 16)) for route in routes}
            field_route = next((
                (directory_id, file_id)
                for directory_id, file_id in route_ids
                if directory_id == 0x04 and file_id >= 0xB8 and (file_id - 0xB8) % 2 == 0
            ), None)
            if field_route and entry.size >= 0x154:
                actor_file = disc.read_user_data(entry.lba, entry.size, padded=True)
                try:
                    section3, _ = lzss_decompress(actor_file, u32(actor_file, 0x13C))
                    section4, _ = lzss_decompress(actor_file, u32(actor_file, 0x140))
                    bundle_spans = parse_offset_bundle(section3)
                    sheet_spans = parse_offset_bundle(section4)
                except (ValueError, struct.error):
                    continue
                for sheet_index, (sheet_start, sheet_end) in enumerate(sheet_spans):
                    if sheet_index >= len(bundle_spans):
                        break
                    mapping = struct.unpack_from("<hhhh", actor_file, sheet_index * 8)
                    if mapping[3] != 0:
                        continue
                    bundle_start, bundle_end = bundle_spans[sheet_index]
                    try:
                        bundle_size, actor_metadata = parse_sprite_bundle(section3, bundle_start)
                        images, _ = parse_raw_vram_images(section4[sheet_start:sheet_end])
                    except (ValueError, struct.error):
                        continue
                    if (
                        actor_metadata["kind"] != "actor" or not actor_metadata["vram_prebacked"]
                        or not images or not actor_metadata["palette"]["payload_variant_count"]
                    ):
                        continue
                    bundle = section3[bundle_start : bundle_start + bundle_size]
                    sheet = section4[sheet_start:sheet_end]
                    actor_digest = hashlib.sha256(bundle).hexdigest()
                    palette_entry = bundle[
                        int(actor_metadata["entry_offsets"][2], 16) : int(actor_metadata["entry_offsets"][3], 16)
                    ]
                    resource_id = hashlib.sha256(b"field-sprite-sheet-v1\0" + sheet + palette_entry).hexdigest()
                    occurrence = _sheet_occurrence(
                        disc_index, path, entry, fat_index, routes,
                        sheet_index, sheet_start, "field_actor_section4_sheet",
                    )
                    if resource_id in unique:
                        unique[resource_id]["occurrences"].append(occurrence)
                    else:
                        unique[resource_id] = _create_sheet_record(
                            output, resource_id, sheet, images, actor_metadata, actor_digest,
                            occurrence, "field_npc_sheet", previews,
                        )
                continue
            battle_enemy_route = next((
                (directory_id, file_id)
                for directory_id, file_id in route_ids
                if directory_id == 0x0D and file_id >= 3 and file_id % 2 == 1
            ), None)
            if not battle_enemy_route or entry.size <= 0:
                continue
            visual_file = disc.read_user_data(entry.lba, entry.size)
            for pair in parse_battle_enemy_visual_pairs(visual_file):
                actor_metadata = pair["metadata"]
                if (
                    actor_metadata["kind"] != "actor" or not actor_metadata["vram_prebacked"]
                    or not pair["images"] or not actor_metadata["palette"]["payload_variant_count"]
                ):
                    continue
                sheet = visual_file[pair["texture_offset"] : pair["texture_end"]]
                offsets = [int(value, 16) for value in actor_metadata["entry_offsets"]]
                palette_entry = pair["bundle"][offsets[2] : offsets[3]]
                actor_digest = hashlib.sha256(pair["bundle"]).hexdigest()
                resource_id = hashlib.sha256(b"battle-enemy-sheet-v1\0" + sheet + palette_entry).hexdigest()
                occurrence = _sheet_occurrence(
                    disc_index, path, entry, fat_index, routes,
                    pair["record_index"], pair["texture_offset"], "battle_enemy_raw_vram_sheet",
                )
                if resource_id in unique:
                    unique[resource_id]["occurrences"].append(occurrence)
                else:
                    unique[resource_id] = _create_sheet_record(
                        output, resource_id, sheet, pair["images"], actor_metadata, actor_digest,
                        occurrence, "battle_enemy_sheet", previews,
                    )
    for record in unique.values():
        record["occurrences"].sort(key=lambda value: (
            value["disc_index"], value["fat_index"], value["container_bundle_index"],
        ))
    return [unique[key] for key in sorted(unique)]


def _slug(value: str, limit: int = 72) -> str:
    normalized = unicodedata.normalize("NFKD", value).encode("ascii", "ignore").decode("ascii")
    normalized = re.sub(r"\[[A-Z0-9, /+-]+\]", " ", normalized)
    normalized = re.sub(r"[^a-z0-9]+", "-", normalized.lower()).strip("-")
    return normalized[:limit].rstrip("-") or "unnamed"


def _preferred_occurrence(record: dict) -> dict:
    return min(record["occurrences"], key=lambda occurrence: (
        occurrence["disc_index"],
        occurrence["fat_index"],
        occurrence["container_bundle_index"],
        occurrence.get("decoded_container_offset", 0),
    ))


def _catalog_identity(record: dict) -> tuple[str, str, str, dict]:
    occurrence = _preferred_occurrence(record)
    source_type = record["metadata"].get("source_type")
    if record["metadata"]["kind"] == "actor":
        category = "static" if record["metadata"]["vram_prebacked"] else "dynamic"
        if category == "dynamic":
            conceptual = f"{occurrence['subsystem']}-dynamic-actor"
        else:
            conceptual = {
                "field_reconstructed_vram": "field-static-actor",
                "battle_enemy_reconstructed_vram": "battle-enemy-static",
                "battle_action_reconstructed_vram": "battle-action-static",
                "battle_common_reconstructed_vram": "battle-common-static",
                "battle_vfx_descriptor_pair": "battle-vfx-static",
            }.get(source_type, f"{occurrence['subsystem']}-static-actor")
    elif source_type == "field_npc_sheet":
        category = "NPC-sheet"
        conceptual = "NPC-sheet"
    elif source_type == "battle_enemy_sheet":
        category = "enemy-sheet"
        conceptual = "enemy-sheet"
    else:
        raise ValueError(f"unsupported catalog resource source type: {source_type}")
    name = f"{conceptual}_{occurrence['fat_index']}_{record['sha256'][:10]}"
    return name, name, category, occurrence


def _hardlink_or_copy(source: Path, destination: Path) -> None:
    try:
        os.link(source, destination)
    except OSError:
        shutil.copy2(source, destination)


def _relative_symlink(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.symlink(os.path.relpath(source, destination.parent), destination)
    except OSError:
        # Windows requires elevation or Developer Mode for symbolic links.
        _hardlink_or_copy(source, destination)


def build_catalog(output: Path, records: list[dict]) -> dict:
    catalog_root = output / "catalog"
    if catalog_root.exists():
        shutil.rmtree(catalog_root)
    catalog_records = []
    for record in records:
        folder_name, display_name, category, occurrence = _catalog_identity(record)
        relative = Path("catalog") / occurrence["subsystem"] / category / folder_name
        destination = output / relative
        asset_name = "sheet.bin" if record["metadata"]["kind"] == "sprite_sheet" else "bundle.bin"
        _relative_symlink(output / record["path"], destination / asset_name)
        _relative_symlink(output / record["metadata_path"], destination / "metadata.json")
        for entry in record["entries"]:
            entry_name = f"{entry['index']:02d}-{_slug(entry['role'])}.bin"
            _relative_symlink(output / entry["path"], destination / "entries" / entry_name)
        palette_variants = {preview["palette_variant"] for preview in record["previews"]}
        for preview in record["previews"]:
            if preview["preview_type"] in {"rendered_frame", "rendered_external_frame"}:
                preview_name = f"frame-{preview['frame_id']:04d}_palette-{preview['palette_variant']:02d}.png"
            elif preview["preview_type"] == "rendered_sheet":
                preview_name = (
                    f"sheet_palette-{preview['palette_variant']:02d}"
                    f"_bank-{preview['palette_bank']:02d}.png"
                )
            else:
                raise ValueError(f"unsupported catalog preview type: {preview['preview_type']}")
            preview_relative = relative / "previews"
            if len(palette_variants) > 1:
                preview_relative /= f"palette-{preview['palette_variant']:02d}"
            preview_relative /= preview_name
            preview_destination = output / preview_relative
            preview_destination.parent.mkdir(parents=True, exist_ok=True)
            _hardlink_or_copy(output / preview["path"], preview_destination)
            preview["path"] = preview_relative.as_posix()
        source_record = {
            "sha256": record["sha256"],
            "display_name": display_name,
            "occurrences": record["occurrences"],
        }
        write_if_changed(
            destination / "sources.json",
            (json.dumps(source_record, indent=2, ensure_ascii=True) + "\n").encode("ascii"),
        )
        record["catalog_path"] = relative.as_posix()
        catalog_records.append({
            "sha256": record["sha256"],
            "display_name": display_name,
            "path": relative.as_posix(),
            "subsystem": occurrence["subsystem"],
            "category": category,
        })
    catalog = {
        "schema": SCHEMA,
        "resource_count": len(catalog_records),
        "resources": sorted(catalog_records, key=lambda item: item["path"]),
    }
    write_if_changed(
        catalog_root / "catalog.json",
        (json.dumps(catalog, indent=2, ensure_ascii=True) + "\n").encode("ascii"),
    )
    return catalog


def extract_sprites(
    discs: Iterable[Path],
    output: Path,
    *,
    previews: bool = True,
) -> dict:
    disc_paths = list(discs)
    if not disc_paths:
        raise ValueError("at least one disc is required")
    unique: dict[str, dict] = {}
    static_candidates: dict[str, list[dict]] = {}
    disc_records = []
    for disc_index, path in enumerate(disc_paths):
        disc_record, candidates = scan_disc(path, disc_index)
        disc_records.append(disc_record)
        for candidate, occurrence in candidates:
            if candidate.metadata["kind"] != "actor":
                continue
            digest = hashlib.sha256(candidate.payload).hexdigest()
            if candidate.metadata["vram_prebacked"]:
                static_candidates.setdefault(digest, []).append(occurrence)
                continue
            if digest not in unique:
                unique[digest] = {"candidate": candidate, "occurrences": []}
            unique[digest]["occurrences"].append(occurrence)
    output.mkdir(parents=True, exist_ok=True)
    records = []
    png_hashes = set()
    battle_common_cluts: dict[int, list[int]] = {}
    for digest in sorted(unique):
        candidate = unique[digest]["candidate"]
        payload = candidate.payload
        metadata = candidate.metadata
        metadata_relative = Path("metadata") / f"{digest}.json"
        write_if_changed(
            output / metadata_relative,
            (json.dumps(metadata, indent=2, ensure_ascii=True) + "\n").encode("ascii"),
        )
        asset_relative = Path("assets") / f"{digest}.bin"
        write_if_changed(output / asset_relative, payload)
        offsets = [int(value, 16) for value in metadata["entry_offsets"]]
        entries = []
        for index, (start, end) in enumerate(zip(offsets, offsets[1:])):
            entry = payload[start:end]
            entry_digest = hashlib.sha256(entry).hexdigest()
            entry_relative = Path("assets") / f"{entry_digest}.bin"
            write_if_changed(output / entry_relative, entry)
            entries.append({
                "index": index,
                "role": metadata["entry_roles"][index],
                "sha256": entry_digest,
                "size": len(entry),
                "path": entry_relative.as_posix(),
            })
        preview_records = []
        preview_failures = []
        def add_preview(png: bytes, frame_id: int | None, variant: int | None, render_metadata: dict) -> None:
            png_digest = hashlib.sha256(png).hexdigest()
            png_relative = Path("assets") / f"{png_digest}.png"
            write_if_changed(output / png_relative, png)
            png_hashes.add(png_digest)
            preview_records.append({
                "frame_id": frame_id,
                "palette_variant": variant,
                "sha256": png_digest,
                "size": len(png),
                "path": png_relative.as_posix(),
                **render_metadata,
            })

        if not previews:
            preview_failures.append("preview_generation_disabled")
        elif metadata["palette"]["payload_variant_count"] == 0:
            preview_failures.append("bundle_has_no_local_palette_matrix")
        else:
            for frame in metadata["frames"]:
                if not any(
                    tile["source"]["width"] and tile["source"]["height"]
                    for tile in frame["tiles"]
                ):
                    continue
                for variant in range(metadata["palette"]["payload_variant_count"]):
                    external_colors_by_bank = None
                    if "external_or_specialized_8bpp_clut" in frame["dependencies"]:
                        source_disc = unique[digest]["occurrences"][0]["disc_index"]
                        if source_disc not in battle_common_cluts:
                            battle_common_cluts[source_disc] = load_battle_common_clut(disc_paths[source_disc])
                        clut_row = list(battle_common_cluts[source_disc])
                        local_colors = [
                            int(value, 16)
                            for bank in metadata["palette"]["variants"][variant]["banks"]
                            for value in bank["rgb555"]
                        ]
                        clut_row[: len(local_colors)] = local_colors
                        external_colors_by_bank = {}
                        for tile in frame["tiles"]:
                            if tile["source"]["bpp"] != 8:
                                continue
                            start = tile["palette_bank"] * 16
                            external_colors_by_bank[tile["palette_bank"]] = clut_row[start : start + 256]
                    try:
                        png, render_metadata = render_dynamic_frame(
                            payload, metadata, frame, variant, external_colors_by_bank
                        )
                    except ValueError as error:
                        preview_failures.append(f"frame_{frame['id']}_palette_{variant}:{error}")
                        continue
                    if external_colors_by_bank is not None:
                        render_metadata["palette_source"] = "battle_common_clut_plus_local_palette_matrix"
                        render_metadata["clut_uninitialized_words_assumed_zero"] = 16
                    add_preview(png, frame["id"], variant, render_metadata)
        occurrences = sorted(unique[digest]["occurrences"], key=lambda value: (
            value["disc_index"], value["fat_index"],
            json.dumps(value["transform_chain"], sort_keys=True), value["decoded_container_offset"],
        ))
        records.append({
            "sha256": digest,
            "size": len(payload),
            "path": asset_relative.as_posix(),
            "metadata_path": metadata_relative.as_posix(),
            "metadata": {
                "entry_count": metadata["entry_count"],
                "animation_count": metadata["animation_count"],
                "kind": metadata["kind"],
                "frame_count": metadata["frame_count"],
                "tile_capacity": metadata["tile_capacity"],
                "vram_prebacked": metadata["vram_prebacked"],
                "palette_bank_count": (
                    metadata["palette"]["bank_count"]
                    if metadata["palette"] is not None
                    else None
                ),
                "palette_variant_count": (
                    metadata["palette"]["payload_variant_count"]
                    if metadata["palette"] is not None
                    else None
                ),
            },
            "entries": entries,
            "previews": preview_records,
            "preview_incomplete_reasons": sorted(set(preview_failures)),
            "occurrences": occurrences,
        })
    sheet_records = extract_sprite_sheet_records(
        disc_paths,
        output,
        previews=previews,
    )
    external_actor_records = extract_external_actor_records(
        disc_paths,
        output,
        previews=previews,
    )
    rendered_static_hashes = {record["bundle_sha256"] for record in external_actor_records}
    inherited_static_hashes = {
        digest for digest, occurrences in static_candidates.items()
        if all(any(
            route["directory"] == "0x04" and int(route["file_id"], 16) in {0x7C0, 0x7C1}
            for route in occurrence["routes"]
        ) for occurrence in occurrences)
    }
    missing_static_hashes = set(static_candidates) - rendered_static_hashes - inherited_static_hashes
    if missing_static_hashes:
        raise ValueError(
            "static sprite coverage is incomplete: "
            + ", ".join(sorted(missing_static_hashes))
        )
    failed_static_records = [
        record for record in external_actor_records
        if previews and (not record["previews"] or record["preview_incomplete_reasons"])
    ]
    if failed_static_records:
        raise ValueError(
            "one or more static sprite frames failed to render: "
            + ", ".join(record["sha256"] for record in failed_static_records[:8])
        )
    records.extend(sheet_records)
    records.extend(external_actor_records)
    records.sort(key=lambda record: record["sha256"])
    png_hashes.update(
        preview["sha256"]
        for record in sheet_records + external_actor_records
        for preview in record["previews"]
    )
    occurrence_count = sum(len(record["occurrences"]) for record in records)
    summary = {
        "unique_resources": len(records),
        "occurrences": occurrence_count,
        "unique_entries": len({entry["sha256"] for record in records for entry in record["entries"]}),
        "unique_png_previews": len(png_hashes),
        "preview_references": sum(len(record["previews"]) for record in records),
        "resources_with_previews": sum(bool(record["previews"]) for record in records),
        "previews_by_type": dict(sorted(Counter(
            preview["preview_type"] for record in records for preview in record["previews"]
        ).items())),
        "by_kind": dict(sorted(Counter(record["metadata"]["kind"] for record in records).items())),
        "static_coverage": {
            "candidate_bundle_hashes": len(static_candidates),
            "rendered_bundle_hashes": len(set(static_candidates) & rendered_static_hashes),
            "inherited_animation_bundle_hashes": len(inherited_static_hashes),
            "unexpected_missing_bundle_hashes": len(missing_static_hashes),
        },
        "occurrences_by_disc": dict(sorted(Counter(
            str(occurrence["disc_index"])
            for record in records
            for occurrence in record["occurrences"]
        ).items())),
    }
    catalog = build_catalog(output, records)
    manifest = {
        "schema": SCHEMA,
        "discs": disc_records,
        "summary": summary,
        "catalog": {"path": "catalog/catalog.json", "resource_count": catalog["resource_count"]},
        "resources": records,
    }
    write_if_changed(output / "manifest.json", (json.dumps(manifest, indent=2, ensure_ascii=True) + "\n").encode("ascii"))
    referenced_paths = {"manifest.json"}
    for record in records:
        referenced_paths.add(record["path"])
        referenced_paths.add(record["metadata_path"])
        referenced_paths.update(entry["path"] for entry in record["entries"])
        referenced_paths.update(preview["path"] for preview in record["previews"])
    for directory_name in ("assets", "metadata", "previews"):
        directory = output / directory_name
        if not directory.is_dir():
            continue
        for path in directory.iterdir():
            if path.is_file() and path.relative_to(output).as_posix() not in referenced_paths:
                path.unlink()
        if directory_name == "previews" and not any(directory.iterdir()):
            directory.rmdir()
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="Only resources with verified sprite pixels are cataloged; PNGs preserve source pixels but approximate PS1 blend behavior.",
    )
    parser.add_argument("discs", nargs="+", type=Path, help="one or more retail CUE, BIN, or ISO images")
    parser.add_argument(
        "--output", type=Path, default=Path("extracted-sprites"),
        help="output directory (default: extracted-sprites)",
    )
    parser.add_argument("--no-previews", action="store_true", help="extract raw assets and metadata without generating PNG previews")
    args = parser.parse_args()
    manifest = extract_sprites(
        args.discs,
        args.output,
        previews=not args.no_previews,
    )
    print(
        f"Extracted {manifest['summary']['unique_resources']} unique sprite resources from "
        f"{manifest['summary']['occurrences']} occurrences; "
        f"wrote {manifest['summary']['unique_png_previews']} unique PNG previews."
    )
    print(f"Manifest: {args.output / 'manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
