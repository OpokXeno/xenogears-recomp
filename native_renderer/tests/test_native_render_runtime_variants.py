#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///

from __future__ import annotations

import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import tomllib
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]
TOOL = REPOSITORY / "tools" / "native_render_runtime_variants.py"
CANONICAL_TOOL = REPOSITORY / "tools" / "native_render_manifest.py"
CANONICAL_MANIFEST = REPOSITORY / "native_renderer" / "xg_render_manifest.toml"
GAME = REPOSITORY / "game" / "slus_006.64"
OVERLAYS = REPOSITORY / "overlays"
COMPANION = REPOSITORY / "native_renderer" / "xg_render_runtime_variants.toml"
ARTIFACT = REPOSITORY / "overlays" / "field_runtime.bin"


class NativeRenderRuntimeVariantsTests(unittest.TestCase):
    def require_private_artifact(self) -> None:
        if not ARTIFACT.is_file():
            self.skipTest("private runtime artifact is unavailable")
        artifact = tomllib.loads(COMPANION.read_text(encoding="utf-8"))["artifact"]
        contents = ARTIFACT.read_bytes()
        if (len(contents) != artifact["full_size"] or
                hashlib.sha256(contents).hexdigest() != artifact["full_sha256"]):
            self.skipTest("private runtime artifact does not match companion identity")
    def run_tool(self, *arguments: str, expect: int = 0) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            [sys.executable, str(TOOL), *arguments],
            cwd=REPOSITORY,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, expect, result.stderr)
        return result

    def test_private_artifact_end_to_end_contract(self) -> None:
        self.require_private_artifact()
        self._check_emits_bound_runtime_companion_table()
        self._check_companion_emission_preserves_canonical_generated_bytes()
        self._check_rejects_wrong_canonical_binding_and_artifact_identity()
        self._check_rejects_hook_constraint_and_descriptor_ambiguity()

    def _check_emits_bound_runtime_companion_table(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "runtime_variants.c"

            result = self.run_tool(
                "emit", str(COMPANION), "--artifact", str(ARTIFACT),
                "--out", str(output),
            )

            generated = output.read_text(encoding="ascii")
            self.assertIn("xg_render_runtime_variant_descriptors", generated)
            self.assertIn("0x80075414", generated)
            self.assertIn("0x800764b4", generated)
            self.assertIn("0x80075694", generated)
            self.assertIn("0x8007569c", generated)
            self.assertIn("emit PASS", result.stdout)
            compiler = shutil.which("cc")
            if compiler is not None:
                compiled = subprocess.run(
                    [
                        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-pedantic", "-fsyntax-only", "-I",
                        str(REPOSITORY / "native_renderer" / "include"), str(output),
                    ],
                    cwd=REPOSITORY,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def _check_companion_emission_preserves_canonical_generated_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            canonical = root / "xg_render_manifest_table.c"

            result = subprocess.run(
                [
                    sys.executable, str(CANONICAL_TOOL), "emit",
                    str(CANONICAL_MANIFEST), "--exe", str(GAME),
                    "--overlays", str(OVERLAYS), "--out", str(canonical),
                ],
                cwd=REPOSITORY,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            before = canonical.read_bytes()
            self.run_tool(
                "emit", str(COMPANION), "--artifact", str(ARTIFACT),
                "--out", str(root / "xg_render_runtime_variant_table.c"),
            )
            self.assertEqual(before, canonical.read_bytes())

    def _check_rejects_wrong_canonical_binding_and_artifact_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = root / "runtime_variants.toml"
            manifest.write_text(
                COMPANION.read_text(encoding="utf-8").replace(
                    "dc0b2dd786203d4cce5927c5a3fc85a18f39a3f7406078860076ebb0bbae7119",
                    "0c0b2dd786203d4cce5927c5a3fc85a18f39a3f7406078860076ebb0bbae7119",
                ),
                encoding="utf-8",
                newline="\n",
            )

            result = self.run_tool("validate", str(manifest), "--artifact", str(ARTIFACT), expect=1)

            self.assertIn("canonical game", result.stderr.lower())
            manifest.write_text(COMPANION.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
            artifact = root / "field_runtime.bin"
            artifact.write_bytes(ARTIFACT.read_bytes()[:-1] + b"X")
            result = self.run_tool("validate", str(manifest), "--artifact", str(artifact), expect=1)
            self.assertIn("artifact identity", result.stderr.lower())

    def _check_rejects_hook_constraint_and_descriptor_ambiguity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = COMPANION.read_text(encoding="utf-8")
            manifest = root / "runtime_variants.toml"
            cases = (
                ("activation_target", 'activation_target = "0x800764b4"', 'activation_target = "0x80076344"'),
                ("activation_window", 'activation_window_start = "0x8007540c"', 'activation_window_start = "0x80075410"'),
                ("capture_target", 'capture_target = "0x8004b54c"', 'capture_target = "0x80044bd0"'),
                ("capture_delay", 'capture_delay_instruction = "0x34040001"', 'capture_delay_instruction = "0x34040002"'),
                ("capture_window", 'capture_window_start = "0x8007568c"', 'capture_window_start = "0x80075690"'),
            )
            for name, source, replacement in cases:
                with self.subTest(name=name):
                    manifest.write_text(original.replace(source, replacement), encoding="utf-8", newline="\n")
                    result = self.run_tool("validate", str(manifest), "--artifact", str(ARTIFACT), expect=1)
                    self.assertIn("runtime", result.stderr.lower())
            manifest.write_text(original + original[original.index("[[variants]]") :], encoding="utf-8", newline="\n")

            result = self.run_tool("validate", str(manifest), "--artifact", str(ARTIFACT), expect=1)

            self.assertIn("duplicate normalized", result.stderr.lower())

    def test_rejects_malformed_input_before_private_artifact_access(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "runtime_variants.toml"
            manifest.write_text("schema =", encoding="ascii", newline="\n")

            result = self.run_tool(
                "validate", str(manifest), "--artifact", str(ARTIFACT), expect=1)

            self.assertIn("manifest is unreadable", result.stderr.lower())

    def test_missing_private_artifact_fails_closed(self) -> None:
        result = self.run_tool(
            "validate", str(COMPANION), "--artifact",
            str(ARTIFACT.with_name("missing-runtime.bin")), expect=1)
        self.assertIn("artifact filename mismatch", result.stderr.lower())

    def test_declared_descriptor_binds_companion_manifest_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "runtime_variants.c"

            self.run_tool(
                "emit-declared", str(COMPANION), "--out", str(output))

            identity = hashlib.sha256(COMPANION.read_bytes()).digest()
            initializer = ",".join(f"0x{byte:02x}" for byte in identity)
            self.assertIn(f"{{{initializer}}}", output.read_text(encoding="ascii"))

    def test_codegen_contract_rotates_cache_without_abi_bump(self) -> None:
        overlay_api = (
            REPOSITORY / "psxrecomp" / "runtime" / "include" / "overlay_api.h"
        ).read_text(encoding="utf-8")
        hash_sources = (
            REPOSITORY / "psxrecomp" / "runtime" / "codegen_hash_sources.cmake"
        ).read_text(encoding="utf-8")

        self.assertIn("#define PSX_OVERLAY_ABI_VERSION 25", overlay_api)
        self.assertIn("#define PSX_OVERLAY_CODEGEN_VER 18", overlay_api)
        self.assertIn("recompiler/src/code_generator.cpp", hash_sources)
        self.assertIn("recompiler/include/code_generator.h", hash_sources)
        self.assertIn("runtime/include/overlay_api.h", hash_sources)


if __name__ == "__main__":
    unittest.main()
