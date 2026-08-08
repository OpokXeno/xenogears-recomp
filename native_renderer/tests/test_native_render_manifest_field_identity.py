#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///

from __future__ import annotations

import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
import zlib


REPOSITORY = Path(__file__).resolve().parents[2]
TOOL = REPOSITORY / "tools" / "native_render_manifest.py"
CANONICAL = REPOSITORY / "native_renderer" / "xg_render_manifest.toml"
HEADER_SIZE = 0x800


def psx_exe(payload: bytes, base_address: int) -> bytes:
    header = bytearray(HEADER_SIZE)
    header[:8] = b"PS-X EXE"
    header[0x10:0x14] = base_address.to_bytes(4, "little")
    header[0x18:0x1c] = base_address.to_bytes(4, "little")
    header[0x1c:0x20] = len(payload).to_bytes(4, "little")
    return bytes(header) + payload


class NativeRenderManifestFieldIdentityTests(unittest.TestCase):
    def run_tool(self, *arguments: str, expect: int) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            [sys.executable, str(TOOL), *arguments],
            cwd=REPOSITORY,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, expect, result.stderr)
        return result

    def replace_once(self, text: str, pattern: str, replacement: str) -> str:
        updated, replacements = re.subn(pattern, replacement, text, count=1, flags=re.MULTILINE)
        self.assertEqual(replacements, 1, pattern)
        return updated

    def fixture_game(self, text: str, executable: bytes) -> str:
        text = self.replace_once(
            text,
            r'^sha256 = "[0-9a-f]{64}"$',
            f'sha256 = "{hashlib.sha256(executable).hexdigest()}"',
        )
        text = self.replace_once(
            text,
            r'^crc32 = "[0-9a-f]{8}"$',
            f'crc32 = "{zlib.crc32(executable) & 0xFFFFFFFF:08x}"',
        )
        return self.replace_once(text, r'^size = [0-9]+$', f"size = {len(executable)}")

    def test_blocked_field_record_cannot_validate(self) -> None:
        executable = psx_exe(bytes(range(256)) * 1176, 0x80010000)
        canonical = CANONICAL.read_text(encoding="utf-8")
        blocked = re.sub(
            r"(?ms)^\[overlay\.field\]\n.*?(?=^\[\[functions\]\])",
            "[overlay.field]\n"
            'id = "field-image"\n'
            'state = "blocked"\n'
            'base_address = "0x8006f000"\n'
            'reason_code = "private-image-unavailable"\n\n',
            canonical,
            count=1,
        )
        self.assertNotEqual(blocked, canonical)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = root / "manifest.toml"
            exe = root / "main.exe"
            overlays = root / "overlays"
            overlays.mkdir()
            manifest.write_text(self.fixture_game(blocked, executable), encoding="utf-8", newline="\n")
            exe.write_bytes(executable)

            result = self.run_tool("validate", str(manifest), "--exe", str(exe), "--overlays", str(overlays), expect=1)

            self.assertIn("contract blocked", result.stderr.lower())

    def test_wrong_field_variant_fails_full_range_and_window_anchors(self) -> None:
        executable = psx_exe(bytes(range(256)) * 1176, 0x80010000)
        canonical = CANONICAL.read_text(encoding="utf-8")
        self.assertIn('state = "authenticated"', canonical)
        file_match = re.search(r'^file = "([A-Za-z0-9._-]+)"$', canonical, re.MULTILINE)
        size_match = re.search(r'^full_size = ([0-9]+)$', canonical, re.MULTILINE)
        offset_match = re.search(r'^range_offset = ([0-9]+)$', canonical, re.MULTILINE)
        range_match = re.search(r'^range_size = ([0-9]+)$', canonical, re.MULTILINE)
        self.assertIsNotNone(file_match)
        self.assertIsNotNone(size_match)
        self.assertIsNotNone(offset_match)
        self.assertIsNotNone(range_match)
        file_name = file_match.group(1)
        full_size = int(size_match.group(1))
        wrong_field = psx_exe(bytes(full_size - HEADER_SIZE), 0x8006F000)
        range_offset = int(offset_match.group(1))
        range_size = int(range_match.group(1))
        wrong_range_crc = zlib.crc32(
            wrong_field[HEADER_SIZE + range_offset : HEADER_SIZE + range_offset + range_size]
        ) & 0xFFFFFFFF
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = root / "manifest.toml"
            exe = root / "main.exe"
            overlays = root / "overlays"
            overlays.mkdir()
            exe.write_bytes(executable)
            (overlays / file_name).write_bytes(wrong_field)
            fixture_manifest = self.fixture_game(canonical, executable)
            manifest.write_text(fixture_manifest, encoding="utf-8", newline="\n")

            full = self.run_tool("validate", str(manifest), "--exe", str(exe), "--overlays", str(overlays), expect=1)
            self.assertIn("field image full identity mismatch", full.stderr.lower())

            fixture_manifest = self.replace_once(
                fixture_manifest,
                r'^full_sha256 = "[0-9a-f]{64}"$',
                f'full_sha256 = "{hashlib.sha256(wrong_field).hexdigest()}"',
            )
            fixture_manifest = self.replace_once(
                fixture_manifest,
                r'^full_crc32 = "[0-9a-f]{8}"$',
                f'full_crc32 = "{zlib.crc32(wrong_field) & 0xFFFFFFFF:08x}"',
            )
            manifest.write_text(fixture_manifest, encoding="utf-8", newline="\n")

            ranged = self.run_tool("validate", str(manifest), "--exe", str(exe), "--overlays", str(overlays), expect=1)
            self.assertIn("crc mismatch", ranged.stderr.lower())

            fixture_manifest = self.replace_once(
                fixture_manifest,
                r'^range_crc32 = "[0-9a-f]{8}"$',
                f'range_crc32 = "{wrong_range_crc:08x}"',
            )
            manifest.write_text(fixture_manifest, encoding="utf-8", newline="\n")

            windowed = self.run_tool("validate", str(manifest), "--exe", str(exe), "--overlays", str(overlays), expect=1)
            self.assertIn("delay-slot", windowed.stderr.lower())


if __name__ == "__main__":
    unittest.main()
