"""Field LZSS compression, section replacement and runtime override packages."""

from __future__ import annotations

import hashlib
import json
import re
import struct
from collections import defaultdict, deque
from pathlib import Path

from extract_disc_field_scripts import (
    DIRECTORY_COUNT, DIRECTORY_LBA, FAT_LBA, FAT_SECTORS, FIELD_DIRECTORY,
    FIELD_FILE_BASE, FIELD_SECTION, MAX_SCRIPT_SECTION_SIZE, SECTION_OFFSET_TABLE,
    SECTION_SIZE_TABLE, USER_SECTOR, extract_container_scripts, lzss_decompress,
    open_disc, parse_directory_table, parse_fat, parse_scripts_file,
)


def lzss_compress(payload: bytes, *, exact: bool = False) -> bytes:
    """Encode exact bytes, including the loader's eight-token group rule.

    First choose greedy matches (distance 1..4095, length 3..18). Splitting a
    match into literals can change the token count without changing any output
    byte. A small residue DP finds such splits to complete the final group.
    If exact grouping is impossible, append at most seven zero trailer bytes.
    The stream header reports the padded size; no live script offsets change.
    Set exact=True to require byte equality, including the trailer.
    """
    if not 1 <= len(payload) <= MAX_SCRIPT_SECTION_SIZE:
        raise ValueError("LZSS payload size is outside the supported range")
    history: dict[bytes, deque[int]] = defaultdict(deque)
    tokens = []
    pc = 0
    while pc < len(payload):
        length, distance = 1, 0
        key = payload[pc:pc + 3]
        for previous in reversed(history.get(key, ())):
            if pc - previous > 4095:
                break
            count = 3
            while count < 18 and pc + count < len(payload) and payload[previous + count] == payload[pc + count]:
                count += 1
            if count > length:
                length, distance = count, pc - previous
            if length == 18:
                break
        # A short suffix cannot begin a back-reference.
        if len(key) < 3:
            length, distance = 1, 0
        tokens.append((pc, length, distance))
        for position in range(pc, pc + length):
            if position >= 4095:
                old_key = payload[position - 4095:position - 4092]
                queue = history.get(old_key)
                if queue:
                    queue.popleft()
                    if not queue:
                        del history[old_key]
            if position + 3 <= len(payload):
                history[payload[position:position + 3]].append(position)
        pc += length

    needed = -len(tokens) % 8
    paths: dict[int, list[tuple[int, int]]] = {0: []}
    for index, (_, length, distance) in enumerate(tokens):
        if needed in paths:
            break
        if not distance:
            continue
        choices = list(range(1, min(length - 3, 7) + 1)) + [length - 1]
        for residue, path in list(paths.items()):
            for added in choices:
                target = (residue + added) % 8
                if target not in paths:
                    paths[target] = path + [(index, added)]
    if needed not in paths:
        if exact:
            raise ValueError("payload cannot end on an eight-token LZSS boundary without padding")
        start = len(payload)
        payload += bytes(needed)
        tokens.extend((start + index, 1, 0) for index in range(needed))
        needed = 0
    splits = dict(paths[needed])
    adjusted = []
    for index, (pc, length, distance) in enumerate(tokens):
        added = splits.get(index, 0)
        if added == length - 1 and added:
            adjusted.extend((pc + i, 1, 0) for i in range(length))
        else:
            adjusted.extend((pc + i, 1, 0) for i in range(added))
            adjusted.append((pc + added, length - added, distance))
    encoded = bytearray(struct.pack("<I", len(payload)))
    for index in range(0, len(adjusted), 8):
        group = adjusted[index:index + 8]
        control = sum(1 << bit for bit, (_, _, distance) in enumerate(group) if distance)
        encoded.append(control)
        for pc, length, distance in group:
            if distance:
                encoded.extend((distance & 255, ((length - 3) << 4) | (distance >> 8)))
            else:
                encoded.append(payload[pc])
    result = bytes(encoded)
    if lzss_decompress(result)[0] != payload:
        raise ValueError("internal LZSS round-trip failure")
    return result


def repack_container(container: bytes, scripts: bytes, *, logical_size: int | None = None) -> bytes:
    logical_size = len(container) if logical_size is None else logical_size
    _, _, metadata = extract_container_scripts(container, logical_size)
    parse_scripts_file(scripts)
    compressed = lzss_compress(scripts)
    expanded_size = struct.unpack_from("<I", compressed)[0]
    # The section directory contains aligned stream starts; retain this property
    # without including alignment bytes in the expanded ScriptsFile.
    compressed += bytes(-len(compressed) % 4)
    offsets = metadata["section_offsets"]
    start, end = offsets[FIELD_SECTION:FIELD_SECTION + 2]
    replacement = bytearray(container[:start] + compressed + container[end:logical_size])
    delta = len(compressed) - (end - start)
    struct.pack_into("<I", replacement, SECTION_SIZE_TABLE + FIELD_SECTION * 4, expanded_size)
    for index in range(FIELD_SECTION + 1, len(offsets)):
        struct.pack_into("<I", replacement, SECTION_OFFSET_TABLE + index * 4, offsets[index] + delta)
    result = bytes(replacement)
    if extract_container_scripts(result)[0] != scripts + bytes(expanded_size - len(scripts)):
        raise ValueError("internal Field repack round-trip failure")
    return result


def read_field_container(disc_path: Path, field_id: int) -> tuple[bytes, int, int]:
    if field_id < 0:
        raise ValueError("Field ID must be non-negative")
    disc = open_disc(disc_path)
    directory = parse_directory_table(disc.read_user_data(DIRECTORY_LBA, DIRECTORY_COUNT * 2))
    entries = parse_fat(disc.read_user_data(FAT_LBA, FAT_SECTORS * USER_SECTOR), disc.sector_count)
    start = directory[FIELD_DIRECTORY] - 1
    file_id = FIELD_FILE_BASE + field_id * 2
    index = start + file_id - 1
    next_starts = [encoded - 1 for encoded in directory if encoded - 1 > start]
    end = min(next_starts, default=len(entries))
    if start < 0 or not start <= index < end or entries[index].size <= 0:
        raise ValueError(f"Field {field_id} is not a stored file in directory 4")
    entry = entries[index]
    return disc.read_user_data(entry.lba, entry.size, padded=True), entry.size, index


def write_override(disc: Path, field_id: int, scripts: bytes, output: Path, *, disc_sha256: str, package_id: str, game_id: str) -> Path:
    """Create a format-6 source package for the existing indexed-file runtime."""
    if not re.fullmatch(r"[0-9a-f]{64}", disc_sha256):
        raise ValueError("--disc-sha256 requires the runtime's lowercase canonical disc digest")
    if not re.fullmatch(r"[a-z0-9][a-z0-9._-]*", package_id):
        raise ValueError("invalid package ID")
    if game_id not in {"SLUS-00664", "SLUS-00669"}:
        raise ValueError("unsupported game ID (expected SLUS-00664 or SLUS-00669)")
    container, logical_size, index = read_field_container(disc, field_id)
    replacement = repack_container(container, scripts, logical_size=logical_size)
    payload_path = output / "assets" / f"field-{field_id:04d}.bin"
    manifest_path = output / "manifest.toml"
    if manifest_path.exists() or payload_path.exists():
        raise ValueError("override output already exists; choose a new package directory")
    def digest(data: bytes) -> str:
        return hashlib.sha256(data).hexdigest()

    quote = json.dumps
    manifest = f'''format_version = 6
id = {quote(package_id)}
version = "1.0.0"
name = "Field {field_id} script edit"
author = "Local"
description = "Recompiled Field script"
license = "LicenseRef-Local"
resolver = "declarative"
save_compatibility = "shared"

[[target]]
game_id = {quote(game_id)}
disc_sha256 = {quote(disc_sha256)}

[[feature]]
id = "field-script"
name = "Field {field_id} script edit"
description = "Replace the Field container with its recompiled script section"
default_enabled = false

[[indexed_file]]
feature = "field-script"
format = "xenogears"
index = {index}
disc_sha256 = {quote(disc_sha256)}
file = "assets/{payload_path.name}"
sha256 = "{digest(replacement)}"
expected_sha256 = "{digest(container[:logical_size])}"
'''
    payload_path.parent.mkdir(parents=True, exist_ok=True)
    payload_path.write_bytes(replacement)
    manifest_path.write_text(manifest, encoding="utf-8")
    return manifest_path
