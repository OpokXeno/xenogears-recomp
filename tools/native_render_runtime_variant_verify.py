from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path
import zlib

from native_render_manifest_model import Digest32, fail
from native_render_manifest_model import load_contract as load_canonical_contract
from native_render_manifest_verify import (
    RECORD_IDS,
    bounded_bytes,
    bounded_file_bytes,
    file_identity,
)
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
        fail("runtime canonical manifest binding mismatch")
    check_canonical_contract(contract, canonical_manifest)
    check_contract_structure(contract)
    if contract.document_identity is None:
        fail("runtime companion manifest identity is unavailable")
    return VerifiedRuntimeVariants(contract, contract.document_identity)


def range_contains(start: int, size: int, value: int, value_size: int) -> bool:
    return start <= value and value + value_size <= start + size


def check_canonical_contract(contract: RuntimeVariantContract,
                             canonical_manifest: Path) -> None:
    canonical = load_canonical_contract(canonical_manifest)
    site = canonical.site
    static_callee = next((
        function.entry_address for function in canonical.functions
        if function.identifier == "vsync"
    ), None)
    if (canonical.game.identity.sha256 != contract.canonical.game_identity or
            contract.canonical.producer_record_id !=
                RECORD_IDS["render-field-character-sprites"] or
            contract.canonical.site_record_id !=
                RECORD_IDS["field-vsync-call-0x800781bc"] or
            canonical.producer.entry_address !=
                contract.canonical.producer_entry or
            not hasattr(site, "call_address") or
            site.call_address != contract.canonical.capture_site or
            static_callee != contract.canonical.static_callee or
            site.return_address != contract.canonical.return_site):
        fail("runtime canonical game or producer binding mismatch")


def check_hook(artifact: Path, base: int, range_start: int, range_size: int,
               hook: HookSpec, label: str) -> None:
    if not range_contains(range_start, range_size, hook.window_start, hook.window_size):
        fail(f"runtime {label} window escapes authenticated range")
    if not range_contains(hook.window_start, hook.window_size, hook.site, 8):
        fail(f"runtime {label} site escapes authenticated window")
    window = bounded_bytes(artifact, hook.window_start - base, hook.window_size)
    if hashlib.sha256(window).digest() != hook.window_identity:
        fail(f"runtime {label} window identity mismatch")
    offset = hook.site - hook.window_start
    jal = int.from_bytes(window[offset : offset + 4], "little")
    delay = int.from_bytes(window[offset + 4 : offset + 8], "little")
    target = (hook.site & 0xF0000000) | ((jal & 0x03FFFFFF) << 2)
    if jal >> 26 != 3 or target != hook.target or delay != hook.delay_instruction:
        fail(f"runtime {label} instruction constraints mismatch")


def check_contract_structure(contract: RuntimeVariantContract) -> None:
    spec = contract.artifact
    range_start = spec.base_address + spec.range_offset
    for variant in contract.variants:
        if not range_contains(range_start, spec.range_size,
                              variant.physical_producer_entry, 4):
            fail("runtime physical producer escapes authenticated range")
        if not range_contains(range_start, spec.range_size,
                              variant.physical_return_site, 4):
            fail("runtime physical return escapes authenticated range")
        for label, hook in (
                ("activation", variant.activation),
                ("capture", variant.capture)):
            if not range_contains(range_start, spec.range_size,
                                  hook.window_start, hook.window_size):
                fail(f"runtime {label} window escapes authenticated range")
        if variant.activation.target != variant.physical_producer_entry:
            fail("runtime activation target does not enter physical producer")
        if variant.capture.target != contract.canonical.static_callee:
            fail("runtime capture target does not bind canonical VSync")
        if variant.physical_return_site != variant.capture.site + 8:
            fail("runtime physical return does not follow the capture delay slot")
        if not range_contains(
                range_start, spec.range_size,
                variant.model_dispatch_window_start,
                len(variant.model_dispatch_instructions) * 4):
            fail("runtime model dispatch contract escapes authenticated range")
        for cutover in variant.native_cutovers:
            if (not range_contains(range_start, spec.range_size,
                                   cutover.code_range_start,
                                   cutover.code_range_size) or
                    not range_contains(cutover.code_range_start,
                                       cutover.code_range_size,
                                       cutover.pc, 4) or (
                    cutover.transfer == "local" and not range_contains(
                        range_start, spec.range_size,
                        cutover.continuation, 4))):
                fail("runtime native cutover escapes authenticated range")
        for site in variant.source_sites:
            if not range_contains(range_start, spec.range_size, site.pc, 4):
                fail("runtime source site escapes authenticated range")


def check_variant(contract: RuntimeVariantContract, artifact: Path,
                   variant: RuntimeVariant) -> None:
    spec = contract.artifact
    range_start = spec.base_address + spec.range_offset
    check_hook(artifact, spec.base_address, range_start, spec.range_size,
               variant.activation, "activation")
    check_hook(artifact, spec.base_address, range_start, spec.range_size,
               variant.capture, "capture")
    model_dispatch = bounded_file_bytes(
        artifact, variant.model_dispatch_window_start - spec.base_address,
        len(variant.model_dispatch_instructions) * 4)
    for index, instruction in enumerate(variant.model_dispatch_instructions):
        if int.from_bytes(
                model_dispatch[index * 4:index * 4 + 4], "little") != instruction:
            fail("runtime model dispatch instruction identity mismatch")
    if not variant.native_cutovers:
        fail("runtime native cutover is missing")
    for cutover in variant.native_cutovers:
        code_range = bounded_file_bytes(
            artifact, cutover.code_range_start - spec.base_address,
            cutover.code_range_size)
        if hashlib.sha256(code_range).digest() != cutover.code_range_identity:
            fail("runtime native cutover code range identity mismatch")
        cutover_word = int.from_bytes(
            bounded_file_bytes(artifact, cutover.pc - spec.base_address, 4), "little")
        if cutover_word != cutover.instruction:
            fail("runtime native cutover instruction identity mismatch")
    for site in variant.source_sites:
        word = int.from_bytes(bounded_file_bytes(artifact, site.pc - spec.base_address, 4), "little")
        if word != site.instruction:
            fail("runtime source instruction identity mismatch")


def verify(contract: RuntimeVariantContract,
           inputs: VerificationInputs) -> VerifiedRuntimeVariants:
    canonical_identity = Digest32(hashlib.sha256(inputs.canonical_manifest.read_bytes()).digest())
    if canonical_identity != contract.canonical.manifest_identity:
        fail("runtime canonical manifest binding mismatch")
    check_canonical_contract(contract, inputs.canonical_manifest)
    check_contract_structure(contract)
    if inputs.artifact.name != contract.artifact.file_name:
        fail("runtime artifact filename mismatch")
    actual = file_identity(inputs.artifact)
    if actual != contract.artifact.identity:
        fail("runtime artifact identity mismatch")
    spec = contract.artifact
    range_data = bounded_bytes(inputs.artifact, spec.range_offset, spec.range_size)
    if (Digest32(hashlib.sha256(range_data).digest()) != spec.range_identity or
            zlib.crc32(range_data) & 0xFFFFFFFF != spec.range_crc32):
        fail("runtime artifact range identity mismatch")
    for variant in contract.variants:
        check_variant(contract, inputs.artifact, variant)
    if contract.document_identity is None:
        fail("runtime companion manifest identity is unavailable")
    return VerifiedRuntimeVariants(contract, contract.document_identity)
