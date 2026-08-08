from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
import stat
from typing import Final

from native_render_atomic import canonical_ascii_json, load_bounded_json
import native_render_private_fs as private_fs
import native_render_process_identity as process_identity
from native_render_review_schema import ProcessIdentity
from native_render_schema import ContractError, JsonValue


SCRATCH_ROOT: Final = Path("/tmp/opencode/xg-native-render-producer-family")
_OWNER_FILE: Final = "owner.json"
_OWNER_SCHEMA: Final = "xenogears.producer-family-scratch-owner/v1"


@dataclass(frozen=True, slots=True)
class ProducerFamilyScratch:
    path: Path
    parent_fd: int
    directory_fd: int
    name: str
    device: int
    inode: int

    def close(self) -> None:
        os.close(self.directory_fd)
        os.close(self.parent_fd)


def _open_root() -> int:
    parent = SCRATCH_ROOT.parent
    parent_fd = os.open(parent, private_fs.DIRECTORY_FLAGS)
    try:
        try:
            os.mkdir(SCRATCH_ROOT.name, 0o700, dir_fd=parent_fd)
        except FileExistsError:
            pass
        root_fd, observed = private_fs.open_verified_directory(
            parent_fd, SCRATCH_ROOT.name,
        )
        if observed.st_uid != os.getuid() or stat.S_IMODE(observed.st_mode) != 0o700:
            raise private_fs.PrivateNamespaceError("scratch_root_protection_invalid")
        return root_fd
    finally:
        os.close(parent_fd)


def _owner_json(owner: ProcessIdentity) -> dict[str, object]:
    return {"schema": _OWNER_SCHEMA, "owner": owner.to_json()}


def _parse_owner(value: JsonValue) -> ProcessIdentity:
    if not isinstance(value, dict) or set(value) != {"schema", "owner"} or value["schema"] != _OWNER_SCHEMA:
        raise ContractError("scratch_owner_shape_invalid")
    owner = value["owner"]
    if not isinstance(owner, dict) or set(owner) != {"boot_id", "pid", "starttime_ticks"}:
        raise ContractError("scratch_owner_shape_invalid")
    boot_id = owner["boot_id"]
    pid = owner["pid"]
    starttime = owner["starttime_ticks"]
    if (
        not isinstance(boot_id, str)
        or isinstance(pid, bool)
        or not isinstance(pid, int)
        or isinstance(starttime, bool)
        or not isinstance(starttime, int)
        or pid < 1
        or starttime < 1
    ):
        raise ContractError("scratch_owner_value_invalid")
    return ProcessIdentity(boot_id, pid, starttime)


def _write_owner(directory_fd: int, owner: ProcessIdentity) -> None:
    encoded = canonical_ascii_json(_owner_json(owner))
    descriptor = os.open(
        _OWNER_FILE,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | private_fs.FILE_FLAGS,
        0o600,
        dir_fd=directory_fd,
    )
    try:
        os.fchmod(descriptor, 0o600)
        if os.write(descriptor, encoded) != len(encoded):
            raise private_fs.PrivateNamespaceError("scratch_owner_write_incomplete")
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    os.fsync(directory_fd)


def replace_producer_family_scratch_owner(
    scratch: ProducerFamilyScratch, owner: ProcessIdentity,
) -> None:
    encoded = canonical_ascii_json(_owner_json(owner))
    temporary = "owner.next"
    descriptor = os.open(
        temporary,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | private_fs.FILE_FLAGS,
        0o600,
        dir_fd=scratch.directory_fd,
    )
    try:
        os.fchmod(descriptor, 0o600)
        if os.write(descriptor, encoded) != len(encoded):
            raise private_fs.PrivateNamespaceError("scratch_owner_write_incomplete")
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    os.replace(
        temporary, _OWNER_FILE,
        src_dir_fd=scratch.directory_fd, dst_dir_fd=scratch.directory_fd,
    )
    os.fsync(scratch.directory_fd)


def _read_owner(directory_fd: int) -> ProcessIdentity:
    named = os.stat(_OWNER_FILE, dir_fd=directory_fd, follow_symlinks=False)
    if (
        not stat.S_ISREG(named.st_mode)
        or named.st_uid != os.getuid()
        or stat.S_IMODE(named.st_mode) != 0o600
    ):
        raise private_fs.PrivateNamespaceError("scratch_owner_protection_invalid")
    descriptor = os.open(
        _OWNER_FILE, os.O_RDONLY | private_fs.FILE_FLAGS, dir_fd=directory_fd,
    )
    try:
        opened = os.fstat(descriptor)
        if not private_fs.same_inode(named, opened):
            raise private_fs.PrivateNamespaceError("scratch_owner_raced")
        raw = os.read(descriptor, 1_000_001)
    finally:
        os.close(descriptor)
    return _parse_owner(load_bounded_json(raw))


def _remove_scratch_tree(directory_fd: int) -> None:
    for name in os.listdir(directory_fd):
        private_fs.require_component(name)
        named = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        if stat.S_ISDIR(named.st_mode):
            child_fd = os.open(
                name, private_fs.DIRECTORY_FLAGS, dir_fd=directory_fd,
            )
            try:
                opened = private_fs.directory_stat(child_fd, private=False)
                if opened.st_uid != os.getuid() or not private_fs.same_inode(named, opened):
                    raise private_fs.PrivateNamespaceError("scratch_entry_raced")
                _remove_scratch_tree(child_fd)
            finally:
                os.close(child_fd)
            private_fs.assert_directory_entry(
                directory_fd, name, named.st_dev, named.st_ino,
            )
            os.rmdir(name, dir_fd=directory_fd)
        else:
            os.unlink(name, dir_fd=directory_fd)


def create_producer_family_scratch(
    owner: ProcessIdentity | None = None,
) -> ProducerFamilyScratch:
    root_fd = _open_root()
    try:
        name, directory_fd, observed = private_fs.create_verified_directory(
            root_fd, "run", private_fs.new_component,
        )
        try:
            _write_owner(
                directory_fd,
                owner or process_identity.read_process_identity(os.getpid()),
            )
        except BaseException:
            _remove_scratch_tree(directory_fd)
            os.close(directory_fd)
            os.rmdir(name, dir_fd=root_fd)
            raise
        return ProducerFamilyScratch(
            SCRATCH_ROOT / name, root_fd, directory_fd, name,
            observed.st_dev, observed.st_ino,
        )
    except BaseException:
        os.close(root_fd)
        raise


def destroy_producer_family_scratch(scratch: ProducerFamilyScratch) -> None:
    try:
        _remove_scratch_tree(scratch.directory_fd)
        private_fs.assert_directory_entry(
            scratch.parent_fd, scratch.name, scratch.device, scratch.inode,
        )
        os.rmdir(scratch.name, dir_fd=scratch.parent_fd)
        os.fsync(scratch.parent_fd)
    finally:
        scratch.close()


def recover_stale_producer_family_scratch() -> int:
    root_fd = _open_root()
    recovered = 0
    try:
        for name in os.listdir(root_fd):
            private_fs.require_component(name)
            directory_fd, observed = private_fs.open_verified_directory(root_fd, name)
            try:
                owner = _read_owner(directory_fd)
                state = process_identity.owner_state(
                    owner, process_identity.read_process_identity,
                )
                if state is process_identity.OwnerState.LIVE:
                    continue
                if state is process_identity.OwnerState.INDETERMINATE:
                    raise private_fs.PrivateNamespaceError("scratch_owner_indeterminate")
                _remove_scratch_tree(directory_fd)
                private_fs.assert_directory_entry(
                    root_fd, name, observed.st_dev, observed.st_ino,
                )
                os.rmdir(name, dir_fd=root_fd)
                os.fsync(root_fd)
                recovered += 1
            finally:
                os.close(directory_fd)
        return recovered
    finally:
        os.close(root_fd)
