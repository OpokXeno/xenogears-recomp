import argparse
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from pgo_training import verify_optimized_binary
from train_linux_pgo import profile_environment, profile_snapshot, train


class LinuxProfileTests(unittest.TestCase):
    def test_nested_container_profile_path_is_redirected_without_renaming_objects(self):
        env = profile_environment(Path("/host/checkout/out/runtime/pgo"),
                                  Path("/workspace/out/runtime/pgo"))
        self.assertEqual(env, {"GCOV_PREFIX": "/host/checkout/out/runtime/pgo",
                               "GCOV_PREFIX_STRIP": "4"})

    def test_relative_compiler_profile_path_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "absolute"):
            profile_environment(Path("/host/pgo"), Path("runtime/pgo"))

    def test_only_nonempty_gcc_counters_count_as_training(self):
        with tempfile.TemporaryDirectory() as directory:
            profiles = Path(directory)
            (profiles / "stale.gcda").touch()
            (profiles / "unrelated.profraw").write_bytes(b"not GCC")
            self.assertEqual(profile_snapshot(profiles), {})
            (profiles / "runtime.gcda").write_bytes(b"fresh counters")
            self.assertEqual(set(profile_snapshot(profiles)), {str(profiles / "runtime.gcda")})

    def test_concurrent_training_cannot_overwrite_profiles(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            with (build / ".pgo-training.lock").open("w") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with self.assertRaisesRegex(ValueError, "another PGO training"):
                    train(argparse.Namespace(build_dir=build))

    def test_packaging_rejects_instrumented_executables(self):
        for symbol in ("__gcov_init", "__llvm_profile_write_file"):
            with self.subTest(symbol=symbol), \
                    patch("pgo_training.subprocess.check_output", return_value=f"1234 T {symbol}\n"):
                with self.assertRaisesRegex(ValueError, "profiling counters"):
                    verify_optimized_binary(Path("release"), "nm")

    def test_optimized_symbols_are_accepted(self):
        with patch("pgo_training.subprocess.check_output", return_value="1234 T main\n"):
            verify_optimized_binary(Path("release"), "nm")

    def test_container_cannot_package_the_generate_pass(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "CMakeCache.txt").write_text(
                "PSX_PGO:STRING=generate\nPSX_RUNTIME_IPO:BOOL=ON\n")
            env = dict(os.environ, XGR_RUNTIME_BUILD_DIR=directory, BUILD_JOBS="1")
            result = subprocess.run(["bash", str(root / "ci/build-linux-release-container.sh"), "package"],
                                    cwd=root, env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("packaging requires PSX_PGO=use", result.stderr)

    @unittest.skipUnless(shutil.which("gcc"), "requires GCC for profile relocation integration check")
    def test_host_profiles_are_consumed_when_reentering_the_compiler_container(self):
        # Model two different mount paths without requiring Podman or graphics.
        # A real GCC use-pass with missing-profile errors catches renamed .gcda
        # files, the wrong GCOV_PREFIX_STRIP, and accidental stale/no profiles.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            compiled = root / "container/workspace/runtime/pgo"
            host = root / "host/runtime/pgo"
            host.mkdir(parents=True)
            source = root / "runtime.c"
            source.write_text("int main(int argc, char **argv) { return argc > 10; }\n")
            executable = root / "runtime"
            obj = root / "runtime.c.o"
            subprocess.run(["gcc", f"-fprofile-generate={compiled}", "-fprofile-update=atomic", "-c", str(source),
                            "-o", str(obj)], check=True, capture_output=True)
            subprocess.run(["gcc", f"-fprofile-generate={compiled}", "-fprofile-update=atomic", str(obj), "-o", str(executable)],
                           check=True, capture_output=True)
            env = dict(os.environ, **profile_environment(host, compiled))
            subprocess.run([str(executable)], env=env, check=True, capture_output=True)
            profiles = profile_snapshot(host)
            self.assertTrue(profiles)
            self.assertFalse(compiled.exists())
            shutil.copytree(host, compiled)
            subprocess.run(["gcc", f"-fprofile-use={compiled}", "-Werror=missing-profile",
                            "-Werror=coverage-mismatch", "-c", str(source), "-o", str(obj)],
                           check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
