from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import tomllib
from typing import Final

from native_render_auth_proof import (
    AUTH_PROOF_MATRIX_SCHEMA,
    AUTH_PROOF_PRIVACY,
    AUTH_PROOF_SCHEMA,
    assert_auth_proof,
    assert_auth_proof_matrix,
)


__all__ = (
    "AUTH_PROOF_MATRIX_SCHEMA",
    "AUTH_PROOF_PRIVACY",
    "AUTH_PROOF_SCHEMA",
    "assert_auth_proof",
    "assert_auth_proof_matrix",
)


SCHEMA: Final = "xenogears.native-render-replay/v1"
REPOSITORY_ROOT: Final = Path(__file__).resolve().parents[1]
DEFAULT_BIOS: Final = REPOSITORY_ROOT / "game" / "SCPH1001.BIN"
PROHIBITED: Final = ("--netplay", "press", "set_input", "teleport", "savestate", "write_ram")
BASELINE_SCHEMA: Final = "xenogears.native-render-baseline/v2"
BASELINE_ROWS: Final = (("debug", "cold"), ("debug", "warm"),
                        ("release", "cold"), ("release", "warm"))
HOST_FIELD_ALLOWLIST: Final = ("host.elapsed_ms",)
TASK15_MATRIX_SCHEMA: Final = "xenogears.native-render-task15-matrix/v1"
P0_MODE_MATRIX_SCHEMA: Final = "xenogears.native-render-p0-mode-matrix/v1"
P0_BASELINE_FIELDS: Final = (
    "complete", "overflow", "invalid_ot", "cyclic_ot",
    "field_completeness_mask", "required_field_mask", "visual_scene_epoch",
    "visual_state_sequence", "requested_render_mode", "effective_render_mode",
    "fallback_reason", "fallback_count", "producer_count",
    "producer_binding_count", "interpreter_calls", "native_calls",
    "gte_total_count", "gte_inside_producer_count",
    "gte_outside_producer_count", "gte_tier_counts", "gte_overflow_reason",
    "gte_blocked", "ot_lists", "ot_nodes", "ot_words", "ot_digest",
    "topology_digest", "material_samples", "material_digest", "gp0_writes",
    "gp1_writes", "vram_mutations", "global_vram_mutation_serial",
    "global_vram_serial_overflowed", "vram_digest", "gpu_digest",
    "display_samples", "display15_digest", "display_digest",
    "host_framebuffer_samples", "host_framebuffer_digest", "vblank_delta",
    "guest_cycle_delta", "cycles_per_vblank", "cycle_digest", "game_digest",
    "camera_actor_digest",
)
P0_EQUIVALENCE_FIELDS: Final = (
    "complete", "overflow", "invalid_ot", "cyclic_ot",
    "field_completeness_mask", "required_field_mask", "ot_lists", "ot_nodes",
    "ot_words", "ot_digest", "topology_digest", "material_samples",
    "material_digest", "gp0_writes", "gp1_writes", "vram_mutations",
    "global_vram_mutation_serial", "global_vram_serial_overflowed",
    "vram_digest", "gpu_digest", "display_samples", "display15_digest",
    "display_digest", "host_framebuffer_samples", "host_framebuffer_digest",
    "vblank_delta", "guest_cycle_delta", "cycles_per_vblank", "cycle_digest",
    "game_digest", "camera_actor_digest",
)
# GTE attribution remains in the baseline evidence, but it is diagnostic-only
# for the pre-GTE Native render gate.
P0_ORACLE_ATTRIBUTION_FIELDS: Final = ()


@dataclass(frozen=True, slots=True)
class PadState:
    buttons: tuple[str, ...] = ()
    left_x: int = 0
    left_y: int = 0
    right_x: int = 0
    right_y: int = 0


@dataclass(frozen=True, slots=True)
class TraceState:
    p1: PadState
    p2: PadState


@dataclass(frozen=True, slots=True)
class Trace:
    vblank_budget: int
    states: tuple[TraceState, ...]
    checkpoint_field: int | None
    record_on_close: bool


@dataclass(slots=True)
class ReplayCounters:
    vblank_latches: int = 0
    trace_state_latches: int = 0
    port_reads: int = 0


class ReplayCursor:
    def __init__(self, trace: Trace) -> None:
        self.trace = trace
        self.counters = ReplayCounters()
        self._index = 0
        self._current: TraceState | None = None
        self.stop_reason: str | None = None

    def latch_vblank(self) -> TraceState | None:
        if self._index >= self.trace.vblank_budget or self._index >= len(self.trace.states):
            self.stop_reason = (
                "trace_complete" if self.trace.record_on_close and
                self._index == self.trace.vblank_budget == len(self.trace.states)
                else "checkpoint_not_reached"
            )
            return None
        self._current = self.trace.states[self._index]
        self._index += 1
        self.counters.vblank_latches += 1
        self.counters.trace_state_latches += 1
        return self._current

    def read_port(self, port: int) -> PadState:
        assert self._current is not None
        assert port in (1, 2)
        self.counters.port_reads += 1
        return self._current.p1 if port == 1 else self._current.p2


@dataclass(frozen=True, slots=True)
class RunRequest:
    build: Path
    trace: Path
    runtime_state: Path
    memcard_dir: Path
    evidence: Path
    renderer: str
    disc: Path | None = None
    timing_mode: str = "original"
    render_mode: str = "original"
    overlay_mode: str = "cold"
    bios: Path | None = DEFAULT_BIOS
    baseline_request: bool = False


@dataclass(frozen=True, slots=True)
class RecordRequest:
    build: Path
    trace: Path
    runtime_state: Path
    memcard_dir: Path
    renderer: str
    disc: Path
    max_vblanks: int
    on_close: bool = False


@dataclass(frozen=True, slots=True)
class RecordArgumentError(Exception):
    message: str

    def __str__(self) -> str:
        return self.message


@dataclass(frozen=True, slots=True)
class CardSnapshot:
    size: int
    digest: bytes


def snapshot_root_cards(root: Path) -> dict[str, CardSnapshot]:
    snapshots: dict[str, CardSnapshot] = {}
    for name in ("card1.mcd", "card2.mcd"):
        card = root / name
        if not card.is_file() or card.is_symlink():
            raise ValueError("root memory cards are required")
        contents = card.read_bytes()
        snapshots[name.removesuffix(".mcd")] = CardSnapshot(len(contents), hashlib.sha256(contents).digest())
    return snapshots


def public_card_integrity(before: dict[str, CardSnapshot], after: dict[str, CardSnapshot]) -> dict[str, dict[str, int | bool]]:
    if set(before) != {"card1", "card2"} or set(after) != set(before):
        raise ValueError("root memory cards are required")
    public: dict[str, dict[str, int | bool]] = {}
    for name in ("card1", "card2"):
        earlier = before[name]
        later = after[name]
        if earlier != later:
            raise ValueError("memory card changed")
        public[name] = {"size": earlier.size, "unchanged": True}
    return public


def canonical_renderer_digest(run: dict[str, object]) -> str:
    normalized = {key: value for key, value in run.items() if key != "host"}
    execution = normalized.get("producer_execution")
    if isinstance(execution, dict):
        normalized["producer_execution"] = {key: value for key, value in execution.items() if key != "mode"}
    baseline = normalized.get("baseline")
    if isinstance(baseline, dict):
        normalized_baseline = {key: value for key, value in baseline.items() if key != "audio_digest"}
        normalized_baseline.pop("normalized_digest", None)
        gte = normalized_baseline.get("gte")
        if isinstance(gte, dict):
            normalized_baseline["gte"] = {key: value for key, value in gte.items() if key != "tiers"}
        duration = normalized_baseline.get("duration")
        if isinstance(duration, dict):
            normalized_baseline["duration"] = {
                key: value for key, value in duration.items()
                if key not in {"audio_samples", "audio_events"}
            }
        normalized["baseline"] = normalized_baseline
    encoded = json.dumps(normalized, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    return hashlib.sha256(encoded).hexdigest()


def compare_measured_runs(first: dict[str, object], second: dict[str, object]) -> None:
    if canonical_renderer_digest(first) != canonical_renderer_digest(second):
        raise ValueError("guest baseline mismatch")


def _mapping(value: object, message: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise ValueError(message)
    return value


def _required_digest(value: object, name: str) -> None:
    if not isinstance(value, str) or len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
        raise ValueError(f"baseline {name} is invalid")


def _assert_measured_run(run: dict[str, object], overlay_mode: str) -> None:
    if set(run) != {"status", "identity", "producer_execution", "baseline", "host"}:
        raise ValueError("measured run is not closed")
    if run.get("status") != "PASS":
        raise ValueError("measured run did not pass")
    identity = _mapping(run.get("identity"), "identity is missing")
    if set(identity) != {"authenticated", "producer"} or identity.get("authenticated") is not True or not isinstance(identity.get("producer"), str):
        raise ValueError("producer identity is not authenticated")
    execution = _mapping(run.get("producer_execution"), "producer execution is missing")
    expected_mode = "interpreter" if overlay_mode == "cold" else "native"
    if set(execution) != {"mode", "calls"} or execution.get("mode") != expected_mode or not isinstance(execution.get("calls"), int) or execution["calls"] < 1:
        raise ValueError(f"{overlay_mode} producer execution proof is invalid")
    baseline = _mapping(run.get("baseline"), "baseline is missing")
    expected_baseline = {
        "schema_version", "complete", "overflow", "invalid_ot", "cyclic_ot",
        "unsupported_display", "field_completeness_mask", "required_field_mask",
        "visual_state", "render_modes", "fallback", "producers", "gte", "ot",
        "material_samples", "gpu_counters", "display_samples",
        "host_framebuffer_samples", "camera_actor_digest", "ot_digest",
        "topology_digest", "material_digest", "vram_digest", "gpu_digest",
        "display15_digest", "display_digest", "host_framebuffer_digest",
        "cycle_digest", "audio_digest", "game_digest", "normalized_digest",
        "duration",
    }
    if set(baseline) != expected_baseline:
        raise ValueError("baseline fields are not closed")
    if baseline.get("complete") is not True:
        raise ValueError("baseline capture is incomplete")
    for key, message in (("overflow", "overflow"), ("invalid_ot", "invalid OT"),
                         ("cyclic_ot", "cyclic OT"),
                         ("unsupported_display", "unsupported display")):
        if baseline.get(key) is not False:
            raise ValueError(message)
    if baseline.get("schema_version") != 2 or baseline.get("field_completeness_mask") != 0x7ff or baseline.get("required_field_mask") != 0x7ff:
        raise ValueError("baseline schema fields are invalid")
    nested_fields = {
        "visual_state": {"scene_epoch", "state_sequence"},
        "render_modes": {"requested", "effective"},
        "fallback": {"reason", "count"},
        "producers": {"count", "bindings"},
        "gte": {"total", "inside_producer", "outside_producer", "tiers"},
        "ot": {"lists", "nodes", "words"},
        "gpu_counters": {"gp0_writes", "gp1_writes", "vram_mutations",
                         "global_vram_mutation_serial"},
    }
    for name, fields in nested_fields.items():
        value = _mapping(baseline.get(name), f"baseline {name} is missing")
        if set(value) != fields:
            raise ValueError(f"baseline {name} is not closed")
    tiers = _mapping(baseline.get("gte"), "baseline gte is missing").get("tiers")
    if (not isinstance(tiers, list) or len(tiers) != 4 or
            any(type(value) is not int or value < 0 for value in tiers)):
        raise ValueError("baseline gte tiers are invalid")
    for name in ("material_samples", "display_samples", "host_framebuffer_samples"):
        if type(baseline.get(name)) is not int or baseline[name] < 1:
            raise ValueError(f"baseline {name} is invalid")
    for key in ("camera_actor_digest", "ot_digest", "topology_digest",
                "material_digest", "vram_digest", "gpu_digest",
                "display15_digest", "display_digest", "host_framebuffer_digest",
                "cycle_digest", "audio_digest", "game_digest", "normalized_digest"):
        _required_digest(baseline.get(key), key)
    duration = _mapping(baseline.get("duration"), "duration is missing")
    if set(duration) != {"guest_vblanks", "guest_cycles", "cycles_per_vblank", "audio_samples", "audio_events"}:
        raise ValueError("duration fields are not closed")
    for key in ("guest_vblanks", "guest_cycles", "cycles_per_vblank", "audio_samples"):
        if not isinstance(duration.get(key), int) or duration[key] < 1:
            raise ValueError("duration is invalid")
    if type(duration.get("audio_events")) is not int or duration["audio_events"] < 0:
        raise ValueError("duration is invalid")
    host = _mapping(run.get("host"), "host fields are missing")
    if set(host) != {"elapsed_ms"} or not isinstance(host.get("elapsed_ms"), int) or host["elapsed_ms"] < 0:
        raise ValueError("host fields are invalid")


def assert_baseline_evidence(evidence: dict[str, object]) -> None:
    if set(evidence) != {"schema", "schema_version", "task", "status", "host_field_allowlist", "matrix", "privacy"}:
        raise ValueError("baseline schema is not closed")
    if evidence.get("schema") != BASELINE_SCHEMA or evidence.get("schema_version") != 2 or evidence.get("task") != 5:
        raise ValueError("baseline schema is invalid")
    if evidence.get("host_field_allowlist") != list(HOST_FIELD_ALLOWLIST):
        raise ValueError("host allowlist is invalid")
    privacy = _mapping(evidence.get("privacy"), "privacy is missing")
    if privacy != {"raw_payloads": False, "private_paths": False, "card_hashes": False}:
        raise ValueError("privacy policy is invalid")
    matrix = _mapping(evidence.get("matrix"), "matrix is missing")
    if set(matrix) != {"scenario", "rows"} or matrix.get("scenario") != "field5-natural":
        raise ValueError("baseline matrix is invalid")
    rows = matrix.get("rows")
    if not isinstance(rows, list) or len(rows) != len(BASELINE_ROWS):
        raise ValueError("baseline matrix requires four rows")
    actual_rows = {(row.get("build"), row.get("overlay_mode")) for row in (_mapping(item, "matrix row is invalid") for item in rows)}
    if actual_rows != set(BASELINE_ROWS):
        raise ValueError("baseline matrix rows are invalid")
    all_runs: list[dict[str, object]] = []
    for row_value in rows:
        row = _mapping(row_value, "matrix row is invalid")
        if set(row) != {"row_id", "build", "overlay_mode", "status", "repetitions", "cards", "cleanup"}:
            raise ValueError("matrix row is not closed")
        repetitions = row.get("repetitions")
        if not isinstance(repetitions, list) or len(repetitions) != 2:
            raise ValueError("matrix row requires two repetitions")
        runs = [_mapping(value, "measured run is invalid") for value in repetitions]
        _assert_measured_run(runs[0], row["overlay_mode"])
        _assert_measured_run(runs[1], row["overlay_mode"])
        compare_measured_runs(runs[0], runs[1])
        all_runs.extend(runs)
        cards = _mapping(row.get("cards"), "card integrity is missing")
        if set(cards) != {"card1", "card2"}:
            raise ValueError("card integrity is incomplete")
        for card in cards.values():
            public = _mapping(card, "card integrity is malformed")
            if set(public) != {"size", "unchanged"} or not isinstance(public.get("size"), int) or public.get("unchanged") is not True:
                raise ValueError("card privacy or integrity is invalid")
        cleanup = _mapping(row.get("cleanup"), "cleanup is missing")
        if cleanup != {"runtime_state_removed": True, "process_reaped": True}:
            raise ValueError("cleanup is incomplete")
    if evidence.get("status") != "PASS":
        raise ValueError("baseline did not pass")
    reference = canonical_renderer_digest(all_runs[0])
    for run in all_runs[1:]:
        if canonical_renderer_digest(run) != reference:
            raise ValueError("guest baseline mismatch")


def _pad(entry: dict[str, object], port: int) -> PadState:
    prefix = f"p{port}_"
    raw_buttons = entry.get(f"{prefix}buttons", [])
    if not isinstance(raw_buttons, list) or not all(isinstance(button, str) for button in raw_buttons):
        raise ValueError(f"{prefix}buttons must be an array of SDL controller button names")
    values = tuple(raw_buttons)
    axes = tuple(entry.get(f"{prefix}{axis}", 0) for axis in ("left_x", "left_y", "right_x", "right_y"))
    if not all(isinstance(axis, int) and -32768 <= axis <= 32767 for axis in axes):
        raise ValueError(f"p{port} axes must be signed 16-bit integers")
    return PadState(values, *axes)


def parse_trace(path: Path) -> Trace:
    with path.open("rb") as source:
        document = tomllib.load(source)
    schema = document.get("schema")
    if schema not in (SCHEMA, "xenogears.native-render-replay/v2", "xenogears.native-render-replay/v3"):
        raise ValueError("trace schema must be xenogears.native-render-replay/v1")
    if schema in ("xenogears.native-render-replay/v2", "xenogears.native-render-replay/v3"):
        checkpoint_fields = {
            "schema", "complete", "vblank_budget", "record_stop_field",
            "record_stable_vblanks", "checkpoint", "vblank",
        }
        if document.get("baseline") is True:
            checkpoint_fields.add("baseline")
        close_fields = {
            "schema", "complete", "vblank_budget", "record_on_close", "vblank",
        }
        checkpoint_variant = set(document) == checkpoint_fields
        close_variant = (
            schema == "xenogears.native-render-replay/v3" and
            set(document) == close_fields and document.get("record_on_close") is True
        )
        if (not checkpoint_variant and not close_variant) or document.get("complete") is not True:
            raise ValueError("trace v2 must be complete and use the closed record schema")
        if checkpoint_variant:
            checkpoint = document.get("checkpoint")
            if (not isinstance(checkpoint, dict) or set(checkpoint) != {"kind", "address", "equals"} or
                    checkpoint.get("kind") != "u16" or
                    checkpoint.get("address") != "0x8006F94E" or
                    checkpoint.get("equals") != document.get("record_stop_field") or
                    document.get("record_stable_vblanks") != 4):
                raise ValueError("trace checkpoint metadata is invalid")
    budget = document.get("vblank_budget")
    entries = document.get("vblank")
    if not isinstance(budget, int) or budget < 1:
        raise ValueError("vblank_budget must be positive")
    if not isinstance(entries, list) or not entries:
        raise ValueError("trace requires at least one [[vblank]] entry")
    states: list[TraceState] = []
    for entry in entries:
        if not isinstance(entry, dict):
            raise ValueError("each [[vblank]] entry must be a table")
        repeat = entry.get("repeat", 1)
        if not isinstance(repeat, int) or repeat < 1:
            raise ValueError("vblank.repeat must be positive")
        state = TraceState(_pad(entry, 1), _pad(entry, 2))
        states.extend(state for _ in range(repeat))
        if len(states) > budget:
            raise ValueError("trace states exceed guest vblank budget")
    checkpoint = document.get("checkpoint")
    checkpoint_field = checkpoint.get("equals") if isinstance(checkpoint, dict) else None
    record_on_close = document.get("record_on_close") is True
    if record_on_close and len(states) != budget:
        raise ValueError("close-record trace must exhaust its declared budget")
    return Trace(budget, tuple(states), checkpoint_field, record_on_close)


def validate_disc(path: Path) -> Path:
    disc = path.resolve()
    if disc.suffix.lower() not in (".cue", ".bin", ".iso") or not disc.is_file():
        raise ValueError("disc must be an existing regular .cue, .bin, or .iso file")
    return disc


def validate_memcard_dir(path: Path) -> Path:
    memcard_dir = path.resolve()
    if not memcard_dir.is_dir():
        raise ValueError("memcard-dir must be an existing directory")
    cards = [memcard_dir / "card1.mcd", memcard_dir / "card2.mcd"]
    if not any(card.is_file() and not card.is_symlink() for card in cards):
        raise ValueError("memcard-dir must contain card1.mcd and/or card2.mcd")
    return memcard_dir


def scrub_private(value: object, disc: Path | None, *local_paths: Path) -> object:
    if isinstance(value, str):
        if disc is not None:
            value = value.replace(str(disc.resolve()), "<private-disc>")
        for path in local_paths:
            value = value.replace(str(path.resolve()), "<private-local-path>")
        return value
    if isinstance(value, list):
        return [scrub_private(item, disc, *local_paths) for item in value]
    if isinstance(value, dict):
        return {key: scrub_private(item, disc, *local_paths) for key, item in value.items()}
    return value


def bounded_diagnostic(prefix: str, stdout: str, stderr: str, disc: Path | None) -> str:
    del stdout, stderr, disc
    return prefix


def runtime_command(request: RunRequest, runtime_evidence: Path | None = None) -> tuple[str, ...]:
    build = request.build.resolve()
    runtime_state = request.runtime_state.resolve()
    memcard_dir = request.memcard_dir.resolve()
    trace = request.trace.resolve()
    runtime_output = (runtime_evidence or request.runtime_state / "runtime-evidence.json").resolve()
    bios = request.bios if request.bios is not None else DEFAULT_BIOS
    bios_arguments = ("--bios", str(bios.resolve()))
    game_arguments = ("--game", str((REPOSITORY_ROOT / "game.toml").resolve()))
    disc_arguments = ("--disc", str(request.disc)) if request.disc is not None else ()
    command = (
        str(build), *bios_arguments, *game_arguments, "--no-launcher",
        "--runtime-state", str(runtime_state),
        "--memcard-dir", str(memcard_dir), "--renderer", request.renderer,
        "--native-fps", request.timing_mode, "--render-mode", request.render_mode,
        "--input-replay", str(trace), "--evidence-out", str(runtime_output), *disc_arguments)
    assert not any(token in command for token in PROHIBITED)
    return command


def runtime_record_command(request: RecordRequest) -> tuple[str, ...]:
    memcard_dir = request.memcard_dir.resolve()
    completion_arguments = ("--record-on-close",) if request.on_close else (
        "--record-stop-field", "5")
    command = (
        str(request.build.resolve()), "--game", str((REPOSITORY_ROOT / "game.toml").resolve()),
        "--no-launcher", "--runtime-state", str(request.runtime_state.resolve()),
        "--memcard-dir", str(memcard_dir), "--renderer", request.renderer,
        "--input-record", str(request.trace.resolve()), *completion_arguments,
        "--record-max-vblanks", str(request.max_vblanks), "--disc", str(request.disc),
    )
    assert not any(token in command for token in PROHIBITED)
    return command


def assert_run_evidence(
    run: dict[str, object], trace: Trace, render_mode: str = "original",
    require_post_checkpoint_cross: bool = True,
) -> None:
    if run.get("status") != "PASS":
        raise ValueError("runtime did not report PASS")
    if run.get("timing_mode") != "original" or run.get("render_mode") != render_mode:
        raise ValueError("runtime modes are invalid")
    checkpoint = run.get("checkpoint")
    if trace.record_on_close:
        if checkpoint is not None or run.get("completion") != "trace_complete":
            raise ValueError("manual trace did not complete cleanly")
    elif (not isinstance(checkpoint, dict) or
          checkpoint.get("field_id") != trace.checkpoint_field):
        raise ValueError(
            "Field ID 5 was not reached" if trace.checkpoint_field == 5
            else "configured checkpoint was not reached"
        )
    if run.get("backend") != "opengl":
        raise ValueError("effective backend is not opengl")
    native_render = run.get("native_render")
    if render_mode != "original" or native_render is not None:
        native_render = _mapping(native_render, "native render evidence is missing")
        expected_fields = {
            "requested_timing_mode", "effective_timing_mode",
             "requested_render_mode", "effective_render_mode",
             "transaction_count", "substitution_count", "stream", "ui_ot",
            "cumulative_fallback_count", "scene_fallback_count_baseline",
            "scene_fallback_count_delta", "scene_fallback_reason",
            "last_fallback_reason", "fallback_count_overflowed",
            "presentation_history",
        }
        if set(native_render) != expected_fields:
            raise ValueError("native render evidence is not closed")
        transactions = native_render.get("transaction_count")
        substitutions = native_render.get("substitution_count")
        if not isinstance(transactions, int) or not isinstance(substitutions, int):
            raise ValueError("native render counters are invalid")
        ui_ot = _mapping(native_render.get("ui_ot"), "UI OT evidence is missing")
        if set(ui_ot) != {
            "prepare_count", "completed_count", "node_count", "candidate_count",
            "prebound_count", "staged_count", "blocked_count", "last_start_address",
            "last_node_count", "last_candidate_count", "last_prebound_count",
            "last_staged_count", "last_ot_digest", "last_packet_digest",
            "last_semantic_digest", "last_environment_digest", "last_vram_serial",
            "pending", "blocked",
        }:
            raise ValueError("UI OT evidence is not closed")
        ui_ot_counts = tuple(ui_ot.get(key) for key in (
            "prepare_count", "completed_count", "node_count", "candidate_count",
            "prebound_count", "staged_count", "blocked_count", "last_start_address",
            "last_node_count", "last_candidate_count", "last_prebound_count",
            "last_staged_count", "last_ot_digest", "last_packet_digest",
            "last_semantic_digest", "last_environment_digest", "last_vram_serial",
        ))
        if (any(type(value) is not int or value < 0 for value in ui_ot_counts) or
                type(ui_ot.get("pending")) is not bool or
                type(ui_ot.get("blocked")) is not bool or
                ui_ot["completed_count"] > ui_ot["prepare_count"] or
                ui_ot["staged_count"] > ui_ot["candidate_count"] or
                ui_ot["last_staged_count"] > ui_ot["last_candidate_count"]):
            raise ValueError("UI OT evidence is invalid")
        if render_mode == "native" and (ui_ot["pending"] or ui_ot["blocked"]):
            raise ValueError("Native UI OT preparation was not cleanly closed")
        if (native_render.get("requested_timing_mode") != "original" or
                native_render.get("effective_timing_mode") != "original" or
                native_render.get("requested_render_mode") != render_mode or
                native_render.get("effective_render_mode") != render_mode):
            raise ValueError("effective render mode is invalid")
        cumulative = native_render.get("cumulative_fallback_count")
        scene_baseline = native_render.get("scene_fallback_count_baseline")
        scene_delta = native_render.get("scene_fallback_count_delta")
        scene_reason = native_render.get("scene_fallback_reason")
        last_reason = native_render.get("last_fallback_reason")
        overflowed = native_render.get("fallback_count_overflowed")
        reasons = {
            "none", "forced_original", "invalid_argument", "scene_reset",
            "nested_producer", "active_producer", "wrong_state",
            "stale_handle", "invalid_provenance", "slot_capacity",
            "counter_exhausted", "wrong_thread", "invalid_packet_address",
            "duplicate_packet_address", "duplicate_primitive_index",
            "binding_capacity", "presentation_gate", "backend_failure",
        }
        if (type(cumulative) is not int or type(scene_baseline) is not int or
                type(scene_delta) is not int or cumulative < 0 or
                scene_baseline < 0 or scene_delta < 0 or
                scene_baseline > cumulative or
                scene_delta != cumulative - scene_baseline or
                scene_reason not in reasons or last_reason not in reasons or
                type(overflowed) is not bool):
            raise ValueError("fallback telemetry is invalid")
        if overflowed:
            raise ValueError("fallback counter overflowed")
        if ((scene_delta == 0 and scene_reason != "none") or
                (scene_delta != 0 and scene_reason == "none") or
                (cumulative == 0 and last_reason != "none") or
                (cumulative != 0 and last_reason == "none") or
                (scene_delta != 0 and last_reason != scene_reason)):
            raise ValueError("fallback telemetry is incoherent")
        if scene_delta != 0:
            raise ValueError("runtime fallback was observed")
        if render_mode == "native":
            stream = _mapping(
                native_render.get("stream"),
                "Native stream evidence is missing",
            )
            if set(stream) != {
                "enabled", "staged_count", "total_staged", "total_consumed",
                "total_not_found", "total_original_draws",
                 "first_original_draw_opcode", "last_original_draw_opcode",
                 "total_parser_replay_commands", "total_parser_replay_draws",
                 "total_native_line_segments",
                 "total_shared_fmv_frames", "total_shared_fmv_pixels",
                 "last_shared_fmv_width", "last_shared_fmv_height",
                 "last_shared_fmv_depth24",
                 "total_independent_fmv_frames", "total_independent_fmv_pixels",
                 "last_independent_fmv_width", "last_independent_fmv_height",
                 "last_independent_fmv_depth24", "total_ui_ot_adapter_calls",
                  "total_guest_gp0_commands", "total_shared_vram_presents",
                   "total_native_lists", "total_native_packets",
                     "total_native_bound_packets", "total_native_state_packets",
                     "total_native_unbound_packets",
                    "total_native_producer_bound_draws",
                    "total_native_packet_derived_draws",
                   "total_native_unsupported_packets",
                   "first_native_unsupported_opcode", "last_native_unsupported_opcode",
                   "first_native_unbound_opcode", "last_native_unbound_opcode",
                   "first_native_unbound_source", "first_native_unsupported_source",
                   "first_native_unbound_pc", "first_native_unbound_function",
                   "first_native_unsupported_pc", "first_native_unsupported_function",
                   "first_native_unbound_return_address",
                   "first_native_unsupported_return_address",
                     "native_opcode_counts", "native_state_opcode_counts",
                      "native_unbound_opcode_counts",
                     "native_producer_bound_opcode_counts",
                     "native_packet_derived_opcode_counts",
                    "native_unsupported_opcode_counts",
                    "native_unbound_source_by_opcode",
                    "native_unbound_pc_by_opcode",
                   "native_unsupported_pc_by_opcode",
                   "native_unbound_return_address_by_opcode",
                    "native_unsupported_return_address_by_opcode",
                     "native_unbound_source_hotspots", "last_native_state",
                    "total_independent_vram_presents",
                   "native_claim", "native_coverage_contract",
                 "total_visual_states", "last_command_id", "last_status",
                "last_stage_status", "last_consume_status",
                "stage_failure_count", "first_stage_failure_command_id",
                "first_stage_failure_visual_id", "first_stage_failure_status",
                "total_superseded",
            }:
                raise ValueError("Native stream evidence is not closed")
            counters = tuple(stream.get(key) for key in (
                "staged_count", "total_staged", "total_consumed",
                "total_not_found", "total_original_draws",
                 "first_original_draw_opcode", "last_original_draw_opcode",
                  "total_parser_replay_commands", "total_parser_replay_draws",
                  "total_native_line_segments",
                 "total_visual_states", "last_command_id", "last_status",
                "last_stage_status", "last_consume_status",
                "stage_failure_count", "first_stage_failure_command_id",
                 "first_stage_failure_status", "total_superseded",
            ))
            fmv_counters = tuple(stream.get(key) for key in (
                "total_shared_fmv_frames", "total_shared_fmv_pixels",
                "last_shared_fmv_width", "last_shared_fmv_height",
                "total_independent_fmv_frames", "total_independent_fmv_pixels",
                "last_independent_fmv_width", "last_independent_fmv_height",
                "total_ui_ot_adapter_calls",
             "total_guest_gp0_commands", "total_shared_vram_presents",
             ))
            native_packet_counters = tuple(stream.get(key) for key in (
                "total_native_lists", "total_native_packets",
                 "total_native_bound_packets", "total_native_state_packets",
                  "total_native_unbound_packets",
                 "total_native_producer_bound_draws",
                 "total_native_packet_derived_draws",
                "total_native_unsupported_packets",
                "first_native_unsupported_opcode", "last_native_unsupported_opcode",
                "first_native_unbound_opcode", "last_native_unbound_opcode",
                "first_native_unbound_source", "first_native_unsupported_source",
                "first_native_unbound_pc", "first_native_unbound_function",
                "first_native_unsupported_pc", "first_native_unsupported_function",
                "total_independent_vram_presents",
            ))
            opcode_histograms = tuple(stream.get(key) for key in (
                 "native_opcode_counts", "native_state_opcode_counts",
                  "native_unbound_opcode_counts",
                 "native_producer_bound_opcode_counts",
                 "native_packet_derived_opcode_counts",
                 "native_unsupported_opcode_counts",
            ))
            attribution_arrays = tuple(stream.get(key) for key in (
                "native_unbound_source_by_opcode",
                "native_unbound_pc_by_opcode",
                "native_unsupported_pc_by_opcode",
                "native_unbound_return_address_by_opcode",
                "native_unsupported_return_address_by_opcode",
            ))
            opcode_values = tuple(stream.get(key) for key in (
                "first_native_unsupported_opcode", "last_native_unsupported_opcode",
                "first_native_unbound_opcode", "last_native_unbound_opcode",
            ))
            attribution_values = tuple(stream.get(key) for key in (
                "first_native_unbound_source", "first_native_unsupported_source",
                "first_native_unbound_pc", "first_native_unbound_function",
                "first_native_unsupported_pc", "first_native_unsupported_function",
                "first_native_unbound_return_address",
                "first_native_unsupported_return_address",
            ))
            if (stream.get("enabled") is not True or
                    any(type(value) is not int or value < 0 for value in counters) or
                    any(type(value) is not int or value < 0 for value in fmv_counters) or
                     any(type(value) is not int or value < 0
                         for value in native_packet_counters) or
                     any(type(value) is not int or value < 0 or value > 255
                         for value in opcode_values) or
                     any(type(value) is not int or value < 0
                         for value in attribution_values) or
                     any(not isinstance(histogram, list) or len(histogram) != 256 or
                         any(type(count) is not int or count < 0 for count in histogram)
                         for histogram in opcode_histograms) or
                     any(not isinstance(values, list) or len(values) != 256 or
                         any(type(value) is not int or value < 0 or value > 0xffffffff
                             for value in values)
                         for values in attribution_arrays) or
                     sum(opcode_histograms[0]) != stream.get("total_native_packets") or
                     sum(opcode_histograms[1]) != stream.get("total_native_state_packets") or
                     sum(opcode_histograms[2]) != stream.get("total_native_unbound_packets") or
                     sum(opcode_histograms[3]) !=
                         stream.get("total_native_producer_bound_draws") or
                     sum(opcode_histograms[4]) !=
                         stream.get("total_native_packet_derived_draws") or
                     sum(opcode_histograms[5]) != stream.get("total_native_unsupported_packets") or
                     type(stream.get("last_shared_fmv_depth24")) is not bool or
                    type(stream.get("last_independent_fmv_depth24")) is not bool or
                      stream.get("native_claim") != (
                          "packet-faithful"
                          if stream.get("total_native_packet_derived_draws")
                          else "independent"
                      ) or
                     stream.get("native_coverage_contract") !=
                         "eligible-3d-producer" or
                    stream.get("total_original_draws") != 0 or
                    stream.get("stage_failure_count") != 0 or
                    stream.get("total_not_found") != 0 or
                    stream.get("total_parser_replay_commands") != 0 or
                    stream.get("total_ui_ot_adapter_calls") != 0 or
                    stream.get("total_guest_gp0_commands") != 0 or
                     stream.get("total_shared_vram_presents") != 0 or
                     stream.get("total_shared_fmv_frames") != 0 or
                     stream.get("total_shared_fmv_pixels") != 0 or
                     stream.get("total_native_packets") !=
                          stream.get("total_native_bound_packets") +
                          stream.get("total_native_state_packets") +
                          stream.get("total_native_unbound_packets") or
                     stream.get("total_native_unsupported_packets") != 0 or
                     stream.get("total_staged") !=
                         stream.get("total_consumed") +
                          stream.get("total_superseded") +
                          stream.get("staged_count")):
                raise ValueError("Native mode telemetry is invalid")
            native_state = _mapping(
                stream.get("last_native_state"),
                "Native state stream evidence is missing",
            )
            if (set(native_state) != {
                    "sequence", "command_word", "source_word_address", "draw_mode",
                    "draw_area_left", "draw_area_top", "draw_area_right",
                    "draw_area_bottom", "draw_offset_x", "draw_offset_y",
                    "texture_window_mask_x", "texture_window_mask_y",
                    "texture_window_offset_x", "texture_window_offset_y", "dither",
                    "draw_to_display", "texture_disable", "mask_set", "mask_check",
                } or any(type(value) is not int for value in native_state.values()) or
                    native_state["sequence"] != stream.get("total_native_state_packets")):
                raise ValueError("Native state stream evidence is invalid")
            native_command_complete = (
                stream.get("total_staged", 0) >= 1 and
                stream.get("total_consumed", 0) >= 1 and
                stream.get("total_consumed", 0) <= stream.get("total_staged", 0) and
                stream.get("total_independent_vram_presents", 0) >= 1)
            native_fmv_complete = (
                stream.get("total_independent_fmv_frames", 0) >= 1 and
                stream.get("total_independent_fmv_pixels", 0) >= 1 and
                stream.get("last_independent_fmv_width", 0) >= 1 and
                stream.get("last_independent_fmv_height", 0) >= 1)
            if not native_command_complete and not native_fmv_complete:
                raise ValueError("native mode has no complete Native command or FMV stream")
            if (stream.get("first_original_draw_opcode") != 0 or
                    stream.get("last_original_draw_opcode") != 0):
                raise ValueError("native mode Original draw telemetry is incoherent")
            failure_visual = _mapping(
                stream.get("first_stage_failure_visual_id"),
                "Native stream failure identity is missing",
            )
            if (set(failure_visual) != {"scene_epoch", "state_sequence"} or
                    any(type(value) is not int or value < 0
                        for value in failure_visual.values())):
                raise ValueError("Native stream failure identity is invalid")
        elif substitutions != 0:
            raise ValueError("non-native mode staged a substitution")
        else:
            stream = _mapping(
                native_render.get("stream"),
                "Native stream evidence is missing",
            )
            if stream.get("enabled") is not False or stream.get("total_consumed") != 0:
                raise ValueError("non-native mode consumed the Native stream")
        history = _mapping(
            native_render.get("presentation_history"),
            "presentation history evidence is missing",
        )
        if set(history) != {
            "interpolation_requested", "interpolation_effective",
            "smooth_requested", "smooth_effective", "history_count",
            "quiesced", "gate_reason",
        }:
            raise ValueError("presentation history evidence is not closed")
        if render_mode in {"shadow", "native"} and (
            history.get("interpolation_effective") is not False
            or history.get("smooth_effective") is not False
            or history.get("history_count") != 0
            or history.get("quiesced") is not True
        ):
            raise ValueError("presentation history was not quiesced")
    consumed_states = len(trace.states)
    if trace.vblank_budget != consumed_states:
        raise ValueError("trace did not exhaust its declared budget")
    counters = run.get("counters")
    if not isinstance(counters, dict):
        raise ValueError("runtime counters are missing")
    latches = counters.get("vblank_latches")
    trace_state_latches = counters.get("trace_state_latches")
    if not isinstance(latches, int) or not isinstance(trace_state_latches, int):
        raise ValueError("runtime counters are missing")
    if latches != consumed_states or trace_state_latches != consumed_states:
        raise ValueError("trace did not exhaust the supplied budget")
    for key in ("provider_updates", "capture_samples", "mapping_reads", "sio_applies"):
        value = counters.get(key)
        if not isinstance(value, int) or value < latches:
            raise ValueError(f"missing replay traversal counter: {key}")
    if require_post_checkpoint_cross:
        if trace.checkpoint_field != 5:
            raise ValueError("post-checkpoint validation requires Field 5")
        replay = run.get("replay")
        sio = run.get("sio")
        if not isinstance(replay, dict) or not isinstance(sio, dict):
            raise ValueError("runtime tail evidence is missing")
        checkpoint_vblank = replay.get("checkpoint_seen_vblank")
        cross_count = sio.get("cross_count")
        cross_first = sio.get("cross_first")
        cross_last = sio.get("cross_last")
        if (not isinstance(checkpoint_vblank, int) or not isinstance(cross_count, int) or
                not isinstance(cross_first, int) or not isinstance(cross_last, int)):
            raise ValueError("runtime tail evidence is malformed")
        if checkpoint_vblank < 1 or cross_count < 1 or cross_last <= checkpoint_vblank or cross_last < cross_first:
            raise ValueError("Cross activity did not follow the Field 5 checkpoint")
    prohibited = run.get("prohibited_apis")
    if not isinstance(prohibited, dict) or any(value is not False for value in prohibited.values()):
        raise ValueError("a prohibited replay API was enabled")


def assert_duplicate_runs(
    runs: tuple[dict[str, object], dict[str, object]], trace: Trace,
    render_mode: str = "original",
) -> None:
    for run in runs:
        assert_run_evidence(
            run, trace, render_mode,
            require_post_checkpoint_cross=trace.checkpoint_field == 5)
    first, second = runs
    for key in ("backend", "guest_sequence", "counters"):
        if first.get(key) != second.get(key):
            raise ValueError(f"duplicate runs diverged: {key}")


def _runtime_environment() -> dict[str, str]:
    environment = dict(__import__("os").environ)
    environment.update({"PSX_DEV_INPUT": "0", "PSX_LOW_LATENCY_INPUT": "1"})
    return environment


def assert_task15_matrix_evidence(evidence: dict[str, object]) -> None:
    if set(evidence) != {
        "schema", "task", "status", "timing_mode", "overlay_mode",
        "privacy", "rows",
    }:
        raise ValueError("Task 15 matrix is not closed")
    if evidence.get("schema") != TASK15_MATRIX_SCHEMA or evidence.get("task") != 15:
        raise ValueError("Task 15 matrix schema is invalid")
    if (evidence.get("timing_mode") != "original" or
            evidence.get("overlay_mode") not in {"cold", "warm"}):
        raise ValueError("Task 15 matrix axes are invalid")
    if evidence.get("privacy") != {"metadata_only": True, "private_paths": False}:
        raise ValueError("Task 15 matrix privacy is invalid")
    rows = evidence.get("rows")
    if not isinstance(rows, list) or len(rows) != 3:
        raise ValueError("Task 15 matrix requires three rows")
    modes: set[str] = set()
    for value in rows:
        row = _mapping(value, "Task 15 matrix row is invalid")
        if set(row) != {"render_mode", "status", "backend", "native_render", "cleanup"}:
            raise ValueError("Task 15 matrix row is not closed")
        mode = row.get("render_mode")
        if mode not in {"original", "shadow", "native"}:
            raise ValueError("Task 15 render mode is invalid")
        modes.add(mode)
        if row.get("status") != "PASS" or row.get("backend") != "opengl":
            raise ValueError("Task 15 matrix row did not pass")
        cleanup = _mapping(row.get("cleanup"), "Task 15 cleanup is missing")
        if cleanup != {"runtime_state_removed": True, "process_reaped": True}:
            raise ValueError("Task 15 process cleanup is incomplete")
        native = _mapping(row.get("native_render"), "Task 15 native evidence is missing")
        substitutions = native.get("substitution_count")
        if mode == "native":
            stream = _mapping(native.get("stream"), "Task 15 Native stream evidence is missing")
            if (stream.get("enabled") is not True or
                     type(stream.get("total_staged")) is not int or
                      type(stream.get("total_consumed")) is not int or
                      type(stream.get("staged_count")) is not int or
                      type(stream.get("total_superseded")) is not int or
                      type(stream.get("total_not_found")) is not int or
                      type(stream.get("total_original_draws")) is not int or
                     type(stream.get("total_parser_replay_commands")) is not int or
                     type(stream.get("total_shared_fmv_frames")) is not int or
                     type(stream.get("total_ui_ot_adapter_calls")) is not int or
                     type(stream.get("total_guest_gp0_commands")) is not int or
                       type(stream.get("total_shared_vram_presents")) is not int or
                       type(stream.get("total_native_unbound_packets")) is not int or
                       type(stream.get("total_native_producer_bound_draws")) is not int or
                       type(stream.get("total_native_packet_derived_draws")) is not int or
                       type(stream.get("total_native_unsupported_packets")) is not int or
                      stream.get("native_claim") != (
                          "packet-faithful"
                          if stream.get("total_native_packet_derived_draws")
                          else "independent"
                      ) or
                     stream.get("native_coverage_contract") !=
                         "eligible-3d-producer" or
                      type(stream.get("stage_failure_count")) is not int or
                      stream["total_staged"] < 1 or
                      stream["total_consumed"] < 1 or
                      stream["total_consumed"] > stream["total_staged"] or
                      stream["staged_count"] < 0 or
                      stream["total_superseded"] < 0 or
                      stream["total_staged"] != stream["total_consumed"] +
                          stream["total_superseded"] + stream["staged_count"] or
                      stream["total_not_found"] != 0 or
                      stream["total_original_draws"] != 0 or
                     stream["total_parser_replay_commands"] != 0 or
                     stream["total_shared_fmv_frames"] != 0 or
                     stream["total_ui_ot_adapter_calls"] != 0 or
                      stream["total_guest_gp0_commands"] != 0 or
                      stream["total_shared_vram_presents"] != 0 or
                       stream["total_native_unsupported_packets"] != 0 or
                      stream["stage_failure_count"] != 0):
                raise ValueError("Task 15 native row has no complete Native stream")
        elif substitutions != 0:
            raise ValueError("Task 15 non-native row substituted")
    if modes != {"original", "shadow", "native"} or evidence.get("status") != "PASS":
        raise ValueError("Task 15 matrix is incomplete")


def p0_baseline_projection(run: dict[str, object]) -> dict[str, object]:
    baseline = _mapping(run.get("baseline"), "P0 baseline is missing")
    if any(field not in baseline for field in P0_BASELINE_FIELDS):
        raise ValueError("P0 baseline fields are incomplete")
    projected = {field: baseline[field] for field in P0_BASELINE_FIELDS}
    if (projected["complete"] is not True or
            any(projected[field] is not False for field in (
                "overflow", "invalid_ot", "cyclic_ot", "gte_blocked",
                "global_vram_serial_overflowed",
            ))):
        raise ValueError("P0 baseline did not complete cleanly")
    return projected


def p0_baseline_differences(
    left: dict[str, object], right: dict[str, object], fields: tuple[str, ...],
) -> list[str]:
    return [field for field in fields if left.get(field) != right.get(field)]


def build_p0_mode_matrix_evidence(
    rows: list[dict[str, object]],
) -> dict[str, object]:
    by_mode: dict[str, dict[str, object]] = {}
    deterministic = True
    for row in rows:
        if set(row) not in (
            {"render_mode", "runs"},
            {"render_mode", "runs", "determinism"},
        ):
            raise ValueError("P0 matrix row is not closed")
        mode = row.get("render_mode")
        runs = row.get("runs")
        if mode not in {"original", "shadow", "native"} or mode in by_mode or not isinstance(runs, list) or len(runs) != 2:
            raise ValueError("P0 matrix rows are invalid")
        baselines = []
        for value in runs:
            run = _mapping(value, "P0 run is invalid")
            if set(run) != {
                "status", "backend", "native_render", "baseline", "cleanup",
            } or run.get("status") != "PASS" or run.get("backend") != "opengl":
                raise ValueError("P0 run is not closed")
            if run.get("cleanup") != {
                "runtime_state_removed": True, "process_reaped": True,
            }:
                raise ValueError("P0 run cleanup is incomplete")
            _mapping(run.get("native_render"), "P0 native-render evidence is missing")
            baseline = _mapping(run.get("baseline"), "P0 run baseline is missing")
            if set(baseline) != set(P0_BASELINE_FIELDS):
                raise ValueError("P0 run baseline is not closed")
            baselines.append(baseline)
        differences = p0_baseline_differences(
            baselines[0], baselines[1], P0_BASELINE_FIELDS)
        native_difference = runs[0].get("native_render") != runs[1].get("native_render")
        row["determinism"] = {
            "equal": not differences and not native_difference,
            "baseline_differences": differences,
            "native_render_difference": native_difference,
        }
        deterministic = deterministic and row["determinism"]["equal"]
        by_mode[mode] = row
    if set(by_mode) != {"original", "shadow", "native"}:
        raise ValueError("P0 matrix modes are incomplete")

    original = _mapping(by_mode["original"]["runs"][0]["baseline"], "P0 original baseline is missing")
    shadow = _mapping(by_mode["shadow"]["runs"][0]["baseline"], "P0 shadow baseline is missing")
    native = _mapping(by_mode["native"]["runs"][0]["baseline"], "P0 native baseline is missing")
    comparisons = []
    for left_mode, right_mode, required, left, right, fields in (
        ("original", "shadow", True, original, shadow,
         P0_EQUIVALENCE_FIELDS + P0_ORACLE_ATTRIBUTION_FIELDS),
        ("original", "native", False, original, native,
         P0_EQUIVALENCE_FIELDS),
    ):
        differences = p0_baseline_differences(left, right, fields)
        comparisons.append({
            "left": left_mode,
            "right": right_mode,
            "required": required,
            "equal": not differences,
            "differences": differences,
        })
    status = "PASS" if deterministic and all(
        comparison["equal"] or not comparison["required"]
        for comparison in comparisons) else "BLOCKED"
    return {
        "schema": P0_MODE_MATRIX_SCHEMA,
        "phase": "P0",
        "status": status,
        "timing_mode": "original",
        "overlay_mode": "cold",
        "native_classification": "pre-gte",
        "privacy": {"metadata_only": True, "private_paths": False},
        "rows": rows,
        "comparisons": comparisons,
    }


def assert_p0_mode_matrix_evidence(evidence: dict[str, object]) -> None:
    if set(evidence) != {
        "schema", "phase", "status", "timing_mode", "overlay_mode",
        "native_classification", "privacy", "rows", "comparisons",
    }:
        raise ValueError("P0 mode matrix is not closed")
    if (evidence.get("schema") != P0_MODE_MATRIX_SCHEMA or
            evidence.get("phase") != "P0" or
            evidence.get("status") not in {"PASS", "BLOCKED"} or
            evidence.get("timing_mode") != "original" or
            evidence.get("overlay_mode") != "cold" or
             evidence.get("native_classification") != "pre-gte" or
            evidence.get("privacy") != {"metadata_only": True, "private_paths": False}):
        raise ValueError("P0 mode matrix metadata is invalid")
    rows = evidence.get("rows")
    comparisons = evidence.get("comparisons")
    if not isinstance(rows, list) or len(rows) != 3 or not isinstance(comparisons, list) or len(comparisons) != 2:
        raise ValueError("P0 mode matrix is incomplete")
    rebuilt = build_p0_mode_matrix_evidence(rows)
    if rebuilt != evidence:
        raise ValueError("P0 mode matrix verdict is inconsistent")


def _record_environment() -> dict[str, str]:
    environment = dict(__import__("os").environ)
    environment["PSX_LOW_LATENCY_INPUT"] = "1"
    return environment

if __name__ == "__main__":
    from native_render_replay_cli import main

    main()
