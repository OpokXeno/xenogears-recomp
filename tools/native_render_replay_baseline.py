from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import stat
from subprocess import PIPE, Popen, TimeoutExpired
import tempfile
import time

from native_render_replay import (
    AUTH_PROOF_PRIVACY,
    AUTH_PROOF_MATRIX_SCHEMA,
    BASELINE_ROWS,
    HOST_FIELD_ALLOWLIST,
    _runtime_environment,
    assert_auth_proof,
    assert_auth_proof_matrix,
    canonical_renderer_digest,
    parse_trace,
    public_card_integrity,
    scrub_private,
    snapshot_root_cards,
)
from native_render_auth_proof import AuthProofError, JsonObject


INCOMPLETE_REASONS = {
    1: "disabled", 2: "invalid_config", 3: "missing_game_digest", 4: "producer_absent",
    5: "invalid_ot", 6: "cyclic_ot", 7: "overflow", 8: "unsupported_display",
    9: "missing_camera_digest", 10: "incomplete_observation",
    11: "missing_visual_state", 12: "gte_overflow",
    13: "vram_serial_overflow", 14: "incomplete_fields",
}

RUNTIME_BASELINE_FIELDS = frozenset({
    "requested", "schema_version", "enabled", "complete", "overflow",
    "invalid_ot", "cyclic_ot", "reason", "field_completeness_mask",
    "required_field_mask", "visual_scene_epoch", "visual_state_sequence",
    "requested_render_mode", "effective_render_mode", "fallback_reason",
    "fallback_count", "producer_count", "producer_binding_count",
    "interpreter_calls", "native_calls", "gte_total_count",
    "gte_inside_producer_count", "gte_outside_producer_count",
    "gte_tier_counts", "gte_overflow_reason", "gte_blocked", "ot_lists",
    "ot_nodes", "ot_words", "ot_digest", "topology_digest",
    "material_samples", "material_digest", "gp0_writes", "gp1_writes",
    "vram_mutations", "global_vram_mutation_serial",
    "global_vram_serial_overflowed", "vram_digest", "gpu_digest",
    "display_samples", "display15_digest", "display_digest",
    "host_framebuffer_samples", "host_framebuffer_digest", "vblank_delta",
    "guest_cycle_delta", "cycles_per_vblank", "cycle_digest", "audio_frames",
    "audio_events", "audio_digest", "game_digest", "camera_actor_digest",
    "normalized_digest",
})


@dataclass(frozen=True, slots=True)
class BaselineError(Exception):
    reason: str
    mismatch_path: str

    def __str__(self) -> str:
        return self.reason


@dataclass(frozen=True, slots=True)
class BuildTarget:
    name: str
    executable: Path


@dataclass(frozen=True, slots=True)
class BaselineRequest:
    repository_root: Path
    trace: Path
    builds: tuple[BuildTarget, BuildTarget]
    disc: Path
    warm_cache: Path
    evidence: Path
    watchdog_seconds: int
    memcard_dir: Path | None = None


@dataclass(frozen=True, slots=True)
class BaselineChild:
    repository_root: Path
    source_build: Path
    executable: Path
    source_cache: Path | None
    trace: Path
    disc: Path
    runtime_state: Path
    cache_dir: Path
    capture_path: Path
    overlay_mode: str
    prime: bool
    memcard_dir: Path | None = None


def validate_warm_cache(source: Path) -> Path:
    try:
        root_stat = source.lstat()
        if source.is_symlink() or not stat.S_ISDIR(root_stat.st_mode):
            raise BaselineError("warm_cache_invalid", "/warm_cache")
        directories = [source]
        shared_objects: set[Path] = set()
        ranges: set[Path] = set()
        while directories:
            directory = directories.pop()
            with os.scandir(directory) as entries:
                for entry in entries:
                    entry_stat = entry.stat(follow_symlinks=False)
                    if entry.is_symlink():
                        raise BaselineError("warm_cache_invalid", "/warm_cache")
                    entry_path = Path(entry.path)
                    if stat.S_ISDIR(entry_stat.st_mode):
                        directories.append(entry_path)
                    elif stat.S_ISREG(entry_stat.st_mode):
                        relative = entry_path.relative_to(source)
                        if relative.suffix == ".so":
                            shared_objects.add(relative.with_suffix(""))
                        elif relative.suffix == ".ranges":
                            ranges.add(relative.with_suffix(""))
                    else:
                        raise BaselineError("warm_cache_invalid", "/warm_cache")
        if not shared_objects or shared_objects != ranges:
            raise BaselineError("warm_cache_invalid", "/warm_cache")
    except OSError:
        raise BaselineError("warm_cache_invalid", "/warm_cache") from None
    return source


def _mapping(value: object, path: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise BaselineError("runtime_snapshot_malformed", path)
    return value


def _integer(value: object, path: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise BaselineError("runtime_snapshot_malformed", path)
    return value


def _boolean(value: object, path: str) -> bool:
    if type(value) is not bool:
        raise BaselineError("runtime_snapshot_malformed", path)
    return value


def _digest(value: object, name: str) -> str:
    if not isinstance(value, str) or len(value) != 16 or any(character not in "0123456789abcdef" for character in value):
        raise BaselineError("runtime_digest_malformed", f"/baseline/{name}")
    return hashlib.sha256(f"xg-task5/{name}/{value}".encode("ascii")).hexdigest()


def _incomplete_reason(snapshot: dict[str, object], prefix: str) -> BaselineError:
    reason = INCOMPLETE_REASONS.get(_integer(snapshot.get("reason"), f"{prefix}/reason"), "unknown")
    return BaselineError(f"baseline_{reason}", f"{prefix}/reason")


def measured_run_from_runtime(
    runtime: dict[str, object], overlay_mode: str, elapsed_ms: int,
    expected_checkpoint: int,
) -> dict[str, object]:
    checkpoint = _mapping(runtime.get("checkpoint"), "/checkpoint")
    if checkpoint.get("field_id") != expected_checkpoint:
        raise BaselineError("checkpoint_not_reached", "/checkpoint/field_id")
    snapshot = _mapping(runtime.get("baseline"), "/baseline")
    if set(snapshot) != RUNTIME_BASELINE_FIELDS:
        raise BaselineError("runtime_snapshot_malformed", "/baseline")
    if snapshot.get("requested") is not True or snapshot.get("enabled") is not True:
        raise BaselineError("baseline_not_authenticated", "/baseline/enabled")
    if _integer(snapshot.get("schema_version"), "/baseline/schema_version") != 2:
        raise BaselineError("baseline_schema_mismatch", "/baseline/schema_version")
    if snapshot.get("complete") is not True:
        raise _incomplete_reason(snapshot, "/baseline")
    for name in ("overflow", "invalid_ot", "cyclic_ot", "gte_blocked",
                 "global_vram_serial_overflowed"):
        if _boolean(snapshot.get(name), f"/baseline/{name}") is not False:
            raise BaselineError("baseline_invalid", f"/baseline/{name}")
    if _integer(snapshot.get("reason"), "/baseline/reason") == 8:
        raise BaselineError("baseline_unsupported_display", "/baseline/reason")
    field_mask = _integer(snapshot.get("field_completeness_mask"),
                          "/baseline/field_completeness_mask")
    required_mask = _integer(snapshot.get("required_field_mask"),
                             "/baseline/required_field_mask")
    if field_mask != 0x7ff or required_mask != 0x7ff:
        raise BaselineError("baseline_incomplete_fields",
                            "/baseline/field_completeness_mask")
    tiers = snapshot.get("gte_tier_counts")
    if (not isinstance(tiers, list) or len(tiers) != 4 or
            any(type(value) is not int or value < 0 for value in tiers)):
        raise BaselineError("runtime_snapshot_malformed", "/baseline/gte_tier_counts")
    if _integer(snapshot.get("gte_overflow_reason"),
                "/baseline/gte_overflow_reason") != 0:
        raise BaselineError("baseline_gte_overflow", "/baseline/gte_overflow_reason")
    for name in ("material_samples", "host_framebuffer_samples", "ot_lists",
                 "ot_nodes", "ot_words", "display_samples"):
        _integer(snapshot.get(name), f"/baseline/{name}", 1)
    if runtime.get("status") != "PASS":
        raise BaselineError("runtime_nonpass", "/status")
    if runtime.get("render_mode") != "original":
        raise BaselineError("runtime_mode_mismatch", "/render_mode")
    if runtime.get("backend") != "opengl":
        raise BaselineError("runtime_route_mismatch", "/backend")
    for name, reason in (("game_digest", "baseline_missing_game_digest"),
                         ("camera_actor_digest", "baseline_missing_camera_digest")):
        if snapshot.get(name) == "0000000000000000":
            raise BaselineError(reason, f"/baseline/{name}")
    interpreter_calls = _integer(snapshot.get("interpreter_calls"), "/baseline/interpreter_calls")
    native_calls = _integer(snapshot.get("native_calls"), "/baseline/native_calls")
    loader = _mapping(runtime.get("loader"), "/baseline/loader")
    if overlay_mode == "cold":
        if interpreter_calls < 1 or native_calls != 0:
            raise BaselineError("cold_execution_mismatch", "/baseline/native_calls")
        execution = {"mode": "interpreter", "calls": interpreter_calls}
    elif overlay_mode == "warm":
        if native_calls < 1:
            raise BaselineError("warm_execution_mismatch", "/baseline/native_calls")
        if not isinstance(loader.get("file_found"), int) or loader["file_found"] < 1 or not isinstance(loader.get("registered"), int) or loader["registered"] < 1:
            raise BaselineError("warm_loader_unavailable", "/baseline/loader")
        execution = {"mode": "native", "calls": native_calls}
    else:
        raise BaselineError("overlay_mode_invalid", "/overlay_mode")
    return {
        "status": "PASS",
        "identity": {"authenticated": True, "producer": "render-field-character-sprites"},
        "producer_execution": execution,
        "baseline": {
            "schema_version": 2,
            "complete": True,
            "overflow": False,
            "invalid_ot": False,
            "cyclic_ot": False,
            "unsupported_display": False,
            "field_completeness_mask": field_mask,
            "required_field_mask": required_mask,
            "visual_state": {
                "scene_epoch": _integer(snapshot.get("visual_scene_epoch"), "/baseline/visual_scene_epoch", 1),
                "state_sequence": _integer(snapshot.get("visual_state_sequence"), "/baseline/visual_state_sequence"),
            },
            "render_modes": {
                "requested": _integer(snapshot.get("requested_render_mode"), "/baseline/requested_render_mode"),
                "effective": _integer(snapshot.get("effective_render_mode"), "/baseline/effective_render_mode"),
            },
            "fallback": {
                "reason": _integer(snapshot.get("fallback_reason"), "/baseline/fallback_reason"),
                "count": _integer(snapshot.get("fallback_count"), "/baseline/fallback_count"),
            },
            "producers": {
                "count": _integer(snapshot.get("producer_count"), "/baseline/producer_count"),
                "bindings": _integer(snapshot.get("producer_binding_count"), "/baseline/producer_binding_count"),
            },
            "gte": {
                "total": _integer(snapshot.get("gte_total_count"), "/baseline/gte_total_count"),
                "inside_producer": _integer(snapshot.get("gte_inside_producer_count"), "/baseline/gte_inside_producer_count"),
                "outside_producer": _integer(snapshot.get("gte_outside_producer_count"), "/baseline/gte_outside_producer_count"),
                "tiers": tiers,
            },
            "ot": {
                "lists": _integer(snapshot.get("ot_lists"), "/baseline/ot_lists", 1),
                "nodes": _integer(snapshot.get("ot_nodes"), "/baseline/ot_nodes", 1),
                "words": _integer(snapshot.get("ot_words"), "/baseline/ot_words", 1),
            },
            "material_samples": _integer(snapshot.get("material_samples"), "/baseline/material_samples", 1),
            "gpu_counters": {
                "gp0_writes": _integer(snapshot.get("gp0_writes"), "/baseline/gp0_writes"),
                "gp1_writes": _integer(snapshot.get("gp1_writes"), "/baseline/gp1_writes"),
                "vram_mutations": _integer(snapshot.get("vram_mutations"), "/baseline/vram_mutations"),
                "global_vram_mutation_serial": _integer(snapshot.get("global_vram_mutation_serial"), "/baseline/global_vram_mutation_serial"),
            },
            "display_samples": _integer(snapshot.get("display_samples"), "/baseline/display_samples", 1),
            "host_framebuffer_samples": _integer(snapshot.get("host_framebuffer_samples"), "/baseline/host_framebuffer_samples", 1),
            "camera_actor_digest": _digest(snapshot.get("camera_actor_digest"), "camera_actor_digest"),
            "ot_digest": _digest(snapshot.get("ot_digest"), "ot_digest"),
            "topology_digest": _digest(snapshot.get("topology_digest"), "topology_digest"),
            "material_digest": _digest(snapshot.get("material_digest"), "material_digest"),
            "vram_digest": _digest(snapshot.get("vram_digest"), "vram_digest"),
            "gpu_digest": _digest(snapshot.get("gpu_digest"), "gpu_digest"),
            "display15_digest": _digest(snapshot.get("display15_digest"), "display15_digest"),
            "display_digest": _digest(snapshot.get("display_digest"), "display_digest"),
            "host_framebuffer_digest": _digest(snapshot.get("host_framebuffer_digest"), "host_framebuffer_digest"),
            "cycle_digest": _digest(snapshot.get("cycle_digest"), "cycle_digest"),
            "audio_digest": _digest(snapshot.get("audio_digest"), "audio_digest"),
            "game_digest": _digest(snapshot.get("game_digest"), "game_digest"),
            "normalized_digest": _digest(snapshot.get("normalized_digest"), "normalized_digest"),
            "duration": {
                "guest_vblanks": _integer(snapshot.get("vblank_delta"), "/baseline/vblank_delta", 1),
                "guest_cycles": _integer(snapshot.get("guest_cycle_delta"), "/baseline/guest_cycle_delta", 1),
                "cycles_per_vblank": _integer(snapshot.get("cycles_per_vblank"), "/baseline/cycles_per_vblank", 1),
                "audio_samples": _integer(snapshot.get("audio_frames"), "/baseline/audio_frames", 1),
                "audio_events": _integer(snapshot.get("audio_events"), "/baseline/audio_events"),
            },
        },
        "host": {"elapsed_ms": elapsed_ms},
    }


def _stage_executable(child: BaselineChild) -> None:
    child.executable.parent.mkdir(parents=True, exist_ok=True)
    source_build = child.source_build.resolve()
    shutil.copy2(source_build, child.executable)
    bios = source_build.parent / "bios"
    staged_bios = child.executable.parent / "bios"
    if bios.is_dir() and not staged_bios.exists() and not staged_bios.is_symlink():
        os.symlink(bios, staged_bios, target_is_directory=True)
    if child.source_cache is not None and child.overlay_mode == "warm" and child.prime:
        shutil.copytree(validate_warm_cache(child.source_cache), child.cache_dir)


def _root_memcard_dir(child: BaselineChild) -> Path:
    memcard_dir = child.memcard_dir or child.repository_root
    if memcard_dir.resolve() != child.repository_root.resolve():
        raise BaselineError("root_cards_invalid", "/cards")
    try:
        for name in ("card1.mcd", "card2.mcd"):
            source = memcard_dir / name
            source_stat = source.lstat()
            if stat.S_ISLNK(source_stat.st_mode) or not stat.S_ISREG(source_stat.st_mode):
                raise BaselineError("root_cards_invalid", "/cards")
    except OSError:
        raise BaselineError("root_cards_invalid", "/cards") from None
    return memcard_dir


def execute_runtime_child(child: BaselineChild, watchdog_seconds: int) -> dict[str, object]:
    _stage_executable(child)
    child.runtime_state.mkdir(parents=True, exist_ok=False)
    memcard_dir = _root_memcard_dir(child)
    runtime_evidence = child.runtime_state / "runtime-evidence.json"
    command = (
        str(child.executable), "--no-launcher", "--game", str(child.repository_root / "game.toml"),
        "--bios", str(child.repository_root / "game" / "SCPH1001.BIN"),
        "--runtime-state", str(child.runtime_state), "--memcard-dir", str(memcard_dir),
        "--renderer", "opengl", "--input-replay", str(child.trace), "--evidence-out", str(runtime_evidence),
        "--disc", str(child.disc),
    )
    environment = _runtime_environment()
    environment["PSX_OVERLAY_CAPTURES"] = str(child.capture_path)
    environment["PSX_OVERLAY_AUTOCOMPILE_OFF"] = "1"
    process = Popen(command, cwd=child.repository_root, stdout=PIPE, stderr=PIPE, text=True,
                    start_new_session=True, env=environment)
    try:
        stdout, stderr = process.communicate(timeout=watchdog_seconds)
    except TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise BaselineError("watchdog_timeout", "/process") from None
    if process.returncode != 0:
        del stdout, stderr
        raise BaselineError(f"runtime_nonzero_exit_{process.returncode}", "/process")
    if not runtime_evidence.is_file():
        del stdout, stderr
        raise BaselineError("runtime_evidence_missing", "/runtime_evidence")
    payload = json.loads(runtime_evidence.read_text(encoding="utf-8"))
    private_paths = (child.repository_root, child.runtime_state, child.cache_dir,
                     child.capture_path, child.executable)
    if child.source_cache is not None:
        private_paths += (child.source_cache,)
    payload = scrub_private(payload, child.disc, *private_paths)
    return _mapping(payload, "/runtime_evidence")


def _cache_published(cache_dir: Path) -> bool:
    return any(path.is_file() and not path.is_symlink() and path.suffix in {".dll", ".so", ".dylib"} for path in cache_dir.rglob("*"))


def _blocked_run(error: BaselineError) -> dict[str, object]:
    return {"status": "BLOCKED", "reason": error.reason, "mismatch_paths": [error.mismatch_path]}


def _warm_candidate_unavailable(proof: dict[str, object]) -> bool:
    static = _mapping(proof["static"], "/auth_proof/static")
    provenance = _mapping(static["provenance"], "/auth_proof/static/provenance")
    candidate = _mapping(provenance["candidate"], "/auth_proof/static/provenance/candidate")
    return candidate["matched"] is not True or candidate["dispatched"] is not True


def _assert_prime(runtime: dict[str, object]) -> None:
    baseline = _mapping(runtime.get("baseline"), "/prime/baseline")
    if baseline.get("requested") is not True or baseline.get("enabled") is not True:
        raise BaselineError("prime_not_authenticated", "/prime/baseline/enabled")
    if baseline.get("complete") is not True:
        raise _incomplete_reason(baseline, "/prime/baseline")
    if _integer(baseline.get("native_calls"), "/prime/baseline/native_calls") < 1:
        raise BaselineError("prime_native_execution_missing", "/prime/baseline/native_calls")
    if runtime.get("status") != "PASS":
        raise BaselineError("prime_nonpass", "/prime/status")
    loader = _mapping(runtime.get("loader"), "/prime/loader")
    if (not isinstance(loader.get("active"), int) or loader["active"] < 1 or
            not isinstance(loader.get("registered"), int) or loader["registered"] < 1 or
            not isinstance(loader.get("file_found"), int) or loader["file_found"] < 1):
        raise BaselineError("prime_loader_unavailable", "/prime/loader")


def _child(request: BaselineRequest, build: BuildTarget, mode: str, root: Path, prime: bool) -> BaselineChild:
    executable = root / "exe" / build.executable.name
    phase = "prime" if prime else "measure"
    source_cache = request.warm_cache if mode == "warm" and prime else None
    return BaselineChild(request.repository_root, build.executable, executable, source_cache,
                         request.trace, request.disc, root / f"{phase}-runtime-state",
                         executable.parent / "cache", root / f"{phase}-overlay-captures.json", mode, prime,
                         request.memcard_dir)


def _row(request: BaselineRequest, build: BuildTarget, mode: str,
          execute: Callable[[BaselineChild], dict[str, object]]) -> dict[str, object]:
    expected_checkpoint = parse_trace(request.trace).checkpoint_field
    if expected_checkpoint is None:
        raise BaselineError("checkpoint_not_configured", "/checkpoint")
    before = snapshot_root_cards(request.repository_root)
    repetitions: list[dict[str, object]] = []
    mismatches: list[str] = []
    for index in range(2):
        with tempfile.TemporaryDirectory(prefix=f"xg-task5-{build.name}-{mode}-{index}-") as directory:
            root = Path(directory)
            child = _child(request, build, mode, root, mode == "warm")
            try:
                if mode == "warm":
                    _assert_prime(execute(child))
                    if not _cache_published(child.cache_dir):
                        raise BaselineError("authenticated_cache_unpublished", "/cache/publication")
                child = _child(request, build, mode, root, False)
                if mode == "cold" and _cache_published(child.cache_dir):
                    raise BaselineError("cold_cache_present", "/cache")
                started = time.monotonic_ns()
                repetitions.append(measured_run_from_runtime(
                    execute(child), mode,
                    (time.monotonic_ns() - started) // 1_000_000,
                    expected_checkpoint))
            except (BaselineError, OSError, json.JSONDecodeError) as failure:
                error = failure if isinstance(failure, BaselineError) else BaselineError("runtime_execution_error", "/process")
                repetitions.append(_blocked_run(error))
                mismatches.append(error.mismatch_path)
    after = snapshot_root_cards(request.repository_root)
    try:
        cards = public_card_integrity(before, after)
    except ValueError:
        cards = {name: {"size": before[name].size, "unchanged": before[name] == after.get(name)} for name in before}
        mismatches.append("/cards")
    status = "PASS" if not mismatches else "BLOCKED"
    row: dict[str, object] = {
        "row_id": f"{build.name}-{mode}", "build": build.name, "overlay_mode": mode, "status": status,
        "repetitions": repetitions, "cards": cards,
        "cleanup": {"runtime_state_removed": True, "process_reaped": True},
    }
    if mismatches:
        row["reason"] = "baseline_row_blocked"
        row["mismatch_paths"] = sorted(set(mismatches))
    return row


def execute_matrix(request: BaselineRequest,
                   execute: Callable[[BaselineChild], dict[str, object]] | None = None) -> dict[str, object]:
    runner = execute or (lambda child: execute_runtime_child(child, request.watchdog_seconds))
    targets = {target.name: target for target in request.builds}
    rows = [_row(request, targets[build], mode, runner) for build, mode in BASELINE_ROWS]
    status = "PASS" if all(row["status"] == "PASS" for row in rows) else "BLOCKED"
    if status == "PASS":
        reference = canonical_renderer_digest(rows[0]["repetitions"][0])
        for row_index, row in enumerate(rows):
            for repetition_index, run in enumerate(row["repetitions"]):
                if canonical_renderer_digest(run) != reference:
                    row["status"] = "BLOCKED"
                    row["reason"] = "guest_baseline_mismatch"
                    row["mismatch_paths"] = [f"/matrix/rows/{row_index}/repetitions/{repetition_index}"]
                    status = "BLOCKED"
    return {
        "schema": "xenogears.native-render-baseline/v2", "schema_version": 2, "task": 5,
        "status": status, "host_field_allowlist": list(HOST_FIELD_ALLOWLIST),
        "matrix": {"scenario": "game-producer", "rows": rows},
        "privacy": {"raw_payloads": False, "private_paths": False, "card_hashes": False},
    }


def execute_auth_proof_matrix(request: BaselineRequest) -> JsonObject:
    build_names = {str(target.executable.resolve()): target.name for target in request.builds}
    observations: dict[str, list[JsonObject]] = {
        f"{build}-{mode}": [] for build, mode in BASELINE_ROWS
    }

    def execute(child: BaselineChild) -> JsonObject:
        runtime = execute_runtime_child(child, request.watchdog_seconds)
        proof = runtime.get("auth_proof")
        if not isinstance(proof, dict):
            raise BaselineError("auth_proof_missing", "/auth_proof")
        try:
            assert_auth_proof(proof, child.overlay_mode)
        except AuthProofError as failure:
            if failure.reason == "auth proof candidate is unavailable":
                raise BaselineError(
                    "auth_proof_candidate_unavailable", "/auth_proof/static/provenance/candidate",
                ) from None
            raise BaselineError("auth_proof_invalid", "/auth_proof") from None
        if proof.get("status") != "OBSERVED":
            if child.overlay_mode == "warm" and _warm_candidate_unavailable(proof):
                raise BaselineError(
                    "auth_proof_candidate_unavailable", "/auth_proof/static/provenance/candidate",
                )
            raise BaselineError("auth_proof_unobserved", "/auth_proof/status")
        if not child.prime:
            build_name = build_names.get(str(child.source_build.resolve()))
            if build_name is None:
                raise BaselineError("auth_proof_build_unknown", "/build")
            observations[f"{build_name}-{child.overlay_mode}"].append(proof)
        return runtime

    baseline = execute_matrix(request, execute)
    baseline_rows = {
        row["row_id"]: row for row in baseline["matrix"]["rows"]
        if isinstance(row, dict) and isinstance(row.get("row_id"), str)
    }
    rows: list[JsonObject] = []
    for build, mode in BASELINE_ROWS:
        row_id = f"{build}-{mode}"
        proofs = observations[row_id]
        baseline_row = baseline_rows.get(row_id)
        observed = isinstance(baseline_row, dict) and baseline_row.get("status") == "PASS" and len(proofs) == 2
        reason: str | None = None
        mismatch_paths: list[str] = []
        if not observed:
            if isinstance(baseline_row, dict) and isinstance(baseline_row.get("mismatch_paths"), list):
                mismatch_paths = [path for path in baseline_row["mismatch_paths"] if isinstance(path, str)]
            if not mismatch_paths:
                mismatch_paths = ["/auth_proof"]
            if "/auth_proof/static/provenance/candidate" in mismatch_paths:
                reason = "auth_proof_candidate_unavailable"
            elif isinstance(baseline_row, dict) and isinstance(baseline_row.get("reason"), str):
                reason = baseline_row["reason"]
            else:
                reason = "auth_proof_observation_incomplete"
        rows.append({
            "row_id": row_id,
            "build": build,
            "overlay_mode": mode,
            "status": "OBSERVED" if observed else "BLOCKED",
            "observations": proofs,
            "reason": reason,
            "mismatch_paths": mismatch_paths,
        })
    status = "PASS" if all(row["status"] == "OBSERVED" for row in rows) else "BLOCKED"
    receipt: JsonObject = {
        "schema": AUTH_PROOF_MATRIX_SCHEMA,
        "schema_version": 1,
        "task": 9,
        "status": status,
        "matrix": {"scenario": "game-producer", "rows": rows},
        "privacy": AUTH_PROOF_PRIVACY,
    }
    assert_auth_proof_matrix(receipt)
    return receipt
