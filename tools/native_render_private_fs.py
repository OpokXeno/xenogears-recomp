from __future__ import annotations

import os
import re
import secrets
import stat
from collections.abc import Callable
from typing import Final, NoReturn

from native_render_atomic import canonical_ascii_json
from native_render_schema import JsonObject

REVIEW_BASE: Final = "/tmp/opencode/xg-native-render-review"
LEASE_NAME: Final = "lease.json"
DIRECTORY_FLAGS: Final = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
FILE_FLAGS: Final = os.O_NOFOLLOW | os.O_CLOEXEC
_COMPONENT: Final = re.compile(r"[A-Za-z0-9._-]+\Z")


class PrivateNamespaceError(RuntimeError):
    def __init__(self, code: str) -> None:
        super().__init__(code)
        self.code = code

    def __str__(self) -> str:
        return self.code


def fail(code: str) -> NoReturn:
    raise PrivateNamespaceError(code)


def new_component(prefix: str) -> str:
    return f"{prefix}-{secrets.token_hex(16)}"


def require_component(name: str) -> None:
    if _COMPONENT.fullmatch(name) is None or name in {".", ".."}:
        fail("component_invalid")


def same_inode(first: os.stat_result, second: os.stat_result) -> bool:
    return first.st_dev == second.st_dev and first.st_ino == second.st_ino


def directory_stat(descriptor: int, *, private: bool = True) -> os.stat_result:
    observed = os.fstat(descriptor)
    if not stat.S_ISDIR(observed.st_mode):
        fail("directory_required")
    mode = stat.S_IMODE(observed.st_mode)
    if observed.st_uid != os.getuid() or (private and mode != 0o700) or (not private and mode & 0o022):
        fail("directory_protection_invalid")
    return observed


def open_verified_directory(parent_fd: int, name: str, *, private: bool = True) -> tuple[int, os.stat_result]:
    require_component(name)
    try:
        named = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
        if not stat.S_ISDIR(named.st_mode):
            fail("directory_required")
        descriptor = os.open(name, DIRECTORY_FLAGS, dir_fd=parent_fd)
    except OSError as error:
        raise PrivateNamespaceError("directory_open_failed") from error
    try:
        opened = directory_stat(descriptor, private=private)
        if not same_inode(named, opened):
            fail("directory_raced")
        return descriptor, opened
    except (OSError, PrivateNamespaceError):
        os.close(descriptor)
        raise


def create_verified_directory(parent_fd: int, prefix: str, component_factory: Callable[[str], str]) -> tuple[str, int, os.stat_result]:
    for _ in range(32):
        name = component_factory(prefix)
        require_component(name)
        try:
            os.mkdir(name, 0o700, dir_fd=parent_fd)
        except FileExistsError:
            continue
        except OSError as error:
            raise PrivateNamespaceError("directory_create_failed") from error
        descriptor, observed = open_verified_directory(parent_fd, name)
        try:
            os.fchmod(descriptor, 0o700)
            protected = directory_stat(descriptor)
            if not same_inode(observed, protected):
                fail("directory_raced")
            return name, descriptor, protected
        except (OSError, PrivateNamespaceError):
            os.close(descriptor)
            raise
    fail("directory_collision_limit")


def _open_or_create_base_component(parent_fd: int, name: str, *, private: bool) -> tuple[int, os.stat_result]:
    require_component(name)
    try:
        os.mkdir(name, 0o700, dir_fd=parent_fd)
    except FileExistsError:
        pass
    except OSError as error:
        raise PrivateNamespaceError("base_create_failed") from error
    descriptor, observed = open_verified_directory(parent_fd, name, private=private)
    try:
        protected = directory_stat(descriptor, private=private)
        if not same_inode(observed, protected):
            fail("directory_raced")
        return descriptor, protected
    except (OSError, PrivateNamespaceError):
        os.close(descriptor)
        raise


def open_review_base() -> int:
    try:
        temporary_fd = os.open("/tmp", DIRECTORY_FLAGS)
    except OSError as error:
        raise PrivateNamespaceError("temporary_root_open_failed") from error
    try:
        opencode_fd, _ = _open_or_create_base_component(temporary_fd, "opencode", private=False)
        try:
            review_fd, _ = _open_or_create_base_component(opencode_fd, "xg-native-render-review", private=True)
        finally:
            os.close(opencode_fd)
        return review_fd
    finally:
        os.close(temporary_fd)


def assert_directory_entry(parent_fd: int, name: str, device: int, inode: int) -> None:
    require_component(name)
    try:
        observed = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
    except OSError as error:
        raise PrivateNamespaceError("namespace_entry_missing") from error
    if not stat.S_ISDIR(observed.st_mode) or observed.st_dev != device or observed.st_ino != inode:
        fail("namespace_entry_raced")


def write_lease(run_fd: int, lease: JsonObject) -> None:
    encoded = canonical_ascii_json(lease)
    try:
        descriptor = os.open(LEASE_NAME, os.O_WRONLY | os.O_CREAT | os.O_EXCL | FILE_FLAGS, 0o600, dir_fd=run_fd)
    except OSError as error:
        raise PrivateNamespaceError("lease_create_failed") from error
    try:
        os.fchmod(descriptor, 0o600)
        if os.write(descriptor, encoded) != len(encoded):
            fail("lease_write_incomplete")
        os.fsync(descriptor)
        observed = os.fstat(descriptor)
        if not stat.S_ISREG(observed.st_mode) or observed.st_uid != os.getuid() or stat.S_IMODE(observed.st_mode) != 0o600:
            fail("lease_protection_invalid")
    finally:
        os.close(descriptor)
    os.fsync(run_fd)


def remove_exact_tree(directory_fd: int) -> None:
    try:
        entries = os.listdir(directory_fd)
    except OSError as error:
        raise PrivateNamespaceError("namespace_list_failed") from error
    for name in entries:
        require_component(name)
        try:
            observed = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        except OSError as error:
            raise PrivateNamespaceError("namespace_entry_missing") from error
        if stat.S_ISDIR(observed.st_mode):
            try:
                child_fd = os.open(name, DIRECTORY_FLAGS, dir_fd=directory_fd)
            except OSError as error:
                raise PrivateNamespaceError("namespace_child_open_failed") from error
            try:
                child_stat = directory_stat(child_fd)
                if not same_inode(observed, child_stat):
                    fail("namespace_entry_raced")
                remove_exact_tree(child_fd)
            finally:
                os.close(child_fd)
            assert_directory_entry(directory_fd, name, observed.st_dev, observed.st_ino)
            try:
                os.rmdir(name, dir_fd=directory_fd)
            except OSError as error:
                raise PrivateNamespaceError("namespace_remove_failed") from error
        else:
            try:
                os.unlink(name, dir_fd=directory_fd)
            except OSError as error:
                raise PrivateNamespaceError("namespace_remove_failed") from error
