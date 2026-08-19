from __future__ import annotations

import argparse
from collections.abc import Callable
import json
import os
from pathlib import Path
import shutil
import signal
import tempfile
from subprocess import PIPE, Popen, TimeoutExpired

from native_render_atomic import load_bounded_json
from native_render_field_character_shadow_schema import parse_field_character_shadow
from native_render_producer_family_schema import build_producer_family_evidence
from native_render_producer_family_scratch import (
    create_producer_family_scratch,
    destroy_producer_family_scratch,
    recover_stale_producer_family_scratch,
    replace_producer_family_scratch_owner,
)
from native_render_process_identity import read_process_identity
from native_render_replay import (
    RecordArgumentError,
    RecordRequest,
    RunRequest,
    _record_environment,
    _runtime_environment,
    assert_auth_proof,
    assert_auth_proof_matrix,
    assert_baseline_evidence,
    assert_duplicate_runs,
    assert_p0_mode_matrix_evidence,
    assert_run_evidence,
    assert_task15_matrix_evidence,
    build_p0_mode_matrix_evidence,
    bounded_diagnostic,
    p0_baseline_projection,
    parse_trace,
    runtime_command,
    runtime_record_command,
    scrub_private,
    validate_disc,
    validate_memcard_dir,
    snapshot_root_cards,
    DEFAULT_BIOS,
    SCHEMA,
    TASK15_MATRIX_SCHEMA,
)
from native_render_replay_baseline import (
    BaselineRequest,
    BuildTarget,
    execute_auth_proof_matrix,
    execute_matrix,
    validate_warm_cache,
)


def _interrupt(_signum: int, _frame: object) -> None:
    raise KeyboardInterrupt


def _terminate_and_reap(process: Popen[str]) -> tuple[str, str]:
    if process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    try:
        return process.communicate(timeout=5)
    except TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        return process.communicate()


def execute_run(
    request: RunRequest, watchdog_seconds: int, *, producer_family: bool = False,
    require_post_checkpoint_activity: bool | None = None,
    on_process_start: Callable[[Popen[str]], None] | None = None,
) -> dict[str, object]:
    request.runtime_state.mkdir(mode=0o700, parents=True, exist_ok=False)
    runtime_evidence = request.runtime_state / "runtime-evidence.json"
    command = runtime_command(request, runtime_evidence)
    environment = _runtime_environment()
    if producer_family:
        environment["PSX_NATIVE_RENDER_PRODUCER_FAMILY"] = "1"
    if request.baseline_request:
        environment["PSX_INPUT_REPLAY_BASELINE"] = "1"
    if request.overlay_mode == "cold":
        environment["PSX_OVERLAY_NATIVE_OFF"] = "1"
    process = Popen(command, cwd=request.build.parent, stdout=PIPE, stderr=PIPE, text=True,
                    start_new_session=True, env=environment)
    try:
        if on_process_start is not None:
            on_process_start(process)
        stdout, stderr = process.communicate(timeout=watchdog_seconds)
    except TimeoutExpired:
        stdout, stderr = _terminate_and_reap(process)
        raise RuntimeError(bounded_diagnostic("watchdog_timeout", stdout, stderr, request.disc)) from None
    except BaseException:
        _terminate_and_reap(process)
        raise
    if process.returncode != 0:
        raise RuntimeError(bounded_diagnostic(f"runtime exited {process.returncode}", stdout, stderr, request.disc))
    if not runtime_evidence.is_file():
        raise RuntimeError(bounded_diagnostic("runtime produced no evidence", stdout, stderr, request.disc))
    with runtime_evidence.open("rb") as source:
        payload = json.load(source)
    payload = scrub_private(payload, request.disc, request.memcard_dir, request.runtime_state,
                            request.trace, request.build)
    if not isinstance(payload, dict):
        raise RuntimeError("runtime evidence is not an object")
    trace = parse_trace(request.trace)
    assert_run_evidence(
        payload,
        trace,
        request.render_mode,
        require_post_checkpoint_activity=(
            not trace.record_on_close
            if require_post_checkpoint_activity is None
            else require_post_checkpoint_activity),
    )
    proof = payload.get("auth_proof")
    if not isinstance(proof, dict):
        raise RuntimeError("runtime auth proof is missing")
    try:
        assert_auth_proof(proof)
    except ValueError as error:
        raise RuntimeError("runtime auth proof is invalid") from error
    return payload


def execute_record(request: RecordRequest, watchdog_seconds: int) -> None:
    if request.trace.exists() or request.trace.is_symlink():
        raise RuntimeError("record_destination_exists")
    request.runtime_state.mkdir(parents=True, exist_ok=False)
    request.trace.parent.mkdir(parents=True, exist_ok=True)
    command = runtime_record_command(request)
    process = Popen(command, cwd=request.build.resolve().parent, stdout=PIPE, stderr=PIPE, text=True,
                    start_new_session=True, env=_record_environment())
    try:
        stdout, stderr = process.communicate(timeout=watchdog_seconds)
    except TimeoutExpired:
        process.kill()
        stdout, stderr = process.communicate()
        raise RuntimeError(bounded_diagnostic("record_watchdog_timeout", stdout, stderr, request.disc)) from None
    if process.returncode != 0:
        raise RuntimeError(bounded_diagnostic(f"recording exited {process.returncode}", stdout, stderr, request.disc))
    if not request.trace.is_file() or request.trace.with_suffix(request.trace.suffix + ".incomplete").exists():
        raise RuntimeError("recording produced no complete trace")
    parse_trace(request.trace)


def clean_runtime_state(path: Path) -> None:
    shutil.rmtree(path, ignore_errors=False)


def self_test() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        trace_path = Path(temporary) / "trace.toml"
        trace_path.write_text(
            f'schema = "{SCHEMA}"\nvblank_budget = 2\n[[vblank]]\np1_buttons = ["a"]\n',
            encoding="utf-8",
        )
        cursor = parse_trace(trace_path)
        assert cursor.vblank_budget == 2
        assert cursor.states[0].p1.buttons == ("a",)
    print("replay self-test: PASS")


def write_evidence(path: Path, payload: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.")
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(payload, output, indent=2, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        try:
            os.link(temporary, path)
        except FileExistsError:
            raise FileExistsError("evidence destination exists") from None
    finally:
        Path(temporary).unlink(missing_ok=True)


def run_duplicate(request: RunRequest, watchdog_seconds: int) -> dict[str, object]:
    runs: list[dict[str, object]] = []
    for suffix in ("run-1", "run-2"):
        runs.append(execute_run(
            RunRequest(request.build, request.trace, request.runtime_state / suffix,
                       request.memcard_dir, request.evidence, request.renderer, request.disc,
                       request.timing_mode, request.render_mode, request.overlay_mode,
                       request.bios, request.baseline_request),
            watchdog_seconds, require_post_checkpoint_activity=False))
    assert_duplicate_runs(
        (runs[0], runs[1]), parse_trace(request.trace), request.render_mode)
    return {"schema": "xenogears.native-render-replay-evidence/v1", "status": "PASS", "runs": runs}


def run_producer_family(
    family_metadata: Path,
    build: Path,
    trace: Path,
    memcard_dir: Path,
    disc: Path,
    watchdog_seconds: int,
    bios: Path = DEFAULT_BIOS,
) -> dict[str, object]:
    previous_term = signal.signal(signal.SIGTERM, _interrupt)
    try:
        if family_metadata.is_symlink() or not family_metadata.is_file():
            raise RecordArgumentError("family-metadata must be a regular file")
        metadata = parse_field_character_shadow(load_bounded_json(family_metadata.read_bytes()))
        if build.is_symlink() or not build.is_file():
            raise RecordArgumentError("build must be a regular executable")
        parsed_trace = parse_trace(trace)
        if (parsed_trace.checkpoint_field is None and
                not parsed_trace.record_on_close):
            raise RecordArgumentError(
                "producer-family trace requires a completion mode")
        if len(parsed_trace.states) != parsed_trace.vblank_budget:
            raise RecordArgumentError("producer-family trace must exhaust its budget")
        before_cards = snapshot_root_cards(memcard_dir)
        recover_stale_producer_family_scratch()
        scratch = create_producer_family_scratch()
        try:
            root = scratch.path
            request = RunRequest(
                build, trace, root / "runtime-state", memcard_dir,
                root / "aggregate.json", "opengl", disc,
                "original", "shadow", "cold", bios,
            )
            runtime = execute_run(
                request, watchdog_seconds, producer_family=True,
                on_process_start=lambda process: replace_producer_family_scratch_owner(
                    scratch, read_process_identity(process.pid),
                ),
            )
            producer = runtime.get("producer_family")
            if not isinstance(producer, dict):
                raise RuntimeError("runtime producer-family evidence is missing")
            payload = build_producer_family_evidence(metadata, producer)
        finally:
            destroy_producer_family_scratch(scratch)
        if snapshot_root_cards(memcard_dir) != before_cards:
            raise RuntimeError("memory card changed")
        return payload
    finally:
        signal.signal(signal.SIGTERM, previous_term)


def run_task15_matrix(
    build: Path,
    trace: Path,
    runtime_state_root: Path,
    memcard_dir: Path,
    disc: Path,
    watchdog_seconds: int,
    bios: Path,
    overlay_mode: str,
) -> dict[str, object]:
    rows: list[dict[str, object]] = []
    runtime_state_root.mkdir(mode=0o700, parents=True, exist_ok=True)
    for render_mode in ("original", "shadow", "native"):
        state = runtime_state_root / render_mode
        completed = False
        request = RunRequest(
            build, trace, state, memcard_dir, runtime_state_root / "unused.json",
            "opengl", disc, "original", render_mode, overlay_mode, bios,
        )
        try:
            runtime = execute_run(
                request, watchdog_seconds, require_post_checkpoint_activity=False)
            native_render = runtime.get("native_render")
            if not isinstance(native_render, dict):
                raise RuntimeError("runtime native-render evidence is missing")
            row = {
                "render_mode": render_mode,
                "status": runtime.get("status"),
                "backend": runtime.get("backend"),
                "native_render": native_render,
                "cleanup": {
                    "runtime_state_removed": False,
                    "process_reaped": True,
                },
            }
            completed = True
        finally:
            if completed and state.exists():
                clean_runtime_state(state)
        row["cleanup"]["runtime_state_removed"] = True
        rows.append(row)
    payload: dict[str, object] = {
        "schema": TASK15_MATRIX_SCHEMA,
        "task": 15,
        "status": "PASS",
        "timing_mode": "original",
        "overlay_mode": overlay_mode,
        "privacy": {"metadata_only": True, "private_paths": False},
        "rows": rows,
    }
    assert_task15_matrix_evidence(payload)
    return payload


def run_p0_mode_matrix(
    build: Path,
    trace: Path,
    runtime_state_root: Path,
    memcard_dir: Path,
    disc: Path,
    watchdog_seconds: int,
    bios: Path,
) -> dict[str, object]:
    rows: list[dict[str, object]] = []
    before_cards = snapshot_root_cards(memcard_dir)
    runtime_state_root.mkdir(mode=0o700, parents=True, exist_ok=True)
    for render_mode in ("original", "shadow", "native"):
        runs: list[dict[str, object]] = []
        for repetition in ("run-1", "run-2"):
            state = runtime_state_root / render_mode / repetition
            request = RunRequest(
                build, trace, state, memcard_dir,
                runtime_state_root / "unused.json", "opengl", disc,
                "original", render_mode, "cold", bios, True,
            )
            try:
                runtime = execute_run(
                    request, watchdog_seconds,
                    require_post_checkpoint_activity=False)
                native_render = runtime.get("native_render")
                if not isinstance(native_render, dict):
                    raise RuntimeError("runtime native-render evidence is missing")
                run = {
                    "status": runtime.get("status"),
                    "backend": runtime.get("backend"),
                    "native_render": native_render,
                    "baseline": p0_baseline_projection(runtime),
                    "cleanup": {
                        "runtime_state_removed": False,
                        "process_reaped": True,
                    },
                }
            finally:
                if state.exists():
                    clean_runtime_state(state)
            run["cleanup"]["runtime_state_removed"] = True
            runs.append(run)
        rows.append({"render_mode": render_mode, "runs": runs})
    if snapshot_root_cards(memcard_dir) != before_cards:
        raise RuntimeError("memory card changed")
    payload = build_p0_mode_matrix_evidence(rows)
    assert_p0_mode_matrix_evidence(payload)
    return payload


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("self-test")
    run_parser = subparsers.add_parser("run")
    run_parser.add_argument("--build", type=Path, required=True)
    run_parser.add_argument("--bios", type=Path, default=root / "game" / "SCPH1001.BIN")
    run_parser.add_argument("--trace", type=Path, required=True)
    run_parser.add_argument("--runtime-state", type=Path, required=True)
    run_parser.add_argument("--memcard-dir", type=Path, required=True,
                            help="existing directory containing project-root card1.mcd and/or card2.mcd")
    run_parser.add_argument("--evidence", type=Path, required=True)
    run_parser.add_argument("--disc", type=Path, required=True)
    run_parser.add_argument("--renderer", choices=("opengl",), required=True)
    run_parser.add_argument("--timing-mode", choices=("original",), required=True)
    run_parser.add_argument(
        "--render-mode", choices=("original", "shadow", "native"), required=True)
    run_parser.add_argument(
        "--overlay-mode", choices=("cold", "warm"), required=True)
    run_parser.add_argument("--runs", type=int, choices=(1, 2), default=2)
    run_parser.add_argument("--watchdog-seconds", type=int, default=900)
    baseline_parser = subparsers.add_parser("baseline")
    baseline_parser.add_argument("--trace", type=Path, required=True)
    baseline_parser.add_argument("--builds", required=True)
    baseline_parser.add_argument("--timing-mode", choices=("original",), required=True)
    baseline_parser.add_argument("--render-mode", choices=("original",), required=True)
    baseline_parser.add_argument("--overlay-modes", choices=("cold,warm",), required=True)
    baseline_parser.add_argument("--warm-cache", type=Path, required=True)
    baseline_parser.add_argument("--memcard-dir", type=Path, required=True)
    baseline_parser.add_argument("--evidence", type=Path, required=True)
    baseline_parser.add_argument("--disc", type=Path)
    baseline_parser.add_argument("--watchdog-seconds", type=int, default=1200)
    auth_proof_parser = subparsers.add_parser("auth-proof")
    auth_proof_parser.add_argument("--trace", type=Path, required=True)
    auth_proof_parser.add_argument("--builds", required=True)
    auth_proof_parser.add_argument("--timing-mode", choices=("original",), required=True)
    auth_proof_parser.add_argument("--render-mode", choices=("original",), required=True)
    auth_proof_parser.add_argument("--overlay-modes", choices=("cold,warm",), required=True)
    auth_proof_parser.add_argument("--warm-cache", type=Path, required=True)
    auth_proof_parser.add_argument("--memcard-dir", type=Path, required=True)
    auth_proof_parser.add_argument("--evidence", type=Path, required=True)
    auth_proof_parser.add_argument("--disc", type=Path)
    auth_proof_parser.add_argument("--watchdog-seconds", type=int, default=1200)
    matrix_parser = subparsers.add_parser("matrix")
    matrix_parser.add_argument("--build", type=Path, required=True)
    matrix_parser.add_argument("--trace", type=Path, required=True)
    matrix_parser.add_argument("--runtime-state-root", type=Path)
    matrix_parser.add_argument("--memcard-dir", type=Path, default=root)
    matrix_parser.add_argument("--evidence", type=Path, required=True)
    matrix_parser.add_argument("--disc", type=Path, default=root / "game" / "disc1.cue")
    matrix_parser.add_argument("--bios", type=Path, default=root / "game" / "SCPH1001.BIN")
    matrix_parser.add_argument("--renderer", choices=("opengl",), required=True)
    matrix_parser.add_argument("--timing-mode", choices=("original",), required=True)
    matrix_parser.add_argument(
        "--render-modes", choices=("original,shadow,native",), required=True)
    matrix_parser.add_argument(
        "--overlay-mode", "--overlay-modes", dest="overlay_mode",
        choices=("cold", "warm"), required=True)
    matrix_parser.add_argument("--watchdog-seconds", type=int, default=1200)
    p0_matrix_parser = subparsers.add_parser("p0-matrix")
    p0_matrix_parser.add_argument("--build", type=Path, required=True)
    p0_matrix_parser.add_argument("--trace", type=Path, required=True)
    p0_matrix_parser.add_argument("--runtime-state-root", type=Path)
    p0_matrix_parser.add_argument("--memcard-dir", type=Path, default=root)
    p0_matrix_parser.add_argument("--evidence", type=Path, required=True)
    p0_matrix_parser.add_argument(
        "--disc", type=Path, default=root / "game" / "disc1.cue")
    p0_matrix_parser.add_argument("--bios", type=Path, default=root / "game" / "SCPH1001.BIN")
    p0_matrix_parser.add_argument("--renderer", choices=("opengl",), required=True)
    p0_matrix_parser.add_argument("--timing-mode", choices=("original",), required=True)
    p0_matrix_parser.add_argument(
        "--render-modes", choices=("original,shadow,native",), required=True)
    p0_matrix_parser.add_argument(
        "--overlay-mode", choices=("cold", "warm"), required=True)
    p0_matrix_parser.add_argument("--watchdog-seconds", type=int, default=1200)
    producer_parser = subparsers.add_parser("producer-family")
    producer_parser.add_argument("--family-metadata", type=Path, required=True)
    producer_parser.add_argument("--timing-mode", choices=("original",), required=True)
    producer_parser.add_argument("--render-mode", choices=("shadow",), required=True)
    producer_parser.add_argument("--evidence", type=Path, required=True)
    producer_parser.add_argument("--build", type=Path, default=root / "build-dbg" / "XenogearsRecomp")
    producer_parser.add_argument("--trace", type=Path, required=True)
    producer_parser.add_argument("--memcard-dir", type=Path, default=root)
    producer_parser.add_argument("--disc", type=Path, default=root / "game" / "disc1.cue")
    producer_parser.add_argument("--bios", type=Path, default=root / "game" / "SCPH1001.BIN")
    producer_parser.add_argument("--watchdog-seconds", type=int, default=1200)
    record_parser = subparsers.add_parser("record")
    record_parser.add_argument("--build", type=Path, required=True)
    record_parser.add_argument("--trace", type=Path, required=True)
    record_parser.add_argument("--runtime-state", type=Path, required=True)
    record_parser.add_argument("--memcard-dir", type=Path, required=True,
                                help="existing directory containing project-root card1.mcd and/or card2.mcd")
    record_parser.add_argument("--disc", type=Path, required=True)
    record_parser.add_argument("--renderer", choices=("opengl",), required=True)
    record_parser.add_argument("--timing-mode", choices=("original",), required=True)
    record_parser.add_argument("--render-mode", choices=("original",), required=True)
    record_parser.add_argument("--max-vblanks", type=int, required=True)
    record_parser.add_argument("--on-close", action="store_true")
    record_parser.add_argument("--checkpoint-field", type=int)
    record_parser.add_argument("--watchdog-seconds", type=int, default=900)
    arguments = parser.parse_args()
    if arguments.command == "self-test":
        self_test()
        return
    if arguments.command == "record":
        if arguments.max_vblanks < 1:
            raise RecordArgumentError("max-vblanks must be positive")
        if arguments.on_close == (arguments.checkpoint_field is not None):
            raise RecordArgumentError(
                "choose exactly one of --on-close or --checkpoint-field")
        if arguments.checkpoint_field is not None and not (
                1 <= arguments.checkpoint_field <= 0xffff):
            raise RecordArgumentError("checkpoint-field must be an unsigned 16-bit value")
        request = RecordRequest(arguments.build, arguments.trace, arguments.runtime_state,
                                 validate_memcard_dir(arguments.memcard_dir), arguments.renderer,
                                 validate_disc(arguments.disc), arguments.max_vblanks,
                                 arguments.checkpoint_field, arguments.on_close)
        execute_record(request, arguments.watchdog_seconds)
        print('{"schema":"xenogears.native-render-record-evidence/v1","status":"PASS"}')
        return
    if arguments.command == "producer-family":
        payload = run_producer_family(
            arguments.family_metadata,
            arguments.build,
            arguments.trace,
            validate_memcard_dir(arguments.memcard_dir),
            validate_disc(arguments.disc),
            arguments.watchdog_seconds,
            arguments.bios.resolve(),
        )
        write_evidence(arguments.evidence, payload)
        return
    if arguments.command == "matrix":
        if arguments.build.is_symlink() or not arguments.build.is_file():
            raise RecordArgumentError("build must be a regular executable")
        parse_trace(arguments.trace)
        runtime_state_root = arguments.runtime_state_root
        if runtime_state_root is None:
            runtime_state_root = (
                arguments.evidence.parent / "runtime-state" /
                f".{arguments.evidence.stem}"
            )
        payload = run_task15_matrix(
            arguments.build,
            arguments.trace,
            runtime_state_root,
            validate_memcard_dir(arguments.memcard_dir),
            validate_disc(arguments.disc),
            arguments.watchdog_seconds,
            arguments.bios.resolve(),
            arguments.overlay_mode,
        )
        write_evidence(arguments.evidence, payload)
        return
    if arguments.command == "p0-matrix":
        if arguments.build.is_symlink() or not arguments.build.is_file():
            raise RecordArgumentError("build must be a regular executable")
        parse_trace(arguments.trace)
        runtime_state_root = arguments.runtime_state_root
        if runtime_state_root is None:
            runtime_state_root = (
                arguments.evidence.parent / "runtime-state" /
                f".{arguments.evidence.stem}"
            )
        payload = run_p0_mode_matrix(
            arguments.build,
            arguments.trace,
            runtime_state_root,
            validate_memcard_dir(arguments.memcard_dir),
            validate_disc(arguments.disc),
            arguments.watchdog_seconds,
            arguments.bios.resolve(),
        )
        write_evidence(arguments.evidence, payload)
        if payload["status"] != "PASS":
            raise RecordArgumentError("p0-matrix BLOCKED")
        return
    if arguments.command in ("baseline", "auth-proof"):
        build_paths = tuple(Path(value) for value in arguments.builds.split(",") if value)
        if len(build_paths) != 2 or any(not path.is_file() or path.is_symlink() for path in build_paths):
            raise RecordArgumentError("builds must name two regular Debug and Release executables")
        disc = validate_disc(arguments.disc) if arguments.disc else validate_disc(root / "game" / "disc1.cue")
        memcard_dir = validate_memcard_dir(arguments.memcard_dir)
        if memcard_dir.resolve() != root.resolve():
            raise RecordArgumentError("memcard-dir must be the XenogearsRecomp repository root")
        warm_cache = validate_warm_cache(arguments.warm_cache) if arguments.command == "baseline" else arguments.warm_cache
        trace = arguments.trace.resolve()
        parse_trace(trace)
        request = BaselineRequest(
            root, trace,
            (BuildTarget("debug", build_paths[0]), BuildTarget("release", build_paths[1])), disc,
            warm_cache, arguments.evidence, arguments.watchdog_seconds, memcard_dir,
        )
        payload = execute_matrix(request) if arguments.command == "baseline" else execute_auth_proof_matrix(request)
        if arguments.command == "auth-proof":
            assert_auth_proof_matrix(payload)
        write_evidence(arguments.evidence, payload)
        if payload["status"] != "PASS":
            raise RecordArgumentError(f"{arguments.command} BLOCKED")
        if arguments.command == "baseline":
            assert_baseline_evidence(payload)
        return
    request = RunRequest(arguments.build, arguments.trace, arguments.runtime_state,
                           validate_memcard_dir(arguments.memcard_dir), arguments.evidence,
                           arguments.renderer, validate_disc(arguments.disc),
                           arguments.timing_mode, arguments.render_mode,
                           arguments.overlay_mode, arguments.bios)
    if arguments.runs == 1:
        run = execute_run(request, arguments.watchdog_seconds)
        payload = {
            "schema": "xenogears.native-render-replay-evidence/v1",
            "status": "PASS",
            "runs": [run],
        }
    else:
        payload = run_duplicate(request, arguments.watchdog_seconds)
    write_evidence(arguments.evidence, payload)


if __name__ == "__main__":
    main()
