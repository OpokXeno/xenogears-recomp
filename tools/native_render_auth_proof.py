from __future__ import annotations

from typing import Final


type JsonScalar = str | int | float | bool | None
type JsonValue = JsonScalar | list[JsonValue] | dict[str, JsonValue]
type JsonObject = dict[str, JsonValue]
type StaticProvenance = tuple[str, str, int, int, int, bool, bool]
type NormalizedObservation = tuple[
    StaticProvenance, tuple[int, int, int, int], int, bool, int, int, bool, int,
]


class AuthProofError(ValueError):
    def __init__(self, reason: str) -> None:
        super().__init__(reason)
        self.reason = reason

    def __str__(self) -> str:
        return self.reason


AUTH_PROOF_SCHEMA: Final = "xenogears.native-render-auth-proof/v4"
AUTH_PROOF_MATRIX_SCHEMA: Final = "xenogears.native-render-auth-proof-matrix/v1"
AUTH_PROOF_PRIVACY: Final = {
    "metadata_only": True,
    "raw_instruction_words": False,
    "raw_delay_slot_words": False,
    "identities_or_digests": False,
    "private_paths": False,
    "disc_cards_cache_hashes": False,
    "input_states": False,
    "packets": False,
    "child_runtime_json": False,
}
_TUPLE_FIELDS: Final = ("producer_entry", "capture_site", "static_callee", "return_site")
_MANIFEST_TUPLE: Final = (0x80075B44, 0x800781BC, 0x8004B54C, 0x800781C4)
_STATIC_PROVENANCE_FIELDS: Final = {
    "source", "image", "producer_entry", "range_start", "range_size", "manifest_bound",
    "range_bound", "candidate",
}
_STATIC_CANDIDATE_FIELDS: Final = {"matched", "dispatched"}
_RUNTIME_DIAGNOSTIC_FIELDS: Final = {
    "available", "producer_begin_count", "hook_count", "trace_event_count",
    "trace_overflowed", "accepted_entry", "accepted_capture", "accepted_return",
    "rejected_event_count", "reset_since_trace_start", "scene_aborted", "reject_reason",
    "rejection_source", "rejection_hook", "rejection_guest_pc", "instrumentation",
}
_RUNTIME_INSTRUMENTATION_FIELDS: Final = {
    "revision", "cold_hook_ingress_count", "activation_physical_count",
    "activation_exact_count", "entry_physical_count", "entry_exact_count",
    "capture_physical_count", "capture_exact_count", "return_physical_count",
    "return_exact_count", "last_progress_sequence", "last_reset_sequence",
    "last_publish_sequence", "scene_boundary_count", "disarm_count",
    "completed_proof_publication_count", "native_ir_flush_attempt_count",
    "native_ir_flush_failure_count", "first_native_ir_flush_failure_index",
    "first_native_ir_flush_failure_reason", "first_native_ir_flush_failure_packet",
    "first_native_ir_flush_failure_status", "counter_overflow_events",
    "counters_poisoned",
}
_RUNTIME_TRACE_CAPACITY: Final = 64
_STATIC_PROVENANCE: Final = (
    "manifest-overlay", "field-image", 0x80075B44, 0x8006F000, 282624,
)
_REJECT_REASONS: Final = {
    "none", "identity_mismatch", "validation_mismatch", "cache_identity_mismatch",
    "hook_sequence", "code_page_mutation", "foreign_interior",
    "transaction_failure", "unavailable",
}
_REJECTION_SOURCES: Final = {
    "none", "runtime_hook", "variant_hook", "loader_mismatch", "native_bad_entry",
    "code_page_mutation",
}
_REJECTION_HOOKS: Final = {"none", "entry", "capture", "return", "foreign_interior"}
_HOOK_REJECTION_SOURCES: Final = {"runtime_hook", "variant_hook"}
_MATRIX_ROWS: Final = (("debug", "cold"), ("debug", "warm"),
                       ("release", "cold"), ("release", "warm"))


def _mapping(value: JsonValue, message: str) -> JsonObject:
    if not isinstance(value, dict):
        raise AuthProofError(message)
    return value


def _integer(value: JsonValue, message: str, minimum: int = 0, maximum: int = 0xffffffffffffffff) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or not minimum <= value <= maximum:
        raise AuthProofError(message)
    return value


def _proof_tuple(value: JsonValue, message: str) -> tuple[int, int, int, int]:
    proof_tuple = _mapping(value, message)
    if set(proof_tuple) != set(_TUPLE_FIELDS):
        raise AuthProofError("auth proof tuple is not closed")
    candidate = tuple(_integer(proof_tuple[field], "auth proof tuple is invalid", 1, 0xffffffff)
                      for field in _TUPLE_FIELDS)
    if candidate != _MANIFEST_TUPLE:
        raise AuthProofError("auth proof tuple does not match the manifest")
    return candidate


def _static_provenance(value: JsonValue) -> tuple[StaticProvenance, tuple[bool, bool]]:
    provenance = _mapping(value, "auth proof static provenance is missing")
    if set(provenance) != _STATIC_PROVENANCE_FIELDS:
        raise AuthProofError("auth proof static provenance is not closed")
    candidate = _mapping(provenance["candidate"], "auth proof static candidate is missing")
    if set(candidate) != _STATIC_CANDIDATE_FIELDS:
        raise AuthProofError("auth proof static candidate is not closed")
    matched = candidate["matched"]
    dispatched = candidate["dispatched"]
    if not isinstance(matched, bool) or not isinstance(dispatched, bool):
        raise AuthProofError("auth proof static candidate is invalid")
    if dispatched and not matched:
        raise AuthProofError("auth proof static candidate is invalid")
    source, image, producer_entry, range_start, range_size = _STATIC_PROVENANCE
    if (
        provenance["source"] != source or provenance["image"] != image or
        _integer(provenance["producer_entry"], "auth proof static provenance is invalid", 1, 0xffffffff) != producer_entry or
        _integer(provenance["range_start"], "auth proof static provenance is invalid", 1, 0xffffffff) != range_start or
        _integer(provenance["range_size"], "auth proof static provenance is invalid", 1, 0xffffffff) != range_size or
        not isinstance(provenance["manifest_bound"], bool) or not isinstance(provenance["range_bound"], bool)
    ):
        raise AuthProofError("auth proof static provenance is invalid")
    return (
        (source, image, producer_entry, range_start, range_size,
         provenance["manifest_bound"], provenance["range_bound"]),
        (matched, dispatched),
    )


def _trace(value: JsonValue, runtime: bool) -> None:
    trace = _mapping(value, "auth proof trace is missing")
    expected = {"entry_sequence", "capture_sequence", "return_sequence"}
    if runtime:
        expected |= {"scene_epoch", "state_sequence"}
    if set(trace) != expected:
        raise AuthProofError("auth proof trace is not closed")
    entry = _integer(trace["entry_sequence"], "auth proof trace sequence is invalid", 1)
    capture = _integer(trace["capture_sequence"], "auth proof trace sequence is invalid", 1)
    returned = _integer(trace["return_sequence"], "auth proof trace sequence is invalid", 1)
    if capture != entry + 1 or returned != capture + 1:
        raise AuthProofError("auth proof trace sequence is invalid")
    if runtime:
        _integer(trace["scene_epoch"], "auth proof trace state is invalid", 1)
        _integer(trace["state_sequence"], "auth proof trace state is invalid")


def _field_binding(value: JsonValue) -> bool:
    binding = _mapping(value, "auth proof field binding is missing")
    fields = {
        "checkpoint_field_id", "checkpoint_seen", "checkpoint_seen_vblank", "evidence_vblank",
        "context_valid", "context_field_id",
    }
    if set(binding) != fields:
        raise AuthProofError("auth proof field binding is not closed")
    checkpoint_field_id = _integer(binding["checkpoint_field_id"], "auth proof field binding is invalid", 0, 0xffff)
    checkpoint_seen_vblank = _integer(binding["checkpoint_seen_vblank"], "auth proof field binding is invalid")
    evidence_vblank = _integer(binding["evidence_vblank"], "auth proof field binding is invalid")
    context_field_id = _integer(binding["context_field_id"], "auth proof field binding is invalid", 0, 0xffff)
    if not isinstance(binding["checkpoint_seen"], bool) or not isinstance(binding["context_valid"], bool):
        raise AuthProofError("auth proof field binding is invalid")
    if checkpoint_seen_vblank > evidence_vblank:
        raise AuthProofError("auth proof field binding is invalid")
    return (
        checkpoint_field_id > 0 and binding["checkpoint_seen"] is True and
        checkpoint_seen_vblank >= 1 and evidence_vblank >= checkpoint_seen_vblank and
        binding["context_valid"] is True and
        context_field_id == checkpoint_field_id
    )


def _runtime_diagnostic(value: JsonValue) -> JsonObject:
    diagnostic = _mapping(value, "auth proof runtime diagnostic is missing")
    if set(diagnostic) != _RUNTIME_DIAGNOSTIC_FIELDS:
        raise AuthProofError("auth proof runtime diagnostic is not closed")
    for field in (
        "available", "trace_overflowed", "accepted_entry", "accepted_capture",
        "accepted_return", "reset_since_trace_start", "scene_aborted",
    ):
        if not isinstance(diagnostic[field], bool):
            raise AuthProofError("auth proof runtime diagnostic is invalid")
    producer_begin_count = _integer(
        diagnostic["producer_begin_count"], "auth proof runtime diagnostic is invalid", 0, 1,
    )
    hook_count = _integer(
        diagnostic["hook_count"], "auth proof runtime diagnostic is invalid", 0, 3,
    )
    trace_event_count = _integer(
        diagnostic["trace_event_count"], "auth proof runtime diagnostic is invalid", 0,
        _RUNTIME_TRACE_CAPACITY,
    )
    rejected_event_count = _integer(
        diagnostic["rejected_event_count"], "auth proof runtime diagnostic is invalid", 0,
        trace_event_count,
    )
    accepted_event_count = sum(
        diagnostic[field] is True
        for field in ("accepted_entry", "accepted_capture", "accepted_return")
    )
    if accepted_event_count + rejected_event_count > trace_event_count:
        raise AuthProofError("auth proof runtime diagnostic is invalid")
    if diagnostic["trace_overflowed"] is True and trace_event_count != _RUNTIME_TRACE_CAPACITY:
        raise AuthProofError("auth proof runtime diagnostic is invalid")
    if diagnostic["scene_aborted"] is True and diagnostic["reject_reason"] in {"none", "unavailable"}:
        raise AuthProofError("auth proof runtime diagnostic is invalid")
    if diagnostic["reject_reason"] not in _REJECT_REASONS:
        raise AuthProofError("auth proof runtime diagnostic is invalid")
    rejection_source = diagnostic["rejection_source"]
    rejection_hook = diagnostic["rejection_hook"]
    rejection_guest_pc = _integer(
        diagnostic["rejection_guest_pc"], "auth proof runtime diagnostic is invalid", 0, 0xffffffff,
    )
    if rejection_source not in _REJECTION_SOURCES or rejection_hook not in _REJECTION_HOOKS:
        raise AuthProofError("auth proof runtime diagnostic is invalid")
    if rejection_source == "none":
        if rejection_hook != "none" or rejection_guest_pc != 0:
            raise AuthProofError("auth proof runtime diagnostic is invalid")
    elif (
        rejection_guest_pc == 0 or
        (rejection_source in _HOOK_REJECTION_SOURCES) != (rejection_hook != "none") or
        diagnostic["reject_reason"] == "none"
    ):
        raise AuthProofError("auth proof runtime diagnostic is invalid")
    if diagnostic["available"] is False and (
        producer_begin_count != 0 or hook_count != 0 or trace_event_count != 0 or
        diagnostic["trace_overflowed"] is True or accepted_event_count != 0 or
        rejected_event_count != 0 or diagnostic["reset_since_trace_start"] is True or
        diagnostic["scene_aborted"] is True or diagnostic["reject_reason"] != "unavailable"
    ):
        raise AuthProofError("auth proof runtime diagnostic is invalid")
    instrumentation = _mapping(
        diagnostic["instrumentation"], "auth proof instrumentation is missing",
    )
    if set(instrumentation) != _RUNTIME_INSTRUMENTATION_FIELDS:
        raise AuthProofError("auth proof instrumentation is not closed")
    if _integer(instrumentation["revision"], "auth proof instrumentation is invalid", 2, 2) != 2:
        raise AuthProofError("auth proof instrumentation is invalid")
    if (
        _integer(
            instrumentation["counter_overflow_events"],
            "auth proof instrumentation is invalid",
        ) != 0 or
        instrumentation["counters_poisoned"] is not False
    ):
        raise AuthProofError("auth proof instrumentation is invalid")
    for physical, exact in (
        ("activation_physical_count", "activation_exact_count"),
        ("entry_physical_count", "entry_exact_count"),
        ("capture_physical_count", "capture_exact_count"),
        ("return_physical_count", "return_exact_count"),
    ):
        if _integer(instrumentation[exact], "auth proof instrumentation is invalid") > _integer(
            instrumentation[physical], "auth proof instrumentation is invalid",
        ):
            raise AuthProofError("auth proof instrumentation is invalid")
    for field in (
        "cold_hook_ingress_count", "last_progress_sequence", "last_reset_sequence",
        "last_publish_sequence", "scene_boundary_count", "disarm_count",
        "completed_proof_publication_count",
        "native_ir_flush_attempt_count", "native_ir_flush_failure_count",
        "first_native_ir_flush_failure_index", "first_native_ir_flush_failure_reason",
        "first_native_ir_flush_failure_packet", "first_native_ir_flush_failure_status",
    ):
        _integer(instrumentation[field], "auth proof instrumentation is invalid")
    flush_attempts = _integer(
        instrumentation["native_ir_flush_attempt_count"],
        "auth proof instrumentation is invalid",
    )
    flush_failures = _integer(
        instrumentation["native_ir_flush_failure_count"],
        "auth proof instrumentation is invalid",
    )
    if flush_failures > flush_attempts:
        raise AuthProofError("auth proof instrumentation is invalid")
    failure_reason = _integer(
        instrumentation["first_native_ir_flush_failure_reason"],
        "auth proof instrumentation is invalid", 0, 7,
    )
    _integer(instrumentation["first_native_ir_flush_failure_packet"],
             "auth proof instrumentation is invalid", 0, 0xffffffff)
    _integer(instrumentation["first_native_ir_flush_failure_status"],
             "auth proof instrumentation is invalid", 0, 0xffffffff)
    if flush_failures == 0 and failure_reason != 0:
        raise AuthProofError("auth proof instrumentation is invalid")
    publication_count = _integer(
        instrumentation["completed_proof_publication_count"],
        "auth proof instrumentation is invalid",
    )
    publish_sequence = _integer(
        instrumentation["last_publish_sequence"], "auth proof instrumentation is invalid",
    )
    progress_sequence = _integer(
        instrumentation["last_progress_sequence"], "auth proof instrumentation is invalid",
    )
    reset_sequence = _integer(
        instrumentation["last_reset_sequence"], "auth proof instrumentation is invalid",
    )
    if (publication_count == 0) != (publish_sequence == 0) or (
        publish_sequence != 0 and reset_sequence <= progress_sequence and
        publish_sequence <= progress_sequence
    ):
        raise AuthProofError("auth proof instrumentation is invalid")
    return diagnostic


def assert_auth_proof(proof: JsonObject, expected_tier: str | None = None) -> None:
    if expected_tier not in (None, "cold", "warm"):
        raise AuthProofError("auth proof expected tier is invalid")
    if set(proof) != {"schema", "status", "privacy", "static", "runtime", "field_binding"}:
        raise AuthProofError("auth proof schema is not closed")
    if proof["schema"] != AUTH_PROOF_SCHEMA:
        raise AuthProofError("auth proof schema is invalid")
    if proof["privacy"] != AUTH_PROOF_PRIVACY:
        raise AuthProofError("auth proof privacy policy is invalid")
    static = _mapping(proof["static"], "auth proof static snapshot is missing")
    if set(static) != {"accepted", "provenance"}:
        raise AuthProofError("auth proof static snapshot is not closed")
    if not isinstance(static["accepted"], bool):
        raise AuthProofError("auth proof static acceptance is invalid")
    static_accepted = static["accepted"]
    static_provenance, candidate = _static_provenance(static["provenance"])
    if static_accepted != (static_provenance[-2] and static_provenance[-1]):
        raise AuthProofError("auth proof static acceptance is invalid")
    runtime = _mapping(proof["runtime"], "auth proof runtime snapshot is missing")
    required_runtime_fields = {
        "accepted", "tier", "reject_reason", "scene_aborted", "ir_usable", "native_permitted",
        "diagnostic",
    }
    if not isinstance(runtime.get("accepted"), bool):
        raise AuthProofError("auth proof runtime acceptance is invalid")
    runtime_accepted = runtime["accepted"]
    if set(runtime) != (required_runtime_fields | ({"tuple", "trace"} if runtime_accepted else set())):
        raise AuthProofError("auth proof runtime snapshot is not closed")
    tier = runtime["tier"]
    if tier not in ("none", "cold", "warm"):
        raise AuthProofError("auth proof runtime tier is invalid")
    if runtime["reject_reason"] not in _REJECT_REASONS:
        raise AuthProofError("auth proof runtime reject reason is invalid")
    for field in ("scene_aborted", "ir_usable", "native_permitted"):
        if not isinstance(runtime[field], bool):
            raise AuthProofError("auth proof runtime flags are invalid")
    diagnostic = _runtime_diagnostic(runtime["diagnostic"])
    if (
        diagnostic["scene_aborted"] != runtime["scene_aborted"] or
        diagnostic["reject_reason"] != runtime["reject_reason"]
    ):
        raise AuthProofError("auth proof runtime diagnostic is invalid")
    if runtime_accepted and not (
        diagnostic["available"] is True and diagnostic["producer_begin_count"] == 1 and
        diagnostic["hook_count"] == 3 and diagnostic["trace_event_count"] >= 3 and
        diagnostic["accepted_entry"] is True and diagnostic["accepted_capture"] is True and
        diagnostic["accepted_return"] is True
    ):
        raise AuthProofError("auth proof runtime diagnostic is invalid")
    if diagnostic["available"] is False and (
        runtime_accepted or tier != "none" or runtime["reject_reason"] != "unavailable" or
        runtime["scene_aborted"] is True or runtime["ir_usable"] is True or
        runtime["native_permitted"] is True
    ):
        raise AuthProofError("auth proof runtime diagnostic is invalid")
    if runtime_accepted:
        _proof_tuple(runtime["tuple"], "auth proof runtime tuple is missing")
        _trace(runtime["trace"], True)
    binding_observed = _field_binding(proof["field_binding"])
    status = proof["status"]
    if status not in ("OBSERVED", "BLOCKED"):
        raise AuthProofError("auth proof status is invalid")
    if status == "OBSERVED" and not binding_observed:
        raise AuthProofError("auth proof field binding does not prove the configured checkpoint")
    if expected_tier is not None and status == "OBSERVED" and tier != expected_tier:
        raise AuthProofError("auth proof runtime tier does not match matrix row")
    if status == "OBSERVED" and tier == "cold" and candidate != (False, False):
        raise AuthProofError("auth proof static candidate is invalid")
    if status == "OBSERVED" and tier == "warm" and candidate != (True, True):
        raise AuthProofError("auth proof candidate is unavailable")
    observed = (
        static_accepted and runtime_accepted and tier in ("cold", "warm") and
        runtime["reject_reason"] == "none" and runtime["scene_aborted"] is False and
        runtime["ir_usable"] is True and runtime["native_permitted"] is True and binding_observed and
        ((tier == "cold" and candidate == (False, False)) or (tier == "warm" and candidate == (True, True)))
    )
    if (status == "OBSERVED") != observed:
        raise AuthProofError("auth proof observation is invalid")


def _normalized_observation(proof: JsonObject) -> NormalizedObservation:
    static = _mapping(proof["static"], "auth proof static snapshot is missing")
    runtime = _mapping(proof["runtime"], "auth proof runtime snapshot is missing")
    binding = _mapping(proof["field_binding"], "auth proof field binding is missing")
    static_provenance, _candidate = _static_provenance(static["provenance"])
    return (
        static_provenance,
        _proof_tuple(runtime["tuple"], "auth proof runtime tuple is missing"),
        _integer(binding["checkpoint_field_id"], "auth proof field binding is invalid", 0, 0xffff),
        binding["checkpoint_seen"] is True,
        _integer(binding["checkpoint_seen_vblank"], "auth proof field binding is invalid"),
        _integer(binding["evidence_vblank"], "auth proof field binding is invalid"),
        binding["context_valid"] is True,
        _integer(binding["context_field_id"], "auth proof field binding is invalid", 0, 0xffff),
    )


def assert_auth_proof_matrix(receipt: JsonObject) -> None:
    if set(receipt) != {"schema", "schema_version", "task", "status", "matrix", "privacy"}:
        raise AuthProofError("auth proof matrix schema is not closed")
    if receipt["schema"] != AUTH_PROOF_MATRIX_SCHEMA or receipt["schema_version"] != 1 or receipt["task"] != 9:
        raise AuthProofError("auth proof matrix schema is invalid")
    if receipt["privacy"] != AUTH_PROOF_PRIVACY:
        raise AuthProofError("auth proof matrix privacy policy is invalid")
    matrix = _mapping(receipt["matrix"], "auth proof matrix is missing")
    if set(matrix) != {"scenario", "rows"} or matrix["scenario"] != "game-producer":
        raise AuthProofError("auth proof matrix is invalid")
    rows = matrix["rows"]
    if not isinstance(rows, list) or len(rows) != len(_MATRIX_ROWS):
        raise AuthProofError("auth proof matrix requires four rows")
    normalized: NormalizedObservation | None = None
    all_observed = True
    for index, value in enumerate(rows):
        row = _mapping(value, "auth proof matrix row is invalid")
        if set(row) != {"row_id", "build", "overlay_mode", "status", "observations", "reason", "mismatch_paths"}:
            raise AuthProofError("auth proof matrix row is not closed")
        build, tier = _MATRIX_ROWS[index]
        if row["row_id"] != f"{build}-{tier}" or row["build"] != build or row["overlay_mode"] != tier:
            raise AuthProofError("auth proof matrix rows are invalid")
        observations = row["observations"]
        if not isinstance(observations, list):
            raise AuthProofError("auth proof matrix observations are invalid")
        row_normalized: list[NormalizedObservation] = []
        for observation in observations:
            proof = _mapping(observation, "auth proof matrix observation is invalid")
            assert_auth_proof(proof, tier)
            if proof["status"] != "OBSERVED":
                raise AuthProofError("auth proof matrix observation is not observed")
            row_normalized.append(_normalized_observation(proof))
        if row["status"] == "OBSERVED":
            if len(observations) != 2:
                raise AuthProofError("auth proof matrix observed row requires two observations")
            if row["reason"] is not None or row["mismatch_paths"] != []:
                raise AuthProofError("auth proof matrix observed row has a mismatch")
        elif row["status"] == "BLOCKED":
            all_observed = False
            if not isinstance(row["reason"], str) or not row["reason"]:
                raise AuthProofError("auth proof matrix blocked row requires a reason")
            paths = row["mismatch_paths"]
            if not isinstance(paths, list) or not paths or not all(isinstance(path, str) and path.startswith("/") for path in paths):
                raise AuthProofError("auth proof matrix blocked row mismatch paths are invalid")
        else:
            raise AuthProofError("auth proof matrix row status is invalid")
        if row_normalized and any(item != row_normalized[0] for item in row_normalized[1:]):
            raise AuthProofError("auth proof matrix row observations diverge")
        if row_normalized:
            if normalized is not None and row_normalized[0] != normalized:
                raise AuthProofError("auth proof matrix observations diverge across rows")
            normalized = row_normalized[0]
    if receipt["status"] not in ("PASS", "BLOCKED") or (receipt["status"] == "PASS") != all_observed:
        raise AuthProofError("auth proof matrix status is invalid")
