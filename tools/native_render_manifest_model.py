from __future__ import annotations

from dataclasses import dataclass
import re
from pathlib import Path
import tomllib
from typing import assert_never, Final, NewType, TypeAlias


SCHEMA: Final = "xg-render-manifest/v3"
GAME_ID: Final = "main-disc1-exe"
FIELD_ID: Final = "field-image"
PRODUCER_ID: Final = "render-field-character-sprites"
SITE_ID: Final = "field-vsync-call-0x800781bc"
TOKEN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9-]*$")
GENERIC_OVERLAY_TOKEN = re.compile(r"^[A-Za-z0-9]+(?:[-_][A-Za-z0-9]+)*$")
FILE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
Digest32 = NewType("Digest32", bytes)
ManifestValue: TypeAlias = str | int | float | bool | list["ManifestValue"] | dict[str, "ManifestValue"]


@dataclass(frozen=True, slots=True)
class ManifestError(Exception):
    message: str

    def __str__(self) -> str:
        return self.message


@dataclass(frozen=True, slots=True)
class FileIdentity:
    sha256: Digest32
    crc32: int
    size: int


@dataclass(frozen=True, slots=True)
class GameSpec:
    identity: FileIdentity
    namespace_crc32: int
    base_address: int
    image_format: str
    header_size: int
    loaded_size: int


@dataclass(frozen=True, slots=True)
class AuthenticatedOverlay:
    file: str
    identity: FileIdentity
    base_address: int
    range_offset: int
    range_size: int
    range_crc32: int
    image_format: str
    header_size: int
    loaded_size: int


@dataclass(frozen=True, slots=True)
class BlockedOverlay:
    base_address: int
    reason_code: str


@dataclass(frozen=True, slots=True)
class SourceProvenance:
    disc: int
    directory: int
    file: int
    sector: int
    stored_size: int


@dataclass(frozen=True, slots=True)
class AuthenticatedArtifactOverlay:
    key: str
    identifier: str
    file: str
    identity: FileIdentity
    base_address: int
    image_format: str
    header_size: int
    loaded_size: int
    source: SourceProvenance | None


@dataclass(frozen=True, slots=True)
class FunctionSpec:
    identifier: str
    name: str
    entry_address: int


@dataclass(frozen=True, slots=True)
class ProducerSpec:
    entry_address: int
    framebuffer_context: str
    ot_context: str


@dataclass(frozen=True, slots=True)
class AuthenticatedSite:
    call_address: int
    return_address: int
    window_start: int
    window_size: int
    window_sha256: Digest32
    framebuffer_context: str
    ot_context: str


@dataclass(frozen=True, slots=True)
class BlockedSite:
    call_address: int
    return_address: int
    reason_code: str
    framebuffer_context: str
    ot_context: str


@dataclass(frozen=True, slots=True)
class ManifestContract:
    game: GameSpec
    field: AuthenticatedOverlay | BlockedOverlay
    overlays: tuple[AuthenticatedArtifactOverlay, ...]
    functions: tuple[FunctionSpec, ...]
    producer: ProducerSpec
    site: AuthenticatedSite | BlockedSite

    def blockers(self) -> tuple[str, ...]:
        blocked: list[str] = []
        match self.field:
            case BlockedOverlay():
                blocked.extend((FIELD_ID, PRODUCER_ID, SITE_ID))
            case AuthenticatedOverlay():
                pass
            case unreachable:
                assert_never(unreachable)
        match self.site:
            case BlockedSite():
                blocked.append(SITE_ID)
            case AuthenticatedSite():
                pass
            case unreachable:
                assert_never(unreachable)
        return tuple(sorted(set(blocked)))


def fail(message: str) -> None:
    raise ManifestError(message)


def closed(value: ManifestValue, keys: set[str], label: str) -> dict[str, ManifestValue]:
    if not isinstance(value, dict) or set(value) != keys:
        fail(f"schema {label} fields are not closed")
    return value


def text(value: ManifestValue, label: str) -> str:
    if not isinstance(value, str) or TOKEN.fullmatch(value) is None:
        fail(f"schema {label} must be a safe metadata token")
    return value


def overlay_token(value: ManifestValue, label: str) -> str:
    if not isinstance(value, str) or GENERIC_OVERLAY_TOKEN.fullmatch(value) is None:
        fail(f"schema {label} must be a safe generic overlay token")
    return value


def integer(value: ManifestValue, label: str, positive: bool = False) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < int(positive):
        fail(f"schema {label} must be a {'positive' if positive else 'non-negative'} integer")
    return value


def hex_value(value: ManifestValue, label: str, digits: int) -> int:
    if not isinstance(value, str) or len(value) != digits or re.fullmatch(r"[0-9a-f]+", value) is None:
        fail(f"schema {label} must be {digits} lowercase hexadecimal characters")
    return int(value, 16)


def digest(value: ManifestValue, label: str) -> Digest32:
    return Digest32(hex_value(value, label, 64).to_bytes(32, "big"))


def address(value: ManifestValue, label: str) -> int:
    if not isinstance(value, str) or re.fullmatch(r"0x[0-9a-f]{8}", value) is None:
        fail(f"schema {label} must be a lowercase 32-bit hexadecimal address")
    return int(value, 16)


def parse_game(raw: ManifestValue) -> GameSpec:
    keys = {
        "id", "serial", "disc", "sha256", "crc32", "size",
        "namespace_crc32", "base_address", "image_format", "header_size",
        "loaded_size",
    }
    if isinstance(raw, dict) and "sha256" not in raw:
        fail("missing identity: game.sha256 is required")
    value = closed(raw, keys, "game")
    if value["id"] != GAME_ID or value["serial"] != "SLUS-00664" or value["disc"] != 1:
        fail("disc identity only permits main-disc1-exe SLUS-00664 Disc 1")
    identity = FileIdentity(digest(value["sha256"], "game.sha256"), hex_value(value["crc32"], "game.crc32", 8), integer(value["size"], "game.size", True))
    image_format = text(value["image_format"], "game.image_format")
    header_size = integer(value["header_size"], "game.header_size")
    loaded_size = integer(value["loaded_size"], "game.loaded_size", True)
    if image_format != "ps-x-exe" or header_size != 0x800:
        fail("game image mapping must describe a PS-X EXE header")
    if header_size + loaded_size > identity.size:
        fail("game loaded payload exceeds full image size")
    return GameSpec(identity, hex_value(value["namespace_crc32"], "game.namespace_crc32", 8), address(value["base_address"], "game.base_address"), image_format, header_size, loaded_size)


def parse_field_overlay(raw: ManifestValue) -> AuthenticatedOverlay | BlockedOverlay:
    if not isinstance(raw, dict):
        fail("schema overlay.field must be a table")
    value = raw
    state = value.get("state")
    if state == "blocked":
        blocked = closed(value, {"id", "state", "base_address", "reason_code"}, "overlay.field")
        if blocked["id"] != FIELD_ID:
            fail("required field-image record is missing")
        base = address(blocked["base_address"], "overlay.field.base_address")
        if base != 0x8006F000:
            fail("base mismatch: field-image must load at 0x8006f000")
        return BlockedOverlay(base, text(blocked["reason_code"], "overlay.field.reason_code"))
    if state != "authenticated":
        fail("overlay.field state must be authenticated or blocked")
    authenticated = closed(value, {
        "id", "state", "file", "full_sha256", "full_crc32", "full_size",
        "base_address", "image_format", "header_size", "loaded_size",
        "range_offset", "range_size", "range_crc32",
    }, "overlay.field")
    if authenticated["id"] != FIELD_ID or state != "authenticated":
        fail("required field-image record is missing")
    file_name = authenticated["file"]
    if not isinstance(file_name, str) or Path(file_name).name != file_name:
        fail("schema overlay.field.file must be a basename")
    full_size = integer(authenticated["full_size"], "overlay.field.full_size", True)
    range_offset = integer(authenticated["range_offset"], "overlay.field.range_offset")
    range_size = integer(authenticated["range_size"], "overlay.field.range_size", True)
    image_format = text(authenticated["image_format"], "overlay.field.image_format")
    header_size = integer(authenticated["header_size"], "overlay.field.header_size")
    loaded_size = integer(authenticated["loaded_size"], "overlay.field.loaded_size", True)
    if image_format != "ps-x-exe" or header_size != 0x800:
        fail("field image mapping must describe a PS-X EXE header")
    if header_size + loaded_size > full_size:
        fail("field loaded payload exceeds full image size")
    if range_offset + range_size > loaded_size:
        fail("schema overlay.field range exceeds loaded payload")
    identity = FileIdentity(digest(authenticated["full_sha256"], "overlay.field.full_sha256"), hex_value(authenticated["full_crc32"], "overlay.field.full_crc32", 8), full_size)
    base = address(authenticated["base_address"], "overlay.field.base_address")
    if base != 0x8006F000:
        fail("base mismatch: field-image must load at 0x8006f000")
    return AuthenticatedOverlay(file_name, identity, base, range_offset, range_size, hex_value(authenticated["range_crc32"], "overlay.field.range_crc32", 8), image_format, header_size, loaded_size)


def parse_artifact_overlay(key: str, raw: ManifestValue) -> AuthenticatedArtifactOverlay:
    label = f"overlay.{key}"
    required = {
        "id", "state", "file", "full_sha256", "full_crc32", "full_size",
        "base_address", "image_format", "header_size", "loaded_size",
    }
    provenance = {
        "source_disc", "source_directory", "source_file", "source_sector",
    }
    provenance_shapes = (
        required | provenance | {"compressed_size"},
        required | provenance | {"archive_size"},
    )
    if not isinstance(raw, dict) or set(raw) not in (required, *provenance_shapes):
        fail(f"schema {label} fields are not closed")
    value = raw
    identifier = overlay_token(value["id"], f"{label}.id")
    if value["state"] != "authenticated":
        fail(f"optional overlay {identifier} must be authenticated")
    file_name = value["file"]
    if not isinstance(file_name, str) or FILE_NAME.fullmatch(file_name) is None:
        fail(f"schema {label}.file must be a safe basename")
    full_size = integer(value["full_size"], f"{label}.full_size", True)
    image_format = text(value["image_format"], f"{label}.image_format")
    header_size = integer(value["header_size"], f"{label}.header_size")
    loaded_size = integer(value["loaded_size"], f"{label}.loaded_size", True)
    if image_format == "raw":
        if header_size != 0 or loaded_size != full_size:
            fail(f"{label} raw mapping must cover a headerless full image")
    elif image_format == "ps-x-exe":
        if header_size != 0x800 or header_size + loaded_size > full_size:
            fail(f"{label} mapping must describe a PS-X EXE payload")
    else:
        fail(f"{label} image format is unsupported")
    base = address(value["base_address"], f"{label}.base_address")
    if base + loaded_size > 0x100000000:
        fail(f"{label} loaded range exceeds the 32-bit address space")
    source = None
    if provenance <= set(value):
        size_field = "archive_size" if "archive_size" in value else "compressed_size"
        source = SourceProvenance(
            integer(value["source_disc"], f"{label}.source_disc", True),
            integer(value["source_directory"], f"{label}.source_directory"),
            integer(value["source_file"], f"{label}.source_file"),
            integer(value["source_sector"], f"{label}.source_sector", True),
            integer(value[size_field], f"{label}.{size_field}", True),
        )
        if source.disc != 1:
            fail(f"{label} source provenance must identify Disc 1")
        if source.stored_size > full_size:
            fail(f"{label} source stored size exceeds the image size")
    identity = FileIdentity(
        digest(value["full_sha256"], f"{label}.full_sha256"),
        hex_value(value["full_crc32"], f"{label}.full_crc32", 8),
        full_size,
    )
    return AuthenticatedArtifactOverlay(
        key, identifier, file_name, identity, base, image_format, header_size,
        loaded_size, source)


def parse_overlay(raw: ManifestValue) -> tuple[
        AuthenticatedOverlay | BlockedOverlay,
        tuple[AuthenticatedArtifactOverlay, ...]]:
    if not isinstance(raw, dict) or "field" not in raw:
        fail("required overlay.field table is missing")
    for key in raw:
        overlay_token(key, "overlay key")
    field = parse_field_overlay(raw["field"])
    overlays = tuple(
        parse_artifact_overlay(key, value)
        for key, value in raw.items()
        if key != "field"
    )
    fixed_ids = {GAME_ID, FIELD_ID, PRODUCER_ID, SITE_ID, "draw-otag", "vsync"}
    identifiers = [overlay.identifier for overlay in overlays]
    if len(set(identifiers)) != len(identifiers) or fixed_ids.intersection(identifiers):
        fail("duplicate manifest record id")
    return field, overlays


def parse_functions(raw: ManifestValue) -> tuple[FunctionSpec, ...]:
    if not isinstance(raw, list):
        fail("schema functions must be an array")
    functions: list[FunctionSpec] = []
    for item in raw:
        value = closed(item, {"id", "image", "name", "entry_address", "confidence"}, "function")
        if value["image"] != GAME_ID or value["confidence"] != "verified":
            fail("function authentication is invalid")
        functions.append(FunctionSpec(text(value["id"], "function.id"), text(value["name"], "function.name"), address(value["entry_address"], "function.entry_address")))
    expected = {"draw-otag": ("DrawOTag", 0x80044BD0), "vsync": ("VSync", 0x8004B54C)}
    actual = {item.identifier: (item.name, item.entry_address) for item in functions}
    if actual != expected:
        fail("required VSync and DrawOTag records are missing")
    return tuple(sorted(functions, key=lambda item: item.identifier))


def parse_producer(raw: ManifestValue) -> ProducerSpec:
    if not isinstance(raw, list) or len(raw) != 1:
        fail("required RenderFieldCharacterSprites record is missing")
    value = closed(raw[0], {"id", "image", "name", "entry_address", "framebuffer_context", "ot_context", "confidence"}, "producer")
    if value["id"] != PRODUCER_ID or value["image"] != FIELD_ID or value["name"] != "RenderFieldCharacterSprites" or value["confidence"] != "verified":
        fail("required RenderFieldCharacterSprites record is missing")
    entry = address(value["entry_address"], "producer.entry_address")
    if entry != 0x80075B44:
        fail("required RenderFieldCharacterSprites address is 0x80075b44")
    return ProducerSpec(entry, text(value["framebuffer_context"], "producer.framebuffer_context"), text(value["ot_context"], "producer.ot_context"))


def parse_site(raw: ManifestValue) -> AuthenticatedSite | BlockedSite:
    if not isinstance(raw, list) or len(raw) != 1 or not isinstance(raw[0], dict):
        fail("required 0x800781bc site is missing")
    value = raw[0]
    common = {"id", "state", "producer", "image", "callee", "call_address", "return_address", "framebuffer_context", "ot_context", "confidence"}
    state = value.get("state")
    if state not in {"blocked", "authenticated"}:
        fail("site state must be authenticated or blocked")
    keys = common | ({"reason_code"} if state == "blocked" else {"window_start", "window_size", "window_sha256"})
    site = closed(value, keys, "site")
    if site["id"] != SITE_ID or site["producer"] != PRODUCER_ID or site["image"] != FIELD_ID or site["confidence"] != "verified":
        fail("required 0x800781bc site is missing")
    if site["callee"] != "vsync":
        fail("caller mismatch: 0x800781bc must call VSync")
    call = address(site["call_address"], "site.call_address")
    returned = address(site["return_address"], "site.return_address")
    if call != 0x800781BC:
        fail("required 0x800781bc site does not match its declared call address")
    if returned != call + 8:
        fail("caller mismatch: return address must follow the delay slot")
    framebuffer = text(site["framebuffer_context"], "site.framebuffer_context")
    ot_context = text(site["ot_context"], "site.ot_context")
    if state == "blocked":
        return BlockedSite(call, returned, text(site["reason_code"], "site.reason_code"), framebuffer, ot_context)
    return AuthenticatedSite(call, returned, address(site["window_start"], "site.window_start"), integer(site["window_size"], "site.window_size", True), digest(site["window_sha256"], "site.window_sha256"), framebuffer, ot_context)


def load_contract(path: Path) -> ManifestContract:
    try:
        with path.open("rb") as source:
            raw = tomllib.load(source)
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise ManifestError("manifest is unreadable") from error
    root = closed(raw, {"schema", "game", "overlay", "functions", "producers", "sites"}, "root")
    if root["schema"] != SCHEMA:
        fail("schema version is unsupported")
    field, overlays = parse_overlay(root["overlay"])
    return ManifestContract(parse_game(root["game"]), field, overlays, parse_functions(root["functions"]), parse_producer(root["producers"]), parse_site(root["sites"]))
