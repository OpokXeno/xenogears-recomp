#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///

from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_native_render_manifest import write_fixture


REPOSITORY = Path(__file__).resolve().parents[3]
TOOL = REPOSITORY / "tools" / "native_render_manifest.py"
VALIDATION_FIELDS = {
    "producer_record_id",
    "site_record_id",
    "field_base_crc32",
    "field_range_crc32",
    "field_range_start",
    "field_range_size",
    "producer_entry",
    "caller_site",
    "static_callee",
    "return_site",
    "instruction_window_start",
    "instruction_window_size",
    "instruction_window_identity",
    "required_jal_opcode",
    "jal_target",
    "required_delay_slot_instructions",
    "required_delay_slot_non_control_transfer",
}


class NativeRenderManifestAuthMetadataTests(unittest.TestCase):
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

    def emit_fixture(self, root: Path) -> tuple[Path, Path, Path, Path, tuple[str, ...]]:
        manifest, exe, overlays = write_fixture(root)
        table = root / "table.json"
        output = root / "table.c"
        arguments = (
            "emit", str(manifest), "--exe", str(exe), "--overlays",
            str(overlays), "--out", str(output), "--metadata-out", str(table),
        )
        self.run_tool(*arguments)
        return manifest, exe, overlays, table, arguments

    def test_emits_closed_runtime_validation_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _, _, _, table, _ = self.emit_fixture(root)
            payload = json.loads(table.read_text(encoding="utf-8"))
            generated = (root / "table.c").read_text(encoding="ascii")

            self.assertEqual(
                set(payload),
                {"schema", "game_identity", "manifest_identity", "namespace_crc32", "records", "validation"},
            )
            self.assertEqual(
                [record["record_id"] for record in payload["records"]],
                [5, 2, 4, 1, 3, 6],
            )
            self.assertEqual(set(payload["validation"]), VALIDATION_FIELDS)
            self.assertIn('#include "xg_render_manifest_generated.h"', generated)
            self.assertIn("xg_render_manifest_validation", generated)
            self.assertNotIn("typedef struct", generated)
            self.assertNotIn(str(root), generated)
            self.assertNotIn("field.bin", generated)

    def test_rejects_missing_or_malformed_runtime_validation_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, exe, overlays, table, arguments = self.emit_fixture(root)
            payload = json.loads(table.read_text(encoding="utf-8"))
            del payload["validation"]["return_site"]
            table.write_text(json.dumps(payload), encoding="utf-8")
            result = self.run_tool(
                "check-metadata", str(manifest), "--exe", str(exe),
                "--overlays", str(overlays), "--table", str(table), expect=1,
            )
            self.assertIn("runtime validation metadata mismatch", result.stderr.lower())

            self.run_tool(*arguments)
            payload = json.loads(table.read_text(encoding="utf-8"))
            payload["validation"]["required_jal_opcode"] = "3"
            table.write_text(json.dumps(payload), encoding="utf-8")
            result = self.run_tool(
                "check-metadata", str(manifest), "--exe", str(exe),
                "--overlays", str(overlays), "--table", str(table), expect=1,
            )
            self.assertIn("runtime validation metadata mismatch", result.stderr.lower())

    @unittest.skipUnless(shutil.which("cc"), "a C compiler is required")
    def test_generated_metadata_is_strict_c11(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.emit_fixture(root)
            result = subprocess.run(
                [
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-pedantic", "-fsyntax-only", "-I",
                    str(REPOSITORY / "native_renderer" / "include"),
                    str(root / "table.c"),
                ],
                cwd=REPOSITORY,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(shutil.which("cc"), "a C compiler is required")
    def test_public_generated_metadata_abi_has_a_compiled_consumer(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.emit_fixture(root)
            result = subprocess.run(
                [
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-pedantic", "-I",
                    str(REPOSITORY / "native_renderer" / "include"),
                    str(root / "table.c"),
                    str(REPOSITORY / "native_renderer" / "tests" / "contracts" /
                        "test_xg_render_manifest_generated_consumer.c"),
                    "-o", str(root / "generated-consumer"),
                ],
                cwd=REPOSITORY,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(
                [str(root / "generated-consumer")],
                cwd=REPOSITORY,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
