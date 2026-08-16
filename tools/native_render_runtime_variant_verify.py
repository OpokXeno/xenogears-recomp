from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path
import zlib

from native_render_manifest_model import Digest32, fail
from native_render_manifest_verify import bounded_bytes, bounded_file_bytes, file_identity
from native_render_runtime_variant_model import (
    HookSpec,
    RuntimeVariant,
    RuntimeVariantContract,
)


@dataclass(frozen=True, slots=True)
class VerificationInputs:
    companion: Path
    canonical_manifest: Path
    artifact: Path


@dataclass(frozen=True, slots=True)
class VerifiedRuntimeVariants:
    contract: RuntimeVariantContract
    companion_identity: Digest32


def declare(contract: RuntimeVariantContract, companion: Path,
            canonical_manifest: Path) -> VerifiedRuntimeVariants:
    canonical_identity = Digest32(hashlib.sha256(canonical_manifest.read_bytes()).digest())
    if canonical_identity != contract.canonical.manifest_identity:
        fail("field5 canonical manifest binding mismatch")
    return VerifiedRuntimeVariants(
        contract, Digest32(hashlib.sha256(companion.read_bytes()).digest()))


def range_contains(start: int, size: int, value: int, value_size: int) -> bool:
    return start <= value and value + value_size <= start + size


def check_hook(artifact: Path, base: int, range_start: int, range_size: int,
               hook: HookSpec, label: str) -> None:
    if not range_contains(range_start, range_size, hook.window_start, hook.window_size):
        fail(f"field5 {label} window escapes authenticated range")
    if not range_contains(hook.window_start, hook.window_size, hook.site, 8):
        fail(f"field5 {label} site escapes authenticated window")
    window = bounded_bytes(artifact, hook.window_start - base, hook.window_size)
    if hashlib.sha256(window).digest() != hook.window_identity:
        fail(f"field5 {label} window identity mismatch")
    offset = hook.site - hook.window_start
    jal = int.from_bytes(window[offset : offset + 4], "little")
    delay = int.from_bytes(window[offset + 4 : offset + 8], "little")
    target = (hook.site & 0xF0000000) | ((jal & 0x03FFFFFF) << 2)
    if jal >> 26 != 3 or target != hook.target or delay != hook.delay_instruction:
        fail(f"field5 {label} instruction constraints mismatch")


def check_variant(contract: RuntimeVariantContract, artifact: Path,
                  variant: RuntimeVariant) -> None:
    spec = contract.artifact
    range_start = spec.base_address + spec.range_offset
    if not range_contains(range_start, spec.range_size,
                          variant.physical_producer_entry, 4):
        fail("field5 physical producer escapes authenticated range")
    if not range_contains(range_start, spec.range_size,
                          variant.physical_return_site, 4):
        fail("field5 physical return escapes authenticated range")
    check_hook(artifact, spec.base_address, range_start, spec.range_size,
               variant.activation, "activation")
    check_hook(artifact, spec.base_address, range_start, spec.range_size,
               variant.capture, "capture")
    if variant.activation.target != variant.physical_producer_entry:
        fail("field5 activation target does not enter physical producer")
    if variant.capture.target != contract.canonical.static_callee:
        fail("field5 capture target does not bind canonical VSync")
    if variant.physical_return_site != variant.capture.site + 8:
        fail("field5 physical return does not follow the capture delay slot")
    if not variant.native_cutovers:
        fail("field5 native cutover is missing")
    for cutover in variant.native_cutovers:
        if not range_contains(range_start, spec.range_size, cutover.pc, 4) or (
                cutover.transfer == "local" and not range_contains(
                    range_start, spec.range_size, cutover.continuation, 4)):
            fail("field5 native cutover escapes authenticated range")
        cutover_word = int.from_bytes(
            bounded_file_bytes(artifact, cutover.pc - spec.base_address, 4), "little")
        if cutover_word != cutover.instruction:
            fail("field5 native cutover instruction identity mismatch")
    for site in variant.source_sites:
        if not range_contains(range_start, spec.range_size, site.pc, 4):
            fail("field5 source site escapes authenticated range")
        word = int.from_bytes(bounded_file_bytes(artifact, site.pc - spec.base_address, 4), "little")
        if word != site.instruction:
            fail("field5 source instruction identity mismatch")


def verify(contract: RuntimeVariantContract,
           inputs: VerificationInputs) -> VerifiedRuntimeVariants:
    canonical_identity = Digest32(hashlib.sha256(inputs.canonical_manifest.read_bytes()).digest())
    if canonical_identity != contract.canonical.manifest_identity:
        fail("field5 canonical manifest binding mismatch")
    if inputs.artifact.name != contract.artifact.file_name:
        fail("field5 artifact filename mismatch")
    actual = file_identity(inputs.artifact)
    if actual != contract.artifact.identity:
        fail("field5 artifact identity mismatch")
    spec = contract.artifact
    range_data = bounded_bytes(inputs.artifact, spec.range_offset, spec.range_size)
    if (Digest32(hashlib.sha256(range_data).digest()) != spec.range_identity or
            zlib.crc32(range_data) & 0xFFFFFFFF != spec.range_crc32):
        fail("field5 artifact range identity mismatch")
    if len(contract.variants) != 1:
        fail("field5 companion descriptors are ambiguous")
    check_variant(contract, inputs.artifact, contract.variants[0])
    return VerifiedRuntimeVariants(
        contract,
        Digest32(hashlib.sha256(inputs.companion.read_bytes()).digest()),
    )
