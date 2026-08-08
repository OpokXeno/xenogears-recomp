from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path
from typing import assert_never, Final
import zlib

from native_render_manifest_model import (
    AuthenticatedOverlay,
    AuthenticatedSite,
    BlockedOverlay,
    BlockedSite,
    Digest32,
    FIELD_ID,
    FileIdentity,
    GAME_ID,
    ManifestContract,
    ManifestError,
    PRODUCER_ID,
    SITE_ID,
    fail,
)


@dataclass(frozen=True, slots=True)
class MetadataRecord:
    record_id: int
    identifier: str
    kind: int
    address: int
    target_address: int
    image_identity: Digest32
    window_identity: Digest32
    framebuffer_context: str
    ot_context: str


@dataclass(frozen=True, slots=True)
class ManifestValidationMetadata:
    producer_record_id: int
    site_record_id: int
    field_base_crc32: int
    field_range_crc32: int
    field_range_start: int
    field_range_size: int
    producer_entry: int
    caller_site: int
    static_callee: int
    return_site: int
    instruction_window_start: int
    instruction_window_size: int
    instruction_window_identity: Digest32
    required_jal_opcode: int
    jal_target: int
    required_delay_slot_instructions: int
    required_delay_slot_non_control_transfer: int


@dataclass(frozen=True, slots=True)
class VerifiedManifest:
    game_identity: Digest32
    manifest_identity: Digest32
    namespace_crc32: int
    records: tuple[MetadataRecord, ...]
    validation: ManifestValidationMetadata


@dataclass(frozen=True, slots=True)
class VerificationInputs:
    manifest: Path
    exe: Path
    overlays: Path


RECORD_IDS: Final = {
    GAME_ID: 1,
    FIELD_ID: 2,
    PRODUCER_ID: 3,
    SITE_ID: 4,
    "draw-otag": 5,
    "vsync": 6,
}


def file_identity(path: Path) -> FileIdentity:
    digest = hashlib.sha256()
    checksum = 0
    size = 0
    try:
        with path.open("rb") as source:
            while chunk := source.read(65536):
                digest.update(chunk)
                checksum = zlib.crc32(chunk, checksum)
                size += len(chunk)
    except OSError as error:
        raise ManifestError("identity could not read local input") from error
    return FileIdentity(Digest32(digest.digest()), checksum & 0xFFFFFFFF, size)


def bounded_file_bytes(path: Path, offset: int, size: int) -> bytes:
    if offset < 0 or size <= 0:
        fail("file range is invalid")
    try:
        with path.open("rb") as source:
            source.seek(offset)
            data = source.read(size)
    except OSError as error:
        raise ManifestError("site could not read local input") from error
    if len(data) != size:
        fail("file range exceeds local input")
    return data


def bounded_bytes(path: Path, offset: int, size: int) -> bytes:
    if offset < 0 or size < 8:
        fail("delay-slot window range is invalid")
    data = bounded_file_bytes(path, offset, size)
    return data


def is_control_transfer(word: int) -> bool:
    opcode = word >> 26
    if opcode in {1, 2, 3, 4, 5, 6, 7}:
        return True
    return opcode == 0 and (word & 0x3F) in {8, 9}


def verify_psx_exe_mapping(path: Path, header_size: int, base_address: int,
                           loaded_size: int, label: str) -> None:
    header = bounded_file_bytes(path, 0, header_size)
    if header[:8] != b"PS-X EXE":
        fail(f"{label} is not a PS-X EXE container")
    declared_base = int.from_bytes(header[0x18:0x1c], "little")
    declared_size = int.from_bytes(header[0x1c:0x20], "little")
    if declared_base != base_address or declared_size != loaded_size:
        fail(f"{label} PS-X EXE payload mapping mismatch")


def verify(contract: ManifestContract, inputs: VerificationInputs) -> VerifiedManifest:
    blockers = contract.blockers()
    if blockers:
        fail(f"contract blocked by unavailable authentication: {','.join(blockers)}")
    match contract.field:
        case AuthenticatedOverlay() as field:
            pass
        case BlockedOverlay():
            fail("field image authentication is unavailable")
        case unreachable:
            assert_never(unreachable)
    match contract.site:
        case AuthenticatedSite() as site:
            pass
        case BlockedSite():
            fail("site authentication is unavailable")
        case unreachable:
            assert_never(unreachable)
    game_actual = file_identity(inputs.exe)
    if game_actual != contract.game.identity:
        fail("full game identity mismatch")
    verify_psx_exe_mapping(inputs.exe, contract.game.header_size,
                           contract.game.base_address,
                           contract.game.loaded_size, "game image")
    overlay_path = inputs.overlays / field.file
    overlay_actual = file_identity(overlay_path)
    if overlay_actual != field.identity:
        fail("field image full identity mismatch")
    verify_psx_exe_mapping(overlay_path, field.header_size, field.base_address,
                           field.loaded_size, "field image")
    range_file_offset = field.header_size + field.range_offset
    range_data = bounded_bytes(overlay_path, range_file_offset, field.range_size)
    if zlib.crc32(range_data) & 0xFFFFFFFF != field.range_crc32:
        fail("crc mismatch for authenticated field range")
    field_start = field.base_address + field.range_offset
    field_end = field_start + field.range_size
    if not field_start <= contract.producer.entry_address < field_end:
        fail("base mismatch for RenderFieldCharacterSprites")
    for function in contract.functions:
        if not contract.game.base_address <= function.entry_address < contract.game.base_address + contract.game.loaded_size:
            fail("base mismatch for main executable function")
    if not field_start <= site.window_start <= site.call_address or site.call_address + 8 > site.window_start + site.window_size or site.window_start + site.window_size > field_end:
        fail("base mismatch for 0x800781bc site")
    window = bounded_bytes(overlay_path,
                           field.header_size + site.window_start - field.base_address,
                           site.window_size)
    if hashlib.sha256(window).digest() != site.window_sha256:
        fail("delay-slot window authentication mismatch")
    call_offset = site.call_address - site.window_start
    call_word = int.from_bytes(window[call_offset : call_offset + 4], "little")
    delay_word = int.from_bytes(window[call_offset + 4 : call_offset + 8], "little")
    callee = next(function for function in contract.functions if function.identifier == "vsync")
    target = (site.call_address & 0xF0000000) | ((call_word & 0x03FFFFFF) << 2)
    if call_word >> 26 != 3 or target != callee.entry_address or is_control_transfer(delay_word):
        fail("caller mismatch at authenticated 0x800781bc site")
    if site.framebuffer_context != contract.producer.framebuffer_context or site.ot_context != contract.producer.ot_context:
        fail("site and producer context mismatch")
    zero = Digest32(bytes(32))
    records = [
        MetadataRecord(RECORD_IDS[GAME_ID], GAME_ID, 1, contract.game.base_address, 0, game_actual.sha256, zero, "", ""),
        MetadataRecord(RECORD_IDS[FIELD_ID], FIELD_ID, 2, field.base_address, 0, overlay_actual.sha256, zero, "", ""),
        MetadataRecord(RECORD_IDS[PRODUCER_ID], PRODUCER_ID, 3, contract.producer.entry_address, 0, overlay_actual.sha256, zero, contract.producer.framebuffer_context, contract.producer.ot_context),
        MetadataRecord(RECORD_IDS[SITE_ID], SITE_ID, 4, site.call_address, callee.entry_address, overlay_actual.sha256, site.window_sha256, site.framebuffer_context, site.ot_context),
    ]
    records.extend(MetadataRecord(RECORD_IDS[function.identifier], function.identifier, 5, function.entry_address, 0, game_actual.sha256, zero, "", "") for function in contract.functions)
    validation = ManifestValidationMetadata(
        producer_record_id=RECORD_IDS[PRODUCER_ID],
        site_record_id=RECORD_IDS[SITE_ID],
        field_base_crc32=field.range_crc32,
        field_range_crc32=field.range_crc32,
        field_range_start=field_start,
        field_range_size=field.range_size,
        producer_entry=contract.producer.entry_address,
        caller_site=site.call_address,
        static_callee=callee.entry_address,
        return_site=site.return_address,
        instruction_window_start=site.window_start,
        instruction_window_size=site.window_size,
        instruction_window_identity=site.window_sha256,
        required_jal_opcode=3,
        jal_target=callee.entry_address,
        required_delay_slot_instructions=1,
        required_delay_slot_non_control_transfer=1,
    )
    manifest_identity = Digest32(hashlib.sha256(inputs.manifest.read_bytes()).digest())
    return VerifiedManifest(game_actual.sha256, manifest_identity, contract.game.namespace_crc32, tuple(sorted(records, key=lambda item: item.identifier)), validation)
