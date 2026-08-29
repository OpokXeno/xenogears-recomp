from __future__ import annotations

import importlib.util
from pathlib import Path
from subprocess import CompletedProcess, run
import sys
from tempfile import TemporaryDirectory
from types import ModuleType

import pytest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "native_render_replay.py"
CLI_SCRIPT = ROOT / "tools" / "native_render_replay_cli.py"


def replay_module() -> ModuleType:
    specification = importlib.util.spec_from_file_location("native_render_replay", SCRIPT)
    assert specification is not None
    assert specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def write_trace(path: Path, vblank_budget: int = 3) -> None:
    path.write_text(
        "\n".join(
            (
                'schema = "xenogears.native-render-replay/v1"',
                f"vblank_budget = {vblank_budget}",
                "[checkpoint]",
                'kind = "u16"',
                'address = "0x8006F94E"',
                "equals = 5",
                "[[vblank]]",
                "repeat = 1",
                'p1_buttons = ["a"]',
                "p1_left_x = 0",
                "p1_left_y = 0",
                "[[vblank]]",
                "repeat = 2",
                'p2_buttons = ["start"]',
            )
        ),
        encoding="utf-8",
    )


def write_close_trace(path: Path) -> None:
    lines = [
        'schema = "xenogears.native-render-replay/v3"',
        "complete = true",
        "vblank_budget = 2",
        "record_on_close = true",
    ]
    for _ in range(2):
        lines.extend((
            "[[vblank]]", "repeat = 1",
            "p1_connected = true", 'p1_mode = "digital"', "p1_buttons = []",
            "p1_left_x = 0", "p1_left_y = 0", "p1_right_x = 0", "p1_right_y = 0",
            "p1_trigger_left = 0", "p1_trigger_right = 0",
            "p2_connected = false", 'p2_mode = "digital"', "p2_buttons = []",
            "p2_left_x = 0", "p2_left_y = 0", "p2_right_x = 0", "p2_right_y = 0",
            "p2_trigger_left = 0", "p2_trigger_right = 0",
        ))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def opcode_histogram(total: int, opcode: int = 0) -> list[int]:
    values = [0] * 256
    values[opcode] = total
    return values


def opcode_attribution() -> list[int]:
    return [0xFFFFFFFF] * 256


def native_state() -> dict[str, int]:
    return {
        "sequence": 0,
        "command_word": 0,
        "source_word_address": 0,
        "draw_mode": 0,
        "draw_area_left": 0,
        "draw_area_top": 0,
        "draw_area_right": 0,
        "draw_area_bottom": 0,
        "draw_offset_x": 0,
        "draw_offset_y": 0,
        "texture_window_mask_x": 0,
        "texture_window_mask_y": 0,
        "texture_window_offset_x": 0,
        "texture_window_offset_y": 0,
        "dither": 0,
        "draw_to_display": 0,
        "texture_disable": 0,
        "mask_set": 0,
        "mask_check": 0,
    }


def write_trace_with_state_count(path: Path, vblank_budget: int, state_count: int) -> None:
    lines = [
        'schema = "xenogears.native-render-replay/v1"',
        f"vblank_budget = {vblank_budget}",
        "[checkpoint]",
        'kind = "u16"',
        'address = "0x8006F94E"',
        "equals = 5",
        "[[vblank]]",
        f"repeat = {state_count}",
        'p1_buttons = ["a"]',
        "p1_left_x = 0",
        "p1_left_y = 0",
    ]
    if state_count < vblank_budget:
        lines.extend((
            "[[vblank]]",
            f"repeat = {vblank_budget - state_count}",
            'p2_buttons = ["start"]',
        ))
    path.write_text("\n".join(lines), encoding="utf-8")


def p0_matrix_rows(replay: ModuleType) -> list[dict[str, object]]:
    modes = {"original": 0, "shadow": 1, "native": 2}
    digest_fields = {
        "ot_digest", "topology_digest", "material_digest", "vram_digest",
        "gpu_digest", "display15_digest", "display_digest",
        "host_framebuffer_digest", "cycle_digest", "game_digest",
        "camera_actor_digest",
    }
    boolean_fields = {
        "complete", "overflow", "invalid_ot", "cyclic_ot", "gte_blocked",
        "global_vram_serial_overflowed",
    }
    rows: list[dict[str, object]] = []
    for mode, mode_id in modes.items():
        baseline: dict[str, object] = {}
        for field in replay.P0_BASELINE_FIELDS:
            if field in digest_fields:
                baseline[field] = "1" * 16
            elif field == "gte_tier_counts":
                baseline[field] = [100, 0, 0, 0]
            elif field in boolean_fields:
                baseline[field] = field == "complete"
            else:
                baseline[field] = 1
        baseline["requested_render_mode"] = mode_id
        baseline["effective_render_mode"] = mode_id
        baseline["producer_binding_count"] = 1 if mode == "native" else 0
        native_render = {
            "requested_render_mode": mode,
            "effective_render_mode": mode,
            "transaction_count": 0,
            "substitution_count": 0,
            "stream": {
                "enabled": mode == "native",
                "total_staged": 4 if mode == "native" else 0,
                "total_consumed": 4 if mode == "native" else 0,
                "total_not_found": 0,
                "total_original_draws": 0,
                "first_original_draw_opcode": 0,
                "last_original_draw_opcode": 0,
                "total_parser_replay_commands": 0,
                "total_parser_replay_draws": 0,
                "total_native_line_segments": 0,
                "total_shared_fmv_frames": 0,
                "total_shared_fmv_pixels": 0,
                "last_shared_fmv_width": 0,
                "last_shared_fmv_height": 0,
                "last_shared_fmv_depth24": False,
                "total_independent_fmv_frames": 0,
                "total_independent_fmv_pixels": 0,
                "last_independent_fmv_width": 0,
                "last_independent_fmv_height": 0,
                "last_independent_fmv_depth24": False,
                "total_ui_ot_adapter_calls": 0,
                "total_guest_gp0_commands": 0,
                "total_shared_vram_presents": 0,
                "native_claim": "independent" if mode == "native" else "none",
                "stage_failure_count": 0,
            },
            "ui_ot": {
                "prepare_count": 0,
                "completed_count": 0,
                "node_count": 0,
                "candidate_count": 0,
                "prebound_count": 0,
                "staged_count": 0,
                "blocked_count": 0,
                "last_start_address": 0,
                "last_node_count": 0,
                "last_candidate_count": 0,
                "last_prebound_count": 0,
                "last_staged_count": 0,
                "last_ot_digest": 0,
                "last_packet_digest": 0,
                "last_semantic_digest": 0,
                "last_environment_digest": 0,
                "last_vram_serial": 0,
                "pending": False,
                "blocked": False,
            },
        }
        runs = []
        for _ in range(2):
            runs.append({
                "status": "PASS",
                "backend": "opengl",
                "native_render": dict(native_render),
                "baseline": dict(baseline),
                "cleanup": {
                    "runtime_state_removed": True,
                    "process_reaped": True,
                },
            })
        rows.append({"render_mode": mode, "runs": runs})
    return rows


def test_self_test_when_replay_runtime_is_unavailable() -> None:
    completed: CompletedProcess[str] = run(
        [sys.executable, str(SCRIPT), "self-test"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr
    assert "replay self-test: PASS" in completed.stdout


def test_latch_when_ports_and_second_sample_read_same_vblank() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        trace_path = Path(temporary) / "trace.toml"
        write_trace(trace_path)
        trace = replay.parse_trace(trace_path)
        cursor = replay.ReplayCursor(trace)
        first = cursor.latch_vblank()
        assert first.p1.buttons == ("a",)
        assert cursor.read_port(1) == first.p1
        assert cursor.read_port(2) == first.p2
        assert cursor.read_port(1) == first.p1
        assert cursor.counters.vblank_latches == 1
        assert cursor.counters.trace_state_latches == 1
        assert cursor.counters.port_reads == 3


def test_field_baseline_when_pad_is_digital_has_no_unintended_stick_direction() -> None:
    replay = replay_module()

    trace = replay.parse_trace(ROOT / "tools" / "native_render_replays" / "field_baseline.toml")

    assert all(
        pad.left_x == pad.left_y == pad.right_x == pad.right_y == 0
        for state in trace.states
        for pad in (state.p1, state.p2)
    )


def test_budget_when_trace_is_truncated_stops_at_guest_vblank_limit() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        trace_path = Path(temporary) / "trace.toml"
        trace_path.write_text(
            '\n'.join((
                'schema = "xenogears.native-render-replay/v1"',
                "vblank_budget = 2",
                "[[vblank]]",
            )),
            encoding="utf-8",
        )
        cursor = replay.ReplayCursor(replay.parse_trace(trace_path))
        cursor.latch_vblank()
        assert cursor.latch_vblank() is None
        assert cursor.stop_reason == "checkpoint_not_reached"
        assert cursor.counters.vblank_latches == 1


def test_manual_close_trace_completes_without_checkpoint() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        trace_path = Path(temporary) / "close.toml"
        write_close_trace(trace_path)
        trace = replay.parse_trace(trace_path)
        assert trace.record_on_close is True
        assert trace.checkpoint_field is None
        cursor = replay.ReplayCursor(trace)
        assert cursor.latch_vblank() is not None
        assert cursor.latch_vblank() is not None
        assert cursor.latch_vblank() is None
        assert cursor.stop_reason == "trace_complete"


def test_manual_close_trace_rejects_mixed_checkpoint_metadata() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        trace_path = Path(temporary) / "mixed.toml"
        write_close_trace(trace_path)
        trace_path.write_text(
            trace_path.read_text(encoding="utf-8").replace(
                "record_on_close = true\n",
                "record_on_close = true\nrecord_stop_field = 5\n",
                1,
            ),
            encoding="utf-8",
        )
        with pytest.raises(ValueError, match="closed record schema"):
            replay.parse_trace(trace_path)


def test_runtime_command_when_clean_boot_uses_only_normal_input_path() -> None:
    replay = replay_module()
    request = replay.RunRequest(
        build=Path("build-dbg/XenogearsRecomp"),
        trace=Path("tools/native_render_replays/field_clean_boot.toml"),
        runtime_state=Path(".omo/evidence/runtime-state/task-4/run-a"),
        memcard_dir=Path("memcards"),
        evidence=Path(".omo/evidence/task-4.json"),
        renderer="opengl",
    )
    command = replay.runtime_command(request)
    assert "--no-launcher" in command
    assert command[command.index("--game") + 1] == str((ROOT / "game.toml").resolve())
    assert "--input-replay" in command
    assert "--evidence-out" in command
    assert "--runtime-state" in command
    assert "--memcard-dir" in command
    assert "--renderer" in command
    forbidden = ("--netplay", "press", "set_input", "teleport", "savestate", "write_ram")
    assert not any(token in command for token in forbidden)


def test_runtime_command_when_native_render_is_requested_forwards_only_render_mode() -> None:
    replay = replay_module()
    request = replay.RunRequest(
        build=Path("build-dbg/XenogearsRecomp"),
        trace=Path("tools/native_render_replays/field_clean_boot.toml"),
        runtime_state=Path(".omo/evidence/runtime-state/task-15/native"),
        memcard_dir=Path("memcards"),
        evidence=Path(".omo/evidence/task-15.json"),
        renderer="opengl",
        render_mode="native",
        overlay_mode="cold",
    )

    command = replay.runtime_command(request)

    assert "--native-fps" not in command
    assert command[command.index("--render-mode") + 1] == "native"


def test_task15_matrix_schema_accepts_only_three_closed_metadata_rows() -> None:
    replay = replay_module()

    def native_evidence(mode: str) -> dict[str, object]:
        active = mode == "native"
        return {
            "requested_render_mode": mode,
            "effective_render_mode": mode,
            "transaction_count": 0,
            "substitution_count": 0,
            "stream": {
                "enabled": active,
                "staged_count": 0,
                "total_staged": 4 if active else 0,
                "total_consumed": 4 if active else 0,
                "total_not_found": 0,
                "total_original_draws": 0,
                "first_original_draw_opcode": 0,
                "last_original_draw_opcode": 0,
                "total_parser_replay_commands": 0,
                "total_parser_replay_draws": 0,
                "total_native_line_segments": 0,
                "total_shared_fmv_frames": 0,
                "total_shared_fmv_pixels": 0,
                "last_shared_fmv_width": 0,
                "last_shared_fmv_height": 0,
                "last_shared_fmv_depth24": False,
                "total_independent_fmv_frames": 0,
                "total_independent_fmv_pixels": 0,
                "last_independent_fmv_width": 0,
                "last_independent_fmv_height": 0,
                "last_independent_fmv_depth24": False,
                "total_ui_ot_adapter_calls": 0,
                "total_guest_gp0_commands": 0,
                 "total_shared_vram_presents": 0,
                 "total_native_lists": 1 if active else 0,
                 "total_native_packets": 4 if active else 0,
                 "total_native_bound_packets": 4 if active else 0,
                 "total_native_state_packets": 0,
                 "total_native_unbound_packets": 0,
                 "total_native_producer_bound_draws": 4 if active else 0,
                 "total_native_packet_derived_draws": 0,
                 "total_native_unsupported_packets": 0,
                 "first_native_unsupported_opcode": 0,
                 "last_native_unsupported_opcode": 0,
                 "first_native_unbound_opcode": 0,
                 "last_native_unbound_opcode": 0,
                 "first_native_unbound_source": 0,
                 "first_native_unsupported_source": 0,
                 "first_native_unbound_pc": 0,
                 "first_native_unbound_function": 0,
                 "first_native_unsupported_pc": 0,
                 "first_native_unsupported_function": 0,
                 "first_native_unbound_return_address": 0,
                 "first_native_unsupported_return_address": 0,
                 "native_opcode_counts": opcode_histogram(4 if active else 0),
                 "native_state_opcode_counts": opcode_histogram(0),
                 "native_unbound_opcode_counts": opcode_histogram(0),
                 "native_producer_bound_opcode_counts":
                     opcode_histogram(4 if active else 0),
                 "native_packet_derived_opcode_counts": opcode_histogram(0),
                 "native_unsupported_opcode_counts": opcode_histogram(0),
                 "native_unbound_source_by_opcode": opcode_attribution(),
                 "native_unbound_pc_by_opcode": opcode_attribution(),
                 "native_unsupported_pc_by_opcode": opcode_attribution(),
                 "native_unbound_return_address_by_opcode": opcode_attribution(),
                 "native_unsupported_return_address_by_opcode": opcode_attribution(),
                 "native_unbound_source_hotspots": [],
                 "last_native_state": native_state(),
                 "native_claim": "independent" if active else "none",
                 "native_coverage_contract": "eligible-3d-producer",
                "total_superseded": 0,
                "stage_failure_count": 0,
            },
            "ui_ot": {
                "prepare_count": 1 if active else 0,
                "completed_count": 1 if active else 0,
                "node_count": 1 if active else 0,
                "candidate_count": 4 if active else 0,
                "prebound_count": 0,
                "staged_count": 0,
                "blocked_count": 0,
                "last_start_address": 0,
                "last_node_count": 1 if active else 0,
                "last_candidate_count": 4 if active else 0,
                "last_prebound_count": 0,
                "last_staged_count": 4 if active else 0,
                "last_ot_digest": 0,
                "last_packet_digest": 0,
                "last_semantic_digest": 0,
                "last_environment_digest": 0,
                "last_vram_serial": 0,
                "pending": False,
                "blocked": False,
            },
            "cumulative_fallback_count": 3,
            "scene_fallback_count_baseline": 3,
            "scene_fallback_count_delta": 0,
            "scene_fallback_reason": "none",
            "last_fallback_reason": "backend_failure",
            "fallback_count_overflowed": False,
            "presentation_history": {
                "interpolation_requested": True,
                "interpolation_effective": mode == "original",
                "smooth_requested": True,
                "smooth_effective": mode == "original",
                "history_count": 0,
                "quiesced": mode != "original",
                "gate_reason": "requested_original" if mode == "original" else "none",
            },
        }

    payload = {
        "schema": replay.TASK15_MATRIX_SCHEMA,
        "task": 15,
        "status": "PASS",
        "overlay_mode": "cold",
        "privacy": {"metadata_only": True, "private_paths": False},
        "rows": [
            {
                "render_mode": mode,
                "status": "PASS",
                "backend": "opengl",
                "native_render": native_evidence(mode),
                "cleanup": {"runtime_state_removed": True, "process_reaped": True},
            }
            for mode in ("original", "shadow", "native")
        ],
    }

    replay.assert_task15_matrix_evidence(payload)
    native_stream = payload["rows"][2]["native_render"]["stream"]
    native_stream["total_consumed"] = 2
    native_stream["total_superseded"] = 1
    native_stream["staged_count"] = 1
    replay.assert_task15_matrix_evidence(payload)

    native_stream["staged_count"] = 2
    with pytest.raises(ValueError, match="no complete Native stream"):
        replay.assert_task15_matrix_evidence(payload)


def test_p0_matrix_requires_two_equal_runs_and_cross_mode_digests() -> None:
    replay = replay_module()
    payload = replay.build_p0_mode_matrix_evidence(p0_matrix_rows(replay))

    assert payload["status"] == "PASS"
    assert all(row["determinism"]["equal"] for row in payload["rows"])
    assert all(comparison["equal"] for comparison in payload["comparisons"])
    replay.assert_p0_mode_matrix_evidence(payload)


def test_p0_matrix_records_native_mismatch_as_post_gte_milestone() -> None:
    replay = replay_module()
    rows = p0_matrix_rows(replay)
    for repetition in rows[2]["runs"]:
        repetition["baseline"]["material_digest"] = "2" * 16

    payload = replay.build_p0_mode_matrix_evidence(rows)

    assert payload["status"] == "PASS"
    assert payload["rows"][2]["determinism"]["equal"] is True
    assert payload["comparisons"][1] == {
        "left": "original",
        "right": "native",
        "required": False,
        "equal": False,
        "differences": ["material_digest"],
    }
    replay.assert_p0_mode_matrix_evidence(payload)


def test_p0_matrix_when_shadow_digest_differs_is_blocked() -> None:
    replay = replay_module()
    rows = p0_matrix_rows(replay)
    for repetition in rows[1]["runs"]:
        repetition["baseline"]["material_digest"] = "2" * 16

    payload = replay.build_p0_mode_matrix_evidence(rows)

    assert payload["status"] == "BLOCKED"
    assert payload["rows"][1]["determinism"]["equal"] is True
    assert payload["comparisons"][0] == {
        "left": "original",
        "right": "shadow",
        "required": True,
        "equal": False,
        "differences": ["material_digest"],
    }
    replay.assert_p0_mode_matrix_evidence(payload)


def test_p0_matrix_when_repetitions_diverge_is_blocked() -> None:
    replay = replay_module()
    rows = p0_matrix_rows(replay)
    rows[1]["runs"][1]["baseline"]["ot_digest"] = "3" * 16

    payload = replay.build_p0_mode_matrix_evidence(rows)

    assert payload["status"] == "BLOCKED"
    assert payload["rows"][1]["determinism"] == {
        "equal": False,
        "baseline_differences": ["ot_digest"],
        "native_render_difference": False,
    }
    replay.assert_p0_mode_matrix_evidence(payload)


@pytest.mark.parametrize(
    ("render_mode", "stream_enabled", "stream_total", "fmv_frames"),
    (("native", True, 4, 0), ("native", True, 0, 2),
     ("shadow", False, 0, 0)),
)
def test_runtime_evidence_accepts_a_clean_scene_after_an_earlier_fallback(
    render_mode: str, stream_enabled: bool, stream_total: int,
    fmv_frames: int,
) -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        trace_path = Path(temporary) / "trace.toml"
        write_trace(trace_path)
        trace = replay.parse_trace(trace_path)
        native_render = {
            "requested_render_mode": render_mode,
            "effective_render_mode": render_mode,
            "transaction_count": 2,
            "substitution_count": 4 if render_mode == "native" else 0,
            "stream": {
                "enabled": stream_enabled,
                "staged_count": 0,
                "total_staged": stream_total,
                "total_consumed": stream_total,
                "total_not_found": 0,
                "total_original_draws": 0,
                "first_original_draw_opcode": 0,
                "last_original_draw_opcode": 0,
                "total_parser_replay_commands": 0,
                "total_parser_replay_draws": 0,
                "total_native_line_segments": 0,
                "total_shared_fmv_frames": 0,
                "total_shared_fmv_pixels": 0,
                "last_shared_fmv_width": 0,
                "last_shared_fmv_height": 0,
                "last_shared_fmv_depth24": False,
                "total_independent_fmv_frames": fmv_frames,
                "total_independent_fmv_pixels": fmv_frames * 320 * 240,
                "last_independent_fmv_width": 320 if fmv_frames else 0,
                "last_independent_fmv_height": 240 if fmv_frames else 0,
                "last_independent_fmv_depth24": bool(fmv_frames),
                 "total_ui_ot_adapter_calls": 0,
                 "total_guest_gp0_commands": 0,
                 "total_shared_vram_presents": 0,
                  "total_native_lists": 1 if render_mode == "native" else 0,
                  "total_native_packets": stream_total,
                   "total_native_bound_packets": stream_total,
                   "total_native_state_packets": 0,
                   "total_native_unbound_packets": 0,
                   "total_native_producer_bound_draws": stream_total,
                   "total_native_packet_derived_draws": 0,
                   "total_native_unsupported_packets": 0,
                  "first_native_unsupported_opcode": 0,
                  "last_native_unsupported_opcode": 0,
                  "first_native_unbound_opcode": 0,
                  "last_native_unbound_opcode": 0,
                  "first_native_unbound_source": 0,
                  "first_native_unsupported_source": 0,
                  "first_native_unbound_pc": 0,
                  "first_native_unbound_function": 0,
                  "first_native_unsupported_pc": 0,
                  "first_native_unsupported_function": 0,
                  "first_native_unbound_return_address": 0,
                  "first_native_unsupported_return_address": 0,
                   "native_opcode_counts": opcode_histogram(stream_total),
                   "native_state_opcode_counts": opcode_histogram(0),
                   "native_unbound_opcode_counts": opcode_histogram(0),
                   "native_producer_bound_opcode_counts":
                       opcode_histogram(stream_total),
                   "native_packet_derived_opcode_counts": opcode_histogram(0),
                   "native_unsupported_opcode_counts": opcode_histogram(0),
                  "native_unbound_source_by_opcode": opcode_attribution(),
                  "native_unbound_pc_by_opcode": opcode_attribution(),
                  "native_unsupported_pc_by_opcode": opcode_attribution(),
                  "native_unbound_return_address_by_opcode": opcode_attribution(),
                  "native_unsupported_return_address_by_opcode": opcode_attribution(),
                   "native_unbound_source_hotspots": [],
                   "last_native_state": native_state(),
                  "total_independent_vram_presents": 1 if render_mode == "native" else 0,
                  "native_claim": "independent" if render_mode == "native" else "none",
                  "native_coverage_contract": "eligible-3d-producer",
                "total_visual_states": 1 if stream_total else 0,
                "total_superseded": 0,
                "stage_failure_count": 0,
                "first_stage_failure_command_id": 0,
                "first_stage_failure_visual_id": {
                    "scene_epoch": 0,
                    "state_sequence": 0,
                },
                "first_stage_failure_status": 0,
                "last_command_id": 0x1004 if stream_total else 0,
                "last_status": 0,
                "last_stage_status": 0,
                "last_consume_status": 0,
            },
            "ui_ot": {
                "prepare_count": 1 if stream_enabled else 0,
                "completed_count": 1 if stream_enabled else 0,
                "node_count": 1 if stream_enabled else 0,
                "candidate_count": stream_total,
                "prebound_count": 0,
                "staged_count": stream_total,
                "blocked_count": 0,
                "last_start_address": 0,
                "last_node_count": 1 if stream_enabled else 0,
                "last_candidate_count": stream_total,
                "last_prebound_count": 0,
                "last_staged_count": stream_total,
                "last_ot_digest": 0,
                "last_packet_digest": 0,
                "last_semantic_digest": 0,
                "last_environment_digest": 0,
                "last_vram_serial": 0,
                "pending": False,
                "blocked": False,
            },
            "cumulative_fallback_count": 1,
            "scene_fallback_count_baseline": 1,
            "scene_fallback_count_delta": 0,
            "scene_fallback_reason": "none",
            "last_fallback_reason": "backend_failure",
            "fallback_count_overflowed": False,
            "presentation_history": {
                "interpolation_requested": True,
                "interpolation_effective": False,
                "smooth_requested": True,
                "smooth_effective": False,
                "history_count": 0,
                "quiesced": True,
                "gate_reason": "none",
            },
        }
        payload = {
            "status": "PASS",
            "render_mode": render_mode,
            "checkpoint": {"field_id": 5},
            "backend": "opengl",
            "native_render": native_render,
            "counters": {
                "vblank_latches": 3,
                "trace_state_latches": 3,
                "provider_updates": 3,
                "capture_samples": 3,
                "mapping_reads": 3,
                "sio_applies": 3,
            },
            "replay": {"checkpoint_seen_vblank": 1},
            "sio": {"cross_count": 1, "cross_first": 1, "cross_last": 2},
            "prohibited_apis": {
                "net_pad": False,
                "direct_sio": False,
                "debug_input_override": False,
                "ram_writes": False,
            },
        }

        replay.assert_run_evidence(payload, trace, render_mode)
        if render_mode == "native":
            stream = native_render["stream"]
            stream["total_staged"] = stream_total + 2
            stream["total_superseded"] = 1
            stream["staged_count"] = 1
            replay.assert_run_evidence(payload, trace, render_mode)
            stream["staged_count"] = 2
            with pytest.raises(ValueError, match="Native mode telemetry is invalid"):
                replay.assert_run_evidence(payload, trace, render_mode)
            stream["staged_count"] = 1
            if stream_total:
                stream["total_native_bound_packets"] = stream_total - 1
                stream["total_native_unbound_packets"] = 1
                stream["native_unbound_opcode_counts"] = opcode_histogram(1)
                stream["total_native_producer_bound_draws"] = stream_total - 1
                stream["total_native_packet_derived_draws"] = 1
                stream["native_producer_bound_opcode_counts"] = opcode_histogram(
                    stream_total - 1
                )
                stream["native_packet_derived_opcode_counts"] = opcode_histogram(1)
                stream["native_claim"] = "packet-faithful"
                replay.assert_run_evidence(payload, trace, render_mode)
                stream["total_independent_vram_presents"] = 0
                with pytest.raises(ValueError, match="no complete Native command"):
                    replay.assert_run_evidence(payload, trace, render_mode)
                stream["total_independent_vram_presents"] = 1
            native_render["stream"]["total_parser_replay_commands"] = 1
            with pytest.raises(ValueError, match="Native mode telemetry is invalid"):
                replay.assert_run_evidence(payload, trace, render_mode)
            native_render["stream"]["total_parser_replay_commands"] = 0
            native_render["stream"]["total_shared_fmv_frames"] = 1
            with pytest.raises(ValueError, match="Native mode telemetry is invalid"):
                replay.assert_run_evidence(payload, trace, render_mode)
            native_render["stream"]["total_shared_fmv_frames"] = 0
        native_render["effective_render_mode"] = "original"
        with pytest.raises(ValueError, match="effective render mode is invalid"):
            replay.assert_run_evidence(payload, trace, render_mode)
        native_render["effective_render_mode"] = render_mode
        native_render["fallback_count"] = 1
        with pytest.raises(ValueError, match="native render evidence is not closed"):
            replay.assert_run_evidence(payload, trace, render_mode)


def test_runtime_command_when_memcard_dir_is_explicit_forwards_that_directory() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        root = Path(temporary)
        memcard_dir = root / "cards"
        memcard_dir.mkdir()
        (memcard_dir / "card1.mcd").write_bytes(b"card1")
        request = replay.RunRequest(
            build=Path("build-dbg/XenogearsRecomp"),
            trace=Path("tools/native_render_replays/field_clean_boot.toml"),
            runtime_state=root / "state",
            memcard_dir=replay.validate_memcard_dir(memcard_dir),
            evidence=root / "evidence.json",
            renderer="opengl",
        )

        command = replay.runtime_command(request)

        assert command[command.index("--memcard-dir") + 1] == str(memcard_dir.resolve())
        assert command[command.index("--memcard-dir") + 1] != str((root / "state" / "memcards").resolve())


def test_runtime_command_when_child_changes_cwd_uses_canonical_paths() -> None:
    replay = replay_module()
    request = replay.RunRequest(
        build=Path("build-dbg/XenogearsRecomp"),
        trace=Path("tools/native_render_replays/field_clean_boot.toml"),
        runtime_state=Path(".omo/evidence/runtime-state/task-4/run-relative"),
        memcard_dir=Path("memcards"),
        evidence=Path(".omo/evidence/task-4.json"),
        renderer="opengl",
    )
    command = replay.runtime_command(request)
    path_flags = ("--runtime-state", "--memcard-dir", "--input-replay", "--evidence-out")
    assert Path(command[0]).is_absolute()
    for flag in path_flags:
        assert Path(command[command.index(flag) + 1]).is_absolute()


def test_runtime_command_when_given_private_disc_forwards_only_an_absolute_path() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        disc = Path(temporary) / "disc1.cue"
        disc.write_text("FILE \"disc1.bin\" BINARY\n", encoding="utf-8")
        request = replay.RunRequest(
            build=Path("build-dbg/XenogearsRecomp"),
            trace=Path("tools/native_render_replays/field_clean_boot.toml"),
            runtime_state=Path(".omo/evidence/runtime-state/task-4/run-disc"),
            memcard_dir=Path("memcards"),
            evidence=Path(".omo/evidence/task-4.json"),
            renderer="opengl",
            disc=replay.validate_disc(disc),
        )
        command = replay.runtime_command(request)
        private_path = str(disc.resolve())
        assert command[command.index("--disc") + 1] == private_path
        assert replay.scrub_private({"disc": private_path}, request.disc) == {"disc": "<private-disc>"}


def test_record_command_when_private_disc_is_supplied_is_isolated_and_normal_input_only() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        root = Path(temporary)
        disc = root / "disc1.cue"
        disc.write_text("FILE \"disc1.bin\" BINARY\n", encoding="utf-8")
        memcards = root / "memcards"
        memcards.mkdir()
        (memcards / "card1.mcd").write_bytes(b"card1")
        request = replay.RecordRequest(
            build=Path("build-dbg/XenogearsRecomp"), trace=root / "route.toml",
            runtime_state=root / "state", memcard_dir=replay.validate_memcard_dir(memcards), renderer="opengl",
            disc=replay.validate_disc(disc), max_vblanks=60000,
            checkpoint_field=5,
        )
        command = replay.runtime_record_command(request)
        assert "--input-record" in command
        assert "--record-stop-field" in command
        assert command[command.index("--record-stop-field") + 1] == "5"
        assert "--record-max-vblanks" in command
        assert "--input-replay" not in command
        assert not any(token in command for token in ("--netplay", "press", "set_input", "teleport", "savestate", "write_ram"))


def test_manual_record_command_uses_close_completion_without_field_checkpoint() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        root = Path(temporary)
        disc = root / "disc1.cue"
        disc.write_text('FILE "disc1.bin" BINARY\n', encoding="utf-8")
        memcards = root / "memcards"
        memcards.mkdir()
        (memcards / "card1.mcd").write_bytes(b"card1")
        request = replay.RecordRequest(
            build=Path("build-dbg/XenogearsRecomp"), trace=root / "route.toml",
            runtime_state=root / "state",
            memcard_dir=replay.validate_memcard_dir(memcards), renderer="opengl",
            disc=replay.validate_disc(disc), max_vblanks=60000,
            checkpoint_field=None, on_close=True,
        )
        command = replay.runtime_record_command(request)
        assert "--record-on-close" in command
        assert "--record-stop-field" not in command


def test_record_command_when_memcard_dir_is_explicit_uses_that_directory() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        root = Path(temporary)
        memcard_dir = root / "cards"
        memcard_dir.mkdir()
        (memcard_dir / "card1.mcd").write_bytes(b"card1")
        disc = root / "disc1.cue"
        disc.write_text('FILE "disc1.bin" BINARY\n', encoding="utf-8")
        request = replay.RecordRequest(
            build=Path("build-dbg/XenogearsRecomp"),
            trace=root / "route.toml",
            runtime_state=root / "state",
            memcard_dir=replay.validate_memcard_dir(memcard_dir),
            renderer="opengl",
            disc=replay.validate_disc(disc),
            max_vblanks=60000,
            checkpoint_field=5,
        )
        command = replay.runtime_record_command(request)

        assert command[command.index("--memcard-dir") + 1] == str(memcard_dir.resolve())
        assert command[command.index("--memcard-dir") + 1] != str((root / "state" / "memcards").resolve())


def test_record_trace_when_incomplete_is_rejected() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        trace = Path(temporary) / "incomplete.toml"
        trace.write_text(
            '\n'.join((
                'schema = "xenogears.native-render-replay/v2"', "complete = false", "vblank_budget = 1",
                "record_stop_field = 5", "record_stable_vblanks = 4", "[checkpoint]", 'kind = "u16"',
                'address = "0x8006F94E"', "equals = 5", "[[vblank]]", "repeat = 1",
            )), encoding="utf-8",
        )
        try:
            replay.parse_trace(trace)
        except ValueError as error:
            assert "complete" in str(error)
        else:
            raise AssertionError("incomplete trace was accepted")


def test_evidence_without_post_checkpoint_runtime_activity_is_rejected() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        trace_path = Path(temporary) / "trace.toml"
        write_trace(trace_path)
        trace = replay.parse_trace(trace_path)
        run = {
            "status": "PASS",
            "render_mode": "original",
            "checkpoint": {"field_id": 5},
            "backend": "opengl",
            "replay": {"checkpoint_seen_vblank": 3},
            "sio": {"cross_count": 1, "cross_first": 1, "cross_last": 1},
            "counters": {
                "vblank_latches": 3,
                "trace_state_latches": 3,
                "provider_updates": 3,
                "capture_samples": 3,
                "mapping_reads": 3,
                "sio_applies": 3,
            },
            "prohibited_apis": {"net_pad": False, "direct_sio": False,
                                "debug_input_override": False, "ram_writes": False},
        }

        try:
            replay.assert_run_evidence(run, trace)
        except ValueError as error:
            assert "runtime activity" in str(error)
        else:
            raise AssertionError("evidence accepted without post-checkpoint runtime activity")


def test_evidence_with_post_checkpoint_runtime_activity_is_accepted() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        trace_path = Path(temporary) / "trace.toml"
        write_trace(trace_path)
        trace = replay.parse_trace(trace_path)
        run = {
            "status": "PASS",
            "render_mode": "original",
            "checkpoint": {"field_id": 5},
            "backend": "opengl",
            "replay": {"checkpoint_seen_vblank": 2},
            "sio": {"cross_count": 2, "cross_first": 1, "cross_last": 3},
            "counters": {
                "vblank_latches": 3,
                "trace_state_latches": 3,
                "provider_updates": 3,
                "capture_samples": 3,
                "mapping_reads": 3,
                "sio_applies": 3,
            },
            "prohibited_apis": {"net_pad": False, "direct_sio": False,
                                "debug_input_override": False, "ram_writes": False},
        }

        replay.assert_run_evidence(run, trace)


def test_evidence_when_checkpoint_value_differs_is_rejected() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        trace_path = Path(temporary) / "trace.toml"
        write_trace(trace_path)
        trace = replay.parse_trace(trace_path)
        run = {
            "status": "PASS",
            "render_mode": "original",
            "checkpoint": {"field_id": 4},
            "backend": "opengl",
            "replay": {"checkpoint_seen_vblank": 2},
            "sio": {"cross_count": 2, "cross_first": 1, "cross_last": 3},
            "counters": {
                "vblank_latches": 3,
                "trace_state_latches": 3,
                "provider_updates": 3,
                "capture_samples": 3,
                "mapping_reads": 3,
                "sio_applies": 3,
            },
            "prohibited_apis": {"net_pad": False, "direct_sio": False,
                                "debug_input_override": False, "ram_writes": False},
        }

        try:
            replay.assert_run_evidence(run, trace)
        except ValueError as error:
            assert str(error) == "configured checkpoint was not reached"
        else:
            raise AssertionError("evidence accepted without the configured checkpoint")
