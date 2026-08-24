#!/usr/bin/env python3
from __future__ import annotations

import argparse
import binascii
import hashlib
import re
import sys
import tomllib
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INDEX = ROOT / "annotations" / "overlays" / "index.toml"
SCHEMA = "xenogears-overlay-annotations/v1"
REQUIRED_IMAGE_KEYS = {
    "id",
    "subsystem",
    "logical_name",
    "sha256",
    "size",
    "load_address",
    "image_format",
    "header_size",
    "loaded_size",
    "annotations",
    "identity_source",
    "source_kind",
    "source_record_id",
}
OPTIONAL_IMAGE_KEYS = {"crc32", "artifact"}
HEX_ADDRESS = re.compile(r"0x[0-9a-fA-F]{8}$")
SHA256 = re.compile(r"[0-9a-f]{64}$")
CRC32 = re.compile(r"[0-9a-f]{8}$")
NOTE = re.compile(r"^(function start|call site|instruction site|return site) — .+")
CSV_METADATA = {"image-id", "sha256", "load-address", "identity-source", "source-record-id"}


def _integer(value: object) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError:
            return None
    return None


def _repo_path(root: Path, value: object, label: str, errors: list[str]) -> Path | None:
    if not isinstance(value, str) or not value or Path(value).is_absolute():
        errors.append(f"{label}: path must be a non-empty repository-relative string")
        return None
    path = (root / value).resolve()
    try:
        path.relative_to(root.resolve())
    except ValueError:
        errors.append(f"{label}: path escapes the repository: {value!r}")
        return None
    return path


def _validate_source_record(
    image: dict[str, Any], source_path: Path, label: str, errors: list[str]
) -> None:
    try:
        source = tomllib.loads(source_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, tomllib.TOMLDecodeError) as exc:
        errors.append(f"{label}: cannot parse identity source: {exc}")
        return

    kind = image["source_kind"]
    record_id = image["source_record_id"]
    record: dict[str, Any] | None = None
    fields: dict[str, str] = {}
    if kind.startswith("render-manifest-"):
        subsystem = kind.removeprefix("render-manifest-")
        candidate = source.get("overlay", {}).get(subsystem)
        if isinstance(candidate, dict) and candidate.get("id") == record_id:
            record = candidate
            fields = {
                "sha256": "full_sha256",
                "size": "full_size",
                "load_address": "base_address",
                "image_format": "image_format",
                "header_size": "header_size",
                "loaded_size": "loaded_size",
            }
    elif kind == "runtime-artifact":
        variants = source.get("variants", [])
        if any(isinstance(item, dict) and item.get("id") == record_id for item in variants):
            candidate = source.get("artifact")
            if isinstance(candidate, dict):
                record = candidate
                fields = {
                    "sha256": "full_sha256",
                    "size": "full_size",
                    "load_address": "base_address",
                }
    elif kind == "overlay-range-variant":
        for candidate in source.get("variants", []):
            if isinstance(candidate, dict) and candidate.get("id") == record_id:
                record = candidate
                fields = {
                    "sha256": "artifact_sha256",
                    "size": "artifact_size",
                    "load_address": "base_address",
                }
                break
    else:
        errors.append(f"{label}: unsupported source_kind {kind!r}")
        return

    if record is None:
        errors.append(f"{label}: source record {record_id!r} was not found")
        return
    for image_key, source_key in fields.items():
        expected = image[image_key]
        actual = record.get(source_key)
        if image_key in {"size", "header_size", "loaded_size"}:
            actual = _integer(actual)
        elif image_key == "load_address":
            actual_int = _integer(actual)
            actual = f"0x{actual_int:08x}" if actual_int is not None else actual
            expected_int = _integer(expected)
            expected = f"0x{expected_int:08x}" if expected_int is not None else expected
        if actual != expected:
            errors.append(
                f"{label}: {image_key} does not match {source_key} in source record {record_id!r}"
            )


def _parse_csv(path: Path) -> tuple[dict[str, str], list[tuple[int, str]], list[str]]:
    metadata: dict[str, str] = {}
    rows: list[tuple[int, str]] = []
    errors: list[str] = []

    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("#"):
            match = re.match(r"#\s*([a-z0-9-]+):\s*(.+)$", line)
            if match:
                key = match.group(1)
                if key in metadata:
                    errors.append(f"{path}:{line_number}: duplicate metadata key {key!r}")
                metadata[key] = match.group(2).strip()
            continue
        if "," not in line:
            errors.append(f"{path}:{line_number}: expected 'address, note'")
            continue
        address_text, note = line.split(",", 1)
        address_text = address_text.strip()
        note = note.strip()
        if not HEX_ADDRESS.fullmatch(address_text):
            errors.append(f"{path}:{line_number}: invalid address {address_text!r}")
            continue
        if not NOTE.fullmatch(note):
            errors.append(f"{path}:{line_number}: note lacks an exact approved type prefix")
        rows.append((int(address_text, 16), note))

    unknown_metadata = set(metadata) - CSV_METADATA - {"format"}
    if unknown_metadata:
        errors.append(f"{path}: unknown metadata: {', '.join(sorted(unknown_metadata))}")
    return metadata, rows, errors


def validate_index(index_path: Path = DEFAULT_INDEX, root: Path = ROOT) -> list[str]:
    errors: list[str] = []
    try:
        document = tomllib.loads(index_path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as exc:
        return [f"{index_path}: {exc}"]

    if set(document) != {"schema", "images"}:
        errors.append(f"{index_path}: expected only schema and images at top level")
    if document.get("schema") != SCHEMA:
        errors.append(f"{index_path}: unsupported schema {document.get('schema')!r}")
    images = document.get("images")
    if not isinstance(images, list) or not images:
        return errors + [f"{index_path}: images must be a non-empty array"]

    seen_ids: set[str] = set()
    seen_identities: set[tuple[str, int, int]] = set()
    seen_csv_paths: set[Path] = set()

    for position, image in enumerate(images, 1):
        label = f"{index_path}:images[{position}]"
        if not isinstance(image, dict):
            errors.append(f"{label}: image entry must be a table")
            continue
        keys = set(image)
        missing = REQUIRED_IMAGE_KEYS - keys
        unknown = keys - REQUIRED_IMAGE_KEYS - OPTIONAL_IMAGE_KEYS
        if missing:
            errors.append(f"{label}: missing keys: {', '.join(sorted(missing))}")
        if unknown:
            errors.append(f"{label}: unknown keys: {', '.join(sorted(unknown))}")
        if missing:
            continue

        image_id = image["id"]
        if not isinstance(image_id, str) or not image_id:
            errors.append(f"{label}: id must be a non-empty string")
            continue
        if image_id in seen_ids:
            errors.append(f"{label}: duplicate image id {image_id!r}")
        seen_ids.add(image_id)

        sha256 = image["sha256"]
        if not isinstance(sha256, str) or not SHA256.fullmatch(sha256):
            errors.append(f"{label}: sha256 must be 64 lowercase hexadecimal characters")
            continue
        crc32 = image.get("crc32")
        if crc32 is not None and (not isinstance(crc32, str) or not CRC32.fullmatch(crc32)):
            errors.append(f"{label}: crc32 must be 8 lowercase hexadecimal characters")

        if not isinstance(image["subsystem"], str) or image["subsystem"] not in {
            "field",
            "world",
            "battle",
            "battling",
            "menu",
            "movie",
            "member-change-menu",
            "shop-menu",
            "gear-shop-menu",
            "gear-helper",
            "battle-event",
        }:
            errors.append(f"{label}: unsupported subsystem {image['subsystem']!r}")
        if not isinstance(image["logical_name"], str) or not image["logical_name"]:
            errors.append(f"{label}: logical_name must be a non-empty string")
        if not isinstance(image["source_kind"], str) or not image["source_kind"]:
            errors.append(f"{label}: source_kind must be a non-empty string")
        if not isinstance(image["source_record_id"], str) or not image["source_record_id"]:
            errors.append(f"{label}: source_record_id must be a non-empty string")
        if not isinstance(image["image_format"], str) or image["image_format"] not in {"raw", "ps-x-exe"}:
            errors.append(f"{label}: unsupported image_format {image['image_format']!r}")
        if not isinstance(image["size"], int) or isinstance(image["size"], bool) or image["size"] <= 0:
            errors.append(f"{label}: size must be positive")
            continue
        if not isinstance(image["header_size"], int) or isinstance(image["header_size"], bool) or image["header_size"] < 0:
            errors.append(f"{label}: header_size must be non-negative")
            continue
        if not isinstance(image["loaded_size"], int) or isinstance(image["loaded_size"], bool) or image["loaded_size"] <= 0:
            errors.append(f"{label}: loaded_size must be positive")
            continue
        if image["size"] != image["header_size"] + image["loaded_size"]:
            errors.append(f"{label}: size must equal header_size + loaded_size")
        if image["image_format"] == "raw" and image["header_size"] != 0:
            errors.append(f"{label}: raw images must have header_size 0")
        if image["image_format"] == "ps-x-exe" and image["header_size"] != 2048:
            errors.append(f"{label}: ps-x-exe images must have a 2048-byte header")

        load_text = image["load_address"]
        if not isinstance(load_text, str) or not HEX_ADDRESS.fullmatch(load_text):
            errors.append(f"{label}: invalid load_address {load_text!r}")
            continue
        load_address = int(load_text, 16)
        identity = (sha256, image["size"], load_address)
        if identity in seen_identities:
            errors.append(f"{label}: duplicate binary identity")
        seen_identities.add(identity)

        source_path = _repo_path(root, image["identity_source"], f"{label}:identity_source", errors)
        if source_path is None:
            continue
        if not source_path.is_file():
            errors.append(f"{label}: identity source does not exist: {source_path}")
        else:
            _validate_source_record(image, source_path, label, errors)

        csv_path = _repo_path(root, image["annotations"], f"{label}:annotations", errors)
        if csv_path is None:
            continue
        if csv_path in seen_csv_paths:
            errors.append(f"{label}: annotation CSV is reused by multiple images")
        seen_csv_paths.add(csv_path)
        if not csv_path.is_file():
            errors.append(f"{label}: annotation CSV does not exist: {csv_path}")
            continue

        metadata, rows, csv_errors = _parse_csv(csv_path)
        errors.extend(csv_errors)
        if not rows:
            errors.append(f"{csv_path}: annotation CSV has no rows")
        expected_metadata = {
            "image-id": image_id,
            "sha256": sha256,
            "load-address": load_text,
            "identity-source": image["identity_source"],
            "source-record-id": image["source_record_id"],
        }
        for key, expected in expected_metadata.items():
            if metadata.get(key) != expected:
                errors.append(f"{csv_path}: metadata {key!r} does not match index")

        seen_addresses: set[int] = set()
        image_end = load_address + image["loaded_size"]
        previous_address = -1
        for address, _note in rows:
            if address in seen_addresses:
                errors.append(f"{csv_path}: duplicate address 0x{address:08X}")
            seen_addresses.add(address)
            if address & 3:
                errors.append(f"{csv_path}: unaligned MIPS address 0x{address:08X}")
            if address <= previous_address:
                errors.append(f"{csv_path}: rows must be strictly sorted by address")
            previous_address = address
            if not load_address <= address < image_end:
                errors.append(
                    f"{csv_path}: address 0x{address:08X} lies outside "
                    f"0x{load_address:08X}..0x{image_end - 1:08X}"
                )

        artifact = image.get("artifact")
        if artifact:
            artifact_path = _repo_path(root, artifact, f"{label}:artifact", errors)
            if artifact_path is None:
                continue
            if artifact_path.is_file():
                payload = artifact_path.read_bytes()
                if len(payload) != image["size"]:
                    errors.append(f"{label}: private artifact size does not match index")
                if hashlib.sha256(payload).hexdigest() != sha256:
                    errors.append(f"{label}: private artifact sha256 does not match index")
                if crc32 is not None and f"{binascii.crc32(payload) & 0xffffffff:08x}" != crc32:
                    errors.append(f"{label}: private artifact crc32 does not match index")
                if image["image_format"] == "ps-x-exe":
                    if payload[:8] != b"PS-X EXE":
                        errors.append(f"{label}: private artifact lacks PS-X EXE magic")
                    elif len(payload) >= 0x20:
                        header_load = int.from_bytes(payload[0x18:0x1c], "little")
                        header_size = int.from_bytes(payload[0x1c:0x20], "little")
                        if header_load != load_address:
                            errors.append(f"{label}: PS-X EXE header load address does not match index")
                        if header_size != image["loaded_size"]:
                            errors.append(f"{label}: PS-X EXE header payload size does not match index")

    indexed_csvs = {path.resolve() for path in seen_csv_paths}
    catalog_dir = index_path.parent
    orphan_csvs = sorted(
        path for path in catalog_dir.glob("*_annotations.csv") if path.resolve() not in indexed_csvs
    )
    for path in orphan_csvs:
        errors.append(f"{index_path}: unindexed annotation CSV: {path}")

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate image-qualified overlay annotation CSVs.")
    parser.add_argument("index", nargs="?", type=Path, default=DEFAULT_INDEX)
    args = parser.parse_args(argv)
    errors = validate_index(args.index.resolve(), ROOT)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    document: dict[str, Any] = tomllib.loads(args.index.read_text(encoding="utf-8"))
    print(f"Validated {len(document['images'])} overlay annotation images.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
