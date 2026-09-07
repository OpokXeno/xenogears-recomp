from __future__ import annotations

import importlib.util
import json
from pathlib import Path
from subprocess import CompletedProcess, run
import sys
from tempfile import TemporaryDirectory
from types import ModuleType

import pytest

from native_render_auth_proof import JsonObject


ROOT = Path(__file__).resolve().parents[1]
REPLAY_SCRIPT = ROOT / "tools" / "native_render_replay.py"
CLI_SCRIPT = ROOT / "tools" / "native_render_replay_cli.py"
BASELINE_SCRIPT = ROOT / "tools" / "native_render_replay_baseline.py"


def replay_module() -> ModuleType:
    specification = importlib.util.spec_from_file_location("native_render_replay", REPLAY_SCRIPT)
    assert specification is not None
    assert specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def baseline_module() -> ModuleType:
    replay_module()
    specification = importlib.util.spec_from_file_location("native_render_replay_baseline", BASELINE_SCRIPT)
    assert specification is not None
    assert specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def cli_module() -> ModuleType:
    baseline_module()
    specification = importlib.util.spec_from_file_location("native_render_replay_cli", CLI_SCRIPT)
    assert specification is not None
    assert specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def observed_auth_proof(tier: str = "cold") -> JsonObject:
    proof_tuple = {
        "producer_entry": 2147965764,
        "capture_site": 2147975612,
        "static_callee": 2147792204,
        "return_site": 2147975620,
    }
    return {
        "schema": "xenogears.native-render-auth-proof/v4",
        "status": "OBSERVED",
        "privacy": {
            "metadata_only": True,
            "raw_instruction_words": False,
            "raw_delay_slot_words": False,
            "identities_or_digests": False,
            "private_paths": False,
            "disc_cards_cache_hashes": False,
            "input_states": False,
            "packets": False,
            "child_runtime_json": False,
        },
        "static": {
            "accepted": True,
            "provenance": {
                "source": "manifest-overlay",
                "image": "field-image",
                "producer_entry": 0x80075B44,
                "range_start": 0x8006F000,
                "range_size": 282624,
                "manifest_bound": True,
                "range_bound": True,
                "candidate": {
                    "matched": tier == "warm",
                    "dispatched": tier == "warm",
                },
            },
        },
        "runtime": {
            "accepted": True,
            "tier": tier,
            "reject_reason": "none",
            "scene_aborted": False,
            "ir_usable": True,
            "native_permitted": True,
            "diagnostic": {
                "available": True,
                "producer_begin_count": 1,
                "hook_count": 3,
                "trace_event_count": 3,
                "trace_overflowed": False,
                "accepted_entry": True,
                "accepted_capture": True,
                "accepted_return": True,
                "rejected_event_count": 0,
                "reset_since_trace_start": False,
                "scene_aborted": False,
                "reject_reason": "none",
            "rejection_source": "none",
            "rejection_hook": "none",
            "rejection_guest_pc": 0,
            "instrumentation": {
                "revision": 2,
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
                "last_reset_sequence": 0,
                "last_publish_sequence": 24,
                 "scene_boundary_count": 1,
                 "disarm_count": 0,
                 "completed_proof_publication_count": 1,
                 "native_ir_flush_attempt_count": 0,
                 "native_ir_flush_failure_count": 0,
                 "first_native_ir_flush_failure_index": 0,
                 "first_native_ir_flush_failure_reason": 0,
                 "first_native_ir_flush_failure_packet": 0,
                  "first_native_ir_flush_failure_status": 0,
                  "counter_overflow_events": 0,
                  "counters_poisoned": False,
            },
            },
            "tuple": proof_tuple,
            "trace": {
                "entry_sequence": 21,
                "capture_sequence": 22,
                "return_sequence": 23,
                "scene_epoch": 1,
                "state_sequence": 0,
            },
        },
        "field_binding": {
            "checkpoint_field_id": 5,
            "checkpoint_seen": True,
            "checkpoint_seen_vblank": 2,
            "evidence_vblank": 3,
            "context_valid": True,
            "context_field_id": 5,
        },
    }


def blocked_auth_proof_diagnostic() -> JsonObject:
    return {
        "available": True,
        "producer_begin_count": 0,
        "hook_count": 0,
        "trace_event_count": 0,
        "trace_overflowed": False,
        "accepted_entry": False,
        "accepted_capture": False,
        "accepted_return": False,
        "rejected_event_count": 0,
        "reset_since_trace_start": False,
        "scene_aborted": False,
        "reject_reason": "none",
        "rejection_source": "none",
        "rejection_hook": "none",
        "rejection_guest_pc": 0,
        "instrumentation": {
            "revision": 2,
            "cold_hook_ingress_count": 0,
            "activation_physical_count": 0,
            "activation_exact_count": 0,
            "entry_physical_count": 0,
            "entry_exact_count": 0,
            "capture_physical_count": 0,
            "capture_exact_count": 0,
            "return_physical_count": 0,
            "return_exact_count": 0,
            "last_progress_sequence": 0,
            "last_reset_sequence": 0,
            "last_publish_sequence": 0,
             "scene_boundary_count": 1,
             "disarm_count": 0,
             "completed_proof_publication_count": 0,
             "native_ir_flush_attempt_count": 0,
             "native_ir_flush_failure_count": 0,
             "first_native_ir_flush_failure_index": 0,
             "first_native_ir_flush_failure_reason": 0,
             "first_native_ir_flush_failure_packet": 0,
              "first_native_ir_flush_failure_status": 0,
              "counter_overflow_events": 0,
              "counters_poisoned": False,
        },
    }


def auth_proof_matrix() -> JsonObject:
    rows: list[JsonObject] = []
    for build, mode in (("debug", "cold"), ("debug", "warm"), ("release", "cold"), ("release", "warm")):
        proof = observed_auth_proof(mode)
        rows.append({
            "row_id": f"{build}-{mode}",
            "build": build,
            "overlay_mode": mode,
            "status": "OBSERVED",
            "observations": [proof, json.loads(json.dumps(proof))],
            "reason": None,
            "mismatch_paths": [],
        })
    return {
        "schema": "xenogears.native-render-auth-proof-matrix/v1",
        "schema_version": 1,
        "task": 9,
        "status": "PASS",
        "matrix": {"scenario": "game-producer", "rows": rows},
        "privacy": observed_auth_proof()["privacy"],
    }


def test_auth_proof_cli_when_asked_for_help_exposes_supported_field_five_matrix() -> None:
    completed: CompletedProcess[str] = run(
        [sys.executable, str(CLI_SCRIPT), "auth-proof", "--help"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )

    assert completed.returncode == 0, completed.stderr
    assert "--builds" in completed.stdout
    assert "--warm-cache" in completed.stdout
    assert "--evidence" in completed.stdout


def test_auth_proof_matrix_when_runtime_trace_is_not_observed_emits_only_blocked_receipt(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    baseline = baseline_module()
    blocked = observed_auth_proof()
    blocked["status"] = "BLOCKED"
    static = blocked["static"]
    assert isinstance(static, dict)
    static["accepted"] = False
    provenance = static["provenance"]
    assert isinstance(provenance, dict)
    provenance["manifest_bound"] = False
    runtime = blocked["runtime"]
    assert isinstance(runtime, dict)
    runtime.clear()
    runtime.update({
        "accepted": False,
        "tier": "none",
        "reject_reason": "none",
        "scene_aborted": False,
        "ir_usable": False,
        "native_permitted": False,
        "diagnostic": blocked_auth_proof_diagnostic(),
    })
    monkeypatch.setattr(baseline, "execute_runtime_child", lambda _child, _watchdog: {"auth_proof": blocked})
    with TemporaryDirectory() as temporary:
        root = Path(temporary)
        for name in ("card1.mcd", "card2.mcd"):
            (root / name).write_bytes(b"card")
        request = baseline.BaselineRequest(
            repository_root=root,
            trace=ROOT / "tools" / "native_render_replays" / "field_baseline.toml",
            builds=(
                baseline.BuildTarget("debug", root / "debug"),
                baseline.BuildTarget("release", root / "release"),
            ),
            disc=root / "disc1.cue",
            warm_cache=root / "warm-cache",
            evidence=root / "auth-proof.json",
            watchdog_seconds=1,
        )

        receipt = baseline.execute_auth_proof_matrix(request)

    assert receipt["status"] == "BLOCKED"
    assert all(row["status"] == "BLOCKED" for row in receipt["matrix"]["rows"])
    assert "\"PASS\"" not in json.dumps(receipt)


def test_auth_proof_matrix_when_warm_candidate_is_unavailable_publishes_safe_blocked_rows(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    baseline = baseline_module()
    private_marker = "private-warm-cache-contents"

    def execute(child: object, _watchdog: int) -> dict[str, object]:
        tier = getattr(child, "overlay_mode")
        proof = observed_auth_proof(tier)
        if tier == "warm":
            proof["status"] = "BLOCKED"
            static = proof["static"]
            assert isinstance(static, dict)
            provenance = static["provenance"]
            assert isinstance(provenance, dict)
            candidate = provenance["candidate"]
            assert isinstance(candidate, dict)
            candidate["matched"] = False
            candidate["dispatched"] = False
        return {"auth_proof": proof, "private_cache_contents": private_marker}

    monkeypatch.setattr(baseline, "execute_runtime_child", execute)
    with TemporaryDirectory() as temporary:
        root = Path(temporary)
        for name in ("card1.mcd", "card2.mcd"):
            (root / name).write_bytes(b"card")
        request = baseline.BaselineRequest(
            repository_root=root,
            trace=ROOT / "tools" / "native_render_replays" / "field_baseline.toml",
            builds=(
                baseline.BuildTarget("debug", root / "debug"),
                baseline.BuildTarget("release", root / "release"),
            ),
            disc=root / "disc1.cue",
            warm_cache=root / "warm-cache",
            evidence=root / "auth-proof.json",
            watchdog_seconds=1,
        )

        receipt = baseline.execute_auth_proof_matrix(request)

    warm_rows = [row for row in receipt["matrix"]["rows"] if row["overlay_mode"] == "warm"]
    assert receipt["status"] == "BLOCKED"
    assert all(row["reason"] == "auth_proof_candidate_unavailable" for row in warm_rows)
    assert all(row["mismatch_paths"] == ["/auth_proof/static/provenance/candidate"] for row in warm_rows)
    assert private_marker not in json.dumps(receipt)
