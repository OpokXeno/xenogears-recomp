from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
import tempfile
import tomllib
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Final, NoReturn
from urllib.parse import unquote


CERTIFICATION_SCHEMA: Final = "xenogears.native-3d-certification/v1"
MATRIX_RECEIPT_SCHEMA: Final = "xenogears.native-3d-matrix-receipt/v1"
AGGREGATE_RECEIPT_SCHEMA: Final = "xenogears.native-3d-aggregate-receipt/v1"
EVIDENCE_SCHEMA: Final = "xenogears.native-3d-certification-evidence/v1"
PASS_VERDICT: Final = "NATIVE_RENDER_3D_CERTIFICATION_PASS"
MAX_METADATA_BYTES: Final = 1_000_000
MAX_MATRIX_ROWS: Final = 100_000
MODES: Final = ("original", "shadow", "native")
TIERS: Final = frozenset({"static", "cold", "warm"})
CONFIGURATIONS: Final = frozenset({"debug", "release"})
DOCUMENT_KINDS: Final = frozenset({"ledger", "contract", "branch-ledger", "unreachable-branch-proof"})
REQUIRED_DOCUMENT_KINDS: Final = frozenset({"ledger", "contract", "branch-ledger"})
AGGREGATE_KINDS: Final = frozenset(
    {
        "shadow-layers",
        "gte-attribution-residual",
        "source-isolation-poisoning",
        "ordering-material-vram-framebuffer",
        "fault-rollback",
        "coverage",
    }
)
_HEX_64: Final = re.compile(r"[0-9a-f]{64}\Z")
_IDENTIFIER: Final = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}\Z")
_VERSION: Final = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+ -]{0,127}\Z")
_WINDOWS_ABSOLUTE: Final = re.compile(r"(?i)(?:^|[\s\"'=])[a-z]:[\\/]")
_POSIX_ABSOLUTE: Final = re.compile(r"(?:^|[\s\"'=])/(?:[^/]|$)")
_UNC_PATH: Final = re.compile(r"(?:^|[\s\"'=])\\\\[^\\]+\\")
_BASE64: Final = re.compile(r"[A-Za-z0-9+/]{512,}={0,2}\Z")
_PRIVATE_MARKERS: Final = (
    "ps-x exe",
    "overlay_captures",
    "savestate",
    "memory card",
    "xenogearsrecomp/",
)
_PRIVATE_KEYS: Final = frozenset(
    {
        "bytes",
        "bytes_b64",
        "data_b64",
        "framebuffer",
        "image_bytes",
        "packet",
        "packet_bytes",
        "packet_words",
        "packets",
        "payload",
        "pixels",
        "raw",
        "savestate",
        "texture_bytes",
        "vram_bytes",
        "vram_data",
    }
)

ROOT_FIELDS: Final = frozenset(
    {
        "schema",
        "sealed",
        "certification_id",
        "repetitions",
        "nonsemantic_whitelist",
        "parity",
        "expected",
        "documents",
        "discs",
        "hosts",
        "toolchains",
        "builds",
        "images",
        "families",
        "producers",
        "branches",
        "scenarios",
        "matrix",
        "aggregate_receipts",
    }
)
PARITY_FIELDS: Final = frozenset(
    {"renderer", "present", "color_depth_bits", "interpolation", "smooth", "wide", "hires"}
)
EXPECTED_FIELDS: Final = frozenset(
    {
        "ledger_render_sites",
        "migrated_3d_sites",
        "excluded_pure_2d_proven_sites",
        "non_render_proven_sites",
    }
)
DOCUMENT_FIELDS: Final = frozenset({"id", "kind", "path", "sha256"})
DISC_FIELDS: Final = frozenset({"id", "number"})
HOST_FIELDS: Final = frozenset({"id", "os", "architecture"})
TOOLCHAIN_FIELDS: Final = frozenset({"id", "version", "hosts"})
BUILD_FIELDS: Final = frozenset({"id", "configuration", "toolchain", "host", "sha256"})
IMAGE_FIELDS: Final = frozenset({"id", "disc", "tier", "sha256", "base_address", "generation"})
FAMILY_FIELDS: Final = frozenset({"id"})
PRODUCER_FIELDS: Final = frozenset({"id", "family", "discs", "tiers"})
BRANCH_FIELDS: Final = frozenset({"id", "producer", "reachable", "proof_document"})
SCENARIO_FIELDS: Final = frozenset(
    {"id", "disc", "producers", "branches", "tiers", "ordinary_input", "private_artifacts_authenticated"}
)
MATRIX_FIELDS: Final = frozenset(
    {"disc", "build", "scenario", "producer", "tier", "requested_mode", "repetition", "receipt", "sha256"}
)
AGGREGATE_REF_FIELDS: Final = frozenset({"kind", "receipt", "sha256"})

RECEIPT_ROOT_FIELDS: Final = frozenset(
    {"schema", "run_id", "row", "identity", "coverage", "isolation", "parity", "checks", "fallback", "cleanup", "nonsemantic"}
)
RECEIPT_ROW_FIELDS: Final = frozenset(
    {
        "disc",
        "build",
        "scenario",
        "producer",
        "tier",
        "requested_mode",
        "effective_mode",
        "repetition",
        "renderer",
        "present",
        "color_depth_bits",
        "interpolation",
        "smooth",
        "wide",
        "hires",
    }
)
IDENTITY_FIELDS: Final = frozenset(
    {"image_id", "image_sha256", "base_address", "generation", "build_sha256", "producer", "branches"}
)
COVERAGE_FIELDS: Final = frozenset(
    {
        "target_3d_producers",
        "migrated_3d_producers",
        "exercised_3d_producers",
        "target_3d_families",
        "migrated_3d_families",
        "exercised_3d_families",
        "target_reachable_render_branches",
        "migrated_render_branches",
        "exercised_render_branches",
        "ledger_render_sites",
        "migrated_3d_sites",
        "excluded_pure_2d_proven_sites",
        "non_render_proven_sites",
        "unclassified_render_gte_sites",
        "unclassified_reachable_render_branches",
        "unattributed_original_3d_primitives",
    }
)
ISOLATION_FIELDS: Final = frozenset(
    {
        "render_gte_exec_count",
        "native_target_gte_site_hits_static",
        "native_target_gte_site_hits_cold",
        "native_target_gte_site_hits_warm",
        "semantic_post_gte_reads",
        "target_packet_payload_reads_by_semantic_lane",
        "target_gp0_decode_to_semantic_calls",
        "target_ot_payload_geometry_or_material_reads",
    }
)
TARGET_HIT_FIELDS: Final = {
    "static": "native_target_gte_site_hits_static",
    "cold": "native_target_gte_site_hits_cold",
    "warm": "native_target_gte_site_hits_warm",
}
SEMANTIC_ISOLATION_FIELDS: Final = frozenset(
    {
        "semantic_post_gte_reads",
        "target_packet_payload_reads_by_semantic_lane",
        "target_gp0_decode_to_semantic_calls",
        "target_ot_payload_geometry_or_material_reads",
    }
)
PARITY_RECEIPT_FIELDS: Final = frozenset(
    {
        "original_normalized_primitives",
        "native_normalized_ir_primitives",
        "source_snapshot_digest",
        "source_poisoned_snapshot_digest",
        "model_animation_original_digest",
        "model_animation_shadow_digest",
        "model_animation_native_digest",
        "camera_matrix_light_original_digest",
        "camera_matrix_light_shadow_digest",
        "camera_matrix_light_native_digest",
        "culling_depth_original_digest",
        "culling_depth_shadow_digest",
        "culling_depth_native_digest",
        "ir_semantics_original_digest",
        "ir_semantics_shadow_digest",
        "ir_semantics_native_digest",
        "original_ordering_digest",
        "shadow_ordering_digest",
        "native_ordering_digest",
        "ps1_state_original_digest",
        "ps1_state_shadow_digest",
        "ps1_state_native_digest",
        "shadow_original_initial_vram_digest",
        "shadow_native_initial_vram_digest",
        "original_vram_digest",
        "shadow_vram_digest",
        "native_vram_digest",
        "original_display15_digest",
        "shadow_display15_digest",
        "native_display15_digest",
        "original_host_framebuffer_digest",
        "shadow_host_framebuffer_digest",
        "native_host_framebuffer_digest",
        "host_output_digest",
        "poisoned_host_output_digest",
        "semantic_ir_digest",
        "poisoned_semantic_ir_digest",
    }
)
CHECK_FIELDS: Final = frozenset(
    {
        "host_state_isolated",
        "static_coverage_complete",
        "dynamic_coverage_complete",
        "source_snapshot_complete",
        "source_snapshot_authenticated",
        "source_defaults_demonstrated",
        "model_animation_parity_or_absence_proven",
        "camera_matrix_light_exact",
        "culling_depth_capacity_counts_exact",
        "ir_semantics_exact",
        "ordering_bucket_ordinal_neighbors_exact",
        "ps1_state_exact",
        "vram_sampling_ordinal_exact",
        "framebuffer_normalized_exact",
        "primitive_provenance_complete",
        "poisoning_unchanged",
        "atomic_fault_injection_exercised",
        "atomic_gpu_runtime_guest_restored",
        "atomic_original_before_observation",
        "atomic_zero_double_effects",
        "original_selectable",
        "original_baseline_sealed",
        "original_native_adapter_independent",
    }
)
FALLBACK_FIELDS: Final = frozenset(
    {"qualified_native", "fallback_count", "overflow_count", "stale_state_count", "unsupported_material_count"}
)
CLEANUP_FIELDS: Final = frozenset(
    {
        "complete",
        "guest_state_restored",
        "gpu_state_restored",
        "runtime_state_restored",
        "private_artifacts_removed",
        "live_processes",
        "stale_leases",
    }
)
AGGREGATE_ROOT_FIELDS: Final = frozenset(
    {"schema", "run_id", "kind", "matrix_sha256", "metrics", "cleanup", "nonsemantic"}
)
AGGREGATE_METRIC_FIELDS: Final = frozenset(
    {
        "row_count",
        "target_3d_producers",
        "migrated_3d_producers",
        "exercised_3d_producers",
        "target_3d_families",
        "migrated_3d_families",
        "exercised_3d_families",
        "target_reachable_render_branches",
        "migrated_render_branches",
        "exercised_render_branches",
        "unclassified_render_gte_sites",
        "unclassified_reachable_render_branches",
        "unattributed_original_3d_primitives",
        "render_gte_exec_count",
        "native_target_gte_site_hits",
        "semantic_post_gte_reads",
        "target_packet_payload_reads_by_semantic_lane",
        "target_gp0_decode_to_semantic_calls",
        "target_ot_payload_geometry_or_material_reads",
        "native_qualified_fallback_count",
        "mismatch_count",
        "unknown_identity_count",
        "cleanup_incomplete_count",
        "privacy_violation_count",
        "nondeterministic_group_count",
    }
)


class CertificationError(ValueError):
    def __init__(self, code: str) -> None:
        self.code = code
        super().__init__(code)


def _fail(code: str) -> NoReturn:
    raise CertificationError(code)


def _closed(value: Any, fields: frozenset[str], name: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != fields:
        _fail("SCHEMA")
    return value


def _array(value: Any, name: str, *, nonempty: bool = True) -> list[Any]:
    if not isinstance(value, list) or (nonempty and not value):
        _fail("SCHEMA")
    return value


def _string(value: Any, name: str) -> str:
    if not isinstance(value, str):
        _fail("SCHEMA")
    return value


def _identifier(value: Any, name: str) -> str:
    parsed = _string(value, name)
    if _IDENTIFIER.fullmatch(parsed) is None:
        _fail("SCHEMA")
    return parsed


def _version(value: Any, name: str) -> str:
    parsed = _string(value, name)
    if _VERSION.fullmatch(parsed) is None:
        _fail("SCHEMA")
    return parsed


def _digest(value: Any, name: str) -> str:
    parsed = _string(value, name)
    if _HEX_64.fullmatch(parsed) is None:
        _fail("SCHEMA")
    return parsed


def _integer(value: Any, name: str, *, minimum: int = 0, maximum: int = (1 << 64) - 1) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum or value > maximum:
        _fail("SCHEMA")
    return value


def _boolean(value: Any, name: str) -> bool:
    if not isinstance(value, bool):
        _fail("SCHEMA")
    return value


def _choice(value: Any, choices: set[str] | frozenset[str] | tuple[str, ...], name: str) -> str:
    parsed = _string(value, name)
    if parsed not in choices:
        _fail("SCHEMA")
    return parsed


def _identifier_array(value: Any, name: str, *, nonempty: bool = True) -> tuple[str, ...]:
    result = tuple(_identifier(item, name) for item in _array(value, name, nonempty=nonempty))
    if len(result) != len(set(result)):
        _fail("SCHEMA")
    return result


def _privacy_text(value: str) -> None:
    for candidate in (value, unquote(value), unquote(unquote(value))):
        lowered = candidate.casefold()
        if any(marker in lowered for marker in _PRIVATE_MARKERS):
            _fail("PRIVACY")
        if candidate.startswith(("/", "~/", "file:/")):
            _fail("PRIVACY")
        if _WINDOWS_ABSOLUTE.search(candidate) or _POSIX_ABSOLUTE.search(candidate) or _UNC_PATH.search(candidate):
            _fail("PRIVACY")
        if _BASE64.fullmatch(candidate) is not None:
            _fail("PRIVACY")


def _privacy_value(value: Any) -> None:
    if isinstance(value, str):
        _privacy_text(value)
    elif isinstance(value, list):
        for item in value:
            _privacy_value(item)
    elif isinstance(value, Mapping):
        for key, item in value.items():
            if not isinstance(key, str):
                _fail("SCHEMA")
            lowered = key.casefold()
            if lowered in _PRIVATE_KEYS or lowered.startswith(("raw_", "payload_")):
                _fail("PRIVACY")
            _privacy_text(key)
            _privacy_value(item)
    elif value is not None and not isinstance(value, (bool, int, float)):
        _fail("SCHEMA")


def _json_pairs(items: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in items:
        if key in result:
            _fail("SCHEMA")
        result[key] = value
    return result


def _reject_nonfinite(_token: str) -> NoReturn:
    _fail("SCHEMA")


def _load_json(raw: bytes) -> dict[str, Any]:
    if len(raw) > MAX_METADATA_BYTES:
        _fail("IO")
    try:
        decoded = json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_json_pairs,
            parse_constant=_reject_nonfinite,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CertificationError("SCHEMA") from error
    if not isinstance(decoded, dict):
        _fail("SCHEMA")
    _privacy_value(decoded)
    return decoded


def _relative_path(value: Any, name: str) -> PurePosixPath:
    text = _string(value, name)
    _privacy_text(text)
    path = PurePosixPath(text)
    if path.is_absolute() or not path.parts or any(part in {"", ".", ".."} for part in path.parts) or "\\" in text:
        _fail("PRIVACY")
    return path


def _read_relative(root: Path, relative: PurePosixPath) -> bytes:
    path = root.joinpath(*relative.parts)
    try:
        status = path.lstat()
        if stat.S_ISLNK(status.st_mode) or not stat.S_ISREG(status.st_mode):
            _fail("IO")
        if status.st_size > MAX_METADATA_BYTES:
            _fail("IO")
        resolved_root = root.resolve(strict=True)
        resolved_path = path.resolve(strict=True)
        if not resolved_path.is_relative_to(resolved_root):
            _fail("PRIVACY")
        return path.read_bytes()
    except CertificationError:
        raise
    except OSError as error:
        raise CertificationError("IO") from error


def _verify_file(root: Path, path_value: Any, digest_value: Any) -> tuple[bytes, str]:
    relative = _relative_path(path_value, "path")
    expected = _digest(digest_value, "sha256")
    raw = _read_relative(root, relative)
    if hashlib.sha256(raw).hexdigest() != expected:
        _fail("HASH_MISMATCH")
    return raw, expected


def _unique_records(value: Any, fields: frozenset[str], name: str, key: str = "id") -> tuple[list[dict[str, Any]], dict[str, dict[str, Any]]]:
    records = [_closed(item, fields, name) for item in _array(value, name)]
    by_id: dict[str, dict[str, Any]] = {}
    for record in records:
        identifier = _identifier(record[key], f"{name}.{key}")
        if identifier in by_id:
            _fail("SCHEMA")
        by_id[identifier] = record
    return records, by_id


@dataclass(frozen=True, slots=True)
class MatrixKey:
    disc: str
    build: str
    scenario: str
    producer: str
    tier: str
    requested_mode: str
    repetition: int


@dataclass(slots=True)
class Contract:
    source_sha256: str
    root: Path
    certification_id: str
    repetitions: int
    whitelist: tuple[str, ...]
    parity: dict[str, Any]
    expected: dict[str, int]
    documents: list[dict[str, Any]]
    discs: dict[str, dict[str, Any]]
    hosts: dict[str, dict[str, Any]]
    toolchains: dict[str, dict[str, Any]]
    builds: dict[str, dict[str, Any]]
    images: dict[tuple[str, str], dict[str, Any]]
    families: dict[str, dict[str, Any]]
    producers: dict[str, dict[str, Any]]
    branches: dict[str, dict[str, Any]]
    scenarios: dict[str, dict[str, Any]]
    matrix: dict[MatrixKey, dict[str, Any]]
    aggregates: dict[str, dict[str, Any]]


def _parse_contract(path: Path, renderer: str) -> Contract:
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise CertificationError("IO") from error
    if len(raw) > MAX_METADATA_BYTES:
        _fail("IO")
    try:
        decoded = tomllib.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        raise CertificationError("SCHEMA") from error
    _privacy_value(decoded)
    document = _closed(decoded, ROOT_FIELDS, "certification")
    if _string(document["schema"], "schema") != CERTIFICATION_SCHEMA:
        _fail("SCHEMA")
    if not _boolean(document["sealed"], "sealed"):
        _fail("UNSEALED")
    certification_id = _identifier(document["certification_id"], "certification_id")
    repetitions = _integer(document["repetitions"], "repetitions", minimum=2, maximum=100)
    whitelist = _identifier_array(document["nonsemantic_whitelist"], "nonsemantic_whitelist", nonempty=False)

    parity = _closed(document["parity"], PARITY_FIELDS, "parity")
    if _string(parity["renderer"], "renderer") != "opengl" or renderer != parity["renderer"]:
        _fail("RENDERER_MISMATCH")
    if _string(parity["present"], "present") != "canonical":
        _fail("SCHEMA")
    if _integer(parity["color_depth_bits"], "color_depth_bits") != 15:
        _fail("SCHEMA")
    for field in ("interpolation", "smooth", "wide", "hires"):
        if _boolean(parity[field], field):
            _fail("SCHEMA")

    expected_raw = _closed(document["expected"], EXPECTED_FIELDS, "expected")
    expected = {field: _integer(expected_raw[field], field) for field in EXPECTED_FIELDS}
    if expected["ledger_render_sites"] != (
        expected["migrated_3d_sites"]
        + expected["excluded_pure_2d_proven_sites"]
        + expected["non_render_proven_sites"]
    ) or expected["ledger_render_sites"] == 0 or expected["migrated_3d_sites"] == 0:
        _fail("SCHEMA")

    documents, documents_by_id = _unique_records(document["documents"], DOCUMENT_FIELDS, "documents")
    document_paths: set[PurePosixPath] = set()
    kinds: set[str] = set()
    for item in documents:
        kind = _choice(item["kind"], DOCUMENT_KINDS, "documents.kind")
        relative = _relative_path(item["path"], "documents.path")
        _digest(item["sha256"], "documents.sha256")
        if relative in document_paths:
            _fail("SCHEMA")
        document_paths.add(relative)
        kinds.add(kind)
    if not REQUIRED_DOCUMENT_KINDS.issubset(kinds):
        _fail("MISSING_RECEIPT")

    discs_raw, discs = _unique_records(document["discs"], DISC_FIELDS, "discs")
    disc_numbers = {_integer(item["number"], "discs.number", minimum=1, maximum=2) for item in discs_raw}
    if disc_numbers != {1, 2} or len(discs) != 2:
        _fail("SCHEMA")

    hosts_raw, hosts = _unique_records(document["hosts"], HOST_FIELDS, "hosts")
    for item in hosts_raw:
        _identifier(item["os"], "hosts.os")
        _identifier(item["architecture"], "hosts.architecture")

    toolchains_raw, toolchains = _unique_records(document["toolchains"], TOOLCHAIN_FIELDS, "toolchains")
    supported_pairs: set[tuple[str, str]] = set()
    for item in toolchains_raw:
        toolchain_id = _identifier(item["id"], "toolchains.id")
        _version(item["version"], "toolchains.version")
        toolchain_hosts = _identifier_array(item["hosts"], "toolchains.hosts")
        if any(host not in hosts for host in toolchain_hosts):
            _fail("SCHEMA")
        supported_pairs.update((toolchain_id, host) for host in toolchain_hosts)
    if {host for _, host in supported_pairs} != set(hosts):
        _fail("SCHEMA")

    builds_raw, builds = _unique_records(document["builds"], BUILD_FIELDS, "builds")
    build_combinations: set[tuple[str, str, str]] = set()
    for item in builds_raw:
        configuration = _choice(item["configuration"], CONFIGURATIONS, "builds.configuration")
        toolchain = _identifier(item["toolchain"], "builds.toolchain")
        host = _identifier(item["host"], "builds.host")
        if (toolchain, host) not in supported_pairs:
            _fail("SCHEMA")
        combination = (toolchain, host, configuration)
        if combination in build_combinations:
            _fail("SCHEMA")
        build_combinations.add(combination)
        _digest(item["sha256"], "builds.sha256")
    required_builds = {(toolchain, host, configuration) for toolchain, host in supported_pairs for configuration in CONFIGURATIONS}
    if build_combinations != required_builds:
        _fail("MISSING_ROW")

    images_raw, images_by_id = _unique_records(document["images"], IMAGE_FIELDS, "images")
    images: dict[tuple[str, str], dict[str, Any]] = {}
    for item in images_raw:
        disc = _identifier(item["disc"], "images.disc")
        tier = _choice(item["tier"], TIERS, "images.tier")
        if disc not in discs or (disc, tier) in images:
            _fail("SCHEMA")
        _digest(item["sha256"], "images.sha256")
        _integer(item["base_address"], "images.base_address", maximum=0xFFFFFFFF)
        _integer(item["generation"], "images.generation", maximum=0xFFFFFFFF)
        images[(disc, tier)] = item
    if set(images) != {(disc, tier) for disc in discs for tier in TIERS} or len(images_by_id) != len(images):
        _fail("MISSING_ROW")

    _, families = _unique_records(document["families"], FAMILY_FIELDS, "families")
    producers_raw, producers = _unique_records(document["producers"], PRODUCER_FIELDS, "producers")
    for item in producers_raw:
        if _identifier(item["family"], "producers.family") not in families:
            _fail("SCHEMA")
        producer_discs = _identifier_array(item["discs"], "producers.discs")
        producer_tiers = tuple(_choice(tier, TIERS, "producers.tiers") for tier in _array(item["tiers"], "producers.tiers"))
        if len(producer_tiers) != len(set(producer_tiers)) or any(disc not in discs for disc in producer_discs):
            _fail("SCHEMA")

    branches_raw, branches = _unique_records(document["branches"], BRANCH_FIELDS, "branches")
    for item in branches_raw:
        producer = _identifier(item["producer"], "branches.producer")
        if producer not in producers:
            _fail("UNKNOWN_PRODUCER")
        reachable = _boolean(item["reachable"], "branches.reachable")
        proof = _string(item["proof_document"], "branches.proof_document")
        if reachable:
            if proof:
                _fail("SCHEMA")
        else:
            if proof not in documents_by_id or documents_by_id[proof]["kind"] != "unreachable-branch-proof":
                _fail("MISSING_RECEIPT")
    if any(not any(branch["producer"] == producer for branch in branches_raw) for producer in producers):
        _fail("MISSING_ROW")

    scenarios_raw, scenarios = _unique_records(document["scenarios"], SCENARIO_FIELDS, "scenarios")
    covered_producer_tiers: set[tuple[str, str, str]] = set()
    covered_branches: set[str] = set()
    for item in scenarios_raw:
        disc = _identifier(item["disc"], "scenarios.disc")
        scenario_producers = _identifier_array(item["producers"], "scenarios.producers")
        scenario_branches = _identifier_array(item["branches"], "scenarios.branches", nonempty=False)
        scenario_tiers = tuple(_choice(tier, TIERS, "scenarios.tiers") for tier in _array(item["tiers"], "scenarios.tiers"))
        if disc not in discs or len(scenario_tiers) != len(set(scenario_tiers)):
            _fail("SCHEMA")
        if not _boolean(item["ordinary_input"], "scenarios.ordinary_input") or not _boolean(
            item["private_artifacts_authenticated"], "scenarios.private_artifacts_authenticated"
        ):
            _fail("INVARIANT_MISMATCH")
        for producer in scenario_producers:
            if producer not in producers:
                _fail("UNKNOWN_PRODUCER")
            producer_record = producers[producer]
            if disc not in producer_record["discs"] or any(tier not in producer_record["tiers"] for tier in scenario_tiers):
                _fail("SCHEMA")
            covered_producer_tiers.update((producer, disc, tier) for tier in scenario_tiers)
        for branch in scenario_branches:
            if branch not in branches:
                _fail("UNKNOWN_BRANCH")
            branch_record = branches[branch]
            if not branch_record["reachable"] or branch_record["producer"] not in scenario_producers:
                _fail("SCHEMA")
            covered_branches.add(branch)
    required_producer_tiers = {
        (producer_id, disc, tier)
        for producer_id, producer in producers.items()
        for disc in producer["discs"]
        for tier in producer["tiers"]
    }
    if covered_producer_tiers != required_producer_tiers:
        _fail("MISSING_ROW")
    reachable_branches = {identifier for identifier, item in branches.items() if item["reachable"]}
    if covered_branches != reachable_branches:
        _fail("MISSING_ROW")

    matrix_records = [_closed(item, MATRIX_FIELDS, "matrix") for item in _array(document["matrix"], "matrix")]
    if len(matrix_records) > MAX_MATRIX_ROWS:
        _fail("SCHEMA")
    matrix: dict[MatrixKey, dict[str, Any]] = {}
    matrix_paths: set[PurePosixPath] = set()
    for item in matrix_records:
        producer = _identifier(item["producer"], "matrix.producer")
        if producer not in producers:
            _fail("UNKNOWN_PRODUCER")
        key = MatrixKey(
            _identifier(item["disc"], "matrix.disc"),
            _identifier(item["build"], "matrix.build"),
            _identifier(item["scenario"], "matrix.scenario"),
            producer,
            _choice(item["tier"], TIERS, "matrix.tier"),
            _choice(item["requested_mode"], MODES, "matrix.requested_mode"),
            _integer(item["repetition"], "matrix.repetition", minimum=1, maximum=repetitions),
        )
        receipt_path = _relative_path(item["receipt"], "matrix.receipt")
        _digest(item["sha256"], "matrix.sha256")
        if key in matrix or receipt_path in matrix_paths:
            _fail("SCHEMA")
        matrix[key] = item
        matrix_paths.add(receipt_path)

    required_matrix: set[MatrixKey] = set()
    for scenario_id, scenario in scenarios.items():
        for producer in scenario["producers"]:
            for tier in scenario["tiers"]:
                for build in builds:
                    for mode in MODES:
                        for repetition in range(1, repetitions + 1):
                            required_matrix.add(
                                MatrixKey(scenario["disc"], build, scenario_id, producer, tier, mode, repetition)
                            )
    if set(matrix) != required_matrix:
        _fail("MISSING_ROW")

    aggregate_records = [_closed(item, AGGREGATE_REF_FIELDS, "aggregate_receipts") for item in _array(document["aggregate_receipts"], "aggregate_receipts")]
    aggregates: dict[str, dict[str, Any]] = {}
    for item in aggregate_records:
        kind = _choice(item["kind"], AGGREGATE_KINDS, "aggregate_receipts.kind")
        receipt_path = _relative_path(item["receipt"], "aggregate_receipts.receipt")
        _digest(item["sha256"], "aggregate_receipts.sha256")
        if kind in aggregates or receipt_path in matrix_paths:
            _fail("SCHEMA")
        aggregates[kind] = item
        matrix_paths.add(receipt_path)
    if set(aggregates) != AGGREGATE_KINDS:
        _fail("MISSING_RECEIPT")

    return Contract(
        hashlib.sha256(raw).hexdigest(),
        path.parent,
        certification_id,
        repetitions,
        whitelist,
        parity,
        expected,
        documents,
        discs,
        hosts,
        toolchains,
        builds,
        images,
        families,
        producers,
        branches,
        scenarios,
        matrix,
        aggregates,
    )


def _expected_coverage(contract: Contract) -> dict[str, int]:
    producer_count = len(contract.producers)
    family_count = len(contract.families)
    branch_count = sum(1 for branch in contract.branches.values() if branch["reachable"])
    return {
        "target_3d_producers": producer_count,
        "migrated_3d_producers": producer_count,
        "exercised_3d_producers": producer_count,
        "target_3d_families": family_count,
        "migrated_3d_families": family_count,
        "exercised_3d_families": family_count,
        "target_reachable_render_branches": branch_count,
        "migrated_render_branches": branch_count,
        "exercised_render_branches": branch_count,
        **contract.expected,
        "unclassified_render_gte_sites": 0,
        "unclassified_reachable_render_branches": 0,
        "unattributed_original_3d_primitives": 0,
    }


def _parse_nonsemantic(value: Any, whitelist: tuple[str, ...]) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != set(whitelist):
        _fail("SCHEMA")
    for item in value.values():
        if not isinstance(item, (str, int, bool)) or isinstance(item, float):
            _fail("SCHEMA")
    _privacy_value(value)
    return value


def _parse_cleanup(value: Any) -> dict[str, Any]:
    cleanup = _closed(value, CLEANUP_FIELDS, "cleanup")
    for field in CLEANUP_FIELDS - {"live_processes", "stale_leases"}:
        if not _boolean(cleanup[field], field):
            _fail("CLEANUP_INCOMPLETE")
    if _integer(cleanup["live_processes"], "live_processes") != 0 or _integer(cleanup["stale_leases"], "stale_leases") != 0:
        _fail("CLEANUP_INCOMPLETE")
    return cleanup


def _equal_digests(parity: dict[str, Any], names: tuple[str, ...]) -> None:
    values = [_digest(parity[name], name) for name in names]
    if len(set(values)) != 1:
        _fail("INVARIANT_MISMATCH")


def _parse_matrix_receipt(contract: Contract, key: MatrixKey, value: dict[str, Any]) -> tuple[str, bytes, bytes]:
    receipt = _closed(value, RECEIPT_ROOT_FIELDS, "receipt")
    if _string(receipt["schema"], "schema") != MATRIX_RECEIPT_SCHEMA:
        _fail("SCHEMA")
    run_id = _digest(receipt["run_id"], "run_id")
    row = _closed(receipt["row"], RECEIPT_ROW_FIELDS, "row")
    expected_row: dict[str, Any] = {
        "disc": key.disc,
        "build": key.build,
        "scenario": key.scenario,
        "producer": key.producer,
        "tier": key.tier,
        "requested_mode": key.requested_mode,
        "repetition": key.repetition,
        **contract.parity,
    }
    for field, expected in expected_row.items():
        actual = row[field]
        if isinstance(expected, bool):
            actual = _boolean(actual, field)
        elif isinstance(expected, int):
            actual = _integer(actual, field)
        else:
            actual = _string(actual, field)
        if actual != expected:
            _fail("ROW_MISMATCH")
    effective_mode = _choice(row["effective_mode"], MODES, "effective_mode")
    if key.requested_mode == "native" and effective_mode == "original":
        _fail("EFFECTIVE_ORIGINAL")
    if effective_mode != key.requested_mode:
        _fail("ROW_MISMATCH")

    identity = _closed(receipt["identity"], IDENTITY_FIELDS, "identity")
    image = contract.images[(key.disc, key.tier)]
    if _identifier(identity["producer"], "identity.producer") not in contract.producers:
        _fail("UNKNOWN_PRODUCER")
    if identity["producer"] != key.producer:
        _fail("ROW_MISMATCH")
    image_id = _identifier(identity["image_id"], "identity.image_id")
    if image_id != image["id"]:
        _fail("UNKNOWN_HASH")
    if _digest(identity["image_sha256"], "identity.image_sha256") != image["sha256"]:
        _fail("UNKNOWN_HASH")
    if _digest(identity["build_sha256"], "identity.build_sha256") != contract.builds[key.build]["sha256"]:
        _fail("UNKNOWN_HASH")
    if _integer(identity["base_address"], "identity.base_address", maximum=0xFFFFFFFF) != image["base_address"]:
        _fail("UNKNOWN_HASH")
    if _integer(identity["generation"], "identity.generation", maximum=0xFFFFFFFF) != image["generation"]:
        _fail("UNKNOWN_HASH")
    identity_branches = _identifier_array(identity["branches"], "identity.branches", nonempty=False)
    if any(branch not in contract.branches for branch in identity_branches):
        _fail("UNKNOWN_BRANCH")
    scenario = contract.scenarios[key.scenario]
    expected_branches = tuple(
        sorted(branch for branch in scenario["branches"] if contract.branches[branch]["producer"] == key.producer)
    )
    if tuple(sorted(identity_branches)) != expected_branches:
        _fail("ROW_MISMATCH")

    coverage = _closed(receipt["coverage"], COVERAGE_FIELDS, "coverage")
    expected_coverage = _expected_coverage(contract)
    parsed_coverage = {field: _integer(coverage[field], field) for field in COVERAGE_FIELDS}
    if parsed_coverage != expected_coverage:
        _fail("INVARIANT_MISMATCH")

    isolation = _closed(receipt["isolation"], ISOLATION_FIELDS, "isolation")
    parsed_isolation = {field: _integer(isolation[field], field) for field in ISOLATION_FIELDS}
    if any(parsed_isolation[field] for field in SEMANTIC_ISOLATION_FIELDS):
        _fail("INVARIANT_MISMATCH")
    selected_hit_field = TARGET_HIT_FIELDS[key.tier]
    tier_hits = {field: parsed_isolation[field] for field in TARGET_HIT_FIELDS.values()}
    if effective_mode == "native":
        if parsed_isolation["render_gte_exec_count"] != 0 or any(tier_hits.values()):
            _fail("INVARIANT_MISMATCH")
    else:
        if parsed_isolation["render_gte_exec_count"] == 0 or parsed_isolation[selected_hit_field] == 0:
            _fail("INVARIANT_MISMATCH")
        if any(value for field, value in tier_hits.items() if field != selected_hit_field):
            _fail("INVARIANT_MISMATCH")
    normalized_isolation = {
        "render_gte_exec_count": parsed_isolation["render_gte_exec_count"],
        "selected_tier_target_gte_site_hits": parsed_isolation[selected_hit_field],
        **{field: parsed_isolation[field] for field in SEMANTIC_ISOLATION_FIELDS},
    }

    parity = _closed(receipt["parity"], PARITY_RECEIPT_FIELDS, "parity")
    primitives = (
        _integer(parity["original_normalized_primitives"], "original_normalized_primitives"),
        _integer(parity["native_normalized_ir_primitives"], "native_normalized_ir_primitives"),
    )
    if primitives[0] != primitives[1]:
        _fail("INVARIANT_MISMATCH")
    digest_groups = (
        ("source_snapshot_digest", "source_poisoned_snapshot_digest"),
        ("model_animation_original_digest", "model_animation_shadow_digest", "model_animation_native_digest"),
        ("camera_matrix_light_original_digest", "camera_matrix_light_shadow_digest", "camera_matrix_light_native_digest"),
        ("culling_depth_original_digest", "culling_depth_shadow_digest", "culling_depth_native_digest"),
        ("ir_semantics_original_digest", "ir_semantics_shadow_digest", "ir_semantics_native_digest"),
        ("original_ordering_digest", "shadow_ordering_digest", "native_ordering_digest"),
        ("ps1_state_original_digest", "ps1_state_shadow_digest", "ps1_state_native_digest"),
        ("shadow_original_initial_vram_digest", "shadow_native_initial_vram_digest"),
        ("original_vram_digest", "shadow_vram_digest", "native_vram_digest"),
        ("original_display15_digest", "shadow_display15_digest", "native_display15_digest"),
        ("original_host_framebuffer_digest", "shadow_host_framebuffer_digest", "native_host_framebuffer_digest"),
        ("host_output_digest", "poisoned_host_output_digest"),
        ("semantic_ir_digest", "poisoned_semantic_ir_digest"),
    )
    for group in digest_groups:
        _equal_digests(parity, group)
    parsed_parity = {
        field: _integer(parity[field], field)
        if field in {"original_normalized_primitives", "native_normalized_ir_primitives"}
        else _digest(parity[field], field)
        for field in PARITY_RECEIPT_FIELDS
    }

    checks = _closed(receipt["checks"], CHECK_FIELDS, "checks")
    parsed_checks = {field: _boolean(checks[field], field) for field in CHECK_FIELDS}
    if not all(parsed_checks.values()):
        _fail("INVARIANT_MISMATCH")

    fallback = _closed(receipt["fallback"], FALLBACK_FIELDS, "fallback")
    qualified = _boolean(fallback["qualified_native"], "qualified_native")
    if qualified != (key.requested_mode == "native"):
        _fail("ROW_MISMATCH")
    fallback_counts = {field: _integer(fallback[field], field) for field in FALLBACK_FIELDS - {"qualified_native"}}
    if any(fallback_counts.values()):
        _fail("FALLBACK")

    _parse_cleanup(receipt["cleanup"])
    _parse_nonsemantic(receipt["nonsemantic"], contract.whitelist)
    deterministic = {
        "coverage": parsed_coverage,
        "isolation": normalized_isolation,
        "parity": parsed_parity,
        "checks": parsed_checks,
        "fallback_counts": fallback_counts,
    }
    return run_id, _canonical_json(deterministic), _canonical_json(parsed_parity)


def _aggregate_metrics(contract: Contract) -> dict[str, int]:
    coverage = _expected_coverage(contract)
    return {
        "row_count": len(contract.matrix),
        "target_3d_producers": coverage["target_3d_producers"],
        "migrated_3d_producers": coverage["migrated_3d_producers"],
        "exercised_3d_producers": coverage["exercised_3d_producers"],
        "target_3d_families": coverage["target_3d_families"],
        "migrated_3d_families": coverage["migrated_3d_families"],
        "exercised_3d_families": coverage["exercised_3d_families"],
        "target_reachable_render_branches": coverage["target_reachable_render_branches"],
        "migrated_render_branches": coverage["migrated_render_branches"],
        "exercised_render_branches": coverage["exercised_render_branches"],
        "unclassified_render_gte_sites": 0,
        "unclassified_reachable_render_branches": 0,
        "unattributed_original_3d_primitives": 0,
        "render_gte_exec_count": 0,
        "native_target_gte_site_hits": 0,
        "semantic_post_gte_reads": 0,
        "target_packet_payload_reads_by_semantic_lane": 0,
        "target_gp0_decode_to_semantic_calls": 0,
        "target_ot_payload_geometry_or_material_reads": 0,
        "native_qualified_fallback_count": 0,
        "mismatch_count": 0,
        "unknown_identity_count": 0,
        "cleanup_incomplete_count": 0,
        "privacy_violation_count": 0,
        "nondeterministic_group_count": 0,
    }


def _parse_aggregate_receipt(
    contract: Contract,
    kind: str,
    value: dict[str, Any],
    matrix_sha256: str,
) -> str:
    receipt = _closed(value, AGGREGATE_ROOT_FIELDS, "aggregate")
    if _string(receipt["schema"], "schema") != AGGREGATE_RECEIPT_SCHEMA:
        _fail("SCHEMA")
    run_id = _digest(receipt["run_id"], "run_id")
    if _choice(receipt["kind"], AGGREGATE_KINDS, "kind") != kind:
        _fail("ROW_MISMATCH")
    if _digest(receipt["matrix_sha256"], "matrix_sha256") != matrix_sha256:
        _fail("INVARIANT_MISMATCH")
    metrics = _closed(receipt["metrics"], AGGREGATE_METRIC_FIELDS, "metrics")
    parsed_metrics = {field: _integer(metrics[field], field) for field in AGGREGATE_METRIC_FIELDS}
    if parsed_metrics != _aggregate_metrics(contract):
        _fail("INVARIANT_MISMATCH")
    _parse_cleanup(receipt["cleanup"])
    _parse_nonsemantic(receipt["nonsemantic"], contract.whitelist)
    return run_id


def _canonical_json(value: Any) -> bytes:
    try:
        return json.dumps(value, ensure_ascii=True, allow_nan=False, sort_keys=True, separators=(",", ":")).encode("ascii") + b"\n"
    except (TypeError, ValueError) as error:
        raise CertificationError("SCHEMA") from error


def _set_digest(digests: Sequence[str]) -> str:
    return hashlib.sha256(("\n".join(sorted(digests)) + "\n").encode("ascii")).hexdigest()


def _publish_once(destination: Path, evidence: dict[str, Any]) -> None:
    _privacy_value(evidence)
    encoded = _canonical_json(evidence)
    parent = destination.parent
    if not parent.is_dir():
        _fail("PUBLICATION")
    try:
        descriptor, temporary_name = tempfile.mkstemp(dir=parent, prefix=f".{destination.name}.")
        temporary = Path(temporary_name)
        try:
            os.fchmod(descriptor, 0o600)
            with os.fdopen(descriptor, "wb") as output:
                descriptor = -1
                output.write(encoded)
                output.flush()
                os.fsync(output.fileno())
            try:
                os.link(temporary, destination)
            except FileExistsError as error:
                raise CertificationError("PUBLICATION") from error
            directory_descriptor = os.open(parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory_descriptor)
            finally:
                os.close(directory_descriptor)
        finally:
            if descriptor >= 0:
                os.close(descriptor)
            temporary.unlink(missing_ok=True)
    except CertificationError:
        raise
    except OSError as error:
        raise CertificationError("PUBLICATION") from error


def certify(certification: Path, renderer: str) -> dict[str, Any]:
    contract = _parse_contract(certification, renderer)
    document_evidence: list[dict[str, str]] = []
    for document in contract.documents:
        raw, digest = _verify_file(contract.root, document["path"], document["sha256"])
        parsed = _load_json(raw)
        if not isinstance(parsed.get("schema"), str):
            _fail("SCHEMA")
        document_evidence.append({"id": document["id"], "kind": document["kind"], "sha256": digest})

    matrix_digests: list[str] = []
    run_ids: set[str] = set()
    mode_signatures: dict[tuple[str, str, str, str], bytes] = {}
    parity_signatures: dict[tuple[str, str, str], bytes] = {}
    for key in sorted(
        contract.matrix,
        key=lambda item: (
            item.disc,
            item.scenario,
            item.producer,
            item.build,
            item.tier,
            item.requested_mode,
            item.repetition,
        ),
    ):
        reference = contract.matrix[key]
        raw, digest = _verify_file(contract.root, reference["receipt"], reference["sha256"])
        receipt = _load_json(raw)
        run_id, mode_signature, parity_signature = _parse_matrix_receipt(contract, key, receipt)
        if run_id in run_ids:
            _fail("NONDETERMINISTIC")
        run_ids.add(run_id)
        matrix_digests.append(digest)
        mode_group = (key.disc, key.scenario, key.producer, key.requested_mode)
        previous_mode = mode_signatures.setdefault(mode_group, mode_signature)
        if previous_mode != mode_signature:
            _fail("NONDETERMINISTIC")
        parity_group = (key.disc, key.scenario, key.producer)
        previous_parity = parity_signatures.setdefault(parity_group, parity_signature)
        if previous_parity != parity_signature:
            _fail("NONDETERMINISTIC")

    matrix_sha256 = _set_digest(matrix_digests)
    aggregate_evidence: list[dict[str, str]] = []
    aggregate_digests: list[str] = []
    for kind in sorted(contract.aggregates):
        reference = contract.aggregates[kind]
        raw, digest = _verify_file(contract.root, reference["receipt"], reference["sha256"])
        receipt = _load_json(raw)
        run_id = _parse_aggregate_receipt(contract, kind, receipt, matrix_sha256)
        if run_id in run_ids:
            _fail("NONDETERMINISTIC")
        run_ids.add(run_id)
        aggregate_digests.append(digest)
        aggregate_evidence.append({"kind": kind, "sha256": digest})

    return {
        "schema": EVIDENCE_SCHEMA,
        "verdict": "PASS",
        "certification_id": contract.certification_id,
        "certification_sha256": contract.source_sha256,
        "renderer": renderer,
        "parity": {
            "present": "canonical",
            "color_depth_bits": 15,
            "interpolation": False,
            "smooth": False,
            "wide": False,
            "hires": False,
        },
        "coverage": {
            "discs": len(contract.discs),
            "hosts": len(contract.hosts),
            "toolchains": len(contract.toolchains),
            "builds": len(contract.builds),
            "families": len(contract.families),
            "producers": len(contract.producers),
            "reachable_branches": sum(1 for branch in contract.branches.values() if branch["reachable"]),
            "scenarios": len(contract.scenarios),
            "tiers": len(TIERS),
            "modes": len(MODES),
            "repetitions": contract.repetitions,
            "matrix_rows": len(contract.matrix),
        },
        "invariants": _aggregate_metrics(contract),
        "nonsemantic_whitelist": list(contract.whitelist),
        "documents": sorted(document_evidence, key=lambda item: item["id"]),
        "matrix_receipts_sha256": matrix_sha256,
        "aggregate_receipts_sha256": _set_digest(aggregate_digests),
        "aggregate_receipts": aggregate_evidence,
        "entry_point": {"exit_code": 0, "stdout": PASS_VERDICT},
    }


def _arguments(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(add_help=True, exit_on_error=False)
    parser.add_argument("--certification", required=True)
    parser.add_argument("--renderer", required=True)
    parser.add_argument("--evidence", required=True)
    try:
        options, unknown = parser.parse_known_args(argv)
    except argparse.ArgumentError as error:
        raise CertificationError("ARGUMENT") from error
    if unknown:
        _fail("ARGUMENT")
    return options


def main(argv: Sequence[str]) -> int:
    try:
        options = _arguments(argv)
        evidence = certify(Path(options.certification), _string(options.renderer, "renderer"))
        _publish_once(Path(options.evidence), evidence)
    except CertificationError as error:
        sys.stderr.write(f"NATIVE_RENDER_3D_CERTIFICATION_REJECT {error.code}\n")
        return 2
    sys.stdout.write(f"{PASS_VERDICT}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
