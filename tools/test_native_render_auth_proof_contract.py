from __future__ import annotations

import pytest

from test_native_render_auth_proof import (
    auth_proof_matrix,
    blocked_auth_proof_diagnostic,
    observed_auth_proof,
    replay_module,
)


def test_auth_proof_when_observed_runtime_tuple_matches_static_is_accepted() -> None:
    replay_module().assert_auth_proof(observed_auth_proof(), "cold")


def test_auth_proof_when_tuple_is_nonzero_but_not_manifest_bound_is_rejected() -> None:
    proof = observed_auth_proof()
    runtime = proof["runtime"]
    assert isinstance(runtime, dict)
    runtime_tuple = runtime["tuple"]
    assert isinstance(runtime_tuple, dict)
    runtime_tuple["return_site"] = 0x800789C8

    with pytest.raises(ValueError, match="auth proof"):
        replay_module().assert_auth_proof(proof, "cold")


def test_auth_proof_when_static_provenance_is_valid_but_runtime_is_blocked_is_accepted() -> None:
    proof = observed_auth_proof()
    proof["status"] = "BLOCKED"
    runtime = proof["runtime"]
    assert isinstance(runtime, dict)
    runtime.clear()
    runtime.update(
        {
            "accepted": False,
            "tier": "none",
            "reject_reason": "none",
            "scene_aborted": False,
            "ir_usable": False,
            "native_permitted": False,
            "diagnostic": blocked_auth_proof_diagnostic(),
        }
    )

    replay_module().assert_auth_proof(proof, "cold")


def test_auth_proof_when_runtime_diagnostic_is_missing_is_rejected() -> None:
    proof = observed_auth_proof()
    runtime = proof["runtime"]
    assert isinstance(runtime, dict)
    del runtime["diagnostic"]

    with pytest.raises(ValueError, match="runtime snapshot is not closed"):
        replay_module().assert_auth_proof(proof, "cold")


def test_auth_proof_when_runtime_diagnostic_is_incomplete_is_rejected() -> None:
    proof = observed_auth_proof()
    runtime = proof["runtime"]
    assert isinstance(runtime, dict)
    diagnostic = runtime["diagnostic"]
    assert isinstance(diagnostic, dict)
    del diagnostic["accepted_return"]

    with pytest.raises(ValueError, match="diagnostic is not closed"):
        replay_module().assert_auth_proof(proof, "cold")


@pytest.mark.parametrize(
    "forbidden",
    ("address", "pc", "instruction_word", "trace_events", "identity_digest", "path", "cache_value", "input_data"),
)
def test_auth_proof_when_runtime_diagnostic_contains_private_material_is_rejected(forbidden: str) -> None:
    proof = observed_auth_proof()
    runtime = proof["runtime"]
    assert isinstance(runtime, dict)
    diagnostic = runtime["diagnostic"]
    assert isinstance(diagnostic, dict)
    diagnostic[forbidden] = "private"

    with pytest.raises(ValueError, match="diagnostic is not closed"):
        replay_module().assert_auth_proof(proof, "cold")


def test_auth_proof_when_runtime_trace_sequence_or_state_is_invalid_is_rejected() -> None:
    proof = observed_auth_proof()
    runtime = proof["runtime"]
    assert isinstance(runtime, dict)
    runtime_trace = runtime["trace"]
    assert isinstance(runtime_trace, dict)
    runtime_trace["capture_sequence"] = 24

    with pytest.raises(ValueError, match="trace"):
        replay_module().assert_auth_proof(proof, "cold")

    proof = observed_auth_proof()
    runtime = proof["runtime"]
    assert isinstance(runtime, dict)
    runtime_trace = runtime["trace"]
    assert isinstance(runtime_trace, dict)
    runtime_trace["scene_epoch"] = 0

    with pytest.raises(ValueError, match="trace"):
        replay_module().assert_auth_proof(proof, "cold")


def test_auth_proof_when_field_five_binding_is_invalid_is_rejected() -> None:
    proof = observed_auth_proof()
    binding = proof["field_binding"]
    assert isinstance(binding, dict)
    binding["context_field_id"] = 4

    with pytest.raises(ValueError, match="field binding"):
        replay_module().assert_auth_proof(proof, "cold")


def test_auth_proof_when_unknown_inner_field_is_present_is_rejected() -> None:
    proof = observed_auth_proof()
    proof["unexpected"] = True

    with pytest.raises(ValueError, match="schema is not closed"):
        replay_module().assert_auth_proof(proof, "cold")


def test_auth_proof_when_v2_schema_is_present_is_rejected() -> None:
    proof = observed_auth_proof()
    proof["schema"] = "xenogears.native-render-auth-proof/v2"

    with pytest.raises(ValueError, match="schema is invalid"):
        replay_module().assert_auth_proof(proof, "cold")


@pytest.mark.parametrize(
    ("field", "value"),
    (
        ("source", "runtime-overlay"),
        ("image", "other-image"),
        ("producer_entry", 0x80076348),
        ("range_start", 0x8006F004),
        ("range_size", 284671),
    ),
)
def test_auth_proof_when_static_provenance_is_not_canonical_is_rejected(field: str, value: str | int) -> None:
    proof = observed_auth_proof()
    static = proof["static"]
    assert isinstance(static, dict)
    provenance = static["provenance"]
    assert isinstance(provenance, dict)
    provenance[field] = value

    with pytest.raises(ValueError, match="static provenance"):
        replay_module().assert_auth_proof(proof, "cold")


@pytest.mark.parametrize("forbidden", ("hash", "bytes", "path", "cache_contents", "child_json"))
def test_auth_proof_when_static_provenance_contains_private_material_is_rejected(forbidden: str) -> None:
    proof = observed_auth_proof()
    static = proof["static"]
    assert isinstance(static, dict)
    provenance = static["provenance"]
    assert isinstance(provenance, dict)
    provenance[forbidden] = "private"

    with pytest.raises(ValueError, match="static provenance is not closed"):
        replay_module().assert_auth_proof(proof, "cold")


@pytest.mark.parametrize(("tier", "matched", "dispatched"), (("cold", True, True), ("warm", False, False)))
def test_auth_proof_when_observed_tier_candidate_flags_do_not_match_is_rejected(
    tier: str,
    matched: bool,
    dispatched: bool,
) -> None:
    proof = observed_auth_proof(tier)
    static = proof["static"]
    assert isinstance(static, dict)
    provenance = static["provenance"]
    assert isinstance(provenance, dict)
    candidate = provenance["candidate"]
    assert isinstance(candidate, dict)
    candidate["matched"] = matched
    candidate["dispatched"] = dispatched

    with pytest.raises(ValueError, match="candidate"):
        replay_module().assert_auth_proof(proof, tier)


def test_auth_proof_matrix_when_complete_closed_rows_differ_only_by_expected_tier_candidate_is_accepted() -> None:
    replay_module().assert_auth_proof_matrix(auth_proof_matrix())


def test_auth_proof_matrix_when_inner_schema_is_reused_is_rejected() -> None:
    receipt = auth_proof_matrix()
    receipt["schema"] = "xenogears.native-render-auth-proof/v2"

    with pytest.raises(ValueError, match="matrix schema"):
        replay_module().assert_auth_proof_matrix(receipt)


def test_auth_proof_matrix_when_row_is_incomplete_is_rejected() -> None:
    receipt = auth_proof_matrix()
    matrix = receipt["matrix"]
    assert isinstance(matrix, dict)
    rows = matrix["rows"]
    assert isinstance(rows, list)
    first_row = rows[0]
    assert isinstance(first_row, dict)
    observations = first_row["observations"]
    assert isinstance(observations, list)
    observations.pop()

    with pytest.raises(ValueError, match="two observations"):
        replay_module().assert_auth_proof_matrix(receipt)


def test_auth_proof_matrix_when_row_tier_does_not_match_is_rejected() -> None:
    receipt = auth_proof_matrix()
    matrix = receipt["matrix"]
    assert isinstance(matrix, dict)
    rows = matrix["rows"]
    assert isinstance(rows, list)
    first_row = rows[0]
    assert isinstance(first_row, dict)
    observations = first_row["observations"]
    assert isinstance(observations, list)
    first_proof = observations[0]
    assert isinstance(first_proof, dict)
    runtime = first_proof["runtime"]
    assert isinstance(runtime, dict)
    runtime["tier"] = "warm"

    with pytest.raises(ValueError, match="tier"):
        replay_module().assert_auth_proof_matrix(receipt)


def test_auth_proof_matrix_when_valid_rows_bind_different_evidence_vblanks_is_rejected() -> None:
    receipt = auth_proof_matrix()
    matrix = receipt["matrix"]
    assert isinstance(matrix, dict)
    rows = matrix["rows"]
    assert isinstance(rows, list)
    row = rows[1]
    assert isinstance(row, dict)
    observations = row["observations"]
    assert isinstance(observations, list)
    for observation in observations:
        assert isinstance(observation, dict)
        binding = observation["field_binding"]
        assert isinstance(binding, dict)
        binding["evidence_vblank"] = 4

    with pytest.raises(ValueError, match="across rows"):
        replay_module().assert_auth_proof_matrix(receipt)


def test_auth_proof_matrix_when_blocked_row_has_no_reason_is_rejected() -> None:
    receipt = auth_proof_matrix()
    receipt["status"] = "BLOCKED"
    matrix = receipt["matrix"]
    assert isinstance(matrix, dict)
    rows = matrix["rows"]
    assert isinstance(rows, list)
    first_row = rows[0]
    assert isinstance(first_row, dict)
    first_row["status"] = "BLOCKED"
    first_row["observations"] = []
    first_row["reason"] = None
    first_row["mismatch_paths"] = ["/auth_proof"]

    with pytest.raises(ValueError, match="blocked row"):
        replay_module().assert_auth_proof_matrix(receipt)


def test_auth_proof_matrix_when_unknown_field_is_present_is_rejected() -> None:
    receipt = auth_proof_matrix()
    receipt["unexpected"] = True

    with pytest.raises(ValueError, match="matrix schema is not closed"):
        replay_module().assert_auth_proof_matrix(receipt)
