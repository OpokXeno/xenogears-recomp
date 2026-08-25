#!/usr/bin/env python3
"""Inventory executable-image candidates in the Xenogears indexed filesystem."""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
import struct
import tomllib
from bisect import bisect_left, bisect_right
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

from extract_disc_overlays import DEFAULT_MANIFEST, open_disc


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DISC = ROOT / "game" / "disc1.cue"
FAT_LBA = 0x18
FAT_SECTORS = 0x10
DIRECTORY_LBA = 0x28
DIRECTORY_COUNT = 64
SECTOR_SIZE = 2048
SENTINEL_LBA = 0xFFFFFF
DEFAULT_BASE_LO = 0x80010000
DEFAULT_BASE_HI = 0x80800000
DEFAULT_MAX_IMAGE_SIZE = 8 * 1024 * 1024
KNOWN_DISC1_SHA256 = "39c547a9afc6da15d847ef81a2c6cea1a6516bdfa562cf13b0999b04e8598bda"


@dataclass(frozen=True)
class FatEntry:
    lba: int
    size: int


@dataclass(frozen=True)
class LzssResult:
    payload: bytes
    consumed: int
    truncated: bool


def parse_fat_table(data: bytes, sector_count: int) -> tuple[list[FatEntry], int]:
    entries: list[FatEntry] = []
    for offset in range(0, len(data) - 6, 7):
        lba = int.from_bytes(data[offset : offset + 3], "little")
        size = struct.unpack_from("<i", data, offset + 3)[0]
        if lba == SENTINEL_LBA and size == 0:
            if not entries or entries[0].size >= 0:
                raise ValueError("Xenogears FAT has no negative XA root entry")
            xa_children = -entries[0].size
            if xa_children > len(entries) - 1:
                raise ValueError("Xenogears XA root exceeds the FAT")
            return entries, xa_children
        if lba == SENTINEL_LBA:
            raise ValueError("Xenogears FAT contains a malformed sentinel entry")
        if size > 0:
            end_lba = lba + (size + SECTOR_SIZE - 1) // SECTOR_SIZE
            if lba >= sector_count or end_lba > sector_count:
                raise ValueError(
                    f"Xenogears FAT extent at index {len(entries)} is outside the disc"
                )
        entries.append(FatEntry(lba, size))
    raise ValueError("Xenogears FAT sentinel was not found")


def parse_directory_table(data: bytes) -> list[int]:
    required = DIRECTORY_COUNT * 2
    if len(data) < required:
        raise ValueError("Xenogears directory table is truncated")
    return list(struct.unpack_from(f"<{DIRECTORY_COUNT}H", data))


def map_physical_routes(directory_table: list[int], entry_count: int) -> dict[int, list[dict]]:
    starts: dict[int, list[int]] = {}
    for directory, encoded_start in enumerate(directory_table):
        if encoded_start == 0:
            continue
        start = encoded_start - 1
        if 0 <= start < entry_count:
            starts.setdefault(start, []).append(directory)

    ordered_starts = sorted(starts)
    routes: dict[int, list[dict]] = {}
    for position, start in enumerate(ordered_starts):
        end = ordered_starts[position + 1] if position + 1 < len(ordered_starts) else entry_count
        for index in range(start, end):
            file_id = index - start + 1
            routes[index] = [
                {"directory": f"0x{directory:02X}", "file_id": f"0x{file_id:02X}"}
                for directory in starts[start]
            ]
    return routes


def lzss_decompress_with_status(
    data: bytes,
    offset: int = 0,
    max_size: int = DEFAULT_MAX_IMAGE_SIZE,
) -> LzssResult:
    if offset < 0 or offset + 4 > len(data):
        raise ValueError("LZSS stream has no expanded-size header")
    target_size = int.from_bytes(data[offset : offset + 4], "little")
    if target_size <= 0 or target_size > max_size:
        raise ValueError("LZSS expanded size is outside the configured image aperture")

    cursor = offset + 4
    window = bytearray(4096)
    window_at = 0xFEE
    output = bytearray()
    control = 0
    truncated = False

    def read_or(fallback: int) -> int:
        nonlocal cursor, truncated
        if cursor >= len(data):
            truncated = True
            return fallback
        value = data[cursor]
        cursor += 1
        return value

    def emit(value: int) -> None:
        nonlocal window_at
        if len(output) >= target_size:
            return
        output.append(value)
        window[window_at] = value
        window_at = (window_at + 1) & 0xFFF

    while len(output) < target_size:
        control = read_or(control)
        for bit in range(8):
            if control & (1 << bit):
                low = read_or(0)
                high_length = read_or(low)
                distance = low | ((high_length & 0x0F) << 8)
                for _ in range((high_length >> 4) + 3):
                    emit(window[(window_at - distance) & 0xFFF])
            else:
                emit(read_or(0))
            if len(output) >= target_size:
                break
    return LzssResult(bytes(output), min(cursor, len(data)) - offset, truncated)


def packet_offsets(data: bytes, max_members: int = 4096) -> list[int] | None:
    if len(data) < 12:
        return None
    count = int.from_bytes(data[:4], "little")
    header_size = 4 * count + 8
    if count == 0 or count > max_members or header_size > len(data):
        return None
    offsets = [
        int.from_bytes(data[4 + index * 4 : 8 + index * 4], "little")
        for index in range(count + 1)
    ]
    if (
        offsets[0] != header_size
        or offsets[-1] != len(data)
        or any(offset < header_size or offset > len(data) for offset in offsets)
        or any(left > right for left, right in zip(offsets, offsets[1:]))
    ):
        return None
    return offsets


def prologue_offsets(data: bytes) -> list[int]:
    return [
        offset
        for offset in range(0, len(data) - 3, 4)
        if (struct.unpack_from("<I", data, offset)[0] >> 16) == 0x27BD
        and struct.unpack_from("<I", data, offset)[0] & 0x8000
    ]


def jal_targets(data: bytes) -> set[int]:
    targets = set()
    for offset in range(0, len(data) - 3, 4):
        word = struct.unpack_from("<I", data, offset)[0]
        if word >> 26 == 0x03:
            targets.add(0x80000000 | ((word & 0x03FFFFFF) << 2))
    return targets


def raw_base_votes(
    data: bytes,
    base_lo: int = DEFAULT_BASE_LO,
    base_hi: int = DEFAULT_BASE_HI,
) -> Counter[int]:
    prologues = prologue_offsets(data)
    targets = jal_targets(data)
    votes: Counter[int] = Counter()
    max_base = base_hi - len(data)
    if not prologues or not targets or max_base < base_lo:
        return votes
    for target in targets:
        first = bisect_left(prologues, target - max_base)
        last = bisect_right(prologues, target - base_lo)
        for offset in prologues[first:last]:
            base = target - offset
            if base & 3 == 0:
                votes[base] += 1
    return votes


def analyze_mips(
    data: bytes,
    base_lo: int = DEFAULT_BASE_LO,
    base_hi: int = DEFAULT_BASE_HI,
) -> dict:
    prologues = prologue_offsets(data)
    targets = jal_targets(data)
    returns = sum(
        struct.unpack_from("<I", data, offset)[0] == 0x03E00008
        for offset in range(0, len(data) - 3, 4)
    )
    votes = raw_base_votes(data, base_lo, base_hi) if returns else Counter()
    ranked = votes.most_common(2)
    top_base, top_score = ranked[0] if ranked else (None, 0)
    runner_up = ranked[1][1] if len(ranked) > 1 else 0
    recovered = (
        top_base
        if top_score >= 8
        and top_score >= runner_up * 2
        and top_score * 4 >= len(prologues)
        else None
    )
    if recovered is not None:
        signal = "strong"
    elif returns >= 2 and len(prologues) >= 2:
        signal = "weak"
    elif returns >= 1 and len(prologues) >= 1:
        signal = "weak"
    else:
        signal = "none"
    return {
        "prologues": len(prologues),
        "returns": returns,
        "jal_targets": len(targets),
        "base_candidate": f"0x{top_base:08X}" if top_base is not None else None,
        "base_score": top_score,
        "base_runner_up": runner_up,
        "recovered_base": f"0x{recovered:08X}" if recovered is not None else None,
        "signal": signal,
    }


def _hashes(data: bytes) -> tuple[str, str]:
    return hashlib.sha256(data).hexdigest(), f"{binascii.crc32(data) & 0xFFFFFFFF:08X}"


def _representation(
    kind: str,
    data: bytes,
    base_lo: int,
    base_hi: int,
    **extra: object,
) -> dict:
    sha256, crc32 = _hashes(data)
    return {
        "kind": kind,
        "size": len(data),
        "sha256": sha256,
        "crc32": crc32,
        "mips": analyze_mips(data, base_lo, base_hi),
        **extra,
    }


def _psx_exe_representation(data: bytes, base_lo: int, base_hi: int) -> dict | None:
    if len(data) < 0x800 or data[:8] != b"PS-X EXE":
        return None
    entry_pc = struct.unpack_from("<I", data, 0x10)[0]
    load_address = struct.unpack_from("<I", data, 0x18)[0]
    declared_size = struct.unpack_from("<I", data, 0x1C)[0]
    if declared_size <= 0 or declared_size > len(data) - 0x800:
        return None
    return _representation(
        "ps-x-exe-body",
        data[0x800 : 0x800 + declared_size],
        base_lo,
        base_hi,
        load_address=f"0x{load_address:08X}",
        entry_pc=f"0x{entry_pc:08X}",
    )


def _manifest_images(path: Path) -> list[dict]:
    document = tomllib.loads(path.read_text(encoding="utf-8"))
    if document.get("schema") != "xenogears-disc-overlay-images/v1":
        raise ValueError(f"{path}: unsupported disc image manifest schema")
    images = document.get("images")
    if not isinstance(images, list):
        raise ValueError(f"{path}: images must be an array")
    return images


def _analyse_payload(
    data: bytes,
    manifest_images: list[dict],
    lba: int,
    size: int,
    max_image_size: int,
    base_lo: int,
    base_hi: int,
) -> dict:
    stored_sha256, stored_crc32 = _hashes(data)
    signatures = []
    if data.startswith(b"wds "):
        signatures.append("wds-audio")
    if data.startswith(b"PS-X EXE"):
        signatures.append("ps-x-exe")

    representations = [_representation("raw", data, base_lo, base_hi)]
    padded = data + bytes(-len(data) % SECTOR_SIZE)
    if padded != data:
        representations.append(_representation("raw-zero-pad", padded, base_lo, base_hi))

    exe = _psx_exe_representation(data, base_lo, base_hi)
    if exe is not None:
        representations.append(exe)

    lzss_status = "not-applicable"
    if len(data) >= 4:
        declared_size = int.from_bytes(data[:4], "little")
        if 0 < declared_size <= max_image_size:
            try:
                expanded = lzss_decompress_with_status(data, max_size=max_image_size)
                lzss_status = "guest-compatible-truncated" if expanded.truncated else "complete"
                representations.append(
                    _representation(
                        "lzss",
                        expanded.payload,
                        base_lo,
                        base_hi,
                        stream_status=lzss_status,
                        consumed=expanded.consumed,
                        trailing=max(0, len(data) - expanded.consumed),
                    )
                )
            except ValueError:
                lzss_status = "malformed"

    offsets = packet_offsets(data)
    packet_candidates = []
    if offsets is not None:
        signatures.append("packet-container")
        for member_index, (start, end) in enumerate(zip(offsets, offsets[1:])):
            try:
                expanded = lzss_decompress_with_status(data, start, max_image_size)
            except ValueError:
                continue
            consumed_end = start + expanded.consumed
            if consumed_end > end:
                continue
            member = _representation(
                f"packet-lzss:{member_index}",
                expanded.payload,
                base_lo,
                base_hi,
                stream_status=(
                    "guest-compatible-truncated" if expanded.truncated else "complete"
                ),
            )
            if member["mips"]["signal"] != "none":
                packet_candidates.append(member)

    physical_matches = [
        image
        for image in manifest_images
        if image.get("source_sector") == lba
        and image.get("stored_size") == size
        and image.get("stored_sha256") == stored_sha256
    ]
    loaded_matches = []
    for representation in representations:
        for image in manifest_images:
            if (
                representation["sha256"] == image.get("sha256")
                and representation["size"] == image.get("size")
            ):
                loaded_matches.append(
                    {"image_id": image["id"], "representation": representation["kind"]}
                )
    loaded_matches = sorted(
        {tuple(match.items()) for match in loaded_matches}, key=lambda item: tuple(item)
    )
    loaded_matches = [dict(match) for match in loaded_matches]

    authenticated = []
    for image in physical_matches:
        expected_kind = image.get("storage")
        accepted_kinds = {expected_kind}
        if expected_kind == "raw-zero-pad" and size % SECTOR_SIZE == 0:
            accepted_kinds.add("raw")
        if any(
            representation["kind"] in accepted_kinds
            and representation["sha256"] == image.get("sha256")
            and representation["size"] == image.get("size")
            for representation in representations
        ):
            authenticated.append(image["id"])

    strongest = max(
        (representation["mips"]["signal"] for representation in representations),
        key={"none": 0, "weak": 1, "strong": 2}.get,
    )
    if authenticated:
        classification = "authenticated"
    elif loaded_matches:
        classification = "payload-alias"
    elif exe is not None:
        classification = "self-described-executable"
    elif strongest == "strong":
        classification = "strong-mips-candidate"
    elif strongest == "weak":
        classification = "weak-mips-candidate"
    elif packet_candidates:
        classification = "packet-code-candidate"
    elif signatures:
        classification = "data/container"
    else:
        classification = "unclassified"

    return {
        "stored_sha256": stored_sha256,
        "stored_crc32": stored_crc32,
        "signatures": signatures,
        "lzss_status": lzss_status,
        "representations": representations,
        "packet_member_count": len(offsets) - 1 if offsets is not None else 0,
        "packet_code_candidates": packet_candidates,
        "authenticated_images": sorted(authenticated),
        "payload_matches": loaded_matches,
        "classification": classification,
    }


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def build_census(
    disc_path: Path,
    manifest_path: Path = DEFAULT_MANIFEST,
    max_image_size: int = DEFAULT_MAX_IMAGE_SIZE,
    base_lo: int = DEFAULT_BASE_LO,
    base_hi: int = DEFAULT_BASE_HI,
) -> dict:
    if max_image_size <= 0:
        raise ValueError("max image size must be positive")
    if base_lo < 0x80000000 or base_hi <= base_lo:
        raise ValueError("invalid MIPS base search aperture")
    disc = open_disc(disc_path)
    sector_count = disc.path.stat().st_size // disc.sector_size
    fat_bytes = disc.read_user_data(FAT_LBA, FAT_SECTORS * SECTOR_SIZE)
    entries, xa_children = parse_fat_table(fat_bytes, sector_count)
    directory_table = parse_directory_table(
        disc.read_user_data(DIRECTORY_LBA, DIRECTORY_COUNT * 2)
    )
    routes = map_physical_routes(directory_table, len(entries))
    manifest_images = _manifest_images(manifest_path)

    records = []
    analysis_cache: dict[tuple[int, int], dict] = {}
    for index, entry in enumerate(entries):
        if index == 0:
            kind = "xa-root"
        elif index <= xa_children:
            kind = "xa-child"
        elif entry.size < 0:
            kind = "group-marker"
        elif entry.size == 0:
            kind = "empty"
        else:
            kind = "file"
        record = {
            "index": index,
            "lba": entry.lba,
            "signed_size": entry.size,
            "kind": kind,
            "physical_routes": routes.get(index, []),
        }
        if kind == "file":
            if entry.size > max_image_size:
                record.update(
                    {
                        "classification": "analysis-skipped",
                        "skip_reason": "stored extent exceeds configured image aperture",
                    }
                )
            else:
                cache_key = (entry.lba, entry.size)
                if cache_key not in analysis_cache:
                    payload = disc.read_user_data(entry.lba, entry.size)
                    analysis_cache[cache_key] = _analyse_payload(
                        payload,
                        manifest_images,
                        entry.lba,
                        entry.size,
                        max_image_size,
                        base_lo,
                        base_hi,
                    )
                record.update(analysis_cache[cache_key])
        records.append(record)

    classifications = Counter(
        record.get("classification", record["kind"]) for record in records
    )
    candidate_records = [
        record["index"]
        for record in records
        if record.get("classification")
        in {
            "authenticated",
            "payload-alias",
            "self-described-executable",
            "strong-mips-candidate",
            "weak-mips-candidate",
            "packet-code-candidate",
        }
    ]
    disc_sha256 = _file_sha256(disc.path)
    return {
        "schema": "xenogears-disc-overlay-census/v1",
        "disc_file": disc.path.name,
        "disc_sha256": disc_sha256,
        "known_disc1": disc_sha256 == KNOWN_DISC1_SHA256,
        "sector_size": disc.sector_size,
        "user_data_offset": disc.user_offset,
        "sector_count": sector_count,
        "fat_entry_count": len(entries),
        "xa_children": xa_children,
        "directory_count": len(directory_table),
        "base_search_aperture": [f"0x{base_lo:08X}", f"0x{base_hi:08X}"],
        "max_image_size": max_image_size,
        "summary": {
            "classifications": dict(sorted(classifications.items())),
            "candidate_record_count": len(candidate_records),
            "candidate_records": candidate_records,
        },
        "records": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disc", type=Path, default=DEFAULT_DISC)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--max-image-size", type=lambda value: int(value, 0), default=DEFAULT_MAX_IMAGE_SIZE)
    parser.add_argument("--base-lo", type=lambda value: int(value, 0), default=DEFAULT_BASE_LO)
    parser.add_argument("--base-hi", type=lambda value: int(value, 0), default=DEFAULT_BASE_HI)
    args = parser.parse_args()

    census = build_census(
        args.disc,
        args.manifest,
        args.max_image_size,
        args.base_lo,
        args.base_hi,
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(census, indent=2) + "\n", encoding="ascii")
    summary = census["summary"]
    print(
        f"Indexed {census['fat_entry_count']} FAT records; "
        f"flagged {summary['candidate_record_count']} executable-image records."
    )
    print(json.dumps(summary["classifications"], sort_keys=True))
    print(f"Wrote byte-free census metadata to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
