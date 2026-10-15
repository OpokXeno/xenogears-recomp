import copy
import argparse
import fcntl
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from train_windows_pgo import graphical_environment, train, validate_training


class TrainingValidationTests(unittest.TestCase):
    def setUp(self):
        self.evidence = {
            "completion": "trace_complete", "runtime_status": "PASS",
            "render_mode": "native", "backend": "opengl",
            "replay": {"trace_index": 1800},
            "native_midpoint": {"target_fps": 240},
            "native_work": {"executed": True, "health": "healthy",
                            "errors": {"compose_failures": 0},
                            "motion": {"last_endpoint_phase_count": 7, "evaluations": 1},
                            "presentation": {"presented_holds": 7000}},
        }
        self.log = "savestate: LOADED slot 0"

    def test_completed_gameplay(self):
        validate_training(self.evidence, self.log, 240, 1800)

    def test_concurrent_training_cannot_overwrite_profiles(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            with (build / ".pgo-training.lock").open("w") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with self.assertRaisesRegex(ValueError, "another PGO training"):
                    train(argparse.Namespace(build_dir=build))

    def test_explicit_display_takes_precedence(self):
        env = {"DISPLAY": ":7", "XAUTHORITY": "/chosen/auth"}
        with patch("pgo_training.subprocess.run") as run:
            self.assertEqual(graphical_environment(env), env)
            run.assert_not_called()

    def test_runner_imports_only_display_session_values(self):
        result = argparse.Namespace(returncode=0, stdout="DISPLAY=:0\nXAUTHORITY=/session/auth\nUNRELATED_TOKEN=private\n")
        with patch("pgo_training.shutil.which", return_value="/usr/bin/systemctl"), \
                patch("pgo_training.subprocess.run", return_value=result):
            env = graphical_environment({"EXISTING": "preserved", "XDG_RUNTIME_DIR": "/run/user/test"})
        self.assertEqual(env, {"EXISTING": "preserved", "DISPLAY": ":0", "XAUTHORITY": "/session/auth",
                               "XDG_RUNTIME_DIR": "/run/user/test"})

    def test_system_service_can_find_its_graphical_user_bus(self):
        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory) / str(os.getuid())
            runtime.mkdir()
            (runtime / "bus").touch()
            result = argparse.Namespace(returncode=0, stdout="DISPLAY=:0\nXAUTHORITY=/session/auth\n")
            with patch("pgo_training.Path", return_value=Path(directory)), \
                    patch("pgo_training.shutil.which", return_value="/usr/bin/systemctl"), \
                    patch("pgo_training.subprocess.run", return_value=result) as run:
                env = graphical_environment({})
            self.assertEqual(run.call_args.kwargs["env"]["XDG_RUNTIME_DIR"], str(runtime))
            self.assertEqual(env["DISPLAY"], ":0")

    def test_missing_graphical_session_fails_before_training(self):
        with patch("pgo_training.shutil.which", return_value=None):
            with self.assertRaisesRegex(ValueError, "DISPLAY and XAUTHORITY"):
                graphical_environment({})

    def test_30fps_accepts_clamped_legacy_diagnostic(self):
        self.evidence["native_midpoint"]["target_fps"] = 60
        self.evidence["native_work"]["presentation"]["presented_holds"] = 0
        validate_training(self.evidence, self.log, 30, 1800)

    def test_missing_slot(self):
        with self.assertRaisesRegex(ValueError, "slot 0"):
            validate_training(self.evidence, "booted normally", 240, 1800)

    def test_rejects_incomplete_or_wrong_rendering(self):
        for key, value in [("completion", "timeout"), ("runtime_status", "FAIL"),
                           ("render_mode", "original"), ("backend", "software")]:
            evidence = copy.deepcopy(self.evidence)
            evidence[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_training(evidence, self.log, 240, 1800)

    def test_rejects_short_trace(self):
        self.evidence["replay"]["trace_index"] = 1799
        with self.assertRaisesRegex(ValueError, "budget"):
            validate_training(self.evidence, self.log, 240, 1800)

    def test_rejects_disabled_interpolation(self):
        self.evidence["native_midpoint"]["target_fps"] = 60
        with self.assertRaisesRegex(ValueError, "FPS"):
            validate_training(self.evidence, self.log, 240, 1800)
        self.evidence["native_midpoint"]["target_fps"] = 240
        self.evidence["native_work"]["presentation"]["presented_holds"] = 0
        with self.assertRaisesRegex(ValueError, "interpolated"):
            validate_training(self.evidence, self.log, 240, 1800)
        self.evidence["native_work"]["presentation"]["presented_holds"] = 7000
        self.evidence["native_work"]["motion"]["last_endpoint_phase_count"] = 0
        with self.assertRaisesRegex(ValueError, "interpolated"):
            validate_training(self.evidence, self.log, 240, 1800)

    def test_rejects_render_errors_or_missing_telemetry(self):
        for errors in ({}, {"compose_failures": 1}):
            self.evidence["native_work"]["errors"] = errors
            with self.subTest(errors=errors), self.assertRaisesRegex(ValueError, "errors"):
                validate_training(self.evidence, self.log, 240, 1800)


if __name__ == "__main__":
    unittest.main()
