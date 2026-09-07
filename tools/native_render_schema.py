from __future__ import annotations

import re
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Final, NoReturn, TypeAlias, TypeIs

JsonScalar: TypeAlias = str | int | float | bool | None
JsonValue: TypeAlias = JsonScalar | list["JsonValue"] | Mapping[str, "JsonValue"]
JsonObject: TypeAlias = Mapping[str, JsonValue]

AGGREGATE_SCHEMA: Final = "xenogears.native-render-aggregate/v1"
MAX_METADATA_BYTES: Final = 1_000_000
MAX_OT_ITEMS: Final = 4096
_HEX_64: Final = re.compile(r"[0-9a-f]{64}\Z")
_U32: Final = 0xFFFFFFFF
_U64: Final = 0xFFFFFFFFFFFFFFFF


class ContractError(ValueError):
    def __init__(self, code: str) -> None:
        self.code: str = code
        super().__init__(code)


def _fail(code: str) -> NoReturn:
    raise ContractError(code)


def _record(value: JsonValue, name: str) -> JsonObject:
    if not _is_record(value):
        _fail(f"{name}_object_required")
    return value


def _keys(record: JsonObject, fields: frozenset[str], name: str) -> None:
    if set(record) != set(fields):
        _fail(f"{name}_keys_invalid")


def _integer(value: JsonValue, name: str, maximum: int = _U64) -> int:
    if not _is_integer(value) or value < 0 or value > maximum:
        _fail(f"{name}_integer_invalid")
    return value


def _positive_u32(value: JsonValue, name: str) -> int:
    parsed = _integer(value, name, _U32)
    if parsed == 0:
        _fail(f"{name}_integer_invalid")
    return parsed


def _boolean(value: JsonValue, name: str) -> bool:
    if not _is_boolean(value):
        _fail(f"{name}_boolean_invalid")
    return value


def _choice(value: JsonValue, values: frozenset[str], name: str) -> str:
    if not _is_string(value) or value not in values:
        _fail(f"{name}_choice_invalid")
    return value


def _digest(value: JsonValue, name: str) -> str:
    if not _is_string(value) or _HEX_64.fullmatch(value) is None:
        _fail(f"{name}_digest_invalid")
    return value


def _is_record(value: JsonValue) -> TypeIs[JsonObject]:
    return isinstance(value, Mapping)


def _is_array(value: JsonValue) -> TypeIs[list[JsonValue]]:
    return isinstance(value, list)


def _is_integer(value: JsonValue) -> TypeIs[int]:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_boolean(value: JsonValue) -> TypeIs[bool]:
    return isinstance(value, bool)


def _is_string(value: JsonValue) -> TypeIs[str]:
    return isinstance(value, str)


@dataclass(frozen=True, slots=True)
class VisualId:
    scene_epoch: int
    state_sequence: int

    def to_json(self) -> JsonObject:
        return {"scene_epoch": self.scene_epoch, "state_sequence": self.state_sequence}


@dataclass(frozen=True, slots=True)
class Provenance:
    authenticated: bool
    tier: str
    producer_record_id: int
    site_record_id: int
    producer_entry: int
    capture_site: int
    return_site: int
    static_callee: int

    def to_json(self) -> JsonObject:
        return {
            "authenticated": self.authenticated, "tier": self.tier,
            "producer_record_id": self.producer_record_id, "site_record_id": self.site_record_id,
            "producer_entry": self.producer_entry, "capture_site": self.capture_site,
            "return_site": self.return_site, "static_callee": self.static_callee,
        }


@dataclass(frozen=True, slots=True)
class Material:
    textured: bool
    semi_transparent: bool
    blend_mode: str
    texture_depth: str
    tpage: int
    clut_x: int
    clut_y: int
    dither: bool
    mask_set: bool
    mask_check: bool

    def to_json(self) -> JsonObject:
        return {
            "textured": self.textured, "semi_transparent": self.semi_transparent,
            "blend_mode": self.blend_mode, "texture_depth": self.texture_depth,
            "tpage": self.tpage, "clut_x": self.clut_x, "clut_y": self.clut_y,
            "dither": self.dither, "mask_set": self.mask_set, "mask_check": self.mask_check,
        }


@dataclass(frozen=True, slots=True)
class OtItem:
    bucket: int
    ordinal: int
    kind: str
    packet_address: int
    predecessor_address: int
    successor_address: int
    tag_link_address: int
    opcode: int
    length_words: int
    material: Material

    def to_json(self) -> JsonObject:
        return {
            "bucket": self.bucket, "ordinal": self.ordinal, "kind": self.kind,
            "packet_address": self.packet_address, "predecessor_address": self.predecessor_address,
            "successor_address": self.successor_address, "tag_link_address": self.tag_link_address,
            "opcode": self.opcode, "length_words": self.length_words,
            "material": self.material.to_json(),
        }


@dataclass(frozen=True, slots=True)
class VramSummary:
    start_serial: int
    end_serial: int
    mutation_count: int
    mutation_digest: str
    complete: bool

    def to_json(self) -> JsonObject:
        return {
            "start_serial": self.start_serial, "end_serial": self.end_serial,
            "mutation_count": self.mutation_count, "mutation_digest": self.mutation_digest,
            "complete": self.complete,
        }


@dataclass(frozen=True, slots=True)
class CameraActorDigest:
    camera_digest: str
    actor_digest: str
    actor_count: int

    def to_json(self) -> JsonObject:
        return {
            "camera_digest": self.camera_digest, "actor_digest": self.actor_digest,
            "actor_count": self.actor_count,
        }


@dataclass(frozen=True, slots=True)
class Display15Bit:
    digest: str
    pixel_count: int

    def to_json(self) -> JsonObject:
        return {"digest": self.digest, "pixel_count": self.pixel_count}


@dataclass(frozen=True, slots=True)
class Fallback:
    required: bool
    used: bool
    reason: str

    def to_json(self) -> JsonObject:
        return {"required": self.required, "used": self.used, "reason": self.reason}


@dataclass(frozen=True, slots=True)
class RenderAggregate:
    visual_id: VisualId
    provenance: Provenance
    ot_items: tuple[OtItem, ...]
    vram: VramSummary
    camera_actor: CameraActorDigest
    display_15bit: Display15Bit
    fallback: Fallback

    def to_json(self) -> JsonObject:
        return {
            "schema": AGGREGATE_SCHEMA, "visual_id": self.visual_id.to_json(),
            "provenance": self.provenance.to_json(),
            "ot_items": [item.to_json() for item in self.ot_items], "vram": self.vram.to_json(),
            "camera_actor": self.camera_actor.to_json(), "display_15bit": self.display_15bit.to_json(),
            "fallback": self.fallback.to_json(),
        }


def _parse_material(value: JsonValue) -> Material:
    record = _record(value, "material")
    _keys(record, frozenset({"textured", "semi_transparent", "blend_mode", "texture_depth", "tpage", "clut_x", "clut_y", "dither", "mask_set", "mask_check"}), "material")
    return Material(_boolean(record["textured"], "material_textured"), _boolean(record["semi_transparent"], "material_semi_transparent"), _choice(record["blend_mode"], frozenset({"average", "add", "subtract", "add_quarter"}), "material_blend_mode"), _choice(record["texture_depth"], frozenset({"4bit", "8bit", "15bit"}), "material_texture_depth"), _integer(record["tpage"], "material_tpage", 0xFFFF), _integer(record["clut_x"], "material_clut_x", 0xFFFF), _integer(record["clut_y"], "material_clut_y", 0xFFFF), _boolean(record["dither"], "material_dither"), _boolean(record["mask_set"], "material_mask_set"), _boolean(record["mask_check"], "material_mask_check"))


def _parse_item(value: JsonValue) -> OtItem:
    record = _record(value, "ot_item")
    _keys(record, frozenset({"bucket", "ordinal", "kind", "packet_address", "predecessor_address", "successor_address", "tag_link_address", "opcode", "length_words", "material"}), "ot_item")
    return OtItem(_integer(record["bucket"], "ot_bucket", _U32), _integer(record["ordinal"], "ot_ordinal", _U32), _choice(record["kind"], frozenset({"compatibility", "native"}), "ot_kind"), _integer(record["packet_address"], "packet_address", _U32), _integer(record["predecessor_address"], "predecessor_address", _U32), _integer(record["successor_address"], "successor_address", _U32), _integer(record["tag_link_address"], "tag_link_address", _U32), _integer(record["opcode"], "opcode", 0xFF), _integer(record["length_words"], "length_words", 255), _parse_material(record["material"]))


def parse_aggregate(value: JsonValue) -> RenderAggregate:
    record = _record(value, "aggregate")
    _keys(record, frozenset({"schema", "visual_id", "provenance", "ot_items", "vram", "camera_actor", "display_15bit", "fallback"}), "aggregate")
    if record["schema"] != AGGREGATE_SCHEMA:
        _fail("aggregate_schema_invalid")
    visual = _record(record["visual_id"], "visual_id")
    _keys(visual, frozenset({"scene_epoch", "state_sequence"}), "visual_id")
    visual_id = VisualId(_integer(visual["scene_epoch"], "scene_epoch"), _integer(visual["state_sequence"], "state_sequence"))
    if visual_id.scene_epoch == 0:
        _fail("scene_epoch_integer_invalid")
    provenance = _record(record["provenance"], "provenance")
    _keys(provenance, frozenset({"authenticated", "tier", "producer_record_id", "site_record_id", "producer_entry", "capture_site", "return_site", "static_callee"}), "provenance")
    parsed_provenance = Provenance(_boolean(provenance["authenticated"], "authenticated"), _choice(provenance["tier"], frozenset({"static", "cold", "warm"}), "tier"), _positive_u32(provenance["producer_record_id"], "producer_record_id"), _positive_u32(provenance["site_record_id"], "site_record_id"), _positive_u32(provenance["producer_entry"], "producer_entry"), _positive_u32(provenance["capture_site"], "capture_site"), _positive_u32(provenance["return_site"], "return_site"), _positive_u32(provenance["static_callee"], "static_callee"))
    if not parsed_provenance.authenticated:
        _fail("authenticated_boolean_invalid")
    raw_items = record["ot_items"]
    if not _is_array(raw_items) or not 1 <= len(raw_items) <= MAX_OT_ITEMS:
        _fail("ot_items_bounds_invalid")
    items = tuple(_parse_item(item) for item in raw_items)
    if any(item.ordinal != index for index, item in enumerate(items)):
        _fail("ot_items_order_invalid")
    vram = _record(record["vram"], "vram")
    _keys(vram, frozenset({"start_serial", "end_serial", "mutation_count", "mutation_digest", "complete"}), "vram")
    parsed_vram = VramSummary(_integer(vram["start_serial"], "start_serial"), _integer(vram["end_serial"], "end_serial"), _integer(vram["mutation_count"], "mutation_count", _U32), _digest(vram["mutation_digest"], "mutation"), _boolean(vram["complete"], "vram_complete"))
    if parsed_vram.end_serial < parsed_vram.start_serial:
        _fail("vram_serial_order_invalid")
    camera_actor = _record(record["camera_actor"], "camera_actor")
    _keys(camera_actor, frozenset({"camera_digest", "actor_digest", "actor_count"}), "camera_actor")
    parsed_camera_actor = CameraActorDigest(_digest(camera_actor["camera_digest"], "camera"), _digest(camera_actor["actor_digest"], "actor"), _integer(camera_actor["actor_count"], "actor_count", 256))
    display = _record(record["display_15bit"], "display_15bit")
    _keys(display, frozenset({"digest", "pixel_count"}), "display_15bit")
    parsed_display = Display15Bit(_digest(display["digest"], "display"), _integer(display["pixel_count"], "pixel_count", 1_048_576))
    fallback = _record(record["fallback"], "fallback")
    _keys(fallback, frozenset({"required", "used", "reason"}), "fallback")
    parsed_fallback = Fallback(_boolean(fallback["required"], "fallback_required"), _boolean(fallback["used"], "fallback_used"), _choice(fallback["reason"], frozenset({"none", "authentication_failed", "oracle_incomplete", "stale_vram", "compare_mismatch", "unsupported_display", "internal_error"}), "fallback_reason"))
    if parsed_fallback.used and not parsed_fallback.required:
        _fail("fallback_usage_invalid")
    if (parsed_fallback.required or parsed_fallback.used) != (parsed_fallback.reason != "none"):
        _fail("fallback_reason_invalid")
    return RenderAggregate(visual_id, parsed_provenance, items, parsed_vram, parsed_camera_actor, parsed_display, parsed_fallback)
