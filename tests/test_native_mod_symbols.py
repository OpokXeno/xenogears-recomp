"""Keep annotation resolution and generated live-code checks consistent."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from native_mod_symbols import Catalog, DEFAULT_ANNOTATIONS, annotation_name
from generate_native_mod_symbols import generate


class SymbolTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="xg-symbol-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.annotations = self.root / "annotations"
        (self.annotations / "overlays").mkdir(parents=True)
        (self.annotations / "slus_006.64_annotations.csv").write_text(
            "0x80010000, Transform(unsigned count, void* data) — useful prose, with commas\n"
            "0x80010020, _Transform — different name\n")
        self.aot = self.root / "aot"
        (self.aot / "inputs").mkdir(parents=True)
        (self.aot / "metadata").mkdir()
        self.payload = bytes(range(64))
        index = 'schema = "xenogears-overlay-annotations/v1"\n'
        for owner in ("scene-a", "scene-b"):
            # Identical function code needs an image identity check, even when
            # the caller qualified the name. Different data alone is excluded.
            data = self.payload[:32] + (b"A" if owner == "scene-a" else b"B") * 32
            digest = hashlib.sha256(data).hexdigest()
            index += f'[[images]]\nid = "{owner}"\nannotations = "annotations/overlays/{owner}.csv"\n'
            index += f'load_address = "0x80012000"\nsize = 64\nloaded_size = 64\nsha256 = "{digest}"\n'
            (self.annotations / f"overlays/{owner}.csv").write_text(
                "0x80012000, function start — Shared; original function\n"
                "0x80012004, instruction site — NotFunction\n")
            (self.aot / f"inputs/{owner}.bin").write_bytes(data)
            metadata = {"input_sha256": digest, "image": {"ranges": [[0x12000, 64]], "code_sha256": digest},
                        "variants": [{"addr": 0x80012000, "producer_entry": 0x80012000, "resume": 0,
                                      "ranges": [[0x80012000, 16]], "code_sha256": hashlib.sha256(data[:16]).hexdigest()}]}
            (self.aot / f"metadata/{owner}.json").write_text(json.dumps(metadata))
        (self.annotations / "overlays/index.toml").write_text(index)
        header = bytearray(0x800); header[:8] = b"PS-X EXE"
        header[0x18:0x1c] = (0x80010000).to_bytes(4, "little")
        header[0x1c:0x20] = len(self.payload).to_bytes(4, "little")
        self.exe = self.root / "game.exe"; self.exe.write_bytes(header + self.payload)
        self.catalog = Catalog(self.annotations)

    def test_annotation_names_preserve_identifiers_and_remove_signatures(self):
        self.assertEqual(annotation_name("_SendPAD — PsyQ"), "_SendPAD")
        self.assertEqual(annotation_name("Transform(unsigned n) — prose"), "Transform")
        self.assertEqual(annotation_name("Kernel MENU — prose"), "Kernel_MENU")

    def test_qualified_names_offsets_and_hex_aliases(self):
        self.assertEqual(self.catalog.resolve("Transform+0x10").address, 0x80010010)
        self.assertEqual(self.catalog.resolve("scene-a/Shared").address, 0x80012000)
        self.assertEqual(self.catalog.resolve("resident/func_80010000").address, 0x80010000)
        self.assertIsNone(self.catalog.resolve("0xa0010000").symbol)
        with self.assertRaisesRegex(ValueError, "ambiguous.*scene-a/Shared.*scene-b/Shared"):
            self.catalog.resolve("Shared")
        for name in ("transform", "NotFunction", "Shared+3", "Transform+0x200000"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.catalog.resolve(name)

    def test_generated_catalog_is_deterministic_and_checks_shared_code_owner(self):
        source = generate(self.catalog, self.exe, self.aot)
        self.assertEqual(source, generate(self.catalog, self.exe, self.aot))
        self.assertIn('"scene-a/Shared"', source)
        self.assertIn('"scene-b/Shared"', source)
        self.assertEqual(source.count('{"Shared",'), 2)
        self.assertIn("(!g->scope || available(g->scope))", source)
        # Both per-function fingerprints point to distinct image guard records.
        self.assertEqual(source.count("nullptr};"), 4)  # two resident + two image guards
        (self.aot / "inputs/scene-a.bin").write_bytes(bytes(64))
        with self.assertRaisesRegex(ValueError, "source identity"):
            generate(self.catalog, self.exe, self.aot)

    def test_paths_duplicates_and_untrusted_metadata_fail_closed(self):
        csv = self.annotations / "overlays/scene-a.csv"
        csv.write_text(csv.read_text() + "0x80012010, function start — Shared; duplicate\n")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            Catalog(self.annotations)
        metadata = self.aot / "metadata/scene-a.json"
        data = json.loads(metadata.read_text()); data["variants"][0]["code_sha256"] = "0" * 64
        metadata.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "function code fingerprint"):
            generate(self.catalog, self.exe, self.aot)

    def test_repository_names_use_the_same_catalog(self):
        catalog = Catalog(DEFAULT_ANNOTATIONS)
        self.assertEqual(catalog.resolve("WaitForVerticalRetrace").address, 0x8004B54C)
        self.assertEqual(catalog.resolve("battle-overlay/BattleMain").address, 0x80070F40)
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            catalog.resolve("MeasureSpriteFrameBounds")
        with self.assertRaisesRegex(ValueError, "supported 2 MiB"):
            catalog.resolve("BattleDebugFrame")


if __name__ == "__main__":
    unittest.main()
