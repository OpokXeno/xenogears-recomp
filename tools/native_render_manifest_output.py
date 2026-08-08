from __future__ import annotations

import json
import os
from pathlib import Path
import re
import tempfile
from typing import Final, TypeAlias

from native_render_manifest_model import Digest32, ManifestError, fail
from native_render_manifest_verify import VerifiedManifest


TABLE_SCHEMA: Final = "xg-render-table-metadata/v1"
CONFIG_SCHEMA: Final = "xg-render-config-metadata/v1"
SHARD_SCHEMA: Final = "xg-render-shard-export/v1"
EVIDENCE_SCHEMA: Final = "xg-render-manifest-evidence/v2"
FIXTURE_SCHEMA: Final = "xg-render-manifest-fixture-evidence/v1"
PRIVACY = {
    "game_bytes_included": False,
    "overlay_bytes_included": False,
    "opcode_bytes_included": False,
    "window_bytes_included": False,
    "local_paths_included": False,
    "raw_hash_payloads_included": False,
}
MetadataValue: TypeAlias = str | int | bool | list["MetadataValue"] | dict[str, "MetadataValue"]
MetadataDocument: TypeAlias = dict[str, MetadataValue]


def atomic_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_name, path)
    except OSError as error:
        Path(temporary_name).unlink(missing_ok=True)
        raise ManifestError("atomic output failed") from error


def byte_initializer(value: Digest32) -> str:
    return ",".join(f"0x{byte:02x}" for byte in value)


def c_string(value: str) -> str:
    if re.fullmatch(r"[A-Za-z0-9-]*", value) is None:
        fail("unsafe metadata string reached emission")
    return value


def render_c(verified: VerifiedManifest) -> bytes:
    rows = []
    for record in verified.records:
        rows.append(
            "    { %du, \"%s\", %du, UINT32_C(0x%08x), UINT32_C(0x%08x), {%s}, {%s}, \"%s\", \"%s\" },"
            % (
                record.record_id,
                c_string(record.identifier),
                record.kind,
                record.address,
                record.target_address,
                byte_initializer(record.image_identity),
                byte_initializer(record.window_identity),
                c_string(record.framebuffer_context),
                c_string(record.ot_context),
            )
        )
    lines = [
        "#include \"xg_render_manifest_generated.h\"",
        "",
        f"const uint8_t xg_render_game_identity[32] = {{{byte_initializer(verified.game_identity)}}};",
        f"const uint8_t xg_render_manifest_identity[32] = {{{byte_initializer(verified.manifest_identity)}}};",
        f"const uint32_t xg_render_namespace_crc32 = UINT32_C(0x{verified.namespace_crc32:08x});",
        "const XgRenderManifestValidation xg_render_manifest_validation = {",
        f"    {verified.validation.producer_record_id}u, {verified.validation.site_record_id}u,",
        f"    UINT32_C(0x{verified.validation.field_base_crc32:08x}), UINT32_C(0x{verified.validation.field_range_crc32:08x}),",
        f"    UINT32_C(0x{verified.validation.field_range_start:08x}), {verified.validation.field_range_size}u,",
        f"    UINT32_C(0x{verified.validation.producer_entry:08x}), UINT32_C(0x{verified.validation.caller_site:08x}),",
        f"    UINT32_C(0x{verified.validation.static_callee:08x}), UINT32_C(0x{verified.validation.return_site:08x}),",
        f"    UINT32_C(0x{verified.validation.instruction_window_start:08x}), {verified.validation.instruction_window_size}u,",
        f"    {{{byte_initializer(verified.validation.instruction_window_identity)}}},",
        f"    {verified.validation.required_jal_opcode}u, UINT32_C(0x{verified.validation.jal_target:08x}),",
        f"    {verified.validation.required_delay_slot_instructions}u, {verified.validation.required_delay_slot_non_control_transfer}u,",
        "};",
        "const XgRenderManifestRecord xg_render_manifest_records[] = {",
        *rows,
        "};",
        f"const uint32_t xg_render_manifest_record_count = {len(rows)}u;",
        "",
    ]
    return "\n".join(lines).encode("ascii")


def table_payload(verified: VerifiedManifest) -> MetadataDocument:
    return {
        "schema": TABLE_SCHEMA,
        "game_identity": list(verified.game_identity),
        "manifest_identity": list(verified.manifest_identity),
        "namespace_crc32": f"{verified.namespace_crc32:08x}",
        "records": [
            {"id": record.identifier, "record_id": record.record_id}
            for record in verified.records
        ],
        "validation": {
            "producer_record_id": verified.validation.producer_record_id,
            "site_record_id": verified.validation.site_record_id,
            "field_base_crc32": f"{verified.validation.field_base_crc32:08x}",
            "field_range_crc32": f"{verified.validation.field_range_crc32:08x}",
            "field_range_start": verified.validation.field_range_start,
            "field_range_size": verified.validation.field_range_size,
            "producer_entry": verified.validation.producer_entry,
            "caller_site": verified.validation.caller_site,
            "static_callee": verified.validation.static_callee,
            "return_site": verified.validation.return_site,
            "instruction_window_start": verified.validation.instruction_window_start,
            "instruction_window_size": verified.validation.instruction_window_size,
            "instruction_window_identity": list(verified.validation.instruction_window_identity),
            "required_jal_opcode": verified.validation.required_jal_opcode,
            "jal_target": verified.validation.jal_target,
            "required_delay_slot_instructions": verified.validation.required_delay_slot_instructions,
            "required_delay_slot_non_control_transfer": verified.validation.required_delay_slot_non_control_transfer,
        },
    }


def configuration_payload(verified: VerifiedManifest) -> MetadataDocument:
    return {
        "schema": CONFIG_SCHEMA,
        "game_identity": verified.game_identity.hex(),
        "manifest_identity": verified.manifest_identity.hex(),
        "namespace_crc32": f"{verified.namespace_crc32:08x}",
    }


def json_bytes(payload: MetadataDocument) -> bytes:
    return (json.dumps(payload, sort_keys=True, indent=2) + "\n").encode("ascii")


def load_json(path: Path) -> MetadataDocument:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ManifestError("metadata document is unreadable") from error
    if not isinstance(payload, dict):
        fail("metadata document must be an object")
    return payload


def identity_bytes(value: MetadataValue, label: str) -> Digest32:
    if not isinstance(value, list) or len(value) != 32 or any(not isinstance(byte, int) or isinstance(byte, bool) or not 0 <= byte <= 255 for byte in value):
        fail(f"{label} must be exactly 32 decoded bytes")
    return Digest32(bytes(value))


def check_table_payload(verified: VerifiedManifest, payload: MetadataDocument) -> None:
    if set(payload) != {"schema", "game_identity", "manifest_identity", "namespace_crc32", "records", "validation"} or payload["schema"] != TABLE_SCHEMA:
        fail("table metadata fields are not closed")
    namespace = payload["namespace_crc32"]
    if namespace != f"{verified.namespace_crc32:08x}":
        fail("table namespace metadata mismatch")
    if identity_bytes(payload["game_identity"], "table game identity") != verified.game_identity:
        fail("namespace collision: same namespace CRC with different full SHA-256")
    if identity_bytes(payload["manifest_identity"], "table manifest identity") != verified.manifest_identity:
        fail("stale table manifest identity")
    expected = table_payload(verified)
    if payload["records"] != expected["records"]:
        fail("stale table record metadata")
    if payload["validation"] != expected["validation"]:
        fail("table runtime validation metadata mismatch")


def check_shard_payload(verified: VerifiedManifest, payload: MetadataDocument) -> None:
    if set(payload) != {"schema", "game_identity", "manifest_identity", "namespace_crc32"} or payload["schema"] != SHARD_SCHEMA:
        fail("shard-export fields are not closed")
    game = identity_bytes(payload["game_identity"], "shard-export game identity")
    manifest = identity_bytes(payload["manifest_identity"], "shard-export manifest identity")
    if game != verified.game_identity or manifest != verified.manifest_identity or payload["namespace_crc32"] != f"{verified.namespace_crc32:08x}":
        fail("shard-export identity mismatch")


def evidence_payload(verified: VerifiedManifest) -> MetadataDocument:
    return {
        "schema": EVIDENCE_SCHEMA,
        "state": "pass",
        "authenticated_records": [record.identifier for record in verified.records],
        "privacy": PRIVACY,
    }


def validate_evidence_payload(payload: MetadataDocument) -> None:
    schemas = {
        EVIDENCE_SCHEMA: {"schema", "state", "authenticated_records", "privacy"},
        FIXTURE_SCHEMA: {"schema", "state", "positive_paths", "rejected_cases", "canonical_blockers", "privacy", "cleanup"},
    }
    schema = payload.get("schema")
    expected = schemas.get(schema) if isinstance(schema, str) else None
    if expected is None or set(payload) != expected:
        fail("evidence fields are not closed")
    if payload["state"] != "pass" or payload["privacy"] != PRIVACY:
        fail("evidence state or privacy contract is invalid")
    if schema == EVIDENCE_SCHEMA:
        records = payload["authenticated_records"]
        if not isinstance(records, list) or any(not isinstance(record, str) or re.fullmatch(r"[a-z0-9-]+", record) is None for record in records):
            fail("authenticated evidence records are invalid")
        if records != sorted(set(records)):
            fail("authenticated evidence records are invalid")
    if schema == FIXTURE_SCHEMA:
        if payload["positive_paths"] != ["validate", "emit", "check-metadata"]:
            fail("fixture positive paths are invalid")
        expected_cases = ["missing-identity", "full-identity-mismatch", "same-namespace-different-full-sha", "stale-manifest-table-metadata", "shard-export-identity-mismatch"]
        if payload["rejected_cases"] != expected_cases:
            fail("fixture rejected cases are invalid")
        blockers = payload["canonical_blockers"]
        if not isinstance(blockers, list) or any(not isinstance(blocker, str) or re.fullmatch(r"[a-z0-9-]+", blocker) is None for blocker in blockers):
            fail("fixture canonical blockers are invalid")
        if blockers != sorted(set(blockers)):
            fail("fixture canonical blockers are invalid")
        if payload["cleanup"] != {"temporary_inputs_created": True, "temporary_inputs_cleaned": True}:
            fail("fixture cleanup receipt is invalid")
    if re.search(r"[0-9a-f]{64}", json.dumps(payload)) is not None:
        fail("evidence contains an arbitrary raw hash-shaped payload")
