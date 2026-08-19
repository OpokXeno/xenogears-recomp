from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from typing import Final, NoReturn, TypeIs

from native_render_field_character_shadow_schema import (
    FIELD_CHARACTER_SHADOW_SCHEMA,
    FieldCharacterShadow,
)
from native_render_schema import ContractError, JsonObject, JsonValue


PRODUCER_FAMILY_RUNTIME_SCHEMA: Final = "xenogears.field-character-candidate/v1"
PRODUCER_FAMILY_EVIDENCE_SCHEMA: Final = "xenogears.field-character-candidate-evidence/v1"


@dataclass(frozen=True, slots=True)
class ProducerFamilySpec:
    """Packet-shape contract for a reusable producer family.

    Families are classified by the semantic packet contract, not by a scene
    label such as terrain or model. A family may be used by multiple game
    systems and producer entry points.
    """

    name: str
    packet_class: str
    opcode: int
    length_words: int
    minimum_matches: int


PRODUCER_FAMILY_SPECS: Final = (
    ProducerFamilySpec("poly-ft4-semitrans", "fixed-textured", 0x2E, 9, 1000),
)
PRODUCER_FAMILY_NAME: Final = PRODUCER_FAMILY_SPECS[0].name
PRODUCER_FAMILY_OPCODE: Final = PRODUCER_FAMILY_SPECS[0].opcode
PRODUCER_FAMILY_WORDS: Final = PRODUCER_FAMILY_SPECS[0].length_words
MINIMUM_NATURAL_MATCHES: Final = PRODUCER_FAMILY_SPECS[0].minimum_matches
_U32: Final = 0xFFFFFFFF
_U64: Final = 0xFFFFFFFFFFFFFFFF
_RUNTIME_KEYS: Final = frozenset({
    "schema", "family", "opcode", "length_words", "enabled", "blocked",
    "geometry_count", "candidate_count", "match_count", "mismatch_count",
    "last_ot_bucket", "last_runtime_result", "last_compare_result",
    "first_mismatch_word", "first_mismatch_byte", "blocker", "diagnostic",
    "privacy",
})
_DIAGNOSTIC_KEYS: Final = frozenset({
    "source_event_count", "source_blocker", "source_context_bits",
    "collector_phase", "collector_blocker", "collector_access_count",
    "collector_site_count",
    "source_blocked", "source_overflowed", "geometry_completed_count",
    "geometry_queued_count", "geometry_pending", "geometry_blocked",
    "geometry_overflowed",
})
_EVIDENCE_KEYS: Final = frozenset({
    "schema", "schema_version", "task", "status", "scenario", "timing_mode",
    "render_mode", "authentication", "family", "comparison", "privacy",
    "cleanup",
})


def _fail(code: str) -> NoReturn:
    raise ContractError(code)


def _is_record(value: JsonValue) -> TypeIs[JsonObject]:
    return isinstance(value, Mapping)


def _is_integer(value: JsonValue) -> TypeIs[int]:
    return isinstance(value, int) and not isinstance(value, bool)


def _record(value: JsonValue, label: str) -> JsonObject:
    if not _is_record(value):
        _fail(f"{label}_object_required")
    return value


def _keys(value: JsonObject, expected: frozenset[str], label: str) -> None:
    if set(value) != set(expected):
        _fail(f"{label}_keys_invalid")


def _integer(value: JsonValue, label: str, maximum: int = _U64) -> int:
    if not _is_integer(value) or value < 0 or value > maximum:
        _fail(f"{label}_integer_invalid")
    return value


def _boolean(value: JsonValue, label: str) -> bool:
    if not isinstance(value, bool):
        _fail(f"{label}_boolean_invalid")
    return value


def producer_family_spec(name: object) -> ProducerFamilySpec | None:
    if not isinstance(name, str):
        return None
    return next((spec for spec in PRODUCER_FAMILY_SPECS if spec.name == name), None)


@dataclass(frozen=True, slots=True)
class ProducerFamilyRuntime:
    geometry_count: int
    candidate_count: int
    match_count: int
    mismatch_count: int


def parse_producer_family_runtime(value: JsonValue) -> ProducerFamilyRuntime:
    record = _record(value, "producer_family_runtime")
    _keys(record, _RUNTIME_KEYS, "producer_family_runtime")
    spec = producer_family_spec(record["family"])
    if (
        record["schema"] != PRODUCER_FAMILY_RUNTIME_SCHEMA
        or spec is None
        or record["opcode"] != spec.opcode
        or record["length_words"] != spec.length_words
    ):
        _fail("producer_family_runtime_identity_invalid")
    if not _boolean(record["enabled"], "producer_family_enabled"):
        _fail("producer_family_runtime_disabled")
    if _boolean(record["blocked"], "producer_family_blocked"):
        _fail("producer_family_runtime_blocked")

    geometry_count = _integer(record["geometry_count"], "geometry_count")
    candidate_count = _integer(record["candidate_count"], "candidate_count")
    match_count = _integer(record["match_count"], "match_count")
    mismatch_count = _integer(record["mismatch_count"], "mismatch_count")
    for name in ("last_ot_bucket", "last_runtime_result", "last_compare_result",
                 "first_mismatch_word", "first_mismatch_byte", "blocker"):
        _integer(record[name], name, _U32)
    if (
        geometry_count < spec.minimum_matches
        or geometry_count != candidate_count
        or candidate_count != match_count
        or mismatch_count != 0
        or record["last_runtime_result"] != 0
        or record["last_compare_result"] != 0
        or record["first_mismatch_word"] != _U32
        or record["first_mismatch_byte"] != _U32
        or record["blocker"] != 0
    ):
        _fail("producer_family_runtime_comparison_invalid")

    diagnostic = _record(record["diagnostic"], "producer_family_diagnostic")
    _keys(diagnostic, _DIAGNOSTIC_KEYS, "producer_family_diagnostic")
    source_event_count = _integer(diagnostic["source_event_count"], "source_event_count", 28)
    source_blocker = _integer(diagnostic["source_blocker"], "source_blocker", _U32)
    source_context_bits = _integer(diagnostic["source_context_bits"], "source_context_bits", 0x1FF)
    collector_phase = _integer(diagnostic["collector_phase"], "collector_phase", 3)
    collector_blocker = _integer(diagnostic["collector_blocker"], "collector_blocker", 18)
    _integer(diagnostic["collector_access_count"], "collector_access_count", 256)
    _integer(diagnostic["collector_site_count"], "collector_site_count", 16)
    _boolean(diagnostic["source_overflowed"], "source_overflowed")
    completed_count = _integer(diagnostic["geometry_completed_count"], "geometry_completed_count")
    queued_count = _integer(diagnostic["geometry_queued_count"], "geometry_queued_count", 256)
    if (
        source_event_count == 0
        or source_blocker != 0
        or collector_phase != 1
        or collector_blocker != 0
        or source_context_bits & 0x1E1 != 0x1E1
        or _boolean(diagnostic["source_blocked"], "source_blocked")
        or completed_count != geometry_count
        or queued_count != 0
        or _boolean(diagnostic["geometry_pending"], "geometry_pending")
        or _boolean(diagnostic["geometry_blocked"], "geometry_blocked")
        or _boolean(diagnostic["geometry_overflowed"], "geometry_overflowed")
    ):
        _fail("producer_family_runtime_diagnostic_invalid")
    privacy = _record(record["privacy"], "producer_family_runtime_privacy")
    _keys(privacy, frozenset({"metadata_only", "packet_words", "guest_paths"}),
          "producer_family_runtime_privacy")
    if privacy != {"metadata_only": True, "packet_words": False, "guest_paths": False}:
        _fail("producer_family_runtime_privacy_invalid")
    return ProducerFamilyRuntime(geometry_count, candidate_count, match_count,
                                 mismatch_count)


def build_producer_family_evidence(
    metadata: FieldCharacterShadow,
    runtime_value: JsonValue,
) -> dict[str, object]:
    runtime = parse_producer_family_runtime(runtime_value)
    runtime_record = _record(runtime_value, "producer_family_runtime")
    spec = producer_family_spec(runtime_record["family"])
    if (
        spec is None or
        metadata.phase != "complete"
        or metadata.blocker != "none"
        or not metadata.has_selection
        or metadata.opcode != spec.opcode
        or metadata.parser_word_count != spec.length_words
        or metadata.selection_count < spec.minimum_matches
    ):
        _fail("producer_family_metadata_invalid")
    evidence: dict[str, object] = {
        "schema": PRODUCER_FAMILY_EVIDENCE_SCHEMA,
        "schema_version": 1,
        "task": 12,
        "status": "PASS",
        "scenario": "game-producer",
        "timing_mode": "original",
        "render_mode": "shadow",
        "authentication": {
            "metadata_schema": FIELD_CHARACTER_SHADOW_SCHEMA,
            "metadata_status": "pass",
            "source_site": metadata.site,
            "producer_store_pc": metadata.producer_store_pc,
        },
        "family": {
            "name": spec.name,
            "packet_class": spec.packet_class,
            "opcode": spec.opcode,
            "length_words": spec.length_words,
        },
        "comparison": {
            "geometry_count": runtime.geometry_count,
            "candidate_count": runtime.candidate_count,
            "match_count": runtime.match_count,
            "mismatch_count": runtime.mismatch_count,
            "minimum_match_count": spec.minimum_matches,
            "coverage_complete": True,
            "consumed_bits_exact": True,
            "ignored_padding_upper_halfwords": [6, 8],
        },
        "privacy": {
            "classification": "metadata-only",
            "raw_inputs": False,
            "packet_words": False,
            "private_paths": False,
        },
        "cleanup": {"runtime_state_removed": True, "process_reaped": True},
    }
    parse_producer_family_evidence(evidence)
    return evidence


def parse_producer_family_evidence(value: JsonValue) -> JsonObject:
    record = _record(value, "producer_family_evidence")
    _keys(record, _EVIDENCE_KEYS, "producer_family_evidence")
    if (
        record["schema"] != PRODUCER_FAMILY_EVIDENCE_SCHEMA
        or record["schema_version"] != 1
        or record["task"] != 12
        or record["status"] != "PASS"
        or record["scenario"] != "game-producer"
        or record["timing_mode"] != "original"
        or record["render_mode"] != "shadow"
    ):
        _fail("producer_family_evidence_identity_invalid")
    authentication = _record(record["authentication"], "producer_family_authentication")
    _keys(authentication, frozenset({"metadata_schema", "metadata_status", "source_site", "producer_store_pc"}), "producer_family_authentication")
    if authentication["metadata_schema"] != FIELD_CHARACTER_SHADOW_SCHEMA or authentication["metadata_status"] != "pass":
        _fail("producer_family_authentication_invalid")
    _integer(authentication["source_site"], "source_site", _U32)
    _integer(authentication["producer_store_pc"], "producer_store_pc", _U32)
    family = _record(record["family"], "producer_family")
    _keys(family, frozenset({"name", "packet_class", "opcode", "length_words"}), "producer_family")
    spec = producer_family_spec(family["name"])
    if (
        spec is None
        or family != {
            "name": spec.name,
            "packet_class": spec.packet_class,
            "opcode": spec.opcode,
            "length_words": spec.length_words,
        }
    ):
        _fail("producer_family_identity_invalid")
    comparison = _record(record["comparison"], "producer_family_comparison")
    _keys(comparison, frozenset({"geometry_count", "candidate_count", "match_count", "mismatch_count", "minimum_match_count", "coverage_complete", "consumed_bits_exact", "ignored_padding_upper_halfwords"}), "producer_family_comparison")
    geometry_count = _integer(comparison["geometry_count"], "geometry_count")
    candidate_count = _integer(comparison["candidate_count"], "candidate_count")
    match_count = _integer(comparison["match_count"], "match_count")
    mismatch_count = _integer(comparison["mismatch_count"], "mismatch_count")
    if (
        comparison["minimum_match_count"] != spec.minimum_matches
        or geometry_count < spec.minimum_matches
        or geometry_count != candidate_count
        or candidate_count != match_count
        or mismatch_count != 0
        or comparison["coverage_complete"] is not True
        or comparison["consumed_bits_exact"] is not True
        or comparison["ignored_padding_upper_halfwords"] != [6, 8]
    ):
        _fail("producer_family_comparison_invalid")
    privacy = _record(record["privacy"], "producer_family_privacy")
    _keys(privacy, frozenset({"classification", "raw_inputs", "packet_words", "private_paths"}), "producer_family_privacy")
    if privacy != {"classification": "metadata-only", "raw_inputs": False, "packet_words": False, "private_paths": False}:
        _fail("producer_family_privacy_invalid")
    cleanup = _record(record["cleanup"], "producer_family_cleanup")
    _keys(cleanup, frozenset({"runtime_state_removed", "process_reaped"}), "producer_family_cleanup")
    if cleanup != {"runtime_state_removed": True, "process_reaped": True}:
        _fail("producer_family_cleanup_invalid")
    return record
