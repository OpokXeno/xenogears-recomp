from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path
import tomllib
from typing import Final

from native_render_manifest_model import (
    Digest32,
    FileIdentity,
    ManifestError,
    ManifestValue,
    address,
    closed,
    digest,
    fail,
    hex_value,
    integer,
    text,
)


SCHEMA: Final = "xg-render-runtime-variants/v1"
DESCRIPTOR_CAP: Final = 64
SOURCE_SITE_CAP: Final = 64
CUTOVER_CAP: Final = 64
MODEL_DISPATCH_CAP: Final = 16


@dataclass(frozen=True, slots=True)
class CanonicalTuple:
    game_identity: Digest32
    manifest_identity: Digest32
    producer_record_id: int
    site_record_id: int
    producer_entry: int
    capture_site: int
    static_callee: int
    return_site: int


@dataclass(frozen=True, slots=True)
class ArtifactSpec:
    file_name: str
    identity: FileIdentity
    base_address: int
    range_offset: int
    range_size: int
    range_identity: Digest32
    range_crc32: int


@dataclass(frozen=True, slots=True)
class HookSpec:
    window_start: int
    window_size: int
    window_identity: Digest32
    site: int
    target: int
    delay_instruction: int

@dataclass(frozen=True, slots=True)
class SourceSite:
    pc: int
    instruction: int
    operation: str
    width: int
    auxiliary: str


@dataclass(frozen=True, slots=True)
class NativeCutover:
    pc: int
    instruction: int
    transfer: str
    continuation: int
    handler: str
    code_range_start: int
    code_range_size: int
    code_range_identity: Digest32


@dataclass(frozen=True, slots=True)
class RuntimeVariant:
    identifier: str
    activation: HookSpec
    physical_producer_entry: int
    capture: HookSpec
    physical_return_site: int
    model_dispatch_window_start: int
    model_dispatch_matrix_stack_offset: int
    model_dispatch_instructions: tuple[int, ...]
    source_sites: tuple[SourceSite, ...]
    native_cutovers: tuple[NativeCutover, ...] = ()


@dataclass(frozen=True, slots=True)
class RuntimeVariantContract:
    canonical: CanonicalTuple
    artifact: ArtifactSpec
    variants: tuple[RuntimeVariant, ...]
    document_identity: Digest32 | None = None


def parse_canonical(raw: ManifestValue) -> CanonicalTuple:
    value = closed(raw, {
        "game_sha256", "manifest_sha256", "producer_record_id", "site_record_id",
        "producer_entry", "capture_site", "static_callee", "return_site",
    }, "runtime-variants.canonical")
    canonical = CanonicalTuple(
        digest(value["game_sha256"], "runtime-variants.canonical.game_sha256"),
        digest(value["manifest_sha256"], "runtime-variants.canonical.manifest_sha256"),
        integer(value["producer_record_id"], "runtime-variants.canonical.producer_record_id", True),
        integer(value["site_record_id"], "runtime-variants.canonical.site_record_id", True),
        address(value["producer_entry"], "runtime-variants.canonical.producer_entry"),
        address(value["capture_site"], "runtime-variants.canonical.capture_site"),
        address(value["static_callee"], "runtime-variants.canonical.static_callee"),
        address(value["return_site"], "runtime-variants.canonical.return_site"),
    )
    return canonical


def parse_artifact(raw: ManifestValue) -> ArtifactSpec:
    value = closed(raw, {
        "file", "full_sha256", "full_crc32", "full_size", "base_address",
        "range_offset", "range_size", "range_sha256", "range_crc32",
    }, "runtime-variants.artifact")
    file_name = value["file"]
    if not isinstance(file_name, str) or Path(file_name).name != file_name:
        fail("runtime artifact filename must be a basename")
    artifact = ArtifactSpec(
        file_name,
        FileIdentity(
            digest(value["full_sha256"], "runtime-variants.artifact.full_sha256"),
            hex_value(value["full_crc32"], "runtime-variants.artifact.full_crc32", 8),
            integer(value["full_size"], "runtime-variants.artifact.full_size", True),
        ),
        address(value["base_address"], "runtime-variants.artifact.base_address"),
        integer(value["range_offset"], "runtime-variants.artifact.range_offset"),
        integer(value["range_size"], "runtime-variants.artifact.range_size", True),
        digest(value["range_sha256"], "runtime-variants.artifact.range_sha256"),
        hex_value(value["range_crc32"], "runtime-variants.artifact.range_crc32", 8),
    )
    if artifact.range_offset + artifact.range_size > artifact.identity.size:
        fail("runtime artifact range escapes the declared image")
    return artifact


def parse_hook(value: dict[str, ManifestValue], prefix: str) -> HookSpec:
    hook = HookSpec(
        address(value[f"{prefix}_window_start"], f"runtime-variants.{prefix}_window_start"),
        integer(value[f"{prefix}_window_size"], f"runtime-variants.{prefix}_window_size", True),
        digest(value[f"{prefix}_window_sha256"], f"runtime-variants.{prefix}_window_sha256"),
        address(value[f"{prefix}_site"], f"runtime-variants.{prefix}_site"),
        address(value[f"{prefix}_target"], f"runtime-variants.{prefix}_target"),
        address(value[f"{prefix}_delay_instruction"], f"runtime-variants.{prefix}_delay_instruction"),
    )
    if hook.window_size < 8 or not (
            hook.window_start <= hook.site and
            hook.site + 8 <= hook.window_start + hook.window_size):
        fail(f"runtime {prefix} hook escapes its instruction window")
    return hook


def parse_variant(raw: ManifestValue) -> RuntimeVariant:
    keys = {
        "id", "activation_window_start", "activation_window_size", "activation_window_sha256",
        "activation_site", "activation_target", "activation_delay_instruction",
        "physical_producer_entry", "capture_window_start", "capture_window_size",
        "capture_window_sha256", "capture_site", "capture_target",
        "capture_delay_instruction", "physical_return_site", "native_cutovers",
        "model_dispatch_window_start", "model_dispatch_matrix_stack_offset",
        "model_dispatch_instructions", "source_sites",
    }
    value = closed(raw, keys, "runtime-variants.variant")
    activation = parse_hook(value, "activation")
    capture = parse_hook(value, "capture")
    source_raw = value["source_sites"]
    if (not isinstance(source_raw, list) or not source_raw or
            len(source_raw) > SOURCE_SITE_CAP):
        fail("runtime source sites must be a non-empty bounded array")
    source_sites = tuple(parse_source_site(site) for site in source_raw)
    normalized_pcs = tuple(site.pc & 0x1fffffff for site in source_sites)
    if any(left >= right for left, right in zip(
            normalized_pcs, normalized_pcs[1:])):
        fail("runtime source sites must be ordered by normalized PC")
    if sum(site.operation == "call" for site in source_sites) != 1 or sum(
            site.operation == "bucket" for site in source_sites) != 1:
        fail("runtime source sites require one geometry call and one bucket")
    if len({(site.pc & 0x1fffffff, site.instruction) for site in source_sites}) != len(source_sites):
        fail("duplicate normalized runtime source sites are forbidden")
    cutovers_raw = value["native_cutovers"]
    if (not isinstance(cutovers_raw, list) or not cutovers_raw or
            len(cutovers_raw) > CUTOVER_CAP):
        fail("runtime native cutovers must be a non-empty bounded array")
    cutovers = tuple(parse_native_cutover(item) for item in cutovers_raw)
    if len({(item.pc & 0x1fffffff, item.instruction) for item in cutovers}) != len(cutovers):
        fail("duplicate normalized runtime native cutovers are forbidden")
    model_dispatch_raw = value["model_dispatch_instructions"]
    if (not isinstance(model_dispatch_raw, list) or not model_dispatch_raw or
            len(model_dispatch_raw) > MODEL_DISPATCH_CAP):
        fail("runtime model dispatch contract must be a non-empty bounded array")
    model_dispatch_instructions = tuple(
        address(item, "runtime-variants.model_dispatch_instruction")
        for item in model_dispatch_raw)
    model_dispatch_window_start = address(
        value["model_dispatch_window_start"],
        "runtime-variants.model_dispatch_window_start")
    model_dispatch_matrix_stack_offset = integer(
        value["model_dispatch_matrix_stack_offset"],
        "runtime-variants.model_dispatch_matrix_stack_offset", True)
    if model_dispatch_matrix_stack_offset & 3:
        fail("runtime model dispatch matrix stack offset must be aligned")
    variant = RuntimeVariant(
        text(value["id"], "runtime-variants.variant.id"), activation,
        address(value["physical_producer_entry"], "runtime-variants.variant.physical_producer_entry"),
        capture,
        address(value["physical_return_site"], "runtime-variants.variant.physical_return_site"),
        model_dispatch_window_start, model_dispatch_matrix_stack_offset,
        model_dispatch_instructions,
        source_sites, cutovers,
    )
    return variant


def parse_native_cutover(raw: ManifestValue) -> NativeCutover:
    value = closed(raw, {
        "pc", "instruction", "transfer", "continuation", "handler",
        "code_range_start", "code_range_size", "code_range_sha256",
    },
                   "runtime-variants.native-cutover")
    transfer = text(value["transfer"], "runtime-variants.native-cutover.transfer")
    handler = text(value["handler"], "runtime-variants.native-cutover.handler")
    if transfer not in {"local", "observe", "return"}:
        fail("runtime native cutover transfer is unsupported")
    handler_transfers = {
        "actor": "local",
        "compass-world": "return",
        "compass-screen": "return",
        "zoom-rgb-begin": "observe",
        "zoom-rgb-commit": "observe",
        "zoom-entry": "observe",
        "zoom-native": "local",
        "zoom-initializer-begin": "observe",
        "zoom-initializer-commit": "observe",
        "particle-initializer": "observe",
        "particle-native": "return",
    }
    if handler_transfers.get(handler) != transfer:
        fail("runtime native cutover handler disagrees with transfer")
    cutover = NativeCutover(
        address(value["pc"], "runtime-variants.native-cutover.pc"),
        address(value["instruction"], "runtime-variants.native-cutover.instruction"),
        transfer,
        address(value["continuation"], "runtime-variants.native-cutover.continuation"),
        handler,
        address(value["code_range_start"],
                "runtime-variants.native-cutover.code_range_start"),
        integer(value["code_range_size"],
                "runtime-variants.native-cutover.code_range_size", True),
        digest(value["code_range_sha256"],
               "runtime-variants.native-cutover.code_range_sha256"),
    )
    if (transfer == "local") != (cutover.continuation != 0):
        fail("runtime native cutover continuation disagrees with transfer")
    if not (cutover.code_range_start <= cutover.pc and
            cutover.pc + 4 <=
                cutover.code_range_start + cutover.code_range_size):
        fail("runtime native cutover escapes its code range")
    return cutover


def parse_source_site(raw: ManifestValue) -> SourceSite:
    value = closed(raw, {"pc", "instruction", "operation", "width", "auxiliary"},
                   "runtime-variants.source-site")
    operation = text(value["operation"], "runtime-variants.source-site.operation")
    auxiliary = text(value["auxiliary"], "runtime-variants.source-site.auxiliary")
    width = integer(value["width"], "runtime-variants.source-site.width")
    if operation not in {"read", "write", "swc2", "call", "bucket"}:
        fail("runtime source operation is unsupported")
    if auxiliary not in {"effective-address", "none", "result-register"}:
        fail("runtime source auxiliary rule is unsupported")
    valid_contract = (
        operation in {"read", "write"} and
        auxiliary == "effective-address" and width in {1, 2, 4}
    ) or (
        operation == "swc2" and
        auxiliary == "effective-address" and width == 4
    ) or (
        operation == "call" and auxiliary == "none" and width == 0
    ) or (
        operation == "bucket" and
        auxiliary == "result-register" and width == 0
    )
    if not valid_contract:
        fail("runtime source operation, width, and auxiliary disagree")
    return SourceSite(address(value["pc"], "runtime-variants.source-site.pc"),
                      address(value["instruction"], "runtime-variants.source-site.instruction"),
                      operation, width, auxiliary)


def normalized_signature(variant: RuntimeVariant) -> tuple[int, ...]:
    base = (
        variant.activation.window_start & 0x1FFFFFFF,
        variant.activation.site & 0x1FFFFFFF,
        variant.physical_producer_entry & 0x1FFFFFFF,
        variant.capture.window_start & 0x1FFFFFFF,
        variant.capture.site & 0x1FFFFFFF,
        variant.physical_return_site & 0x1FFFFFFF,
    )
    return base + tuple(
        value
        for cutover in variant.native_cutovers
        for value in (cutover.pc & 0x1FFFFFFF,
                      cutover.continuation & 0x1FFFFFFF)
    )


def parse_contract(data: bytes) -> RuntimeVariantContract:
    try:
        raw = tomllib.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        raise ManifestError("runtime variants manifest is unreadable") from error
    root = closed(raw, {"schema", "canonical", "artifact", "variants"}, "runtime-variants")
    if root["schema"] != SCHEMA:
        fail("runtime variants schema version is unsupported")
    variants_value = root["variants"]
    if (not isinstance(variants_value, list) or not variants_value or
            len(variants_value) > DESCRIPTOR_CAP):
        fail("runtime variants require a non-empty bounded descriptor array")
    variants = tuple(parse_variant(value) for value in variants_value)
    if len({normalized_signature(variant) for variant in variants}) != len(variants):
        fail("duplicate normalized runtime descriptors are forbidden")
    activation_signatures = {
        (
            variant.activation.site & 0x1fffffff,
            variant.activation.target & 0x1fffffff,
            variant.activation.delay_instruction,
        )
        for variant in variants
    }
    if len(activation_signatures) != len(variants):
        fail("runtime descriptors require distinct activation contracts")
    if len({
            variant.physical_producer_entry & 0x1fffffff
            for variant in variants
            }) != len(variants):
        fail("runtime descriptors require distinct physical producers")
    model_dispatch_contracts = {
        (
            variant.model_dispatch_matrix_stack_offset,
            variant.model_dispatch_instructions,
        )
        for variant in variants
    }
    if len(model_dispatch_contracts) != 1:
        fail("runtime descriptors sharing an artifact must agree on model dispatch")
    cutover_contracts: dict[tuple[int, int], tuple[object, ...]] = {}
    for variant in variants:
        for cutover in variant.native_cutovers:
            key = (cutover.pc & 0x1fffffff, cutover.instruction)
            value = (
                cutover.transfer, cutover.continuation & 0x1fffffff,
                cutover.handler, cutover.code_range_start & 0x1fffffff,
                cutover.code_range_size, cutover.code_range_identity,
            )
            if key in cutover_contracts and cutover_contracts[key] != value:
                fail("runtime descriptors disagree on a shared cutover")
            cutover_contracts[key] = value
    return RuntimeVariantContract(
        parse_canonical(root["canonical"]),
        parse_artifact(root["artifact"]), variants,
        Digest32(hashlib.sha256(data).digest()),
    )


def load_contract(path: Path) -> RuntimeVariantContract:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise ManifestError("runtime variants manifest is unreadable") from error
    return parse_contract(data)
