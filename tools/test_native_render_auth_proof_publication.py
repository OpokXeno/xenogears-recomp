from __future__ import annotations

import json
from pathlib import Path
from tempfile import TemporaryDirectory

import pytest

from test_native_render_auth_proof import baseline_module, cli_module
from test_native_render_replay import write_trace


def test_runtime_child_when_exit_is_nonzero_rejects_written_evidence_before_reading(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    baseline = baseline_module()
    with TemporaryDirectory() as temporary:
        root = Path(temporary)
        for name in ("card1.mcd", "card2.mcd"):
            (root / name).write_bytes(b"card")
        child = baseline.BaselineChild(
            root, root / "source", root / "staged", None, root / "trace", root / "disc",
            root / "runtime", root / "cache", root / "capture.json", "cold", False,
        )
        evidence = child.runtime_state / "runtime-evidence.json"

        class Process:
            returncode = 7

            def communicate(self, timeout: int) -> tuple[str, str]:
                del timeout
                evidence.write_text("not-json", encoding="utf-8")
                return "", ""

        monkeypatch.setattr(baseline, "_stage_executable", lambda _child: None)
        monkeypatch.setattr(baseline, "Popen", lambda *args, **kwargs: Process())

        with pytest.raises(baseline.BaselineError, match="runtime_nonzero_exit_7"):
            baseline.execute_runtime_child(child, 1)


def test_baseline_row_when_evidence_is_public_stages_private_paths_elsewhere() -> None:
    baseline = baseline_module()
    observed_private_paths: list[tuple[Path, Path, Path, Path]] = []
    with TemporaryDirectory() as temporary:
        root = Path(temporary)
        evidence = root / "public-evidence" / "task-9.json"
        write_trace(root / "trace")
        for name in ("card1.mcd", "card2.mcd"):
            (root / name).write_bytes(b"card")
        request = baseline.BaselineRequest(
            root, root / "trace", (baseline.BuildTarget("debug", root / "debug"), baseline.BuildTarget("release", root / "release")),
            root / "disc", root / "warm-cache", evidence, 1,
        )

        def execute(child):
            observed_private_paths.append((child.executable, child.cache_dir, child.capture_path, child.runtime_state))
            raise baseline.BaselineError("test_blocked", "/test")

        baseline._row(request, request.builds[0], "cold", execute)

        assert len(observed_private_paths) == 2
        for paths in observed_private_paths:
            for private_path in paths:
                assert not private_path.is_relative_to(evidence.parent)


def test_write_evidence_when_destination_is_absent_publishes_payload() -> None:
    cli = cli_module()
    with TemporaryDirectory() as temporary:
        destination = Path(temporary) / "evidence.json"

        cli.write_evidence(destination, {"status": "BLOCKED"})

        assert json.loads(destination.read_text(encoding="utf-8")) == {"status": "BLOCKED"}


def test_write_evidence_when_destination_exists_preserves_blocked_receipt() -> None:
    cli = cli_module()
    with TemporaryDirectory() as temporary:
        destination = Path(temporary) / "evidence.json"
        destination.write_text('{"status":"existing"}\n', encoding="utf-8")

        with pytest.raises(FileExistsError, match="evidence destination exists"):
            cli.write_evidence(destination, {"status": "BLOCKED"})

        assert destination.read_text(encoding="utf-8") == '{"status":"existing"}\n'


def test_write_evidence_when_destination_is_symlink_preserves_link_target() -> None:
    cli = cli_module()
    with TemporaryDirectory() as temporary:
        root = Path(temporary)
        target = root / "target.json"
        target.write_text('{"status":"existing"}\n', encoding="utf-8")
        destination = root / "evidence.json"
        destination.symlink_to(target)

        with pytest.raises(FileExistsError, match="evidence destination exists"):
            cli.write_evidence(destination, {"status": "BLOCKED"})

        assert destination.is_symlink()
        assert target.read_text(encoding="utf-8") == '{"status":"existing"}\n'
