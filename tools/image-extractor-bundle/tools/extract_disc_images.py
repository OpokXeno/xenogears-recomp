#!/usr/bin/env python3
"""Extract non-sprite raster images and fonts from Xenogears retail discs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import zlib
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


SCHEMA = "xenogears-disc-images/v1"
FAT_LBA = 0x18
FAT_SECTORS = 0x10
DIRECTORY_LBA = 0x28
DIRECTORY_COUNT = 64
USER_SECTOR = 2048
MAX_STORED_EXTENT_SIZE = 64 * 1024 * 1024
MAX_EXPANDED_SIZE = 64 * 1024 * 1024
MAX_PACKET_MEMBERS = 4096
TIM_MAGIC = b"\x10\x00\x00\x00"
MAIN_FONT_SHA256 = "66c595468d0f2edd762771a082a85d4c1ab9782bd9a26e4a25376bc67050ca65"


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
class TimImage:
    raw: bytes
    flags: int
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
class RawIndexedImage:
    raw: bytes
    format: str
    width: int
    height: int
    bpp: int
    image_data: bytes
    palettes: tuple[tuple[int, ...], ...]
    format_metadata: dict


@dataclass(frozen=True)
class ComposedImage:
    raw: bytes
    format: str
    width: int
    height: int
    bpp: int
    image_data: bytes
    format_metadata: dict


RawImage = RawIndexedImage | ComposedImage


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


def raw_image_sha256(image: RawImage) -> str:
    digest = hashlib.sha256()
    digest.update(b"xenogears-raw-image-v1\0")
    digest.update(image.format.encode("ascii"))
    digest.update(b"\0")
    digest.update(image.raw)
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


def lzss_decompress(data: bytes, offset: int = 0, max_size: int = MAX_EXPANDED_SIZE) -> tuple[bytes, int]:
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


def _parse_tim_block(data: bytes, offset: int, boundary: int) -> tuple[dict, bytes, int]:
    if offset + 12 > boundary:
        raise ValueError("truncated TIM block header")
    length, x, y, width_words, height = struct.unpack_from("<Ihhhh", data, offset)
    if length < 12 or width_words <= 0 or height <= 0:
        raise ValueError("invalid TIM block dimensions")
    expected = 12 + width_words * height * 2
    end = offset + length
    if length != expected or end > boundary:
        raise ValueError("invalid TIM block length")
    if x < 0 or y < 0 or x + width_words > 1024 or y + height > 512:
        raise ValueError("TIM block lies outside PS1 VRAM")
    return {
        "x": x,
        "y": y,
        "width_words": width_words,
        "height": height,
        "length": length,
    }, data[offset + 12 : end], end


def parse_tim(data: bytes, offset: int = 0, boundary: int | None = None) -> TimImage:
    boundary = len(data) if boundary is None else boundary
    if offset < 0 or offset + 8 > boundary or data[offset : offset + 4] != TIM_MAGIC:
        raise ValueError("missing TIM header")
    flags = u32(data, offset + 4)
    if flags & ~0x0F or flags & 7 > 3:
        raise ValueError("unsupported TIM flags")
    mode = flags & 7
    has_clut = bool(flags & 8)
    if has_clut != (mode in (0, 1)):
        raise ValueError("TIM CLUT presence does not match indexed mode")
    cursor = offset + 8
    palettes: tuple[tuple[int, ...], ...] = ()
    clut_x = clut_y = None
    if has_clut:
        clut, clut_data, cursor = _parse_tim_block(data, cursor, boundary)
        palette_size = 16 if mode == 0 else 256
        if clut["width_words"] != palette_size:
            raise ValueError("TIM CLUT has an unsupported layout")
        words = struct.unpack(f"<{len(clut_data) // 2}H", clut_data)
        palettes = tuple(
            tuple(words[start : start + palette_size])
            for start in range(0, len(words), palette_size)
        )
        clut_x, clut_y = clut["x"], clut["y"]
    image, image_data, end = _parse_tim_block(data, cursor, boundary)
    row_bytes = image["width_words"] * 2
    width = (image["width_words"] * 4, image["width_words"] * 2, image["width_words"], row_bytes // 3)[mode]
    if width <= 0:
        raise ValueError("TIM image has zero display width")
    return TimImage(
        raw=data[offset:end],
        flags=flags,
        mode=mode,
        image_x=image["x"],
        image_y=image["y"],
        width_words=image["width_words"],
        width=width,
        height=image["height"],
        image_data=image_data,
        clut_x=clut_x,
        clut_y=clut_y,
        palettes=palettes,
        row_trailing_bytes=row_bytes % 3 if mode == 3 else 0,
    )


def scan_tims(data: bytes) -> list[tuple[int, TimImage]]:
    found = []
    cursor = 0
    while True:
        offset = data.find(TIM_MAGIC, cursor)
        if offset < 0:
            break
        try:
            tim = parse_tim(data, offset)
        except (ValueError, struct.error):
            cursor = offset + 1
            continue
        found.append((offset, tim))
        cursor = offset + len(tim.raw)
    return found


def tim_bundle_members(data: bytes) -> list[tuple[int, int, TimImage, int]] | None:
    if len(data) < 16:
        return None
    count = u32(data, 0)
    header = count * 4 + 8
    if count == 0 or count > MAX_PACKET_MEMBERS or header > len(data):
        return None
    offsets = [u32(data, 4 + index * 4) for index in range(count + 1)]
    if offsets[0] != header or offsets[-1] > len(data):
        return None
    if any(left >= right or left < header for left, right in zip(offsets, offsets[1:])):
        return None
    members = []
    for member_index, (start, end) in enumerate(zip(offsets, offsets[1:])):
        try:
            tim = parse_tim(data, start, end)
        except (ValueError, struct.error):
            return None
        trailer_size = end - (start + len(tim.raw))
        if trailer_size > 8:
            return None
        members.append((member_index, start, tim, trailer_size))
    return members


def _rgb555(color: int, *, transparent_zero: bool) -> bytes:
    red = color & 0x1F
    green = color >> 5 & 0x1F
    blue = color >> 10 & 0x1F
    alpha = 0 if transparent_zero and color == 0 else 255
    return bytes(((red << 3) | (red >> 2), (green << 3) | (green >> 2), (blue << 3) | (blue >> 2), alpha))


def decode_tim(tim: TimImage, palette_variant: int = 0) -> bytes:
    output = bytearray()
    if tim.mode in (0, 1):
        if not 0 <= palette_variant < len(tim.palettes):
            raise ValueError("TIM palette variant is outside the CLUT")
        palette = [_rgb555(color, transparent_zero=True) for color in tim.palettes[palette_variant]]
        if tim.mode == 0:
            for value in tim.image_data:
                output.extend(palette[value & 0x0F])
                output.extend(palette[value >> 4])
        else:
            for value in tim.image_data:
                output.extend(palette[value])
    elif tim.mode == 2:
        for offset in range(0, len(tim.image_data), 2):
            output.extend(_rgb555(u16(tim.image_data, offset), transparent_zero=False))
    else:
        row_bytes = tim.width_words * 2
        for row in range(tim.height):
            row_data = tim.image_data[row * row_bytes : (row + 1) * row_bytes]
            for offset in range(0, tim.width * 3, 3):
                output.extend(row_data[offset : offset + 3])
                output.append(255)
    required = tim.width * tim.height * 4
    if len(output) != required:
        raise ValueError("decoded TIM dimensions do not match its payload")
    return bytes(output)


def decode_raw_indexed(image: RawIndexedImage, palette_variant: int = 0) -> bytes:
    if not 0 <= palette_variant < len(image.palettes):
        raise ValueError("raw image palette variant is outside the CLUT")
    palette = [_rgb555(color, transparent_zero=True) for color in image.palettes[palette_variant]]
    output = bytearray()
    if image.bpp == 4:
        for value in image.image_data:
            output.extend(palette[value & 0x0F])
            output.extend(palette[value >> 4])
    elif image.bpp == 8:
        for value in image.image_data:
            output.extend(palette[value])
    else:
        raise ValueError("unsupported raw indexed-image depth")
    if len(output) != image.width * image.height * 4:
        raise ValueError("decoded raw image dimensions do not match its payload")
    return bytes(output)


def parse_battling_portraits(data: bytes) -> list[RawIndexedImage]:
    record_size = 0x1000
    if len(data) != 49 * record_size:
        raise ValueError("Battling portrait file does not contain 49 records")
    images = []
    for index in range(49):
        record = data[index * record_size : (index + 1) * record_size]
        stored_palette = struct.unpack_from("<128H", record)
        images.append(RawIndexedImage(
            raw=record,
            format="battling-selection-portrait",
            width=60,
            height=64,
            bpp=8,
            image_data=record[0x100:],
            palettes=(stored_palette + (0,) * 128,),
            format_metadata={
                "record_size": record_size,
                "serialized_palette_words": 128,
                "effective_palette_words": 256,
                "unwritten_palette_policy": "rgb555_zero",
                "image_width_words": 30,
                "serialized_palette_offset": 0,
                "serialized_image_offset": 0x100,
            },
        ))
    return images


def parse_battling_fighter_hud(data: bytes) -> list[tuple[int, RawIndexedImage]]:
    if len(data) < 40:
        raise ValueError("Battling fighter archive header is truncated")
    build_base = u32(data, 0x1C)
    hud_pointer = u32(data, 0x20)
    if hud_pointer < build_base:
        raise ValueError("Battling fighter HUD pointer is invalid")
    hud_offset = hud_pointer - build_base
    if hud_offset < 40 or hud_offset + 0x4E4 > len(data):
        raise ValueError("Battling fighter HUD block leaves the archive")
    palette_bytes = data[hud_offset : hud_offset + 0x200]
    palette = struct.unpack("<256H", palette_bytes)
    layouts = (
        ("battling-fighter-hud-primary", 0x200, 22, 22),
        ("battling-fighter-hud-secondary", 0x3E4, 32, 8),
    )
    images = []
    for format_name, relative_offset, width, height in layouts:
        image_data = data[hud_offset + relative_offset : hud_offset + relative_offset + width * height]
        images.append((hud_offset + relative_offset, RawIndexedImage(
            raw=palette_bytes + image_data,
            format=format_name,
            width=width,
            height=height,
            bpp=8,
            image_data=image_data,
            palettes=(palette,),
            format_metadata={
                "canonical_palette_offset": 0,
                "canonical_image_offset": 0x200,
                "serialized_orientation": "fighter-side-1",
                "fighter_side_0_mirrors_rows": format_name.endswith("primary"),
            },
        )))
    return images


def parse_battle_common_ui(data: bytes) -> list[tuple[int, RawIndexedImage]]:
    if len(data) != 33841 or struct.unpack_from("<III", data) != (2, 12, 7708):
        raise ValueError("Battle common UI tagged-image offset table is invalid")
    clut_header = struct.unpack_from("<Ihhhhhh", data, 12)
    image_header = struct.unpack_from("<Ihhhhhh", data, 7708)
    if clut_header != (0x1101, 0, 497, 0, 0, 256, 15):
        raise ValueError("Battle common UI CLUT record is invalid")
    if image_header != (0x1100, 960, 52, 0, 0, 64, 204):
        raise ValueError("Battle common UI image record is invalid")
    clut_words = struct.unpack_from("<3840H", data, 28)
    image_data = data[7724:33836]
    palettes_8bpp = tuple(
        tuple(clut_words[row * 256 : (row + 1) * 256]) for row in range(15)
    )
    palettes_4bpp = tuple(
        tuple(clut_words[row * 256 + column * 16 : row * 256 + (column + 1) * 16])
        for row in range(15)
        for column in range(16)
    )
    common_metadata = {
        "packed_tagged_set_sha256": hashlib.sha256(data).hexdigest(),
        "serialized_clut_rectangle": {"x_words": 0, "y": 497, "width_words": 256, "height": 15},
        "serialized_image_rectangle": {"x_words": 960, "y": 52, "width_words": 64, "height": 204},
        "serialized_trailer_bytes": 5,
    }
    return [
        (7724, RawIndexedImage(
            raw=data,
            format="battle-common-ui-atlas-4bpp-view",
            width=256,
            height=204,
            bpp=4,
            image_data=image_data,
            palettes=palettes_4bpp,
            format_metadata={
                **common_metadata,
                "palette_order": "clut_row_major_then_16_word_column",
            },
        )),
        (7724, RawIndexedImage(
            raw=data,
            format="battle-common-ui-atlas-8bpp-view",
            width=128,
            height=204,
            bpp=8,
            image_data=image_data,
            palettes=palettes_8bpp,
            format_metadata={
                **common_metadata,
                "palette_order": "clut_row",
            },
        )),
    ]


def render_battle_sfont(sfont: bytes, ui_data: bytes) -> ComposedImage:
    if len(sfont) != 0xAFE8 or u32(sfont, 0) != 234:
        raise ValueError("Battle sFont does not have the authenticated glyph count and size")
    first_glyph = 4 + 234 * 2
    glyph_offsets = [u16(sfont, 4 + index * 2) for index in range(234)]
    if glyph_offsets[0] != first_glyph or any(
        left > right for left, right in zip(glyph_offsets, glyph_offsets[1:])
    ):
        raise ValueError("Battle sFont glyph offsets are invalid")
    ui_views = parse_battle_common_ui(ui_data)
    view_4bpp = ui_views[0][1]
    view_8bpp = ui_views[1][1]
    source_row_bytes = 64 * 2
    glyph_rasters = []
    glyph_metadata = []
    total_polygons = 0

    for glyph_index, glyph_offset in enumerate(glyph_offsets):
        if glyph_offset + 4 > len(sfont):
            raise ValueError("Battle sFont glyph header is truncated")
        polygon_count, reserved = struct.unpack_from("<HH", sfont, glyph_offset)
        glyph_end = glyph_offset + 4 + polygon_count * 0x1C
        expected_end = glyph_offsets[glyph_index + 1] if glyph_index + 1 < len(glyph_offsets) else len(sfont)
        if reserved != 0 or glyph_end != expected_end:
            raise ValueError("Battle sFont glyph does not cover its serialized interval")
        polygons = [
            struct.unpack_from("<hhhhhhHHHHHHHBB", sfont, glyph_offset + 4 + index * 0x1C)
            for index in range(polygon_count)
        ]
        if any(poly[6:8] != (4, 0) or poly[8] not in {0, 1} for poly in polygons):
            raise ValueError("Battle sFont polygon has unsupported state")
        min_x = min((poly[4] for poly in polygons), default=0)
        min_y = min((poly[5] for poly in polygons), default=0)
        max_x = max((poly[4] + poly[2] for poly in polygons), default=1)
        max_y = max((poly[5] + poly[3] for poly in polygons), default=1)
        width, height = max(1, max_x - min_x), max(1, max_y - min_y)
        raster = bytearray(width * height * 4)
        for poly in polygons:
            texture_u, texture_v, texture_width, texture_height = poly[0:4]
            x_offset, y_offset = poly[4:6]
            depth, clut_x, clut_y, tpage_x, tpage_y = poly[8:13]
            flip_x, flip_y = bool(poly[13]), bool(poly[14])
            if texture_width < 0 or texture_height < 0:
                raise ValueError("Battle sFont polygon has negative dimensions")
            if texture_width == 0 or texture_height == 0:
                continue
            page_x_words = tpage_x // 64 * 64
            page_y = 256 if tpage_y >= 256 else 0
            if depth == 0:
                palette_index = (clut_y - 497) * 16 + clut_x // 16
                if not 0 <= palette_index < len(view_4bpp.palettes):
                    raise ValueError("Battle sFont 4-bpp CLUT leaves the UI atlas")
                palette = view_4bpp.palettes[palette_index]
                source_x = (page_x_words - 960) * 4 + texture_u
            else:
                palette_index = clut_y - 497
                if not 0 <= palette_index < len(view_8bpp.palettes):
                    raise ValueError("Battle sFont 8-bpp CLUT leaves the UI atlas")
                palette = view_8bpp.palettes[palette_index]
                source_x = (page_x_words - 960) * 2 + texture_u
            source_y = page_y + texture_v - 52
            source_width = 256 if depth == 0 else 128
            if (
                source_x < 0 or source_y < 0
                or source_x + texture_width > source_width
                or source_y + texture_height > 204
            ):
                raise ValueError("Battle sFont polygon samples outside the UI atlas")
            for y in range(texture_height):
                sample_y = texture_height - 1 - y if flip_y else y
                for x in range(texture_width):
                    sample_x = texture_width - 1 - x if flip_x else x
                    pixel_x = source_x + sample_x
                    source_at = (source_y + sample_y) * source_row_bytes
                    if depth == 0:
                        value = view_4bpp.image_data[source_at + pixel_x // 2]
                        color_index = value >> 4 if pixel_x & 1 else value & 0x0F
                    else:
                        color_index = view_8bpp.image_data[source_at + pixel_x]
                    color = _rgb555(palette[color_index], transparent_zero=True)
                    if color[3] == 0:
                        continue
                    destination_x = x_offset - min_x + x
                    destination_y = y_offset - min_y + y
                    destination_at = (destination_y * width + destination_x) * 4
                    raster[destination_at : destination_at + 4] = color
        glyph_rasters.append((width, height, bytes(raster)))
        glyph_metadata.append({
            "glyph_index": glyph_index,
            "polygon_count": polygon_count,
            "bounding_box": {"x": min_x, "y": min_y, "width": width, "height": height},
        })
        total_polygons += polygon_count
    if total_polygons != 1558:
        raise ValueError("Battle sFont polygon census does not match the authenticated layout")

    atlas_width = 1024
    placements = []
    cursor_x = cursor_y = row_height = 0
    for width, height, _ in glyph_rasters:
        if cursor_x and cursor_x + width + 1 > atlas_width:
            cursor_x = 0
            cursor_y += row_height + 1
            row_height = 0
        placements.append((cursor_x, cursor_y))
        cursor_x += width + 1
        row_height = max(row_height, height)
    atlas_height = cursor_y + row_height
    atlas = bytearray(atlas_width * atlas_height * 4)
    for metadata, (width, height, raster), (origin_x, origin_y) in zip(
        glyph_metadata, glyph_rasters, placements,
    ):
        metadata["atlas_rectangle"] = {
            "x": origin_x, "y": origin_y, "width": width, "height": height,
        }
        for y in range(height):
            source_at = y * width * 4
            destination_at = ((origin_y + y) * atlas_width + origin_x) * 4
            atlas[destination_at : destination_at + width * 4] = raster[source_at : source_at + width * 4]
    canonical = struct.pack("<II", len(sfont), len(ui_data)) + sfont + ui_data
    return ComposedImage(
        raw=canonical,
        format="battle-polygon-sfont-atlas",
        width=atlas_width,
        height=atlas_height,
        bpp=32,
        image_data=bytes(atlas),
        format_metadata={
            "glyph_count": 234,
            "polygon_count": total_polygons,
            "atlas_layout": "variable_shelf_pack_with_one_pixel_gutter",
            "source_sfont_sha256": hashlib.sha256(sfont).hexdigest(),
            "source_ui_sha256": hashlib.sha256(ui_data).hexdigest(),
            "glyphs": glyph_metadata,
        },
    )


def encode_png_rgba(width: int, height: int, pixels: bytes) -> bytes:
    if width <= 0 or height <= 0 or len(pixels) != width * height * 4:
        raise ValueError("invalid RGBA raster")
    scanlines = b"".join(b"\x00" + pixels[y * width * 4 : (y + 1) * width * 4] for y in range(height))

    def chunk(kind: bytes, payload: bytes) -> bytes:
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(scanlines, 9))
        + chunk(b"IEND", b"")
    )


def render_main_font(data: bytes) -> tuple[int, int, bytes]:
    if len(data) != 6594 or hashlib.sha256(data).hexdigest() != MAIN_FONT_SHA256:
        raise ValueError("main font does not match the authenticated layout")
    bitmap_offset = u16(data, 2)
    glyph_count = (len(data) - bitmap_offset) // 22
    columns = 16
    rows = (glyph_count + columns - 1) // columns
    width, height = columns * 16, rows * 11
    pixels = bytearray(width * height * 4)
    for glyph in range(glyph_count):
        origin_x = glyph % columns * 16
        origin_y = glyph // columns * 11
        for y in range(11):
            bits = u16(data, bitmap_offset + glyph * 22 + y * 2)
            for x in range(16):
                if bits & (0x8000 >> x):
                    at = ((origin_y + y) * width + origin_x + x) * 4
                    pixels[at : at + 4] = b"\xff\xff\xff\xff"
    return width, height, bytes(pixels)


def write_if_changed(path: Path, content: bytes) -> None:
    if path.is_file() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(content)
    temporary.replace(path)


def _route_ids(routes: list[dict]) -> set[tuple[int, int]]:
    return {(int(route["directory"], 16), int(route["file_id"], 16)) for route in routes}


def _main_font_route(routes: list[dict]) -> bool:
    return (0x01, 0x06) in _route_ids(routes)


def scan_disc(
    path: Path, disc_index: int,
) -> tuple[dict, list[tuple[TimImage, dict]], list[tuple[RawIndexedImage, dict]], list[tuple[bytes, dict]]]:
    disc = open_disc(path)
    entries = parse_fat(disc.read_user_data(FAT_LBA, FAT_SECTORS * USER_SECTOR), disc.sector_count)
    xa_children = -entries[0].size if entries and entries[0].size < 0 else 0
    if xa_children > len(entries) - 1:
        raise ValueError("XA streaming root exceeds the FAT")
    directory = parse_directory_table(disc.read_user_data(DIRECTORY_LBA, DIRECTORY_COUNT * 2))
    routes_by_index = map_physical_routes(directory, len(entries))
    images: list[tuple[TimImage, dict]] = []
    raw_images: list[tuple[RawImage, dict]] = []
    fonts: list[tuple[bytes, dict]] = []
    occurrence_keys = set()
    raw_occurrence_keys = set()
    font_keys = set()
    skipped_extents = []

    def occurrence(entry: FatEntry, fat_index: int, routes: list[dict], chain: list[dict], offset: int, ordinal: int) -> dict:
        return {
            "disc_index": disc_index,
            "disc_name": path.name,
            "fat_index": fat_index,
            "lba": entry.lba,
            "stored_size": entry.size,
            "container_image_index": ordinal,
            "decoded_container_offset": offset,
            "transform_chain": chain,
            "routes": routes,
        }

    def add_container(container: bytes, entry: FatEntry, fat_index: int, routes: list[dict], chain: list[dict]) -> None:
        bundle = tim_bundle_members(container)
        candidates = (
            [
                (offset, tim, chain + [{
                    "type": "tim_bundle_member",
                    "member_index": member_index,
                    "stored_offset": offset,
                    "logical_stored_size": len(tim.raw) + trailer_size,
                    "trailer_size": trailer_size,
                }])
                for member_index, offset, tim, trailer_size in bundle
            ]
            if bundle is not None else
            [(offset, tim, chain) for offset, tim in scan_tims(container)]
        )
        for ordinal, (offset, tim, candidate_chain) in enumerate(candidates):
            digest = hashlib.sha256(tim.raw).hexdigest()
            chain_key = tuple(
                (step["type"], step.get("member_index"), step.get("stored_offset"))
                for step in candidate_chain
            )
            key = (fat_index, chain_key, offset, digest)
            if key in occurrence_keys:
                continue
            occurrence_keys.add(key)
            images.append((tim, occurrence(entry, fat_index, routes, candidate_chain, offset, ordinal)))
        if _main_font_route(routes) and hashlib.sha256(container).hexdigest() == MAIN_FONT_SHA256:
            key = (fat_index, hashlib.sha256(container).hexdigest())
            if key not in font_keys:
                font_keys.add(key)
                fonts.append((container, occurrence(entry, fat_index, routes, chain, 0, 0)))

    def add_raw_image(
        image: RawImage, entry: FatEntry, fat_index: int, routes: list[dict],
        chain: list[dict], offset: int, ordinal: int,
    ) -> None:
        digest = raw_image_sha256(image)
        key = (fat_index, image.format, offset, digest)
        if key in raw_occurrence_keys:
            return
        raw_occurrence_keys.add(key)
        raw_images.append((image, occurrence(entry, fat_index, routes, chain, offset, ordinal)))

    for fat_index, entry in enumerate(entries):
        if fat_index <= xa_children or entry.size <= 0:
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
        add_container(raw, entry, fat_index, routes, [])
        if (0x30, 0x06) in _route_ids(routes):
            try:
                portraits = parse_battling_portraits(raw)
            except (ValueError, struct.error):
                portraits = []
            for portrait_index, portrait in enumerate(portraits):
                add_raw_image(portrait, entry, fat_index, routes, [{
                    "type": "fixed_record",
                    "record_index": portrait_index,
                    "stored_offset": portrait_index * 0x1000,
                    "logical_stored_size": 0x1000,
                }], portrait_index * 0x1000, portrait_index)
        members = packet_members(raw)
        if members:
            battle_sfont = None
            for member_index, (start, end) in enumerate(members):
                if end - start < 4:
                    continue
                try:
                    expanded, consumed = lzss_decompress(padded, start)
                except ValueError:
                    continue
                member_chain = [{
                    "type": "packet_member_lzss",
                    "member_index": member_index,
                    "stored_offset": start,
                    "logical_stored_size": end - start,
                    "consumed_size": consumed,
                }]
                add_container(expanded, entry, fat_index, routes, member_chain)
                if member_index == 0 and (0x0C, 0x03) in _route_ids(routes):
                    battle_sfont = expanded
                if member_index == 1 and (0x0C, 0x03) in _route_ids(routes):
                    try:
                        battle_ui_views = parse_battle_common_ui(expanded)
                    except (ValueError, struct.error):
                        battle_ui_views = []
                    for view_index, (image_offset, image) in enumerate(battle_ui_views):
                        add_raw_image(image, entry, fat_index, routes, member_chain + [{
                            "type": "packed_tagged_image_view",
                            "record_index": 1,
                            "decoded_offset": image_offset,
                            "pixel_depth": image.bpp,
                        }], image_offset, view_index)
                    if battle_sfont is not None:
                        try:
                            font_atlas = render_battle_sfont(battle_sfont, expanded)
                        except (ValueError, struct.error):
                            font_atlas = None
                        if font_atlas is not None:
                            add_raw_image(font_atlas, entry, fat_index, routes, member_chain + [{
                                "type": "polygon_font_composition",
                                "sfont_member_index": 0,
                                "ui_member_index": 1,
                            }], 0, len(battle_ui_views))
        try:
            expanded, consumed = lzss_decompress(padded)
        except ValueError:
            expanded = None
        if expanded is not None:
            whole_chain = [{"type": "whole_file_lzss", "stored_offset": 0, "consumed_size": consumed}]
            add_container(expanded, entry, fat_index, routes, whole_chain)
            if any(directory_id == 0x31 and 2 <= file_id <= 0x32 for directory_id, file_id in _route_ids(routes)):
                try:
                    fighter_hud = parse_battling_fighter_hud(expanded)
                except (ValueError, struct.error):
                    fighter_hud = []
                for hud_index, (hud_offset, image) in enumerate(fighter_hud):
                    add_raw_image(image, entry, fat_index, routes, whole_chain + [{
                        "type": "fighter_hud_component",
                        "component": image.format.removeprefix("battling-fighter-hud-"),
                        "decoded_offset": hud_offset,
                        "palette_decoded_offset": u32(expanded, 0x20) - u32(expanded, 0x1C),
                    }], hud_offset, hud_index)
            expanded_members = packet_members(expanded)
            if expanded_members:
                guarded = expanded + bytes(USER_SECTOR)
                for member_index, (start, end) in enumerate(expanded_members):
                    if end - start < 4:
                        continue
                    try:
                        member, member_consumed = lzss_decompress(guarded, start)
                    except ValueError:
                        continue
                    add_container(member, entry, fat_index, routes, whole_chain + [{
                        "type": "packet_member_lzss",
                        "member_index": member_index,
                        "stored_offset": start,
                        "logical_stored_size": end - start,
                        "consumed_size": member_consumed,
                    }])
        field_actor_route = any(
            directory_id == 0x04 and file_id >= 0xB8 and (file_id - 0xB8) % 2 == 0
            for directory_id, file_id in _route_ids(routes)
        )
        if field_actor_route and len(raw) >= 0x134:
            section_offset = u32(raw, 0x130)
            declared_size = u32(raw, 0x10C)
            if section_offset and declared_size and section_offset < len(raw):
                try:
                    section, consumed = lzss_decompress(padded, section_offset)
                except ValueError:
                    pass
                else:
                    if len(section) <= declared_size + 0x10:
                        add_container(section, entry, fat_index, routes, [{
                            "type": "field_actor_section0_lzss",
                            "section": 0,
                            "stored_offset": section_offset,
                            "consumed_size": consumed,
                            "declared_expanded_size": declared_size,
                            "actual_expanded_size": len(section),
                        }])
    return {
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
        "image_occurrences": len(images),
        "raw_image_occurrences": len(raw_images),
        "font_occurrences": len(fonts),
        "skipped_extent_count": len(skipped_extents),
        "skipped_extents": skipped_extents,
    }, images, raw_images, fonts


def _tim_metadata(tim: TimImage) -> dict:
    palette_words = [[f"0x{color:04X}" for color in palette] for palette in tim.palettes]
    all_colors = [color for palette in tim.palettes for color in palette]
    if tim.mode == 2:
        all_colors = list(struct.unpack(f"<{len(tim.image_data) // 2}H", tim.image_data))
    return {
        "kind": "tim_image",
        "flags": f"0x{tim.flags:08X}",
        "pixel_mode": tim.mode,
        "bpp": tim.bpp,
        "width": tim.width,
        "height": tim.height,
        "image_rectangle": {
            "x_words": tim.image_x,
            "y": tim.image_y,
            "width_words": tim.width_words,
            "height": tim.height,
        },
        "clut_rectangle": None if not tim.palettes else {
            "x_words": tim.clut_x,
            "y": tim.clut_y,
            "width_words": len(tim.palettes[0]),
            "height": len(tim.palettes),
        },
        "palette_variant_count": len(tim.palettes) if tim.palettes else 1,
        "palettes_rgb555_stp": palette_words,
        "transparent_color_word_count": sum(color == 0 for color in all_colors),
        "stp_color_word_count": sum(bool(color & 0x8000) for color in all_colors),
        "row_trailing_bytes": tim.row_trailing_bytes,
        "png_transparency_policy": "resolved_rgb555_zero" if tim.mode in (0, 1) else "opaque_direct_color",
    }


def _materialize_tim(output: Path, digest: str, tim: TimImage, occurrences: list[dict]) -> dict:
    asset_path = Path("assets") / f"{digest}.tim"
    metadata_path = Path("metadata") / f"{digest}.json"
    write_if_changed(output / asset_path, tim.raw)
    metadata = _tim_metadata(tim)
    write_if_changed(output / metadata_path, (json.dumps(metadata, indent=2, ensure_ascii=True) + "\n").encode("ascii"))
    previews = []
    palette_count = len(tim.palettes) if tim.palettes else 1
    for variant in range(palette_count):
        png = encode_png_rgba(tim.width, tim.height, decode_tim(tim, variant))
        png_digest = hashlib.sha256(png).hexdigest()
        png_path = Path("previews") / f"{png_digest}.png"
        write_if_changed(output / png_path, png)
        previews.append({
            "palette_variant": variant,
            "sha256": png_digest,
            "path": png_path.as_posix(),
            "width": tim.width,
            "height": tim.height,
        })
    return {
        "sha256": digest,
        "kind": "tim_image",
        "asset_path": asset_path.as_posix(),
        "asset_extension": ".tim",
        "metadata_path": metadata_path.as_posix(),
        "bpp": tim.bpp,
        "width": tim.width,
        "height": tim.height,
        "palette_variant_count": palette_count,
        "previews": previews,
        "occurrences": sorted(occurrences, key=_occurrence_sort_key),
    }


def _materialize_raw_image(
    output: Path, digest: str, image: RawImage, occurrences: list[dict],
) -> dict:
    asset_path = Path("assets") / f"{digest}.bin"
    metadata_path = Path("metadata") / f"{digest}.json"
    write_if_changed(output / asset_path, image.raw)
    metadata = {
        "kind": (
            "raw_indexed_image" if isinstance(image, RawIndexedImage)
            else "composed_image"
        ),
        "format": image.format,
        "resource_sha256": digest,
        "asset_sha256": hashlib.sha256(image.raw).hexdigest(),
        "bpp": image.bpp,
        "width": image.width,
        "height": image.height,
        "image_data_bytes": len(image.image_data),
        **image.format_metadata,
    }
    if isinstance(image, RawIndexedImage):
        metadata.update({
            "palette_variant_count": len(image.palettes),
            "palettes_rgb555_stp": [
                [f"0x{color:04X}" for color in palette] for palette in image.palettes
            ],
            "transparent_color_word_count": sum(
                color == 0 for palette in image.palettes for color in palette
            ),
            "stp_color_word_count": sum(
                bool(color & 0x8000) for palette in image.palettes for color in palette
            ),
            "png_transparency_policy": "resolved_rgb555_zero",
        })
        decoded_variants = [decode_raw_indexed(image, variant) for variant in range(len(image.palettes))]
    else:
        metadata.update({
            "palette_variant_count": 1,
            "png_transparency_policy": "composed_source_alpha",
        })
        decoded_variants = [image.image_data]
    write_if_changed(output / metadata_path, (json.dumps(metadata, indent=2, ensure_ascii=True) + "\n").encode("ascii"))
    previews = []
    for variant, pixels in enumerate(decoded_variants):
        png = encode_png_rgba(image.width, image.height, pixels)
        png_digest = hashlib.sha256(png).hexdigest()
        png_path = Path("previews") / f"{png_digest}.png"
        write_if_changed(output / png_path, png)
        previews.append({
            "palette_variant": variant,
            "sha256": png_digest,
            "path": png_path.as_posix(),
            "width": image.width,
            "height": image.height,
        })
    return {
        "sha256": digest,
        "kind": metadata["kind"],
        "format": image.format,
        "asset_path": asset_path.as_posix(),
        "asset_extension": ".bin",
        "metadata_path": metadata_path.as_posix(),
        "bpp": image.bpp,
        "width": image.width,
        "height": image.height,
        "palette_variant_count": len(decoded_variants),
        "previews": previews,
        "occurrences": sorted(occurrences, key=_occurrence_sort_key),
    }


def _materialize_font(output: Path, digest: str, data: bytes, occurrences: list[dict]) -> dict:
    width, height, pixels = render_main_font(data)
    png = encode_png_rgba(width, height, pixels)
    png_digest = hashlib.sha256(png).hexdigest()
    asset_path = Path("assets") / f"{digest}.bin"
    metadata_path = Path("metadata") / f"{digest}.json"
    png_path = Path("previews") / f"{png_digest}.png"
    metadata = {
        "kind": "bitmap_font",
        "format": "xenogears-main-dialog-font",
        "glyph_width": 16,
        "glyph_height": 11,
        "glyph_count": 299,
        "first_code": "0x0010",
        "bitmap_offset": u16(data, 2),
        "atlas_columns": 16,
        "atlas_width": width,
        "atlas_height": height,
    }
    write_if_changed(output / asset_path, data)
    write_if_changed(output / metadata_path, (json.dumps(metadata, indent=2, ensure_ascii=True) + "\n").encode("ascii"))
    write_if_changed(output / png_path, png)
    return {
        "sha256": digest,
        "kind": "bitmap_font",
        "asset_path": asset_path.as_posix(),
        "asset_extension": ".bin",
        "metadata_path": metadata_path.as_posix(),
        "width": width,
        "height": height,
        "palette_variant_count": 1,
        "previews": [{
            "palette_variant": 0,
            "sha256": png_digest,
            "path": png_path.as_posix(),
            "width": width,
            "height": height,
        }],
        "occurrences": sorted(occurrences, key=_occurrence_sort_key),
    }


def _occurrence_sort_key(occurrence: dict) -> tuple:
    return (
        occurrence["disc_index"],
        occurrence["fat_index"],
        tuple((step["type"], step.get("member_index", -1)) for step in occurrence["transform_chain"]),
        occurrence["decoded_container_offset"],
    )


def _member_index(occurrence: dict) -> int | None:
    for step in reversed(occurrence["transform_chain"]):
        if "member_index" in step:
            return step["member_index"]
    return None


_BATTLING_3D_ONLY_TIM_MEMBERS = frozenset(range(17, 21)) | frozenset(range(26, 36))
_WORLD_SHARED_3D_ONLY_TIM_MEMBERS = {
    0: frozenset(range(11, 29)),
    1: frozenset(range(11, 15)) | frozenset(range(16, 28)),
    2: frozenset(range(12, 17)) | frozenset(range(18, 59)),
    3: frozenset(range(11, 16)) | frozenset(range(17, 65)),
    4: frozenset(range(11, 16)) | frozenset(range(17, 61)),
    5: frozenset(range(11, 16)) | frozenset(range(17, 63)),
    6: frozenset(range(11, 16)) | frozenset(range(17, 65)),
    7: frozenset(range(11, 16)) | frozenset(range(17, 65)),
    8: frozenset(range(11, 16)) | frozenset(range(17, 36)) | frozenset(range(37, 65)) | frozenset(range(66, 75)),
    9: (
        frozenset(range(9, 32)) | frozenset(range(34, 70)) | frozenset(range(71, 82))
        | frozenset(range(83, 88)) | frozenset(range(89, 103)) | frozenset({105})
    ),
    10: frozenset(range(8, 19)) | frozenset(range(20, 23)),
    11: frozenset(range(9, 40)),
    12: frozenset(range(11, 17)),
    13: frozenset(range(9, 11)),
    14: frozenset(),
    15: frozenset(range(11, 16)) | frozenset(range(31, 65)) | frozenset(range(66, 74)),
    16: frozenset(range(9, 32)),
}


def _route_is_3d_only_tim(directory_id: int, file_id: int, member: int | None) -> bool:
    if member is None:
        return False
    if directory_id == 0x30 and file_id == 0x05:
        return member in _BATTLING_3D_ONLY_TIM_MEMBERS
    if directory_id != 0x24:
        return False
    for configuration in range(17):
        base = 0x2B + configuration * 0x0B
        if file_id == base + 2:
            return member in range(6)
        if file_id == base + 3:
            return member in _WORLD_SHARED_3D_ONLY_TIM_MEMBERS[configuration]
    return False


def _tim_occurrence_is_3d_only(occurrence: dict) -> bool:
    routes = _route_ids(occurrence["routes"])
    member = _member_index(occurrence)
    return bool(routes) and all(
        _route_is_3d_only_tim(directory_id, file_id, member)
        for directory_id, file_id in routes
    )


def _tim_resource_is_3d_only(item: dict) -> bool:
    return bool(item["occurrences"]) and all(
        _tim_occurrence_is_3d_only(occurrence) for occurrence in item["occurrences"]
    )


def _route_classification(
    occurrence: dict, directory_id: int, file_id: int, kind: str, format_name: str | None,
) -> tuple[int, str, str]:
    member = _member_index(occurrence)
    if kind == "bitmap_font":
        return 1000, "fonts/dialogue", "dialogue-main"
    if format_name == "battling-selection-portrait" and directory_id == 0x30 and file_id == 0x06:
        return 1000, "images/portraits/battling", "battling-selection-portrait"
    if format_name in {"battling-fighter-hud-primary", "battling-fighter-hud-secondary"}:
        component = format_name.removeprefix("battling-fighter-hud-")
        return 1000, "images/ui/battling/fighters", f"battling-fighter-hud-{component}"
    if format_name in {"battle-common-ui-atlas-4bpp-view", "battle-common-ui-atlas-8bpp-view"}:
        depth = format_name.removeprefix("battle-common-ui-atlas-").removesuffix("-view")
        return 1000, "images/ui/battle/common-atlas", f"battle-common-ui-{depth}"
    if format_name == "battle-polygon-sfont-atlas":
        return 1000, "fonts/battle", "battle-polygon-sfont"
    if directory_id == 0x04 and 0x46 <= file_id <= 0xA0:
        return 1000, "images/portraits/dialogue", f"portrait-{file_id - 0x46:03d}"
    if directory_id == 0x04 and 0x7FB <= file_id <= 0x852:
        return 1000, "images/events/slides", f"slide-{file_id - 0x7FB:03d}"
    if directory_id == 0x04 and file_id == 0xAC:
        return 990, "images/logos", "publisher-title-production-ig"
    if directory_id == 0x04 and file_id == 0xA7:
        return 980, "images/ui/field", "field-common-ui"
    if directory_id == 0x04 and file_id == 0xAA:
        return 980, "images/ui/battle", "battle-gear-hud"
    if directory_id == 0x0C and file_id == 3:
        if member == 35:
            return 990, "images/portraits/battle", "battle-party-portrait"
        return 970, "images/ui/battle", "battle-common-ui"
    if directory_id == 0x10 and file_id == 1:
        return 970, "images/menu", "menu-common"
    if directory_id in {0x10, 0x11, 0x12}:
        return 850, "images/menu", "menu-image"
    if directory_id == 0x24 and file_id == 0x26:
        return 960, "images/menu", "menu-common-alias"
    if directory_id == 0x30 and file_id == 5:
        if member == 14:
            return 990, "fonts/battling", "battling-menu-glyph-atlas"
        if member in {21, 22, 23, 24}:
            return 980, "images/ui/battling", "battling-ui"
        return 950, "images/effects/battling", "battling-effect"
    if directory_id == 0x24:
        for configuration in range(17):
            base = 0x2B + configuration * 0x0B
            if file_id == base + 2:
                return 990, "images/world/unclassified", f"world-{configuration:02d}-ground-alias"
            if file_id == base + 3:
                return 980, "images/world/shared", f"world-{configuration:02d}-shared"
        return 700, "images/world", "world-image"
    if directory_id == 0x0F and 0x06 <= file_id <= 0x9A and file_id % 2 == 0:
        return 970, "images/battle/unclassified", f"arena-{(file_id - 6) // 2:02d}-image"
    if directory_id == 0x04:
        return 600, "images/field/unclassified", "field-image"
    if directory_id in {0x0C, 0x0D, 0x0E, 0x0F, 0x20, 0x28, 0x29, 0x2A, 0x2C, 0x2D}:
        return 600, "images/battle/unclassified", "battle-image"
    if directory_id in {0x30, 0x31}:
        return 600, "images/battling/unclassified", "battling-image"
    return 100, "images/unclassified", "image"


def _classification(record: dict) -> tuple[str, str, dict, dict | None]:
    choices = []
    for occurrence in record["occurrences"]:
        for route in occurrence["routes"]:
            directory_id = int(route["directory"], 16)
            file_id = int(route["file_id"], 16)
            score, category, label = _route_classification(
                occurrence, directory_id, file_id, record["kind"], record.get("format"),
            )
            choices.append((score, category, label, occurrence, route))
    if not choices:
        occurrence = min(record["occurrences"], key=_occurrence_sort_key)
        return "images/unclassified", "image", occurrence, None
    _, category, label, occurrence, route = max(
        choices,
        key=lambda choice: (choice[0], tuple(-value for value in _occurrence_sort_key(choice[3])[:2])),
    )
    return category, label, occurrence, route


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
        _hardlink_or_copy(source, destination)


def build_catalog(output: Path, records: list[dict]) -> dict:
    catalog_root = output / "catalog"
    if catalog_root.exists():
        shutil.rmtree(catalog_root)
    catalog_records = []
    for record in records:
        category, label, occurrence, route = _classification(record)
        route_suffix = ""
        if route is not None:
            route_suffix = f"_d{int(route['directory'], 16):02x}-f{int(route['file_id'], 16):03x}"
        folder = f"{label}{route_suffix}_{record['sha256'][:10]}"
        relative = Path("catalog") / Path(category) / folder
        destination = output / relative
        if record["kind"] == "tim_image":
            asset_name = "image.tim"
        elif record["kind"] == "bitmap_font" or record.get("format") == "battle-polygon-sfont-atlas":
            asset_name = "font.bin"
        else:
            asset_name = "image.bin"
        _relative_symlink(output / record["asset_path"], destination / asset_name)
        _relative_symlink(output / record["metadata_path"], destination / "metadata.json")
        for preview in record["previews"]:
            preview_relative = relative / "previews"
            if record["palette_variant_count"] > 1:
                preview_relative /= f"palette-{preview['palette_variant']:02d}"
            preview_relative /= (
                "glyph-atlas.png"
                if record["kind"] == "bitmap_font" or record.get("format") == "battle-polygon-sfont-atlas"
                else "image.png"
            )
            preview_destination = output / preview_relative
            preview_destination.parent.mkdir(parents=True, exist_ok=True)
            _hardlink_or_copy(output / preview["path"], preview_destination)
            preview["catalog_path"] = preview_relative.as_posix()
        source_record = {
            "sha256": record["sha256"],
            "kind": record["kind"],
            "occurrences": record["occurrences"],
        }
        write_if_changed(
            destination / "sources.json",
            (json.dumps(source_record, indent=2, ensure_ascii=True) + "\n").encode("ascii"),
        )
        record["catalog_path"] = relative.as_posix()
        catalog_records.append({
            "sha256": record["sha256"],
            "kind": record["kind"],
            "path": relative.as_posix(),
            "category": category,
            "width": record["width"],
            "height": record["height"],
            "palette_variant_count": record["palette_variant_count"],
        })
    catalog = {
        "schema": SCHEMA,
        "resource_count": len(catalog_records),
        "resources": sorted(catalog_records, key=lambda item: item["path"]),
    }
    write_if_changed(catalog_root / "catalog.json", (json.dumps(catalog, indent=2, ensure_ascii=True) + "\n").encode("ascii"))
    return catalog


def finalize_previews(output: Path, records: list[dict]) -> None:
    for record in records:
        for preview in record["previews"]:
            catalog_path = preview.get("catalog_path")
            if not catalog_path or not (output / catalog_path).is_file():
                raise ValueError("catalog preview was not materialized")
            preview["path"] = catalog_path
    preview_root = output / "previews"
    if preview_root.exists():
        shutil.rmtree(preview_root)


def extract_images(discs: Iterable[Path], output: Path) -> dict:
    disc_paths = list(discs)
    if not disc_paths:
        raise ValueError("at least one disc is required")
    output.mkdir(parents=True, exist_ok=True)
    tims: dict[str, dict] = {}
    raw_images: dict[str, dict] = {}
    fonts: dict[str, dict] = {}
    disc_records = []
    for disc_index, path in enumerate(disc_paths):
        disc_record, found_images, found_raw_images, found_fonts = scan_disc(path, disc_index)
        disc_records.append(disc_record)
        for tim, occurrence in found_images:
            digest = hashlib.sha256(tim.raw).hexdigest()
            if digest not in tims:
                tims[digest] = {"tim": tim, "occurrences": []}
            tims[digest]["occurrences"].append(occurrence)
        for image, occurrence in found_raw_images:
            digest = raw_image_sha256(image)
            key = f"{image.format}:{digest}"
            if key not in raw_images:
                raw_images[key] = {"digest": digest, "image": image, "occurrences": []}
            raw_images[key]["occurrences"].append(occurrence)
        for data, occurrence in found_fonts:
            digest = hashlib.sha256(data).hexdigest()
            if digest not in fonts:
                fonts[digest] = {"data": data, "occurrences": []}
            fonts[digest]["occurrences"].append(occurrence)
    excluded_tims = {
        digest: item for digest, item in tims.items()
        if _tim_resource_is_3d_only(item)
    }
    mixed_tims = {
        digest: item for digest, item in tims.items()
        if any(_tim_occurrence_is_3d_only(occurrence) for occurrence in item["occurrences"])
        and not _tim_resource_is_3d_only(item)
    }
    tims = {digest: item for digest, item in tims.items() if digest not in excluded_tims}
    excluded_by_disc = Counter(
        occurrence["disc_index"]
        for item in excluded_tims.values()
        for occurrence in item["occurrences"]
    )
    for disc_record in disc_records:
        excluded_count = excluded_by_disc[disc_record["disc_index"]]
        disc_record["scanned_tim_occurrences"] = disc_record["image_occurrences"]
        disc_record["excluded_3d_only_tim_occurrences"] = excluded_count
        disc_record["image_occurrences"] -= excluded_count
    records = [
        _materialize_tim(output, digest, item["tim"], item["occurrences"])
        for digest, item in sorted(tims.items())
    ]
    records.extend(
        _materialize_raw_image(
            output, item["digest"], item["image"], item["occurrences"],
        )
        for _, item in sorted(raw_images.items())
    )
    records.extend(
        _materialize_font(output, digest, item["data"], item["occurrences"])
        for digest, item in sorted(fonts.items())
    )
    records.sort(key=lambda record: record["sha256"])
    catalog = build_catalog(output, records)
    finalize_previews(output, records)
    summary = {
        "unique_resources": len(records),
        "unique_tim_images": len(tims),
        "excluded_3d_only_tim_images": len(excluded_tims),
        "excluded_3d_only_tim_occurrences": sum(excluded_by_disc.values()),
        "mixed_2d_3d_tim_images_preserved": len(mixed_tims),
        "unique_raw_images": len(raw_images),
        "unique_fonts": len(fonts) + sum(
            item["image"].format == "battle-polygon-sfont-atlas" for item in raw_images.values()
        ),
        "unique_main_bitmap_fonts": len(fonts),
        "occurrences": sum(len(record["occurrences"]) for record in records),
        "unique_png_previews": len({preview["sha256"] for record in records for preview in record["previews"]}),
        "preview_references": sum(len(record["previews"]) for record in records),
        "tim_by_bpp": dict(sorted(Counter(str(record["bpp"]) for record in records if record["kind"] == "tim_image").items())),
        "raw_images_by_format": dict(sorted(Counter(
            record["format"] for record in records
            if record["kind"] in {"raw_indexed_image", "composed_image"}
        ).items())),
        "by_kind": dict(sorted(Counter(record["kind"] for record in records).items())),
    }
    manifest = {
        "schema": SCHEMA,
        "exclusion_policy": {
            "exclusive_3d_model_and_terrain_textures": "excluded",
            "mixed_2d_and_3d_resources": "preserved",
            "unproven_consumers": "preserved",
        },
        "discs": disc_records,
        "summary": summary,
        "catalog": {"path": "catalog/catalog.json", "resource_count": catalog["resource_count"]},
        "resources": records,
    }
    write_if_changed(output / "manifest.json", (json.dumps(manifest, indent=2, ensure_ascii=True) + "\n").encode("ascii"))
    referenced = {"manifest.json"}
    for record in records:
        referenced.add(record["asset_path"])
        referenced.add(record["metadata_path"])
        referenced.update(preview["path"] for preview in record["previews"])
    for directory_name in ("assets", "metadata", "previews"):
        directory = output / directory_name
        if not directory.is_dir():
            continue
        for path in directory.iterdir():
            if path.is_file() and path.relative_to(output).as_posix() not in referenced:
                path.unlink()
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("discs", nargs="+", type=Path, help="one or more retail CUE, BIN, or ISO images")
    parser.add_argument(
        "--output", type=Path, default=Path("extracted-images"),
        help="output directory (default: extracted-images)",
    )
    args = parser.parse_args()
    manifest = extract_images(args.discs, args.output)
    print(
        f"Extracted {manifest['summary']['unique_tim_images']} unique TIM images and "
        f"{manifest['summary']['unique_raw_images']} raw/composed images and "
        f"{manifest['summary']['unique_fonts']} fonts from "
        f"{manifest['summary']['occurrences']} occurrences; "
        f"wrote {manifest['summary']['unique_png_previews']} unique PNG previews."
    )
    print(f"Manifest: {args.output / 'manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
