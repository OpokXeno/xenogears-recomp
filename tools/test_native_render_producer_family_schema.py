from __future__ import annotations

from copy import deepcopy
import json
import os
from pathlib import Path
import signal
from tempfile import TemporaryDirectory

import pytest

import native_render_replay_cli as replay_cli
from native_render_field_character_shadow_schema import parse_field_character_shadow
from native_render_producer_family_schema import (
    MINIMUM_NATURAL_MATCHES,
    build_producer_family_evidence,
    parse_producer_family_evidence,
    parse_producer_family_runtime,
)
from native_render_producer_family_scratch import (
    create_producer_family_scratch,
    recover_stale_producer_family_scratch,
    replace_producer_family_scratch_owner,
)
from native_render_process_identity import read_process_identity
from native_render_schema import ContractError, JsonValue
from test_native_render_field_character_shadow_schema import shadow_summary


def runtime_summary(count: int = 2800) -> dict[str, JsonValue]:
    return {
        "schema": "xenogears.field-character-candidate/v1",
        "family": "poly-ft4-semitrans",
        "opcode": 46,
        "length_words": 9,
        "enabled": True,
        "blocked": False,
        "geometry_count": count,
        "candidate_count": count,
        "match_count": count,
        "mismatch_count": 0,
        "last_ot_bucket": 194,
        "last_runtime_result": 0,
        "last_compare_result": 0,
        "first_mismatch_word": 0xFFFFFFFF,
        "first_mismatch_byte": 0xFFFFFFFF,
        "blocker": 0,
        "diagnostic": {
            "source_event_count": 28,
            "source_blocker": 0,
            "source_context_bits": 507,
            "collector_phase": 1,
            "collector_blocker": 0,
            "collector_access_count": 28,
            "collector_site_count": 14,
            "source_blocked": False,
            "source_overflowed": True,
            "geometry_completed_count": count,
            "geometry_queued_count": 0,
            "geometry_pending": False,
            "geometry_blocked": False,
            "geometry_overflowed": False,
        },
        "privacy": {
            "metadata_only": True,
            "packet_words": False,
            "guest_paths": False,
        },
    }


def test_evidence_when_all_natural_candidates_match_is_closed_metadata_only() -> None:
    metadata = parse_field_character_shadow(shadow_summary())

    evidence = build_producer_family_evidence(metadata, runtime_summary())

    assert parse_producer_family_evidence(evidence) == evidence
    assert evidence["comparison"] == {
        "geometry_count": 2800,
        "candidate_count": 2800,
        "match_count": 2800,
        "mismatch_count": 0,
        "minimum_match_count": 1000,
        "coverage_complete": True,
        "consumed_bits_exact": True,
        "ignored_padding_upper_halfwords": [6, 8],
    }
    encoded = repr(evidence).lower()
    assert "packet_words" in encoded
    assert "runtime-state" not in encoded
    assert "disc1" not in encoded


@pytest.mark.parametrize("count", (0, MINIMUM_NATURAL_MATCHES - 1))
def test_runtime_when_natural_sample_is_below_minimum_rejects(count: int) -> None:
    with pytest.raises(ContractError, match="comparison_invalid"):
        parse_producer_family_runtime(runtime_summary(count))


@pytest.mark.parametrize(
    ("field", "value"),
    (("mismatch_count", 1), ("candidate_count", 2799),
     ("match_count", 2799), ("blocked", True)),
)
def test_runtime_when_coverage_or_comparison_diverges_rejects(
    field: str, value: JsonValue,
) -> None:
    runtime = runtime_summary()
    runtime[field] = value

    with pytest.raises(ContractError):
        parse_producer_family_runtime(runtime)


def test_runtime_when_raw_packet_material_is_added_rejects() -> None:
    runtime = runtime_summary()
    runtime["raw_packet_words"] = [0x2E808080]

    with pytest.raises(ContractError, match="keys_invalid"):
        parse_producer_family_runtime(runtime)


def test_evidence_when_metadata_names_a_different_family_rejects() -> None:
    metadata_value = deepcopy(shadow_summary())
    selection = metadata_value["selection"]
    assert isinstance(selection, dict)
    selection["opcode"] = 0x2C

    with pytest.raises(ContractError, match="metadata_invalid"):
        build_producer_family_evidence(
            parse_field_character_shadow(metadata_value), runtime_summary(),
        )


def test_evidence_when_public_shape_is_extended_or_cleanup_is_false_rejects() -> None:
    evidence = build_producer_family_evidence(
        parse_field_character_shadow(shadow_summary()), runtime_summary(),
    )
    extended = deepcopy(evidence)
    extended["private_path"] = "/tmp/private"
    bad_cleanup = deepcopy(evidence)
    cleanup = bad_cleanup["cleanup"]
    assert isinstance(cleanup, dict)
    cleanup["runtime_state_removed"] = False

    with pytest.raises(ContractError, match="keys_invalid"):
        parse_producer_family_evidence(extended)
    with pytest.raises(ContractError, match="cleanup_invalid"):
        parse_producer_family_evidence(bad_cleanup)


def producer_inputs(root: Path) -> tuple[Path, Path, Path, Path, Path]:
    metadata = root / "task-11.json"
    metadata.write_text(json.dumps(shadow_summary()), encoding="utf-8")
    build = root / "XenogearsRecomp"
    build.write_bytes(b"executable")
    trace = root / "field5.toml"
    trace.write_text(
        'schema = "xenogears.native-render-replay/v1"\n'
        "vblank_budget = 1\n"
        "[[vblank]]\n"
        "repeat = 1\n",
        encoding="utf-8",
    )
    cards = root / "cards"
    cards.mkdir()
    (cards / "card1.mcd").write_bytes(b"card-1")
    (cards / "card2.mcd").write_bytes(b"card-2")
    disc = root / "disc1.cue"
    disc.write_text('FILE "disc1.bin" BINARY\n', encoding="utf-8")
    return metadata, build, trace, cards, disc


def test_producer_command_when_runtime_passes_removes_its_scratch() -> None:
    with TemporaryDirectory() as temporary:
        inputs = producer_inputs(Path(temporary))
        observed: list[Path] = []
        original = replay_cli.execute_run

        def execute(
            request: object, _watchdog: int, *, producer_family: bool,
            on_process_start: object = None,
        ) -> dict[str, object]:
            del on_process_start
            assert producer_family
            runtime_state = request.runtime_state
            runtime_state.mkdir()
            (runtime_state / "private.bin").write_bytes(b"private")
            observed.append(runtime_state)
            return {"producer_family": runtime_summary()}

        replay_cli.execute_run = execute
        try:
            evidence = replay_cli.run_producer_family(*inputs, 1)
        finally:
            replay_cli.execute_run = original

        assert evidence["status"] == "PASS"
        assert len(observed) == 1
        assert not observed[0].exists()


@pytest.mark.parametrize("outcome", ("exception", "sigint", "sigterm"))
def test_producer_command_when_runtime_aborts_removes_its_scratch(outcome: str) -> None:
    with TemporaryDirectory() as temporary:
        inputs = producer_inputs(Path(temporary))
        observed: list[Path] = []
        original = replay_cli.execute_run

        def execute(
            request: object, _watchdog: int, *, producer_family: bool,
            on_process_start: object = None,
        ) -> dict[str, object]:
            del on_process_start
            assert producer_family
            runtime_state = request.runtime_state
            runtime_state.mkdir()
            observed.append(runtime_state)
            if outcome == "sigterm":
                os.kill(os.getpid(), signal.SIGTERM)
            if outcome == "sigint":
                raise KeyboardInterrupt
            raise RuntimeError("child failed")

        replay_cli.execute_run = execute
        try:
            with pytest.raises((KeyboardInterrupt, RuntimeError)):
                replay_cli.run_producer_family(*inputs, 1)
        finally:
            replay_cli.execute_run = original

        assert len(observed) == 1
        assert not observed[0].exists()


def test_scratch_recovery_waits_until_the_recorded_owner_is_dead() -> None:
    ready, release = os.pipe2(os.O_CLOEXEC)
    child = os.fork()
    if child == 0:
        os.close(release)
        os.read(ready, 1)
        os._exit(0)
    os.close(ready)
    scratch = create_producer_family_scratch()
    try:
        replace_producer_family_scratch_owner(
            scratch, read_process_identity(child),
        )
        assert recover_stale_producer_family_scratch() == 0
        assert scratch.path.is_dir()
        os.close(release)
        os.waitpid(child, 0)
        assert recover_stale_producer_family_scratch() == 1
        assert not scratch.path.exists()
    finally:
        try:
            os.close(release)
        except OSError:
            pass
        try:
            os.waitpid(child, 0)
        except ChildProcessError:
            pass
        scratch.close()
