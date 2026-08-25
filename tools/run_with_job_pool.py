#!/usr/bin/env python3
"""Run a command while holding one slot in a cross-platform process pool."""

from __future__ import annotations

import argparse
import os
import subprocess
import time
from pathlib import Path
from typing import BinaryIO


if os.name == "nt":
    import msvcrt
else:
    import fcntl


def _try_lock(handle: BinaryIO) -> bool:
    try:
        if os.name == "nt":
            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return False
    return True


def _unlock(handle: BinaryIO) -> None:
    if os.name == "nt":
        handle.seek(0)
        msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
    else:
        fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def acquire_slot(lock_dir: Path, jobs: int) -> BinaryIO:
    if jobs < 1:
        raise ValueError("jobs must be a positive integer")
    lock_dir.mkdir(parents=True, exist_ok=True)
    while True:
        for index in range(jobs):
            handle = (lock_dir / f"slot-{index}.lock").open("a+b")
            if handle.tell() == 0:
                handle.write(b"\0")
                handle.flush()
            if _try_lock(handle):
                return handle
            handle.close()
        time.sleep(0.05)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock-dir", required=True, type=Path)
    parser.add_argument("--jobs", required=True, type=int)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        parser.error("a command is required after --")

    slot = acquire_slot(args.lock_dir, args.jobs)
    try:
        return subprocess.run(command, check=False).returncode
    finally:
        _unlock(slot)
        slot.close()


if __name__ == "__main__":
    raise SystemExit(main())
