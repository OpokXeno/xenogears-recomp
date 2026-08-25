#!/usr/bin/env python3
"""Extract authenticated Xenogears overlays from a retail Disc 1 image."""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import json
import re
import tomllib
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CATALOG = ROOT / "annotations" / "overlays" / "index.toml"
DEFAULT_MANIFEST = ROOT / "annotations" / "overlays" / "disc1-images.toml"
DEFAULT_ROOTS = ROOT / "annotations" / "overlays" / "aot-dispatch-roots.toml"
CATALOG_SCHEMA = "xenogears-overlay-annotations/v1"
MANIFEST_SCHEMA = "xenogears-disc-overlay-images/v1"
ROOTS_SCHEMA = "xenogears-overlay-aot-roots/v1"
SHA256_RE = re.compile(r"[0-9a-f]{64}")
FUNCTION_START_RE = re.compile(r"^(0x[0-9A-Fa-f]{8}),\s*function start\s+—\s+.+$")


@dataclass(frozen=True)
class DiscLayout:
    path: Path
    sector_size: int
    user_offset: int

    def read_user_data(self, lba: int, size: int) -> bytes:
        if lba < 0 or size <= 0:
            raise ValueError("disc extent must have a non-negative LBA and positive size")
        sectors = (size + 2047) // 2048
        output = bytearray()
        with self.path.open("rb") as disc:
            for index in range(sectors):
                offset = (lba + index) * self.sector_size + self.user_offset
                disc.seek(offset)
                sector = disc.read(2048)
                if len(sector) != 2048:
                    raise ValueError(
                        f"disc ends inside extent at LBA {lba + index}"
                    )
                output.extend(sector)
        return bytes(output[:size])


def _cue_layout(path: Path) -> DiscLayout:
    text = path.read_text(encoding="utf-8-sig")
    file_match = re.search(r'^\s*FILE\s+"([^"]+)"\s+BINARY\s*$', text, re.MULTILINE | re.IGNORECASE)
    track_match = re.search(r"^\s*TRACK\s+01\s+(MODE[12]/(?:2048|2352))\s*$", text, re.MULTILINE | re.IGNORECASE)
    index_match = re.search(r"^\s*INDEX\s+01\s+00:00:00\s*$", text, re.MULTILINE | re.IGNORECASE)
    if not file_match or not track_match or not index_match:
        raise ValueError("CUE must describe track 01 at 00:00:00 in MODE1/2 2048/2352")
    binary = (path.parent / file_match.group(1)).resolve()
    mode = track_match.group(1).upper()
    if not binary.is_file():
        raise ValueError(f"CUE data file does not exist: {binary}")
    if mode.endswith("/2048"):
        return DiscLayout(binary, 2048, 0)
    return DiscLayout(binary, 2352, 24 if mode.startswith("MODE2") else 16)


def open_disc(path: Path) -> DiscLayout:
    path = path.resolve()
    if path.suffix.lower() == ".cue":
        return _cue_layout(path)
    if not path.is_file():
        raise ValueError(f"disc image does not exist: {path}")
    size = path.stat().st_size
    candidates = []
    if size % 2352 == 0:
        candidates.extend((DiscLayout(path, 2352, 24), DiscLayout(path, 2352, 16)))
    if size % 2048 == 0:
        candidates.append(DiscLayout(path, 2048, 0))
    for candidate in candidates:
        try:
            if candidate.read_user_data(16, 7)[1:6] == b"CD001":
                return candidate
        except ValueError:
            pass
    raise ValueError("cannot identify a 2048-byte ISO or MODE1/2 2352-byte BIN image")


def lzss_decompress(data: bytes) -> bytes:
    if len(data) < 4:
        raise ValueError("LZSS stream has no expanded-size header")
    target_size = int.from_bytes(data[:4], "little")
    if target_size <= 0 or target_size > 8 * 1024 * 1024:
        raise ValueError("LZSS expanded size is outside PSX RAM")
    cursor = 4
    window = bytearray(4096)
    window_at = 0xFEE
    output = bytearray()

    control = 0

    def read_or(fallback: int) -> int:
        nonlocal cursor
        if cursor >= len(data):
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
    return bytes(output)


def _load_toml(path: Path, expected_schema: str) -> dict:
    document = tomllib.loads(path.read_text(encoding="utf-8"))
    if document.get("schema") != expected_schema:
        raise ValueError(f"{path}: unsupported schema {document.get('schema')!r}")
    if not isinstance(document.get("images"), list) or not document["images"]:
        raise ValueError(f"{path}: images must be a non-empty array")
    return document


def _function_starts(path: Path) -> list[str]:
    starts = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = FUNCTION_START_RE.match(line.strip())
        if match:
            starts.append(f"0x{int(match.group(1), 16):08X}")
    if not starts:
        raise ValueError(f"{path}: no annotated function starts")
    if starts != sorted(set(starts)):
        raise ValueError(f"{path}: function starts must be unique and sorted")
    return starts


def build_captures(
    disc_path: Path,
    catalog_path: Path,
    manifest_path: Path,
    roots_path: Path = DEFAULT_ROOTS,
    image_id: str | None = None,
) -> list[dict]:
    disc = open_disc(disc_path)
    catalog = _load_toml(catalog_path, CATALOG_SCHEMA)
    manifest = _load_toml(manifest_path, MANIFEST_SCHEMA)
    roots_document = _load_toml(roots_path, ROOTS_SCHEMA)
    roots_by_image: dict[str, list[str]] = {}
    for record in roots_document["images"]:
        roots_image_id = record.get("id")
        values = record.get("dispatch_entry_pcs")
        if (
            not isinstance(roots_image_id, str)
            or roots_image_id in roots_by_image
            or not isinstance(values, list)
        ):
            raise ValueError(f"{roots_path}: malformed or duplicate image roots")
        normalized = [f"0x{int(value, 0):08X}" for value in values]
        if normalized != sorted(set(normalized)):
            raise ValueError(
                f"{roots_path}: {roots_image_id} roots must be unique and sorted"
            )
        roots_by_image[roots_image_id] = normalized
    catalog_by_source = {image["source_record_id"]: image for image in catalog["images"]}
    manifest_ids = [image.get("id") for image in manifest["images"]]
    if len(catalog_by_source) != len(catalog["images"]) or set(catalog_by_source) != set(manifest_ids):
        raise ValueError("catalog and disc manifest image sets differ")
    catalog_image_ids = {image["id"] for image in catalog["images"]}
    if not set(roots_by_image).issubset(catalog_image_ids):
        raise ValueError(f"{roots_path}: roots reference unknown catalog images")
    if image_id is not None and image_id not in catalog_image_ids:
        raise ValueError(f"unknown overlay image id: {image_id}")

    captures = []
    for source in manifest["images"]:
        source_id = source["id"]
        image = catalog_by_source[source_id]
        if image_id is not None and image["id"] != image_id:
            continue
        stored = disc.read_user_data(source["source_sector"], source["stored_size"])
        stored_sha256 = hashlib.sha256(stored).hexdigest()
        if not SHA256_RE.fullmatch(source.get("stored_sha256", "")) or stored_sha256 != source["stored_sha256"]:
            raise ValueError(f"{source_id}: stored disc extent SHA-256 mismatch")
        if source["storage"] == "lzss":
            payload = lzss_decompress(stored)
        elif source["storage"] == "raw-zero-pad":
            if len(stored) > source["size"]:
                raise ValueError(f"{source_id}: stored extent exceeds loaded size")
            payload = stored + bytes(source["size"] - len(stored))
        else:
            raise ValueError(f"{source_id}: unsupported storage {source['storage']!r}")

        expected = {
            "sha256": hashlib.sha256(payload).hexdigest(),
            "crc32": f"{binascii.crc32(payload) & 0xFFFFFFFF:08x}",
            "size": len(payload),
            "load_address": source["load_address"],
        }
        for key, actual in expected.items():
            if source.get(key) != actual or image.get(key) != actual:
                raise ValueError(f"{source_id}: extracted {key} does not match authenticated manifests")
        if image.get("header_size") != 0 or image.get("loaded_size") != len(payload):
            raise ValueError(f"{source_id}: AOT disc image must be a headerless loaded image")

        annotation_path = (catalog_path.parents[2] / image["annotations"]).resolve()
        starts = _function_starts(annotation_path)
        isolated_entries = roots_by_image.get(image["id"], [])
        dispatch_entries = sorted(set(starts) | set(isolated_entries))
        load_address = int(source["load_address"], 0)
        if any(
            not load_address <= int(address, 0) < load_address + len(payload)
            for address in dispatch_entries
        ):
            raise ValueError(f"{source_id}: AOT entry lies outside extracted image")
        captures.append({
            "schema": "xenogears-disc-overlay-aot/v1",
            "image_id": image["id"],
            "load_addr": f"0x{load_address:08X}",
            "size": len(payload),
            "bytes_b64": base64.b64encode(payload).decode("ascii"),
            "executed_pcs": [],
            "dispatch_entry_pcs": dispatch_entries,
            "static_dispatch_entry_pcs": dispatch_entries,
            "static_isolated_entry_pcs": isolated_entries,
            "static_discovery_entry_pcs": starts,
            "function_entry_pcs": starts,
        })
    return captures


def image_plan(catalog_path: Path, manifest_path: Path, roots_path: Path) -> list[dict]:
    catalog = _load_toml(catalog_path, CATALOG_SCHEMA)
    manifest = _load_toml(manifest_path, MANIFEST_SCHEMA)
    roots = _load_toml(roots_path, ROOTS_SCHEMA)
    catalog_by_source = {image["source_record_id"]: image for image in catalog["images"]}
    if len(catalog_by_source) != len(catalog["images"]):
        raise ValueError(f"{catalog_path}: duplicate source_record_id")
    if set(catalog_by_source) != {image.get("id") for image in manifest["images"]}:
        raise ValueError("catalog and disc manifest image sets differ")
    root_ids = [image.get("id") for image in roots["images"]]
    catalog_ids = {image.get("id") for image in catalog["images"]}
    if len(root_ids) != len(set(root_ids)) or not set(root_ids).issubset(catalog_ids):
        raise ValueError(f"{roots_path}: roots reference duplicate or unknown images")

    result = []
    for source in manifest["images"]:
        image = catalog_by_source[source["id"]]
        annotation_path = (catalog_path.parents[2] / image["annotations"]).resolve()
        starts = _function_starts(annotation_path)
        result.append({
            "image_id": image["id"],
            "source_id": source["id"],
            "size": source["size"],
            "load_address": source["load_address"],
            "entry_pc": starts[0],
            "sha256": source["sha256"],
            "annotation": str(annotation_path),
        })
    return result


def _write_bytes_if_changed(path: Path, content: bytes) -> None:
    if path.is_file() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_bytes(content)
    temporary.replace(path)


def _write_text_if_changed(path: Path, content: str) -> None:
    _write_bytes_if_changed(path, content.encode("ascii"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disc", type=Path, help="retail Disc 1 CUE/BIN/ISO")
    parser.add_argument("--catalog", type=Path, default=DEFAULT_CATALOG)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--roots", type=Path, default=DEFAULT_ROOTS)
    parser.add_argument("--image-id", help="extract exactly one catalog image")
    parser.add_argument("--out", type=Path, help="legacy aggregate capture JSON")
    parser.add_argument("--raw-out", type=Path, help="authenticated single-image raw bytes")
    parser.add_argument("--capture-out", type=Path, help="single-image capture recipe")
    parser.add_argument("--seeds-out", type=Path, help="single-image native recompiler seeds")
    parser.add_argument("--list-json", action="store_true", help="print the byte-free image build plan")
    args = parser.parse_args()
    catalog = args.catalog.resolve()
    manifest = args.manifest.resolve()
    roots = args.roots.resolve()
    if args.list_json:
        if args.disc or args.out or args.raw_out or args.capture_out or args.seeds_out or args.image_id:
            parser.error("--list-json cannot be combined with extraction options")
        print(json.dumps(image_plan(catalog, manifest, roots), separators=(",", ":")))
        return 0
    if args.disc is None:
        parser.error("--disc is required for extraction")
    single_outputs = (args.raw_out, args.capture_out, args.seeds_out)
    if any(single_outputs) and (not args.image_id or args.out):
        parser.error("single-image outputs require --image-id and cannot be combined with --out")
    if not args.out and not any(single_outputs):
        parser.error("one of --out, --raw-out, --capture-out, or --seeds-out is required")
    captures = build_captures(
        args.disc, catalog, manifest, roots, args.image_id
    )
    if args.image_id and len(captures) != 1:
        raise ValueError(f"{args.image_id}: expected exactly one authenticated image")
    if args.out:
        _write_text_if_changed(args.out, json.dumps(captures, separators=(",", ":")))
    if args.raw_out:
        _write_bytes_if_changed(args.raw_out, base64.b64decode(captures[0]["bytes_b64"]))
    if args.capture_out:
        _write_text_if_changed(
            args.capture_out, json.dumps(captures, separators=(",", ":")))
    if args.seeds_out:
        capture = captures[0]
        isolated = set(capture["static_isolated_entry_pcs"])
        lines = [
            *(f"call_root {entry}" for entry in capture["static_discovery_entry_pcs"]),
            *(f"isolated_root {entry}" for entry in sorted(
                isolated - set(capture["static_discovery_entry_pcs"]))),
        ]
        _write_text_if_changed(args.seeds_out, "\n".join(lines) + "\n")
    for capture in captures:
        print(f"{capture['image_id']}: {capture['load_addr']} {capture['size']} bytes authenticated")
    destinations = [str(path) for path in (args.out, *single_outputs) if path]
    print(f"Wrote {len(captures)} AOT overlay image(s) to {', '.join(destinations)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
