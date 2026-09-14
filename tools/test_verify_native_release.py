from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from verify_native_release import verify_build


class NativeReleaseVerificationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / "build"
        self.game_hash = hashlib.sha256(b"synthetic executable").hexdigest()
        self.manifest_hash = hashlib.sha256(b"synthetic manifest").hexdigest()
        self.write("game/fixture", "synthetic executable")
        self.write("native_renderer/xg_render_manifest.toml", "synthetic manifest")
        self.write("game.toml", '[game]\nexe = "game/fixture"\n'
                   '[runtime]\nrender_mode = "native"\noverlay_cache = false\n')
        self.write("build/CMakeCache.txt", "XG_RENDER_NATIVE:BOOL=ON\n")
        self.write("native_renderer/xg_render_resident_plan.txt",
                   "psxrecomp-source-observation-plan-v5\n"
                   "cutover 80010000 27BDFFE0 observe 00000000\n"
                   "cutover 80010004 E8B60000 observe-after 00000000\n")
        self.source = self.write("generated/fixture_full_00.c",
                                "void func(CPUState *cpu) {\n"
                                "(void)psx_xg_render_auth_native_ft4_bypass("
                                "cpu, 0x80010000u, 0x27BDFFE0u);\n"
                                "(void)psx_xg_render_auth_native_ft4_bypass("
                                "cpu, 0x80010004u, 0xE8B60000u);\n}\n")
        self.write("build/psx-runtime_generated_sources.txt", f"{self.source}\n")
        arrays = ""
        for name, digest in [("game", self.game_hash), ("manifest", self.manifest_hash)]:
            values = ",".join(f"0x{digest[i:i+2]}" for i in range(0, 64, 2))
            arrays += f"const uint8_t xg_render_{name}_identity[32] = {{{values}}};\n"
        self.table = self.write("build/generated/xg_render_manifest_table.c", arrays +
                               "const uint32_t xg_render_manifest_record_count = 1u;\n")
        self.write("build/generated/xg_render_runtime_variant_table.c",
                   "const uint32_t xg_render_runtime_variant_descriptor_count = 1u;\n")
        self.write("annotations/overlays/index.toml",
                   '[[images]]\nid = "field"\n[[images]]\nid = "world"\n')
        self.coverage = {
            "schema": "psxrecomp static overlay coverage v3",
            "game_identity_sha256": self.game_hash,
            "manifest_identity_sha256": self.manifest_hash,
            "images": [
                {"image_id": "field", "ranges": [["0x80020000", 32]]},
                {"image_id": "world", "ranges": [["0x80030000", 32]]},
            ],
        }
        self.write_coverage()

    def write(self, relative: str, content: str) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def write_coverage(self) -> None:
        self.write("build/overlay_aot/static/overlays_static_coverage.json",
                   json.dumps(self.coverage))

    def test_complete_build(self) -> None:
        result = verify_build(self.root, self.build)
        self.assertEqual(result["resident_hooks"], 2)
        self.assertEqual(result["aot_images"], 2)

    def test_native_disabled_in_cmake_cache(self) -> None:
        self.write("build/CMakeCache.txt", "XG_RENDER_NATIVE:BOOL=OFF\n")
        with self.assertRaisesRegex(ValueError, "XG_RENDER_NATIVE=ON"):
            verify_build(self.root, self.build)

    def test_original_selected_in_package_config(self) -> None:
        self.write("game.toml", '[runtime]\nrender_mode = "original"\n')
        with self.assertRaisesRegex(ValueError, "render_mode"):
            verify_build(self.root, self.build)

    def test_dynamic_overlay_cache_is_rejected(self) -> None:
        config = self.root / "game.toml"
        original = config.read_text()
        for replacement in ("overlay_cache = true", ""):
            with self.subTest(replacement=replacement):
                config.write_text(original.replace("overlay_cache = false", replacement))
                with self.assertRaisesRegex(ValueError, "overlay_cache = false"):
                    verify_build(self.root, self.build)

    def test_autocompile_commands_are_rejected(self) -> None:
        config = self.root / "game.toml"
        original = config.read_text()
        for key in ("overlay_autocompile_cmd", "overlay_autocompile_cmd_tcc"):
            with self.subTest(key=key):
                config.write_text(original + f'{key} = "compiler command"\n')
                with self.assertRaisesRegex(ValueError, "must not configure overlay autocompilation"):
                    verify_build(self.root, self.build)

    def test_missing_plan_cannot_pass_using_unused_generated_source(self) -> None:
        uninstrumented = self.write("generated/fixture_full_01.c", "void func(void) {}\n")
        self.write("build/psx-runtime_generated_sources.txt", f"{uninstrumented}\n")
        with self.assertRaisesRegex(ValueError, "missing resident Native hooks"):
            verify_build(self.root, self.build)

    def test_commented_hooks_do_not_count(self) -> None:
        self.source.write_text("/*\n" + self.source.read_text() + "*/\n")
        with self.assertRaisesRegex(ValueError, "missing resident Native hooks"):
            verify_build(self.root, self.build)

    def test_partial_or_wrong_instruction_capture_is_rejected(self) -> None:
        self.source.write_text(self.source.read_text().replace("E8B60000", "E8B60004"))
        with self.assertRaisesRegex(ValueError, "0x80010004"):
            verify_build(self.root, self.build)

    def test_stale_manifest_or_game_identity_is_rejected(self) -> None:
        for relative in ("native_renderer/xg_render_manifest.toml", "game/fixture"):
            path = self.root / relative
            original = path.read_text()
            with self.subTest(relative=relative):
                path.write_text(original + " changed")
                with self.assertRaisesRegex(ValueError, "stale or disabled Native identity"):
                    verify_build(self.root, self.build)
                path.write_text(original)

    def test_disabled_manifest_table_is_rejected(self) -> None:
        self.table.write_text(self.table.read_text().replace("count = 1u", "count = 0u"))
        with self.assertRaisesRegex(ValueError, "empty Native table"):
            verify_build(self.root, self.build)

    def test_empty_runtime_variant_table_is_rejected(self) -> None:
        self.write("build/generated/xg_render_runtime_variant_table.c",
                   "const uint32_t xg_render_runtime_variant_descriptor_count = 0u;\n")
        with self.assertRaisesRegex(ValueError, "empty Native table"):
            verify_build(self.root, self.build)

    def test_missing_aot_overlay_is_rejected(self) -> None:
        self.coverage["images"].pop()
        self.write_coverage()
        with self.assertRaisesRegex(ValueError, "incomplete AOT overlay coverage"):
            verify_build(self.root, self.build)

    def test_stale_aot_overlay_identity_is_rejected(self) -> None:
        self.coverage["manifest_identity_sha256"] = "0" * 64
        self.write_coverage()
        with self.assertRaisesRegex(ValueError, "stale AOT overlay identities"):
            verify_build(self.root, self.build)

    def test_empty_aot_ranges_are_rejected(self) -> None:
        self.coverage["images"][0]["ranges"] = []
        self.write_coverage()
        with self.assertRaisesRegex(ValueError, "incomplete AOT overlay coverage"):
            verify_build(self.root, self.build)


if __name__ == "__main__":
    unittest.main()
