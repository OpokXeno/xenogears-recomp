from __future__ import annotations

import enum
import hmac
import os
import re
import select
from collections.abc import Callable
from typing import Final

from native_render_private_fs import PrivateNamespaceError, fail
from native_render_review_schema import ProcessIdentity

MAX_PROC_STAT_BYTES: Final = 16_384
_BOOT_ID: Final = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\Z")
_PROCESS_STATES: Final = frozenset("RSDZTWtXxKPI")


class OwnerState(enum.StrEnum):
    LIVE = "live"
    DEAD = "dead"
    INDETERMINATE = "indeterminate"


def read_process_identity(pid: int) -> ProcessIdentity:
    if pid < 1:
        fail("pid_invalid")
    return ProcessIdentity(read_boot_id(), pid, read_proc_starttime(pid))


def read_boot_id() -> str:
    try:
        descriptor = os.open("/proc/sys/kernel/random/boot_id", os.O_RDONLY | os.O_CLOEXEC)
    except OSError as error:
        raise PrivateNamespaceError("boot_id_open_failed") from error
    try:
        raw = os.read(descriptor, 128)
    finally:
        os.close(descriptor)
    try:
        boot_id = raw.decode("ascii").strip()
    except UnicodeDecodeError as error:
        raise PrivateNamespaceError("boot_id_invalid") from error
    if _BOOT_ID.fullmatch(boot_id) is None:
        fail("boot_id_invalid")
    return boot_id


def read_proc_starttime(pid: int) -> int:
    try:
        descriptor = os.open(f"/proc/{pid}/stat", os.O_RDONLY | os.O_CLOEXEC)
    except FileNotFoundError as error:
        raise ProcessLookupError(pid) from error
    except OSError as error:
        raise PrivateNamespaceError("proc_stat_open_failed") from error
    try:
        raw = os.read(descriptor, MAX_PROC_STAT_BYTES + 1)
    finally:
        os.close(descriptor)
    return parse_proc_starttime(raw, pid)


def parse_proc_starttime(raw: bytes, expected_pid: int) -> int:
    if expected_pid < 1 or len(raw) > MAX_PROC_STAT_BYTES:
        fail("proc_stat_invalid")
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError as error:
        raise PrivateNamespaceError("proc_stat_invalid") from error
    closing = text.rfind(")")
    prefix = f"{expected_pid} ("
    if closing <= len(prefix) or not text.startswith(prefix):
        fail("proc_stat_invalid")
    trailing = text[closing + 1 :].strip().split()
    if len(trailing) < 20 or len(trailing[0]) != 1 or trailing[0] not in _PROCESS_STATES:
        fail("proc_stat_invalid")
    starttime = trailing[19]
    if not starttime.isdecimal():
        fail("proc_stat_invalid")
    parsed = int(starttime)
    if parsed < 1:
        fail("proc_stat_invalid")
    return parsed


def _poll_owner(descriptor: int, live: OwnerState) -> OwnerState:
    events = select.poll()
    events.register(descriptor, select.POLLIN | select.POLLHUP | select.POLLERR)
    try:
        return OwnerState.DEAD if events.poll(0) else live
    except OSError:
        return OwnerState.INDETERMINATE


def owner_state(owner: ProcessIdentity, identity_reader: Callable[[int], ProcessIdentity] = read_process_identity) -> OwnerState:
    try:
        boot_id = read_boot_id()
    except PrivateNamespaceError:
        return OwnerState.INDETERMINATE
    if not hmac.compare_digest(owner.boot_id, boot_id):
        return OwnerState.DEAD
    try:
        descriptor = os.pidfd_open(owner.pid, 0)
    except ProcessLookupError:
        return OwnerState.DEAD
    except (AttributeError, OSError):
        return OwnerState.INDETERMINATE
    try:
        try:
            observed = identity_reader(owner.pid)
        except ProcessLookupError:
            return _poll_owner(descriptor, OwnerState.INDETERMINATE)
        except PrivateNamespaceError:
            return OwnerState.INDETERMINATE
        if observed != owner:
            return OwnerState.DEAD
        return _poll_owner(descriptor, OwnerState.LIVE)
    finally:
        os.close(descriptor)
