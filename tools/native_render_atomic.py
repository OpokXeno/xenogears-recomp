from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path

from native_render_schema import (
    MAX_METADATA_BYTES,
    ContractError,
    JsonObject,
    JsonValue,
)


def _pairs(items: list[tuple[str, JsonValue]]) -> JsonObject:
    result: dict[str, JsonValue] = {}
    for key, value in items:
        if key in result:
            raise ContractError("json_duplicate_key")
        result[key] = value
    return result


def _non_finite(_token: str) -> None:
    raise ContractError("json_non_finite_number")


def load_bounded_json(raw: bytes) -> JsonValue:
    if len(raw) > MAX_METADATA_BYTES:
        raise ContractError("json_oversized")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ContractError("json_utf8_invalid") from error
    try:
        decoded = json.loads(text, object_pairs_hook=_pairs, parse_constant=_non_finite)
    except json.JSONDecodeError as error:
        raise ContractError("json_syntax_invalid") from error
    return decoded


def canonical_ascii_json(value: JsonValue) -> bytes:
    try:
        encoded = json.dumps(value, allow_nan=False, ensure_ascii=True, separators=(",", ":"), sort_keys=True).encode("ascii") + b"\n"
    except (TypeError, ValueError) as error:
        raise ContractError("json_value_invalid") from error
    if len(encoded) > MAX_METADATA_BYTES:
        raise ContractError("json_oversized")
    return encoded


def publish_once(destination: Path, value: JsonValue) -> None:
    encoded = canonical_ascii_json(value)
    parent = destination.parent
    if not parent.is_dir():
        raise ContractError("publication_parent_invalid")
    descriptor, temporary_name = tempfile.mkstemp(dir=parent, prefix=f".{destination.name}.")
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "wb") as output:
            _ = output.write(encoded)
            output.flush()
            os.fsync(output.fileno())
        try:
            os.link(temporary, destination)
        except FileExistsError as error:
            raise FileExistsError("publication destination exists") from error
        directory_descriptor = os.open(parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_descriptor)
        finally:
            os.close(directory_descriptor)
    finally:
        temporary.unlink(missing_ok=True)
