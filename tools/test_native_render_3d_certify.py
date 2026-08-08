from __future__ import annotations

import copy
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "native_render_3d_certify.py"
TEMPLATE = ROOT / "native_renderer" / "xg_3d_certification.toml"
AGGREGATE_KINDS = (
    "coverage",
    "fault-rollback",
    "gte-attribution-residual",
    "ordering-material-vram-framebuffer",
    "shadow-layers",
    "source-isolation-poisoning",
)
CHECK_FIELDS = (
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
)
DIGEST_FIELDS = (
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
)


def canonical(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":")).encode("ascii") + b"\n"


def sha256(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def set_digest(values: list[str]) -> str:
    return sha256(("\n".join(sorted(values)) + "\n").encode("ascii"))


def write_json(root: Path, relative: str, value: object) -> str:
    destination = root / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    raw = canonical(value)
    destination.write_bytes(raw)
    return sha256(raw)


def cleanup() -> dict[str, object]:
    return {
        "complete": True,
        "guest_state_restored": True,
        "gpu_state_restored": True,
        "runtime_state_restored": True,
        "private_artifacts_removed": True,
        "live_processes": 0,
        "stale_leases": 0,
    }


def coverage() -> dict[str, int]:
    return {
        "target_3d_producers": 2,
        "migrated_3d_producers": 2,
        "exercised_3d_producers": 2,
        "target_3d_families": 1,
        "migrated_3d_families": 1,
        "exercised_3d_families": 1,
        "target_reachable_render_branches": 2,
        "migrated_render_branches": 2,
        "exercised_render_branches": 2,
        "ledger_render_sites": 2,
        "migrated_3d_sites": 2,
        "excluded_pure_2d_proven_sites": 0,
        "non_render_proven_sites": 0,
        "unclassified_render_gte_sites": 0,
        "unclassified_reachable_render_branches": 0,
        "unattributed_original_3d_primitives": 0,
    }


def isolation(mode: str, tier: str) -> dict[str, int]:
    result = {
        "render_gte_exec_count": 0,
        "native_target_gte_site_hits_static": 0,
        "native_target_gte_site_hits_cold": 0,
        "native_target_gte_site_hits_warm": 0,
        "semantic_post_gte_reads": 0,
        "target_packet_payload_reads_by_semantic_lane": 0,
        "target_gp0_decode_to_semantic_calls": 0,
        "target_ot_payload_geometry_or_material_reads": 0,
    }
    if mode != "native":
        result["render_gte_exec_count"] = 4
        result[f"native_target_gte_site_hits_{tier}"] = 4
    return result


def parity(digest: str = "d" * 64) -> dict[str, object]:
    return {
        "original_normalized_primitives": 7,
        "native_normalized_ir_primitives": 7,
        **{field: digest for field in DIGEST_FIELDS},
    }


def aggregate_metrics(row_count: int) -> dict[str, int]:
    source = coverage()
    return {
        "row_count": row_count,
        "target_3d_producers": source["target_3d_producers"],
        "migrated_3d_producers": source["migrated_3d_producers"],
        "exercised_3d_producers": source["exercised_3d_producers"],
        "target_3d_families": source["target_3d_families"],
        "migrated_3d_families": source["migrated_3d_families"],
        "exercised_3d_families": source["exercised_3d_families"],
        "target_reachable_render_branches": source["target_reachable_render_branches"],
        "migrated_render_branches": source["migrated_render_branches"],
        "exercised_render_branches": source["exercised_render_branches"],
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


def matrix_receipt(
    *,
    index: int,
    disc: str,
    build: str,
    scenario: str,
    producer: str,
    branch: str,
    tier: str,
    mode: str,
    repetition: int,
    image: dict[str, object],
    build_sha256: str,
    nonsemantic: dict[str, object],
) -> dict[str, object]:
    return {
        "schema": "xenogears.native-3d-matrix-receipt/v1",
        "run_id": sha256(f"matrix-run-{index}".encode("ascii")),
        "row": {
            "disc": disc,
            "build": build,
            "scenario": scenario,
            "producer": producer,
            "tier": tier,
            "requested_mode": mode,
            "effective_mode": mode,
            "repetition": repetition,
            "renderer": "opengl",
            "present": "canonical",
            "color_depth_bits": 15,
            "interpolation": False,
            "smooth": False,
            "wide": False,
            "hires": False,
        },
        "identity": {
            "image_id": image["id"],
            "image_sha256": image["sha256"],
            "base_address": image["base_address"],
            "generation": image["generation"],
            "build_sha256": build_sha256,
            "producer": producer,
            "branches": [branch],
        },
        "coverage": coverage(),
        "isolation": isolation(mode, tier),
        "parity": parity(),
        "checks": {field: True for field in CHECK_FIELDS},
        "fallback": {
            "qualified_native": mode == "native",
            "fallback_count": 0,
            "overflow_count": 0,
            "stale_state_count": 0,
            "unsupported_material_count": 0,
        },
        "cleanup": cleanup(),
        "nonsemantic": nonsemantic,
    }


def mutate_receipt(receipt: dict[str, object], variant: str, *, first: bool) -> None:
    if variant == "mismatch" and first:
        receipt["parity"]["native_normalized_ir_primitives"] = 8
    elif variant == "fallback" and receipt["row"]["requested_mode"] == "native" and first:
        receipt["fallback"]["fallback_count"] = 1
    elif variant == "cleanup" and first:
        receipt["cleanup"]["complete"] = False
    elif variant == "privacy" and first:
        receipt["nonsemantic"]["capture_note"] = "/home/operator/private-capture.bin"
    elif variant == "determinism" and receipt["row"]["repetition"] == 2 and first:
        tier = receipt["row"]["tier"]
        receipt["isolation"]["render_gte_exec_count"] = 5
        receipt["isolation"][f"native_target_gte_site_hits_{tier}"] = 5
    elif variant == "cross-mode-parity" and receipt["row"]["requested_mode"] == "shadow":
        receipt["parity"] = parity("e" * 64)
    elif variant == "unknown-producer" and first:
        receipt["identity"]["producer"] = "unknown-producer"
    elif variant == "unknown-hash" and first:
        receipt["identity"]["image_sha256"] = "f" * 64
    elif variant == "unknown-branch" and first:
        receipt["identity"]["branches"] = ["unknown-branch"]
    elif variant == "effective-original" and receipt["row"]["requested_mode"] == "native" and first:
        receipt["row"]["effective_mode"] = "original"
    elif variant == "effective-shadow" and receipt["row"]["requested_mode"] == "native" and first:
        receipt["row"]["effective_mode"] = "shadow"
    elif variant == "unqualified-native" and receipt["row"]["requested_mode"] == "native" and first:
        receipt["fallback"]["qualified_native"] = False
    elif variant == "qualified-original" and receipt["row"]["requested_mode"] == "original" and first:
        receipt["fallback"]["qualified_native"] = True
    elif variant == "oracle-zero" and receipt["row"]["requested_mode"] == "original" and first:
        receipt["isolation"] = isolation("native", receipt["row"]["tier"])
    elif variant == "oracle-wrong-tier" and receipt["row"]["requested_mode"] == "original" and first:
        selected_tier = receipt["row"]["tier"]
        wrong_tier = next(tier for tier in ("static", "cold", "warm") if tier != selected_tier)
        receipt["isolation"][f"native_target_gte_site_hits_{selected_tier}"] = 0
        receipt["isolation"][f"native_target_gte_site_hits_{wrong_tier}"] = 4
    elif variant == "native-gte" and receipt["row"]["requested_mode"] == "native" and first:
        tier = receipt["row"]["tier"]
        receipt["isolation"]["render_gte_exec_count"] = 4
        receipt["isolation"][f"native_target_gte_site_hits_{tier}"] = 4
    elif variant == "semantic-isolation" and receipt["row"]["requested_mode"] == "original" and first:
        receipt["isolation"]["semantic_post_gte_reads"] = 1
    elif variant == "missing-field" and first:
        del receipt["checks"]["source_snapshot_complete"]


def build_certification(root: Path, variant: str = "success") -> Path:
    documents = []
    for identifier, kind in (("ledger", "ledger"), ("contracts", "contract"), ("branches", "branch-ledger")):
        relative = f"metadata/{identifier}.json"
        digest = write_json(root, relative, {"schema": f"fixture.{identifier}/v1", "records": []})
        documents.append({"id": identifier, "kind": kind, "path": relative, "sha256": digest})

    discs = ({"id": "disc1", "number": 1}, {"id": "disc2", "number": 2})
    hosts = ({"id": "linux-x86_64", "os": "linux", "architecture": "x86-64"},)
    toolchains = ({"id": "gcc", "version": "13.2.0", "hosts": ["linux-x86_64"]},)
    builds = (
        {"id": "gcc-debug", "configuration": "debug", "toolchain": "gcc", "host": "linux-x86_64", "sha256": "1" * 64},
        {"id": "gcc-release", "configuration": "release", "toolchain": "gcc", "host": "linux-x86_64", "sha256": "2" * 64},
    )
    images = []
    for disc_index, disc in enumerate(("disc1", "disc2"), start=1):
        for tier_index, tier in enumerate(("static", "cold", "warm"), start=1):
            images.append(
                {
                    "id": f"{disc}-{tier}",
                    "disc": disc,
                    "tier": tier,
                    "sha256": sha256(f"{disc}-{tier}-image".encode("ascii")),
                    "base_address": 0x80010000 + disc_index * 0x10000 + tier_index * 0x1000,
                    "generation": tier_index,
                }
            )
    image_by_key = {(item["disc"], item["tier"]): item for item in images}
    families = ({"id": "field-models"},)
    producers = (
        {"id": "disc1-producer", "family": "field-models", "discs": ["disc1"], "tiers": ["static", "cold", "warm"]},
        {"id": "disc2-producer", "family": "field-models", "discs": ["disc2"], "tiers": ["static", "cold", "warm"]},
    )
    branches = (
        {"id": "disc1-visible", "producer": "disc1-producer", "reachable": True, "proof_document": ""},
        {"id": "disc2-visible", "producer": "disc2-producer", "reachable": True, "proof_document": ""},
    )
    scenarios = (
        {
            "id": "disc1-scenario",
            "disc": "disc1",
            "producers": ["disc1-producer"],
            "branches": ["disc1-visible"],
            "tiers": ["static", "cold", "warm"],
            "ordinary_input": True,
            "private_artifacts_authenticated": True,
        },
        {
            "id": "disc2-scenario",
            "disc": "disc2",
            "producers": ["disc2-producer"],
            "branches": ["disc2-visible"],
            "tiers": ["static", "cold", "warm"],
            "ordinary_input": True,
            "private_artifacts_authenticated": True,
        },
    )
    whitelist = ["capture_note"] if variant == "privacy" else []
    default_nonsemantic = {"capture_note": "isolated-fixture"} if whitelist else {}
    matrix = []
    matrix_digests = []
    index = 0
    mutation_done = False
    for scenario in scenarios:
        producer = scenario["producers"][0]
        branch = scenario["branches"][0]
        for tier in scenario["tiers"]:
            for build in builds:
                for mode in ("original", "shadow", "native"):
                    for repetition in (1, 2):
                        index += 1
                        receipt = matrix_receipt(
                            index=index,
                            disc=scenario["disc"],
                            build=build["id"],
                            scenario=scenario["id"],
                            producer=producer,
                            branch=branch,
                            tier=tier,
                            mode=mode,
                            repetition=repetition,
                            image=image_by_key[(scenario["disc"], tier)],
                            build_sha256=build["sha256"],
                            nonsemantic=copy.deepcopy(default_nonsemantic),
                        )
                        should_mutate = not mutation_done
                        if variant in {"fallback", "effective-original", "effective-shadow", "unqualified-native", "native-gte"} and mode != "native":
                            should_mutate = False
                        if variant in {"qualified-original", "oracle-zero", "oracle-wrong-tier", "semantic-isolation"} and mode != "original":
                            should_mutate = False
                        if variant == "determinism" and repetition != 2:
                            should_mutate = False
                        if variant == "cross-mode-parity":
                            should_mutate = mode == "shadow"
                        mutate_receipt(receipt, variant, first=should_mutate)
                        if should_mutate and variant not in {"success", "missing-row", "cross-mode-parity"}:
                            mutation_done = True
                        relative = f"receipts/matrix-{index:03d}.json"
                        digest = write_json(root, relative, receipt)
                        matrix_digests.append(digest)
                        matrix.append(
                            {
                                "disc": scenario["disc"],
                                "build": build["id"],
                                "scenario": scenario["id"],
                                "producer": producer,
                                "tier": tier,
                                "requested_mode": mode,
                                "repetition": repetition,
                                "receipt": relative,
                                "sha256": digest,
                            }
                        )
    if variant == "missing-row":
        matrix.pop()
        matrix_digests.pop()

    expected_row_count = 72
    matrix_sha256 = set_digest(matrix_digests)
    aggregate_receipts = []
    for aggregate_index, kind in enumerate(AGGREGATE_KINDS, start=1):
        receipt = {
            "schema": "xenogears.native-3d-aggregate-receipt/v1",
            "run_id": sha256(f"aggregate-run-{aggregate_index}".encode("ascii")),
            "kind": kind,
            "matrix_sha256": matrix_sha256,
            "metrics": aggregate_metrics(expected_row_count),
            "cleanup": cleanup(),
            "nonsemantic": copy.deepcopy(default_nonsemantic),
        }
        relative = f"receipts/aggregate-{aggregate_index}.json"
        digest = write_json(root, relative, receipt)
        aggregate_receipts.append({"kind": kind, "receipt": relative, "sha256": digest})

    lines = [
        'schema = "xenogears.native-3d-certification/v1"',
        "sealed = true",
        'certification_id = "synthetic-p11"',
        "repetitions = 2",
        f"nonsemantic_whitelist = {json.dumps(whitelist)}",
        "",
        "[parity]",
        'renderer = "opengl"',
        'present = "canonical"',
        "color_depth_bits = 15",
        "interpolation = false",
        "smooth = false",
        "wide = false",
        "hires = false",
        "",
        "[expected]",
        "ledger_render_sites = 2",
        "migrated_3d_sites = 2",
        "excluded_pure_2d_proven_sites = 0",
        "non_render_proven_sites = 0",
        "",
    ]

    def tables(name: str, records: object) -> None:
        assert isinstance(records, (list, tuple))
        for record in records:
            lines.append(f"[[{name}]]")
            for key, value in record.items():
                rendered = str(value).lower() if isinstance(value, bool) else json.dumps(value)
                lines.append(f"{key} = {rendered}")
            lines.append("")

    tables("documents", documents)
    tables("discs", discs)
    tables("hosts", hosts)
    tables("toolchains", toolchains)
    tables("builds", builds)
    tables("images", images)
    tables("families", families)
    tables("producers", producers)
    tables("branches", branches)
    tables("scenarios", scenarios)
    tables("matrix", matrix)
    tables("aggregate_receipts", aggregate_receipts)
    certification = root / "certification.toml"
    certification.write_text("\n".join(lines), encoding="utf-8")
    return certification


def run_tool(certification: Path, evidence: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(TOOL),
            "--certification",
            str(certification),
            "--renderer",
            "opengl",
            "--evidence",
            str(evidence),
        ],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )


def test_complete_closed_matrix_emits_canonical_metadata_only_evidence(tmp_path: Path) -> None:
    certification = build_certification(tmp_path)
    evidence = tmp_path / "evidence.json"

    result = run_tool(certification, evidence)

    assert result.returncode == 0
    assert result.stdout == "NATIVE_RENDER_3D_CERTIFICATION_PASS\n"
    assert result.stderr == ""
    raw = evidence.read_bytes()
    decoded = json.loads(raw)
    assert raw == canonical(decoded)
    assert decoded["verdict"] == "PASS"
    assert decoded["coverage"]["matrix_rows"] == 72
    assert decoded["invariants"]["native_qualified_fallback_count"] == 0
    assert str(tmp_path).encode() not in raw
    assert b'"receipt"' not in raw


@pytest.mark.parametrize(
    ("variant", "code"),
    (
        ("missing-row", "MISSING_ROW"),
        ("mismatch", "INVARIANT_MISMATCH"),
        ("fallback", "FALLBACK"),
        ("cleanup", "CLEANUP_INCOMPLETE"),
        ("privacy", "PRIVACY"),
        ("determinism", "NONDETERMINISTIC"),
        ("cross-mode-parity", "NONDETERMINISTIC"),
        ("effective-original", "EFFECTIVE_ORIGINAL"),
        ("effective-shadow", "ROW_MISMATCH"),
        ("unqualified-native", "ROW_MISMATCH"),
        ("qualified-original", "ROW_MISMATCH"),
        ("oracle-zero", "INVARIANT_MISMATCH"),
        ("oracle-wrong-tier", "INVARIANT_MISMATCH"),
        ("native-gte", "INVARIANT_MISMATCH"),
        ("semantic-isolation", "INVARIANT_MISMATCH"),
        ("missing-field", "SCHEMA"),
    ),
)
def test_failed_gate_never_publishes_evidence(tmp_path: Path, variant: str, code: str) -> None:
    certification = build_certification(tmp_path, variant)
    evidence = tmp_path / "must-not-exist.json"

    result = run_tool(certification, evidence)

    assert result.returncode != 0
    assert result.stdout == ""
    assert result.stderr == f"NATIVE_RENDER_3D_CERTIFICATION_REJECT {code}\n"
    assert not evidence.exists()
    assert str(tmp_path) not in result.stderr


@pytest.mark.parametrize(
    ("variant", "code"),
    (
        ("unknown-producer", "UNKNOWN_PRODUCER"),
        ("unknown-hash", "UNKNOWN_HASH"),
        ("unknown-branch", "UNKNOWN_BRANCH"),
    ),
)
def test_unknown_sealed_identity_is_rejected(tmp_path: Path, variant: str, code: str) -> None:
    certification = build_certification(tmp_path, variant)
    evidence = tmp_path / "must-not-exist.json"

    result = run_tool(certification, evidence)

    assert result.returncode == 2
    assert result.stderr == f"NATIVE_RENDER_3D_CERTIFICATION_REJECT {code}\n"
    assert not evidence.exists()


def test_checked_in_template_is_explicitly_unsealed_and_rejected(tmp_path: Path) -> None:
    evidence = tmp_path / "must-not-exist.json"

    result = run_tool(TEMPLATE, evidence)

    assert result.returncode == 2
    assert result.stderr == "NATIVE_RENDER_3D_CERTIFICATION_REJECT UNSEALED\n"
    assert not evidence.exists()
