#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import tempfile
from typing import Any, Final


REVIEW_SCHEMA: Final = "xg-native-3d-census-review/v3"
INVENTORY_SCHEMA: Final = "xg-native-3d-static-inventory/v3"
LEDGER_SCHEMA: Final = "xg-native-3d-static-ledger/v3"
PSX_EXE_HEADER_SIZE: Final = 0x800
MAX_REVIEW_BYTES: Final = 16 * 1024 * 1024
TOKEN: Final = re.compile(r"^[a-z0-9][a-z0-9-]{0,199}$")
SERIAL: Final = re.compile(r"^[A-Z0-9][A-Z0-9-]{0,31}$")
SHA256: Final = re.compile(r"^[0-9a-f]{64}$")
ADDRESS: Final = re.compile(r"^0x[0-9a-f]{8}$")

REVIEW_STATUSES: Final = frozenset(
    {
        "migrated-render",
        "excluded-pure-2d-proven",
        "non-render-proven",
        "error",
    }
)
NON_RENDER_STATUSES: Final = frozenset(
    {
        "excluded-pure-2d-proven",
        "non-render-proven",
        "error",
    }
)
KNOWN_LIMITATIONS: Final = (
    "dynamic-dispatch-targets-require-runtime-provenance",
    "inline-ot-idiom-detection-is-conservative",
    "packet-store-candidates-require-terminal-manual-classification",
    "static-closure-is-limited-to-authenticated-disc-1-executable-ranges",
)
RENDER_TARGET_ROLES: Final = frozenset(
    {
        "gte-wrapper",
        "matrix-function",
        "light-function",
        "addprim-function",
        "ot-function",
    }
)
INTRINSIC_RENDER_SITE_KINDS: Final = frozenset(
    {
        "cop2-command",
        "mfc2",
        "mtc2",
        "cfc2",
        "ctc2",
        "lwc2",
        "swc2",
        "inline-ot-insert",
    }
)

GTE_COMMANDS: Final = {
    0x01: "rtps",
    0x06: "nclip",
    0x0C: "op",
    0x10: "dpcs",
    0x11: "intpl",
    0x12: "mvmva",
    0x13: "ncds",
    0x14: "cdp",
    0x16: "ncdt",
    0x1B: "nccs",
    0x1C: "cc",
    0x1E: "ncs",
    0x20: "nct",
    0x28: "sqr",
    0x29: "dcpl",
    0x2A: "dpct",
    0x2D: "avsz3",
    0x2E: "avsz4",
    0x30: "rtpt",
    0x3D: "gpf",
    0x3E: "gpl",
    0x3F: "ncct",
}

JsonObject = dict[str, Any]


class CensusError(Exception):
    pass


def _object(value: Any, keys: set[str], label: str) -> JsonObject:
    if not isinstance(value, dict) or set(value) != keys:
        raise CensusError(f"{label} fields are not closed")
    if not all(isinstance(key, str) for key in value):
        raise CensusError(f"{label} contains a non-string field")
    return value


def _array(value: Any, label: str) -> list[Any]:
    if not isinstance(value, list):
        raise CensusError(f"{label} must be an array")
    return value


def _integer(value: Any, label: str, *, minimum: int = 0) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum:
        raise CensusError(f"{label} must be an integer >= {minimum}")
    return value


def _token(value: Any, label: str) -> str:
    if not isinstance(value, str) or TOKEN.fullmatch(value) is None:
        raise CensusError(f"{label} must be a safe metadata token")
    return value


def _serial(value: Any, label: str) -> str:
    if not isinstance(value, str) or SERIAL.fullmatch(value) is None:
        raise CensusError(f"{label} must be a serial token")
    return value


def _sha256(value: Any, label: str) -> str:
    if not isinstance(value, str) or SHA256.fullmatch(value) is None:
        raise CensusError(f"{label} must be a full lowercase SHA-256")
    return value


def _address(value: Any, label: str) -> int:
    if not isinstance(value, str) or ADDRESS.fullmatch(value) is None:
        raise CensusError(f"{label} must be a lowercase 32-bit address")
    return int(value, 16)


def _optional_address(value: Any, label: str) -> int | None:
    if value is None:
        return None
    return _address(value, label)


def _format_address(value: int) -> str:
    return f"0x{value:08x}"


def _unique_sorted(values: list[Any], parser: Any, label: str) -> list[Any]:
    parsed = [parser(value, f"{label}[{index}]") for index, value in enumerate(values)]
    if len(set(parsed)) != len(parsed):
        raise CensusError(f"{label} contains duplicates")
    return sorted(parsed)


def _references(value: Any, label: str) -> list[str]:
    references = _unique_sorted(_array(value, label), _token, label)
    if not references:
        raise CensusError(f"{label} must not be empty")
    return references


def _pairs(items: list[tuple[str, Any]]) -> JsonObject:
    result: JsonObject = {}
    for key, value in items:
        if key in result:
            raise CensusError("review JSON contains a duplicate field")
        result[key] = value
    return result


def _load_json(path: Path) -> Any:
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise CensusError("review document is unreadable") from error
    if len(raw) > MAX_REVIEW_BYTES:
        raise CensusError("review document is oversized")
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=_pairs)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CensusError("review document is invalid JSON") from error


def _canonical_json(value: Any) -> bytes:
    try:
        return (
            json.dumps(
                value,
                allow_nan=False,
                ensure_ascii=True,
                separators=(",", ":"),
                sort_keys=True,
            ).encode("ascii")
            + b"\n"
        )
    except (TypeError, ValueError) as error:
        raise CensusError("public document is not canonical JSON") from error


def _artifact_id(digest: str) -> str:
    return f"artifact-{digest}"


def _site_id(digest: str, payload_offset: int, kind: str) -> str:
    return f"site-{digest}-{payload_offset:08x}-{kind}"


def _branch_id(digest: str, payload_offset: int, kind: str) -> str:
    return f"branch-{digest}-{payload_offset:08x}-{kind}"


def _parse_mapping(raw: Any, label: str, full_size: int) -> JsonObject:
    value = _object(
        raw,
        {
            "format",
            "endian",
            "header_size",
            "payload_file_offset",
            "payload_size",
            "runtime_address",
            "entry_address",
        },
        label,
    )
    image_format = value["format"]
    if image_format not in {"ps-x-exe", "flat-binary"}:
        raise CensusError(f"{label}.format is unsupported")
    if value["endian"] != "little":
        raise CensusError(f"{label}.endian must be little")
    header_size = _integer(value["header_size"], f"{label}.header_size")
    payload_offset = _integer(
        value["payload_file_offset"], f"{label}.payload_file_offset"
    )
    payload_size = _integer(
        value["payload_size"], f"{label}.payload_size", minimum=4
    )
    runtime_address = _address(value["runtime_address"], f"{label}.runtime_address")
    entry_address = _optional_address(value["entry_address"], f"{label}.entry_address")
    if image_format == "ps-x-exe":
        if header_size != PSX_EXE_HEADER_SIZE or payload_offset != PSX_EXE_HEADER_SIZE:
            raise CensusError(f"{label} must map the explicit 0x800-byte PS-X EXE header")
        if entry_address is None:
            raise CensusError(f"{label}.entry_address is required for a PS-X EXE")
    elif header_size != 0:
        raise CensusError(f"{label}.header_size must be zero for a flat binary")
    if payload_offset + payload_size > full_size:
        raise CensusError(f"{label} payload exceeds the full artifact")
    if payload_offset % 4 or payload_size % 4 or runtime_address % 4:
        raise CensusError(f"{label} does not provide aligned MIPS payload mapping")
    runtime_end = runtime_address + payload_size
    if runtime_end > 0xFFFFFFFF:
        raise CensusError(f"{label} runtime mapping exceeds 32-bit address space")
    if entry_address is not None and not (runtime_address <= entry_address < runtime_end):
        raise CensusError(f"{label}.entry_address is outside the runtime payload")
    return {
        "endian": "little",
        "entry_address": None if entry_address is None else _format_address(entry_address),
        "format": image_format,
        "header_size": header_size,
        "payload_file_offset": payload_offset,
        "payload_size": payload_size,
        "runtime_address": _format_address(runtime_address),
    }


def _parse_executable_range(
    raw: Any, index: int, artifact_label: str, payload_size: int
) -> JsonObject:
    label = f"{artifact_label}.executable_ranges[{index}]"
    value = _object(
        raw,
        {"range_id", "payload_offset", "size", "provenance_refs", "evidence_refs"},
        label,
    )
    payload_offset = _integer(value["payload_offset"], f"{label}.payload_offset")
    size = _integer(value["size"], f"{label}.size", minimum=4)
    if payload_offset % 4 or size % 4:
        raise CensusError(f"{label} must be 4-byte aligned")
    if payload_offset + size > payload_size:
        raise CensusError(f"{label} exceeds the authenticated payload")
    return {
        "evidence_refs": _references(value["evidence_refs"], f"{label}.evidence_refs"),
        "payload_offset": payload_offset,
        "provenance_refs": _references(
            value["provenance_refs"], f"{label}.provenance_refs"
        ),
        "range_id": _token(value["range_id"], f"{label}.range_id"),
        "size": size,
    }


def _parse_artifact(raw: Any, index: int) -> JsonObject:
    label = f"artifacts[{index}]"
    value = _object(
        raw,
        {
            "sha256",
            "size",
            "discs",
            "serials",
            "role",
            "mapping",
            "executable_ranges",
        },
        label,
    )
    digest = _sha256(value["sha256"], f"{label}.sha256")
    size = _integer(value["size"], f"{label}.size", minimum=4)
    discs = _unique_sorted(
        _array(value["discs"], f"{label}.discs"),
        lambda item, item_label: _integer(item, item_label, minimum=1),
        f"{label}.discs",
    )
    if discs != [1]:
        raise CensusError(f"{label}.discs must be Disc 1 only; Disc 2 artifacts are rejected")
    serials = _unique_sorted(
        _array(value["serials"], f"{label}.serials"),
        _serial,
        f"{label}.serials",
    )
    if any(serial != "SLUS-00664" for serial in serials):
        raise CensusError(
            f"{label}.serials must contain only Disc 1 serial SLUS-00664; "
            "Disc 2 serials are rejected"
        )
    role = _token(value["role"], f"{label}.role")
    if role not in {"main-exe", "overlay"}:
        raise CensusError(f"{label}.role must be main-exe or overlay")
    mapping = _parse_mapping(value["mapping"], f"{label}.mapping", size)
    if role == "main-exe":
        if serials != ["SLUS-00664"]:
            raise CensusError(f"{label} main executable must identify SLUS-00664")
        if mapping["format"] != "ps-x-exe":
            raise CensusError(f"{label} main executable must be a PS-X EXE")
    elif mapping["format"] != "flat-binary":
        raise CensusError(f"{label} overlay must be a flat binary")
    executable_ranges = [
        _parse_executable_range(item, range_index, label, mapping["payload_size"])
        for range_index, item in enumerate(
            _array(value["executable_ranges"], f"{label}.executable_ranges")
        )
    ]
    if not executable_ranges:
        raise CensusError(f"{label}.executable_ranges must not be empty")
    executable_ranges.sort(key=lambda item: (item["payload_offset"], item["range_id"]))
    for previous, current in zip(executable_ranges, executable_ranges[1:]):
        if current["payload_offset"] < previous["payload_offset"] + previous["size"]:
            raise CensusError(f"{label}.executable_ranges overlap")
    entry_address = mapping["entry_address"]
    if entry_address is not None:
        entry_offset = int(entry_address, 16) - int(mapping["runtime_address"], 16)
        if not any(
            item["payload_offset"] <= entry_offset < item["payload_offset"] + item["size"]
            for item in executable_ranges
        ):
            raise CensusError(f"{label}.executable_ranges do not cover the image entry")
    return {
        "artifact_id": _artifact_id(digest),
        "discs": discs,
        "executable_ranges": executable_ranges,
        "mapping": mapping,
        "role": role,
        "serials": serials,
        "sha256": digest,
        "size": size,
    }


def _review_status(value: Any, label: str) -> str:
    if not isinstance(value, str) or value not in REVIEW_STATUSES:
        raise CensusError(f"{label} is not a terminal P10 review status")
    return value


def _classification_common(value: JsonObject, label: str) -> JsonObject:
    evidence_refs = _unique_sorted(
        _array(value["evidence_refs"], f"{label}.evidence_refs"),
        _token,
        f"{label}.evidence_refs",
    )
    if not evidence_refs:
        raise CensusError(f"{label}.evidence_refs must not be empty")
    return {
        "evidence_refs": evidence_refs,
        "status": _review_status(value["status"], f"{label}.status"),
        "subject": _token(value["subject"], f"{label}.subject"),
    }


def _parse_artifact_classification(raw: Any, index: int) -> JsonObject:
    label = f"classifications.artifacts[{index}]"
    value = _object(
        raw,
        {"artifact_id", "status", "subject", "evidence_refs"},
        label,
    )
    result = _classification_common(value, label)
    result["artifact_id"] = _token(value["artifact_id"], f"{label}.artifact_id")
    return result


def _parse_site_classification(raw: Any, index: int) -> JsonObject:
    label = f"classifications.sites[{index}]"
    value = _object(
        raw,
        {"site_id", "status", "subject", "family_ids", "evidence_refs"},
        label,
    )
    result = _classification_common(value, label)
    result["site_id"] = _token(value["site_id"], f"{label}.site_id")
    families = _unique_sorted(
        _array(value["family_ids"], f"{label}.family_ids"),
        _token,
        f"{label}.family_ids",
    )
    if result["status"] in NON_RENDER_STATUSES:
        if families:
            raise CensusError(f"{label} non-render or error site cannot claim a family")
    elif not families:
        raise CensusError(f"{label} migrated render site must claim a concrete family")
    result["family_ids"] = families
    return result


def _parse_branch_classification(raw: Any, index: int) -> JsonObject:
    label = f"classifications.branches[{index}]"
    value = _object(
        raw,
        {
            "branch_id",
            "status",
            "subject",
            "reachability",
            "family_ids",
            "evidence_refs",
        },
        label,
    )
    result = _classification_common(value, label)
    result["branch_id"] = _token(value["branch_id"], f"{label}.branch_id")
    reachability = value["reachability"]
    if reachability not in {"reachable", "unreachable"}:
        raise CensusError(f"{label}.reachability must be reachable or unreachable")
    families = _unique_sorted(
        _array(value["family_ids"], f"{label}.family_ids"),
        _token,
        f"{label}.family_ids",
    )
    if result["status"] in NON_RENDER_STATUSES:
        if families:
            raise CensusError(f"{label} non-render or error branch cannot claim a family")
    elif not families:
        raise CensusError(f"{label} migrated render branch must claim a concrete family")
    result["family_ids"] = families
    result["reachability"] = reachability
    return result


def _deduplicate_classifications(records: list[JsonObject], key: str, label: str) -> None:
    identifiers = [record[key] for record in records]
    if len(set(identifiers)) != len(identifiers):
        raise CensusError(f"{label} contains duplicate reviewed IDs")


def _parse_function(
    raw: Any,
    index: int,
    artifacts: dict[str, JsonObject],
    ranges: dict[str, tuple[JsonObject, JsonObject]],
) -> JsonObject:
    label = f"functions[{index}]"
    value = _object(
        raw,
        {
            "function_id",
            "artifact_id",
            "executable_range_id",
            "payload_offset",
            "size",
            "role",
            "provenance_refs",
            "evidence_refs",
        },
        label,
    )
    function_id = _token(value["function_id"], f"{label}.function_id")
    artifact_id = _token(value["artifact_id"], f"{label}.artifact_id")
    range_id = _token(value["executable_range_id"], f"{label}.executable_range_id")
    if artifact_id not in artifacts:
        raise CensusError(f"{label}.artifact_id is unknown")
    if range_id not in ranges or ranges[range_id][0]["artifact_id"] != artifact_id:
        raise CensusError(f"{label}.executable_range_id is unknown for its artifact")
    payload_offset = _integer(value["payload_offset"], f"{label}.payload_offset")
    size = _integer(value["size"], f"{label}.size", minimum=4)
    if payload_offset % 4 or size % 4:
        raise CensusError(f"{label} must be 4-byte aligned")
    executable_range = ranges[range_id][1]
    if not (
        executable_range["payload_offset"] <= payload_offset
        and payload_offset + size
        <= executable_range["payload_offset"] + executable_range["size"]
    ):
        raise CensusError(f"{label} exceeds its authenticated executable range")
    role = value["role"]
    if role not in {"producer", "render-support"}:
        raise CensusError(f"{label}.role must be producer or render-support")
    return {
        "artifact_id": artifact_id,
        "evidence_refs": _references(value["evidence_refs"], f"{label}.evidence_refs"),
        "executable_range_id": range_id,
        "function_id": function_id,
        "payload_offset": payload_offset,
        "provenance_refs": _references(
            value["provenance_refs"], f"{label}.provenance_refs"
        ),
        "role": role,
        "size": size,
    }


def _parse_render_target(
    raw: Any, index: int, artifacts: dict[str, JsonObject]
) -> JsonObject:
    label = f"render_targets[{index}]"
    value = _object(
        raw,
        {
            "target_id",
            "artifact_id",
            "address",
            "role",
            "caller_artifact_ids",
            "indirect_call_site_ids",
            "provenance_refs",
            "evidence_refs",
        },
        label,
    )
    artifact_id = _token(value["artifact_id"], f"{label}.artifact_id")
    if artifact_id not in artifacts:
        raise CensusError(f"{label}.artifact_id is unknown")
    artifact = artifacts[artifact_id]
    address = _address(value["address"], f"{label}.address")
    if address % 4:
        raise CensusError(f"{label}.address must be 4-byte aligned")
    runtime_address = int(artifact["mapping"]["runtime_address"], 16)
    payload_offset = address - runtime_address
    if not any(
        executable_range["payload_offset"]
        <= payload_offset
        < executable_range["payload_offset"] + executable_range["size"]
        for executable_range in artifact["executable_ranges"]
    ):
        raise CensusError(f"{label}.address is outside authenticated executable ranges")
    role = value["role"]
    if role not in RENDER_TARGET_ROLES:
        raise CensusError(f"{label}.role is not a render-relevant target role")
    caller_ids = _unique_sorted(
        _array(value["caller_artifact_ids"], f"{label}.caller_artifact_ids"),
        _token,
        f"{label}.caller_artifact_ids",
    )
    if not caller_ids:
        raise CensusError(f"{label}.caller_artifact_ids must not be empty")
    for caller_id in caller_ids:
        if caller_id not in artifacts:
            raise CensusError(f"{label}.caller_artifact_ids contains an unknown artifact")
        if not set(artifacts[caller_id]["discs"]) & set(artifact["discs"]):
            raise CensusError(f"{label} caller and target have no shared disc context")
    return {
        "address": _format_address(address),
        "artifact_id": artifact_id,
        "caller_artifact_ids": caller_ids,
        "evidence_refs": _references(value["evidence_refs"], f"{label}.evidence_refs"),
        "indirect_call_site_ids": _unique_sorted(
            _array(value["indirect_call_site_ids"], f"{label}.indirect_call_site_ids"),
            _token,
            f"{label}.indirect_call_site_ids",
        ),
        "provenance_refs": _references(
            value["provenance_refs"], f"{label}.provenance_refs"
        ),
        "role": role,
        "target_id": _token(value["target_id"], f"{label}.target_id"),
    }


def _parse_review(path: Path) -> JsonObject:
    root = _object(
        _load_json(path),
        {"schema", "artifacts", "functions", "render_targets", "classifications"},
        "review",
    )
    if root["schema"] != REVIEW_SCHEMA:
        raise CensusError("review schema version is unsupported")
    artifacts = [
        _parse_artifact(item, index)
        for index, item in enumerate(_array(root["artifacts"], "artifacts"))
    ]
    artifacts.sort(key=lambda item: item["artifact_id"])
    hashes = [item["sha256"] for item in artifacts]
    if len(set(hashes)) != len(hashes):
        raise CensusError("artifacts contains duplicate full SHA-256 identities")
    artifacts_by_id = {item["artifact_id"]: item for item in artifacts}
    ranges: dict[str, tuple[JsonObject, JsonObject]] = {}
    for artifact in artifacts:
        for executable_range in artifact["executable_ranges"]:
            range_id = executable_range["range_id"]
            if range_id in ranges:
                raise CensusError("executable range IDs must be globally unique")
            ranges[range_id] = (artifact, executable_range)
    main_executables = [item for item in artifacts if item["role"] == "main-exe"]
    if len(main_executables) != 1:
        raise CensusError(
            "exactly one Disc 1 main PS-X EXE SLUS-00664 is required"
        )

    functions = [
        _parse_function(item, index, artifacts_by_id, ranges)
        for index, item in enumerate(_array(root["functions"], "functions"))
    ]
    functions.sort(key=lambda item: item["function_id"])
    function_ids = [item["function_id"] for item in functions]
    if len(set(function_ids)) != len(function_ids):
        raise CensusError("functions contains duplicate function IDs")
    by_artifact: dict[str, list[JsonObject]] = {}
    for function in functions:
        by_artifact.setdefault(function["artifact_id"], []).append(function)
    for artifact_functions in by_artifact.values():
        artifact_functions.sort(key=lambda item: item["payload_offset"])
        for previous, current in zip(artifact_functions, artifact_functions[1:]):
            if current["payload_offset"] < previous["payload_offset"] + previous["size"]:
                raise CensusError("reviewed producer/function ranges overlap")

    render_targets = [
        _parse_render_target(item, index, artifacts_by_id)
        for index, item in enumerate(_array(root["render_targets"], "render_targets"))
    ]
    render_targets.sort(key=lambda item: item["target_id"])
    target_ids = [item["target_id"] for item in render_targets]
    if len(set(target_ids)) != len(target_ids):
        raise CensusError("render_targets contains duplicate target IDs")
    direct_bindings: set[tuple[str, str]] = set()
    indirect_bindings: set[str] = set()
    for target in render_targets:
        for caller_id in target["caller_artifact_ids"]:
            binding = (caller_id, target["address"])
            if binding in direct_bindings:
                raise CensusError("render_targets contains an ambiguous direct-call binding")
            direct_bindings.add(binding)
        for site_id in target["indirect_call_site_ids"]:
            if site_id in indirect_bindings:
                raise CensusError("render_targets contains an ambiguous indirect-call binding")
            indirect_bindings.add(site_id)

    classifications = _object(
        root["classifications"], {"artifacts", "sites", "branches"}, "classifications"
    )
    artifact_classes = [
        _parse_artifact_classification(item, index)
        for index, item in enumerate(
            _array(classifications["artifacts"], "classifications.artifacts")
        )
    ]
    site_classes = [
        _parse_site_classification(item, index)
        for index, item in enumerate(
            _array(classifications["sites"], "classifications.sites")
        )
    ]
    branch_classes = [
        _parse_branch_classification(item, index)
        for index, item in enumerate(
            _array(classifications["branches"], "classifications.branches")
        )
    ]
    _deduplicate_classifications(artifact_classes, "artifact_id", "classifications.artifacts")
    _deduplicate_classifications(site_classes, "site_id", "classifications.sites")
    _deduplicate_classifications(branch_classes, "branch_id", "classifications.branches")
    artifact_classes.sort(key=lambda item: item["artifact_id"])
    site_classes.sort(key=lambda item: item["site_id"])
    branch_classes.sort(key=lambda item: item["branch_id"])
    return {
        "artifacts": artifacts,
        "classifications": {
            "artifacts": artifact_classes,
            "branches": branch_classes,
            "sites": site_classes,
        },
        "functions": functions,
        "render_targets": render_targets,
        "schema": REVIEW_SCHEMA,
    }


def _read_artifacts(paths: list[Path], specs: list[JsonObject]) -> dict[str, bytes]:
    expected = {spec["sha256"]: spec for spec in specs}
    loaded: dict[str, bytes] = {}
    for index, path in enumerate(paths, start=1):
        try:
            data = path.read_bytes()
        except OSError as error:
            raise CensusError(f"artifact input {index} is unreadable") from error
        digest = hashlib.sha256(data).hexdigest()
        if digest not in expected:
            raise CensusError(f"unknown executable artifact SHA-256 {digest}")
        if digest in loaded:
            raise CensusError(f"duplicate executable artifact input {_artifact_id(digest)}")
        if len(data) != expected[digest]["size"]:
            raise CensusError(f"full artifact size mismatch for {_artifact_id(digest)}")
        loaded[digest] = data
    missing = sorted(set(expected) - set(loaded))
    if missing:
        raise CensusError(
            "known executable artifacts are missing: "
            + ",".join(_artifact_id(digest) for digest in missing)
        )
    return loaded


def _validate_mapping(spec: JsonObject, data: bytes) -> tuple[JsonObject, bytes]:
    mapping = spec["mapping"]
    payload_offset = mapping["payload_file_offset"]
    payload_size = mapping["payload_size"]
    runtime_address = int(mapping["runtime_address"], 16)
    entry_address = (
        None if mapping["entry_address"] is None else int(mapping["entry_address"], 16)
    )
    if mapping["format"] == "ps-x-exe":
        if len(data) < PSX_EXE_HEADER_SIZE or data[:8] != b"PS-X EXE":
            raise CensusError(f"PS-X EXE header mismatch for {spec['artifact_id']}")
        header_entry, header_load, header_size = struct.unpack_from("<I4xII", data, 0x10)
        if (
            header_entry != entry_address
            or header_load != runtime_address
            or header_size != payload_size
        ):
            raise CensusError(f"PS-X EXE header/payload/runtime mapping mismatch for {spec['artifact_id']}")
    payload = data[payload_offset : payload_offset + payload_size]
    public_mapping = {
        "endian": "little",
        "entry_address": mapping["entry_address"],
        "format": mapping["format"],
        "header_file_offset": 0,
        "header_size": mapping["header_size"],
        "payload_file_offset": payload_offset,
        "payload_size": payload_size,
        "runtime_address": mapping["runtime_address"],
        "runtime_end_exclusive": _format_address(runtime_address + payload_size),
    }
    return public_mapping, payload


def _signed_immediate(word: int) -> int:
    immediate = word & 0xFFFF
    return immediate - 0x10000 if immediate & 0x8000 else immediate


def _jump_target(pc: int, word: int) -> int:
    return ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)


def _branch_target(pc: int, word: int) -> int:
    return (pc + 4 + (_signed_immediate(word) << 2)) & 0xFFFFFFFF


def _site(
    spec: JsonObject,
    payload_offset: int,
    runtime_address: int,
    kind: str,
    details: JsonObject,
) -> JsonObject:
    return {
        "address": _format_address(runtime_address + payload_offset),
        "artifact_id": spec["artifact_id"],
        "details": details,
        "kind": kind,
        "payload_offset": payload_offset,
        "site_id": _site_id(spec["sha256"], payload_offset, kind),
    }


def _branch(
    spec: JsonObject,
    function_id: str,
    payload_offset: int,
    runtime_address: int,
    kind: str,
    details: JsonObject,
) -> JsonObject:
    return {
        "address": _format_address(runtime_address + payload_offset),
        "artifact_id": spec["artifact_id"],
        "branch_id": _branch_id(spec["sha256"], payload_offset, kind),
        "details": details,
        "function_id": function_id,
        "kind": kind,
        "payload_offset": payload_offset,
    }


def _scan_instruction_sites(
    spec: JsonObject, payload: bytes, start: int, size: int
) -> list[JsonObject]:
    runtime_address = int(spec["mapping"]["runtime_address"], 16)
    sites: list[JsonObject] = []
    for offset in range(start, start + size, 4):
        word = struct.unpack_from("<I", payload, offset)[0]
        opcode = word >> 26
        pc = runtime_address + offset
        if opcode == 0x12 and word & (1 << 25):
            command = word & 0x3F
            sites.append(
                _site(
                    spec,
                    offset,
                    runtime_address,
                    "cop2-command",
                    {
                        "command_code": command,
                        "command_name": GTE_COMMANDS.get(command, "unknown-gte-command"),
                        "cv": (word >> 13) & 3,
                        "lm": (word >> 10) & 1,
                        "mx": (word >> 17) & 3,
                        "sf": (word >> 19) & 1,
                        "v": (word >> 15) & 3,
                    },
                )
            )
        elif opcode == 0x12 and ((word >> 21) & 0x1F) in {0x00, 0x02, 0x04, 0x06}:
            transfer_kind = {
                0x00: "mfc2",
                0x02: "cfc2",
                0x04: "mtc2",
                0x06: "ctc2",
            }[(word >> 21) & 0x1F]
            sites.append(
                _site(
                    spec,
                    offset,
                    runtime_address,
                    transfer_kind,
                    {
                        "cop2_register": (word >> 11) & 0x1F,
                        "general_register": (word >> 16) & 0x1F,
                    },
                )
            )
        elif opcode in {0x32, 0x3A}:
            kind = "lwc2" if opcode == 0x32 else "swc2"
            sites.append(
                _site(
                    spec,
                    offset,
                    runtime_address,
                    kind,
                    {
                        "base_register": (word >> 21) & 0x1F,
                        "cop2_register": (word >> 16) & 0x1F,
                        "offset": _signed_immediate(word),
                    },
                )
            )
        elif opcode == 0x2B:
            sites.append(
                _site(
                    spec,
                    offset,
                    runtime_address,
                    "packet-store-candidate",
                    {
                        "base_register": (word >> 21) & 0x1F,
                        "offset": _signed_immediate(word),
                        "source_register": (word >> 16) & 0x1F,
                    },
                )
            )
        elif opcode == 0x03:
            sites.append(
                _site(
                    spec,
                    offset,
                    runtime_address,
                    "direct-jal",
                    {
                        "delay_slot_address": _format_address(pc + 4),
                        "return_address": _format_address(pc + 8),
                        "target_address": _format_address(_jump_target(pc, word)),
                    },
                )
            )
        elif opcode == 0 and (word & 0x3F) == 0x09:
            sites.append(
                _site(
                    spec,
                    offset,
                    runtime_address,
                    "jalr",
                    {
                        "delay_slot_address": _format_address(pc + 4),
                        "link_register": (word >> 11) & 0x1F,
                        "source_register": (word >> 21) & 0x1F,
                    },
                )
            )
    return sites


def _scan_inline_ot_sites(
    spec: JsonObject, payload: bytes, start: int, size: int
) -> list[JsonObject]:
    runtime_address = int(spec["mapping"]["runtime_address"], 16)
    words = [
        struct.unpack_from("<I", payload, offset)[0]
        for offset in range(start, start + size, 4)
    ]
    sites: list[JsonObject] = []
    for load_index, load in enumerate(words):
        if load >> 26 != 0x23 or _signed_immediate(load) != 0:
            continue
        ot_register = (load >> 21) & 0x1F
        old_head_register = (load >> 16) & 0x1F
        if ot_register == 0 or old_head_register == 0:
            continue
        for first_store_index in range(load_index + 1, min(load_index + 7, len(words))):
            first_store = words[first_store_index]
            if first_store >> 26 != 0x2B or _signed_immediate(first_store) != 0:
                continue
            packet_register = (first_store >> 21) & 0x1F
            if (
                packet_register in {0, ot_register}
                or (first_store >> 16) & 0x1F != old_head_register
            ):
                continue
            for second_store_index in range(
                first_store_index + 1, min(first_store_index + 5, len(words))
            ):
                second_store = words[second_store_index]
                if (
                    second_store >> 26 == 0x2B
                    and _signed_immediate(second_store) == 0
                    and (second_store >> 21) & 0x1F == ot_register
                    and (second_store >> 16) & 0x1F == packet_register
                ):
                    offset = start + load_index * 4
                    sites.append(
                        _site(
                            spec,
                            offset,
                            runtime_address,
                            "inline-ot-insert",
                            {
                                "old_head_register": old_head_register,
                                "ot_register": ot_register,
                                "packet_register": packet_register,
                                "store_addresses": [
                                    _format_address(
                                        runtime_address + start + first_store_index * 4
                                    ),
                                    _format_address(
                                        runtime_address + start + second_store_index * 4
                                    ),
                                ],
                            },
                        )
                    )
                    break
            else:
                continue
            break
    return sites


def _scan_branches(
    spec: JsonObject, payload: bytes, function: JsonObject
) -> list[JsonObject]:
    runtime_address = int(spec["mapping"]["runtime_address"], 16)
    branches: list[JsonObject] = []
    simple_conditions = {
        0x04: "beq",
        0x05: "bne",
        0x06: "blez",
        0x07: "bgtz",
        0x14: "beql",
        0x15: "bnel",
        0x16: "blezl",
        0x17: "bgtzl",
    }
    regimm_conditions = {
        0x00: "bltz",
        0x01: "bgez",
        0x02: "bltzl",
        0x03: "bgezl",
        0x10: "bltzal",
        0x11: "bgezal",
        0x12: "bltzall",
        0x13: "bgezall",
    }
    start = function["payload_offset"]
    for offset in range(start, start + function["size"], 4):
        word = struct.unpack_from("<I", payload, offset)[0]
        opcode = word >> 26
        pc = runtime_address + offset
        details: JsonObject | None = None
        kind = ""
        if opcode == 0x02:
            kind = "direct-jump"
            details = {
                "delay_slot_address": _format_address(pc + 4),
                "target_address": _format_address(_jump_target(pc, word)),
            }
        elif opcode in simple_conditions:
            kind = "conditional-branch"
            details = {
                "condition": simple_conditions[opcode],
                "delay_slot_address": _format_address(pc + 4),
                "target_address": _format_address(_branch_target(pc, word)),
            }
        elif opcode == 0x01 and ((word >> 16) & 0x1F) in regimm_conditions:
            kind = "conditional-branch"
            details = {
                "condition": regimm_conditions[(word >> 16) & 0x1F],
                "delay_slot_address": _format_address(pc + 4),
                "target_address": _format_address(_branch_target(pc, word)),
            }
        elif opcode in {0x10, 0x11, 0x12, 0x13} and ((word >> 21) & 0x1F) == 0x08:
            cop_condition = (word >> 16) & 0x1F
            if cop_condition <= 3:
                kind = "coprocessor-branch"
                details = {
                    "condition": ("bcf", "bct", "bcfl", "bctl")[cop_condition],
                    "coprocessor": opcode - 0x10,
                    "delay_slot_address": _format_address(pc + 4),
                    "target_address": _format_address(_branch_target(pc, word)),
                }
        elif opcode == 0 and (word & 0x3F) == 0x08 and ((word >> 21) & 0x1F) != 31:
            kind = "indirect-jump"
            details = {
                "delay_slot_address": _format_address(pc + 4),
                "source_register": (word >> 21) & 0x1F,
            }
        if details is not None:
            branches.append(
                _branch(
                    spec,
                    function["function_id"],
                    offset,
                    runtime_address,
                    kind,
                    details,
                )
            )
    return branches


def _scan_artifact(
    spec: JsonObject, data: bytes, functions: list[JsonObject]
) -> tuple[JsonObject, list[JsonObject], list[JsonObject]]:
    mapping, payload = _validate_mapping(spec, data)
    sites: list[JsonObject] = []
    for executable_range in spec["executable_ranges"]:
        start = executable_range["payload_offset"]
        size = executable_range["size"]
        sites.extend(_scan_instruction_sites(spec, payload, start, size))
        sites.extend(_scan_inline_ot_sites(spec, payload, start, size))
    sites.sort(key=lambda item: item["site_id"])
    branches: list[JsonObject] = []
    for function in functions:
        branches.extend(_scan_branches(spec, payload, function))
    branches.sort(key=lambda item: item["branch_id"])
    artifact = {
        "artifact_id": spec["artifact_id"],
        "discs": spec["discs"],
        "executable_ranges": spec["executable_ranges"],
        "mapping": mapping,
        "role": spec["role"],
        "serials": spec["serials"],
        "sha256": spec["sha256"],
        "size": spec["size"],
    }
    return artifact, sites, branches


def _check_reviewed_ids(
    fact_ids: set[str], reviewed: list[JsonObject], key: str, label: str
) -> list[str]:
    reviewed_ids = {item[key] for item in reviewed}
    stale = sorted(reviewed_ids - fact_ids)
    if stale:
        raise CensusError(f"{label} contains stale or foreign IDs: {','.join(stale)}")
    return sorted(fact_ids - reviewed_ids)


def _render_relevant_sites(
    facts: JsonObject, render_targets: list[JsonObject]
) -> tuple[set[str], list[JsonObject]]:
    relevant = {
        site["site_id"]
        for site in facts["sites"]
        if site["kind"] in INTRINSIC_RENDER_SITE_KINDS
    }
    direct_targets = {
        (caller_id, target["address"]): target
        for target in render_targets
        for caller_id in target["caller_artifact_ids"]
    }
    facts_by_id = {site["site_id"]: site for site in facts["sites"]}
    bindings: list[JsonObject] = []
    for site in facts["sites"]:
        if site["kind"] != "direct-jal":
            continue
        target = direct_targets.get(
            (site["artifact_id"], site["details"]["target_address"])
        )
        if target is None:
            continue
        relevant.add(site["site_id"])
        bindings.append(
            {
                "role": target["role"],
                "site_id": site["site_id"],
                "target_id": target["target_id"],
            }
        )
    for target in render_targets:
        for site_id in target["indirect_call_site_ids"]:
            site = facts_by_id.get(site_id)
            if site is None or site["kind"] != "jalr":
                raise CensusError(
                    f"render target {target['target_id']} references a non-JALR indirect site"
                )
            if site["artifact_id"] not in target["caller_artifact_ids"]:
                raise CensusError(
                    f"render target {target['target_id']} indirect site has the wrong caller artifact"
                )
            relevant.add(site_id)
            bindings.append(
                {
                    "role": target["role"],
                    "site_id": site_id,
                    "target_id": target["target_id"],
                }
            )
    bindings.sort(key=lambda item: item["site_id"])
    return relevant, bindings


def _coverage(
    facts: JsonObject,
    classifications: JsonObject,
    render_relevant_site_ids: set[str],
) -> JsonObject:
    artifact_ids = {item["artifact_id"] for item in facts["artifacts"]}
    site_ids = {item["site_id"] for item in facts["sites"]}
    branch_ids = {item["branch_id"] for item in facts["branches"]}
    unreviewed_artifacts = _check_reviewed_ids(
        artifact_ids,
        classifications["artifacts"],
        "artifact_id",
        "classifications.artifacts",
    )
    unreviewed_scanned_sites = _check_reviewed_ids(
        site_ids, classifications["sites"], "site_id", "classifications.sites"
    )
    reviewed_site_ids = {item["site_id"] for item in classifications["sites"]}
    unreviewed_render_sites = sorted(render_relevant_site_ids - reviewed_site_ids)
    unreviewed_branches = _check_reviewed_ids(
        branch_ids,
        classifications["branches"],
        "branch_id",
        "classifications.branches",
    )
    site_counts: dict[str, int] = {}
    for site in facts["sites"]:
        site_counts[site["kind"]] = site_counts.get(site["kind"], 0) + 1
    reachable = sum(
        item["reachability"] == "reachable" for item in classifications["branches"]
    )
    candidate_calls = sorted(
        site["site_id"]
        for site in facts["sites"]
        if site["kind"] in {"direct-jal", "jalr"}
    )
    error_artifacts = sorted(
        item["artifact_id"]
        for item in classifications["artifacts"]
        if item["status"] == "error"
    )
    error_sites = sorted(
        item["site_id"]
        for item in classifications["sites"]
        if item["status"] == "error"
    )
    error_branches = sorted(
        item["branch_id"]
        for item in classifications["branches"]
        if item["status"] == "error"
    )
    unreviewed_site_set = set(unreviewed_scanned_sites)
    unreviewed_candidate_calls = sorted(set(candidate_calls) & unreviewed_site_set)
    return {
        "candidate_call_site_ids": candidate_calls,
        "error_branch_ids": error_branches,
        "error_branches": len(error_branches),
        "error_executable_image_ids": error_artifacts,
        "error_executable_images": len(error_artifacts),
        "error_scanned_site_ids": error_sites,
        "error_scanned_sites": len(error_sites),
        "render_relevant_site_ids": sorted(render_relevant_site_ids),
        "reachable_branches": reachable,
        "reviewed_branches": len(branch_ids) - len(unreviewed_branches),
        "reviewed_executable_images": len(artifact_ids) - len(unreviewed_artifacts),
        "reviewed_sites": len(site_ids) - len(unreviewed_scanned_sites),
        "site_counts": site_counts,
        "total_branches": len(branch_ids),
        "total_candidate_call_sites": len(candidate_calls),
        "total_executable_images": len(artifact_ids),
        "total_render_relevant_sites": len(render_relevant_site_ids),
        "total_sites": len(site_ids),
        "unreviewed_branch_ids": unreviewed_branches,
        "unreviewed_branches": len(unreviewed_branches),
        "unreviewed_candidate_call_site_ids": unreviewed_candidate_calls,
        "unreviewed_candidate_call_sites": len(unreviewed_candidate_calls),
        "unreviewed_executable_image_ids": unreviewed_artifacts,
        "unreviewed_executable_images": len(unreviewed_artifacts),
        "unreviewed_render_relevant_site_ids": unreviewed_render_sites,
        "unreviewed_render_relevant_sites": len(unreviewed_render_sites),
        "unreviewed_scanned_site_ids": unreviewed_scanned_sites,
        "unreviewed_scanned_sites": len(unreviewed_scanned_sites),
    }


def build_document(
    review_path: Path,
    artifact_paths: list[Path],
    *,
    require_complete: bool,
) -> JsonObject:
    review = _parse_review(review_path)
    loaded = _read_artifacts(artifact_paths, review["artifacts"])
    artifacts: list[JsonObject] = []
    sites: list[JsonObject] = []
    branches: list[JsonObject] = []
    functions_by_artifact: dict[str, list[JsonObject]] = {}
    for function in review["functions"]:
        functions_by_artifact.setdefault(function["artifact_id"], []).append(function)
    for spec in review["artifacts"]:
        artifact, artifact_sites, artifact_branches = _scan_artifact(
            spec,
            loaded[spec["sha256"]],
            functions_by_artifact.get(spec["artifact_id"], []),
        )
        artifacts.append(artifact)
        sites.extend(artifact_sites)
        branches.extend(artifact_branches)
    artifacts.sort(key=lambda item: item["artifact_id"])
    sites.sort(key=lambda item: item["site_id"])
    branches.sort(key=lambda item: item["branch_id"])
    facts = {"artifacts": artifacts, "branches": branches, "sites": sites}
    render_relevant_site_ids, target_bindings = _render_relevant_sites(
        facts, review["render_targets"]
    )
    coverage = _coverage(
        facts, review["classifications"], render_relevant_site_ids
    )
    blocker_keys = (
        "unreviewed_executable_images",
        "unreviewed_scanned_sites",
        "unreviewed_branches",
        "error_executable_images",
        "error_scanned_sites",
        "error_branches",
    )
    blocked = any(coverage[key] for key in blocker_keys)
    if require_complete and blocked:
        raise CensusError(
            "static ledger blocked: "
            + ",".join(f"{key}={coverage[key]}" for key in blocker_keys)
        )
    normalized_review = {
        "artifacts": review["artifacts"],
        "classifications": review["classifications"],
        "functions": review["functions"],
        "render_targets": review["render_targets"],
        "schema": review["schema"],
    }
    return {
        "coverage": coverage,
        "facts": facts,
        "known_limitations": list(KNOWN_LIMITATIONS),
        "p10_static_closure_claimed": require_complete and not blocked,
        "review_sha256": hashlib.sha256(_canonical_json(normalized_review)).hexdigest(),
        "reviewed_classifications": review["classifications"],
        "reviewed_scope": {
            "functions": review["functions"],
            "render_target_bindings": target_bindings,
            "render_targets": review["render_targets"],
        },
        "schema": LEDGER_SCHEMA if require_complete else INVENTORY_SCHEMA,
        "static_classification_gate": "blocked" if blocked else "pass",
    }


def _write_document(path: Path, document: JsonObject) -> None:
    parent = path.parent
    if not parent.is_dir():
        raise CensusError("output parent directory does not exist")
    encoded = _canonical_json(document)
    descriptor, temporary_name = tempfile.mkstemp(dir=parent, prefix=f".{path.name}.")
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "wb") as output:
            output.write(encoded)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    except OSError as error:
        raise CensusError("public document could not be written") from error
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Deterministic metadata-only MIPS executable census"
    )
    parser.add_argument("command", choices=("inventory", "ledger"))
    parser.add_argument("review", type=Path)
    parser.add_argument("--artifact", action="append", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        complete = arguments.command == "ledger"
        document = build_document(
            arguments.review, arguments.artifact, require_complete=complete
        )
        _write_document(arguments.out, document)
        if complete:
            print("ledger PASS: P10 Disc 1 static closure complete")
        else:
            print(
                "inventory PASS: "
                f"static classification gate {document['static_classification_gate']}"
            )
        return 0
    except CensusError as error:
        print(f"FAIL: {error}", file=os.sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
