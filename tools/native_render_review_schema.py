from __future__ import annotations

import hashlib
import hmac
import re
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Final

from native_render_atomic import canonical_ascii_json
from native_render_schema import ContractError, JsonObject, JsonValue

PRIVATE_LEASE_SCHEMA: Final = "xenogears.native-render-private-lease/v1"
_U64: Final = 0xFFFFFFFFFFFFFFFF
_BOOT_ID: Final = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\Z")
_HEX_64: Final = re.compile(r"[0-9a-f]{64}\Z")


def _record(value: JsonValue, name: str) -> JsonObject:
    if not isinstance(value, Mapping):
        raise ContractError(f"{name}_object_required")
    return value


def _keys(record: JsonObject, fields: frozenset[str], name: str) -> None:
    if set(record) != set(fields):
        raise ContractError(f"{name}_keys_invalid")


def _hex(value: JsonValue, name: str) -> str:
    if not isinstance(value, str) or _HEX_64.fullmatch(value) is None:
        raise ContractError(f"{name}_invalid")
    return value


@dataclass(frozen=True, slots=True)
class ProcessIdentity:
    boot_id: str
    pid: int
    starttime_ticks: int

    def to_json(self) -> JsonObject:
        return {"boot_id": self.boot_id, "pid": self.pid, "starttime_ticks": self.starttime_ticks}


@dataclass(frozen=True, slots=True)
class PrivateLease:
    lane: str
    run_id: str
    owner: ProcessIdentity
    token_digest: str
    lifecycle: str
    mac: str

    def to_json(self) -> JsonObject:
        return {
            "schema": PRIVATE_LEASE_SCHEMA, "lane": self.lane, "run_id": self.run_id,
            "owner": self.owner.to_json(), "token_digest": self.token_digest,
            "lifecycle": self.lifecycle, "mac": self.mac,
        }


def _unsigned(record: JsonObject) -> JsonObject:
    _keys(record, frozenset({"schema", "lane", "run_id", "owner", "token_digest", "lifecycle"}), "private_lease")
    return record


def sign_private_lease(value: JsonObject, key: bytes) -> JsonObject:
    if len(key) != 32:
        raise ContractError("lease_key_invalid")
    unsigned = _unsigned(value)
    encoded = canonical_ascii_json(unsigned)
    signed = dict(unsigned)
    signed["mac"] = hmac.new(key, encoded, hashlib.sha256).hexdigest()
    return signed


def parse_private_lease(value: JsonValue, key: bytes) -> PrivateLease:
    if len(key) != 32:
        raise ContractError("lease_key_invalid")
    record = _record(value, "private_lease")
    _keys(record, frozenset({"schema", "lane", "run_id", "owner", "token_digest", "lifecycle", "mac"}), "private_lease")
    if record["schema"] != PRIVATE_LEASE_SCHEMA:
        raise ContractError("private_lease_schema_invalid")
    lane = record["lane"]
    if not isinstance(lane, str) or lane not in {"comparison", "evidence", "review-f2", "review-f3"}:
        raise ContractError("private_lease_lane_invalid")
    owner = _record(record["owner"], "private_lease_owner")
    _keys(owner, frozenset({"boot_id", "pid", "starttime_ticks"}), "private_lease_owner")
    boot_id = owner["boot_id"]
    pid = owner["pid"]
    starttime_ticks = owner["starttime_ticks"]
    if not isinstance(boot_id, str) or _BOOT_ID.fullmatch(boot_id) is None or isinstance(pid, bool) or not isinstance(pid, int) or not 1 <= pid <= 0x7FFFFFFF or isinstance(starttime_ticks, bool) or not isinstance(starttime_ticks, int) or not 1 <= starttime_ticks <= _U64:
        raise ContractError("private_lease_owner_invalid")
    lifecycle = record["lifecycle"]
    if lifecycle not in {"active", "finalized"}:
        raise ContractError("private_lease_lifecycle_invalid")
    mac = _hex(record["mac"], "private_lease_mac")
    unsigned: JsonObject = {key_name: item for key_name, item in record.items() if key_name != "mac"}
    expected = hmac.new(key, canonical_ascii_json(_unsigned(unsigned)), hashlib.sha256).hexdigest()
    if not hmac.compare_digest(mac, expected):
        raise ContractError("private_lease_mac_invalid")
    return PrivateLease(lane, _hex(record["run_id"], "private_lease_run_id"), ProcessIdentity(boot_id, pid, starttime_ticks), _hex(record["token_digest"], "private_lease_token_digest"), lifecycle, mac)
