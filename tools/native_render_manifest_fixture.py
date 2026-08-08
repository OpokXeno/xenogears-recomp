from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import zlib

from native_render_manifest_model import ManifestError, load_contract
from native_render_manifest_output import (
    FIXTURE_SCHEMA,
    MetadataDocument,
    PRIVACY,
    SHARD_SCHEMA,
    check_shard_payload,
    check_table_payload,
    render_c,
    table_payload,
)
from native_render_manifest_verify import VerificationInputs, verify


FIELD_BASE = 0x8006F000
FIELD_HEADER_SIZE = 0x800
PRODUCER_ADDRESS = 0x80075B44
SITE_ADDRESS = 0x800781BC
VSYNC_ADDRESS = 0x8004B54C


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def checksum(data: bytes) -> str:
    return f"{zlib.crc32(data) & 0xFFFFFFFF:08x}"


def psx_exe(payload: bytes, base_address: int) -> bytes:
    header = bytearray(FIELD_HEADER_SIZE)
    header[:8] = b"PS-X EXE"
    header[0x10:0x14] = base_address.to_bytes(4, "little")
    header[0x18:0x1c] = base_address.to_bytes(4, "little")
    header[0x1c:0x20] = len(payload).to_bytes(4, "little")
    return bytes(header) + payload


def fixture_manifest(exe: bytes, field: bytes) -> str:
    field_payload = field[FIELD_HEADER_SIZE:]
    window_start = SITE_ADDRESS - 8
    window_offset = FIELD_HEADER_SIZE + window_start - FIELD_BASE
    window = field[window_offset : window_offset + 16]
    return f'''schema = "xg-render-manifest/v3"

[game]
id = "main-disc1-exe"
serial = "SLUS-00664"
disc = 1
sha256 = "{digest(exe)}"
crc32 = "{checksum(exe)}"
size = {len(exe)}
namespace_crc32 = "89abcdef"
base_address = "0x80010000"
image_format = "ps-x-exe"
header_size = {FIELD_HEADER_SIZE}
loaded_size = {len(exe) - FIELD_HEADER_SIZE}

[overlay.field]
id = "field-image"
state = "authenticated"
file = "field.bin"
full_sha256 = "{digest(field)}"
full_crc32 = "{checksum(field)}"
full_size = {len(field)}
base_address = "0x8006f000"
image_format = "ps-x-exe"
header_size = {FIELD_HEADER_SIZE}
loaded_size = {len(field_payload)}
range_offset = 0
range_size = {len(field_payload)}
range_crc32 = "{checksum(field_payload)}"

[[functions]]
id = "draw-otag"
image = "main-disc1-exe"
name = "DrawOTag"
entry_address = "0x80044bd0"
confidence = "verified"

[[functions]]
id = "vsync"
image = "main-disc1-exe"
name = "VSync"
entry_address = "0x8004b54c"
confidence = "verified"

[[producers]]
id = "render-field-character-sprites"
image = "field-image"
name = "RenderFieldCharacterSprites"
entry_address = "0x{PRODUCER_ADDRESS:08x}"
framebuffer_context = "field-double-buffer"
ot_context = "field-ot"
confidence = "verified"

[[sites]]
id = "field-vsync-call-0x800781bc"
state = "authenticated"
producer = "render-field-character-sprites"
image = "field-image"
callee = "vsync"
call_address = "0x{SITE_ADDRESS:08x}"
return_address = "0x{SITE_ADDRESS + 8:08x}"
window_start = "0x{window_start:08x}"
window_size = 16
window_sha256 = "{digest(window)}"
framebuffer_context = "field-double-buffer"
ot_context = "field-ot"
confidence = "verified"
'''


def expect_rejection(action, expected: str) -> None:
    try:
        action()
    except ManifestError as error:
        if expected not in str(error):
            raise ManifestError(f"fixture expected {expected}, received {error}") from error
        return
    raise ManifestError(f"fixture expected rejection: {expected}")


def run_fixture(canonical_manifest: Path) -> MetadataDocument:
    rejected: list[str] = []
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        exe_payload = bytes(range(256)) * 1176
        exe_bytes = psx_exe(exe_payload, 0x80010000)
        field_data = bytearray((index * 17 + 3) & 0xFF for index in range(0x9200))
        call_offset = SITE_ADDRESS - FIELD_BASE
        field_data[call_offset : call_offset + 4] = ((3 << 26) | ((VSYNC_ADDRESS >> 2) & 0x03FFFFFF)).to_bytes(4, "little")
        field_data[call_offset + 4 : call_offset + 8] = (0x34040002).to_bytes(4, "little")
        field_bytes = psx_exe(bytes(field_data), FIELD_BASE)
        overlays = root / "overlays"
        overlays.mkdir()
        exe = root / "main.exe"
        manifest = root / "manifest.toml"
        field_path = overlays / "field.bin"
        exe.write_bytes(exe_bytes)
        field_path.write_bytes(field_bytes)
        valid_text = fixture_manifest(exe_bytes, field_bytes)
        manifest.write_text(valid_text, encoding="utf-8", newline="\n")
        inputs = VerificationInputs(manifest, exe, overlays)
        verified = verify(load_contract(manifest), inputs)
        (root / "table.c").write_bytes(render_c(verified))
        table = table_payload(verified)
        check_table_payload(verified, table)

        missing = root / "missing.toml"
        missing.write_text(valid_text.replace(next(line for line in valid_text.splitlines(keepends=True) if line.startswith("sha256 = ")), ""), newline="\n")
        expect_rejection(lambda: load_contract(missing), "missing identity")
        rejected.append("missing-identity")

        exe.write_bytes(exe_bytes[:-1] + b"X")
        expect_rejection(lambda: verify(load_contract(manifest), inputs), "full game identity mismatch")
        rejected.append("full-identity-mismatch")
        exe.write_bytes(exe_bytes)

        collision = json.loads(json.dumps(table))
        collision["game_identity"][0] ^= 0xFF
        expect_rejection(lambda: check_table_payload(verified, collision), "namespace collision")
        rejected.append("same-namespace-different-full-sha")

        changed = root / "changed.toml"
        changed.write_text(valid_text.replace("field-ot", "field-ot-next"), newline="\n")
        changed_verified = verify(load_contract(changed), VerificationInputs(changed, exe, overlays))
        expect_rejection(lambda: check_table_payload(changed_verified, table), "stale table manifest identity")
        rejected.append("stale-manifest-table-metadata")

        shard = {
            "schema": SHARD_SCHEMA,
            "game_identity": list(verified.game_identity),
            "manifest_identity": [0] * 32,
            "namespace_crc32": f"{verified.namespace_crc32:08x}",
        }
        expect_rejection(lambda: check_shard_payload(verified, shard), "shard-export identity mismatch")
        rejected.append("shard-export-identity-mismatch")

    blockers = list(load_contract(canonical_manifest).blockers())
    return {
        "schema": FIXTURE_SCHEMA,
        "state": "pass",
        "positive_paths": ["validate", "emit", "check-metadata"],
        "rejected_cases": rejected,
        "canonical_blockers": blockers,
        "privacy": PRIVACY,
        "cleanup": {"temporary_inputs_created": True, "temporary_inputs_cleaned": True},
    }
