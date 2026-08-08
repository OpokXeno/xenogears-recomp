#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///

from __future__ import annotations

import argparse
import os
from pathlib import Path

from native_render_manifest_model import ManifestError
from native_render_manifest_output import atomic_write
from native_render_runtime_variant_model import load_contract
from native_render_runtime_variant_output import render_c
from native_render_runtime_variant_verify import VerificationInputs, verify


REPOSITORY = Path(__file__).resolve().parents[1]
CANONICAL_MANIFEST = REPOSITORY / "native_renderer" / "xg_render_manifest.toml"


def add_inputs(command: argparse.ArgumentParser) -> None:
    command.add_argument("companion", type=Path)
    command.add_argument("--artifact", type=Path, required=True)
    command.add_argument("--canonical-manifest", type=Path, default=CANONICAL_MANIFEST)


def main() -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate")
    add_inputs(validate)
    emit = commands.add_parser("emit")
    add_inputs(emit)
    emit.add_argument("--out", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        inputs = VerificationInputs(arguments.companion, arguments.canonical_manifest,
                                    arguments.artifact)
        verified = verify(load_contract(arguments.companion), inputs)
        if arguments.command == "validate":
            print("validate PASS: one identity-bound field5 runtime descriptor")
            return 0
        atomic_write(arguments.out, render_c(verified))
        print("emit PASS: one identity-bound field5 runtime descriptor")
        return 0
    except ManifestError as error:
        print(f"FAIL: {error}", file=os.sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
