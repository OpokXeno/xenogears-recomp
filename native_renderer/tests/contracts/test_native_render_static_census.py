from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
from typing import Any

import pytest


REPOSITORY = Path(__file__).resolve().parents[3]
TOOL = REPOSITORY / "tools" / "native_render_static_census.py"
SCHEMA = REPOSITORY / "native_renderer" / "xg_native_3d_census_review_v3.schema.json"
DISC1_BASE = 0x80010000
OVERLAY_BASE = 0x80100000
HEADER_SIZE = 0x800


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _artifact_id(data: bytes) -> str:
    return f"artifact-{_sha256(data)}"


def _site_id(data: bytes, offset: int, kind: str) -> str:
    return f"site-{_sha256(data)}-{offset:08x}-{kind}"


def _branch_id(data: bytes, offset: int, kind: str) -> str:
    return f"branch-{_sha256(data)}-{offset:08x}-{kind}"


def _put(payload: bytearray, offset: int, word: int) -> None:
    struct.pack_into("<I", payload, offset, word)


def _psx_exe(payload: bytes, base: int) -> bytes:
    header = bytearray(HEADER_SIZE)
    header[:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, base)
    struct.pack_into("<I", header, 0x18, base)
    struct.pack_into("<I", header, 0x1C, len(payload))
    struct.pack_into("<I", header, 0x30, 0x801FFFF0)
    return bytes(header) + payload


def _mapping(image_format: str, size: int, base: int, entry: str | None) -> dict[str, Any]:
    header_size = HEADER_SIZE if image_format == "ps-x-exe" else 0
    return {
        "format": image_format,
        "endian": "little",
        "header_size": header_size,
        "payload_file_offset": header_size,
        "payload_size": size - header_size,
        "runtime_address": f"0x{base:08x}",
        "entry_address": entry,
    }


def _artifact(
    data: bytes,
    *,
    discs: list[int],
    serials: list[str],
    role: str,
    image_format: str,
    base: int,
    executable_ranges: list[dict[str, Any]],
) -> dict[str, Any]:
    return {
        "sha256": _sha256(data),
        "size": len(data),
        "discs": discs,
        "serials": serials,
        "role": role,
        "executable_ranges": executable_ranges,
        "mapping": _mapping(
            image_format,
            len(data),
            base,
            f"0x{base:08x}" if image_format == "ps-x-exe" else None,
        ),
    }


def _executable_range(range_id: str, offset: int, size: int) -> dict[str, Any]:
    return {
        "range_id": range_id,
        "payload_offset": offset,
        "size": size,
        "provenance_refs": ["fixture-range-provenance"],
        "evidence_refs": ["fixture-range-evidence"],
    }


def _artifact_classification(data: bytes, evidence: str) -> dict[str, Any]:
    return {
        "artifact_id": _artifact_id(data),
        "status": "migrated-render",
        "subject": "executable-image",
        "evidence_refs": [evidence],
    }


def _site_classification(
    data: bytes,
    offset: int,
    kind: str,
    subject: str,
    *,
    status: str = "migrated-render",
    families: list[str] | None = None,
) -> dict[str, Any]:
    return {
        "site_id": _site_id(data, offset, kind),
        "status": status,
        "subject": subject,
        "family_ids": ["fixture-render-family"] if families is None else families,
        "evidence_refs": ["fixture-static-review"],
    }


def _write_review(path: Path, review: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(review, ensure_ascii=True, indent=2, sort_keys=True) + "\n",
        encoding="ascii",
        newline="\n",
    )


def fixture(tmp_path: Path) -> tuple[Path, list[Path], dict[str, Any], dict[str, bytes]]:
    disc1_payload = bytearray(0x100)
    _put(disc1_payload, 0x00, 0x4A000030)  # RTPT
    _put(disc1_payload, 0x04, (0x32 << 26) | (4 << 21) | (2 << 16) | 0x10)
    _put(disc1_payload, 0x08, (0x3A << 26) | (5 << 21) | (3 << 16) | 0xFFF0)
    wrapper = DISC1_BASE + 0x80
    _put(disc1_payload, 0x0C, (0x03 << 26) | ((wrapper >> 2) & 0x03FFFFFF))
    _put(disc1_payload, 0x10, (0x12 << 26) | (2 << 16) | (3 << 11))  # MFC2
    _put(disc1_payload, 0x14, (0x23 << 26) | (4 << 21) | (8 << 16))
    _put(disc1_payload, 0x18, (0x2B << 26) | (5 << 21) | (8 << 16))
    _put(disc1_payload, 0x1C, (0x2B << 26) | (4 << 21) | (5 << 16))
    _put(disc1_payload, 0x20, (25 << 21) | (31 << 11) | 0x09)
    _put(disc1_payload, 0x24, (0x12 << 26) | (4 << 21) | (4 << 16) | (5 << 11))  # MTC2
    _put(disc1_payload, 0x28, (0x04 << 26) | (8 << 21) | (9 << 16) | 1)
    _put(disc1_payload, 0x2C, (0x12 << 26) | (2 << 21) | (6 << 16) | (7 << 11))  # CFC2
    _put(disc1_payload, 0x30, (0x12 << 26) | (6 << 21) | (8 << 16) | (9 << 11))  # CTC2
    unrelated_target = DISC1_BASE + 0xC0
    _put(
        disc1_payload,
        0x34,
        (0x03 << 26) | ((unrelated_target >> 2) & 0x03FFFFFF),
    )
    _put(disc1_payload, 0x38, (0x2B << 26) | (12 << 21) | (13 << 16) | 0x20)
    _put(disc1_payload, 0x40, (0x04 << 26) | (10 << 21) | (11 << 16) | 1)
    _put(disc1_payload, 0x80, 0x4A000006)  # NCLIP in the synthetic wrapper
    _put(disc1_payload, 0xC0, 0x4A000030)  # GTE-shaped data outside executable ranges
    disc1 = _psx_exe(bytes(disc1_payload), DISC1_BASE)

    overlay = bytes(0x20)

    data_by_name = {"disc1": disc1, "overlay": overlay}
    paths: list[Path] = []
    for name, data in data_by_name.items():
        path = tmp_path / f"{name}.bin"
        path.write_bytes(data)
        paths.append(path)

    disc1_artifact = _artifact(
        disc1,
        discs=[1],
        serials=["SLUS-00664"],
        role="main-exe",
        image_format="ps-x-exe",
        base=DISC1_BASE,
        executable_ranges=[
            _executable_range("disc1-main-code", 0, 0x48),
            _executable_range("disc1-wrapper-code", 0x80, 4),
        ],
    )
    overlay_artifact = _artifact(
        overlay,
        discs=[1],
        serials=["SLUS-00664"],
        role="overlay",
        image_format="flat-binary",
        base=OVERLAY_BASE,
        executable_ranges=[_executable_range("fixture-overlay-code", 0, len(overlay))],
    )
    review: dict[str, Any] = {
        "schema": "xg-native-3d-census-review/v3",
        "artifacts": [disc1_artifact, overlay_artifact],
        "functions": [
            {
                "function_id": "fixture-producer-function",
                "artifact_id": _artifact_id(disc1),
                "executable_range_id": "disc1-main-code",
                "payload_offset": 0,
                "size": 0x30,
                "role": "producer",
                "provenance_refs": ["fixture-function-provenance"],
                "evidence_refs": ["fixture-function-evidence"],
            }
        ],
        "render_targets": [
            {
                "target_id": "fixture-gte-wrapper",
                "artifact_id": _artifact_id(disc1),
                "address": f"0x{wrapper:08x}",
                "role": "gte-wrapper",
                "caller_artifact_ids": [_artifact_id(disc1)],
                "indirect_call_site_ids": [_site_id(disc1, 0x20, "jalr")],
                "provenance_refs": ["fixture-target-provenance"],
                "evidence_refs": ["fixture-target-evidence"],
            }
        ],
        "classifications": {
            "artifacts": [
                _artifact_classification(disc1, "fixture-disc1-identity"),
                _artifact_classification(overlay, "fixture-overlay-identity"),
            ],
            "sites": [
                _site_classification(disc1, 0x00, "cop2-command", "direct-render-gte"),
                _site_classification(disc1, 0x04, "lwc2", "gte-register-load"),
                _site_classification(disc1, 0x08, "swc2", "gte-register-store"),
                _site_classification(disc1, 0x0C, "direct-jal", "gte-wrapper-call"),
                _site_classification(disc1, 0x10, "mfc2", "gte-data-read"),
                _site_classification(disc1, 0x14, "inline-ot-insert", "ordering-insertion"),
                _site_classification(disc1, 0x18, "packet-store-candidate", "ot-link-store"),
                _site_classification(disc1, 0x1C, "packet-store-candidate", "ot-head-store"),
                _site_classification(
                    disc1,
                    0x20,
                    "jalr",
                    "indirect-render-call",
                ),
                _site_classification(disc1, 0x24, "mtc2", "gte-data-write"),
                _site_classification(disc1, 0x2C, "cfc2", "gte-control-read"),
                _site_classification(disc1, 0x30, "ctc2", "gte-control-write"),
                _site_classification(
                    disc1,
                    0x34,
                    "direct-jal",
                    "non-render-call",
                    status="non-render-proven",
                    families=[],
                ),
                _site_classification(
                    disc1,
                    0x38,
                    "packet-store-candidate",
                    "non-render-word-store",
                    status="non-render-proven",
                    families=[],
                ),
                _site_classification(disc1, 0x80, "cop2-command", "gte-wrapper-body"),
            ],
            "branches": [
                {
                    "branch_id": _branch_id(disc1, 0x28, "conditional-branch"),
                    "status": "migrated-render",
                    "subject": "fixture-render-branch",
                    "reachability": "reachable",
                    "family_ids": ["fixture-render-family"],
                    "evidence_refs": ["fixture-static-review"],
                }
            ],
        },
    }
    review_path = tmp_path / "review.json"
    _write_review(review_path, review)
    return review_path, paths, review, data_by_name


def run_tool(
    command: str,
    review: Path,
    artifacts: list[Path],
    output: Path,
) -> subprocess.CompletedProcess[str]:
    arguments = [sys.executable, str(TOOL), command, str(review)]
    for artifact in artifacts:
        arguments.extend(("--artifact", str(artifact)))
    arguments.extend(("--out", str(output)))
    return subprocess.run(
        arguments,
        cwd=REPOSITORY,
        capture_output=True,
        text=True,
        check=False,
    )


def test_plan_fixtures_are_scanned_and_review_facts_remain_separate(tmp_path: Path) -> None:
    review_path, paths, _, data = fixture(tmp_path)
    output = tmp_path / "ledger.json"

    result = run_tool("ledger", review_path, paths, output)

    assert result.returncode == 0, result.stderr
    ledger = json.loads(output.read_text(encoding="ascii"))
    assert ledger["schema"] == "xg-native-3d-static-ledger/v3"
    assert ledger["static_classification_gate"] == "pass"
    assert ledger["p10_static_closure_claimed"] is True
    assert "P10 Disc 1 static closure complete" in result.stdout
    assert not any(
        "linear-scan" in limitation for limitation in ledger["known_limitations"]
    )
    assert ledger["coverage"]["unreviewed_executable_images"] == 0
    assert ledger["coverage"]["unreviewed_render_relevant_sites"] == 0
    assert ledger["coverage"]["unreviewed_branches"] == 0
    assert ledger["coverage"]["unreviewed_scanned_sites"] == 0
    assert ledger["coverage"]["total_sites"] == 15
    assert ledger["coverage"]["total_render_relevant_sites"] == 11

    facts = ledger["facts"]
    kinds = {site["kind"] for site in facts["sites"]}
    assert kinds == {
        "cfc2",
        "cop2-command",
        "ctc2",
        "direct-jal",
        "inline-ot-insert",
        "jalr",
        "lwc2",
        "mfc2",
        "mtc2",
        "packet-store-candidate",
        "swc2",
    }
    wrapper_call = next(
        site
        for site in facts["sites"]
        if site["kind"] == "direct-jal"
        and site["details"]["target_address"] == f"0x{DISC1_BASE + 0x80:08x}"
    )
    assert wrapper_call["details"]["target_address"] == f"0x{DISC1_BASE + 0x80:08x}"
    assert wrapper_call["details"]["return_address"] == f"0x{DISC1_BASE + 0x14:08x}"
    indirect = next(site for site in facts["sites"] if site["kind"] == "jalr")
    assert indirect["details"] == {
        "delay_slot_address": f"0x{DISC1_BASE + 0x24:08x}",
        "link_register": 31,
        "source_register": 25,
    }
    inline_ot = next(site for site in facts["sites"] if site["kind"] == "inline-ot-insert")
    assert inline_ot["details"]["store_addresses"] == [
        f"0x{DISC1_BASE + 0x18:08x}",
        f"0x{DISC1_BASE + 0x1C:08x}",
    ]
    transfers = {
        site["kind"]: site["details"]
        for site in facts["sites"]
        if site["kind"] in {"mfc2", "mtc2", "cfc2", "ctc2"}
    }
    assert transfers == {
        "mfc2": {"cop2_register": 3, "general_register": 2},
        "mtc2": {"cop2_register": 5, "general_register": 4},
        "cfc2": {"cop2_register": 7, "general_register": 6},
        "ctc2": {"cop2_register": 9, "general_register": 8},
    }
    general_store = next(
        site
        for site in facts["sites"]
        if site["kind"] == "packet-store-candidate"
        and site["payload_offset"] == 0x38
    )
    assert general_store["details"] == {
        "base_register": 12,
        "offset": 0x20,
        "source_register": 13,
    }
    direct_gte = next(
        site
        for site in facts["sites"]
        if site["artifact_id"] == _artifact_id(data["disc1"])
        and site["payload_offset"] == 0
    )
    assert direct_gte["details"]["command_name"] == "rtpt"
    assert not any(site["payload_offset"] == 0xC0 for site in facts["sites"])
    assert len(facts["branches"]) == 1
    assert facts["branches"][0]["payload_offset"] == 0x28
    assert facts["branches"][0]["function_id"] == "fixture-producer-function"
    assert not any(branch["payload_offset"] == 0x40 for branch in facts["branches"])
    candidate_id = _site_id(data["disc1"], 0x34, "direct-jal")
    assert set(ledger["coverage"]["candidate_call_site_ids"]) == {
        _site_id(data["disc1"], 0x0C, "direct-jal"),
        _site_id(data["disc1"], 0x20, "jalr"),
        candidate_id,
    }
    assert ledger["coverage"]["unreviewed_candidate_call_site_ids"] == []
    assert candidate_id not in ledger["coverage"]["render_relevant_site_ids"]
    candidate_review = next(
        item
        for item in ledger["reviewed_classifications"]["sites"]
        if item["site_id"] == candidate_id
    )
    assert candidate_review["status"] == "non-render-proven"
    assert candidate_review["subject"] == "non-render-call"
    assert "status" not in json.dumps(facts, sort_keys=True)
    assert "details" not in json.dumps(ledger["reviewed_classifications"], sort_keys=True)


def test_disc1_psx_exe_has_explicit_header_payload_runtime_mapping(tmp_path: Path) -> None:
    review_path, paths, _, _ = fixture(tmp_path)
    output = tmp_path / "ledger.json"
    result = run_tool("ledger", review_path, paths, output)
    assert result.returncode == 0, result.stderr

    artifacts = json.loads(output.read_text(encoding="ascii"))["facts"]["artifacts"]
    main_exes = [artifact for artifact in artifacts if artifact["role"] == "main-exe"]
    assert len(main_exes) == 1
    assert main_exes[0]["serials"] == ["SLUS-00664"]
    for artifact in main_exes:
        mapping = artifact["mapping"]
        assert mapping["format"] == "ps-x-exe"
        assert mapping["header_file_offset"] == 0
        assert mapping["header_size"] == 0x800
        assert mapping["payload_file_offset"] == 0x800
        assert mapping["runtime_address"] == "0x80010000"
        assert mapping["entry_address"] == "0x80010000"


@pytest.mark.parametrize(
    ("case", "message"),
    (
        ("missing", "fields are not closed"),
        ("empty", "executable_ranges must not be empty"),
        ("unaligned", "must be 4-byte aligned"),
        ("overlap", "executable_ranges overlap"),
        ("outside", "exceeds the authenticated payload"),
        ("entry", "do not cover the image entry"),
        ("provenance", "provenance_refs must not be empty"),
        ("evidence", "evidence_refs must not be empty"),
    ),
)
def test_executable_ranges_are_mandatory_authenticated_and_bounded(
    tmp_path: Path, case: str, message: str
) -> None:
    review_path, paths, review, _ = fixture(tmp_path)
    artifact = review["artifacts"][0]
    ranges = artifact["executable_ranges"]
    if case == "missing":
        artifact.pop("executable_ranges")
    elif case == "empty":
        artifact["executable_ranges"] = []
    elif case == "unaligned":
        ranges[0]["payload_offset"] = 1
    elif case == "overlap":
        ranges.append(_executable_range("overlap", 4, 4))
    elif case == "outside":
        ranges.append(_executable_range("outside", 0xFC, 8))
    elif case == "entry":
        ranges[0]["payload_offset"] = 4
        ranges[0]["size"] -= 4
    elif case == "provenance":
        ranges[0]["provenance_refs"] = []
    elif case == "evidence":
        ranges[0]["evidence_refs"] = []
    else:
        raise AssertionError(case)
    _write_review(review_path, review)

    result = run_tool("ledger", review_path, paths, tmp_path / f"{case}.json")

    assert result.returncode == 1
    assert message in result.stderr


def test_candidate_direct_and_indirect_calls_block_when_unreviewed(
    tmp_path: Path,
) -> None:
    review_path, paths, review, data = fixture(tmp_path)
    wrapper_call_id = _site_id(data["disc1"], 0x0C, "direct-jal")
    indirect_call_id = _site_id(data["disc1"], 0x20, "jalr")
    unrelated_call_id = _site_id(data["disc1"], 0x34, "direct-jal")
    review["render_targets"] = []
    review["classifications"]["sites"] = [
        item
        for item in review["classifications"]["sites"]
        if item["site_id"]
        not in {wrapper_call_id, indirect_call_id, unrelated_call_id}
    ]
    _write_review(review_path, review)
    inventory_output = tmp_path / "inventory.json"

    inventory_result = run_tool("inventory", review_path, paths, inventory_output)

    assert inventory_result.returncode == 0, inventory_result.stderr
    coverage = json.loads(inventory_output.read_text(encoding="ascii"))["coverage"]
    assert set(coverage["unreviewed_candidate_call_site_ids"]) == {
        wrapper_call_id,
        indirect_call_id,
        unrelated_call_id,
    }
    assert coverage["unreviewed_candidate_call_sites"] == 3

    ledger_result = run_tool("ledger", review_path, paths, tmp_path / "ledger.json")
    assert ledger_result.returncode == 1
    assert "unreviewed_scanned_sites=3" in ledger_result.stderr


def test_function_ranges_are_required_to_be_inside_executable_scope(tmp_path: Path) -> None:
    review_path, paths, review, _ = fixture(tmp_path)
    review["functions"][0]["payload_offset"] = 0x40
    review["functions"][0]["size"] = 0x10
    _write_review(review_path, review)

    result = run_tool("ledger", review_path, paths, tmp_path / "function.json")

    assert result.returncode == 1
    assert "exceeds its authenticated executable range" in result.stderr


def test_canonical_output_is_byte_identical_across_input_and_review_order(tmp_path: Path) -> None:
    review_path, paths, review, _ = fixture(tmp_path)
    first = tmp_path / "first.json"
    second = tmp_path / "second.json"
    first_result = run_tool("ledger", review_path, paths, first)
    assert first_result.returncode == 0, first_result.stderr

    reordered = deepcopy(review)
    reordered["artifacts"].reverse()
    for artifact in reordered["artifacts"]:
        artifact["executable_ranges"].reverse()
    reordered["functions"].reverse()
    reordered["render_targets"].reverse()
    for records in reordered["classifications"].values():
        records.reverse()
    _write_review(review_path, reordered)
    second_result = run_tool("ledger", review_path, list(reversed(paths)), second)

    assert second_result.returncode == 0, second_result.stderr
    assert first.read_bytes() == second.read_bytes()
    assert first.read_bytes().endswith(b"\n")
    assert b"\r" not in first.read_bytes()


def test_public_output_has_full_hashes_but_no_private_paths_or_raw_instruction_bytes(
    tmp_path: Path,
) -> None:
    review_path, paths, _, data = fixture(tmp_path)
    output = tmp_path / "ledger.json"
    result = run_tool("ledger", review_path, paths, output)
    assert result.returncode == 0, result.stderr

    encoded = output.read_text(encoding="ascii")
    assert str(tmp_path) not in encoded
    assert all(path.name not in encoded for path in paths)
    assert all(_sha256(artifact) in encoded for artifact in data.values())
    for raw_word in ("4a000030", "4a000006", "0320f809", "8c880000"):
        assert raw_word not in encoded
    assert '"bytes"' not in encoded
    assert '"instruction"' not in encoded


def test_unknown_full_hash_fails_without_publishing_or_leaking_path(tmp_path: Path) -> None:
    review_path, paths, _, _ = fixture(tmp_path)
    unknown = tmp_path / "private-unknown-overlay.bin"
    unknown.write_bytes(paths[1].read_bytes()[:-1] + b"X")
    output = tmp_path / "ledger.json"

    result = run_tool("ledger", review_path, [unknown, paths[0]], output)

    assert result.returncode == 1
    assert "unknown executable artifact SHA-256" in result.stderr
    assert str(tmp_path) not in result.stderr
    assert unknown.name not in result.stderr
    assert not output.exists()


def test_disc2_artifact_is_rejected(tmp_path: Path) -> None:
    review_path, paths, review, _ = fixture(tmp_path)
    review["artifacts"][1]["discs"] = [2]
    review["artifacts"][1]["serials"] = ["SLUS-00669"]
    _write_review(review_path, review)

    result = run_tool("ledger", review_path, paths, tmp_path / "out.json")

    assert result.returncode == 1
    assert "Disc 2 artifacts are rejected" in result.stderr


def test_disc2_serial_is_rejected_even_on_disc1_artifact(tmp_path: Path) -> None:
    review_path, paths, review, _ = fixture(tmp_path)
    review["artifacts"][1]["serials"] = ["SLUS-00669"]
    _write_review(review_path, review)

    result = run_tool("ledger", review_path, paths, tmp_path / "out.json")

    assert result.returncode == 1
    assert "Disc 2 serials are rejected" in result.stderr


@pytest.mark.parametrize("main_count", (0, 2))
def test_exactly_one_disc1_main_executable_is_required(
    tmp_path: Path, main_count: int
) -> None:
    review_path, paths, review, _ = fixture(tmp_path)
    if main_count == 0:
        review["artifacts"] = [review["artifacts"][1]]
        artifact_paths = [paths[1]]
    else:
        second_payload = bytes(0x20)
        second_main = _psx_exe(second_payload, DISC1_BASE)
        second_path = tmp_path / "second-main.bin"
        second_path.write_bytes(second_main)
        review["artifacts"].append(
            _artifact(
                second_main,
                discs=[1],
                serials=["SLUS-00664"],
                role="main-exe",
                image_format="ps-x-exe",
                base=DISC1_BASE,
                executable_ranges=[_executable_range("second-main-code", 0, 0x20)],
            )
        )
        artifact_paths = paths + [second_path]
    _write_review(review_path, review)

    result = run_tool("ledger", review_path, artifact_paths, tmp_path / "out.json")

    assert result.returncode == 1
    assert "exactly one Disc 1 main PS-X EXE SLUS-00664" in result.stderr


@pytest.mark.parametrize(
    ("classification_kind", "coverage_key"),
    (
        ("artifacts", "unreviewed_executable_images=1"),
        ("sites", "unreviewed_scanned_sites=1"),
        ("branches", "unreviewed_branches=1"),
    ),
)
def test_ledger_fails_closed_for_every_unreviewed_denominator(
    tmp_path: Path,
    classification_kind: str,
    coverage_key: str,
) -> None:
    review_path, paths, review, _ = fixture(tmp_path)
    review["classifications"][classification_kind].pop()
    _write_review(review_path, review)
    output = tmp_path / "ledger.json"
    output.write_bytes(b"do-not-replace")

    result = run_tool("ledger", review_path, paths, output)

    assert result.returncode == 1
    assert coverage_key in result.stderr
    assert output.read_bytes() == b"do-not-replace"


def test_inventory_exposes_blockers_without_misrepresenting_them_as_a_ledger(
    tmp_path: Path,
) -> None:
    review_path, paths, review, _ = fixture(tmp_path)
    review["classifications"] = {"artifacts": [], "sites": [], "branches": []}
    _write_review(review_path, review)
    output = tmp_path / "inventory.json"

    result = run_tool("inventory", review_path, paths, output)

    assert result.returncode == 0, result.stderr
    inventory = json.loads(output.read_text(encoding="ascii"))
    assert inventory["schema"] == "xg-native-3d-static-inventory/v3"
    assert inventory["static_classification_gate"] == "blocked"
    assert inventory["p10_static_closure_claimed"] is False
    assert inventory["coverage"]["unreviewed_executable_images"] == 2
    assert inventory["coverage"]["unreviewed_render_relevant_sites"] == 11
    assert inventory["coverage"]["unreviewed_branches"] == 1
    assert inventory["coverage"]["unreviewed_scanned_sites"] == 15


def test_psx_header_is_authenticated_against_declared_mapping(tmp_path: Path) -> None:
    review_path, paths, review, data = fixture(tmp_path)
    old_disc1 = data["disc1"]
    damaged = bytearray(old_disc1)
    damaged[0] ^= 0x01
    damaged_disc1 = bytes(damaged)
    paths[0].write_bytes(damaged_disc1)
    old_digest = _sha256(old_disc1)
    new_digest = _sha256(damaged_disc1)
    artifact = next(item for item in review["artifacts"] if item["sha256"] == old_digest)
    artifact["sha256"] = new_digest
    review["functions"][0]["artifact_id"] = _artifact_id(damaged_disc1)
    target = review["render_targets"][0]
    target["artifact_id"] = _artifact_id(damaged_disc1)
    target["caller_artifact_ids"] = [_artifact_id(damaged_disc1)]
    target["indirect_call_site_ids"] = [
        site_id.replace(old_digest, new_digest)
        for site_id in target["indirect_call_site_ids"]
    ]
    for section in review["classifications"].values():
        for item in section:
            for key in ("artifact_id", "site_id", "branch_id"):
                if key in item:
                    item[key] = item[key].replace(old_digest, new_digest)
    _write_review(review_path, review)

    result = run_tool("ledger", review_path, paths, tmp_path / "ledger.json")

    assert result.returncode == 1
    assert "PS-X EXE header mismatch" in result.stderr


def test_duplicate_artifact_input_is_rejected(tmp_path: Path) -> None:
    review_path, paths, _, _ = fixture(tmp_path)
    result = run_tool(
        "ledger", review_path, paths + [paths[0]], tmp_path / "ledger.json"
    )
    assert result.returncode == 1
    assert "duplicate executable artifact input" in result.stderr


@pytest.mark.parametrize("status", ("candidate", "unknown", "render-unmigrated"))
def test_nonterminal_review_status_is_rejected(tmp_path: Path, status: str) -> None:
    review_path, paths, review, _ = fixture(tmp_path)
    review["classifications"]["sites"][0]["status"] = status
    _write_review(review_path, review)
    result = run_tool("ledger", review_path, paths, tmp_path / "status.json")
    assert result.returncode == 1
    assert "terminal P10 review status" in result.stderr


@pytest.mark.parametrize(
    ("classification_kind", "blocker"),
    (
        ("artifacts", "error_executable_images=1"),
        ("sites", "error_scanned_sites=1"),
        ("branches", "error_branches=1"),
    ),
)
def test_error_review_status_blocks_ledger(
    tmp_path: Path, classification_kind: str, blocker: str
) -> None:
    review_path, paths, review, _ = fixture(tmp_path)
    record = review["classifications"][classification_kind][0]
    record["status"] = "error"
    if "family_ids" in record:
        record["family_ids"] = []
    _write_review(review_path, review)
    output = tmp_path / "ledger.json"
    output.write_bytes(b"do-not-replace")

    result = run_tool("ledger", review_path, paths, output)

    assert result.returncode == 1
    assert blocker in result.stderr
    assert output.read_bytes() == b"do-not-replace"


def test_v2_review_schema_is_not_accepted(tmp_path: Path) -> None:
    review_path, paths, review, _ = fixture(tmp_path)
    review["schema"] = "xg-native-3d-census-review/v2"
    _write_review(review_path, review)

    result = run_tool("ledger", review_path, paths, tmp_path / "ledger.json")

    assert result.returncode == 1
    assert "review schema version is unsupported" in result.stderr


def test_non_explicit_psx_mapping_is_rejected(tmp_path: Path) -> None:
    review_path, paths, review, _ = fixture(tmp_path)

    review["artifacts"][0]["mapping"]["header_size"] = 0
    _write_review(review_path, review)
    result = run_tool("ledger", review_path, paths, tmp_path / "mapping.json")
    assert result.returncode == 1
    assert "explicit 0x800-byte PS-X EXE header" in result.stderr


def test_versioned_schema_is_closed_and_lists_exact_p10_statuses() -> None:
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    assert schema["additionalProperties"] is False
    assert schema["$defs"]["mapping"]["additionalProperties"] is False
    assert schema["$defs"]["executableRange"]["additionalProperties"] is False
    assert schema["$defs"]["function"]["additionalProperties"] is False
    assert schema["$defs"]["renderTarget"]["additionalProperties"] is False
    assert set(schema["$defs"]["reviewStatus"]["enum"]) == {
        "migrated-render",
        "excluded-pure-2d-proven",
        "non-render-proven",
        "error",
    }
    assert schema["$defs"]["artifact"]["properties"]["discs"]["items"] == {
        "const": 1
    }
    assert set(schema["$defs"]["artifact"]["properties"]["role"]["enum"]) == {
        "main-exe",
        "overlay",
    }
    assert set(schema["$defs"]["renderTarget"]["properties"]["role"]["enum"]) == {
        "gte-wrapper",
        "matrix-function",
        "light-function",
        "addprim-function",
        "ot-function",
    }
