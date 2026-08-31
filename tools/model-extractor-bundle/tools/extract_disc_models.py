#!/usr/bin/env python3
"""Extract Xenogears 3D models and their dependent resources from retail discs."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
import re
import shutil
import struct
import zlib
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


SCHEMA = "xenogears-disc-models/v1"
FAT_LBA = 0x18
FAT_SECTORS = 0x10
DIRECTORY_LBA = 0x28
DIRECTORY_COUNT = 64
USER_SECTOR = 2048
MAX_RESOURCE_SIZE = 64 * 1024 * 1024
MAX_MEMBERS = 4096
MECHA_ANIMATION_FPS = 19.3

ATTRIBUTE_SIZES = (4, 8, 4, 8, 4, 8, 4, 8, 4, 12, 4, 12, 4, 12, 4, 12, 4)
PACKET_SIZES = (0x14, 0x20, 0x1C, 0x28, 0x14, 0x20, 0x1C, 0x28,
                0x18, 0x28, 0x24, 0x34, 0x18, 0x28, 0x24, 0x34, 0x20)
GP0_BASES = (0x20, 0x24, 0x30, 0x34, 0x20, 0x24, 0x30, 0x34,
             0x28, 0x2C, 0x38, 0x3C, 0x28, 0x2C, 0x38, 0x3C, 0x24)
TEXTURED_FAMILIES = frozenset({1, 3, 5, 7, 9, 11, 13, 15, 16})
METADATA_FAMILIES = frozenset({1, 3, 5, 7, 9, 11, 13, 15})
GOURAUD_FAMILIES = frozenset({2, 3, 6, 7, 10, 11, 14, 15})
RELIGHTABLE_FAMILIES = frozenset({0, 1, 2, 3, 8, 9})
GEAR_VM_WORDS = {
    **{opcode: 2 for opcode in (
        0x01, 0x04, 0x11, 0x12, 0x14, 0x15, 0x18, 0x22, 0x23, 0x28,
        0x29, 0x2E, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x37, 0x38,
        0x39, 0x3B, 0x3C, 0x54, 0x55, 0x56, 0x5C, 0x5D, 0x5E, 0x5F,
        0x61, 0x63, 0x64, 0x6B, 0x6E, 0x70,
    )},
    0x13: 3, 0x36: 3,
    **{opcode: 4 for opcode in (
        0x2C, 0x2D, 0x40, 0x41, 0x44, 0x45, 0x46, 0x47, 0x49,
        0x4B, 0x4C, 0x4D, 0x4E, 0x50, 0x65, 0x66,
    )},
    0x25: 5, 0x52: 5, 0x62: 5, 0x67: 5, 0x1A: 7, 0x1D: 10,
    0x1B: 16,
}


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
            raise ValueError("negative disc extent")
        read_size = ((size + USER_SECTOR - 1) // USER_SECTOR) * USER_SECTOR if padded else size
        output = bytearray()
        with self.data_path.open("rb") as source:
            for index in range((read_size + USER_SECTOR - 1) // USER_SECTOR):
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


@dataclass
class Primitive:
    family: int
    indices: tuple[int, int, int, int]
    command: int
    color: tuple[int, int, int, int]
    uvs: tuple[tuple[int, int], ...]
    tpage: int | None
    clut: int | None
    texture_state_status: str
    metadata: tuple[dict, ...]


@dataclass
class ModelPart:
    flags: int
    vertices: tuple[tuple[int, int, int, int], ...]
    normals: tuple[tuple[int, int, int, int], ...]
    bounds_min: tuple[int, int, int, int]
    bounds_max: tuple[int, int, int, int]
    primitives: tuple[Primitive, ...]
    deformation_offset: int
    packet_buffer_size: int


@dataclass
class ModelArchive:
    raw: bytes
    flags: int
    parts: tuple[ModelPart, ...]
    metadata_command_count: int


@dataclass(frozen=True)
class TimImage:
    raw: bytes
    mode: int
    image_x: int
    image_y: int
    width_words: int
    width: int
    height: int
    image_data: bytes
    clut_x: int | None
    clut_y: int | None
    palettes: tuple[tuple[int, ...], ...]
    row_trailing_bytes: int

    @property
    def bpp(self) -> int:
        return (4, 8, 16, 24)[self.mode]


@dataclass(frozen=True)
class TextureUpload:
    tag: int
    stored_x: int
    stored_y: int
    relative_x: int
    relative_y: int
    width_words: int
    height: int
    data: bytes


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def s16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<h", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def s32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


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
        raise ValueError(f"{path}: unsupported CUE layout")
    binary = (path.parent / file_match.group(1)).resolve()
    mode = track_match.group(1).upper()
    if mode.endswith("/2048"):
        layout = DiscLayout(path.resolve(), binary, 2048, 0)
    else:
        layout = DiscLayout(path.resolve(), binary, 2352, 24 if mode.startswith("MODE2") else 16)
    if not binary.is_file() or binary.stat().st_size % layout.sector_size:
        raise ValueError(f"{path}: missing or unaligned CUE data file")
    return layout


def open_disc(path: Path) -> DiscLayout:
    path = path.resolve()
    if path.suffix.lower() == ".cue":
        return _cue_layout(path)
    if not path.is_file():
        raise ValueError(f"disc image does not exist: {path}")
    size = path.stat().st_size
    candidates = []
    if size % 2352 == 0:
        candidates.extend((DiscLayout(path, path, 2352, 24), DiscLayout(path, path, 2352, 16)))
    if size % 2048 == 0:
        candidates.append(DiscLayout(path, path, 2048, 0))
    for candidate in candidates:
        try:
            if candidate.read_user_data(16, 7)[1:6] == b"CD001":
                return candidate
        except ValueError:
            continue
    raise ValueError(f"{path}: cannot identify ISO or MODE1/2 BIN layout")


def parse_fat(data: bytes, sector_count: int) -> list[FatEntry]:
    entries = []
    for offset in range(0, len(data) - 6, 7):
        lba = int.from_bytes(data[offset:offset + 3], "little")
        size = s32(data, offset + 3)
        if lba == 0xFFFFFF and size == 0:
            return entries
        if lba == 0xFFFFFF:
            raise ValueError("malformed FAT sentinel")
        if size > 0:
            end = lba + (size + USER_SECTOR - 1) // USER_SECTOR
            if lba >= sector_count or end > sector_count:
                raise ValueError("FAT extent leaves disc")
        entries.append(FatEntry(lba, size))
    raise ValueError("FAT sentinel not found")


def parse_directory_table(data: bytes) -> list[int]:
    if len(data) < DIRECTORY_COUNT * 2:
        raise ValueError("directory table is truncated")
    return list(struct.unpack_from(f"<{DIRECTORY_COUNT}H", data))


def map_physical_routes(directory: list[int], entry_count: int) -> dict[int, list[dict]]:
    starts: dict[int, list[int]] = {}
    for directory_id, encoded in enumerate(directory):
        if encoded and encoded - 1 < entry_count:
            starts.setdefault(encoded - 1, []).append(directory_id)
    ordered = sorted(starts)
    routes = {}
    for position, start in enumerate(ordered):
        end = ordered[position + 1] if position + 1 < len(ordered) else entry_count
        for fat_index in range(start, end):
            routes[fat_index] = [
                {"directory": f"0x{directory_id:02X}", "file_id": f"0x{fat_index - start + 1:03X}"}
                for directory_id in starts[start]
            ]
    return routes


def route_ids(routes: list[dict]) -> set[tuple[int, int]]:
    return {(int(route["directory"], 16), int(route["file_id"], 16)) for route in routes}


def lzss_decompress(data: bytes, offset: int = 0) -> tuple[bytes, int]:
    if offset < 0 or offset + 4 > len(data):
        raise ValueError("missing LZSS header")
    target = u32(data, offset)
    if target == 0 or target > MAX_RESOURCE_SIZE:
        raise ValueError("invalid LZSS expanded size")
    cursor = offset + 4
    output = bytearray()
    while len(output) < target:
        if cursor >= len(data):
            raise ValueError("truncated LZSS control")
        control = data[cursor]
        cursor += 1
        for bit in range(8):
            if control & (1 << bit):
                if cursor + 2 > len(data):
                    raise ValueError("truncated LZSS reference")
                low, high = data[cursor:cursor + 2]
                cursor += 2
                distance = low | ((high & 0x0F) << 8)
                length = (high >> 4) + 3
                if distance == 0 or distance > len(output):
                    raise ValueError("invalid LZSS distance")
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


def offset_bundle(data: bytes, offset: int = 0, boundary: int | None = None) -> list[tuple[int, int]]:
    boundary = len(data) if boundary is None else boundary
    if offset < 0 or offset + 4 > boundary:
        raise ValueError("offset bundle header is missing")
    count = s32(data, offset)
    if count <= 0:
        return []
    if count > MAX_MEMBERS or offset + 4 + count * 4 > boundary:
        raise ValueError("offset bundle count is invalid")
    relative = [u32(data, offset + 4 + index * 4) for index in range(count)]
    terminal_layout = relative[0] == count * 4 + 8
    if terminal_layout:
        if offset + 8 + count * 4 > boundary:
            raise ValueError("terminal offset is truncated")
        terminal = u32(data, offset + 4 + count * 4)
        starts = relative
        ends = relative[1:] + [terminal]
    else:
        table_end = count * 4 + 4
        starts = relative
        ends = relative[1:] + [boundary - offset]
        if starts[0] < table_end:
            raise ValueError("first member overlaps offset table")
    if any(left > right or left < 0 or right > boundary - offset for left, right in zip(starts, ends)):
        raise ValueError("offset bundle member is invalid")
    return [(offset + left, offset + right) for left, right in zip(starts, ends)]


def nullable_pointer_split(data: bytes) -> list[tuple[int, int] | None]:
    if len(data) < 8:
        raise ValueError("nullable pointer split is truncated")
    count = s32(data, 0)
    if count <= 0 or count > MAX_MEMBERS or 4 + count * 4 > len(data):
        raise ValueError("nullable pointer split count is invalid")
    starts = [u32(data, 4 + index * 4) for index in range(count)]
    table_end = 4 + count * 4
    if starts[0] == table_end + 4:
        terminal = u32(data, table_end)
    else:
        terminal = len(data)
    if terminal > len(data):
        raise ValueError("nullable pointer split terminal is invalid")
    spans = []
    for index, start in enumerate(starts):
        if start == 0:
            spans.append(None)
            continue
        candidates = [candidate for candidate in starts[index + 1:]
                      if candidate != 0 and candidate >= start]
        end = candidates[0] if candidates else terminal
        if start < table_end or start > end or end > len(data):
            raise ValueError("nullable pointer split member is invalid")
        spans.append((start, end))
    return spans


def _parse_tim_block(data: bytes, offset: int, boundary: int) -> tuple[dict, bytes, int]:
    if offset + 12 > boundary:
        raise ValueError("truncated TIM block header")
    length, x, y, width_words, height = struct.unpack_from("<Ihhhh", data, offset)
    expected = 12 + width_words * height * 2
    if length != expected or width_words <= 0 or height <= 0 or offset + length > boundary:
        raise ValueError("invalid TIM block")
    if x < 0 or y < 0 or x + width_words > 1024 or y + height > 512:
        raise ValueError("TIM block lies outside PS1 VRAM")
    return {
        "x": x, "y": y, "width_words": width_words, "height": height,
    }, data[offset + 12:offset + length], offset + length


def parse_tim(data: bytes, offset: int = 0, boundary: int | None = None) -> TimImage:
    boundary = len(data) if boundary is None else boundary
    if offset < 0 or offset + 8 > boundary or u32(data, offset) != 0x10:
        raise ValueError("missing TIM header")
    flags = u32(data, offset + 4)
    if flags & ~0x0F or flags & 7 > 3:
        raise ValueError("unsupported TIM flags")
    mode = flags & 7
    has_clut = bool(flags & 8)
    if has_clut != (mode in {0, 1}):
        raise ValueError("TIM CLUT presence does not match indexed mode")
    cursor = offset + 8
    palettes: tuple[tuple[int, ...], ...] = ()
    clut_x = clut_y = None
    if has_clut:
        clut, clut_data, cursor = _parse_tim_block(data, cursor, boundary)
        palette_size = 16 if mode == 0 else 256
        if clut["width_words"] != palette_size:
            raise ValueError("unsupported TIM CLUT layout")
        words = struct.unpack(f"<{len(clut_data) // 2}H", clut_data)
        palettes = tuple(
            tuple(words[start:start + palette_size])
            for start in range(0, len(words), palette_size)
        )
        clut_x, clut_y = clut["x"], clut["y"]
    image, image_data, end = _parse_tim_block(data, cursor, boundary)
    row_bytes = image["width_words"] * 2
    width = (image["width_words"] * 4, image["width_words"] * 2,
             image["width_words"], row_bytes // 3)[mode]
    if width <= 0:
        raise ValueError("TIM image has zero display width")
    return TimImage(
        data[offset:end], mode, image["x"], image["y"], image["width_words"],
        width, image["height"], image_data, clut_x, clut_y, palettes,
        row_bytes % 3 if mode == 3 else 0,
    )


def tim_bundle_members(data: bytes) -> list[tuple[int, int, TimImage, int]]:
    spans = offset_bundle(data)
    members = []
    for member_index, (start, end) in enumerate(spans):
        tim = parse_tim(data, start, end)
        trailer_size = end - start - len(tim.raw)
        if trailer_size > 8:
            raise ValueError("TIM bundle member has an excessive trailer")
        members.append((member_index, start, tim, trailer_size))
    return members


def parse_packed_image_set(data: bytes) -> list[tuple[int, int, TextureUpload, int]]:
    if len(data) < 4:
        raise ValueError("packed image set is truncated")
    count = s32(data, 0)
    if count <= 0:
        return []
    if count > MAX_MEMBERS or 4 + count * 4 > len(data):
        raise ValueError("packed image count is invalid")
    starts = [u32(data, 4 + index * 4) for index in range(count)]
    table_end = 4 + count * 4
    if starts[0] != table_end or any(left >= right for left, right in zip(starts, starts[1:])):
        raise ValueError("packed image offsets are invalid")
    records = []
    for record_index, start in enumerate(starts):
        boundary = starts[record_index + 1] if record_index + 1 < count else len(data)
        if start + 16 > boundary:
            raise ValueError("packed image record header is truncated")
        tag, stored_x, stored_y, relative_x, relative_y, width_words, height = struct.unpack_from(
            "<Ihhhhhh", data, start,
        )
        if tag not in {0x1100, 0x1101} or width_words <= 0 or height <= 0:
            raise ValueError("packed image record is invalid")
        end = start + 16 + width_words * height * 2
        if end > boundary:
            raise ValueError("packed image pixels leave the record")
        if record_index + 1 < count and end != boundary:
            raise ValueError("packed image records are not sequential")
        records.append((record_index, start, TextureUpload(
            tag, stored_x, stored_y, relative_x, relative_y, width_words, height,
            data[start + 16:end],
        ), boundary - end))
    return records


def parse_sector_graphics(data: bytes) -> list[tuple[int, int, TextureUpload]]:
    descriptors = []
    offset = 0
    expected_count = None
    while offset < len(data):
        if offset + USER_SECTOR > len(data):
            raise ValueError("sector graphics descriptor is truncated")
        values = struct.unpack_from("<14H", data, offset)
        (tag, reserved_02, stored_x, stored_y, relative_x, relative_y,
         width_words, rows_per_sector, unit_sectors, chunk_count,
         descriptor_count, reserved_16, chunk_copy, reserved_1a) = values
        if (
            tag not in {0x1200, 0x1201} or reserved_02 != 0 or width_words == 0
            or rows_per_sector != 1024 // width_words or unit_sectors != 1
            or chunk_count == 0 or descriptor_count == 0 or reserved_16 != 0
            or chunk_copy != chunk_count or reserved_1a != 0
            or offset + (1 + chunk_count) * USER_SECTOR > len(data)
        ):
            raise ValueError("sector graphics descriptor is invalid")
        if expected_count is None:
            expected_count = descriptor_count
        elif descriptor_count != expected_count:
            raise ValueError("sector graphics descriptor count is inconsistent")
        heights = struct.unpack_from(f"<{chunk_count}H", data, offset + 28)
        pixels = bytearray()
        for chunk_index, chunk_height in enumerate(heights):
            byte_count = width_words * chunk_height * 2
            if chunk_height == 0 or byte_count > USER_SECTOR:
                raise ValueError("sector graphics chunk dimensions are invalid")
            chunk_at = offset + (chunk_index + 1) * USER_SECTOR
            pixels.extend(data[chunk_at:chunk_at + byte_count])
        x, y = stored_x + relative_x, stored_y + relative_y
        height = sum(heights)
        if x + width_words > 1024 or y + height > 512:
            raise ValueError("sector graphics upload lies outside PS1 VRAM")
        descriptors.append((len(descriptors), offset, TextureUpload(
            tag, stored_x, stored_y, relative_x, relative_y,
            width_words, height, bytes(pixels),
        )))
        offset += (1 + chunk_count) * USER_SECTOR
    if expected_count != len(descriptors):
        raise ValueError("sector graphics file does not match its descriptor count")
    return descriptors


def _rgb555(color: int, *, transparent_zero: bool) -> bytes:
    red, green, blue = color & 31, color >> 5 & 31, color >> 10 & 31
    alpha = 0 if transparent_zero and color == 0 else 255
    return bytes(((red << 3) | (red >> 2), (green << 3) | (green >> 2),
                  (blue << 3) | (blue >> 2), alpha))


def decode_tim(tim: TimImage, palette_variant: int = 0) -> bytes:
    output = bytearray()
    if tim.mode in {0, 1}:
        if not 0 <= palette_variant < len(tim.palettes):
            raise ValueError("TIM palette variant is outside the CLUT")
        palette = [_rgb555(color, transparent_zero=True) for color in tim.palettes[palette_variant]]
        for value in tim.image_data:
            output.extend(palette[value & 15])
            if tim.mode == 0:
                output.extend(palette[value >> 4])
    elif tim.mode == 2:
        for offset in range(0, len(tim.image_data), 2):
            output.extend(_rgb555(u16(tim.image_data, offset), transparent_zero=False))
    else:
        row_bytes = tim.width_words * 2
        for row in range(tim.height):
            row_data = tim.image_data[row * row_bytes:(row + 1) * row_bytes]
            for offset in range(0, tim.width * 3, 3):
                output.extend(row_data[offset:offset + 3])
                output.append(255)
    if len(output) != tim.width * tim.height * 4:
        raise ValueError("decoded TIM dimensions do not match its payload")
    return bytes(output)


def encode_png_rgba(width: int, height: int, pixels: bytes,
                    compression_level: int = 6) -> bytes:
    if width <= 0 or height <= 0 or len(pixels) != width * height * 4:
        raise ValueError("invalid RGBA raster")
    if not 0 <= compression_level <= 9:
        raise ValueError("invalid PNG compression level")
    scanlines = b"".join(
        b"\0" + pixels[row * width * 4:(row + 1) * width * 4]
        for row in range(height)
    )

    def chunk(kind: bytes, payload: bytes) -> bytes:
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(
        ">IIBBBBB", width, height, 8, 6, 0, 0, 0,
    )) + chunk(b"IDAT", zlib.compress(scanlines, compression_level)) + chunk(b"IEND", b""))


def vector(data: bytes, offset: int) -> tuple[int, int, int, int]:
    if offset < 0 or offset + 8 > len(data):
        raise ValueError("vector leaves model archive")
    return struct.unpack_from("<hhhh", data, offset)


def _pointer_range(data: bytes, offset: int, size: int, label: str) -> None:
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError(f"{label} leaves model archive")


def parse_model_archive(data: bytes) -> ModelArchive:
    if len(data) < 0x48:
        raise ValueError("model archive is too small")
    part_count, flags, reserved_08, reserved_0c = struct.unpack_from("<IIII", data)
    if not 1 <= part_count <= MAX_MEMBERS or reserved_08 != 0 or reserved_0c != 0 or flags & ~3:
        raise ValueError("invalid model archive header")
    _pointer_range(data, 0x10, part_count * 0x38, "model part table")
    parts = []
    tpage: int | None = None
    clut: int | None = None
    metadata_total = 0
    for part_index in range(part_count):
        at = 0x10 + part_index * 0x38
        part_flags, vertex_count, primitive_count, group_count = struct.unpack_from("<HHHH", data, at)
        vertices_at, normals_at, groups_at, display_at = struct.unpack_from("<IIII", data, at + 8)
        deformation_at = u32(data, at + 0x1C)
        if part_flags not in {0, 0x10}:
            raise ValueError("invalid model part header")
        _pointer_range(data, vertices_at, vertex_count * 8, "vertex array")
        _pointer_range(data, normals_at, vertex_count * 8, "normal array")
        vertices = tuple(vector(data, vertices_at + index * 8) for index in range(vertex_count))
        normals = tuple(vector(data, normals_at + index * 8) for index in range(vertex_count))
        bounds_min = vector(data, at + 0x20)
        bounds_max = vector(data, at + 0x28)
        packet_buffer_size = u32(data, at + 0x34)
        group_cursor = groups_at
        display_cursor = display_at
        packet_bytes = parsed_primitives = 0
        primitives = []
        for _ in range(group_count):
            _pointer_range(data, group_cursor, 4, "mesh group")
            family, marker, count = struct.unpack_from("<BBH", data, group_cursor)
            if family > 0x10 or marker != 0x77 or count == 0:
                raise ValueError("invalid mesh group")
            descriptor_at = group_cursor + 4
            _pointer_range(data, descriptor_at, count * 8, "mesh topology")
            for primitive_index in range(count):
                indices = struct.unpack_from("<HHHH", data, descriptor_at + primitive_index * 8)
                used_count = 3 if family < 8 or family == 16 else 4
                if any(index >= vertex_count for index in indices[:used_count]):
                    raise ValueError("model topology index is out of range")
                controls = []
                if family in METADATA_FAMILIES:
                    for _ in range(64):
                        _pointer_range(data, display_cursor, 4, "model metadata")
                        value, reserved, command = struct.unpack_from("<HBB", data, display_cursor)
                        if command not in {0xC4, 0xC8}:
                            break
                        if reserved != 0:
                            raise ValueError("model metadata reserved byte is nonzero")
                        controls.append({"command": f"0x{command:02X}", "value": f"0x{value:04X}"})
                        if command == 0xC4:
                            tpage = value
                        else:
                            clut = value
                        display_cursor += 4
                        metadata_total += 1
                    else:
                        raise ValueError("excessive model metadata")
                attribute_size = ATTRIBUTE_SIZES[family]
                _pointer_range(data, display_cursor, attribute_size, "model attribute")
                attribute = data[display_cursor:display_cursor + attribute_size]
                command = attribute[3]
                if family != 16 and command & 0xFC != GP0_BASES[family]:
                    raise ValueError("model GP0 command does not match primitive family")
                if family in TEXTURED_FAMILIES and family != 16:
                    if used_count == 3:
                        uvs = ((attribute[4], attribute[5]), (attribute[6], attribute[7]),
                               (attribute[0], attribute[1]))
                    else:
                        uvs = tuple((attribute[4 + index * 2], attribute[5 + index * 2]) for index in range(4))
                else:
                    uvs = ()
                if attribute_size in {4, 12}:
                    color = (attribute[0], attribute[1], attribute[2], 255)
                else:
                    color = (255, 255, 255, 255)
                if family == 16:
                    texture_state_status = "runtime_generated"
                elif family not in TEXTURED_FAMILIES:
                    texture_state_status = "not_applicable"
                elif tpage is None:
                    texture_state_status = "inherited_tpage_unknown"
                elif (tpage >> 7) & 3 in {0, 1} and clut is None:
                    texture_state_status = "inherited_clut_unknown"
                else:
                    texture_state_status = "serialized_state_known"
                primitives.append(Primitive(
                    family, indices, command, color, uvs, tpage, clut,
                    texture_state_status, tuple(controls),
                ))
                display_cursor += attribute_size
                packet_bytes += PACKET_SIZES[family]
                parsed_primitives += 1
            group_cursor = descriptor_at + count * 8
        if parsed_primitives != primitive_count or packet_bytes != packet_buffer_size:
            raise ValueError("model primitive or packet census is inconsistent")
        parts.append(ModelPart(
            part_flags, vertices, normals, bounds_min, bounds_max,
            tuple(primitives), deformation_at, packet_buffer_size,
        ))
    return ModelArchive(data, flags, tuple(parts), metadata_total)


def parse_bone_links(data: bytes, part_count: int) -> list[dict]:
    bones = []
    for offset in range(0, len(data) - 3, 4):
        model_part, parent = struct.unpack_from("<HH", data, offset)
        if model_part != 0xFFFF and model_part >= part_count:
            break
        if parent != 0xFFFF and parent >= len(bones):
            raise ValueError("bone parent is not parent-first")
        bones.append({
            "index": len(bones),
            "model_part_index": None if model_part == 0xFFFF else model_part,
            "parent_index": None if parent == 0xFFFF else parent,
        })
    if not bones:
        raise ValueError("bone hierarchy is empty")
    return bones


def _mecha_animation_layout(data: bytes) -> tuple[list[tuple[int, int] | None], list[bytes]]:
    script_table = u32(data, 4)
    if script_table + 8 > len(data):
        raise ValueError("mecha animation script table is invalid")
    script_count = u32(data, script_table)
    sequence_table = script_table + u32(data, script_table + 4)
    if (script_count > MAX_MEMBERS or sequence_table + 4 > len(data)
            or sequence_table < script_table):
        raise ValueError("mecha animation sequence table is invalid")
    sequence_count = u32(data, sequence_table)
    pointer_end = sequence_table + 4 + sequence_count * 4
    if sequence_count > MAX_MEMBERS or pointer_end + 4 > len(data):
        raise ValueError("mecha animation sequence pointers are truncated")
    relative = [u32(data, sequence_table + 4 + index * 4)
                for index in range(sequence_count)]
    terminal = u32(data, pointer_end)
    if sequence_table + terminal > len(data):
        raise ValueError("mecha animation sequence terminal is invalid")
    spans = []
    for index, start in enumerate(relative):
        if start == 0:
            spans.append(None)
            continue
        following = [candidate for candidate in relative[index + 1:]
                     if candidate > start]
        end = following[0] if following else terminal
        if start < 4 + sequence_count * 4 or start > end or sequence_table + end > len(data):
            spans.append(None)
        else:
            spans.append((sequence_table + start, sequence_table + end))

    scripts = []
    script_pointer_end = script_table + 8 + script_count * 4
    script_block_end = min(u32(data, 8), len(data))
    if script_pointer_end <= len(data):
        starts = [script_table + u32(data, script_table + 8 + index * 4)
                  for index in range(script_count)]
        for index, start in enumerate(starts):
            end = starts[index + 1] if index + 1 < len(starts) else script_block_end
            scripts.append(data[start:end] if script_pointer_end <= start <= end <= len(data) else b"")
    return spans, scripts


def _copy_mecha_pose(pose: list[dict]) -> list[dict]:
    return [{"rotation_psx": list(transform["rotation_psx"]),
             "translation_psx": list(transform["translation_psx"])}
            for transform in pose]


def _decode_mecha_sequence(data: bytes, sequence_index: int, span: tuple[int, int],
                           bone_count: int, bind_pose: list[dict]) -> dict | None:
    start, end = span
    if start + 0x18 > end:
        return None
    header_bones, frame_count = struct.unpack_from("<HH", data, start)
    transform_flags = u16(data, start + 4)
    sequence_flags = u16(data, start + 6)
    rotation_count = u16(data, start + 0x0C)
    translation_count = u16(data, start + 0x0E)
    if (header_bones > MAX_MEMBERS or frame_count == 0 or frame_count > 0x4000
            or rotation_count > header_bones or translation_count > header_bones):
        return None
    used_bones = min(header_bones, bone_count)
    payload = start + 0x18

    if sequence_flags == 1:
        pose = _copy_mecha_pose(bind_pose)
        for bone_index in range(used_bones):
            runtime_bone = bone_index + 1
            if not transform_flags & 1 and bone_index < rotation_count:
                if payload + 6 > end:
                    return None
                pose[runtime_bone]["rotation_psx"] = list(struct.unpack_from("<hhh", data, payload))
                payload += 6
            if not transform_flags & 2 and bone_index < translation_count:
                if payload + 6 > end:
                    return None
                pose[runtime_bone]["translation_psx"] = list(struct.unpack_from("<hhh", data, payload))
                payload += 6
        return {
            "sequence_index": sequence_index, "frame_count": frame_count,
            "static": True, "frames": [pose], "header_bones": header_bones,
            "rotation_tracks": rotation_count, "translation_tracks": translation_count,
            "initial_rotation_targets": (
                set() if transform_flags & 1 else set(range(1, min(rotation_count, used_bones) + 1))
            ),
            "initial_translation_targets": (
                set() if transform_flags & 2 else set(range(1, min(translation_count, used_bones) + 1))
            ),
            "rotation_stream_targets": set(), "translation_stream_targets": set(),
        }

    track_count = rotation_count + 1
    track_table = payload
    payload += track_count * 6
    if payload > end:
        return None
    initial_pose = _copy_mecha_pose(bind_pose)
    for bone_index in range(used_bones):
        runtime_bone = bone_index + 1
        if not transform_flags & 1 and bone_index < rotation_count:
            if payload + 6 > end:
                return None
            initial_pose[runtime_bone]["rotation_psx"] = list(struct.unpack_from("<hhh", data, payload))
            payload += 6
        if not transform_flags & 2 and bone_index < translation_count:
            if payload + 6 > end:
                return None
            initial_pose[runtime_bone]["translation_psx"] = list(struct.unpack_from("<hhh", data, payload))
            payload += 6
    frames = [_copy_mecha_pose(initial_pose) for _ in range(frame_count + 1)]
    delta_base = payload
    rotation_stream_targets = set()
    translation_stream_targets = set()

    for track_index in range(track_count):
        target = track_index
        if target > used_bones:
            continue
        at = track_table + track_index * 6
        rotation_offset, translation_offset, rotation_flags, translation_flags = struct.unpack_from(
            "<HHBB", data, at,
        )
        rotation_format = rotation_flags & 0x0F
        translation_format = translation_flags & 0x0F
        rotation_at = delta_base + rotation_offset
        translation_at = delta_base + translation_offset
        has_rotation = (rotation_offset != 0xFFFF
                        and rotation_format in {0, 1} and rotation_at < end)
        has_translation = (translation_offset != 0xFFFF
                           and translation_format in {0, 1, 2} and translation_at < end)
        if has_rotation:
            rotation_stream_targets.add(target)
        if has_translation:
            translation_stream_targets.add(target)
        rotation = list(initial_pose[target]["rotation_psx"])
        translation = list(initial_pose[target]["translation_psx"])
        skip_rotation = tuple(bool(rotation_flags & flag) for flag in (0x10, 0x20, 0x40))
        skip_translation = tuple(bool(translation_flags & flag) for flag in (0x10, 0x20, 0x40))

        for frame_index in range(1, frame_count + 1):
            if has_rotation:
                for axis, skip in enumerate(skip_rotation):
                    if skip:
                        continue
                    if rotation_format == 0:
                        if rotation_at + 2 > end:
                            has_rotation = False
                            break
                        rotation[axis] += s16(data, rotation_at)
                        rotation_at += 2
                    else:
                        if rotation_at >= end:
                            has_rotation = False
                            break
                        delta = struct.unpack_from("<b", data, rotation_at)[0]
                        rotation_at += 1
                        if delta == -128:
                            if rotation_at + 2 > end:
                                has_rotation = False
                                break
                            rotation[axis] = s16(data, rotation_at)
                            rotation_at += 2
                        else:
                            rotation[axis] += delta
                if has_rotation:
                    frames[frame_index][target]["rotation_psx"] = list(rotation)
            if has_translation:
                if translation_format == 2:
                    local_delta = [0, 0, 0]
                    for axis, skip in enumerate(skip_translation):
                        if skip:
                            continue
                        if translation_at + 2 > end:
                            has_translation = False
                            break
                        local_delta[axis] = s16(data, translation_at)
                        translation_at += 2
                    if has_translation:
                        rotated_delta = _rotate_mecha_vector(rotation, local_delta)
                        translation = [translation[axis] + rotated_delta[axis]
                                       for axis in range(3)]
                else:
                    for axis, skip in enumerate(skip_translation):
                        if skip:
                            continue
                        if translation_format == 0:
                            if translation_at + 2 > end:
                                has_translation = False
                                break
                            translation[axis] = s16(data, translation_at)
                            translation_at += 2
                        else:
                            if translation_at >= end:
                                has_translation = False
                                break
                            delta = struct.unpack_from("<b", data, translation_at)[0]
                            translation_at += 1
                            if delta == -128:
                                if translation_at + 2 > end:
                                    has_translation = False
                                    break
                                translation[axis] = s16(data, translation_at)
                                translation_at += 2
                            else:
                                translation[axis] += delta
                if has_translation:
                    frames[frame_index][target]["translation_psx"] = list(translation)
    return {
        "sequence_index": sequence_index, "frame_count": frame_count,
        "static": False, "frames": frames[:frame_count], "loop_frames": frames,
        "header_bones": header_bones,
        "rotation_tracks": rotation_count, "translation_tracks": translation_count,
        "initial_rotation_targets": (
            set() if transform_flags & 1 else set(range(1, min(rotation_count, used_bones) + 1))
        ),
        "initial_translation_targets": (
            set() if transform_flags & 2 else set(range(1, min(translation_count, used_bones) + 1))
        ),
        "rotation_stream_targets": rotation_stream_targets,
        "translation_stream_targets": translation_stream_targets,
    }


def parse_mecha_animation_sequences(data: bytes, bone_count: int) -> list[dict]:
    try:
        spans, _scripts = _mecha_animation_layout(data)
    except (ValueError, struct.error, IndexError):
        return []
    bind_pose = [{"rotation_psx": [0, 0, 0], "translation_psx": [0, 0, 0]}
                 for _ in range(bone_count + 1)]
    if spans and spans[0] is not None:
        decoded_bind = _decode_mecha_sequence(data, 0, spans[0], bone_count, bind_pose)
        if decoded_bind is not None:
            bind_pose = _copy_mecha_pose(decoded_bind["frames"][0])
    sequences = []
    for sequence_index, span in enumerate(spans):
        if span is None:
            continue
        decoded = _decode_mecha_sequence(data, sequence_index, span, bone_count, bind_pose)
        if decoded is None:
            decoded = {
                "sequence_index": sequence_index, "frame_count": 1,
                "static": True, "frames": [_copy_mecha_pose(bind_pose)],
                "header_bones": bone_count, "rotation_tracks": 0,
                "translation_tracks": 0, "initial_rotation_targets": set(),
                "initial_translation_targets": set(),
                "rotation_stream_targets": set(), "translation_stream_targets": set(),
                "fallback_empty": True,
            }
        sequences.append(decoded)
    return sequences


def parse_mecha_initial_pose(data: bytes, bone_count: int) -> list[dict] | None:
    sequences = parse_mecha_animation_sequences(data, bone_count)
    return _copy_mecha_pose(sequences[0]["frames"][0][1:]) if sequences else None


def _mecha_angle_delta(target: int, current: int) -> int:
    delta = (target - current) & 0xFFF
    return delta - 0x1000 if delta > 0x7FF else delta


def _rotate_mecha_vector(rotation: list[int], vector: list[int]) -> list[int]:
    x, y, z = (value * math.tau / 0x1000 for value in rotation)
    sx, cx = math.sin(x), math.cos(x)
    sy, cy = math.sin(y), math.cos(y)
    sz, cz = math.sin(z), math.cos(z)
    matrix = (
        (cz * cy, -sz * cy, sy),
        (sz * cx + cz * sy * sx, cz * cx - sz * sy * sx, -cy * sx),
        (-cz * sy * cx + sz * sx, cz * sx + sz * sy * cx, cy * cx),
    )
    return [round(sum(matrix[row][axis] * vector[axis] for axis in range(3)))
            for row in range(3)]


def _mecha_script_word_count(script: bytes, cursor: int, opcode: int) -> int:
    return GEAR_VM_WORDS.get(opcode, 1)


def evaluate_mecha_vm_scripts(data: bytes, sequences: list[dict], bone_count: int,
                              runtime_children: dict[int, list[int]]) -> list[dict]:
    try:
        _spans, scripts = _mecha_animation_layout(data)
    except (ValueError, struct.error, IndexError):
        return []
    by_index = {sequence["sequence_index"]: sequence for sequence in sequences}
    if not by_index:
        return []
    base_sequence = by_index[min(by_index)]
    base_pose = _copy_mecha_pose(base_sequence["frames"][0])
    runtime_count = bone_count + 1
    actions = []

    for script_index, script in enumerate(scripts):
        if not script:
            continue
        pose = _copy_mecha_pose(base_pose)
        scales = [[0x1000, 0x1000, 0x1000] for _ in range(runtime_count)]
        visible = [False] + [True] * bone_count
        active: dict[tuple[int, str], dict] = {}
        unsupported = set()
        angular_velocity = [0, 0, 0]
        angular_acceleration = [0, 0, 0]
        linear_velocity = [0, 0, 0]
        linear_acceleration = [0, 0, 0]
        pc = 0
        wait_frames = None
        ended = False

        def descendants(runtime_index: int) -> list[int]:
            output = [runtime_index]
            for child in runtime_children.get(runtime_index, []):
                output.extend(descendants(child))
            return output

        def set_sequence_pose(sequence: dict) -> None:
            initial = sequence["frames"][0]
            for target in sequence["initial_rotation_targets"]:
                if target < runtime_count:
                    pose[target]["rotation_psx"] = list(initial[target]["rotation_psx"])
            for target in sequence["initial_translation_targets"]:
                if target < runtime_count:
                    pose[target]["translation_psx"] = list(initial[target]["translation_psx"])

        def start_sequence(sequence: dict, group: int, looping: bool) -> None:
            if sequence["static"]:
                active.clear()
                set_sequence_pose(sequence)
                return
            affected = range(min(sequence["rotation_tracks"] + 1, runtime_count))
            for target in affected:
                for path, targets in (
                        ("rotation_psx", sequence["rotation_stream_targets"]),
                        ("translation_psx", sequence["translation_stream_targets"])):
                    key = (target, path)
                    if target not in targets:
                        if target != 0:
                            active.pop(key, None)
                        continue
                    active[key] = {
                        "kind": "sequence", "sequence": sequence, "position": 1,
                        "duration": max(1, sequence["frame_count"] - (0 if looping else 1)),
                        "elapsed": 0, "looping": looping, "group": group,
                    }

        def start_blend(sequence: dict, group: int, looping: bool,
                        length: int, interpolation_type: int) -> None:
            initial = sequence["frames"][0]
            length = max(1, length)
            for target in sequence["initial_rotation_targets"]:
                if target >= runtime_count:
                    continue
                start_values = list(pose[target]["rotation_psx"])
                target_values = list(initial[target]["rotation_psx"])
                active[(target, "rotation_psx")] = {
                    "kind": "blend", "start": start_values,
                    "delta": [_mecha_angle_delta(target_values[axis], start_values[axis])
                              for axis in range(3)],
                    "target": target_values, "duration": length, "elapsed": 0,
                    "mode": interpolation_type & 1, "looping": looping, "group": group,
                }
            for target in sequence["initial_translation_targets"]:
                if target >= runtime_count:
                    continue
                start_values = list(pose[target]["translation_psx"])
                target_values = list(initial[target]["translation_psx"])
                active[(target, "translation_psx")] = {
                    "kind": "blend", "start": start_values,
                    "delta": [target_values[axis] - start_values[axis] for axis in range(3)],
                    "target": target_values, "duration": length, "elapsed": 0,
                    "mode": interpolation_type & 1, "looping": looping, "group": group,
                }

        def update_tracks() -> set[int]:
            completed = set()
            for axis in range(3):
                angular_velocity[axis] += angular_acceleration[axis]
                pose[0]["rotation_psx"][axis] += angular_velocity[axis] >> 3
                linear_velocity[axis] += linear_acceleration[axis]
            scaled_velocity = [linear_velocity[axis] * scales[0][axis] // 0x1000
                               for axis in range(3)]
            world_velocity = _rotate_mecha_vector(pose[0]["rotation_psx"], scaled_velocity)
            for axis in range(3):
                pose[0]["translation_psx"][axis] += world_velocity[axis]
            for key, track in list(active.items()):
                target, path = key
                if track["kind"] == "sequence":
                    source_frames = track["sequence"].get("loop_frames",
                                                           track["sequence"]["frames"])
                    position = min(track["position"], len(source_frames) - 1)
                    pose[target][path] = list(source_frames[position][target][path])
                    track["position"] += 1
                else:
                    ratio = track["elapsed"] + 1
                    if track["mode"] == 0:
                        pose[target][path] = [
                            track["start"][axis]
                            + int(track["delta"][axis] * ratio / track["duration"])
                            for axis in range(3)
                        ]
                    else:
                        current = pose[target][path]
                        pose[target][path] = [
                            current[axis]
                            + int((track["target"][axis] - current[axis]) / track["duration"])
                            for axis in range(3)
                        ]
                track["elapsed"] += 1
                if track["elapsed"] >= track["duration"]:
                    completed.add(track["group"])
                    if track["looping"]:
                        track["elapsed"] = 0
                        track["position"] = 1
                    else:
                        active.pop(key, None)
            return completed

        def execute(completed_groups: set[int], initial: bool) -> bool:
            nonlocal pc, wait_frames, ended
            if wait_frames is not None:
                if initial:
                    return False
                wait_frames -= 1
                if wait_frames > 0:
                    return False
                wait_frames = None
                pc += 4
            for _ in range(256):
                if pc + 2 > len(script):
                    ended = True
                    return True
                word = u16(script, pc)
                opcode, high = word & 0xFF, word >> 8
                words = _mecha_script_word_count(script, pc, opcode)
                size = words * 2
                if pc + size > len(script):
                    ended = True
                    unsupported.add(opcode)
                    return True
                if opcode in {0x00, 0x77}:
                    ended = True
                    return True
                if opcode == 0x01:
                    wait_frames = max(1, u16(script, pc + 2))
                    return False
                if opcode in {0x02, 0x03, 0x28, 0x29, 0x2F, 0x6E}:
                    unsupported.add(opcode)
                    ended = True
                    return True
                if opcode == 0x08:
                    active.clear()
                elif opcode == 0x09:
                    active_keys = [key for key, track in active.items()
                                   if track["group"] != high]
                    for key in active_keys:
                        active.pop(key, None)
                elif opcode == 0x0A:
                    for path in ("rotation_psx", "translation_psx", "scale_psx"):
                        active.pop((high, path), None)
                elif opcode == 0x10:
                    sequence = by_index.get(high)
                    if sequence is not None:
                        set_sequence_pose(sequence)
                    else:
                        unsupported.add(opcode)
                elif opcode == 0x11:
                    sequence = by_index.get(high)
                    parameter = u16(script, pc + 2)
                    if sequence is not None:
                        start_sequence(sequence, parameter & 0xFF, bool(parameter >> 8))
                    else:
                        unsupported.add(opcode)
                elif opcode == 0x13:
                    parameter = u16(script, pc + 2)
                    timing = u16(script, pc + 4)
                    sequence = by_index.get(parameter & 0xFF)
                    if sequence is not None:
                        start_blend(sequence, parameter >> 8, bool(timing & 0xFF),
                                    timing >> 8, high)
                    else:
                        unsupported.add(opcode)
                elif opcode == 0x21:
                    if high not in completed_groups and any(
                            track["group"] == high for track in active.values()):
                        return False
                elif opcode == 0x32:
                    branch = s16(script, pc + 2)
                    if branch == 0 or not 0 <= pc + branch < len(script):
                        unsupported.add(opcode)
                        ended = True
                        return True
                    pc += branch
                    continue
                elif opcode == 0x23:
                    target = u16(script, pc + 2)
                    targets = descendants(target) if high & 0x80 else [target]
                    for runtime_index in targets:
                        if runtime_index < runtime_count:
                            visible[runtime_index] = bool(high & 1)
                elif opcode == 0x62:
                    target = u16(script, pc + 2)
                    values = [s16(script, pc + 4 + axis * 2) for axis in range(3)]
                    targets = descendants(target) if high & 0x80 else [target]
                    path = ("rotation_psx", "translation_psx", "scale_psx")[min(high & 7, 2)]
                    for runtime_index in targets:
                        if runtime_index >= runtime_count:
                            continue
                        destination = scales[runtime_index] if path == "scale_psx" else pose[runtime_index][path]
                        for axis in range(3):
                            destination[axis] = destination[axis] + values[axis] if high & 0x20 else values[axis]
                elif opcode == 0x0C:
                    for values in (angular_velocity, angular_acceleration,
                                   linear_velocity, linear_acceleration):
                        values[:] = [0, 0, 0]
                elif opcode in {0x40, 0x41}:
                    target_values = [s16(script, pc + 2 + axis * 2) for axis in range(3)]
                    if opcode == 0x41:
                        target_values = [pose[0]["rotation_psx"][axis] + target_values[axis]
                                         for axis in range(3)]
                    if high < 2:
                        pose[0]["rotation_psx"] = target_values
                    else:
                        start_values = list(pose[0]["rotation_psx"])
                        active[(0, "rotation_psx")] = {
                            "kind": "blend", "start": start_values,
                            "delta": [_mecha_angle_delta(target_values[axis], start_values[axis])
                                      for axis in range(3)],
                            "target": target_values, "duration": high, "elapsed": 0,
                            "mode": 0, "looping": False, "group": -2,
                        }
                elif opcode in {0x44, 0x45, 0x46, 0x47, 0x4B, 0x4C, 0x4D, 0x4E}:
                    values = [s16(script, pc + 2 + axis * 2) for axis in range(3)]
                    destination = {
                        0x44: angular_velocity, 0x45: angular_velocity,
                        0x46: angular_acceleration, 0x47: angular_acceleration,
                        0x4B: linear_velocity, 0x4C: linear_velocity,
                        0x4D: linear_acceleration, 0x4E: linear_acceleration,
                    }[opcode]
                    if opcode in {0x45, 0x47, 0x4C, 0x4E}:
                        for axis in range(3):
                            destination[axis] += values[axis]
                    else:
                        destination[:] = values
                elif opcode == 0x49:
                    pose[0]["translation_psx"] = [s16(script, pc + 2 + axis * 2)
                                                     for axis in range(3)]
                elif opcode not in {0x0F, 0x19, 0x1E, 0x24, 0x36,
                                    0x38, 0x39, 0x42, 0x43, 0x48, 0x4A, 0x4B,
                                    0x4D, 0x4F, 0x53, 0x54, 0x57, 0x58, 0x5A,
                                    0x60, 0x61, 0x6F, 0x70}:
                    unsupported.add(opcode)
                elif opcode in {0x14, 0x1D, 0x38, 0x39, 0x42, 0x43, 0x53, 0x57}:
                    unsupported.add(opcode)
                pc += size
            unsupported.add(0xFF)
            ended = True
            return True

        execute(set(), True)
        frames = [{"pose": _copy_mecha_pose(pose),
                   "scales": [list(value) for value in scales],
                   "visible": list(visible)}]
        if not (ended and not active):
            for _ in range(2400):
                completed = update_tracks()
                execute(completed, False)
                frames.append({"pose": _copy_mecha_pose(pose),
                               "scales": [list(value) for value in scales],
                               "visible": list(visible)})
                completed_loop = active and all(
                    track["looping"] and track["group"] in completed
                    for track in active.values()
                )
                if ended and (not active or completed_loop):
                    break
        actions.append({
            "script_index": script_index, "frames": frames,
            "unsupported_opcodes": sorted(unsupported),
            "truncated": len(frames) >= 2401,
        })
    return actions


def parse_battling_hierarchy(data: bytes, offset: int, part_count: int) -> list[dict]:
    if offset < 0 or offset + 4 > len(data):
        raise ValueError("Battling hierarchy pointer is invalid")
    count = u32(data, offset)
    if count == 0 or count > MAX_MEMBERS or offset + 4 + count * 16 > len(data):
        raise ValueError("Battling hierarchy is invalid")
    entries = []
    for index in range(count):
        values = struct.unpack_from("<hhhhhhhh", data, offset + 4 + index * 16)
        parent, mesh = values[:2]
        if parent >= index or parent < -1 or mesh >= part_count or mesh < -1:
            raise ValueError("Battling hierarchy reference is invalid")
        entries.append({
            "index": index, "parent_index": None if parent == -1 else parent,
            "model_part_index": None if mesh == -1 else mesh,
            "rotation_psx": list(values[2:5]), "translation_psx": list(values[5:8]),
        })
    return entries


def apply_battling_clip_first_pose(clip: bytes, bones: list[dict]) -> list[dict]:
    if len(clip) < 8:
        raise ValueError("Battling animation clip header is truncated")
    constant_count, animated_count = s16(clip, 4), s16(clip, 6)
    if constant_count < 0 or animated_count < 0:
        raise ValueError("Battling animation clip channel count is invalid")
    descriptor_end = 8 + (constant_count + animated_count) * 4
    if descriptor_end > len(clip):
        raise ValueError("Battling animation clip descriptors are truncated")
    pose = copy.deepcopy(bones)

    def apply_component(code: int, bone_index: int, value: int) -> None:
        component = (code & 0x7F) - 3
        if not 0 <= bone_index < len(pose) or not 0 <= component < 6:
            return
        key = "rotation_psx" if component < 3 else "translation_psx"
        pose[bone_index][key][component % 3] = value

    for index in range(constant_count):
        code, bone_index, value = struct.unpack_from("<BBh", clip, 8 + index * 4)
        apply_component(code, bone_index, value)
    for index in range(animated_count):
        at = 8 + (constant_count + index) * 4
        code, bone_index, stream_offset = struct.unpack_from("<BBH", clip, at)
        if stream_offset >= len(clip):
            raise ValueError("Battling animation clip stream is out of range")
        command = clip[stream_offset]
        if command & 0xC0 == 0xC0:
            if stream_offset + 2 > len(clip):
                raise ValueError("Battling animation clip first sample is truncated")
            value = struct.unpack_from("<b", clip, stream_offset + 1)[0] * 0x40
            value |= command & 0x3F
        elif not command & 0x80:
            value = command & 0x7F
            if value & 0x40:
                value -= 0x80
        else:
            value = 0
        apply_component(code, bone_index, value)
    for bone in pose:
        bone["initial_pose_resolved"] = True
        bone["initial_pose_provenance"] = "battling_action_000_clip_first_sample"
    return pose


def _psx_local_matrix(rotation: list[int], translation: list[int], unit_scale: float,
                      scale: list[int] | tuple[int, int, int] = (0x1000, 0x1000, 0x1000)) -> list[float]:
    x, y, z = (value * math.tau / 4096.0 for value in rotation)
    sx, cx = math.sin(x), math.cos(x)
    sy, cy = math.sin(y), math.cos(y)
    sz, cz = math.sin(z), math.cos(z)
    psx = (
        (cz * cy, -sz * cy, sy),
        (sz * cx + cz * sy * sx, cz * cx - sz * sy * sx, -cy * sx),
        (-cz * sy * cx + sz * sx, cz * sx + sz * sy * cx, cy * cx),
    )
    basis = (1.0, -1.0, -1.0)
    converted = tuple(
        tuple(basis[row] * psx[row][column] * basis[column] * scale[column] / 0x1000
              for column in range(3))
        for row in range(3)
    )
    tx, ty, tz = (basis[index] * translation[index] * unit_scale for index in range(3))
    return [
        converted[0][0], converted[1][0], converted[2][0], 0.0,
        converted[0][1], converted[1][1], converted[2][1], 0.0,
        converted[0][2], converted[1][2], converted[2][2], 0.0,
        tx, ty, tz, 1.0,
    ]


def _psx_trs(rotation: list[int], translation: list[int], unit_scale: float) -> dict:
    matrix = _psx_local_matrix(rotation, translation, unit_scale)
    m00, m10, m20 = matrix[0:3]
    m01, m11, m21 = matrix[4:7]
    m02, m12, m22 = matrix[8:11]
    trace = m00 + m11 + m22
    if trace > 0:
        factor = math.sqrt(trace + 1.0) * 2
        quaternion = ((m21 - m12) / factor, (m02 - m20) / factor,
                      (m10 - m01) / factor, factor / 4)
    elif m00 > m11 and m00 > m22:
        factor = math.sqrt(1.0 + m00 - m11 - m22) * 2
        quaternion = (factor / 4, (m01 + m10) / factor,
                      (m02 + m20) / factor, (m21 - m12) / factor)
    elif m11 > m22:
        factor = math.sqrt(1.0 + m11 - m00 - m22) * 2
        quaternion = ((m01 + m10) / factor, factor / 4,
                      (m12 + m21) / factor, (m02 - m20) / factor)
    else:
        factor = math.sqrt(1.0 + m22 - m00 - m11) * 2
        quaternion = ((m02 + m20) / factor, (m12 + m21) / factor,
                      factor / 4, (m10 - m01) / factor)
    magnitude = math.sqrt(sum(value * value for value in quaternion)) or 1.0
    return {
        "translation": matrix[12:15],
        "rotation": [value / magnitude for value in quaternion],
    }


def build_hierarchy_gltf(base: dict, bones: list[dict], hierarchy_format: str,
                         unit_scale: float, buffer_uri: str) -> dict:
    if hierarchy_format not in {"bone_links", "battling_hierarchy"}:
        raise ValueError("unsupported hierarchy format")
    part_nodes = base["nodes"]
    nodes = [{
        "name": "hierarchy_root", "children": [],
        "extras": {"xenogears": {"synthetic_runtime_root": True,
                                    "hierarchy_format": hierarchy_format}},
    }]
    referenced_parts = set()
    bone_meshes = []
    for bone in bones:
        index = bone["index"]
        if index != len(nodes) - 1:
            raise ValueError("hierarchy entries are not sequential")
        parent = bone["parent_index"]
        if parent is not None and not 0 <= parent < index:
            raise ValueError("hierarchy parent is not parent-first")
        part_index = bone["model_part_index"]
        if part_index is not None and not 0 <= part_index < len(part_nodes):
            raise ValueError("hierarchy model part is out of range")
        node = {
            "name": f"bone_{index:04d}",
            "extras": {"xenogears": {
                "source_bone_index": index, "source_parent_index": parent,
                "source_model_part_index": part_index,
                "transform_provenance": (
                    bone.get("initial_pose_provenance", "animation_vm_entry_0_direct_pose")
                    if bone.get("initial_pose_resolved")
                    else "runtime_identity_default" if hierarchy_format == "bone_links"
                    else "serialized_rotation_translation"
                ),
            }},
        }
        if part_index is not None:
            referenced_parts.add(part_index)
            if "mesh" in part_nodes[part_index]:
                bone_meshes.append((index, part_index, part_nodes[part_index]["mesh"]))
        if hierarchy_format == "battling_hierarchy" or bone.get("initial_pose_resolved"):
            node.update(_psx_trs(
                bone["rotation_psx"], bone["translation_psx"], unit_scale,
            ))
            node["extras"]["xenogears"]["rotation_psx"] = bone["rotation_psx"]
            node["extras"]["xenogears"]["translation_psx"] = bone["translation_psx"]
            node["extras"]["xenogears"]["rotation_psx"] = bone["rotation_psx"]
            node["extras"]["xenogears"]["translation_psx"] = bone["translation_psx"]
        nodes.append(node)
        parent_node = nodes[0] if parent is None else nodes[parent + 1]
        parent_node.setdefault("children", []).append(index + 1)
    for part_index, part_node in enumerate(part_nodes):
        if part_index in referenced_parts:
            continue
        node = {
            "name": f"unattached_part_{part_index:04d}",
            "extras": {"xenogears": {"source_model_part_index": part_index,
                                       "hierarchy_attachment": "unreferenced_hidden"}},
        }
        if "mesh" in part_node:
            node["mesh"] = part_node["mesh"]
        nodes.append(node)
    if bone_meshes:
        for bone_index, part_index, mesh_index in bone_meshes:
            visibility_joint = len(nodes)
            nodes.append({
                "name": f"bone_{bone_index:04d}_visibility_part_{part_index:04d}",
                "mesh": mesh_index,
                "extras": {"xenogears": {
                    "source_bone_index": bone_index,
                    "source_model_part_index": part_index,
                    "role": "rigid_mesh_visibility_node",
                }},
            })
            nodes[bone_index + 1].setdefault("children", []).append(visibility_joint)
        for mesh in base["meshes"]:
            for primitive in mesh["primitives"]:
                primitive["attributes"].pop("JOINTS_0", None)
                primitive["attributes"].pop("WEIGHTS_0", None)
                primitive.get("extras", {}).pop("rigid_joint_accessors", None)
        base.pop("skins", None)
    base["nodes"] = nodes
    base["scenes"] = [{"nodes": [0]}]
    base["buffers"][0]["uri"] = buffer_uri
    base["extras"] = {
        **base.get("extras", {}),
        "hierarchy_assembly": {"format": hierarchy_format, "bone_count": len(bones),
                               "uses_skinning": False,
                               "skinning_policy": "rigid_mesh_parenting" if bone_meshes else "none"},
    }
    return base


def _append_aligned(buffer: bytearray, payload: bytes, alignment: int = 4) -> tuple[int, int]:
    while len(buffer) % alignment:
        buffer.append(0)
    offset = len(buffer)
    buffer.extend(payload)
    return offset, len(payload)


def _bounds(values: list[tuple[float, ...]]) -> tuple[list[float], list[float]]:
    return ([min(value[index] for value in values) for index in range(len(values[0]))],
            [max(value[index] for value in values) for index in range(len(values[0]))])


def build_gltf(model: ModelArchive, unit_scale: float, *, lit_materials: bool = False,
               generate_missing_normals: bool = False) -> tuple[dict, bytes]:
    gltf = {
        "asset": {"version": "2.0", "generator": "Xenogears model extractor"},
        "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [],
        "materials": [], "buffers": [{"uri": "model.bin", "byteLength": 0}],
        "bufferViews": [], "accessors": [],
        "extras": {"coordinate_conversion": "x,-y,-z", "unit_scale": unit_scale},
    }
    if not lit_materials:
        gltf["extensionsUsed"] = ["KHR_materials_unlit"]
    binary = bytearray()
    material_indexes: dict[tuple, int] = {}

    def accessor(payload: bytes, component_type: int, count: int, kind: str,
                 *, normalized: bool = False,
                 bounds: tuple[list[float], list[float]] | None = None,
                 target: int | None = None) -> int:
        offset, length = _append_aligned(binary, payload)
        view = len(gltf["bufferViews"])
        buffer_view = {"buffer": 0, "byteOffset": offset, "byteLength": length}
        if target is not None:
            buffer_view["target"] = target
        gltf["bufferViews"].append(buffer_view)
        record = {"bufferView": view, "componentType": component_type, "count": count, "type": kind}
        if normalized:
            record["normalized"] = True
        if bounds is not None:
            record["min"], record["max"] = bounds
        result = len(gltf["accessors"])
        gltf["accessors"].append(record)
        return result

    for part_index, part in enumerate(model.parts):
        grouped: dict[tuple, list[Primitive]] = {}
        for primitive in part.primitives:
            key = (
                primitive.family, primitive.tpage, primitive.clut,
                bool(primitive.command & 1), bool(primitive.command & 2),
                primitive.texture_state_status,
            )
            grouped.setdefault(key, []).append(primitive)
        mesh_primitives = []
        for key, primitives in grouped.items():
            family, tpage, clut, raw_texture, semitransparent, texture_state_status = key
            positions: list[tuple[float, float, float]] = []
            normals: list[tuple[float, float, float]] = []
            texcoords: list[tuple[float, float]] = []
            colors: list[tuple[int, int, int, int]] = []
            indices: list[int] = []
            textured = family in TEXTURED_FAMILIES and family != 16
            use_authored_normals = family in RELIGHTABLE_FAMILIES and bool(part.flags & 0x10)
            include_normals = use_authored_normals or generate_missing_normals
            for primitive in primitives:
                corner_count = 3 if family < 8 or family == 16 else 4
                base = len(positions)
                face_normal = (0.0, 1.0, 0.0)
                if include_normals:
                    face = [part.vertices[primitive.indices[index]] for index in range(3)]
                    edge1 = (face[1][0] - face[0][0], -face[1][1] + face[0][1],
                             -face[1][2] + face[0][2])
                    edge2 = (face[2][0] - face[0][0], -face[2][1] + face[0][1],
                             -face[2][2] + face[0][2])
                    cross = (edge1[1] * edge2[2] - edge1[2] * edge2[1],
                             edge1[2] * edge2[0] - edge1[0] * edge2[2],
                             edge1[0] * edge2[1] - edge1[1] * edge2[0])
                    face_magnitude = math.sqrt(sum(value * value for value in cross))
                    if face_magnitude:
                        face_normal = tuple(value / face_magnitude for value in cross)
                for corner in range(corner_count):
                    source = part.vertices[primitive.indices[corner]]
                    positions.append((source[0] * unit_scale, -source[1] * unit_scale, -source[2] * unit_scale))
                    if use_authored_normals:
                        normal = part.normals[primitive.indices[corner]]
                        magnitude = math.sqrt(normal[0] ** 2 + normal[1] ** 2 + normal[2] ** 2)
                        normals.append(
                            (normal[0] / magnitude, -normal[1] / magnitude, -normal[2] / magnitude)
                            if magnitude else face_normal
                        )
                    elif include_normals:
                        normals.append(face_normal)
                    if textured:
                        uv = primitive.uvs[corner]
                        texcoords.append((uv[0] / 256.0, uv[1] / 256.0))
                    colors.append(primitive.color)
                if corner_count == 3:
                    indices.extend((base, base + 1, base + 2))
                else:
                    indices.extend((base, base + 1, base + 2, base + 2, base + 1, base + 3))
            position_accessor = accessor(
                struct.pack(f"<{len(positions) * 3}f", *(component for value in positions for component in value)),
                5126, len(positions), "VEC3", bounds=_bounds(positions), target=34962,
            )
            attributes = {"POSITION": position_accessor}
            if normals:
                attributes["NORMAL"] = accessor(
                    struct.pack(f"<{len(normals) * 3}f", *(component for value in normals for component in value)),
                    5126, len(normals), "VEC3", target=34962,
                )
            if texcoords:
                attributes["TEXCOORD_0"] = accessor(
                    struct.pack(f"<{len(texcoords) * 2}f", *(component for value in texcoords for component in value)),
                    5126, len(texcoords), "VEC2", target=34962,
                )
            attributes["COLOR_0"] = accessor(
                bytes(component for value in colors for component in value), 5121, len(colors),
                "VEC4", normalized=True, target=34962,
            )
            index_component = 5123 if len(positions) <= 0xFFFF else 5125
            index_format = "H" if index_component == 5123 else "I"
            index_accessor = accessor(
                struct.pack(f"<{len(indices)}{index_format}", *indices), index_component,
                len(indices), "SCALAR", target=34963,
            )
            material_key = (family, tpage, clut, raw_texture, semitransparent, texture_state_status)
            if material_key not in material_indexes:
                material_indexes[material_key] = len(gltf["materials"])
                tpage_name = "unknown" if tpage is None else f"{tpage:04x}"
                clut_name = "unknown" if clut is None else f"{clut:04x}"
                material = {
                    "name": f"family_{family:02x}_tpage_{tpage_name}_clut_{clut_name}",
                    "pbrMetallicRoughness": {"metallicFactor": 0, "roughnessFactor": 1},
                    "doubleSided": True,
                    "alphaMode": "BLEND" if semitransparent else "OPAQUE",
                    "extras": {
                        "ps1_family": family, "ps1_tpage": tpage, "ps1_clut": clut,
                        "texture_depth": None if tpage is None else (tpage >> 7) & 3,
                        "blend_mode": None if tpage is None else (tpage >> 5) & 3,
                        "raw_texture": raw_texture, "semitransparent": semitransparent,
                        "texture_state_status": texture_state_status,
                        "texture_binding_status": "preserved_ps1_state_not_baked",
                    },
                }
                if not lit_materials:
                    material["extensions"] = {"KHR_materials_unlit": {}}
                gltf["materials"].append(material)
            mesh_primitive = {
                "attributes": attributes, "indices": index_accessor,
                "material": material_indexes[material_key], "mode": 4,
                "extras": {"source_primitive_count": len(primitives)},
            }
            mesh_primitives.append(mesh_primitive)
        node_index = len(gltf["nodes"])
        node = {"name": f"part_{part_index:04d}"}
        if mesh_primitives:
            mesh_index = len(gltf["meshes"])
            gltf["meshes"].append({"name": f"part_{part_index:04d}", "primitives": mesh_primitives})
            node["mesh"] = mesh_index
        else:
            node["extras"] = {"source_part_index": part_index, "source_primitive_count": 0}
        gltf["nodes"].append(node)
        gltf["scenes"][0]["nodes"].append(node_index)
    gltf["buffers"][0]["byteLength"] = len(binary)
    return gltf, bytes(binary)


def write_if_changed(path: Path, content: bytes) -> None:
    if path.is_file() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(content)
    temporary.replace(path)


def _occurrence(disc_index: int, disc_name: str, fat_index: int, entry: FatEntry,
                routes: list[dict], chain: list[dict]) -> dict:
    return {
        "disc_index": disc_index, "disc_name": disc_name, "fat_index": fat_index,
        "lba": entry.lba, "stored_size": entry.size, "routes": routes,
        "transform_chain": chain,
    }


def _next_pointer(data: bytes, start: int, pointers: Iterable[int]) -> int:
    candidates = [pointer for pointer in pointers if pointer > start and pointer <= len(data)]
    return min(candidates) if candidates else len(data)


def scan_disc(path: Path, disc_index: int) -> tuple[dict, list[dict]]:
    disc = open_disc(path)
    entries = parse_fat(disc.read_user_data(FAT_LBA, FAT_SECTORS * USER_SECTOR), disc.sector_count)
    directory = parse_directory_table(disc.read_user_data(DIRECTORY_LBA, DIRECTORY_COUNT * 2))
    routes_by_index = map_physical_routes(directory, len(entries))
    resources = []
    diagnostics = []
    scan_counts: Counter[str] = Counter()

    def report(message: str, entry: FatEntry, fat_index: int, routes: list[dict],
               chain: list[dict]) -> None:
        diagnostics.append({
            "message": message,
            "occurrence": _occurrence(disc_index, path.name, fat_index, entry, routes, chain),
        })

    def add(kind: str, data: bytes, entry: FatEntry, fat_index: int, routes: list[dict],
            chain: list[dict], **metadata: object) -> None:
        occurrence = _occurrence(disc_index, path.name, fat_index, entry, routes, chain)
        if metadata:
            occurrence["resource_context"] = metadata
        resources.append({
            "kind": kind, "data": data,
            "occurrence": occurrence,
            "metadata": metadata,
        })

    def add_texture_package(data: bytes, entry: FatEntry, fat_index: int,
                            routes: list[dict], chain: list[dict],
                            format_name: str, **context: object) -> None:
        package_digest = sha256(data)
        add("texture_package", data, entry, fat_index, routes, chain,
            format=format_name, **context)
        try:
            if format_name == "tim-bundle":
                for member_index, start, tim, trailer_size in tim_bundle_members(data):
                    add("texture_image", tim.raw, entry, fat_index, routes, chain + [{
                        "type": "tim_bundle_member", "member_index": member_index,
                        "decoded_offset": start,
                    }], source_package_sha256=package_digest, width=tim.width,
                        height=tim.height, bpp=tim.bpp,
                        image_rectangle={"x_words": tim.image_x, "y": tim.image_y,
                                         "width_words": tim.width_words, "height": tim.height},
                        clut_origin=None if tim.clut_x is None else {"x_words": tim.clut_x, "y": tim.clut_y},
                        palette_count=len(tim.palettes), trailer_size=trailer_size, **context)
            elif format_name == "packed-tagged-images":
                for record_index, start, upload, trailer_size in parse_packed_image_set(data):
                    add("texture_upload", upload.data, entry, fat_index, routes, chain + [{
                        "type": "packed_image_record", "record_index": record_index,
                        "decoded_offset": start,
                    }], source_package_sha256=package_digest,
                        upload_kind="image" if upload.tag == 0x1100 else "clut",
                        stored_origin={"x_words": upload.stored_x, "y": upload.stored_y},
                        relative_origin={"x_words": upload.relative_x, "y": upload.relative_y},
                        width_words=upload.width_words, height=upload.height,
                        placement_status="caller_mode_and_base_required",
                        trailer_size=trailer_size, **context)
            elif format_name == "field-sector-graphics" and len(data) >= USER_SECTOR:
                for descriptor_index, start, upload in parse_sector_graphics(data):
                    add("texture_upload", upload.data, entry, fat_index, routes, chain + [{
                        "type": "field_sector_descriptor", "descriptor_index": descriptor_index,
                        "stored_offset": start,
                    }], source_package_sha256=package_digest,
                        upload_kind="image" if upload.tag == 0x1200 else "clut",
                        stored_origin={"x_words": upload.stored_x, "y": upload.stored_y},
                        relative_origin={"x_words": upload.relative_x, "y": upload.relative_y},
                        resolved_origin={"x_words": upload.stored_x + upload.relative_x,
                                         "y": upload.stored_y + upload.relative_y},
                        width_words=upload.width_words, height=upload.height,
                        placement_status="serialized_mode_0", **context)
        except (ValueError, struct.error) as error:
            report(f"{format_name} rejected: {error}", entry, fat_index, routes, chain)

    def add_model(data: bytes, entry: FatEntry, fat_index: int, routes: list[dict],
                   chain: list[dict], *, skeleton: bytes | None = None,
                   skeleton_format: str = "bone_links",
                   report_failure: bool = True, **context: object) -> ModelArchive | None:
        try:
            model = parse_model_archive(data)
        except (ValueError, struct.error) as error:
            if report_failure:
                report(f"model archive rejected: {error}", entry, fat_index, routes, chain)
            return None
        add("model", data, entry, fat_index, routes, chain,
            part_count=len(model.parts), primitive_count=sum(len(part.primitives) for part in model.parts),
            **context)
        if skeleton is not None:
            try:
                bones = parse_bone_links(skeleton, len(model.parts))
            except (ValueError, struct.error):
                pass
            else:
                add("skeleton", skeleton, entry, fat_index, routes,
                    chain + [{"type": skeleton_format}], bones=bones,
                    format=skeleton_format, model_sha256=sha256(data), **context)
        return model

    for fat_index, entry in enumerate(entries):
        if entry.size <= 0 or entry.size > MAX_RESOURCE_SIZE:
            continue
        routes = routes_by_index.get(fat_index, [])
        ids = route_ids(routes)
        if not ids:
            continue
        relevant = any(
            directory_id in {0x04, 0x0D, 0x0F, 0x24, 0x29, 0x30, 0x31}
            for directory_id, _ in ids
        )
        if not relevant:
            continue
        padded = disc.read_user_data(entry.lba, entry.size, padded=True)
        raw = padded[:entry.size]

        field_route = next(((d, f) for d, f in ids if d == 0x04 and f >= 0xB8 and f <= 0x66A and (f - 0xB8) % 2 == 0), None)
        if field_route:
            field_id = (field_route[1] - 0xB8) // 2
            scene_key = f"field:{field_id}"
            if len(raw) < 0x154:
                scan_counts["field_placeholder_files"] += 1
                continue
            try:
                section, consumed = lzss_decompress(padded, u32(raw, 0x138))
                members = offset_bundle(section)
            except (ValueError, struct.error) as error:
                report(f"field model section rejected: {error}", entry, fat_index, routes,
                       [{"type": "field_model_section_lzss"}])
                continue
            for member_index, (start, end) in enumerate(members):
                add_model(section[start:end], entry, fat_index, routes, [{
                    "type": "field_model_section_lzss", "member_index": member_index,
                    "stored_offset": u32(raw, 0x138), "consumed_size": consumed,
                    "decoded_offset": start,
                }], scene_key=scene_key, field_id=field_id, model_member_index=member_index)
            entity_count = u16(raw, 0x18C)
            entity_end = 0x190 + entity_count * 0x10
            if entity_end <= len(raw):
                add("placements", raw[0x190:entity_end], entry, fat_index, routes,
                    [{"type": "field_entity_initialization", "stored_offset": 0x190}],
                    format="field-entity-initialization", entity_count=entity_count,
                    scene_key=scene_key, field_id=field_id)
            try:
                walkmesh, walkmesh_consumed = lzss_decompress(padded, u32(raw, 0x134))
            except (ValueError, struct.error) as error:
                report(f"field walkmesh rejected: {error}", entry, fat_index, routes,
                       [{"type": "field_walkmesh_lzss"}])
            else:
                add("collision", walkmesh, entry, fat_index, routes,
                    [{"type": "field_walkmesh_lzss", "consumed_size": walkmesh_consumed}],
                    format="field-walkmesh", scene_key=scene_key, field_id=field_id)
            continue

        companion_route = next(((d, f) for d, f in ids if d == 0x04 and f >= 0xB9 and f <= 0x66B and (f - 0xB9) % 2 == 0), None)
        if companion_route:
            field_id = (companion_route[1] - 0xB9) // 2
            add_texture_package(raw, entry, fat_index, routes, [{"type": "field_sector_graphics"}],
                                "field-sector-graphics", scene_key=f"field:{field_id}", field_id=field_id)
            continue

        gear_model_route = next(((d, f) for d, f in ids if d == 0x04 and 0x6BB <= f <= 0x749 and (f - 0x6BB) % 2 == 0), None)
        if gear_model_route:
            try:
                members = offset_bundle(raw)
            except (ValueError, struct.error) as error:
                report(f"mecha container rejected: {error}", entry, fat_index, routes,
                       [{"type": "mecha_model_payload"}])
                members = []
            if len(members) >= 3:
                gear_id = (gear_model_route[1] - 0x6BB) // 2
                scene_key = f"gear04:{gear_id}"
                texture = raw[members[0][0]:members[0][1]]
                model_data = raw[members[1][0]:members[1][1]]
                skeleton = raw[members[2][0]:members[2][1]]
                gear_scale = 0x1000
                if len(members) >= 4:
                    try:
                        config_members = offset_bundle(raw, members[3][0], members[3][1])
                        gear_scale = s16(raw, config_members[0][0] + 8)
                    except (ValueError, struct.error, IndexError):
                        pass
                model = add_model(model_data, entry, fat_index, routes,
                                  [{"type": "mecha_model_payload", "member_index": 1}],
                                  skeleton=skeleton, scene_key=scene_key, gear_id=gear_id,
                                  gear_scale_psx=gear_scale)
                add_texture_package(texture, entry, fat_index, routes,
                                    [{"type": "mecha_texture_payload", "member_index": 0}],
                                    "packed-tagged-images", scene_key=scene_key, gear_id=gear_id,
                                    model_sha256=None if model is None else sha256(model_data))
                animation_file = gear_model_route[1] - 1
                animation_fat = next((index for index, candidate in routes_by_index.items()
                                      if (0x04, animation_file) in route_ids(candidate)), None)
                if animation_fat is not None and entries[animation_fat].size > 0:
                    animation_entry = entries[animation_fat]
                    animation = disc.read_user_data(animation_entry.lba, animation_entry.size)
                    add("animation_container", animation, animation_entry, animation_fat,
                        routes_by_index.get(animation_fat, []), [{"type": "mecha_animation_file"}],
                        format="mecha-keyframes-and-vm", model_sha256=sha256(model_data),
                        scene_key=scene_key, gear_id=gear_id)
            continue

        world_configuration = next((configuration for d, f in ids for configuration in range(17)
                                    if d == 0x24 and f == 0x2C + configuration * 0x0B), None)
        if world_configuration is not None:
            try:
                expanded, consumed = lzss_decompress(padded)
                sections = offset_bundle(expanded)
            except (ValueError, struct.error) as error:
                report(f"world model container rejected: {error}", entry, fat_index, routes,
                       [{"type": "world_section", "configuration": world_configuration}])
                sections = []
            if len(sections) >= 4:
                model_start, model_end = sections[1]
                model_data = expanded[model_start:model_end]
                add_model(model_data, entry, fat_index, routes, [{
                    "type": "world_section", "configuration": world_configuration,
                    "section_index": 1, "consumed_size": consumed,
                }], scene_key=f"world:{world_configuration}", configuration=world_configuration)
                add("collision", expanded[sections[2][0]:sections[2][1]], entry, fat_index, routes,
                    [{"type": "world_section", "configuration": world_configuration, "section_index": 2}],
                    format="world-model-collision", model_sha256=sha256(model_data),
                    scene_key=f"world:{world_configuration}", configuration=world_configuration)
                add("placements", expanded[sections[3][0]:sections[3][1]], entry, fat_index, routes,
                    [{"type": "world_section", "configuration": world_configuration, "section_index": 3}],
                    format="world-model-placements", model_sha256=sha256(model_data),
                    scene_key=f"world:{world_configuration}", configuration=world_configuration)
            continue

        world_aux = next(((d, f, configuration, relative) for d, f in ids
                          for configuration in range(17)
                          for relative in (2, 3, 9, 10)
                          if d == 0x24 and f == 0x2B + configuration * 0x0B + relative), None)
        if world_aux:
            relative = world_aux[3]
            if relative in {2, 3}:
                try:
                    expanded, consumed = lzss_decompress(padded)
                except ValueError:
                    expanded, consumed = raw, 0
                add_texture_package(expanded, entry, fat_index, routes,
                    [{"type": "world_texture_bundle", "configuration": world_aux[2],
                      "relative_file": relative, "consumed_size": consumed}], "tim-bundle",
                    scene_key=f"world:{world_aux[2]}", configuration=world_aux[2],
                    texture_role="ground" if relative == 2 else "shared")
            else:
                add("terrain", raw, entry, fat_index, routes,
                    [{"type": "world_terrain", "configuration": world_aux[2], "relative_file": relative}],
                    format="world-terrain-slots", traversal="row-major" if relative == 9 else "transposed",
                    scene_key=f"world:{world_aux[2]}", configuration=world_aux[2])
            continue

        battling_route = next(((d, f) for d, f in ids if d == 0x31 and 2 <= f <= 0x32), None)
        if battling_route:
            try:
                expanded, consumed = lzss_decompress(padded)
                build_base = u32(expanded, 0x1C)
                pointers = [u32(expanded, index * 4) - build_base for index in range(10)
                            if u32(expanded, index * 4) >= build_base and u32(expanded, index * 4) != 0]
                model_offset = u32(expanded, 4) - build_base
                model_end = _next_pointer(expanded, model_offset, pointers)
                model_data = expanded[model_offset:model_end]
                model = parse_model_archive(model_data)
            except (ValueError, struct.error) as error:
                report(f"Battling archive rejected: {error}", entry, fat_index, routes,
                       [{"type": "battling_archive_lzss"}])
                continue
            add("model", model_data, entry, fat_index, routes,
                [{"type": "battling_archive_lzss", "consumed_size": consumed, "decoded_offset": model_offset}],
                part_count=len(model.parts), primitive_count=sum(len(part.primitives) for part in model.parts),
                scene_key=f"battling:{battling_route[1] - 2}", fighter_resource_id=battling_route[1] - 2)
            hierarchy_offset = u32(expanded, 0) - build_base
            try:
                hierarchy = parse_battling_hierarchy(expanded, hierarchy_offset, len(model.parts))
            except (ValueError, struct.error):
                hierarchy = []
            clip_offsets = []
            clip_table_offset = u32(expanded, 8) - build_base if u32(expanded, 8) else -1
            if 0 <= clip_table_offset + 4 <= len(expanded):
                clip_count = u32(expanded, clip_table_offset)
                if 0 < clip_count <= 256 and clip_table_offset + 4 + clip_count * 4 <= len(expanded):
                    clip_offsets = [
                        u32(expanded, clip_table_offset + 4 + index * 4) - build_base
                        for index in range(clip_count)
                    ]
            boundaries = sorted(set(
                offset for offset in pointers + clip_offsets if 0 <= offset <= len(expanded)
            ))
            if hierarchy and clip_offsets and 0 <= clip_offsets[0] < len(expanded):
                clip_end = _next_pointer(expanded, clip_offsets[0], boundaries)
                try:
                    hierarchy = apply_battling_clip_first_pose(
                        expanded[clip_offsets[0]:clip_end], hierarchy,
                    )
                except (ValueError, struct.error):
                    pass
            if hierarchy:
                hierarchy_end = hierarchy_offset + 4 + len(hierarchy) * 16
                add("skeleton", expanded[hierarchy_offset:hierarchy_end], entry, fat_index, routes,
                    [{"type": "battling_hierarchy", "decoded_offset": hierarchy_offset}],
                    bones=hierarchy, format="battling_hierarchy",
                    model_sha256=sha256(model_data),
                    scene_key=f"battling:{battling_route[1] - 2}",
                    fighter_resource_id=battling_route[1] - 2)
            for clip_index, clip_offset in enumerate(clip_offsets):
                clip_end = _next_pointer(expanded, clip_offset, boundaries)
                if clip_offset < clip_end:
                    clip = expanded[clip_offset:clip_end]
                    add("animation_clip", clip, entry, fat_index, routes,
                        [{"type": "battling_clip", "clip_index": clip_index, "decoded_offset": clip_offset}],
                        format="battling-transform-tracks", duration=u16(clip, 0) if len(clip) >= 2 else None,
                        model_sha256=sha256(model_data),
                        scene_key=f"battling:{battling_route[1] - 2}",
                        fighter_resource_id=battling_route[1] - 2)
            texture_pointer = u32(expanded, 0x0C)
            if texture_pointer:
                texture_offset = texture_pointer - build_base
                texture_end = _next_pointer(expanded, texture_offset, pointers)
                add_texture_package(expanded[texture_offset:texture_end], entry, fat_index, routes,
                    [{"type": "battling_packed_textures", "decoded_offset": texture_offset}],
                    "packed-tagged-images", model_sha256=sha256(model_data),
                    scene_key=f"battling:{battling_route[1] - 2}",
                    fighter_resource_id=battling_route[1] - 2)
            continue

        if (0x30, 0x04) in ids:
            try:
                heightfield, consumed = lzss_decompress(padded)
            except ValueError as error:
                report(f"Battling heightfield rejected: {error}", entry, fat_index, routes,
                       [{"type": "battling_heightfield_lzss"}])
            else:
                add("terrain", heightfield, entry, fat_index, routes,
                    [{"type": "battling_heightfield_lzss", "consumed_size": consumed}],
                    format="battling-heightfield-128x128", scene_key="battling:common")
            continue
        if (0x30, 0x05) in ids:
            try:
                expanded, consumed = lzss_decompress(padded)
            except ValueError:
                expanded, consumed = raw, 0
            add_texture_package(expanded, entry, fat_index, routes,
                [{"type": "battling_common_tim_bundle", "consumed_size": consumed}],
                "tim-bundle", scene_key="battling:common")
            continue

        arena_route = next(((d, f) for d, f in ids if d == 0x0F and 6 <= f <= 0x9A and f % 2 == 0), None)
        if arena_route:
            try:
                members = offset_bundle(raw)
            except (ValueError, struct.error) as error:
                report(f"battle arena container rejected: {error}", entry, fat_index, routes,
                       [{"type": "battle_arena_environment"}])
                members = []
            if len(members) >= 3:
                arena_id = (arena_route[1] - 6) // 2
                scene_key = f"battle-arena:{arena_id}"
                texture = raw[members[0][0]:members[0][1]]
                model_data = raw[members[1][0]:members[1][1]]
                skeleton = raw[members[2][0]:members[2][1]]
                model = add_model(model_data, entry, fat_index, routes,
                                  [{"type": "battle_arena_environment", "member_index": 1}],
                                  skeleton=skeleton, scene_key=scene_key, arena_id=arena_id)
                add_texture_package(texture, entry, fat_index, routes,
                                    [{"type": "battle_arena_textures", "member_index": 0}],
                                    "packed-tagged-images", scene_key=scene_key, arena_id=arena_id,
                                    model_sha256=None if model is None else sha256(model_data))
            continue

        arena_terrain_route = next(((d, f) for d, f in ids
                                    if d == 0x0F and 7 <= f <= 0x9B and f % 2 == 1), None)
        if arena_terrain_route:
            arena_id = (arena_terrain_route[1] - 7) // 2
            if len(raw) >= 4:
                payload_size = u32(raw, 0)
                payload = raw[4:4 + payload_size]
                if len(payload) >= 0x514:
                    add("collision", payload, entry, fat_index, routes,
                        [{"type": "battle_arena_terrain", "stored_offset": 4}],
                        format="battle-arena-terrain", scene_key=f"battle-arena:{arena_id}",
                        arena_id=arena_id)
            continue

        enemy_route = next(((d, f) for d, f in ids if d == 0x0D and 3 <= f <= 0x99 and f % 2 == 1), None)
        if enemy_route and len(raw) >= 8:
            record_count = raw[0]
            if 1 <= record_count <= 8 and 8 + record_count * 12 <= len(raw):
                for visual_index in range(record_count):
                    animation_offset, container_offset = struct.unpack_from("<II", raw, 8 + visual_index * 12)
                    visual_type = raw[8 + visual_index * 12 + 8]
                    if visual_type != 1 or container_offset < 8 or container_offset >= len(raw):
                        continue
                    try:
                        members = offset_bundle(raw, container_offset)
                    except (ValueError, struct.error):
                        continue
                    if len(members) >= 3:
                        enemy_set = (enemy_route[1] - 3) // 2
                        scene_key = f"battle-enemy:{enemy_set}:{visual_index}"
                        texture = raw[members[0][0]:members[0][1]]
                        model_data = raw[members[1][0]:members[1][1]]
                        skeleton = raw[members[2][0]:members[2][1]]
                        add_model(model_data, entry, fat_index, routes,
                                  [{"type": "battle_enemy_model", "visual_index": visual_index,
                                    "container_offset": container_offset}], skeleton=skeleton,
                                  scene_key=scene_key, enemy_set=enemy_set, visual_index=visual_index)
                        add_texture_package(texture, entry, fat_index, routes,
                            [{"type": "battle_enemy_textures", "visual_index": visual_index}],
                            "packed-tagged-images", scene_key=scene_key, enemy_set=enemy_set,
                            visual_index=visual_index, model_sha256=sha256(model_data))
                        if 0 < animation_offset < len(raw):
                            animation_end = min(container_offset, len(raw)) if animation_offset < container_offset else len(raw)
                            add("animation_container", raw[animation_offset:animation_end], entry, fat_index, routes,
                                [{"type": "battle_enemy_animation", "visual_index": visual_index}],
                                format="mecha-keyframes-and-vm", model_sha256=sha256(model_data),
                                scene_key=scene_key, enemy_set=enemy_set, visual_index=visual_index)
            continue

        if any(d == 0x29 for d, _ in ids):
            battle_gear_file = next(f for d, f in ids if d == 0x29)
            scene_key = f"battle-gear:{battle_gear_file}"
            try:
                members = offset_bundle(raw)
                texture = raw[members[0][0]:members[0][1]]
                model_data = raw[members[1][0]:members[1][1]]
                skeleton = raw[members[2][0]:members[2][1]]
                model = parse_model_archive(model_data)
                parse_bone_links(skeleton, len(model.parts))
            except (ValueError, struct.error, IndexError):
                model = None
            if model is not None and len(members) >= 4:
                gear_scale = 0x1000
                try:
                    config_members = offset_bundle(raw, members[3][0], members[3][1])
                    gear_scale = s16(raw, config_members[0][0] + 8)
                except (ValueError, struct.error, IndexError):
                    pass
                add_model(
                    model_data, entry, fat_index, routes,
                    [{"type": "battle_gear_model_payload", "member_index": 1}],
                    skeleton=skeleton, scene_key=scene_key,
                    battle_gear_file_id=battle_gear_file, gear_scale_psx=gear_scale,
                )
                dependent_weapons = {
                    6: range(8, 14), 21: range(23, 27),
                    37: range(39, 43), 48: range(50, 54),
                }.get(battle_gear_file, ())
                texture_scenes = [scene_key, *(f"battle-gear:{file_id}" for file_id in dependent_weapons)]
                for texture_scene in texture_scenes:
                    add_texture_package(
                        texture, entry, fat_index, routes,
                        [{"type": "battle_gear_texture_payload", "member_index": 0}],
                        "packed-tagged-images", scene_key=texture_scene,
                        battle_gear_file_id=battle_gear_file,
                        model_sha256=sha256(model_data) if texture_scene == scene_key else None,
                        inherited_by_weapon=texture_scene != scene_key,
                    )
                animation_fat = next((
                    index for index, candidate in routes_by_index.items()
                    if (0x29, battle_gear_file + 1) in route_ids(candidate)
                ), None)
                if animation_fat is not None and entries[animation_fat].size > 0:
                    animation_entry = entries[animation_fat]
                    animation = disc.read_user_data(animation_entry.lba, animation_entry.size)
                    add(
                        "animation_container", animation, animation_entry, animation_fat,
                        routes_by_index.get(animation_fat, []),
                        [{"type": "battle_gear_animation_file"}],
                        format="mecha-keyframes-and-vm", model_sha256=sha256(model_data),
                        scene_key=scene_key, battle_gear_file_id=battle_gear_file,
                    )
                continue

            if battle_gear_file not in {
                    8, 9, 10, 11, 12, 13, 23, 24, 25, 26,
                    39, 40, 41, 42, 50, 51, 52, 53}:
                continue

            visited: set[tuple[int, int]] = set()

            def recurse(start: int, end: int, depth: int, chain: list[dict]) -> None:
                if depth > 3 or (start, end) in visited or start >= end:
                    return
                visited.add((start, end))
                candidate = raw[start:end]
                if add_model(candidate, entry, fat_index, routes, chain, report_failure=False,
                             scene_key=scene_key, battle_gear_file_id=battle_gear_file) is not None:
                    return
                try:
                    members = offset_bundle(raw, start, end)
                except (ValueError, struct.error):
                    return
                for member_index, (member_start, member_end) in enumerate(members):
                    recurse(member_start, member_end, depth + 1,
                            chain + [{"type": "nested_member", "member_index": member_index,
                                      "stored_offset": member_start}])

            recurse(0, len(raw), 0, [{"type": "battle_gear_resource"}])

    return {
        "disc_index": disc_index, "disc_name": path.name,
        "input_path": str(path.resolve()), "data_file": disc.data_path.name,
        "sha256": file_sha256(disc.data_path), "fat_entry_count": len(entries),
        "resource_occurrences": len(resources),
        "scan_counts": dict(sorted(scan_counts.items())), "diagnostics": diagnostics,
    }, resources


def materialize(output: Path, resources: list[dict], unit_scale: float, *,
                lit_materials: bool = False,
                generate_missing_normals: bool = False) -> list[dict]:
    skeleton_sources = {}
    animation_sources = {}
    for resource in resources:
        model_digest = resource["metadata"].get("model_sha256")
        if resource["kind"] == "skeleton" and model_digest is not None:
            sources = skeleton_sources.setdefault(model_digest, [])
            if not any(source["data"] == resource["data"] for source in sources):
                sources.append(resource)
        elif resource["kind"] == "animation_container" and model_digest is not None:
            animation_sources.setdefault(model_digest, resource)
    unique: dict[tuple[str, str], dict] = {}
    for resource in resources:
        digest = sha256(resource["data"])
        key = (resource["kind"], digest)
        if key not in unique:
            unique[key] = {**resource, "sha256": digest, "occurrences": []}
        unique[key]["occurrences"].append(resource["occurrence"])
    records = []
    for (kind, digest), item in sorted(unique.items()):
        data = item["data"]
        if kind == "model":
            model = parse_model_archive(data)
            root = output / "models" / digest
            gltf, binary = build_gltf(
                model, unit_scale, lit_materials=lit_materials,
                generate_missing_normals=generate_missing_normals,
            )
            gltf_path = Path("models") / digest / "model.gltf"
            binary_path = Path("models") / digest / "model.bin"
            source_path = Path("models") / digest / "source.bin"
            metadata_path = Path("models") / digest / "metadata.json"
            metadata = {
                "kind": kind, "sha256": digest, "part_count": len(model.parts),
                "vertex_count": sum(len(part.vertices) for part in model.parts),
                "primitive_count": sum(len(part.primitives) for part in model.parts),
                "triangle_count": sum(
                    1 if primitive.family < 8 or primitive.family == 16 else 2
                    for part in model.parts for primitive in part.primitives
                ),
                "metadata_command_count": model.metadata_command_count,
                "family_counts": dict(sorted(Counter(
                    f"0x{primitive.family:02X}" for part in model.parts for primitive in part.primitives
                ).items())),
                "texture_state_counts": dict(sorted(Counter(
                    primitive.texture_state_status
                    for part in model.parts for primitive in part.primitives
                ).items())),
                "occurrences": item["occurrences"],
            }
            write_if_changed(root / "model.gltf", (json.dumps(gltf, indent=2) + "\n").encode("utf-8"))
            write_if_changed(root / "model.bin", binary)
            write_if_changed(root / "source.bin", data)
            write_if_changed(root / "metadata.json", (json.dumps(metadata, indent=2) + "\n").encode("utf-8"))
            records.append({
                "kind": kind, "sha256": digest, "gltf_path": gltf_path.as_posix(),
                "binary_path": binary_path.as_posix(), "source_path": source_path.as_posix(),
                "metadata_path": metadata_path.as_posix(), "part_count": len(model.parts),
                "primitive_count": metadata["primitive_count"],
                "texture_state_counts": metadata["texture_state_counts"],
                "occurrences": item["occurrences"],
            })
        elif kind == "texture_image":
            tim = parse_tim(data)
            root = output / "textures" / "images" / digest
            source_path = Path("textures") / "images" / digest / "source.tim"
            metadata_path = Path("textures") / "images" / digest / "metadata.json"
            variants = range(len(tim.palettes)) if tim.palettes else range(1)
            previews = []
            for variant in variants:
                name = "image.png" if len(tim.palettes) <= 1 else f"palette-{variant:02d}.png"
                preview_path = Path("textures") / "images" / digest / name
                write_if_changed(output / preview_path, encode_png_rgba(
                    tim.width, tim.height, decode_tim(tim, variant),
                ))
                previews.append({"palette_variant": variant, "path": preview_path.as_posix()})
            payload = {
                "kind": kind, "sha256": digest, "size": len(data), "width": tim.width,
                "height": tim.height, "bpp": tim.bpp, "image_x_words": tim.image_x,
                "image_y": tim.image_y, "width_words": tim.width_words,
                "clut_x_words": tim.clut_x, "clut_y": tim.clut_y,
                "palette_count": len(tim.palettes), "row_trailing_bytes": tim.row_trailing_bytes,
                **item["metadata"], "previews": previews, "occurrences": item["occurrences"],
            }
            write_if_changed(root / "source.tim", data)
            write_if_changed(root / "metadata.json", (json.dumps(payload, indent=2) + "\n").encode("utf-8"))
            records.append({
                "kind": kind, "sha256": digest, "path": source_path.as_posix(),
                "metadata_path": metadata_path.as_posix(), "width": tim.width,
                "height": tim.height, "bpp": tim.bpp, "previews": previews,
                "occurrences": item["occurrences"],
            })
        elif kind == "skeleton":
            directory = f"{kind}s"
            path = Path(directory) / f"{digest}.bin"
            metadata_path = Path(directory) / f"{digest}.json"
            hierarchy_format = item["metadata"]["format"]
            model_hashes = {
                occurrence.get("resource_context", {}).get("model_sha256")
                for occurrence in item["occurrences"]
            }
            model_hashes.discard(None)
            assemblies = []
            for model_digest in sorted(model_hashes):
                model_source = output / "models" / model_digest / "source.bin"
                base_path = output / "models" / model_digest / "model.gltf"
                model = parse_model_archive(model_source.read_bytes())
                if hierarchy_format == "bone_links":
                    bones = parse_bone_links(data, len(model.parts))
                else:
                    source = next((
                        candidate for candidate in skeleton_sources.get(model_digest, [])
                        if candidate["data"] == data
                    ), None)
                    resolved_bones = None if source is None else source["metadata"].get("bones")
                    bones = (copy.deepcopy(resolved_bones) if resolved_bones
                             else parse_battling_hierarchy(data, 0, len(model.parts)))
                animation_source = animation_sources.get(model_digest)
                pose = None if animation_source is None else parse_mecha_initial_pose(
                    animation_source["data"], len(bones),
                )
                if pose is not None:
                    for bone, transform in zip(bones, pose):
                        bone.update(transform)
                        bone["initial_pose_resolved"] = True
                assembly_path = Path("assemblies") / model_digest / digest / "model.gltf"
                assembly = build_hierarchy_gltf(
                    json.loads(base_path.read_text(encoding="utf-8")), bones,
                    hierarchy_format, unit_scale, f"../../../models/{model_digest}/model.bin",
                )
                write_if_changed(output / assembly_path,
                                 (json.dumps(assembly, indent=2) + "\n").encode("utf-8"))
                assemblies.append({"model_sha256": model_digest, "gltf_path": assembly_path.as_posix()})
            payload = {
                "kind": kind, "sha256": digest, "size": len(data),
                **item["metadata"], "assemblies": assemblies, "occurrences": item["occurrences"],
            }
            write_if_changed(output / path, data)
            write_if_changed(output / metadata_path, (json.dumps(payload, indent=2) + "\n").encode("utf-8"))
            records.append({
                "kind": kind, "sha256": digest, "path": path.as_posix(),
                "metadata_path": metadata_path.as_posix(), "assemblies": assemblies,
                "occurrences": item["occurrences"],
            })
        elif kind == "placements":
            path = Path("placements") / f"{digest}.bin"
            metadata_path = Path("placements") / f"{digest}.json"
            payload = {
                "kind": kind, "sha256": digest, "size": len(data),
                **item["metadata"], "occurrences": item["occurrences"],
            }
            write_if_changed(output / path, data)
            write_if_changed(output / metadata_path, (json.dumps(payload, indent=2) + "\n").encode("utf-8"))
            records.append({
                "kind": kind, "sha256": digest, "path": path.as_posix(),
                "metadata_path": metadata_path.as_posix(), "occurrences": item["occurrences"],
            })
        else:
            directory = {
                "texture_package": "textures", "animation_container": "animations",
                "animation_clip": "animations", "terrain": "terrain", "collision": "collision",
                "texture_upload": "textures/uploads",
            }[kind]
            path = Path(directory) / f"{digest}.bin"
            metadata_path = Path(directory) / f"{digest}.json"
            payload = {"kind": kind, "sha256": digest, "size": len(data),
                       **item["metadata"], "occurrences": item["occurrences"]}
            write_if_changed(output / path, data)
            write_if_changed(output / metadata_path, (json.dumps(payload, indent=2) + "\n").encode("utf-8"))
            records.append({"kind": kind, "sha256": digest, "path": path.as_posix(),
                            "metadata_path": metadata_path.as_posix(), "occurrences": item["occurrences"]})
    return records


def _catalog_scene_path(scene_key: str, fat_index: int) -> Path:
    fields = scene_key.split(":")
    category = fields[0]
    values = [int(value) for value in fields[1:]]
    if category == "field":
        return Path("field") / f"field-{fat_index:04d}-{values[0]:04d}"
    if category == "gear04":
        return Path("gears") / f"gear-{fat_index:04d}-{values[0]:02d}"
    if category == "world":
        return Path("world") / f"configuration-{fat_index:04d}-{values[0]:02d}"
    if category == "battling":
        return Path("battling") / f"fighter-{fat_index:04d}-{values[0]:02d}"
    if category == "battle-arena":
        return Path("battle") / "arenas" / f"arena-{fat_index:04d}-{values[0]:02d}"
    if category == "battle-enemy":
        return (Path("battle") / "enemies"
                / f"set-{values[0]:02d}-{fat_index:04d}-visual-{values[1]:02d}")
    if category == "battle-gear":
        return Path("battle") / "gears" / f"file-{fat_index:04d}-{values[0]:03d}"
    safe = re.sub(r"[^a-zA-Z0-9._-]+", "-", scene_key).strip("-") or "unknown"
    return Path("unclassified") / f"scene-{fat_index:04d}-{safe}"


def _catalog_dependency_contexts(record: dict, scene_key: str, model_digest: str) -> list[dict]:
    contexts = []
    for occurrence in record["occurrences"]:
        context = occurrence.get("resource_context", {})
        dependency_scene = context.get("scene_key")
        if dependency_scene == scene_key:
            contexts.append(context)
        elif scene_key.startswith("battling:") and dependency_scene == "battling:common":
            contexts.append(context)
        elif dependency_scene is None and context.get("model_sha256") == model_digest:
            contexts.append(context)
    return contexts


def _append_glb_payload(gltf: dict, binary: bytearray, payload: bytes, name: str) -> int:
    offset, length = _append_aligned(binary, payload)
    index = len(gltf["bufferViews"])
    gltf["bufferViews"].append({
        "buffer": 0, "byteOffset": offset, "byteLength": length, "name": name,
    })
    return index


def _merge_gltf_model(gltf: dict, binary: bytearray, source: dict, source_binary: bytes,
                       model_digest: str, contexts: list[dict]) -> dict:
    while len(binary) % 4:
        binary.append(0)
    binary_base = len(binary)
    binary.extend(source_binary)
    view_base = len(gltf["bufferViews"])
    accessor_base = len(gltf["accessors"])
    material_base = len(gltf["materials"])
    mesh_base = len(gltf["meshes"])
    node_base = len(gltf["nodes"])
    skin_base = len(gltf.setdefault("skins", []))
    for view in source.get("bufferViews", []):
        merged = copy.deepcopy(view)
        merged["buffer"] = 0
        merged["byteOffset"] = binary_base + merged.get("byteOffset", 0)
        gltf["bufferViews"].append(merged)
    for accessor in source.get("accessors", []):
        merged = copy.deepcopy(accessor)
        if "bufferView" in merged:
            merged["bufferView"] += view_base
        if "sparse" in merged:
            merged["sparse"]["indices"]["bufferView"] += view_base
            merged["sparse"]["values"]["bufferView"] += view_base
        gltf["accessors"].append(merged)
    gltf["materials"].extend(copy.deepcopy(source.get("materials", [])))
    for mesh in source.get("meshes", []):
        merged = copy.deepcopy(mesh)
        for primitive in merged["primitives"]:
            primitive["attributes"] = {
                name: index + accessor_base for name, index in primitive["attributes"].items()
            }
            if "indices" in primitive:
                primitive["indices"] += accessor_base
            if "material" in primitive:
                primitive["material"] += material_base
            for target in primitive.get("targets", []):
                for name in target:
                    target[name] += accessor_base
        gltf["meshes"].append(merged)
    source_nodes = copy.deepcopy(source.get("nodes", []))
    for node in source_nodes:
        if "mesh" in node:
            node["mesh"] += mesh_base
        if "skin" in node:
            node["skin"] += skin_base
        if "children" in node:
            node["children"] = [child + node_base for child in node["children"]]
    implicit_bind_accessors = {}
    for skin in source.get("skins", []):
        merged = copy.deepcopy(skin)
        merged["joints"] = [joint + node_base for joint in merged["joints"]]
        if "skeleton" in merged:
            merged["skeleton"] += node_base
        if "inverseBindMatrices" in merged:
            merged["inverseBindMatrices"] += accessor_base
        else:
            joint_count = len(merged["joints"])
            if joint_count not in implicit_bind_accessors:
                identity = struct.pack(
                    "<16f",
                    1, 0, 0, 0,
                    0, 1, 0, 0,
                    0, 0, 1, 0,
                    0, 0, 0, 1,
                )
                view_index = _append_glb_payload(
                    gltf, binary, identity * joint_count,
                    f"model_{model_digest[:12]}_implicit_inverse_bind_matrices_{joint_count}",
                )
                implicit_bind_accessors[joint_count] = len(gltf["accessors"])
                gltf["accessors"].append({
                    "bufferView": view_index, "componentType": 5126,
                    "count": joint_count, "type": "MAT4",
                })
            merged["inverseBindMatrices"] = implicit_bind_accessors[joint_count]
        gltf["skins"].append(merged)
    gltf["nodes"].extend(source_nodes)
    source_scene = source.get("scenes", [{}])[source.get("scene", 0)]
    root_index = len(gltf["nodes"])
    root = {
        "name": f"model_{model_digest[:12]}",
        "children": [node + node_base for node in source_scene.get("nodes", [])],
        "extras": {
            "xenogears": {
                "model_sha256": model_digest,
                "model_member_indices": sorted({
                    context["model_member_index"]
                    for context in contexts if "model_member_index" in context
                }),
            },
        },
    }
    gear_scales = {context.get("gear_scale_psx") for context in contexts
                   if context.get("gear_scale_psx") is not None}
    if len(gear_scales) == 1:
        scale = gear_scales.pop() / 0x1000
        root["scale"] = [scale, scale, scale]
        root["extras"]["xenogears"]["gear_scale_psx"] = round(scale * 0x1000)
    gltf["nodes"].append(root)
    gltf["scenes"][0]["nodes"].append(root_index)
    for extension in source.get("extensionsUsed", []):
        if extension not in gltf["extensionsUsed"]:
            gltf["extensionsUsed"].append(extension)
    part_meshes = {}
    for index, node in enumerate(source_nodes):
        match = re.fullmatch(r"part_(\d+)", node.get("name", ""))
        if match and "mesh" in node:
            part_meshes[int(match.group(1))] = node["mesh"]
    return {"root": root_index, "part_meshes": part_meshes,
            "node_start": node_base, "node_end": root_index}


def _isolate_gltf_mesh_node(source: dict, source_binary: bytes,
                            node_index: int) -> tuple[dict, bytes]:
    source_node = source["nodes"][node_index]
    if "mesh" not in source_node or source_node.get("children") or "skin" in source_node:
        raise ValueError("standalone World model node is not an isolated mesh")
    source_mesh = source["meshes"][source_node["mesh"]]
    accessor_indices = set()
    material_indices = set()
    for primitive in source_mesh["primitives"]:
        accessor_indices.update(primitive.get("attributes", {}).values())
        if "indices" in primitive:
            accessor_indices.add(primitive["indices"])
        for target in primitive.get("targets", []):
            accessor_indices.update(target.values())
        if "material" in primitive:
            material_indices.add(primitive["material"])

    view_indices = set()
    for accessor_index in accessor_indices:
        accessor = source["accessors"][accessor_index]
        if "bufferView" in accessor:
            view_indices.add(accessor["bufferView"])
        if "sparse" in accessor:
            view_indices.add(accessor["sparse"]["indices"]["bufferView"])
            view_indices.add(accessor["sparse"]["values"]["bufferView"])

    binary = bytearray()
    views = []
    view_map = {}
    for old_index in sorted(view_indices):
        source_view = source["bufferViews"][old_index]
        start = source_view.get("byteOffset", 0)
        end = start + source_view["byteLength"]
        if source_view.get("buffer", 0) != 0 or end > len(source_binary):
            raise ValueError("standalone World model buffer view is invalid")
        offset, _ = _append_aligned(binary, source_binary[start:end])
        view = copy.deepcopy(source_view)
        view["buffer"] = 0
        view["byteOffset"] = offset
        view_map[old_index] = len(views)
        views.append(view)

    accessors = []
    accessor_map = {}
    for old_index in sorted(accessor_indices):
        accessor = copy.deepcopy(source["accessors"][old_index])
        if "bufferView" in accessor:
            accessor["bufferView"] = view_map[accessor["bufferView"]]
        if "sparse" in accessor:
            accessor["sparse"]["indices"]["bufferView"] = view_map[
                accessor["sparse"]["indices"]["bufferView"]
            ]
            accessor["sparse"]["values"]["bufferView"] = view_map[
                accessor["sparse"]["values"]["bufferView"]
            ]
        accessor_map[old_index] = len(accessors)
        accessors.append(accessor)

    materials = []
    material_map = {}
    for old_index in sorted(material_indices):
        material_map[old_index] = len(materials)
        materials.append(copy.deepcopy(source["materials"][old_index]))
    mesh = copy.deepcopy(source_mesh)
    for primitive in mesh["primitives"]:
        primitive["attributes"] = {
            name: accessor_map[index] for name, index in primitive.get("attributes", {}).items()
        }
        if "indices" in primitive:
            primitive["indices"] = accessor_map[primitive["indices"]]
        for target in primitive.get("targets", []):
            for name, index in target.items():
                target[name] = accessor_map[index]
        if "material" in primitive:
            primitive["material"] = material_map[primitive["material"]]

    node = copy.deepcopy(source_node)
    node["mesh"] = 0
    isolated = {
        "asset": copy.deepcopy(source.get("asset", {"version": "2.0"})),
        "scene": 0,
        "scenes": [{"name": source_node.get("name", "world_model"), "nodes": [0]}],
        "nodes": [node], "meshes": [mesh], "materials": materials,
        "bufferViews": views, "accessors": accessors,
        "buffers": [{"byteLength": len(binary)}],
    }
    if source.get("extensionsUsed"):
        isolated["extensionsUsed"] = copy.deepcopy(source["extensionsUsed"])
    if source.get("extensionsRequired"):
        isolated["extensionsRequired"] = copy.deepcopy(source["extensionsRequired"])
    return isolated, bytes(binary)


def encode_glb(gltf: dict, binary: bytes) -> bytes:
    gltf["buffers"] = [{"byteLength": len(binary)}]
    json_chunk = json.dumps(gltf, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
    json_chunk += b" " * (-len(json_chunk) % 4)
    binary_chunk = binary + b"\0" * (-len(binary) % 4)
    total = 12 + 8 + len(json_chunk) + 8 + len(binary_chunk)
    return (struct.pack("<III", 0x46546C67, 2, total)
            + struct.pack("<II", len(json_chunk), 0x4E4F534A) + json_chunk
            + struct.pack("<II", len(binary_chunk), 0x004E4942) + binary_chunk)


def _scene_occurrences(record: dict, scene_key: str, preferred_disc: int) -> list[dict]:
    occurrences = [
        occurrence for occurrence in record["occurrences"]
        if occurrence.get("resource_context", {}).get("scene_key") == scene_key
        or (scene_key.startswith("battling:")
            and occurrence.get("resource_context", {}).get("scene_key") == "battling:common")
    ]
    preferred = [occurrence for occurrence in occurrences if occurrence.get("disc_index") == preferred_disc]
    return preferred or occurrences


def _write_vram_upload(vram: bytearray, coverage: bytearray, x: int, y: int,
                       width_words: int, height: int, data: bytes) -> None:
    if (x < 0 or y < 0 or width_words <= 0 or height <= 0
            or x + width_words > 1024 or y + height > 512
            or len(data) != width_words * height * 2):
        return
    row_bytes = width_words * 2
    for row in range(height):
        word_start = (y + row) * 1024 + x
        byte_start = word_start * 2
        vram[byte_start:byte_start + row_bytes] = data[row * row_bytes:(row + 1) * row_bytes]
        coverage[word_start:word_start + width_words] = b"\1" * width_words


def build_scene_vram(assets: Path, dependencies: list[dict], scene_key: str,
                     preferred_disc: int) -> tuple[bytearray, bytearray]:
    vram = bytearray(1024 * 512 * 2)
    coverage = bytearray(1024 * 512)
    jobs = []
    for record in dependencies:
        if record["kind"] == "texture_image":
            for occurrence in _scene_occurrences(record, scene_key, preferred_disc):
                member = occurrence["transform_chain"][-1].get("member_index", 0)
                order = -member if scene_key.startswith("world:") else member
                jobs.append((0, occurrence.get("resource_context", {}).get("source_package_sha256", ""),
                             order, record["sha256"], "tim", record, occurrence))
        elif record["kind"] == "texture_upload":
            for occurrence in _scene_occurrences(record, scene_key, preferred_disc):
                final = occurrence["transform_chain"][-1]
                order = final.get("descriptor_index", final.get("record_index", 0))
                jobs.append((1, occurrence.get("resource_context", {}).get("source_package_sha256", ""),
                             order, record["sha256"], "upload", record, occurrence))
    seen = set()
    for _, package, order, digest, kind, record, occurrence in sorted(jobs, key=lambda item: item[:4]):
        context = occurrence.get("resource_context", {})
        identity = (package, order, digest, occurrence.get("disc_index"))
        if identity in seen:
            continue
        seen.add(identity)
        if kind == "tim":
            tim = parse_tim((assets / record["path"]).read_bytes())
            if tim.palettes:
                palette_size = len(tim.palettes[0])
                palette_data = struct.pack(
                    f"<{sum(len(palette) for palette in tim.palettes)}H",
                    *(color for palette in tim.palettes for color in palette),
                )
                _write_vram_upload(
                    vram, coverage, tim.clut_x or 0, tim.clut_y or 0,
                    palette_size, len(tim.palettes), palette_data,
                )
            _write_vram_upload(
                vram, coverage, tim.image_x, tim.image_y,
                tim.width_words, tim.height, tim.image_data,
            )
        else:
            origin = context.get("resolved_origin")
            if origin is None:
                stored = context.get("stored_origin", {})
                relative = context.get("relative_origin", {})
                origin = {
                    "x_words": stored.get("x_words", 0) + relative.get("x_words", 0),
                    "y": stored.get("y", 0) + relative.get("y", 0),
                }
            _write_vram_upload(
                vram, coverage, origin["x_words"], origin["y"],
                context["width_words"], context["height"],
                (assets / record["path"]).read_bytes(),
            )
    return vram, coverage


def _bake_texture_page(vram: bytearray, coverage: bytearray, tpage: int,
                       clut: int | None) -> bytes | None:
    depth = (tpage >> 7) & 3
    if depth > 2 or depth < 2 and clut is None:
        return None
    page_x = (tpage & 0x0F) * 64
    page_y = ((tpage >> 4) & 1) * 256
    clut_x = ((clut or 0) & 0x3F) * 16
    clut_y = (clut or 0) >> 6
    palette_size = (16, 256, 0)[depth]
    if palette_size and not any(coverage[clut_y * 1024 + clut_x:clut_y * 1024 + clut_x + palette_size]):
        return None
    palette = []
    for index in range(palette_size):
        at = (clut_y * 1024 + clut_x + index) * 2
        palette.append(bytes(_rgb555(u16(vram, at), transparent_zero=True)))
    words_per_row = (64, 128, 256)[depth]
    valid_words = min(words_per_row, max(0, 1024 - page_x))
    if not any(
        any(coverage[(page_y + y) * 1024 + page_x:
                     (page_y + y) * 1024 + page_x + valid_words])
        for y in range(256)
    ):
        return None
    pixels = bytearray()
    for y in range(256):
        row_offset = ((page_y + y) * 1024 + page_x) * 2
        values = struct.iter_unpack("<H", vram[row_offset:row_offset + valid_words * 2])
        for value, in values:
            if depth == 0:
                pixels.extend(palette[value & 0x0F])
                pixels.extend(palette[value >> 4 & 0x0F])
                pixels.extend(palette[value >> 8 & 0x0F])
                pixels.extend(palette[value >> 12])
            elif depth == 1:
                pixels.extend(palette[value & 0xFF])
                pixels.extend(palette[value >> 8])
            else:
                pixels.extend(_rgb555(value, transparent_zero=True))
        pixels.extend(b"\0" * ((words_per_row - valid_words) * (16 >> depth)))
    return bytes(pixels)


def bind_scene_material_textures(gltf: dict, binary: bytearray, vram: bytearray,
                                 coverage: bytearray,
                                 png_cache: dict[tuple[int, int | None], bytes | None] | None = None) -> int:
    baked: dict[tuple[int, int | None], int] = {}
    sampler_index = None
    bindings = []
    for material_index, material in enumerate(gltf["materials"]):
        extras = material.get("extras", {})
        family = extras.get("ps1_family")
        tpage = extras.get("ps1_tpage")
        clut = extras.get("ps1_clut")
        if (family not in TEXTURED_FAMILIES or family == 16 or tpage is None
                or extras.get("texture_state_status") != "serialized_state_known"):
            continue
        key = (tpage, clut)
        if key not in baked:
            png = png_cache.get(key) if png_cache is not None and key in png_cache else None
            cache_hit = png_cache is not None and key in png_cache
            if not cache_hit:
                pixels = _bake_texture_page(vram, coverage, tpage, clut)
                png = None if pixels is None else encode_png_rgba(256, 256, pixels, 1)
                if png_cache is not None:
                    png_cache[key] = png
            if png is None:
                baked[key] = -1
                continue
            view = _append_glb_payload(gltf, binary, png, f"baked_tpage_{tpage:04x}_clut_{clut or 0:04x}")
            image_index = len(gltf["images"])
            gltf["images"].append({
                "name": f"baked_tpage_{tpage:04x}_clut_{clut or 0:04x}",
                "bufferView": view, "mimeType": "image/png",
            })
            if sampler_index is None:
                sampler_index = len(gltf.setdefault("samplers", []))
                gltf["samplers"].append({
                    "magFilter": 9728, "minFilter": 9728, "wrapS": 33071, "wrapT": 33071,
                })
            texture_index = len(gltf["textures"])
            gltf["textures"].append({"source": image_index, "sampler": sampler_index})
            baked[key] = texture_index
        texture_index = baked[key]
        if texture_index < 0:
            continue
        pbr = material.setdefault("pbrMetallicRoughness", {})
        pbr["baseColorTexture"] = {"index": texture_index}
        pbr["baseColorFactor"] = [1, 1, 1, 1]
        if extras.get("semitransparent"):
            material["alphaMode"] = "BLEND"
        else:
            material["alphaMode"] = "MASK"
            material["alphaCutoff"] = 0.5
        bindings.append({
            "material": material_index, "texture": texture_index,
            "tpage": tpage, "clut": clut,
        })
    for mesh in gltf["meshes"]:
        for primitive in mesh["primitives"]:
            material = gltf["materials"][primitive.get("material", 0)]
            if "baseColorTexture" in material.get("pbrMetallicRoughness", {}):
                primitive["attributes"].pop("COLOR_0", None)
    gltf["extensions"]["XENOGEARS_resource_bundle"]["materialTextureBindings"] = bindings
    return len(bindings)


def _scene_accessor(gltf: dict, binary: bytearray, payload: bytes, component_type: int,
                    count: int, kind: str, name: str,
                    bounds: tuple[list[float], list[float]] | None = None,
                    target: int | None = None) -> int:
    view = _append_glb_payload(gltf, binary, payload, name)
    if target is not None:
        gltf["bufferViews"][view]["target"] = target
    accessor = {"bufferView": view, "componentType": component_type,
                "count": count, "type": kind}
    if bounds is not None:
        accessor["min"], accessor["max"] = bounds
    index = len(gltf["accessors"])
    gltf["accessors"].append(accessor)
    return index


def _scene_material(gltf: dict, name: str, color: list[float], *,
                    lit_materials: bool = False, **extras: object) -> int:
    for index, material in enumerate(gltf["materials"]):
        if material.get("name") == name:
            return index
    index = len(gltf["materials"])
    material = {
        "name": name,
        "pbrMetallicRoughness": {
            "baseColorFactor": color, "metallicFactor": 0, "roughnessFactor": 1,
        },
        "doubleSided": True, "alphaMode": "BLEND" if color[3] < 1 else "OPAQUE",
        "extras": extras,
    }
    if not lit_materials:
        material["extensions"] = {"KHR_materials_unlit": {}}
        if "KHR_materials_unlit" not in gltf["extensionsUsed"]:
            gltf["extensionsUsed"].append("KHR_materials_unlit")
    gltf["materials"].append(material)
    return index


def _indexed_vertex_normals(positions: list[tuple[float, float, float]],
                            indices: list[int]) -> list[tuple[float, float, float]]:
    accumulated = [[0.0, 0.0, 0.0] for _ in positions]
    for at in range(0, len(indices) - 2, 3):
        first, second, third = (positions[indices[at + offset]] for offset in range(3))
        edge1 = tuple(second[index] - first[index] for index in range(3))
        edge2 = tuple(third[index] - first[index] for index in range(3))
        cross = (
            edge1[1] * edge2[2] - edge1[2] * edge2[1],
            edge1[2] * edge2[0] - edge1[0] * edge2[2],
            edge1[0] * edge2[1] - edge1[1] * edge2[0],
        )
        for vertex_index in indices[at:at + 3]:
            for component in range(3):
                accumulated[vertex_index][component] += cross[component]
    normals = []
    for normal in accumulated:
        magnitude = math.sqrt(sum(component * component for component in normal))
        normals.append(
            tuple(component / magnitude for component in normal)
            if magnitude else (0.0, 1.0, 0.0)
        )
    return normals


def _add_indexed_mesh(gltf: dict, binary: bytearray, name: str,
                       positions: list[tuple[float, float, float]], indices: list[int],
                       material: int, *, texcoords: list[tuple[float, float]] | None = None,
                       extras: dict | None = None,
                       generate_missing_normals: bool = False) -> int:
    position_accessor = _scene_accessor(
        gltf, binary,
        struct.pack(f"<{len(positions) * 3}f", *(value for position in positions for value in position)),
        5126, len(positions), "VEC3", f"{name}_positions", _bounds(positions), 34962,
    )
    attributes = {"POSITION": position_accessor}
    if generate_missing_normals:
        normals = _indexed_vertex_normals(positions, indices)
        attributes["NORMAL"] = _scene_accessor(
            gltf, binary,
            struct.pack(f"<{len(normals) * 3}f", *(value for normal in normals for value in normal)),
            5126, len(normals), "VEC3", f"{name}_normals", target=34962,
        )
    if texcoords:
        attributes["TEXCOORD_0"] = _scene_accessor(
            gltf, binary,
            struct.pack(f"<{len(texcoords) * 2}f", *(value for uv in texcoords for value in uv)),
            5126, len(texcoords), "VEC2", f"{name}_texcoords", target=34962,
        )
    component_type = 5123 if max(indices, default=0) <= 0xFFFF else 5125
    code = "H" if component_type == 5123 else "I"
    index_accessor = _scene_accessor(
        gltf, binary, struct.pack(f"<{len(indices)}{code}", *indices),
        component_type, len(indices), "SCALAR", f"{name}_indices", target=34963,
    )
    mesh_index = len(gltf["meshes"])
    primitive = {"attributes": attributes, "indices": index_accessor,
                 "material": material, "mode": 4}
    if extras:
        primitive["extras"] = extras
    gltf["meshes"].append({"name": name, "primitives": [primitive]})
    return mesh_index


def _collision_mesh(gltf: dict, binary: bytearray, data: bytes, name: str,
                     unit_scale: float, triangle_at: int, triangle_count: int,
                     vertex_at: int, triangle_stride: int = 0x0E, *,
                     lit_materials: bool = False,
                     generate_missing_normals: bool = False) -> int | None:
    if triangle_count <= 0 or triangle_at < 0 or vertex_at < 0:
        return None
    triangles = []
    greatest = -1
    for index in range(triangle_count):
        at = triangle_at + index * triangle_stride
        if at + 6 > len(data):
            return None
        triangle = struct.unpack_from("<HHH", data, at)
        greatest = max(greatest, *triangle)
        triangles.extend(triangle)
    if greatest < 0 or vertex_at + (greatest + 1) * 8 > len(data):
        return None
    positions = []
    for index in range(greatest + 1):
        x, y, z, _ = struct.unpack_from("<hhhh", data, vertex_at + index * 8)
        positions.append((x * unit_scale, -y * unit_scale, -z * unit_scale))
    material = _scene_material(gltf, "collision_debug", [0.1, 0.8, 0.25, 0.35],
                               lit_materials=lit_materials,
                               xenogears_role="collision_debug")
    return _add_indexed_mesh(gltf, binary, name, positions, triangles, material,
                             extras={"xenogears_role": "collision"},
                             generate_missing_normals=generate_missing_normals)


def _add_field_walkmesh(gltf: dict, binary: bytearray, data: bytes,
                        unit_scale: float, *, lit_materials: bool = False,
                        generate_missing_normals: bool = False) -> list[int]:
    if len(data) < 0x38 or not 1 <= u32(data, 0) <= 4:
        return []
    meshes = []
    for layer in range(u32(data, 0)):
        triangle_bytes = u32(data, 4 + layer * 4)
        triangle_at = u32(data, 0x18 + layer * 8)
        vertex_at = u32(data, 0x1C + layer * 8)
        if triangle_bytes % 0x0E:
            continue
        mesh = _collision_mesh(
            gltf, binary, data, f"field_walkmesh_layer_{layer:02d}", unit_scale,
            triangle_at, triangle_bytes // 0x0E, vertex_at,
            lit_materials=lit_materials,
            generate_missing_normals=generate_missing_normals,
        )
        if mesh is not None:
            meshes.append(mesh)
    return meshes


def _add_world_collision(gltf: dict, binary: bytearray, data: bytes,
                         unit_scale: float, *, lit_materials: bool = False,
                         generate_missing_normals: bool = False) -> dict[int, int]:
    if len(data) < 8:
        return {}
    count = u32(data, 0)
    if count <= 0 or count > MAX_MEMBERS or 4 + count * 4 > len(data):
        return {}
    meshes = {}
    for model_index in range(count):
        at = u32(data, 4 + model_index * 4)
        if at + 8 > len(data):
            continue
        triangle_count, vertex_offset = struct.unpack_from("<II", data, at)
        mesh = _collision_mesh(
            gltf, binary, data, f"world_collision_{model_index:04d}", unit_scale,
            at + 8, triangle_count, at + vertex_offset,
            lit_materials=lit_materials,
            generate_missing_normals=generate_missing_normals,
        )
        if mesh is not None:
            meshes[model_index] = mesh
    return meshes


def _terrain_uvs(base_u: int, base_v: int, rotation: int) -> tuple[tuple[int, int], ...]:
    corners = ((base_u, base_v), (base_u + 15, base_v),
               (base_u, base_v + 15), (base_u + 15, base_v + 15))
    orders = ((0, 1, 2, 3), (1, 0, 3, 2), (2, 3, 0, 1), (3, 2, 1, 0))
    return tuple(corners[index] for index in orders[rotation & 3])


def _add_world_terrain(gltf: dict, binary: bytearray, data: bytes,
                       unit_scale: float, *, lit_materials: bool = False,
                       generate_missing_normals: bool = False) -> list[int]:
    if len(data) != 256 * 0x800:
        return []
    chunk_meshes: dict[bytes, int | None] = {}
    nodes = []
    for slot_index in range(256):
        chunk = data[slot_index * 0x800:(slot_index + 1) * 0x800]
        key = chunk[:0x710]
        if key in chunk_meshes:
            mesh_index = chunk_meshes[key]
        else:
            grouped: dict[tuple[int, int], tuple[list, list, list]] = {}
            for quadrant in range(4):
                samples_at = quadrant * 0x144
                qx, qz = quadrant & 1, quadrant >> 1
                for z in range(8):
                    for x in range(8):
                        words = (
                            u32(chunk, samples_at + (z * 9 + x) * 4),
                            u32(chunk, samples_at + (z * 9 + x + 1) * 4),
                            u32(chunk, samples_at + ((z + 1) * 9 + x) * 4),
                            u32(chunk, samples_at + ((z + 1) * 9 + x + 1) * 4),
                        )
                        state = words[0]
                        page = state >> 8 & 7
                        if page > 5:
                            continue
                        tpage = (0x88, 0x8A, 0x8C, 0x8E, 0x96, 0x98)[page]
                        clut = (480 if page < 3 else 481) << 6
                        positions, texcoords, indices = grouped.setdefault(
                            (tpage, clut), ([], [], []),
                        )
                        base = len(positions)
                        x0 = (qx * 8 + x) * 0x80
                        z0 = (qz * 8 + z) * 0x80
                        coordinates = ((x0, z0), (x0 + 0x80, z0),
                                       (x0, z0 + 0x80), (x0 + 0x80, z0 + 0x80))
                        for word, (px, pz) in zip(words, coordinates):
                            height = struct.unpack("b", bytes((word & 0xFF,)))[0] * 8
                            positions.append((px * unit_scale, -height * unit_scale,
                                              -pz * unit_scale))
                        uv = _terrain_uvs((state >> 16 & 15) * 16,
                                          (state >> 20 & 15) * 16, state >> 13 & 3)
                        texcoords.extend((u / 256.0, v / 256.0) for u, v in uv)
                        if state & 0x8000:
                            indices.extend((base, base + 3, base + 2,
                                            base + 1, base + 3, base))
                        else:
                            indices.extend((base, base + 1, base + 2,
                                            base + 1, base + 3, base + 2))
            primitives = []
            mesh_index = len(gltf["meshes"])
            for group_index, ((tpage, clut), (positions, texcoords, indices)) in enumerate(grouped.items()):
                material = _scene_material(
                    gltf, f"world_terrain_tpage_{tpage:04x}_clut_{clut:04x}", [1, 1, 1, 1],
                    lit_materials=lit_materials,
                    ps1_family=9, ps1_tpage=tpage, ps1_clut=clut,
                    texture_depth=1, blend_mode=(tpage >> 5) & 3,
                    raw_texture=False, semitransparent=False,
                    texture_state_status="serialized_state_known",
                    texture_binding_status="preserved_ps1_state_not_baked",
                    xenogears_role="world_terrain",
                )
                temporary = _add_indexed_mesh(
                    gltf, binary, f"world_terrain_chunk_{len(chunk_meshes):03d}_{group_index}",
                    positions, indices, material, texcoords=texcoords,
                    extras={"xenogears_role": "world_terrain"},
                    generate_missing_normals=generate_missing_normals,
                )
                primitives.extend(gltf["meshes"].pop(temporary)["primitives"])
            if primitives:
                gltf["meshes"].append({
                    "name": f"world_terrain_chunk_{len(chunk_meshes):03d}",
                    "primitives": primitives,
                })
            else:
                mesh_index = None
            chunk_meshes[key] = mesh_index
        if mesh_index is None:
            continue
        node_index = len(gltf["nodes"])
        sx, sz = slot_index & 15, slot_index >> 4
        gltf["nodes"].append({
            "name": f"world_terrain_slot_{slot_index:03d}", "mesh": mesh_index,
            "translation": [sx * 0x800 * unit_scale, 0, -sz * 0x800 * unit_scale],
            "extras": {"xenogears": {"slot": [sx, sz]}},
        })
        nodes.append(node_index)
    return nodes


def _add_battling_heightfield(gltf: dict, binary: bytearray, data: bytes,
                               unit_scale: float, *, lit_materials: bool = False,
                               generate_missing_normals: bool = False) -> int | None:
    if len(data) < 128 * 128 * 4:
        return None
    positions = []
    for z in range(128):
        for x in range(128):
            height = s16(data, (z * 128 + x) * 4) * 12
            positions.append((x * 0x100 * unit_scale, -height * unit_scale,
                              -z * 0x100 * unit_scale))
    indices = []
    for z in range(127):
        for x in range(127):
            base = z * 128 + x
            indices.extend((base, base + 1, base + 128,
                            base + 1, base + 129, base + 128))
    material = _scene_material(gltf, "battling_terrain", [0.35, 0.38, 0.42, 1],
                               lit_materials=lit_materials,
                               xenogears_role="terrain")
    return _add_indexed_mesh(gltf, binary, "battling_heightfield", positions, indices,
                             material, extras={"xenogears_role": "terrain"},
                             generate_missing_normals=generate_missing_normals)


def _clone_node_tree(gltf: dict, source_index: int) -> int:
    clone = copy.deepcopy(gltf["nodes"][source_index])
    clone["children"] = [_clone_node_tree(gltf, child)
                         for child in clone.get("children", [])]
    index = len(gltf["nodes"])
    gltf["nodes"].append(clone)
    return index


def _add_mecha_sequence_animations(gltf: dict, binary: bytearray, data: bytes,
                                   merged: dict, model_digest: str,
                                   unit_scale: float,
                                   decoded_cache: dict[tuple, tuple[list[dict], list[dict]]] | None = None) -> dict:
    runtime_nodes = {0: merged["node_start"]}
    runtime_visibility_nodes: dict[int, list[int]] = {}
    for node_index in range(merged["node_start"], merged["node_end"]):
        name = gltf["nodes"][node_index].get("name", "")
        match = re.fullmatch(r"bone_(\d{4})", name)
        if match:
            runtime_nodes[int(match.group(1)) + 1] = node_index
        visibility_match = re.fullmatch(r"bone_(\d{4})_visibility_part_\d{4}", name)
        if visibility_match:
            runtime_visibility_nodes.setdefault(
                int(visibility_match.group(1)) + 1, [],
            ).append(node_index)
    if len(runtime_nodes) == 1:
        return {"sequence_count": 0, "dynamic_sequence_count": 0, "frame_count": 0,
                "vm_action_count": 0, "vm_frame_count": 0,
                "unsupported_vm_opcodes": [], "skipped_vm_action_count": 0}
    node_to_runtime = {node: runtime_index for runtime_index, node in runtime_nodes.items()}
    runtime_children = {
        runtime_index: [
            node_to_runtime[child] for child in gltf["nodes"][node_index].get("children", [])
            if child in node_to_runtime
        ]
        for runtime_index, node_index in runtime_nodes.items()
    }
    cache_key = (
        sha256(data), len(runtime_nodes) - 1,
        tuple((index, tuple(children)) for index, children in sorted(runtime_children.items())),
    )
    decoded = None if decoded_cache is None else decoded_cache.get(cache_key)
    if decoded is None:
        sequences = parse_mecha_animation_sequences(data, len(runtime_nodes) - 1)
        all_vm_actions = evaluate_mecha_vm_scripts(
            data, sequences, len(runtime_nodes) - 1, runtime_children,
        )
        if decoded_cache is not None:
            decoded_cache[cache_key] = (sequences, all_vm_actions)
    else:
        sequences, all_vm_actions = decoded
    dynamic_count = total_frames = 0
    for sequence in sequences:
        sequence_index = sequence["sequence_index"]
        frames = sequence["frames"]
        sample_count = len(frames)
        total_frames += sample_count
        if not sequence["static"]:
            dynamic_count += 1
        times = [index / MECHA_ANIMATION_FPS for index in range(sample_count)]
        time_accessor = _scene_accessor(
            gltf, binary, struct.pack(f"<{sample_count}f", *times), 5126,
            sample_count, "SCALAR",
            f"animation_{model_digest[:12]}_{sequence_index:03d}_time",
            ([times[0]], [times[-1]]),
        )
        samplers = []
        channels = []
        for runtime_index, node_index in runtime_nodes.items():
            if runtime_index >= len(frames[0]):
                continue
            translations = []
            rotations = []
            previous_rotation = None
            for frame in frames:
                transform = frame[runtime_index]
                trs = _psx_trs(
                    transform["rotation_psx"], transform["translation_psx"], unit_scale,
                )
                quaternion = trs["rotation"]
                if (previous_rotation is not None
                        and sum(left * right for left, right in zip(previous_rotation, quaternion)) < 0):
                    quaternion = [-value for value in quaternion]
                translations.extend(trs["translation"])
                rotations.extend(quaternion)
                previous_rotation = quaternion
            for path, values, kind, width in (
                    ("translation", translations, "VEC3", 3),
                    ("rotation", rotations, "VEC4", 4)):
                output_accessor = _scene_accessor(
                    gltf, binary, struct.pack(f"<{len(values)}f", *values),
                    5126, len(values) // width, kind,
                    f"animation_{model_digest[:12]}_{sequence_index:03d}_runtime_{runtime_index:04d}_{path}",
                )
                sampler = len(samplers)
                samplers.append({
                    "input": time_accessor, "output": output_accessor,
                    "interpolation": "STEP" if sequence["static"] else "LINEAR",
                })
                channels.append({
                    "sampler": sampler,
                    "target": {"node": node_index, "path": path},
                })
        if channels:
            gltf.setdefault("animations", []).append({
                "name": f"model_{model_digest[:12]}_sequence_{sequence_index:03d}",
                "samplers": samplers, "channels": channels,
                "extras": {"xenogears": {
                    "source_sequence_index": sequence_index,
                    "source_frame_count": sequence["frame_count"],
                    "source_fps": MECHA_ANIMATION_FPS,
                    "source_static": sequence["static"],
                    "source_empty_entry": sequence.get("fallback_empty", False),
                    "rotation_track_count": sequence["rotation_tracks"],
                    "translation_track_count": sequence["translation_tracks"],
                    "conversion": "decoded_mecha_track_timeline",
                }},
            })
    unsupported_opcodes = {
        opcode for action in all_vm_actions for opcode in action["unsupported_opcodes"]
    }
    vm_actions = [action for action in all_vm_actions
                  if not action["unsupported_opcodes"] and not action["truncated"]]
    vm_frames = 0
    for action in vm_actions:
        frames = action["frames"]
        sample_count = len(frames)
        vm_frames += sample_count
        times = [index / MECHA_ANIMATION_FPS for index in range(sample_count)]
        time_accessor = _scene_accessor(
            gltf, binary, struct.pack(f"<{sample_count}f", *times), 5126,
            sample_count, "SCALAR",
            f"animation_{model_digest[:12]}_vm_{action['script_index']:03d}_time",
            ([times[0]], [times[-1]]),
        )
        samplers = []
        channels = []

        def add_channel(node_index: int, path: str, values: list[float], width: int,
                        interpolation: str) -> None:
            output = _scene_accessor(
                gltf, binary, struct.pack(f"<{len(values)}f", *values), 5126,
                len(values) // width, f"VEC{width}",
                f"animation_{model_digest[:12]}_vm_{action['script_index']:03d}_{node_index}_{path}",
            )
            sampler = len(samplers)
            samplers.append({"input": time_accessor, "output": output,
                             "interpolation": interpolation})
            channels.append({"sampler": sampler,
                             "target": {"node": node_index, "path": path}})

        for runtime_index, node_index in runtime_nodes.items():
            translations = []
            rotations = []
            scale_values = []
            previous_rotation = None
            for frame in frames:
                transform = frame["pose"][runtime_index]
                trs = _psx_trs(transform["rotation_psx"], transform["translation_psx"],
                               unit_scale)
                quaternion = trs["rotation"]
                if (previous_rotation is not None
                        and sum(left * right for left, right in zip(previous_rotation, quaternion)) < 0):
                    quaternion = [-value for value in quaternion]
                translations.extend(trs["translation"])
                rotations.extend(quaternion)
                scale_values.extend(value / 0x1000 for value in frame["scales"][runtime_index])
                previous_rotation = quaternion
            add_channel(node_index, "translation", translations, 3, "LINEAR")
            add_channel(node_index, "rotation", rotations, 4, "LINEAR")
            if any(abs(value - 1.0) > 1e-9 for value in scale_values):
                add_channel(node_index, "scale", scale_values, 3, "LINEAR")
        if any(not all(frame["visible"][1:]) for frame in frames):
            for runtime_index, visibility_nodes in runtime_visibility_nodes.items():
                values = [component for frame in frames
                          for component in ([1.0, 1.0, 1.0]
                                            if frame["visible"][runtime_index]
                                            else [0.0, 0.0, 0.0])]
                for node_index in visibility_nodes:
                    add_channel(node_index, "scale", values, 3, "STEP")
        gltf.setdefault("animations", []).append({
            "name": f"model_{model_digest[:12]}_vm_action_{action['script_index']:03d}",
            "samplers": samplers, "channels": channels,
            "extras": {"xenogears": {
                "source_vm_script_index": action["script_index"],
                "source_fps": MECHA_ANIMATION_FPS,
                "conversion": "offline_mecha_vm_evaluation",
                "unsupported_opcodes": action["unsupported_opcodes"],
                "truncated": action["truncated"],
                "initial_state": "sequence_000_initial_pose",
            }},
        })
    return {
        "sequence_count": len(sequences), "dynamic_sequence_count": dynamic_count,
        "frame_count": total_frames, "vm_action_count": len(vm_actions),
        "vm_frame_count": vm_frames, "unsupported_vm_opcodes": sorted(unsupported_opcodes),
        "skipped_vm_action_count": len(all_vm_actions) - len(vm_actions),
    }


def _dependency_with_format(dependencies: list[dict], scene_key: str,
                            format_name: str) -> tuple[dict, dict] | None:
    for record in dependencies:
        for context in _catalog_dependency_contexts(record, scene_key, ""):
            if context.get("format") == format_name:
                return record, context
    return None


def _new_catalog_gltf(name: str) -> dict:
    return {
        "asset": {"version": "2.0", "generator": "Xenogears model extractor"},
        "extensionsUsed": ["XENOGEARS_resource_bundle"],
        "scene": 0, "scenes": [{"name": name, "nodes": []}],
        "nodes": [], "meshes": [], "materials": [], "bufferViews": [],
        "accessors": [], "images": [], "textures": [], "skins": [], "animations": [],
        "extensions": {"XENOGEARS_resource_bundle": {
            "version": 1, "scene_key": name, "resources": [],
        }},
    }


def _prepare_catalog_gltf(gltf: dict) -> None:
    identity_matrix = [
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    ]
    for node in gltf["nodes"]:
        if not node.get("children"):
            node.pop("children", None)
        if node.get("matrix") == identity_matrix:
            node.pop("matrix")
    for key in ("accessors", "animations", "bufferViews", "images", "materials",
                "meshes", "samplers", "skins", "textures"):
        if not gltf.get(key):
            gltf.pop(key, None)


def build_scene_catalog(catalog: Path, assets: Path, records: list[dict],
                        unit_scale: float, *, lit_materials: bool = False,
                        generate_missing_normals: bool = False) -> dict:
    catalog.mkdir(parents=True, exist_ok=True)
    models_by_scene: dict[str, dict[str, dict]] = {}
    dependencies_by_scene: dict[str, set[tuple[str, str]]] = {}
    record_by_key = {(record["kind"], record["sha256"]): record for record in records}
    for record in records:
        if record["kind"] == "model":
            for occurrence in record["occurrences"]:
                context = occurrence.get("resource_context", {})
                scene_key = context.get("scene_key")
                if scene_key is None:
                    continue
                model = models_by_scene.setdefault(scene_key, {}).setdefault(record["sha256"], {
                    "record": record, "contexts": [], "occurrences": [],
                })
                model["contexts"].append(context)
                model["occurrences"].append(occurrence)
        else:
            key = (record["kind"], record["sha256"])
            for occurrence in record["occurrences"]:
                scene_key = occurrence.get("resource_context", {}).get("scene_key")
                if scene_key is not None:
                    dependencies_by_scene.setdefault(scene_key, set()).add(key)

    scene_records = []
    individual_glb_cache: dict[tuple, tuple[Path, int]] = {}
    for scene_key, scene_models in sorted(models_by_scene.items()):
        dependency_keys = set(dependencies_by_scene.get(scene_key, set()))
        if scene_key.startswith("battling:"):
            dependency_keys.update(dependencies_by_scene.get("battling:common", set()))
        dependencies = [record_by_key[key] for key in sorted(dependency_keys)]
        preferred_disc = min(
            occurrence["disc_index"]
            for model in scene_models.values() for occurrence in model["occurrences"]
        )
        scene_occurrence = min(
            (occurrence for model in scene_models.values() for occurrence in model["occurrences"]),
            key=lambda occurrence: (
                occurrence["disc_index"] != preferred_disc,
                occurrence["disc_index"], occurrence["fat_index"],
            ),
        )
        scene_fat_index = scene_occurrence["fat_index"]
        scene_path = _catalog_scene_path(scene_key, scene_fat_index)
        vram, coverage = build_scene_vram(assets, dependencies, scene_key, preferred_disc)
        vram_digest = hashlib.sha256()
        vram_digest.update(vram)
        vram_digest.update(coverage)
        vram_signature = vram_digest.digest()
        texture_png_cache: dict[tuple[int, int | None], bytes | None] = {}
        decoded_animation_cache: dict[tuple, tuple[list[dict], list[dict]]] = {}
        gltf = _new_catalog_gltf(scene_key)
        binary = bytearray()
        model_entries = []
        merged_models = {}
        scene_model_hashes = set(scene_models)
        for model_digest, model in sorted(scene_models.items()):
            model_record = model["record"]
            skeletons = []
            for dependency in dependencies:
                if dependency["kind"] != "skeleton":
                    continue
                contexts = _catalog_dependency_contexts(dependency, scene_key, model_digest)
                if any(context.get("model_sha256") == model_digest for context in contexts):
                    skeletons.append(dependency)
            source_gltf_path = assets / model_record["gltf_path"]
            if skeletons:
                assembly = next((
                    item for item in skeletons[0]["assemblies"]
                    if item["model_sha256"] == model_digest
                ), None)
                if assembly is not None:
                    source_gltf_path = assets / assembly["gltf_path"]
            source_gltf = json.loads(source_gltf_path.read_text(encoding="utf-8"))
            source_binary = (assets / model_record["binary_path"]).read_bytes()
            model_source_data = (assets / model_record["source_path"]).read_bytes()
            merged_models[model_digest] = _merge_gltf_model(
                gltf, binary, source_gltf, source_binary, model_digest, model["contexts"],
            )
            animation = next((
                dependency for dependency in dependencies
                if dependency["kind"] == "animation_container"
                and any(context.get("model_sha256") == model_digest
                        for context in _catalog_dependency_contexts(
                            dependency, scene_key, model_digest,
                        ))
            ), None)
            animation_stats = {"sequence_count": 0, "dynamic_sequence_count": 0,
                               "frame_count": 0, "vm_action_count": 0,
                               "vm_frame_count": 0, "unsupported_vm_opcodes": [],
                               "skipped_vm_action_count": 0}
            animation_data = None if animation is None else (assets / animation["path"]).read_bytes()
            if animation is not None:
                animation_stats = _add_mecha_sequence_animations(
                    gltf, binary, animation_data, merged_models[model_digest],
                    model_digest, unit_scale, decoded_animation_cache,
                )
            source_view = _append_glb_payload(
                gltf, binary, model_source_data, f"model_source_{model_digest[:12]}",
            )
            gltf["extensions"]["XENOGEARS_resource_bundle"]["resources"].append({
                "kind": "model_source", "sha256": model_digest, "bufferView": source_view,
                "mimeType": "application/x-xenogears-model",
            })
            model_occurrence = min(
                model["occurrences"],
                key=lambda occurrence: (
                    occurrence["disc_index"] != preferred_disc,
                    occurrence["disc_index"], occurrence["fat_index"],
                ),
            )
            model_fat_index = model_occurrence["fat_index"]
            associated_dependencies = []
            for dependency in dependencies:
                if dependency["kind"] in {"texture_image", "texture_upload"}:
                    continue
                contexts = _catalog_dependency_contexts(dependency, scene_key, model_digest)
                if any(context.get("model_sha256") == model_digest for context in contexts):
                    associated_dependencies.append(dependency)
            member_indices = tuple(sorted({
                context["model_member_index"]
                for context in model["contexts"] if "model_member_index" in context
            }))
            gear_scales = tuple(sorted({
                context["gear_scale_psx"] for context in model["contexts"]
                if context.get("gear_scale_psx") is not None
            }))
            individual_variants = [(None, source_gltf, source_binary)]
            if scene_key.startswith("world:"):
                individual_variants = []
                for source_node_index, source_node in enumerate(source_gltf.get("nodes", [])):
                    match = re.fullmatch(r"part_(\d+)", source_node.get("name", ""))
                    if match and "mesh" in source_node:
                        isolated_gltf, isolated_binary = _isolate_gltf_mesh_node(
                            source_gltf, source_binary, source_node_index,
                        )
                        individual_variants.append(
                            (int(match.group(1)), isolated_gltf, isolated_binary)
                        )
                if not individual_variants:
                    raise ValueError("World model archive has no standalone model parts")

            use_models_directory = len(scene_models) > 1 or len(individual_variants) > 1
            for world_model_index, individual_source_gltf, individual_source_binary in individual_variants:
                model_directory = f"model-{model_fat_index:04d}-{model_digest}"
                if world_model_index is not None:
                    model_directory += f"-part-{world_model_index:04d}"
                individual_path = Path(model_directory) / "model.glb"
                if use_models_directory:
                    individual_path = Path("models") / individual_path
                individual_destination = catalog / scene_path / individual_path
                individual_cache_key = (
                    model_digest, source_gltf_path.as_posix(), world_model_index,
                    None if animation is None else animation["sha256"],
                    member_indices, gear_scales, vram_signature,
                    tuple((dependency["kind"], dependency["sha256"])
                          for dependency in associated_dependencies),
                )
                cached_individual = individual_glb_cache.get(individual_cache_key)
                if cached_individual is not None:
                    cached_path, individual_bindings = cached_individual
                    individual_destination.parent.mkdir(parents=True, exist_ok=True)
                    os.link(cached_path, individual_destination)
                else:
                    individual_gltf = _new_catalog_gltf(f"model:{model_digest}")
                    individual_binary = bytearray()
                    individual_model = _merge_gltf_model(
                        individual_gltf, individual_binary, individual_source_gltf,
                        individual_source_binary, model_digest, model["contexts"],
                    )
                    if world_model_index is not None:
                        root = individual_gltf["nodes"][individual_model["root"]]
                        root["name"] = f"world_model_{world_model_index:04d}"
                        root["extras"]["xenogears"]["world_model_index"] = world_model_index
                    if animation is not None:
                        _add_mecha_sequence_animations(
                            individual_gltf, individual_binary, animation_data,
                            individual_model, model_digest, unit_scale, decoded_animation_cache,
                        )
                    individual_source_view = _append_glb_payload(
                        individual_gltf, individual_binary, model_source_data,
                        f"model_source_{model_digest[:12]}",
                    )
                    individual_resources = individual_gltf["extensions"]["XENOGEARS_resource_bundle"]["resources"]
                    individual_resources.append({
                        "kind": "model_source", "sha256": model_digest,
                        "bufferView": individual_source_view,
                        "mimeType": "application/x-xenogears-model",
                    })
                    for dependency in associated_dependencies:
                        view = _append_glb_payload(
                            individual_gltf, individual_binary,
                            (assets / dependency["path"]).read_bytes(),
                            f"{dependency['kind']}_{dependency['sha256'][:12]}",
                        )
                        individual_resources.append({
                            "kind": dependency["kind"], "sha256": dependency["sha256"],
                            "bufferView": view, "mimeType": "application/octet-stream",
                        })
                    individual_extension = individual_gltf["extensions"]["XENOGEARS_resource_bundle"]
                    individual_extension["animation_policy"] = (
                        "All sequence tracks are decoded as timelines; deterministic VM scripts are evaluated as actions, and context-dependent scripts remain embedded."
                    )
                    individual_extension["texture_policy"] = (
                        "Textures required by this model are baked from the scene VRAM into standard embedded PNG images."
                    )
                    individual_bindings = bind_scene_material_textures(
                        individual_gltf, individual_binary, vram, coverage, texture_png_cache,
                    )
                    _prepare_catalog_gltf(individual_gltf)
                    write_if_changed(
                        individual_destination,
                        encode_glb(individual_gltf, bytes(individual_binary)),
                    )
                    individual_glb_cache[individual_cache_key] = (
                        individual_destination, individual_bindings,
                    )
                model_entry = {
                    "sha256": model_digest,
                    "fat_index": model_fat_index,
                    "glb_path": individual_path.as_posix(),
                    "material_texture_bindings": individual_bindings,
                    "member_indices": sorted({
                        context["model_member_index"]
                        for context in model["contexts"] if "model_member_index" in context
                    }),
                    "skeleton_sha256": skeletons[0]["sha256"] if skeletons else None,
                    "animation_sequence_count": animation_stats["sequence_count"],
                    "dynamic_animation_sequence_count": animation_stats["dynamic_sequence_count"],
                    "animation_frame_count": animation_stats["frame_count"],
                    "vm_action_count": animation_stats["vm_action_count"],
                    "vm_animation_frame_count": animation_stats["vm_frame_count"],
                    "unsupported_vm_opcodes": animation_stats["unsupported_vm_opcodes"],
                    "skipped_vm_action_count": animation_stats["skipped_vm_action_count"],
                }
                if world_model_index is not None:
                    model_entry["world_model_index"] = world_model_index
                model_entries.append(model_entry)

        model_roots = {merged["root"] for merged in merged_models.values()}
        if scene_key.startswith("field:"):
            placement = _dependency_with_format(dependencies, scene_key, "field-entity-initialization")
            if placement is not None:
                gltf["scenes"][0]["nodes"] = [node for node in gltf["scenes"][0]["nodes"]
                                                    if node not in model_roots]
                member_models = {}
                for digest, model in scene_models.items():
                    for context in model["contexts"]:
                        if "model_member_index" in context:
                            member_models[context["model_member_index"]] = digest
                placement_record, placement_context = placement
                data = (assets / placement_record["path"]).read_bytes()
                entity_count = placement_context.get("entity_count", len(data) // 0x10)
                for entity_id in range(min(entity_count, len(data) // 0x10)):
                    values = struct.unpack_from("<hhhhhhhh", data, entity_id * 0x10)
                    status, rotation, translation, model_index = values[0], values[1:4], values[4:7], values[7]
                    if status & 0x40 or model_index not in member_models:
                        continue
                    digest = member_models[model_index]
                    child = _clone_node_tree(gltf, merged_models[digest]["root"])
                    node_index = len(gltf["nodes"])
                    gltf["nodes"].append({
                        "name": f"field_entity_{entity_id:03d}", "children": [child],
                        "matrix": _psx_local_matrix(list(rotation), list(translation), unit_scale),
                        "extras": {"xenogears": {
                            "entity_id": entity_id, "status": status,
                            "model_member_index": model_index,
                            "transform_provenance": "field_header_initial",
                            "initially_hidden": bool(status & 0x20),
                        }},
                    })
                    gltf["scenes"][0]["nodes"].append(node_index)
            walkmesh = _dependency_with_format(dependencies, scene_key, "field-walkmesh")
            if walkmesh is not None:
                meshes = _add_field_walkmesh(
                    gltf, binary, (assets / walkmesh[0]["path"]).read_bytes(), unit_scale,
                    lit_materials=lit_materials,
                    generate_missing_normals=generate_missing_normals,
                )
                if meshes:
                    children = []
                    for layer, mesh in enumerate(meshes):
                        children.append(len(gltf["nodes"]))
                        gltf["nodes"].append({"name": f"field_walkmesh_layer_{layer:02d}",
                                              "mesh": mesh})
                    root = len(gltf["nodes"])
                    gltf["nodes"].append({"name": "field_collision", "children": children,
                                          "extras": {"xenogears": {"role": "collision_debug"}}})
                    gltf["scenes"][0]["nodes"].append(root)

        elif scene_key.startswith("world:"):
            gltf["scenes"][0]["nodes"] = [node for node in gltf["scenes"][0]["nodes"]
                                                if node not in model_roots]
            part_meshes = next(iter(merged_models.values()))["part_meshes"] if merged_models else {}
            collision_meshes = {}
            collision = next((record for record in dependencies if record["kind"] == "collision"), None)
            if collision is not None:
                collision_meshes = _add_world_collision(
                    gltf, binary, (assets / collision["path"]).read_bytes(), unit_scale,
                    lit_materials=lit_materials,
                    generate_missing_normals=generate_missing_normals,
                )
            placement = next((record for record in dependencies if record["kind"] == "placements"), None)
            if placement is not None:
                data = (assets / placement["path"]).read_bytes()
                count = u16(data, 0) if len(data) >= 2 else 0
                for placement_index in range(count):
                    at = 2 + placement_index * 0x10
                    if at + 0x10 > len(data):
                        break
                    model_index, placement_class, x, y, z, rx, ry, rz = struct.unpack_from(
                        "<HHhhhhhh", data, at,
                    )
                    children = []
                    node = {
                        "name": f"world_placement_{placement_index:03d}",
                        "matrix": _psx_local_matrix([rx, ry, rz], [x, y, -z], unit_scale,
                                                    (0x800, 0x800, 0x800)),
                        "extras": {"xenogears": {"model_index": model_index,
                                                    "placement_class": placement_class}},
                    }
                    if model_index in part_meshes:
                        node["mesh"] = part_meshes[model_index]
                    if model_index in collision_meshes:
                        collision_node = len(gltf["nodes"])
                        gltf["nodes"].append({"name": f"world_placement_{placement_index:03d}_collision",
                                              "mesh": collision_meshes[model_index]})
                        children.append(collision_node)
                    if children:
                        node["children"] = children
                    node_index = len(gltf["nodes"])
                    gltf["nodes"].append(node)
                    gltf["scenes"][0]["nodes"].append(node_index)
            terrain = None
            for record in dependencies:
                if record["kind"] != "terrain":
                    continue
                contexts = _catalog_dependency_contexts(record, scene_key, "")
                if any(context.get("traversal") == "row-major" for context in contexts):
                    terrain = record
                    break
            if terrain is not None:
                terrain_nodes = _add_world_terrain(
                    gltf, binary, (assets / terrain["path"]).read_bytes(), unit_scale,
                    lit_materials=lit_materials,
                    generate_missing_normals=generate_missing_normals,
                )
                if terrain_nodes:
                    root = len(gltf["nodes"])
                    gltf["nodes"].append({"name": "world_terrain", "children": terrain_nodes,
                                          "extras": {"xenogears": {"dimensions_slots": [16, 16],
                                                                       "toroidal": True}}})
                    gltf["scenes"][0]["nodes"].append(root)

        elif scene_key.startswith("battle-arena:"):
            collision = _dependency_with_format(dependencies, scene_key, "battle-arena-terrain")
            if collision is not None:
                data = (assets / collision[0]["path"]).read_bytes()
                if len(data) >= 0x514:
                    vertex_at, triangle_at = u32(data, 0x50C), u32(data, 0x510)
                    if triangle_at + 4 <= len(data):
                        mesh = _collision_mesh(
                            gltf, binary, data, "battle_arena_collision", unit_scale,
                            triangle_at + 4, u32(data, triangle_at), vertex_at,
                            lit_materials=lit_materials,
                            generate_missing_normals=generate_missing_normals,
                        )
                        if mesh is not None:
                            node = len(gltf["nodes"])
                            gltf["nodes"].append({"name": "battle_arena_collision", "mesh": mesh})
                            gltf["scenes"][0]["nodes"].append(node)

        elif scene_key.startswith("battling:"):
            terrain = _dependency_with_format(dependencies, scene_key,
                                              "battling-heightfield-128x128")
            if terrain is not None:
                mesh = _add_battling_heightfield(
                    gltf, binary, (assets / terrain[0]["path"]).read_bytes(), unit_scale,
                    lit_materials=lit_materials,
                    generate_missing_normals=generate_missing_normals,
                )
                if mesh is not None:
                    node = len(gltf["nodes"])
                    gltf["nodes"].append({"name": "battling_heightfield", "mesh": mesh})
                    gltf["scenes"][0]["nodes"].append(node)

        package_hashes = {
            record["sha256"] for record in dependencies if record["kind"] == "texture_package"
        }
        resource_entries = []
        for record in dependencies:
            kind, digest = record["kind"], record["sha256"]
            contexts = _catalog_dependency_contexts(record, scene_key, "")
            if not contexts:
                contexts = [
                    occurrence.get("resource_context", {})
                    for occurrence in record["occurrences"]
                    if occurrence.get("resource_context", {}).get("scene_key") == scene_key
                    or (scene_key.startswith("battling:")
                        and occurrence.get("resource_context", {}).get("scene_key") == "battling:common")
                ]
            source_packages = sorted({
                context["source_package_sha256"]
                for context in contexts if context.get("source_package_sha256") in package_hashes
            })
            extension_entry = {
                "kind": kind, "sha256": digest,
                "association": (
                    "exact_model_package"
                    if any(context.get("model_sha256") in scene_model_hashes for context in contexts)
                    else "shared_scene_resource"
                ),
            }
            if source_packages and kind in {"texture_image", "texture_upload"}:
                extension_entry["sourcePackageSha256"] = source_packages
            else:
                extension_entry["bufferView"] = _append_glb_payload(
                    gltf, binary, (assets / record["path"]).read_bytes(),
                    f"{kind}_{digest[:12]}",
                )
                extension_entry["mimeType"] = (
                    "application/x-playstation-tim" if kind == "texture_image"
                    else "application/octet-stream"
                )
            if kind == "texture_image":
                image_indexes = []
                for preview_index, preview in enumerate(record["previews"]):
                    png_view = _append_glb_payload(
                        gltf, binary, (assets / preview["path"]).read_bytes(),
                        f"texture_{digest[:12]}_{preview_index}",
                    )
                    image_index = len(gltf["images"])
                    gltf["images"].append({
                        "name": f"texture_{digest[:12]}_{preview_index}",
                        "bufferView": png_view, "mimeType": "image/png",
                    })
                    gltf["textures"].append({"source": image_index})
                    image_indexes.append(image_index)
                extension_entry["imageIndexes"] = image_indexes
            gltf["extensions"]["XENOGEARS_resource_bundle"]["resources"].append(extension_entry)
            resource_entries.append({
                "kind": kind, "sha256": digest,
                "association": extension_entry["association"],
                "embedded_as": (
                    "package_reference" if "sourcePackageSha256" in extension_entry
                    else "buffer_view"
                ),
                "image_count": len(extension_entry.get("imageIndexes", [])),
            })

        gltf["extensions"]["XENOGEARS_resource_bundle"]["animation_policy"] = (
            "All sequence tracks are decoded as timelines; deterministic VM scripts are evaluated as actions, and context-dependent scripts remain embedded."
        )
        gltf["extensions"]["XENOGEARS_resource_bundle"]["texture_policy"] = (
            "Decoded TIMs are standard images; unresolved VRAM uploads remain original embedded packages."
        )
        material_texture_bindings = bind_scene_material_textures(
            gltf, binary, vram, coverage, texture_png_cache,
        )
        _prepare_catalog_gltf(gltf)
        scene_root = catalog / scene_path
        scene_root.mkdir(parents=True, exist_ok=True)
        write_if_changed(scene_root / "scene.glb", encode_glb(gltf, bytes(binary)))
        bundle = {
            "schema": "xenogears-scene-glb/v1", "scene_key": scene_key,
            "fat_index": scene_fat_index,
            "glb_path": "scene.glb", "models": model_entries, "resources": resource_entries,
            "material_texture_bindings": material_texture_bindings,
        }
        write_if_changed(scene_root / "bundle.json",
                         (json.dumps(bundle, indent=2) + "\n").encode("utf-8"))
        scene_records.append({
            "scene_key": scene_key, "fat_index": scene_fat_index,
            "path": scene_path.as_posix(),
            "glb_path": (scene_path / "scene.glb").as_posix(),
            "model_count": len(model_entries), "resource_count": len(resource_entries),
            "individual_model_glbs": [
                (scene_path / model["glb_path"]).as_posix()
                for model in model_entries
            ],
            "material_texture_bindings": material_texture_bindings,
        })

    catalog_manifest = {
        "schema": "xenogears-scene-glb-catalog/v1", "scene_count": len(scene_records),
        "model_count": sum(scene["model_count"] for scene in scene_records),
        "individual_model_glbs": sum(len(scene["individual_model_glbs"])
                                     for scene in scene_records),
        "material_texture_bindings": sum(scene["material_texture_bindings"] for scene in scene_records),
        "scenes": scene_records,
    }
    write_if_changed(catalog / "catalog.json",
                     (json.dumps(catalog_manifest, indent=2) + "\n").encode("utf-8"))
    return {
        "path": "catalog/catalog.json", "scene_count": len(scene_records),
        "model_count": catalog_manifest["model_count"],
        "individual_model_glbs": catalog_manifest["individual_model_glbs"],
        "material_texture_bindings": catalog_manifest["material_texture_bindings"],
    }


def extract_models(discs: Iterable[Path], output: Path, unit_scale: float, *,
                   lit_materials: bool = False,
                   generate_missing_normals: bool = False) -> dict:
    disc_paths = list(discs)
    if not disc_paths:
        raise ValueError("at least one disc is required")
    all_resources = []
    disc_records = []
    for disc_index, path in enumerate(disc_paths):
        disc_record, resources = scan_disc(path, disc_index)
        disc_records.append(disc_record)
        all_resources.extend(resources)
    output = output.expanduser().absolute()
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = output.with_name(f".{output.name}.tmp")
    if staging.exists() or staging.is_symlink():
        if staging.is_symlink() or staging.is_file():
            staging.unlink()
        else:
            shutil.rmtree(staging)
    catalog_root = staging / "catalog"
    assets = catalog_root / "_assets"
    assets.mkdir(parents=True)
    try:
        records = materialize(
            assets, all_resources, unit_scale, lit_materials=lit_materials,
            generate_missing_normals=generate_missing_normals,
        )
        catalog = build_scene_catalog(
            catalog_root, assets, records, unit_scale, lit_materials=lit_materials,
            generate_missing_normals=generate_missing_normals,
        )
        texture_state_counts: Counter[str] = Counter()
        for record in records:
            texture_state_counts.update(record.get("texture_state_counts", {}))
        summary = {
            "unique_resources": len(records),
            "occurrences": sum(len(record["occurrences"]) for record in records),
            "by_kind": dict(sorted(Counter(record["kind"] for record in records).items())),
            "model_parts": sum(record.get("part_count", 0) for record in records),
            "model_primitives": sum(record.get("primitive_count", 0) for record in records),
            "catalog_models": catalog["model_count"],
            "scene_glbs": catalog["scene_count"],
            "individual_model_glbs": catalog["individual_model_glbs"],
            "material_texture_bindings": catalog["material_texture_bindings"],
            "texture_state_counts": dict(sorted(texture_state_counts.items())),
        }
        shutil.rmtree(assets)
        manifest = {
            "schema": SCHEMA, "unit_scale": unit_scale,
            "export_options": {
                "lit_materials": lit_materials,
                "generate_missing_normals": generate_missing_normals,
            },
            "discs": disc_records, "summary": summary, "catalog": catalog,
        }
        write_if_changed(catalog_root / "manifest.json",
                         (json.dumps(manifest, indent=2) + "\n").encode("utf-8"))
        if output.exists() or output.is_symlink():
            if output.is_symlink() or output.is_file():
                output.unlink()
            else:
                shutil.rmtree(output)
        staging.replace(output)
    except Exception:
        if staging.exists():
            shutil.rmtree(staging)
        raise
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("discs", nargs="+", type=Path, help="one or more retail CUE, BIN, or ISO images")
    parser.add_argument("--output", type=Path, default=Path("extracted-models"))
    parser.add_argument("--unit-scale", type=float, default=1.0 / 4096.0,
                        help="glTF units per PS1 model unit (default: 1/4096)")
    parser.add_argument(
        "--lit-materials", action="store_true",
        help="omit KHR_materials_unlit so exported materials respond to lights",
    )
    parser.add_argument(
        "--generate-missing-normals", action="store_true",
        help="generate flat face normals when model primitives have no authored normals",
    )
    args = parser.parse_args()
    if not math.isfinite(args.unit_scale) or args.unit_scale <= 0:
        parser.error("--unit-scale must be a finite positive number")
    manifest = extract_models(
        args.discs, args.output, args.unit_scale,
        lit_materials=args.lit_materials,
        generate_missing_normals=args.generate_missing_normals,
    )
    print(
        f"Extracted {manifest['summary']['by_kind'].get('model', 0)} unique models, "
        f"{manifest['summary']['by_kind'].get('skeleton', 0)} skeletons, and "
        f"{manifest['summary']['by_kind'].get('animation_clip', 0) + manifest['summary']['by_kind'].get('animation_container', 0)} "
        f"animation resources from {manifest['summary']['occurrences']} occurrences."
    )
    print(f"Model catalog: {args.output / manifest['catalog']['path']}")
    print(f"Manifest: {args.output / 'catalog' / 'manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
