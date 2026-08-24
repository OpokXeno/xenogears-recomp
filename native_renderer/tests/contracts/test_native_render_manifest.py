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
import subprocess
import sys
import tempfile
import unittest
import zlib


REPOSITORY = Path(__file__).resolve().parents[3]
TOOL = REPOSITORY / "tools" / "native_render_manifest.py"
CANONICAL = REPOSITORY / "native_renderer" / "xg_render_manifest.toml"
FIELD_BASE = 0x8006F000
HEADER_SIZE = 0x800
PRODUCER_ADDRESS = 0x80075B44
SITE_ADDRESS = 0x800781BC
VSYNC_ADDRESS = 0x8004B54C


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def crc32(data: bytes) -> str:
    return f"{zlib.crc32(data) & 0xFFFFFFFF:08x}"


def psx_exe(payload: bytes, base_address: int) -> bytes:
    header = bytearray(HEADER_SIZE)
    header[:8] = b"PS-X EXE"
    header[0x10:0x14] = base_address.to_bytes(4, "little")
    header[0x18:0x1c] = base_address.to_bytes(4, "little")
    header[0x1c:0x20] = len(payload).to_bytes(4, "little")
    return bytes(header) + payload


def write_fixture(root: Path) -> tuple[Path, Path, Path]:
    exe_payload = bytes(range(256)) * 1176
    exe_bytes = psx_exe(exe_payload, 0x80010000)
    field = bytearray((index * 17 + 3) & 0xFF for index in range(0x9200))
    call_offset = SITE_ADDRESS - FIELD_BASE
    call_word = (3 << 26) | ((VSYNC_ADDRESS >> 2) & 0x03FFFFFF)
    field[call_offset : call_offset + 4] = call_word.to_bytes(4, "little")
    field[call_offset + 4 : call_offset + 8] = (0x34040002).to_bytes(4, "little")
    field_payload = bytes(field)
    field_bytes = psx_exe(field_payload, FIELD_BASE)
    window_start = SITE_ADDRESS - 8
    window_offset = HEADER_SIZE + window_start - FIELD_BASE
    window = field_bytes[window_offset : window_offset + 16]
    manifest = root / "manifest.toml"
    exe = root / "main.exe"
    overlays = root / "overlays"
    overlays.mkdir()
    exe.write_bytes(exe_bytes)
    (overlays / "field.bin").write_bytes(field_bytes)
    manifest.write_text(
        f'''schema = "xg-render-manifest/v3"

[game]
id = "main-disc1-exe"
serial = "SLUS-00664"
disc = 1
sha256 = "{sha256(exe_bytes)}"
crc32 = "{crc32(exe_bytes)}"
size = {len(exe_bytes)}
namespace_crc32 = "89abcdef"
base_address = "0x80010000"
image_format = "ps-x-exe"
header_size = {HEADER_SIZE}
loaded_size = {len(exe_payload)}

[overlay.field]
id = "field-image"
state = "authenticated"
file = "field.bin"
full_sha256 = "{sha256(field_bytes)}"
full_crc32 = "{crc32(field_bytes)}"
full_size = {len(field_bytes)}
base_address = "0x{FIELD_BASE:08x}"
image_format = "ps-x-exe"
header_size = {HEADER_SIZE}
loaded_size = {len(field_payload)}
range_offset = 0
range_size = {len(field_payload)}
range_crc32 = "{crc32(field_payload)}"

[[functions]]
id = "draw-otag"
image = "main-disc1-exe"
name = "DrawOTag"
entry_address = "0x80044bd0"
confidence = "verified"

[[functions]]
id = "vsync"
image = "main-disc1-exe"
name = "VSync"
entry_address = "0x{VSYNC_ADDRESS:08x}"
confidence = "verified"

[[producers]]
id = "render-field-character-sprites"
image = "field-image"
name = "RenderFieldCharacterSprites"
entry_address = "0x{PRODUCER_ADDRESS:08x}"
framebuffer_context = "field-double-buffer"
ot_context = "field-ot"
confidence = "verified"

[[sites]]
id = "field-vsync-call-0x800781bc"
state = "authenticated"
producer = "render-field-character-sprites"
image = "field-image"
callee = "vsync"
call_address = "0x{SITE_ADDRESS:08x}"
return_address = "0x{SITE_ADDRESS + 8:08x}"
window_start = "0x{window_start:08x}"
window_size = 16
window_sha256 = "{sha256(window)}"
framebuffer_context = "field-double-buffer"
ot_context = "field-ot"
confidence = "verified"
''',
        encoding="utf-8",
        newline="\n",
    )
    return manifest, exe, overlays


def add_artifact_overlay(
        manifest: Path, overlays: Path, key: str, identifier: str,
        file_name: str, multiplier: int, source_file: int | None,
        source_size_field: str = "compressed_size") -> bytes:
    artifact = bytes(
        (index * multiplier + 7) & 0xFF for index in range(0x4800))
    (overlays / file_name).write_bytes(artifact)
    source = ""
    if source_file is not None:
        source = f'''source_disc = 1
source_directory = 1
source_file = {source_file}
source_sector = {108980 + source_file}
{source_size_field} = 9216
'''
    record = f'''[overlay.{key}]
id = "{identifier}"
state = "authenticated"
file = "{file_name}"
full_sha256 = "{sha256(artifact)}"
full_crc32 = "{crc32(artifact)}"
full_size = {len(artifact)}
base_address = "0x8006faf0"
image_format = "raw"
header_size = 0
loaded_size = {len(artifact)}
{source}

'''
    manifest.write_text(
        manifest.read_text(encoding="utf-8").replace(
            "[[functions]]", record + "[[functions]]", 1),
        encoding="utf-8",
        newline="\n",
    )
    return artifact


def add_world_overlay(manifest: Path, overlays: Path) -> bytes:
    return add_artifact_overlay(
        manifest, overlays, "world", "world-image", "world.bin", 29, 15)


class NativeRenderManifestTests(unittest.TestCase):
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

    def test_validate_and_emit_are_deterministic_metadata_only(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, exe, overlays = write_fixture(root)
            output = root / "table.c"
            metadata = root / "table.json"
            self.run_tool("validate", str(manifest), "--exe", str(exe), "--overlays", str(overlays))
            self.run_tool("emit", str(manifest), "--exe", str(exe), "--overlays", str(overlays), "--out", str(output), "--metadata-out", str(metadata))
            first = output.read_bytes()
            self.run_tool("emit", str(manifest), "--exe", str(exe), "--overlays", str(overlays), "--out", str(output), "--metadata-out", str(metadata))
            self.assertEqual(first, output.read_bytes())
            self.assertNotIn(b"\r", first)
            self.assertNotIn(str(root).encode(), first)
            self.assertNotIn(b"field.bin", first)
            text = first.decode("ascii")
            self.assertNotRegex(text, r'const char .*sha256')
            expected_identities = {
                "xg_render_game_identity": hashlib.sha256(exe.read_bytes()).digest(),
                "xg_render_manifest_identity": hashlib.sha256(manifest.read_bytes()).digest(),
            }
            for symbol in ("xg_render_game_identity", "xg_render_manifest_identity"):
                body = re.search(rf"{symbol}\[32\] = \{{([^}}]+)\}}", text)
                self.assertIsNotNone(body)
                self.assertEqual(len(body.group(1).split(",")), 32)
                self.assertEqual(bytes(int(value, 16) for value in body.group(1).split(",")), expected_identities[symbol])
            self.assertNotEqual(expected_identities["xg_render_game_identity"], expected_identities["xg_render_manifest_identity"])
            positions = [text.index(f'"{identifier}"') for identifier in sorted(("draw-otag", "field-image", "field-vsync-call-0x800781bc", "main-disc1-exe", "render-field-character-sprites", "vsync"))]
            self.assertEqual(positions, sorted(positions))

    def test_validate_and_emit_optional_raw_world_overlay(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, exe, overlays = write_fixture(root)
            add_world_overlay(manifest, overlays)
            output = root / "table.c"

            self.run_tool("validate", str(manifest), "--exe", str(exe), "--overlays", str(overlays))
            self.run_tool(
                "emit", str(manifest), "--exe", str(exe), "--overlays",
                str(overlays), "--out", str(output))

            generated = output.read_text(encoding="ascii")
            self.assertIn('"field-image"', generated)
            self.assertIn('"world-image"', generated)
            self.assertIn("xg_render_manifest_record_count = 7u", generated)

    def test_hyphen_and_underscore_overlay_tokens_emit_sequential_ids(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, exe, overlays = write_fixture(root)
            add_world_overlay(manifest, overlays)
            add_artifact_overlay(
                manifest, overlays, "battle", "battle-image", "battle.bin", 31, 16)
            add_artifact_overlay(
                manifest, overlays, "future_render", "future-image_v2",
                "future_overlay.bin", 37, None)
            output = root / "table.c"
            metadata = root / "table.json"

            self.run_tool(
                "emit", str(manifest), "--exe", str(exe), "--overlays",
                str(overlays), "--out", str(output), "--metadata-out", str(metadata))

            records = json.loads(metadata.read_text(encoding="utf-8"))["records"]
            self.assertEqual(
                records[-3:],
                [
                    {"id": "world-image", "record_id": 7},
                    {"id": "battle-image", "record_id": 8},
                    {"id": "future-image_v2", "record_id": 9},
                ],
            )
            generated = output.read_text(encoding="ascii")
            positions = [
                generated.index(f'"{identifier}"')
                for identifier in ("world-image", "battle-image", "future-image_v2")
            ]
            self.assertEqual(positions, sorted(positions))
            self.assertIn("xg_render_manifest_record_count = 9u", generated)

    def test_explicit_validation_authenticates_every_declared_overlay(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, exe, overlays = write_fixture(root)
            add_world_overlay(manifest, overlays)
            add_artifact_overlay(
                manifest, overlays, "future-render", "future-image",
                "future_overlay.bin", 37, None)
            future = overlays / "future_overlay.bin"
            future.write_bytes(future.read_bytes()[:-1] + b"X")

            result = self.run_tool(
                "validate", str(manifest), "--exe", str(exe), "--overlays",
                str(overlays), expect=1)

            self.assertIn("future-image full identity mismatch", result.stderr)

    def test_declared_commands_do_not_probe_overlay_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, _, overlays = write_fixture(root)
            add_world_overlay(manifest, overlays)
            add_artifact_overlay(
                manifest, overlays, "future-render", "future-image",
                "future_overlay.bin", 37, None)
            for artifact in overlays.iterdir():
                artifact.unlink()

            self.run_tool("metadata-declared", str(manifest))
            output = root / "table.c"
            self.run_tool("emit-declared", str(manifest), "--out", str(output))

            self.assertIn('"future-image"', output.read_text(encoding="ascii"))

    def test_artifact_overlay_schema_is_closed_and_enforces_mapping_and_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest, _, overlays = write_fixture(Path(directory))
            add_world_overlay(manifest, overlays)
            original = manifest.read_text(encoding="utf-8")
            cases = (
                ("header", "header_size = 0", "header_size = 2048", "headerless full image"),
                ("range", "loaded_size = 18432", "loaded_size = 18431", "full image"),
                ("disc", "source_disc = 1", "source_disc = 2", "Disc 1"),
                ("closed", "compressed_size = 9216", "compressed_size = 9216\nunknown = 1", "fields are not closed"),
            )
            for name, source, replacement, expected in cases:
                with self.subTest(name=name):
                    manifest.write_text(
                        original.replace(source, replacement, 1),
                        encoding="utf-8", newline="\n")
                    result = self.run_tool(
                        "metadata-declared", str(manifest), expect=1)
                    self.assertIn(expected.lower(), result.stderr.lower())

    def test_archive_size_provenance_is_accepted_and_remains_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest, _, overlays = write_fixture(Path(directory))
            add_artifact_overlay(
                manifest, overlays, "archive_menu", "archive-menu_image",
                "archive_menu.bin", 41, 9, "archive_size")

            self.run_tool("metadata-declared", str(manifest))
            original = manifest.read_text(encoding="utf-8")
            cases = (
                ("unknown", "archive_size = 9216\nunknown = 1"),
                ("duplicate-size", "archive_size = 9216\ncompressed_size = 9216"),
            )
            for name, replacement in cases:
                with self.subTest(name=name):
                    manifest.write_text(
                        original.replace("archive_size = 9216", replacement),
                        encoding="utf-8", newline="\n")
                    result = self.run_tool(
                        "metadata-declared", str(manifest), expect=1)
                    self.assertIn("fields are not closed", result.stderr)

    def test_rejects_duplicate_optional_overlay_id(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest, _, overlays = write_fixture(Path(directory))
            add_world_overlay(manifest, overlays)
            add_artifact_overlay(
                manifest, overlays, "future-render", "world-image",
                "future_overlay.bin", 37, None)

            result = self.run_tool("metadata-declared", str(manifest), expect=1)

            self.assertIn("duplicate manifest record id", result.stderr)

    def test_rejects_missing_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest, exe, overlays = write_fixture(Path(directory))
            manifest.write_text(manifest.read_text().replace(next(line for line in manifest.read_text().splitlines(keepends=True) if line.startswith("sha256 = ")), ""), newline="\n")
            result = self.run_tool("validate", str(manifest), "--exe", str(exe), "--overlays", str(overlays), expect=1)
            self.assertIn("missing identity", result.stderr.lower())

    def test_rejects_full_game_identity_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest, exe, overlays = write_fixture(Path(directory))
            exe.write_bytes(exe.read_bytes()[:-1] + b"X")
            result = self.run_tool("validate", str(manifest), "--exe", str(exe), "--overlays", str(overlays), expect=1)
            self.assertIn("full game identity mismatch", result.stderr.lower())

    def test_rejects_mismatched_field_artifact_before_range_or_window_validation(self) -> None:
        # Given
        with tempfile.TemporaryDirectory() as directory:
            manifest, exe, overlays = write_fixture(Path(directory))
            mismatched = bytearray((overlays / "field.bin").read_bytes())
            mismatched[0] ^= 0xFF
            (overlays / "field-mismatch.bin").write_bytes(mismatched)
            manifest.write_text(manifest.read_text().replace('file = "field.bin"', 'file = "field-mismatch.bin"'), newline="\n")

            # When
            result = self.run_tool("validate", str(manifest), "--exe", str(exe), "--overlays", str(overlays), expect=1)

            # Then
            self.assertIn("field image full identity mismatch", result.stderr.lower())

    def test_rejects_same_namespace_with_different_full_sha(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, exe, overlays = write_fixture(root)
            table = root / "table.json"
            self.run_tool("emit", str(manifest), "--exe", str(exe), "--overlays", str(overlays), "--out", str(root / "table.c"), "--metadata-out", str(table))
            payload = json.loads(table.read_text())
            payload["game_identity"][0] ^= 0xFF
            table.write_text(json.dumps(payload), encoding="utf-8")
            result = self.run_tool("check-metadata", str(manifest), "--exe", str(exe), "--overlays", str(overlays), "--table", str(table), expect=1)
            self.assertIn("namespace collision", result.stderr.lower())

    def test_rejects_stale_manifest_digest_in_table_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, exe, overlays = write_fixture(root)
            table = root / "table.json"
            self.run_tool("emit", str(manifest), "--exe", str(exe), "--overlays", str(overlays), "--out", str(root / "table.c"), "--metadata-out", str(table))
            manifest.write_text(manifest.read_text().replace("field-ot", "field-ot-next"), newline="\n")
            result = self.run_tool("check-metadata", str(manifest), "--exe", str(exe), "--overlays", str(overlays), "--table", str(table), expect=1)
            self.assertIn("stale table manifest identity", result.stderr.lower())

    def test_rejects_shard_export_identity_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, exe, overlays = write_fixture(root)
            table = root / "table.json"
            self.run_tool("emit", str(manifest), "--exe", str(exe), "--overlays", str(overlays), "--out", str(root / "table.c"), "--metadata-out", str(table))
            payload = json.loads(table.read_text())
            shard = root / "shard.json"
            shard.write_text(json.dumps({"schema": "xg-render-shard-export/v1", "game_identity": payload["game_identity"], "manifest_identity": [0] * 32, "namespace_crc32": payload["namespace_crc32"]}), encoding="utf-8")
            result = self.run_tool("check-metadata", str(manifest), "--exe", str(exe), "--overlays", str(overlays), "--table", str(table), "--shard-export", str(shard), expect=1)
            self.assertIn("shard-export identity mismatch", result.stderr.lower())

    def test_rejects_malformed_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest, exe, overlays = write_fixture(Path(directory))
            manifest.write_text("schema = [", newline="\n")
            self.assertIn("unreadable", self.run_tool("validate", str(manifest), "--exe", str(exe), "--overlays", str(overlays), expect=1).stderr.lower())

    def test_rejects_base_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest, exe, overlays = write_fixture(Path(directory))
            manifest.write_text(manifest.read_text().replace("0x8006f000", "0x8006f004"), newline="\n")
            self.assertIn("base mismatch", self.run_tool("validate", str(manifest), "--exe", str(exe), "--overlays", str(overlays), expect=1).stderr.lower())

    def test_rejects_crc_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest, exe, overlays = write_fixture(Path(directory))
            manifest.write_text(re.sub(r'range_crc32 = "[0-9a-f]{8}"', 'range_crc32 = "00000000"', manifest.read_text()), newline="\n")
            self.assertIn("crc mismatch", self.run_tool("validate", str(manifest), "--exe", str(exe), "--overlays", str(overlays), expect=1).stderr.lower())

    def test_rejects_delay_slot_window_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest, exe, overlays = write_fixture(Path(directory))
            manifest.write_text(re.sub(r'window_sha256 = "[0-9a-f]{64}"', f'window_sha256 = "{"0" * 64}"', manifest.read_text()), newline="\n")
            self.assertIn("delay-slot", self.run_tool("validate", str(manifest), "--exe", str(exe), "--overlays", str(overlays), expect=1).stderr.lower())

    def test_rejects_caller_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest, exe, overlays = write_fixture(Path(directory))
            manifest.write_text(manifest.read_text().replace('callee = "vsync"', 'callee = "draw-otag"'), newline="\n")
            self.assertIn("caller mismatch", self.run_tool("validate", str(manifest), "--exe", str(exe), "--overlays", str(overlays), expect=1).stderr.lower())

    def test_rejects_disc_two(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest, exe, overlays = write_fixture(Path(directory))
            manifest.write_text(manifest.read_text().replace("disc = 1", "disc = 2"), newline="\n")
            self.assertIn("disc", self.run_tool("validate", str(manifest), "--exe", str(exe), "--overlays", str(overlays), expect=1).stderr.lower())

    def test_rejects_misleading_0x800781bc_site_label(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest, exe, overlays = write_fixture(Path(directory))
            manifest.write_text(manifest.read_text().replace('call_address = "0x800781bc"', 'call_address = "0x801e87b8"'), newline="\n")
            result = self.run_tool("validate", str(manifest), "--exe", str(exe), "--overlays", str(overlays), expect=1)
            self.assertIn("required 0x800781bc site", result.stderr.lower())

    def test_canonical_contract_authenticates_private_field(self) -> None:
        result = self.run_tool("contract", str(CANONICAL))
        self.assertIn("contract PASS", result.stdout)

    def test_self_test_uses_real_fixtures_and_emits_closed_safe_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory) / "evidence.json"
            result = self.run_tool("self-test", "--evidence", str(evidence))
            payload = json.loads(evidence.read_text())
            self.assertEqual(set(payload), {"schema", "state", "positive_paths", "rejected_cases", "canonical_blockers", "privacy", "cleanup"})
            self.assertEqual(payload["state"], "pass")
            self.assertEqual(len(payload["rejected_cases"]), 5)
            self.assertTrue(payload["cleanup"]["temporary_inputs_cleaned"])
            self.assertIn("temporary fixture removed", result.stdout.lower())
            encoded = evidence.read_text()
            self.assertNotRegex(encoded, r"[0-9a-f]{64}")
            self.assertNotIn(directory, encoded)
            payload["unknown"] = True
            evidence.write_text(json.dumps(payload), encoding="utf-8")
            self.assertIn("evidence fields are not closed", self.run_tool("validate-evidence", str(evidence), expect=1).stderr.lower())


if __name__ == "__main__":
    unittest.main()
