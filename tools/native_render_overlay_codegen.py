from __future__ import annotations

import hashlib
import struct
from typing import Final
import zlib

from native_render_manifest_model import Digest32, FileIdentity, fail
from native_render_runtime_variant_model import (
    ArtifactSpec,
    RuntimeVariantContract,
    SourceSite,
)


PLAN_SCHEMA: Final = "psxrecomp-source-observation-plan-v5"
OPERATIONS: Final = frozenset({"read", "write", "swc2", "call", "bucket"})
AUXILIARIES: Final = frozenset({"effective-address", "none", "result-register"})


def artifact_matches(spec: ArtifactSpec, data: bytes, load_address: int) -> bool:
    identity = FileIdentity(
        Digest32(hashlib.sha256(data).digest()),
        zlib.crc32(data) & 0xFFFFFFFF,
        len(data),
    )
    if load_address != spec.base_address or identity != spec.identity:
        return False
    range_end = spec.range_offset + spec.range_size
    if range_end > len(data):
        return False
    range_data = data[spec.range_offset:range_end]
    return (
        Digest32(hashlib.sha256(range_data).digest()) == spec.range_identity
        and zlib.crc32(range_data) & 0xFFFFFFFF == spec.range_crc32
    )


def artifact_code_contract_matches(
    contract: RuntimeVariantContract, data: bytes, load_address: int
) -> bool:
    spec = contract.artifact
    if load_address != spec.base_address or len(data) != spec.identity.size:
        return False
    signature_count = sum(
        len(variant.native_cutovers) + len(variant.source_sites)
        for variant in contract.variants
    )
    if signature_count < 8:
        return False
    for variant in contract.variants:
        for cutover in variant.native_cutovers:
            offset = cutover.pc - spec.base_address
            if offset < 0 or offset + 4 > len(data) or struct.unpack_from(
                "<I", data, offset
            )[0] != cutover.instruction:
                return False
        for site in variant.source_sites:
            offset = site.pc - spec.base_address
            if offset < 0 or offset + 4 > len(data) or struct.unpack_from(
                "<I", data, offset
            )[0] != site.instruction:
                return False
    return True


def validate_site(spec: ArtifactSpec, data: bytes, site: SourceSite) -> None:
    range_start = spec.base_address + spec.range_offset
    range_end = range_start + spec.range_size
    if site.pc < range_start or site.pc + 4 > range_end or site.pc % 4 != 0:
        fail("source observation site escapes the authenticated artifact range")
    if site.operation not in OPERATIONS or site.auxiliary not in AUXILIARIES:
        fail("source observation site uses an unsupported closed token")
    address_operation = site.operation in {"read", "write", "swc2"}
    contract_is_valid = (
        address_operation
        and site.auxiliary == "effective-address"
        and site.width in {1, 2, 4}
    ) or (
        site.operation == "call"
        and site.auxiliary == "none"
        and site.width == 0
    ) or (
        site.operation == "bucket"
        and site.auxiliary == "result-register"
        and site.width == 0
    )
    if not contract_is_valid:
        fail("source observation operation, width, and auxiliary disagree")
    offset = site.pc - spec.base_address
    if struct.unpack_from("<I", data, offset)[0] != site.instruction:
        fail("source observation instruction identity mismatch")


def source_observation_plan_for_artifact(
    contract: RuntimeVariantContract,
    data: bytes,
    load_address: int,
) -> str | None:
    if not artifact_matches(contract.artifact, data, load_address) and not (
        artifact_code_contract_matches(contract, data, load_address)
    ):
        return None
    lines = [PLAN_SCHEMA]
    for variant in contract.variants:
        lifecycle = (
            ("entry", variant.physical_producer_entry, 0),
            ("capture", variant.capture.site, variant.capture.delay_instruction),
            ("return", variant.physical_return_site, 0),
        )
        for role, pc, delay in lifecycle:
            offset = pc - contract.artifact.base_address
            if offset < 0 or offset + 4 > len(data):
                fail("lifecycle site escapes the authenticated artifact range")
            instruction = struct.unpack_from("<I", data, offset)[0]
            if role == "capture":
                delay_offset = offset + 4
                if (delay_offset + 4 > len(data) or
                        struct.unpack_from("<I", data, delay_offset)[0] != delay):
                    fail("lifecycle capture delay instruction identity mismatch")
            lines.append(
                f"lifecycle {pc:08X} {instruction:08X} {role} {delay:08X}"
            )
        for cutover in variant.native_cutovers:
            offset = cutover.pc - contract.artifact.base_address
            if offset < 0 or offset + 4 > len(data):
                fail("native cutover escapes the authenticated artifact range")
            if struct.unpack_from("<I", data, offset)[0] != cutover.instruction:
                fail("native cutover instruction identity mismatch")
            lines.append(
                f"cutover {cutover.pc:08X} {cutover.instruction:08X} "
                f"{cutover.transfer} {cutover.continuation:08X}"
            )
        for site in variant.source_sites:
            validate_site(contract.artifact, data, site)
            lines.append(
                f"site {site.pc:08X} {site.instruction:08X} "
                f"{site.operation} {site.width} {site.auxiliary}"
            )
    if len(lines) == 1:
        fail("authenticated artifact has no source observation sites")
    return "\n".join(lines) + "\n"
