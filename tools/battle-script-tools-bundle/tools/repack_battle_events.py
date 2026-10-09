"""Battle Event LZSS compression, event data file rebuilding and runtime override packages."""

from __future__ import annotations

import hashlib
import json
import re
import struct
from collections import defaultdict, deque
from pathlib import Path

from extract_disc_battle_scripts import (
    EVENT_DATA_FILE, EVENT_DIRECTORY, MAX_RESOURCE_SIZE, lzss_decompress, parse_dialog_resource,
    parse_event_data_file, parse_script_resource, read_indexed_file,
)


def lzss_compress(payload: bytes, *, exact: bool = False) -> bytes:
    """Encode bytes for the resident decoder's complete eight-token groups.

    Greedy matches use distances 1..4095 and lengths 3..18. The decoder only
    tests for the declared end between groups, so the token count must be a
    multiple of eight. Splitting a match into literals changes the token count
    without changing any output byte; a small residue search finds such splits.
    If none completes the final group, at most seven zero trailer bytes are
    appended and counted in the declared size (they follow all live data).
    Set exact=True to reject such padding.
    """
    if not 1 <= len(payload) <= MAX_RESOURCE_SIZE:
        raise ValueError("LZSS payload size is outside the supported range")
    history: dict[bytes, deque[int]] = defaultdict(deque)
    tokens = []
    pc = 0
    while pc < len(payload):
        length, distance = 1, 0
        key = payload[pc:pc + 3]
        if len(key) == 3:
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
    for index, (start, length, distance) in enumerate(tokens):
        added = splits.get(index, 0)
        if added == length - 1 and added:
            adjusted.extend((start + offset, 1, 0) for offset in range(length))
        else:
            adjusted.extend((start + offset, 1, 0) for offset in range(added))
            adjusted.append((start + added, length - added, distance))
    encoded = bytearray(struct.pack("<I", len(payload)))
    for index in range(0, len(adjusted), 8):
        group = adjusted[index:index + 8]
        encoded.append(sum(1 << bit for bit, (_, _, distance) in enumerate(group) if distance))
        for start, length, distance in group:
            if distance:
                encoded.extend((distance & 0xFF, ((length - 3) << 4) | (distance >> 8)))
            else:
                encoded.append(payload[start])
    result = bytes(encoded)
    expanded, consumed = lzss_decompress(result)
    if expanded != payload or consumed != len(result):
        raise ValueError("internal LZSS round-trip failure")
    return result


def rebuild_event_data_file(container: bytes, logical_size: int, replacements: dict[int, dict[str, bytes]]) -> bytes:
    """Rebuild directory 0x20 file 2 with replaced script and/or dialogue streams.

    ``replacements`` maps a Battle Event data index to {"script": ..., "dialog": ...}.
    Unchanged streams keep their exact stored bytes. A retail stream may read
    up to seven bytes from the following stream; those bytes are copied into the
    stream itself so its expanded payload does not depend on its neighbor.
    """
    parsed = parse_event_data_file(container, logical_size)
    count = parsed["count"]
    for event_index, payloads in replacements.items():
        if not 0 <= event_index < count // 2:
            raise ValueError(f"Battle Event {event_index} does not exist (0..{count // 2 - 1})")
        if set(payloads) - {"script", "dialog"}:
            raise ValueError("replacements accept only script and dialog payloads")
        if "script" in payloads:
            parse_script_resource(payloads["script"])
        if "dialog" in payloads:
            parse_dialog_resource(payloads["dialog"])
    expected = [stream["payload"] for stream in parsed["streams"]]
    streams = []
    for index, stream in enumerate(parsed["streams"]):
        kind = "script" if index % 2 == 0 else "dialog"
        replacement = replacements.get(index // 2, {}).get(kind)
        if replacement is not None:
            stream_bytes = lzss_compress(replacement)
            streams.append(stream_bytes)
            expected[index] = lzss_decompress(stream_bytes)[0]
        else:
            start = stream["offset"]
            streams.append(container[start:start + max(stream["stored_size"], stream["consumed_size"])])
    table_size = 4 + (count + 1) * 4
    offsets = []
    body = bytearray()
    for stream in streams:
        offsets.append(table_size + len(body))
        body += stream
        body += bytes(-len(body) % 4)
    total = table_size + len(body)
    result = struct.pack("<I", count) + struct.pack(f"<{count + 1}I", *offsets, total) + bytes(body)
    check = parse_event_data_file(result)
    if [stream["payload"] for stream in check["streams"]] != expected:
        raise ValueError("internal event data rebuild round-trip failure")
    return result


def write_override(disc: Path, replacements: dict[int, dict[str, bytes]], output: Path, *,
                   disc_sha256: str, package_id: str, game_id: str) -> Path:
    """Create a format-6 source package replacing the Battle Event data file."""
    if not re.fullmatch(r"[0-9a-f]{64}", disc_sha256):
        raise ValueError("--disc-sha256 requires the runtime's lowercase canonical disc digest")
    if not re.fullmatch(r"[a-z0-9][a-z0-9._-]*", package_id):
        raise ValueError("invalid package ID")
    if game_id not in {"SLUS-00664", "SLUS-00669"}:
        raise ValueError("unsupported game ID (expected SLUS-00664 or SLUS-00669)")
    container, logical_size, index, _ = read_indexed_file(disc, EVENT_DIRECTORY, EVENT_DATA_FILE)
    replacement = rebuild_event_data_file(container, logical_size, replacements)
    payload_path = output / "assets" / "battle-event-data.bin"
    manifest_path = output / "manifest.toml"
    if manifest_path.exists() or payload_path.exists():
        raise ValueError("override output already exists; choose a new package directory")
    events = ", ".join(str(event) for event in sorted(replacements))
    quote = json.dumps
    manifest = f'''format_version = 6
id = {quote(package_id)}
version = "1.0.0"
name = "Battle Event script edit"
author = "Local"
description = "Recompiled Battle Event data ({events})"
license = "LicenseRef-Local"
resolver = "declarative"
save_compatibility = "shared"

[[target]]
game_id = {quote(game_id)}
disc_sha256 = {quote(disc_sha256)}

[[feature]]
id = "battle-event-script"
name = "Battle Event script edit"
description = "Replace directory 0x20 file 2 with recompiled Battle Event data ({events})"
default_enabled = false

[[indexed_file]]
feature = "battle-event-script"
format = "xenogears"
index = {index}
disc_sha256 = {quote(disc_sha256)}
file = "assets/{payload_path.name}"
sha256 = "{hashlib.sha256(replacement).hexdigest()}"
expected_sha256 = "{hashlib.sha256(container[:logical_size]).hexdigest()}"
'''
    payload_path.parent.mkdir(parents=True, exist_ok=True)
    payload_path.write_bytes(replacement)
    manifest_path.write_text(manifest, encoding="utf-8")
    return manifest_path
