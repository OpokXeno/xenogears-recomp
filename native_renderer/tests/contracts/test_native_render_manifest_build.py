#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_native_render_manifest import (
    add_artifact_overlay,
    add_world_overlay,
    write_fixture,
)


REPOSITORY = Path(__file__).resolve().parents[3]
TOOL = REPOSITORY / "tools" / "native_render_manifest.py"
ROOT_CMAKE = REPOSITORY / "CMakeLists.txt"
SHELL_WRAPPER = REPOSITORY / "build.sh"
POWERSHELL_WRAPPER = REPOSITORY / "build.ps1"


class NativeRenderManifestBuildTests(unittest.TestCase):
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

    def test_metadata_emits_closed_safe_configuration_document(self) -> None:
        # Given
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, exe, overlays = write_fixture(root)

            # When
            result = self.run_tool(
                "metadata",
                str(manifest),
                "--exe",
                str(exe),
                "--overlays",
                str(overlays),
            )
            payload = json.loads(result.stdout)

            # Then
            self.assertEqual(
                set(payload),
                {"schema", "game_identity", "manifest_identity", "namespace_crc32"},
            )
            self.assertEqual(payload["schema"], "xg-render-config-metadata/v1")
            self.assertEqual(payload["game_identity"], hashlib.sha256(exe.read_bytes()).hexdigest())
            self.assertEqual(payload["manifest_identity"], hashlib.sha256(manifest.read_bytes()).hexdigest())
            self.assertEqual(payload["namespace_crc32"], "89abcdef")
            self.assertNotIn(str(root), result.stdout)
            self.assertNotIn("field.bin", result.stdout)
            self.assertNotRegex(result.stdout, r"0x[0-9a-f]{8}")

    def test_metadata_declared_accepts_all_canonical_overlays(self) -> None:
        canonical = REPOSITORY / "native_renderer" / "xg_render_manifest.toml"
        manifest = canonical.read_text(encoding="utf-8")
        self.assertIn("[overlay.field]", manifest)
        self.assertIn("[overlay.world]", manifest)
        self.assertIn("[overlay.battle]", manifest)

        result = self.run_tool("metadata-declared", str(canonical))
        payload = json.loads(result.stdout)

        self.assertEqual(
            payload["manifest_identity"],
            hashlib.sha256(canonical.read_bytes()).hexdigest())

    def test_metadata_fails_closed_for_blocked_field_record(self) -> None:
        # Given
        with tempfile.TemporaryDirectory() as directory:
            manifest, exe, overlays = write_fixture(Path(directory))
            blocked = re.sub(
                r"(?ms)^\[overlay\.field\]\n.*?(?=^\[\[functions\]\])",
                "[overlay.field]\n"
                'id = "field-image"\n'
                'state = "blocked"\n'
                'base_address = "0x8006f000"\n'
                'reason_code = "private-image-unavailable"\n\n',
                manifest.read_text(encoding="utf-8"),
                count=1,
            )
            manifest.write_text(blocked, encoding="utf-8", newline="\n")

            # When
            result = self.run_tool(
                "metadata",
                str(manifest),
                "--exe",
                str(exe),
                "--overlays",
                str(overlays),
                expect=1,
            )

            # Then
            self.assertIn("field-image", result.stderr)
            self.assertEqual(result.stdout, "")

    def test_root_cmake_links_generated_table_with_closed_dependencies(self) -> None:
        # Given
        cmake = ROOT_CMAKE.read_text(encoding="utf-8")

        # When
        configured_from_metadata = "native_render_manifest.py\" metadata" in cmake

        # Then
        self.assertTrue(configured_from_metadata)
        self.assertIn("CMAKE_CONFIGURE_DEPENDS", cmake)
        self.assertNotRegex(cmake, r"set\(PSX_GAME_EXTRA_IDENTITY_SHA256")
        self.assertIn('GAME_EXTRA_IDENTITY_SHA256 "${XG_RENDER_GAME_IDENTITY_SHA256}"', cmake)
        self.assertIn('GAME_MANIFEST_DIGEST_SHA256 "${XG_RENDER_MANIFEST_IDENTITY_SHA256}"', cmake)
        self.assertIn('target_sources(psx-runtime PRIVATE "${XG_RENDER_MANIFEST_TABLE}"', cmake)
        self.assertIn('"${XG_RENDER_RUNTIME_VARIANT_TABLE}")', cmake)
        self.assertIn("add_dependencies(psx-runtime xg_render_manifest_table)", cmake)
        self.assertIn("add_dependencies(psx-runtime xg_render_runtime_variant_table)", cmake)
        self.assertIn("native_render_runtime_variants.py", cmake)
        for dependency in (
            "native_render_manifest.py",
            "native_render_manifest_model.py",
            "native_render_manifest_output.py",
            "native_render_manifest_verify.py",
        ):
            self.assertIn(dependency, cmake)
        self.assertGreaterEqual(cmake.count("${XG_RENDER_MANIFEST_TOOL_SOURCES}"), 2)
        private_configure_dependencies = (
            "if(XG_RENDER_NATIVE AND XG_RENDER_VALIDATE_OVERLAYS)\n"
            "    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS\n"
            '        "${XG_RENDER_OVERLAYS}"\n'
            '        "${XG_RENDER_RUNTIME_ARTIFACT}")\n'
            "endif()"
        )
        self.assertIn(private_configure_dependencies, cmake)
        required_dependencies = cmake.split(
            private_configure_dependencies, maxsplit=1)[0].rsplit(
                "set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS",
                maxsplit=1,
            )[1]
        self.assertNotIn("XG_RENDER_OVERLAYS", required_dependencies)
        self.assertNotIn("XG_RENDER_RUNTIME_ARTIFACT", required_dependencies)

    def test_build_wrappers_use_declared_manifest_metadata(self) -> None:
        # Given
        wrappers = {
            "shell": SHELL_WRAPPER.read_text(encoding="utf-8"),
            "powershell": POWERSHELL_WRAPPER.read_text(encoding="utf-8"),
        }

        # When / Then
        for name, wrapper in wrappers.items():
            with self.subTest(wrapper=name):
                self.assertIn("native_render_manifest.py", wrapper)
                self.assertRegex(
                    wrapper,
                    re.compile(r"MANIFEST_TOOL.{0,50}metadata-declared", re.DOTALL),
                )
                self.assertNotIn("OVERLAYS_DIR", wrapper)
                self.assertIn("-DXG_RENDER_VALIDATE_OVERLAYS=OFF", wrapper)
                self.assertIn("BUILD_JOBS", wrapper)
                self.assertIn("16", wrapper)
                for disc_name in ("disc1.cue", "disc1.bin", "disc1.iso"):
                    self.assertIn(disc_name, wrapper)
                self.assertRegex(
                    wrapper,
                    re.compile(r"PSX_GAME_EXTRA_IDENTITY_SHA256.{0,120}GAME.*IDENTITY", re.DOTALL | re.IGNORECASE),
                )
                self.assertRegex(
                    wrapper,
                    re.compile(r"PSX_GAME_MANIFEST_DIGEST_SHA256.{0,120}MANIFEST.*IDENTITY", re.DOTALL | re.IGNORECASE),
                )

    def test_overlay_codegen_uses_a_portable_bounded_pool(self) -> None:
        cmake = ROOT_CMAKE.read_text(encoding="utf-8")

        self.assertIn("set(XG_OVERLAY_CODEGEN_JOBS 2 CACHE STRING", cmake)
        self.assertIn("tools/run_with_job_pool.py", cmake)
        self.assertNotIn("CMAKE_GENERATOR MATCHES", cmake)
        self.assertGreaterEqual(cmake.count('"${XG_OVERLAY_JOB_RUNNER}"'), 8)

    def test_repository_forces_lf_for_hashed_tracked_files(self) -> None:
        # The manifest's pinned identities are byte hashes, not text hashes.
        attributes = (REPOSITORY / ".gitattributes").read_text(encoding="utf-8")

        self.assertIn("* text=auto eol=lf", attributes)

    @unittest.skipUnless(shutil.which("cmake"), "cmake is required")
    def test_default_build_does_not_access_private_overlay_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, exe, overlays = write_fixture(root)
            add_world_overlay(manifest, overlays)
            add_artifact_overlay(
                manifest, overlays, "battle", "battle-image", "battle.bin", 31, 16)
            add_artifact_overlay(
                manifest, overlays, "future_render", "future-image_v2",
                "future_overlay.bin", 37, None)
            shutil.rmtree(overlays)
            runtime_variants = root / "runtime-variants.toml"
            runtime_source = (
                REPOSITORY / "native_renderer" / "xg_render_runtime_variants.toml"
            ).read_text(encoding="utf-8")
            runtime_source, game_replacements = re.subn(
                r'(?m)^game_sha256 = "[0-9a-f]{64}"$',
                f'game_sha256 = "{hashlib.sha256(exe.read_bytes()).hexdigest()}"',
                runtime_source,
            )
            runtime_source, manifest_replacements = re.subn(
                r'(?m)^manifest_sha256 = "[0-9a-f]{64}"$',
                f'manifest_sha256 = "{hashlib.sha256(manifest.read_bytes()).hexdigest()}"',
                runtime_source,
            )
            self.assertEqual((game_replacements, manifest_replacements), (1, 1))
            runtime_variants.write_text(
                runtime_source, encoding="utf-8", newline="\n")
            missing_root = root / "private-inputs-do-not-create"
            missing_overlays = missing_root / "overlays"
            missing_artifact = missing_root / "field5_runtime.bin"
            build = root / "build"

            configure = subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(REPOSITORY),
                    "-B",
                    str(build),
                    "-DBUILD_TESTING=OFF",
                    "-DPSX_RECOMP_UI=OFF",
                    "-DXG_RENDER_VALIDATE_OVERLAYS=OFF",
                    f"-DXG_RENDER_MANIFEST={manifest}",
                    f"-DXG_RENDER_GAME_EXE={exe}",
                    f"-DXG_RENDER_RUNTIME_VARIANTS={runtime_variants}",
                    f"-DXG_RENDER_OVERLAYS={missing_overlays}",
                    f"-DXG_RENDER_RUNTIME_ARTIFACT={missing_artifact}",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                configure.returncode, 0, configure.stdout + configure.stderr)
            self.assertTrue(exe.is_file())
            self.assertFalse(missing_root.exists())

            build_result = subprocess.run(
                [
                    "cmake",
                    "--build",
                    str(build),
                    "--target",
                    "xg_render_manifest_table",
                    "xg_render_runtime_variant_table",
                    "xg_render_auth",
                    "psx-runtime",
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(
                build_result.returncode, 0, build_result.stdout + build_result.stderr)
            manifest_table = build / "generated" / "xg_render_manifest_table.c"
            self.assertTrue(manifest_table.is_file())
            generated_manifest = manifest_table.read_text(encoding="ascii")
            self.assertIn('"field-image"', generated_manifest)
            self.assertIn('"world-image"', generated_manifest)
            self.assertIn('"battle-image"', generated_manifest)
            self.assertIn('"future-image_v2"', generated_manifest)
            self.assertTrue(
                (build / "generated" / "xg_render_runtime_variant_table.c").is_file())
            self.assertFalse(missing_root.exists())

    @unittest.skipUnless(shutil.which("cmake"), "cmake is required")
    def test_manifest_edit_regenerates_cmake_table_target(self) -> None:
        # Given
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, exe, overlays = write_fixture(root)
            build = root / "build"
            configure = subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(REPOSITORY),
                    "-B",
                    str(build),
                    "-DBUILD_TESTING=OFF",
                    "-DPSX_RECOMP_UI=OFF",
                    f"-DXG_RENDER_MANIFEST={manifest}",
                    f"-DXG_RENDER_GAME_EXE={exe}",
                    f"-DXG_RENDER_OVERLAYS={overlays}",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(configure.returncode, 0, configure.stdout + configure.stderr)
            table = build / "generated" / "xg_render_manifest_table.c"

            # When
            first_build = subprocess.run(
                ["cmake", "--build", str(build), "--target", "xg_render_manifest_table"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(first_build.returncode, 0, first_build.stdout + first_build.stderr)
            first = table.read_bytes()
            manifest.write_text(
                manifest.read_text(encoding="utf-8").replace("field-ot", "field-ot-next"),
                encoding="utf-8",
                newline="\n",
            )
            second_build = subprocess.run(
                ["cmake", "--build", str(build), "--target", "xg_render_manifest_table"],
                capture_output=True,
                text=True,
                check=False,
            )

            # Then
            self.assertEqual(second_build.returncode, 0, second_build.stdout + second_build.stderr)
            self.assertNotEqual(first, table.read_bytes())


if __name__ == "__main__":
    unittest.main()
