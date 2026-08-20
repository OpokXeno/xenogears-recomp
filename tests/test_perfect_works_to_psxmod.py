#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import importlib.util
import struct
import sys
import tempfile
import tomllib
import unittest
import zipfile
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
TOOL_PATH = ROOT / "tools/perfect_works_to_psxmod.py"
SPEC = importlib.util.spec_from_file_location("perfect_works_to_psxmod", TOOL_PATH)
pw = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = pw
SPEC.loader.exec_module(pw)


def make_disc(path: Path, entries: int = 1600, visible_file: bool = False) -> None:
    sectors = 64
    image = bytearray(sectors * pw.RAW_SECTOR_SIZE)
    table = bytearray(pw.TABLE_SECTORS * pw.USER_SECTOR_SIZE)
    for index in range(entries):
        size = -1 if index == 0 else 1
        offset = index * 7
        table[offset : offset + 3] = (48).to_bytes(3, "little")
        struct.pack_into("<i", table, offset + 3, size)
    sentinel = entries * 7
    table[sentinel : sentinel + 3] = b"\xff\xff\xff"
    for sector in range(pw.TABLE_SECTORS):
        start = sector * pw.USER_SECTOR_SIZE
        raw = (pw.TABLE_LBA + sector) * pw.RAW_SECTOR_SIZE + pw.USER_OFFSET
        image[raw : raw + pw.USER_SECTOR_SIZE] = table[start : start + pw.USER_SECTOR_SIZE]
    image[48 * pw.RAW_SECTOR_SIZE + pw.USER_OFFSET] = 0xAA

    primary = bytearray(pw.USER_SECTOR_SIZE)
    primary[0:7] = b"\x01CD001\x01"
    root = bytearray(34)
    root[0] = 34
    root[2:6] = (40).to_bytes(4, "little")
    root[10:14] = pw.USER_SECTOR_SIZE.to_bytes(4, "little")
    root[25] = 2
    root[28:30] = (1).to_bytes(2, "little")
    root[32] = 1
    primary[156:190] = root
    raw = 16 * pw.RAW_SECTOR_SIZE + pw.USER_OFFSET
    image[raw : raw + pw.USER_SECTOR_SIZE] = primary
    directory = bytearray(pw.USER_SECTOR_SIZE)
    directory[:34] = root
    directory[33] = 0
    parent = bytearray(root)
    parent[33] = 1
    directory[34:68] = parent
    if visible_file:
        name = b"TEST.BIN;1"
        record = bytearray(44)
        record[0] = len(record)
        record[2:6] = (48).to_bytes(4, "little")
        record[10:14] = (1).to_bytes(4, "little")
        record[28:30] = (1).to_bytes(2, "little")
        record[32] = len(name)
        record[33 : 33 + len(name)] = name
        directory[68:112] = record
    raw = 40 * pw.RAW_SECTOR_SIZE + pw.USER_OFFSET
    image[raw : raw + pw.USER_SECTOR_SIZE] = directory
    path.write_bytes(image)


class PerfectWorksConverterTests(unittest.TestCase):
    def test_version_detection_prefers_version_history(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "README.md").write_text(
                "Download /releases/tag/0.10.0\n\n"
                "## Version history\n\n### Version 0.11.0\n",
                encoding="utf-8",
            )
            self.assertEqual(pw.detect_pwb_version(root), "0.11.0")

    def test_wrong_or_unknown_version_fails_explicitly(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "README.md").write_text(
                "## Version history\n\n### Version 0.11.1\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                RuntimeError, r"unsupported Perfect Works Build version 0\.11\.1.*0\.11\.2"
            ):
                pw.require_supported_pwb_version(root)
            (root / "README.md").write_text("Perfect Works\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "cannot detect"):
                pw.require_supported_pwb_version(root)

    def test_iso_visible_files_are_detected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "disc.bin"
            make_disc(path, visible_file=True)
            self.assertEqual(
                pw.read_iso_files(path), [pw.IsoFile("/TEST.BIN", 48, 1)]
            )

    def test_static_profile_does_not_require_wine(self):
        with mock.patch.object(pw.shutil, "which", return_value=None):
            runner = pw.UpstreamToolRunner(Path("/unused"), "auto")
        self.assertEqual(runner.mode, "wine" if pw.os.name != "nt" else "native")

    def test_toml_strings_escape_control_characters(self):
        manifest = pw.make_manifest(
            "test.package",
            "line one\nline two \U0001f642",
            "description\tvalue",
            {1: "1" * 64, 2: "2" * 64},
            [],
            [],
            False,
        )
        parsed = tomllib.loads(manifest)
        self.assertEqual(parsed["name"], "line one\nline two \U0001f642")

    def test_output_must_not_alias_a_disc(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            edition = root / "edition"
            edition.mkdir()
            disc1 = root / "disc1.bin"
            disc2 = root / "disc2.bin"
            disc1.write_bytes(b"one")
            disc2.write_bytes(b"two")
            alias = root / "output.psxmod"
            alias.symlink_to(disc1)
            with self.assertRaisesRegex(RuntimeError, "aliases"):
                pw.ensure_safe_outputs(
                    edition,
                    {1: disc1, 2: disc2},
                    alias,
                    None,
                    None,
                )

    def test_source_output_must_not_collide_with_archive_temporary_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            edition = root / "edition"
            edition.mkdir()
            output = root / "output.psxmod"
            with self.assertRaisesRegex(RuntimeError, "temporary path"):
                pw.ensure_safe_outputs(
                    edition,
                    {1: root / "disc1.bin", 2: root / "disc2.bin"},
                    output,
                    root / "output.psxmod.tmp",
                    None,
                )

    def test_edition_authentication_rejects_modified_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directories = ("gamefiles/bug_fix1", "gamefiles/bug_fix2", "gamefiles/title_screen")
            for relative in directories:
                path = root / relative
                path.mkdir(parents=True)
                (path / "0002").write_bytes(relative.encode())
            (root / "README.md").write_bytes(b"release")
            expected_directories = {
                relative: pw.directory_sha256(root / relative) for relative in directories
            }
            expected_files = {"README.md": hashlib.sha256(b"release").hexdigest()}
            with (
                mock.patch.dict(pw.EDITION_DIRECTORY_SHA256, expected_directories, clear=True),
                mock.patch.dict(pw.EDITION_FILE_SHA256, expected_files, clear=True),
            ):
                pw.authenticate_edition(root, pw.PatchOptions())
                (root / "gamefiles/bug_fix1/0002").write_bytes(b"modified")
                with self.assertRaisesRegex(RuntimeError, "authentication failed"):
                    pw.authenticate_edition(root, pw.PatchOptions())

    def test_release_directory_order_and_hybrids(self):
        options = pw.PatchOptions(
            script=True,
            half_encounters=True,
            exp_factor="1.5",
            gold_factor="2",
            rebalanced_items=True,
            rebalanced_enemies=True,
            arena="basic",
            face_fix="resize",
            text_speed="fast",
            battle_undub=True,
            music_changes=True,
            jpn_controls=True,
        )
        selected = pw.selected_directories(1, options)
        self.assertEqual(selected[:4], [
            "encounterone_script", "Script_items", "jpn_script_1", "resized_portraits"
        ])
        self.assertLess(selected.index("og_monsters"), selected.index("monsters_both"))
        self.assertLess(selected.index("monsters_both"), selected.index("filesbasic_script"))
        self.assertLess(selected.index("filesbasic_script"), selected.index("text_cd1"))
        self.assertEqual(selected[-2:], ["voice", "title_screen"])

    def test_isolated_selection_does_not_add_implicit_patches(self):
        self.assertEqual(
            pw.selected_directories(1, pw.PatchOptions(script=True), False),
            ["script1"],
        )
        self.assertEqual(
            pw.selected_directories(1, pw.PatchOptions(bug_fixes=True), False),
            ["bug_fix1"],
        )
        self.assertEqual(
            pw.selected_directories(1, pw.PatchOptions(title_screen=True), False),
            ["title_screen"],
        )
        self.assertFalse(pw.PROFILES["retranslation"].implicit_patches)

    def test_dynamic_isolated_patch_seeds_its_stock_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            disc = root / "disc.bin"
            make_disc(disc)
            entries, xa_children = pw.read_entries(disc)
            stage_root = root / "stage"
            stage_root.mkdir()
            stage = pw.StagingArea(1, stage_root)
            pw.seed_stock_dependencies(
                stage,
                disc,
                entries,
                xa_children,
                pw.PatchOptions(no_damage_cap=True),
            )
            self.assertEqual(stage.logical(38).path.read_bytes(), b"\xaa")
            self.assertEqual(stage.logical(38).sources, ["stock-disc1"])

    def test_individual_catalog_groups_option_values_in_selectors(self):
        keys = [individual.key for individual in pw.INDIVIDUAL_MODS]
        self.assertEqual(len(keys), 21)
        self.assertEqual(len(keys), len(set(keys)))
        self.assertIn("fmv-undub", keys)
        self.assertIn("exp", keys)
        self.assertIn("gold", keys)
        self.assertIn("arena", keys)
        self.assertIn("portraits", keys)
        self.assertIn("text-speed", keys)
        exp = next(item for item in pw.INDIVIDUAL_MODS if item.key == "exp")
        self.assertEqual(
            [(choice.value, choice.label) for choice in pw.individual_choices(exp)],
            [("1x", "1x"), ("1-5x", "1.5x"), ("2x", "2x")],
        )

    def test_choice_manifest_conditions_variant_operations(self):
        operation = pw.IndexedOperation(
            1,
            42,
            42,
            Path("assets/2x/disc1/0042.bin"),
            "a" * 64,
            "b" * 64,
            10,
            (),
            (),
            (("multiplier", "2x"),),
        )
        manifest = pw.make_manifest(
            "test.selector",
            "EXP selector",
            "Select EXP.",
            {1: "1" * 64, 2: "2" * 64},
            [operation],
            [],
            True,
            pw.ChoiceOption(
                "multiplier",
                "EXP multiplier",
                "1-5x",
                (("1-5x", "1.5x"), ("2x", "2x")),
            ),
        )
        parsed = tomllib.loads(manifest)
        self.assertEqual(parsed["option"][0]["type"], "choice")
        self.assertEqual(parsed["option"][0]["default"], "1-5x")
        self.assertEqual(
            parsed["indexed_file"][0]["when"], {"multiplier": "2x"}
        )

    def test_story_mode_declares_upstream_gameplay_exclusions(self):
        conflicts = pw.individual_conflicts("story-mode")
        self.assertEqual(len(conflicts), 8)
        self.assertIn(pw.individual_package_id("arena"), conflicts)
        self.assertIn(pw.individual_package_id("rebalanced-enemies"), conflicts)
        self.assertEqual(pw.individual_conflicts("retranslation"), ())
        manifest = pw.make_manifest(
            pw.individual_package_id("story-mode"),
            "Story Mode",
            "Story Mode",
            {1: "1" * 64, 2: "2" * 64},
            [],
            [],
            True,
            conflicts=conflicts,
        )
        self.assertEqual(tomllib.loads(manifest)["conflicts"], list(conflicts))

    def test_format_eight_composition_metadata_is_serialized(self):
        condition = pw.FeatureCondition(
            "org.perfectworksbuild.individual.retranslation"
        )
        operation = pw.IndexedOperation(
            1,
            42,
            42,
            Path("assets/hybrid.bin"),
            "a" * 64,
            "b" * 64,
            10,
            (),
            (),
            (),
            (condition,),
            ("org.perfectworksbuild.individual.retranslation",),
        )
        parsed = tomllib.loads(
            pw.make_manifest(
                "test.hybrid",
                "Hybrid",
                "Hybrid",
                {1: "1" * 64, 2: "2" * 64},
                [operation],
                [],
                False,
            )
        )
        indexed = parsed["indexed_file"][0]
        self.assertEqual(parsed["format_version"], 8)
        self.assertEqual(indexed["compose"], "xenogears-pwb-0.11.2")
        self.assertEqual(indexed["when_features"][0]["package"], condition.package_id)
        self.assertEqual(indexed["supersedes"], [condition.package_id])

    def test_format_eight_scopes_executable_patches(self):
        condition = pw.FeatureCondition(
            pw.individual_package_id("fmv-undub"), enabled=False
        )
        patch = pw.ExePatch(
            0x80012340,
            b"\x01\x02",
            b"\x03\x04",
            "disc-specific",
            disc_sha256="1" * 64,
            when_features=(condition,),
        )
        parsed = tomllib.loads(
            pw.make_manifest(
                "test.patch",
                "Patch",
                "Patch",
                {1: "1" * 64, 2: "2" * 64},
                [],
                [patch],
                False,
            )
        )
        serialized = parsed["patch"][0]
        self.assertEqual(serialized["disc_sha256"], "1" * 64)
        self.assertFalse(serialized["when_features"][0]["enabled"])
        reparsed = pw.feature_conditions_from_manifest(
            serialized["when_features"]
        )
        self.assertEqual(reparsed, (condition,))

    def test_composition_variant_claims_all_participant_resources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            disc_hashes = {1: "1" * 64, 2: "2" * 64}
            expected = "e" * 64
            owner_root = root / "owner"
            bug_root = root / "bug"
            combined_root = root / "combined"
            for package_root in (owner_root, bug_root, combined_root):
                package_root.mkdir()

            def write_source(
                package_root: Path,
                package_id: str,
                entries: tuple[tuple[int, bytes], ...],
            ) -> None:
                operations = []
                for index, payload in entries:
                    relative = Path("assets") / f"{index}.bin"
                    destination = package_root / relative
                    destination.parent.mkdir(exist_ok=True)
                    destination.write_bytes(payload)
                    operations.append(
                        pw.IndexedOperation(
                            1,
                            index,
                            index,
                            relative,
                            pw.sha256_bytes(payload),
                            expected,
                            len(payload),
                            (),
                            (),
                        )
                    )
                (package_root / "manifest.toml").write_text(
                    pw.make_manifest(
                        package_id,
                        package_id,
                        package_id,
                        disc_hashes,
                        operations,
                        [],
                        False,
                    ),
                    encoding="utf-8",
                )
                (package_root / "PORTING_REPORT.txt").write_text(
                    "", encoding="utf-8"
                )
                (package_root / "PORTING_INDEX.tsv").write_text(
                    "disc\tlisted_index\tlogical_index\tpayload\tbytes\texpected_sha256\tpayload_sha256\tsources\n",
                    encoding="utf-8",
                )

            write_source(
                owner_root,
                pw.individual_package_id("retranslation"),
                ((7, b"owner"),),
            )
            write_source(
                bug_root,
                pw.individual_package_id("bug-fixes"),
                ((8, b"bug"),),
            )
            write_source(
                combined_root,
                "test.combined",
                ((7, b"combined-owner"), (8, b"combined-bug")),
            )
            discs = {1: root / "disc1.bin", 2: root / "disc2.bin"}
            for path in discs.values():
                path.write_bytes(b"")
            variant = pw.CompositionVariant(
                "retranslation",
                "script-with-bug-suppressed",
                pw.PatchOptions(script=True),
                (
                    pw.FeatureCondition(
                        pw.individual_package_id("bug-fixes")
                    ),
                ),
                suppressed=("bug-fixes",),
            )
            count = pw.add_composition_variant(
                {"retranslation": owner_root, "bug-fixes": bug_root},
                combined_root,
                variant,
                discs,
                disc_hashes,
                pw.PreparedInputs({}, {}, {}, {}),
            )
            self.assertEqual(count, 2)
            parsed = tomllib.loads(
                (owner_root / "manifest.toml").read_text(encoding="utf-8")
            )
            hybrids = [
                item for item in parsed["indexed_file"] if item.get("supersedes")
            ]
            self.assertEqual({item["index"] for item in hybrids}, {7, 8})
            self.assertEqual(
                set(hybrids[0]["supersedes"]),
                {
                    pw.individual_package_id("retranslation"),
                    pw.individual_package_id("bug-fixes"),
                },
            )
            self.assertEqual(
                len((owner_root / "PORTING_INDEX.tsv").read_text(
                    encoding="utf-8"
                ).splitlines()),
                3,
            )

    def test_conflicts_ignore_identical_resource_claims(self):
        first = {"disc:one:index:7": {"xenogears:stock:payload"}}
        identical = {"disc:one:index:7": {"xenogears:stock:payload"}}
        different = {"disc:one:index:7": {"xenogears:stock:other"}}
        self.assertEqual(pw.incompatible_claims(first, identical), set())
        self.assertEqual(
            pw.incompatible_claims(first, different), {"disc:one:index:7"}
        )

    def test_batch_catalog_emits_separate_archives(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "individual"
            edition = root / "edition"
            edition.mkdir()
            (edition / "README.md").write_text(
                "### Version 0.11.2\n", encoding="utf-8"
            )

            def fake_build(*args, **kwargs):
                package = args[4]
                manifest = pw.make_manifest(
                    args[7],
                    args[8],
                    args[6].description,
                    {1: "1" * 64, 2: "2" * 64},
                    [],
                    [],
                    False,
                )
                source_output = kwargs.get("source_output")
                if source_output is not None:
                    source_output.mkdir()
                    (source_output / "manifest.toml").write_text(
                        manifest, encoding="utf-8"
                    )
                    (source_output / "PORTING_REPORT.txt").write_text(
                        "", encoding="utf-8"
                    )
                    (source_output / "PORTING_INDEX.tsv").write_text(
                        "disc\tlisted_index\tlogical_index\tpayload\tbytes\texpected_sha256\tpayload_sha256\tsources\n",
                        encoding="utf-8",
                    )
                else:
                    with zipfile.ZipFile(package, "w") as archive:
                        archive.writestr("manifest.toml", manifest)
                self.assertFalse(kwargs["implicit_patches"])
                return ""

            def fake_selector(*args, **kwargs):
                individual = args[0]
                package = args[5]
                choices = pw.individual_choices(individual)
                manifest = pw.make_manifest(
                    f"test.{individual.key}",
                    individual.name,
                    individual.description,
                    {1: "1" * 64, 2: "2" * 64},
                    [],
                    [],
                    False,
                    pw.ChoiceOption(
                        individual.option_id,
                        individual.option_label,
                        choices[0].value,
                        tuple((choice.value, choice.label) for choice in choices),
                    ),
                )
                source_output = kwargs.get("source_output")
                if source_output is not None:
                    source_output.mkdir()
                    (source_output / "manifest.toml").write_text(
                        manifest, encoding="utf-8"
                    )
                    (source_output / "PORTING_REPORT.txt").write_text(
                        "", encoding="utf-8"
                    )
                    (source_output / "PORTING_INDEX.tsv").write_text(
                        "choice\tdisc\tlisted_index\tlogical_index\tpayload\tbytes\texpected_sha256\tpayload_sha256\tsources\n",
                        encoding="utf-8",
                    )
                else:
                    with zipfile.ZipFile(package, "w") as archive:
                        archive.writestr("manifest.toml", manifest)
                return []

            prepared = pw.PreparedInputs({}, {}, {}, {})
            with (
                mock.patch.object(pw, "prepare_stock_inputs", return_value=prepared),
                mock.patch.object(pw, "build_package", side_effect=fake_build),
                mock.patch.object(
                    pw, "build_selector_package", side_effect=fake_selector
                ),
            ):
                report = pw.build_individual_catalog(
                    edition,
                    {1: root / "disc1.bin", 2: root / "disc2.bin"},
                    {1: "1" * 64, 2: "2" * 64},
                    {1: "3" * 64, 2: "4" * 64},
                    output,
                    "auto",
                )
            packages = sorted(output.glob("*.psxmod"))
            self.assertEqual(len(packages), len(pw.INDIVIDUAL_MODS))
            self.assertIn("Individual packages: 21", report)
            self.assertIn("Incompatible package pairs: 8", report)
            self.assertTrue((output / "CATALOG.tsv").is_file())
            self.assertTrue((output / "CONFLICTS.tsv").is_file())
            conflicts = (output / "CONFLICTS.tsv").read_text(
                encoding="utf-8"
            ).splitlines()
            self.assertEqual(len(conflicts), 9)
            self.assertEqual(conflicts[0], "first\tsecond\treason")

    def test_exp_reproduces_release_low_16_bit_behavior(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = bytearray(512)
            data[0:2] = (494).to_bytes(2, "little")
            offset = 126 + 0x100
            data[offset : offset + 4] = (0x00020002).to_bytes(4, "little")
            gold_offset = 126 + 0x10A
            data[gold_offset : gold_offset + 2] = (40000).to_bytes(2, "little")
            path = root / "2618"
            path.write_bytes(data)
            stage = pw.StagingArea(1, root)
            stage.files[2618] = pw.StagedFile(2618, 2618, path)
            pw.apply_exp_gold(
                stage, pw.PatchOptions(exp_factor="1.5", gold_factor="2")
            )
            result = path.read_bytes()
            self.assertEqual(int.from_bytes(result[offset : offset + 4], "little"), 3)
            self.assertEqual(
                int.from_bytes(result[gold_offset : gold_offset + 2], "little"),
                80000 & 0xFFFF,
            )

    def test_text_speed_becomes_guarded_main_exe_patches(self):
        executable = bytearray(303104)
        executable[:8] = b"PS-X EXE"
        executable[0x18:0x1C] = (0x80010000).to_bytes(4, "little")
        executable[0x1C:0x20] = (301056).to_bytes(4, "little")
        executable[151908:151910] = b"\x68\x00"
        executable[151911:151913] = b"\x92\x00"
        patches = pw.build_exe_patches(
            {1: bytes(executable), 2: bytes(executable)},
            pw.PatchOptions(text_speed="fast"),
            Path("."),
            {1: "1" * 64, 2: "2" * 64},
            "test.text-speed",
        )
        self.assertEqual([patch.address for patch in patches], [0x80034964, 0x80034967])
        self.assertEqual([patch.replacement for patch in patches], [b"\x05", b"\x34"])

    def test_jpn_executable_guards_follow_fmv_selection(self):
        with tempfile.TemporaryDirectory() as temporary:
            edition = Path(temporary)
            (edition / "data/controls").mkdir(parents=True)
            (edition / "data/controls/0022.csv").write_text(
                "2251,0x0a60\n38144,0x20\n", encoding="ascii"
            )
            executables = {}
            for disc_number, name, expected in (
                (1, "SLUS_006.64", b"\x64\x00"),
                (2, "SLUS_006.69", b"\x00\x00"),
            ):
                executable = bytearray(40000)
                executable[:8] = b"PS-X EXE"
                executable[0x18:0x1C] = (0x80010000).to_bytes(4, "little")
                executable[0x1C:0x20] = (37952).to_bytes(4, "little")
                executable[2251:2253] = expected
                executables[disc_number] = bytes(executable)
                directory = edition / f"gamefiles/sub_executable/disc{disc_number}"
                directory.mkdir(parents=True)
                (directory / name).write_bytes(executable)

            patches = pw.build_exe_patches(
                executables,
                pw.PatchOptions(jpn_controls=True),
                edition,
                {1: "1" * 64, 2: "2" * 64},
                pw.individual_package_id("jpn-controls"),
            )
            self.assertEqual(len(patches), 2)
            self.assertEqual(
                {patch.when_features[0].enabled for patch in patches},
                {False, True},
            )
            self.assertTrue(all(patch.address == 0x80018D00 for patch in patches))

    def test_text_selector_guards_follow_fmv_selection(self):
        with tempfile.TemporaryDirectory() as temporary:
            edition = Path(temporary)
            executable = bytearray(303104)
            executable[:8] = b"PS-X EXE"
            executable[0x18:0x1C] = (0x80010000).to_bytes(4, "little")
            executable[0x1C:0x20] = (301056).to_bytes(4, "little")
            executable[151908:151910] = b"\x68\x00"
            executable[151911:151913] = b"\x92\x00"
            for disc_number, name in ((1, "SLUS_006.64"), (2, "SLUS_006.69")):
                directory = edition / f"gamefiles/sub_executable/disc{disc_number}"
                directory.mkdir(parents=True)
                (directory / name).write_bytes(executable)
            patches = pw.build_exe_patches(
                {1: bytes(executable), 2: bytes(executable)},
                pw.PatchOptions(text_speed="fast"),
                edition,
                {1: "1" * 64, 2: "2" * 64},
                "org.perfectworksbuild.variant.text-speed.fast",
            )
            self.assertEqual(len(patches), 4)
            self.assertEqual(
                [patch.when_features[0].enabled for patch in patches],
                [False, False, True, True],
            )

    def test_story_mode_rejects_gameplay_combinations(self):
        with self.assertRaisesRegex(ValueError, "story mode"):
            pw.validate_options(pw.PatchOptions(story_mode=True, arena="basic"))

    def test_minimal_package_is_deterministic_and_multidisc(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            edition = root / "edition"
            edition.mkdir()
            (edition / "README.md").write_text(
                "### Version 0.11.2\n", encoding="utf-8"
            )
            for relative, name, payload in (
                ("bug_fix1", "0038", b"disc1-bug"),
                ("bug_fix2", "0038", b"disc2-bug"),
                ("title_screen", "1587.raw1", b"title"),
            ):
                directory = edition / "gamefiles" / relative
                directory.mkdir(parents=True, exist_ok=True)
                (directory / name).write_bytes(payload)
            disc1 = root / "disc1.bin"
            disc2 = root / "disc2.bin"
            make_disc(disc1)
            make_disc(disc2)
            raw_hashes = {
                1: hashlib.sha256(disc1.read_bytes()).hexdigest(),
                2: hashlib.sha256(disc2.read_bytes()).hexdigest(),
            }
            canonical = {1: "1" * 64, 2: "2" * 64}
            first = root / "first.psxmod"
            second = root / "second.psxmod"
            profile = pw.PROFILES["bug_fixes"]
            common = dict(
                edition_root=edition,
                disc_paths={1: disc1, 2: disc2},
                disc_hashes=canonical,
                expected_bin_hashes=raw_hashes,
                profile_key="bug_fixes",
                profile=profile,
                package_id="test.perfect-works",
                package_name="Test Perfect Works",
                tool_mode="auto",
                verify_edition=False,
            )
            pw.build_package(output=first, **common)
            pw.build_package(output=second, **common)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with zipfile.ZipFile(first) as archive:
                manifest = tomllib.loads(archive.read("manifest.toml").decode())
                self.assertEqual(len(manifest["target"]), 2)
                indexed = manifest["indexed_file"]
                self.assertEqual({item["disc_sha256"] for item in indexed}, {"1" * 64, "2" * 64})
                self.assertEqual({item["index"] for item in indexed}, {33, 38, 1582, 1587})
                self.assertIn("PORTING_REPORT.txt", archive.namelist())
                self.assertIn("PORTING_INDEX.tsv", archive.namelist())


if __name__ == "__main__":
    unittest.main()
