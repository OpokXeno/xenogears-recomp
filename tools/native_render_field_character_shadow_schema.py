from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from typing import Final, NoReturn, TypeIs

from native_render_atomic import canonical_ascii_json
from native_render_schema import ContractError, JsonObject, JsonValue

FIELD_CHARACTER_SHADOW_SCHEMA: Final = "xenogears.field-character-shadow/v1"
MAX_ACCESSES: Final = 256
MAX_FAMILIES: Final = 64
MAX_SITES: Final = 16
SELECTION_VBLANKS: Final = 3600
MINIMUM_FAMILY_COUNT: Final = 1000
_U8: Final = 0xFF
_U16: Final = 0xFFFF
_U32: Final = 0xFFFFFFFF
_U64: Final = 0xFFFFFFFFFFFFFFFF
_ROOT_KEYS: Final = frozenset({"schema", "phase", "status", "blocker", "window", "selection", "coverage", "effects", "determinism", "accesses", "privacy", "cleanup"})
_PHASES: Final = frozenset({"idle", "collecting", "complete", "blocked"})
_BLOCKERS: Final = frozenset({"none", "malformed-auth", "nested-begin", "unmatched-end", "stale-auth", "auth-loss", "unauthenticated-observation", "malformed-packet", "malformed-access", "malformed-effect", "malformed-coverage", "family-capacity", "access-capacity", "site-capacity", "counter-saturated", "early-window", "late-window", "insufficient-count", "insufficient-coverage"})
_STATUS_BY_PHASE: Final = {"idle": "pending", "collecting": "pending", "complete": "pass", "blocked": "blocked"}


def _fail(code: str) -> NoReturn:
    raise ContractError(code)


def _is_record(value: JsonValue) -> TypeIs[JsonObject]:
    return isinstance(value, Mapping)


def _is_array(value: JsonValue) -> TypeIs[list[JsonValue]]:
    return isinstance(value, list)


def _is_integer(value: JsonValue) -> TypeIs[int]:
    return isinstance(value, int) and not isinstance(value, bool)


def _record(value: JsonValue, label: str) -> JsonObject:
    if not _is_record(value):
        _fail(f"{label}_object_required")
    return value


def _keys(record: JsonObject, fields: frozenset[str], label: str) -> None:
    if set(record) != set(fields):
        _fail(f"{label}_keys_invalid")


def _integer(value: JsonValue, label: str, maximum: int = _U64) -> int:
    if not _is_integer(value) or value < 0 or value > maximum:
        _fail(f"{label}_integer_invalid")
    return value


def _choice(value: JsonValue, choices: frozenset[str], label: str) -> str:
    if not isinstance(value, str) or value not in choices:
        _fail(f"{label}_choice_invalid")
    return value


def _boolean(value: JsonValue, label: str) -> bool:
    if not isinstance(value, bool):
        _fail(f"{label}_boolean_invalid")
    return value


def _eligible_opcode(opcode: int) -> bool:
    return any(lower <= opcode <= upper for lower, upper in ((0x24, 0x27), (0x2C, 0x2F), (0x34, 0x37), (0x3C, 0x3F), (0x64, 0x67), (0x6C, 0x6F), (0x74, 0x77), (0x7C, 0x7F)))


def _status(phase: str) -> str:
    return _STATUS_BY_PHASE[phase]


def _coverage_counter(mask: int, bit: int) -> int:
    return 1 if mask & (1 << bit) else 0


@dataclass(frozen=True, slots=True)
class Access:
    instruction_pc: int
    address: int
    width: int
    kind: str
    count: int

    def to_json(self) -> JsonObject:
        return {"instruction_pc": self.instruction_pc, "address": self.address, "width": self.width, "kind": self.kind, "count": self.count}

    def identity(self) -> tuple[int, int, int, str]:
        return (self.instruction_pc, self.address, self.width, self.kind)


@dataclass(frozen=True, slots=True)
class FieldCharacterShadow:
    phase: str
    blocker: str
    start_guest_vblank: int
    end_guest_vblank: int
    allocator_effect_count: int
    ot_effect_count: int
    gpu_effect_count: int
    family_count: int
    access_count: int
    site_count: int
    coverage_mask: int
    has_selection: bool
    site: int
    producer_store_pc: int
    parser_word_count: int
    opcode: int
    selection_count: int
    accesses: tuple[Access, ...]

    def to_json(self) -> JsonObject:
        return {
            "schema": FIELD_CHARACTER_SHADOW_SCHEMA, "phase": self.phase, "status": _status(self.phase), "blocker": self.blocker,
            "window": {"start_guest_vblank": self.start_guest_vblank, "end_guest_vblank": self.end_guest_vblank},
            "selection": {"has_selection": self.has_selection, "site": self.site, "producer_store_pc": self.producer_store_pc, "parser_word_count": self.parser_word_count, "opcode": self.opcode, "packet_class": "fixed-textured" if self.has_selection else "none", "count": self.selection_count},
            "coverage": {"coverage_mask": self.coverage_mask, "private_overlap_count": _coverage_counter(self.coverage_mask, 0), "actor_count": _coverage_counter(self.coverage_mask, 1), "camera_count": _coverage_counter(self.coverage_mask, 2), "dialog_count": _coverage_counter(self.coverage_mask, 3)},
            "effects": {"allocator_effect_count": self.allocator_effect_count, "ot_effect_count": self.ot_effect_count, "gpu_effect_count": self.gpu_effect_count},
            "determinism": {"family_count": self.family_count, "access_count": self.access_count, "site_count": self.site_count},
            "accesses": [access.to_json() for access in self.accesses],
            "privacy": {"classification": "metadata-only"}, "cleanup": {"state": "not-applicable"},
        }


def _parse_access(value: JsonValue) -> Access:
    record = _record(value, "access")
    _keys(record, frozenset({"instruction_pc", "address", "width", "kind", "count"}), "access")
    instruction_pc = _integer(record["instruction_pc"], "access_instruction_pc", _U32)
    width = _integer(record["width"], "access_width", 4)
    if instruction_pc & 3 or width not in {1, 2, 4}:
        _fail("access_shape_invalid")
    return Access(instruction_pc, _integer(record["address"], "access_address", _U32), width, _choice(record["kind"], frozenset({"read", "write"}), "access_kind"), _integer(record["count"], "access_count"))


def _parse_selection(value: JsonValue) -> tuple[bool, int, int, int, int, int]:
    record = _record(value, "selection")
    _keys(record, frozenset({"has_selection", "site", "producer_store_pc", "parser_word_count", "opcode", "packet_class", "count"}), "selection")
    has_selection = _boolean(record["has_selection"], "selection_has_selection")
    site = _integer(record["site"], "selection_site", _U32)
    store_pc = _integer(record["producer_store_pc"], "selection_producer_store_pc", _U32)
    words = _integer(record["parser_word_count"], "selection_parser_word_count", _U16)
    opcode = _integer(record["opcode"], "selection_opcode", _U8)
    count = _integer(record["count"], "selection_count")
    packet_class = _choice(record["packet_class"], frozenset({"none", "fixed-textured"}), "selection_packet_class")
    if has_selection:
        if site == 0 or store_pc & 3 or words == 0 or not _eligible_opcode(opcode) or count < MINIMUM_FAMILY_COUNT or packet_class != "fixed-textured":
            _fail("selection_eligible_shape_invalid")
    elif (site, store_pc, words, opcode, count, packet_class) != (0, 0, 0, 0, 0, "none"):
        _fail("selection_empty_shape_invalid")
    return has_selection, site, store_pc, words, opcode, count


def parse_field_character_shadow(value: JsonValue) -> FieldCharacterShadow:
    record = _record(value, "field_character_shadow")
    _keys(record, _ROOT_KEYS, "field_character_shadow")
    if record["schema"] != FIELD_CHARACTER_SHADOW_SCHEMA:
        _fail("field_character_shadow_schema_invalid")
    phase = _choice(record["phase"], _PHASES, "phase")
    blocker = _choice(record["blocker"], _BLOCKERS, "blocker")
    if _choice(record["status"], frozenset({"pending", "pass", "blocked"}), "status") != _status(phase):
        _fail("status_derivation_invalid")
    window = _record(record["window"], "window")
    _keys(window, frozenset({"start_guest_vblank", "end_guest_vblank"}), "window")
    start = _integer(window["start_guest_vblank"], "window_start")
    end = _integer(window["end_guest_vblank"], "window_end")
    has_selection, site, store_pc, words, opcode, selection_count = _parse_selection(record["selection"])
    coverage = _record(record["coverage"], "coverage")
    _keys(coverage, frozenset({"coverage_mask", "private_overlap_count", "actor_count", "camera_count", "dialog_count"}), "coverage")
    coverage_mask = _integer(coverage["coverage_mask"], "coverage_mask", 15)
    if tuple(_integer(coverage[name], f"coverage_{name}", 1) for name in ("private_overlap_count", "actor_count", "camera_count", "dialog_count")) != tuple(_coverage_counter(coverage_mask, bit) for bit in range(4)):
        _fail("coverage_derivation_invalid")
    effects = _record(record["effects"], "effects")
    _keys(effects, frozenset({"allocator_effect_count", "ot_effect_count", "gpu_effect_count"}), "effects")
    determinism = _record(record["determinism"], "determinism")
    _keys(determinism, frozenset({"family_count", "access_count", "site_count"}), "determinism")
    family_count = _integer(determinism["family_count"], "family_count", MAX_FAMILIES)
    access_count = _integer(determinism["access_count"], "access_count", MAX_ACCESSES)
    site_count = _integer(determinism["site_count"], "site_count", MAX_SITES)
    accesses_value = record["accesses"]
    if not _is_array(accesses_value) or len(accesses_value) != access_count:
        _fail("accesses_count_invalid")
    accesses = tuple(_parse_access(access) for access in accesses_value)
    if len({access.identity() for access in accesses}) != len(accesses):
        _fail("accesses_duplicate_invalid")
    privacy = _record(record["privacy"], "privacy")
    _keys(privacy, frozenset({"classification"}), "privacy")
    if privacy["classification"] != "metadata-only":
        _fail("privacy_derivation_invalid")
    cleanup = _record(record["cleanup"], "cleanup")
    _keys(cleanup, frozenset({"state"}), "cleanup")
    if cleanup["state"] != "not-applicable":
        _fail("cleanup_derivation_invalid")
    match phase:
        case "complete":
            if blocker != "none" or not has_selection or end - start != SELECTION_VBLANKS or coverage_mask != 15:
                _fail("complete_summary_invalid")
        case "blocked":
            if blocker == "none" or has_selection or end != 0:
                _fail("blocked_summary_invalid")
        case "idle" | "collecting":
            if blocker != "none" or has_selection or end != 0:
                _fail("incomplete_summary_invalid")
        case _:
            _fail("phase_choice_invalid")
    return FieldCharacterShadow(phase, blocker, start, end, _integer(effects["allocator_effect_count"], "allocator_effect_count"), _integer(effects["ot_effect_count"], "ot_effect_count"), _integer(effects["gpu_effect_count"], "gpu_effect_count"), family_count, access_count, site_count, coverage_mask, has_selection, site, store_pc, words, opcode, selection_count, accesses)


def serialize_field_character_shadow(summary: FieldCharacterShadow) -> bytes:
    return canonical_ascii_json(summary.to_json())


def canonical_field_character_shadow(value: JsonValue) -> bytes:
    return serialize_field_character_shadow(parse_field_character_shadow(value))
