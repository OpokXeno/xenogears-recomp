"""Exercise the author-facing packaging CLI without executing native code."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib
import unittest
import zipfile


PACKER = Path(__file__).resolve().parents[1] / "examples/native-hook/package.py"


class NativeHookPackageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="xg-native-author-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.library = self.root / "mod.so"
        self.library.write_bytes(b"Not loaded or executed by the packer")
        self.payload = bytes(range(256))
        header = bytearray(0x800)
        header[:8] = b"PS-X EXE"
        header[0x18:0x1c] = (0x80010000).to_bytes(4, "little")
        header[0x1c:0x20] = len(self.payload).to_bytes(4, "little")
        self.exe = self.root / "game.exe"
        self.exe.write_bytes(header + self.payload)
        self.overlay = self.root / "scene code.bin"
        self.overlay.write_bytes(bytes(reversed(range(128))))
        self.annotations = self.root / "annotations"
        (self.annotations / "overlays").mkdir(parents=True)
        (self.annotations / "slus_006.64_annotations.csv").write_text(
            "0x80010004, Transform(unsigned value) — example function\n"
            "0x80010080, _Internal — underscore is part of the name\n")
        digest = hashlib.sha256(self.overlay.read_bytes()).hexdigest()
        (self.annotations / "overlays/index.toml").write_text(
            'schema = "xenogears-overlay-annotations/v1"\n'
            f'[[images]]\nid = "scene"\nannotations = "annotations/overlays/scene.csv"\n'
            f'load_address = "0x80012000"\nloaded_size = 128\nsha256 = "{digest}"\n')
        (self.annotations / "overlays/scene.csv").write_text(
            "0x80012004, function start — SceneTick; runs a frame.\n"
            "0x80012008, call site — NotAFunction.\n")

    def run_cli(self, *flags, success=True):
        output = self.root / "mod.psxmod"
        result = subprocess.run(
            [sys.executable, str(PACKER), str(self.library), str(output), *flags],
            capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0 if success else 2, result.stderr)
        if not success:
            self.assertNotIn("Traceback", result.stderr)
            return
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(set(archive.namelist()), {"manifest.toml", "native/hook.so"})
            self.assertEqual(archive.read("native/hook.so"), self.library.read_bytes())
            manifest = tomllib.loads(archive.read("manifest.toml").decode())
        self.assertEqual(manifest["format_version"], 9)
        self.assertEqual(manifest["native_module"][0]["sha256"], hashlib.sha256(self.library.read_bytes()).hexdigest())
        return manifest, output.read_bytes()

    def test_function_needs_only_exe_and_address(self):
        manifest, first = self.run_cli("--exe", str(self.exe), "--hook", "0x80010004", "--platform", "linux-x86_64")
        hook = manifest["native_module"][0]["hook"][0]
        self.assertEqual(hook["expected"], self.payload[4:20].hex())
        self.assertEqual(manifest["target"][0]["exe_sha256"], hashlib.sha256(self.exe.read_bytes()).hexdigest())
        _, second = self.run_cli("--exe", str(self.exe), "--hook", "0x80010004", "--platform", "linux-x86_64")
        self.assertEqual(first, second)

    def test_partial_and_multiple_function_guards_are_generated(self):
        manifest, _ = self.run_cli("--exe", str(self.exe), "--hook", "0x00010000",
                                  "--hook", "0xa0010080", "--block", "0x80010020:0x80010070",
                                  "--platform", "linux-x86_64")
        hooks = manifest["native_module"][0]["hook"]
        self.assertEqual(len(hooks), 3)
        self.assertEqual(hooks[0]["expected"], self.payload[:16].hex())
        self.assertEqual(hooks[1]["expected"], self.payload[128:144].hex())
        self.assertEqual(hooks[2]["expected"], self.payload[32:112].hex())
        self.assertEqual(hooks[2]["resume_address"], 0x80010070)

    def test_overlay_guards_use_mapped_file(self):
        manifest, _ = self.run_cli("--image", f"0x80012000:{self.overlay}",
                                  "--block", "0x80012004:0x80012024", "--platform", "linux-x86_64")
        self.assertEqual(manifest["native_module"][0]["hook"][0]["expected"], self.overlay.read_bytes()[4:36].hex())
        self.assertNotIn("exe_sha256", manifest["target"][0])

    def test_long_function_guards_and_manual_guard_compatibility(self):
        manifest, _ = self.run_cli("--exe", str(self.exe), "--address", "0x80010000", "--guard-bytes", "80",
                                  "--platform", "linux-x86_64")
        self.assertEqual(manifest["native_module"][0]["hook"][0]["expected"], self.payload[:80].hex())
        manifest, _ = self.run_cli("--address", "0x80010000", "--expected", self.payload[:80].hex(),
                                  "--platform", "linux-x86_64")
        self.assertEqual(manifest["native_module"][0]["hook"][0]["expected"], self.payload[:80].hex())

    def test_custom_package_metadata(self):
        manifest, _ = self.run_cli("--exe", str(self.exe), "--hook", "0x80010000",
                                  "--id", "coder.my-hook", "--version", "1.2.3-beta",
                                  "--name", 'A "custom" hook 🎮', "--author", "Coder", "--platform", "linux-x86_64")
        self.assertEqual(manifest["id"], "coder.my-hook")
        self.assertEqual(manifest["version"], "1.2.3-beta")
        self.assertEqual(manifest["name"], 'A "custom" hook 🎮')
        self.assertEqual(manifest["author"], "Coder")

    def test_ram_snapshot_can_start_at_zero(self):
        snapshot = self.root / "ram.bin"
        snapshot.write_bytes(bytes(0x10000) + self.payload)
        manifest, _ = self.run_cli("--image", f"0x0:{snapshot}", "--hook", "0x80010004", "--platform", "linux-x86_64")
        self.assertEqual(manifest["native_module"][0]["hook"][0]["expected"], self.payload[4:20].hex())

    def test_output_cannot_overwrite_source_code_or_library(self):
        for path in (self.exe, self.library):
            before = path.read_bytes()
            result = subprocess.run(
                [sys.executable, str(PACKER), str(self.library), str(path), "--exe", str(self.exe), "--hook", "0x80010000"],
                capture_output=True, text=True, timeout=10,
            )
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertEqual(path.read_bytes(), before)

    def test_names_and_hex_produce_identical_format9_packages(self):
        named, first = self.run_cli("--annotations", str(self.annotations), "--exe", str(self.exe),
                                    "--hook", "Transform", "--platform", "linux-x86_64")
        numeric, second = self.run_cli("--exe", str(self.exe), "--hook", "0x80010004", "--platform", "linux-x86_64")
        self.assertEqual(named, numeric)
        self.assertEqual(first, second)
        self.run_cli("--annotations", str(self.annotations), "--exe", str(self.exe),
                     "--address", "resident/Transform", "--hook", "_Internal", "--platform", "linux-x86_64")

    def test_partial_names_offsets_and_mixed_hex_addresses(self):
        manifest, _ = self.run_cli("--annotations", str(self.annotations), "--exe", str(self.exe),
                                  "--block", "Transform+0x20:resident/Transform+0x40", "--hook", "0x80010080",
                                  "--platform", "linux-x86_64")
        hook = manifest["native_module"][0]["hook"][1]
        self.assertEqual(hook["address"], 0x80010024)
        self.assertEqual(hook["resume_address"], 0x80010044)
        self.assertEqual(hook["expected"], self.payload[36:68].hex())

    def test_named_overlay_source_is_authenticated(self):
        manifest, _ = self.run_cli("--annotations", str(self.annotations), "--image", f"0x80012000:{self.overlay}",
                                  "--hook", "scene/SceneTick", "--platform", "linux-x86_64")
        self.assertEqual(manifest["native_module"][0]["hook"][0]["address"], 0x80012004)
        self.overlay.write_bytes(bytes(128))
        self.run_cli("--annotations", str(self.annotations), "--image", f"0x80012000:{self.overlay}",
                     "--hook", "SceneTick", success=False)

    def test_overlay_names_disambiguate_sources_at_the_same_address(self):
        other = self.root / "other.bin"; other.write_bytes(bytes(range(128)))
        index = self.annotations / "overlays/index.toml"
        index.write_text(index.read_text() +
                         f'[[images]]\nid = "other"\nannotations = "annotations/overlays/other.csv"\n'
                         f'load_address = "0x80012000"\nloaded_size = 128\nsha256 = "{hashlib.sha256(other.read_bytes()).hexdigest()}"\n')
        (self.annotations / "overlays/other.csv").write_text("0x80012004, function start — SceneTick; other image\n")
        manifest, _ = self.run_cli("--annotations", str(self.annotations), "--image", f"0x80012000:{other}",
                                  "--image", f"0x80012000:{self.overlay}", "--hook", "scene/SceneTick",
                                  "--platform", "linux-x86_64")
        self.assertEqual(manifest["native_module"][0]["hook"][0]["expected"], self.overlay.read_bytes()[4:20].hex())
        result = subprocess.run([sys.executable, str(PACKER), str(self.library), str(self.root / "ambiguous.psxmod"),
                                 "--annotations", str(self.annotations), "--hook", "SceneTick"],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertIn("scene/SceneTick", result.stderr); self.assertIn("other/SceneTick", result.stderr)
        self.run_cli("--annotations", str(self.annotations), "--exe", str(self.exe),
                     "--block", "Transform:scene/SceneTick", success=False)

    def test_name_errors_and_annotation_output_protection(self):
        for name in ("Typo", "transform", "Transform+3", "Transform+-4", "Transform+0x200000", "NotAFunction"):
            self.run_cli("--annotations", str(self.annotations), "--exe", str(self.exe), "--hook", name, success=False)
        path = self.annotations / "slus_006.64_annotations.csv"
        before = path.read_bytes()
        result = subprocess.run([sys.executable, str(PACKER), str(self.library), str(path), "--annotations", str(self.annotations),
                                 "--exe", str(self.exe), "--hook", "Transform"], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(path.read_bytes(), before)

    def test_function_listing_needs_no_library_or_original_game(self):
        result = subprocess.run([sys.executable, str(PACKER), "--annotations", str(self.annotations),
                                 "--list-functions", "Transform"], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "resident/Transform\t0x80010004\n")

    def test_invalid_sources_and_declarations_fail_without_traceback(self):
        cases = [
            ("--hook", "0x80010000"),
            ("--expected", "00000000"),
            ("--exe", str(self.exe), "--hook", "0x80010001"),
            ("--exe", str(self.exe), "--hook", "0x80010100"),
            ("--exe", str(self.exe), "--hook", "0x80010000", "--guard-bytes", "3"),
            ("--exe", str(self.exe), "--block", "0x80010020:0x80010010"),
            ("--exe", str(self.exe), "--hook", "0x80010000", "--hook", "0xa0010000"),
            ("--exe", str(self.exe), "--block", "0x80010000:0x80010020", "--block", "0x80010010:0x80010030"),
            ("--exe", str(self.exe), "--block", "0x80010000:0x80010020", "--hook", "0xa0010010"),
            ("--exe", str(self.exe), "--image", f"0x80010000:{self.overlay}", "--hook", "0x80010000"),
            ("--exe", str(self.overlay), "--hook", "0x80010000"),
            ("--image", f"0x80012001:{self.overlay}", "--hook", "0x80012004"),
            ("--exe", str(self.root / "missing.exe"), "--hook", "0x80010000"),
            ("--block", "0x80010000:0x80010008:00000000"),
            ("--exe", str(self.exe), "--hook", "0x80010000", "--id", "../outside"),
            ("--exe", str(self.exe), "--hook", "0x80010000", "--version", "2"),
        ]
        for flags in cases:
            with self.subTest(flags=flags):
                self.run_cli(*flags, success=False)
        self.exe.write_bytes(self.exe.read_bytes()[:-4])
        self.run_cli("--exe", str(self.exe), "--hook", "0x80010000", success=False)


if __name__ == "__main__":
    unittest.main()
