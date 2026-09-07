#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


PLAN_SCHEMA = "psxrecomp-source-observation-plan-v5"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    arguments = parser.parse_args()
    command = arguments.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("a command is required after --")

    lines = arguments.plan.read_text(encoding="ascii").splitlines()
    if not lines or lines[0] != PLAN_SCHEMA:
        parser.error("source observation plan has an unsupported schema")
    if any(line.strip() for line in lines[1:]):
        command.extend(("--source-observation-plan", str(arguments.plan)))
    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
