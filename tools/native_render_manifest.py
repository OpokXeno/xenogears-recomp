#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///

from __future__ import annotations

import argparse
import os
from pathlib import Path

from native_render_manifest_fixture import run_fixture
from native_render_manifest_model import ManifestError, load_contract
from native_render_manifest_output import (
    atomic_write,
    check_shard_payload,
    check_table_payload,
    configuration_payload,
    evidence_payload,
    json_bytes,
    load_json,
    render_c,
    table_payload,
    validate_evidence_payload,
)
from native_render_manifest_verify import VerificationInputs, declare, verify


REPOSITORY = Path(__file__).resolve().parents[1]
CANONICAL_MANIFEST = REPOSITORY / "native_renderer" / "xg_render_manifest.toml"


def add_inputs(command: argparse.ArgumentParser) -> None:
    command.add_argument("manifest", type=Path)
    command.add_argument("--exe", type=Path, required=True)
    command.add_argument("--overlays", type=Path, required=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate")
    add_inputs(validate)
    validate.add_argument("--evidence", type=Path)
    emit = commands.add_parser("emit")
    add_inputs(emit)
    emit.add_argument("--out", type=Path, required=True)
    emit.add_argument("--metadata-out", type=Path)
    metadata = commands.add_parser("metadata")
    add_inputs(metadata)
    metadata_declared = commands.add_parser("metadata-declared")
    metadata_declared.add_argument("manifest", type=Path)
    emit_declared = commands.add_parser("emit-declared")
    emit_declared.add_argument("manifest", type=Path)
    emit_declared.add_argument("--out", type=Path, required=True)
    check = commands.add_parser("check-metadata")
    add_inputs(check)
    check.add_argument("--table", type=Path, required=True)
    check.add_argument("--shard-export", type=Path, action="append", default=[])
    contract = commands.add_parser("contract")
    contract.add_argument("manifest", type=Path)
    evidence = commands.add_parser("validate-evidence")
    evidence.add_argument("evidence", type=Path)
    self_test = commands.add_parser("self-test")
    self_test.add_argument("--evidence", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        if arguments.command == "contract":
            blockers = load_contract(arguments.manifest).blockers()
            if blockers:
                raise ManifestError(f"contract blocked: {','.join(blockers)}")
            print("contract PASS: all required records are authenticated")
            return 0
        if arguments.command == "validate-evidence":
            validate_evidence_payload(load_json(arguments.evidence))
            print("evidence PASS: closed metadata-only schema")
            return 0
        if arguments.command == "self-test":
            payload = run_fixture(CANONICAL_MANIFEST)
            validate_evidence_payload(payload)
            atomic_write(arguments.evidence, json_bytes(payload))
            print("self-test PASS: real validation/emission paths; temporary fixture removed")
            return 0
        if arguments.command in {"metadata-declared", "emit-declared"}:
            verified = declare(load_contract(arguments.manifest), arguments.manifest)
            if arguments.command == "metadata-declared":
                print(json_bytes(configuration_payload(verified)).decode("ascii"), end="")
                return 0
            atomic_write(arguments.out, render_c(verified))
            print("emit PASS: declared native-render metadata")
            return 0
        inputs = VerificationInputs(arguments.manifest, arguments.exe, arguments.overlays)
        verified = verify(load_contract(arguments.manifest), inputs)
        if arguments.command == "metadata":
            print(json_bytes(configuration_payload(verified)).decode("ascii"), end="")
            return 0
        if arguments.command == "validate":
            if arguments.evidence is not None:
                payload = evidence_payload(verified)
                validate_evidence_payload(payload)
                atomic_write(arguments.evidence, json_bytes(payload))
            print(f"validate PASS: {len(verified.records)} authenticated record(s)")
            return 0
        if arguments.command == "emit":
            atomic_write(arguments.out, render_c(verified))
            if arguments.metadata_out is not None:
                atomic_write(arguments.metadata_out, json_bytes(table_payload(verified)))
            print(f"emit PASS: {len(verified.records)} sorted metadata-only record(s)")
            return 0
        check_table_payload(verified, load_json(arguments.table))
        for shard_path in arguments.shard_export:
            check_shard_payload(verified, load_json(shard_path))
        print(f"check-metadata PASS: table and {len(arguments.shard_export)} shard export(s)")
        return 0
    except ManifestError as error:
        print(f"FAIL: {error}", file=os.sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
