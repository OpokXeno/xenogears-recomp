from __future__ import annotations

from pathlib import Path
import re

import pytest

from native_render_auth_proof import JsonObject
from test_native_render_auth_proof import (
    blocked_auth_proof_diagnostic,
    observed_auth_proof,
    replay_module,
)


def blocked_proof() -> JsonObject:
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
    return proof


def instrumentation(proof: JsonObject) -> JsonObject:
    runtime = proof["runtime"]
    assert isinstance(runtime, dict)
    diagnostic = runtime["diagnostic"]
    assert isinstance(diagnostic, dict)
    value = diagnostic["instrumentation"]
    assert isinstance(value, dict)
    return value


def test_auth_proof_when_closed_v4_instrumentation_has_only_cold_hook_ingress_is_accepted() -> None:
    proof = blocked_proof()
    value = instrumentation(proof)
    value["cold_hook_ingress_count"] = 55

    replay_module().assert_auth_proof(proof, "cold")


def test_auth_proof_when_physical_stage_count_exceeds_exact_stage_count_is_accepted() -> None:
    proof = blocked_proof()
    value = instrumentation(proof)
    value.update(
        {
            "cold_hook_ingress_count": 55,
            "activation_physical_count": 1,
            "scene_boundary_count": 2,
        }
    )

    replay_module().assert_auth_proof(proof, "cold")


def test_auth_proof_when_progress_is_reset_before_publication_is_accepted() -> None:
    proof = blocked_proof()
    value = instrumentation(proof)
    value.update(
        {
            "cold_hook_ingress_count": 55,
            "activation_physical_count": 1,
            "activation_exact_count": 1,
            "entry_physical_count": 1,
            "entry_exact_count": 1,
            "capture_physical_count": 1,
            "capture_exact_count": 1,
            "return_physical_count": 1,
            "return_exact_count": 1,
            "last_progress_sequence": 23,
            "last_reset_sequence": 24,
            "scene_boundary_count": 2,
            "disarm_count": 1,
        }
    )

    replay_module().assert_auth_proof(proof, "cold")


def test_auth_proof_when_instrumentation_contains_an_unknown_private_metric_is_rejected() -> None:
    proof = observed_auth_proof()
    instrumentation(proof)["unknown_private_metric"] = 0

    with pytest.raises(ValueError, match="instrumentation is not closed"):
        replay_module().assert_auth_proof(proof, "cold")


def test_auth_proof_when_instrumentation_revision_is_not_two_is_rejected() -> None:
    proof = observed_auth_proof()
    instrumentation(proof)["revision"] = 1

    with pytest.raises(ValueError, match="instrumentation is invalid"):
        replay_module().assert_auth_proof(proof, "cold")


@pytest.mark.parametrize(
    ("field", "value"),
    (("counter_overflow_events", 1), ("counters_poisoned", True)),
)
def test_auth_proof_when_instrumentation_counters_are_compromised_is_rejected(
    field: str,
    value: int | bool,
) -> None:
    proof = observed_auth_proof()
    instrumentation(proof)[field] = value

    with pytest.raises(ValueError, match="instrumentation is invalid"):
        replay_module().assert_auth_proof(proof, "cold")


def test_auth_proof_delta_when_counter_regresses_fails_closed_without_unsigned_subtraction() -> None:
    source = (
        Path(__file__).resolve().parents[1]
        / "psxrecomp" / "runtime" / "src" / "input_replay.cpp"
    ).read_text(encoding="utf-8")
    start = source.index("PsxXgRenderAuthInstrumentation auth_instrumentation_delta(")
    end = source.index("\nbool auth_trace_contains_sequence", start)
    delta = source[start:end]

    assert "if (value < baseline)" in delta
    assert "counters_regressed = true" in delta
    assert "start.counters_poisoned || counters_regressed" in delta
    assert re.search(r"current\.\w+\s*-\s*start\.", delta) is None


def test_auth_proof_runtime_acceptance_fails_closed_for_overflow_or_poison() -> None:
    source = (
        Path(__file__).resolve().parents[1]
        / "psxrecomp" / "runtime" / "src" / "input_replay.cpp"
    ).read_text(encoding="utf-8")
    start = source.index("const bool runtime_accepted =")
    end = source.index(";", start)
    acceptance = source[start:end]

    assert "instrumentation.revision == 2u" in acceptance
    assert "instrumentation.counter_overflow_events == 0u" in acceptance
    assert "!instrumentation.counters_poisoned" in acceptance


@pytest.mark.parametrize(
    ("field", "value"),
    (
        ("activation_exact_count", 2),
        ("last_publish_sequence", 23),
        ("completed_proof_publication_count", 0),
    ),
)
def test_auth_proof_when_instrumentation_relationship_is_non_monotonic_is_rejected(
    field: str,
    value: int,
) -> None:
    proof = observed_auth_proof()
    instrumentation(proof)[field] = value

    with pytest.raises(ValueError, match="instrumentation is invalid"):
        replay_module().assert_auth_proof(proof, "cold")
