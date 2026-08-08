from __future__ import annotations

import importlib.util
import json
from pathlib import Path
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


def replay_cli_module() -> ModuleType:
    specification = importlib.util.spec_from_file_location("native_render_replay_cli", CLI_SCRIPT)
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


def write_trace_with_state_count(path: Path, vblank_budget: int, state_count: int) -> None:
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
        f"repeat = {state_count}",
        'p1_buttons = ["a"]',
        "p1_left_x = 0",
        "p1_left_y = 0",
            )
        ),
        encoding="utf-8",
    )


def test_duplicate_runs_when_given_private_disc_preserve_it_for_each_child() -> None:
    replay = replay_module()
    replay_cli = replay_cli_module()
    with TemporaryDirectory() as temporary:
        disc = Path(temporary) / "disc1.cue"
        disc.write_text("FILE \"disc1.bin\" BINARY\n", encoding="utf-8")
        request = replay.RunRequest(
            build=Path("build-dbg/XenogearsRecomp"),
            trace=Path(temporary) / "trace.toml",
            runtime_state=Path(".omo/evidence/runtime-state/task-4"),
            memcard_dir=Path("memcards"),
            evidence=Path(".omo/evidence/task-4.json"),
            renderer="opengl",
            disc=replay.validate_disc(disc),
        )
        write_trace(request.trace, vblank_budget=3)
        observed: list[tuple[Path | None, Path, bool | None]] = []
        run = {
            "status": "PASS", "timing_mode": "original", "render_mode": "original",
            "checkpoint": {"field_id": 5}, "backend": "opengl",
            "counters": {"vblank_latches": 3, "trace_state_latches": 3,
                         "provider_updates": 3, "capture_samples": 3,
                         "mapping_reads": 3, "sio_applies": 3},
            "replay": {"checkpoint_seen_vblank": 1},
            "sio": {"cross_count": 1, "cross_first": 1, "cross_last": 2},
            "guest_sequence": [1, 2, 3],
            "prohibited_apis": {"net_pad": False, "direct_sio": False,
                                "debug_input_override": False, "ram_writes": False},
        }
        original_execute_run = replay_cli.execute_run
        replay_cli.execute_run = lambda child, _watchdog, **options: observed.append((
            child.disc, child.memcard_dir,
            options.get("require_post_checkpoint_cross"),
        )) or run
        try:
            replay_cli.run_duplicate(request, 1)
        finally:
            replay_cli.execute_run = original_execute_run
        assert observed == [
            (request.disc, request.memcard_dir, False),
            (request.disc, request.memcard_dir, False),
        ]


def test_field_five_trace_when_parsed_is_bounded_by_its_guest_budget() -> None:
    replay = replay_module()
    trace = replay.parse_trace(ROOT / "tools" / "native_render_replays" / "field5_clean_boot.toml")
    assert len(trace.states) == trace.vblank_budget


def test_evidence_when_two_original_runs_match_requires_opengl_and_field_five() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        trace_path = Path(temporary) / "trace.toml"
        write_trace(trace_path, vblank_budget=3)
        trace = replay.parse_trace(trace_path)
        run = {
            "status": "PASS",
            "timing_mode": "original",
            "render_mode": "original",
            "checkpoint": {"field_id": 5},
            "backend": "opengl",
            "counters": {
                "vblank_latches": 3,
                "trace_state_latches": 3,
                "provider_updates": 6,
                "capture_samples": 9,
                "mapping_reads": 9,
                "sio_applies": 9,
            },
            "replay": {"checkpoint_seen_vblank": 2},
            "sio": {"cross_count": 2, "cross_first": 1, "cross_last": 3},
            "guest_sequence": [1, 2, 3],
            "prohibited_apis": {"net_pad": False, "direct_sio": False,
                                "debug_input_override": False, "ram_writes": False},
        }
        replay.assert_run_evidence(run, trace)
        replay.assert_duplicate_runs((run, json.loads(json.dumps(run))), trace)


def test_evidence_when_pass_stops_before_trace_budget_is_rejected() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        trace = Path(temporary) / "trace.toml"
        write_trace_with_state_count(trace, 2978, 2905)
        parsed_trace = replay.parse_trace(trace)
        payload = {
            "status": "PASS",
            "timing_mode": "original",
            "render_mode": "original",
            "checkpoint": {"field_id": 5},
            "backend": "opengl",
            "counters": {
                "vblank_latches": 2905,
                "trace_state_latches": 2905,
                "provider_updates": 2905,
                "capture_samples": 2905,
                "mapping_reads": 2905,
                "sio_applies": 2905,
            },
            "guest_sequence": list(range(2905)),
            "prohibited_apis": {
                "net_pad": False,
                "direct_sio": False,
                "debug_input_override": False,
                "ram_writes": False,
            },
        }
        with pytest.raises(ValueError, match="trace did not exhaust its declared budget"):
            replay.assert_run_evidence(payload, parsed_trace)


def test_evidence_when_pass_consumes_full_trace_budget_is_accepted() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        trace_path = Path(temporary) / "trace.toml"
        write_trace_with_state_count(trace_path, 2978, 2978)
        trace = replay.parse_trace(trace_path)
        payload = {
            "status": "PASS",
            "timing_mode": "original",
            "render_mode": "original",
            "checkpoint": {"field_id": 5},
            "backend": "opengl",
            "counters": {
                "vblank_latches": 2978,
                "trace_state_latches": 2978,
                "provider_updates": 2978,
                "capture_samples": 2978,
                "mapping_reads": 2978,
                "sio_applies": 2978,
            },
            "replay": {"checkpoint_seen_vblank": 2},
            "sio": {"cross_count": 2, "cross_first": 1, "cross_last": 2978},
            "guest_sequence": list(range(2978)),
            "prohibited_apis": {
                "net_pad": False,
                "direct_sio": False,
                "debug_input_override": False,
                "ram_writes": False,
            },
        }
        replay.assert_run_evidence(payload, trace)


def test_evidence_when_internal_counter_values_diverge_is_rejected() -> None:
    replay = replay_module()
    with TemporaryDirectory() as temporary:
        trace_path = Path(temporary) / "trace.toml"
        write_trace(trace_path, vblank_budget=3)
        trace = replay.parse_trace(trace_path)
        run = {
            "status": "PASS",
            "timing_mode": "original",
            "render_mode": "original",
            "checkpoint": {"field_id": 5},
            "backend": "opengl",
            "counters": {
                "vblank_latches": 3,
                "trace_state_latches": 3,
                "provider_updates": 2,
                "capture_samples": 9,
                "mapping_reads": 9,
                "sio_applies": 9,
            },
            "guest_sequence": [1, 2, 3],
            "prohibited_apis": {"net_pad": False, "direct_sio": False,
                                "debug_input_override": False, "ram_writes": False},
        }
        with pytest.raises(ValueError, match="missing replay traversal counter: provider_updates"):
            replay.assert_run_evidence(run, trace)
