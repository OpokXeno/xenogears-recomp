#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///

from __future__ import annotations

import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


REPOSITORY = Path(__file__).resolve().parents[3]
TOOLS = REPOSITORY / "tools"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(REPOSITORY / "psxrecomp" / "tools"))

import compile_overlays as overlay_compiler  # noqa: E402
from native_render_manifest_model import Digest32, FileIdentity  # noqa: E402
from native_render_runtime_variant_model import (  # noqa: E402
    ArtifactSpec,
    CanonicalTuple,
    HookSpec,
    RuntimeVariant,
    RuntimeVariantContract,
    SourceSite,
    load_contract,
)


BASE = 0x80010000
SITE_INSTRUCTION = 0x8C840000
RETURN_INSTRUCTION = 0x03E00008
ACTIVATION = BASE + 0x10
PRODUCER = BASE + 0x20
CAPTURE = BASE + 0x30
RETURN = BASE + 0x38
ACTIVATION_JAL = 0x0C004008
CAPTURE_JAL = 0x0C004000
GAME_IDENTITY = "01" * 32
MANIFEST_IDENTITY = "02" * 32
PRODUCTION_MANIFEST = (
    REPOSITORY / "native_renderer" / "xg_render_runtime_variants.toml"
)


def recompiler_path() -> Path:
    candidates = (
        REPOSITORY / "psxrecomp" / "recompiler" / "build" / "psxrecomp-game",
        REPOSITORY / "psxrecomp" / "recompiler" / "build" / "psxrecomp-game.exe",
        REPOSITORY / "psxrecomp" / "recompiler" / "build" / "Release" / "psxrecomp-game.exe",
    )
    path = next((candidate for candidate in candidates if candidate.is_file()), None)
    if path is None:
        raise FileNotFoundError("psxrecomp-game is not built")
    return path


def image(first_delay_instruction: int) -> bytes:
    words = [0] * 15
    words[0] = SITE_INSTRUCTION
    words[1] = RETURN_INSTRUCTION
    words[4] = ACTIVATION_JAL
    words[5] = first_delay_instruction
    words[8] = 0x24020001
    words[12] = CAPTURE_JAL
    words[13] = 0x34040001
    words[14] = 0x24030001
    return struct.pack("<" + "I" * len(words), *words)


def synthetic_contract(data: bytes) -> RuntimeVariantContract:
    digest = Digest32(hashlib.sha256(data).digest())
    checksum = zlib.crc32(data) & 0xFFFFFFFF
    activation = HookSpec(
        ACTIVATION, 8, Digest32(hashlib.sha256(data[0x10:0x18]).digest()),
        ACTIVATION, PRODUCER, struct.unpack_from("<I", data, 0x14)[0]
    )
    capture = HookSpec(
        CAPTURE, 8, Digest32(hashlib.sha256(data[0x30:0x38]).digest()),
        CAPTURE, BASE, 0x34040001
    )
    site = SourceSite(BASE, SITE_INSTRUCTION, "read", 4, "effective-address")
    variant = RuntimeVariant(
        "synthetic-v1", activation, PRODUCER, capture, RETURN,
        BASE, 16, (SITE_INSTRUCTION,), (site,)
    )
    return RuntimeVariantContract(
        CanonicalTuple(
            Digest32(bytes.fromhex(GAME_IDENTITY)),
            Digest32(bytes.fromhex(MANIFEST_IDENTITY)),
            1,
            2,
            BASE,
            BASE,
            BASE,
            BASE + 8,
        ),
        ArtifactSpec(
            "synthetic.bin",
            FileIdentity(digest, checksum, len(data)),
            BASE,
            0,
            len(data),
            digest,
            checksum,
        ),
        (variant,),
    )


def write_psx_executable(path: Path, data: bytes) -> None:
    header = bytearray(0x800)
    header[:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, BASE)
    struct.pack_into("<I", header, 0x18, BASE)
    struct.pack_into("<I", header, 0x1C, len(data))
    path.write_bytes(header + data)


def generate_source(data: bytes, plan: str | None) -> subprocess.CompletedProcess[str]:
    recompiler = recompiler_path()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        executable = root / "synthetic.psx"
        seeds = root / "seeds.txt"
        output = root / "generated"
        write_psx_executable(executable, data)
        seeds.write_text(f"dispatch_root 0x{BASE:08X}\n", encoding="ascii")
        command = [
            str(recompiler),
            str(executable),
            "--seeds",
            str(seeds),
            "--out-dir",
            str(output),
            "--overlay",
            "--game-identity-sha256",
            GAME_IDENTITY,
            "--manifest-digest-sha256",
            MANIFEST_IDENTITY,
        ]
        overlay_compiler.add_source_observation_plan(command, plan, str(root))
        result = subprocess.run(
            command,
            cwd=REPOSITORY,
            capture_output=True,
            text=True,
            check=False,
        )
        generated = next(output.glob("*_full.c"), None)
        if generated is not None:
            result.stdout = generated.read_text(encoding="utf-8")
        return result


class WarmOverlaySourceObservationTests(unittest.TestCase):
    def test_production_manifest_rejects_foreign_artifact_without_private_bytes(self) -> None:
        # Given
        contract = load_contract(PRODUCTION_MANIFEST)
        foreign = image(0x24840001)

        # When
        plan = overlay_compiler.source_observation_plan_for_artifact(
            contract, foreign, contract.artifact.base_address)

        # Then
        self.assertIsNone(plan)

    def test_matching_artifact_forwards_exact_sites_and_emits_hooks(self) -> None:
        # Given
        artifact = image(0x24840001)
        contract = synthetic_contract(artifact)

        # When
        plan = overlay_compiler.source_observation_plan_for_artifact(
            contract, artifact, BASE)
        result = generate_source(artifact, plan)

        # Then
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count("PSX_XG_RENDER_AUTH_HOOK_SOURCE_PRE"), 1)
        self.assertEqual(result.stdout.count("PSX_XG_RENDER_AUTH_HOOK_SOURCE_COMMIT"), 1)
        self.assertIn(f"0x{BASE:08X}u, 0x{SITE_INSTRUCTION:08X}u", result.stdout)
        self.assertIn(
            f"lifecycle {PRODUCER:08X} 24020001 entry 00000000", plan
        )
        self.assertIn(
            f"lifecycle {CAPTURE:08X} {CAPTURE_JAL:08X} capture 34040001", plan
        )
        self.assertIn(
            f"lifecycle {RETURN:08X} 24030001 return 00000000", plan
        )

    def test_foreign_artifact_at_same_site_emits_no_plan_or_hooks(self) -> None:
        # Given
        authenticated = image(0x24840001)
        foreign = image(0x24840002)
        contract = synthetic_contract(authenticated)

        # When
        plan = overlay_compiler.source_observation_plan_for_artifact(
            contract, foreign, BASE)
        result = generate_source(foreign, plan)

        # Then
        self.assertIsNone(plan)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("PSX_XG_RENDER_AUTH_HOOK_SOURCE_PRE", result.stdout)
        self.assertNotIn("PSX_XG_RENDER_AUTH_HOOK_SOURCE_COMMIT", result.stdout)

    def test_malformed_plan_fails_closed(self) -> None:
        # Given
        artifact = image(0x24840001)
        malformed = (
            "psxrecomp-source-observation-plan-v5\n"
            f"site {BASE:08X} {SITE_INSTRUCTION:08X} execute 4 effective-address\n"
        )

        # When
        result = generate_source(artifact, malformed)

        # Then
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source observation plan", result.stderr.lower())

    def test_lifecycle_plan_rejects_unknown_role_and_wrong_capture_delay(self) -> None:
        artifact = image(0x24840001)
        unknown_role = (
            "psxrecomp-source-observation-plan-v5\n"
            f"lifecycle {PRODUCER:08X} 24020001 execute 00000000\n"
        )
        wrong_delay = (
            "psxrecomp-source-observation-plan-v5\n"
            f"lifecycle {CAPTURE:08X} {CAPTURE_JAL:08X} capture 34040002\n"
        )

        role_result = generate_source(artifact, unknown_role)
        delay_result = generate_source(artifact, wrong_delay)

        self.assertNotEqual(role_result.returncode, 0)
        self.assertIn("lifecycle", role_result.stderr.lower())
        self.assertNotEqual(delay_result.returncode, 0)
        self.assertIn("lifecycle capture", delay_result.stderr.lower())

    def test_generic_plan_accepts_every_closed_operation_token(self) -> None:
        # Given
        artifact = image(0x24840001) + struct.pack("<I", 0x00621007)
        plan = (
            "psxrecomp-source-observation-plan-v5\n"
            f"site {BASE:08X} {SITE_INSTRUCTION:08X} read 4 effective-address\n"
            f"site {BASE + 4:08X} {RETURN_INSTRUCTION:08X} write 2 effective-address\n"
            f"site {BASE + 8:08X} 00000000 swc2 4 effective-address\n"
            f"site {BASE + 12:08X} 00000000 call 0 none\n"
            f"site {BASE + 16:08X} {ACTIVATION_JAL:08X} bucket 0 result-register\n"
        )

        # When
        result = generate_source(artifact, plan)

        # Then
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_observe_cutover_preserves_original_instruction(self) -> None:
        artifact = image(0x24840001)
        plan = (
            "psxrecomp-source-observation-plan-v5\n"
            f"cutover {BASE:08X} {SITE_INSTRUCTION:08X} observe 00000000\n"
            f"site {BASE + 4:08X} {RETURN_INSTRUCTION:08X} write 2 effective-address\n"
        )

        result = generate_source(artifact, plan)

        self.assertEqual(result.returncode, 0, result.stderr)
        callback = (
            f"(void)psx_xg_render_native_ft4_bypass(cpu, 0x{BASE:08X}u, "
            f"0x{SITE_INSTRUCTION:08X}u);"
        )
        self.assertEqual(result.stdout.count(callback), 1)
        self.assertIn(
            "cpu->gpr[4] = psx_cyc_load_word(cpu, cpu->gpr[4], 4, 0x10u);",
            result.stdout,
        )

    def test_plan_pcs_become_exact_fragment_demands_without_execution(self) -> None:
        plan = (
            "psxrecomp-source-observation-plan-v5\n"
            f"cutover {BASE:08X} {SITE_INSTRUCTION:08X} observe 00000000\n"
            f"site {BASE + 4:08X} {RETURN_INSTRUCTION:08X} write 2 effective-address\n"
        )
        audit = {
            "included_reasons": {},
            "executed_pcs": set(),
            "static_exact_fragment_demands": set(),
            "static_interval_fragment_demands": set(),
            "producer_ranges": [(BASE, BASE + 0x40)],
            "accepted_cross_producer_calls": set(),
        }

        job = overlay_compiler.make_interior_fragment_job(
            BASE & 0x1FFFFFFF,
            BASE,
            0x40,
            bytes(0x40),
            audit,
            set(),
            source_plan=plan,
        )

        self.assertIsNotNone(job)
        self.assertEqual(job["candidates"], {BASE, BASE + 4})
        self.assertEqual(job["forced"], {BASE, BASE + 4})
        self.assertEqual(job["executed"], set())

    def test_plan_repair_recovers_current_byte_canonical_ordinary_roots(self) -> None:
        artifact = image(0x24840001)
        code_crc = zlib.crc32(artifact) & 0xFFFFFFFF
        unrelated_crc = zlib.crc32(artifact[8:12]) & 0xFFFFFFFF
        plan = (
            "psxrecomp-source-observation-plan-v5\n"
            f"cutover {BASE:08X} {SITE_INSTRUCTION:08X} observe 00000000\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ordinary = root / "cg18_12345678"
            repair = root / "cg18_12345678_p89abcdef"
            ordinary.mkdir()
            repair.mkdir()
            (ordinary / "bundle.so").write_bytes(b"complete")
            (ordinary / "bundle.ranges").write_text(
                "# psxrecomp overlay code-range manifest v2\n"
                f"I {GAME_IDENTITY.upper()} {MANIFEST_IDENTITY.upper()}\n"
                f"F {BASE:08X} {code_crc:08X}\n"
                f"R {BASE:08X} {len(artifact):X}\n"
                f"F {BASE + 4:08X} {code_crc:08X}\n"
                f"R {BASE:08X} {len(artifact):X}\n"
                f"F {BASE + 8:08X} {unrelated_crc:08X}\n"
                f"R {BASE + 8:08X} 4\n",
                encoding="ascii",
            )

            roots = overlay_compiler.source_plan_repair_roots(
                str(repair), 0x89ABCDEF, plan, artifact, BASE, len(artifact))

        self.assertEqual(roots, {BASE})

if __name__ == "__main__":
    unittest.main()
