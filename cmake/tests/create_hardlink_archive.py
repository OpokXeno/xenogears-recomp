#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = []
# ///
#
# How to run:
#   uv run --script cmake/tests/create_hardlink_archive.py <package-root> <archive>

from __future__ import annotations

import sys
import tarfile
from pathlib import Path
from typing import Final


HARDLINK_RELATIVE_PATH: Final = "assets/hardlink-entry"
HARDLINK_TARGET_RELATIVE_PATH: Final = "assets/placeholder.txt"


def create_hardlink_archive(package_root: Path, archive_path: Path) -> None:
    package_name = package_root.name
    hardlink_name = f"{package_name}/{HARDLINK_RELATIVE_PATH}"
    hardlink_target = f"{package_name}/{HARDLINK_TARGET_RELATIVE_PATH}"

    with tarfile.open(archive_path, mode="w:gz", format=tarfile.GNU_FORMAT) as archive:
        archive.add(package_root, arcname=package_name)
        hardlink_entry = tarfile.TarInfo(hardlink_name)
        hardlink_entry.type = tarfile.LNKTYPE
        hardlink_entry.linkname = hardlink_target
        hardlink_entry.mode = 0o644
        hardlink_entry.mtime = 0
        archive.addfile(hardlink_entry)

    with tarfile.open(archive_path, mode="r:gz") as archive:
        hardlink_entry = archive.getmember(hardlink_name)
        hardlink_target_entry = archive.getmember(hardlink_target)

    if not hardlink_entry.islnk():
        raise SystemExit(f"Hardlink fixture entry is not a hardlink: {hardlink_name}")
    if hardlink_entry.type != tarfile.LNKTYPE:
        raise SystemExit(f"Hardlink fixture entry has the wrong type: {hardlink_name}")
    if hardlink_entry.linkname != hardlink_target:
        raise SystemExit(f"Hardlink fixture entry targets the wrong member: {hardlink_name}")
    if not hardlink_target_entry.isreg():
        raise SystemExit(f"Hardlink fixture target is not a regular file: {hardlink_target}")


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(f"Usage: {Path(sys.argv[0]).name} <package-root> <archive>")

    create_hardlink_archive(Path(sys.argv[1]), Path(sys.argv[2]))


if __name__ == "__main__":
    main()
