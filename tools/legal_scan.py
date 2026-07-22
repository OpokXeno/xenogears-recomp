#!/usr/bin/env python3
"""Legal scanner — pre-commit gate (master plan 1.2).

Scans STAGED files and refuses the commit if any of them looks like
copyrighted game data. Never allow: PS-X EXE / VAG / TIM magic, BIOS-sized
files, disc images, or the overlay_captures.json runtime capture.

Exit 0 = clean. Exit 1 = at least one staged file tripped a rule.
"""

import os
import subprocess
import sys

MAGIC_RULES = [
    (b"PS-X EXE", "PS-X EXE header (game executable)"),
    (b"VAGp", "VAG audio header (PSX sound data)"),
    (b"\x10\x00\x00\x00", "TIM magic (PSX image data)"),
]
MAGIC_WINDOW = 16          # "near offset 0"
BIOS_SIZE = 524288         # exactly 512 KiB -> BIOS-sized
FORBIDDEN_NAMES = {"overlay_captures.json"}
FORBIDDEN_EXTS = {".cue", ".bin"}


def staged_files():
    out = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACM"],
        capture_output=True, text=True, check=True,
    )
    return [line for line in out.stdout.splitlines() if line.strip()]


def scan(path):
    problems = []
    lower = path.lower()
    base = lower.rsplit("/", 1)[-1]

    # A staged path that is a directory on disk is a submodule gitlink —
    # a commit pointer with no content of its own to scan.
    if os.path.isdir(path):
        return problems

    if base in FORBIDDEN_NAMES:
        problems.append(f"forbidden filename '{base}' (runtime capture data)")
    for ext in FORBIDDEN_EXTS:
        if lower.endswith(ext):
            problems.append(f"forbidden extension '{ext}' (disc image data)")
            break

    try:
        with open(path, "rb") as fh:
            head = fh.read(MAGIC_WINDOW)
            fh.seek(0, 2)
            size = fh.tell()
    except OSError as exc:
        return [f"unreadable staged file ({exc}) — cannot certify clean"]

    if size == BIOS_SIZE:
        problems.append("exactly 524288 bytes (BIOS-sized)")
    for magic, desc in MAGIC_RULES:
        if magic in head:
            problems.append(f"{desc} found within first {MAGIC_WINDOW} bytes")
    return problems


def main():
    failures = {}
    for path in staged_files():
        probs = scan(path)
        if probs:
            failures[path] = probs

    if not failures:
        return 0

    print("LEGAL SCAN FAILED — refusing to commit copyrighted game data:",
          file=sys.stderr)
    for path, probs in failures.items():
        for p in probs:
            print(f"  {path}: {p}", file=sys.stderr)
    print("See master plan 1 (legal rules). Unstage with: git reset -- <file>",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
