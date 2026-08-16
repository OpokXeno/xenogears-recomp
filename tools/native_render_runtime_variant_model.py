from __future__ import annotations

from dataclasses import dataclass
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
CANONICAL_GAME_SHA256: Final = "dc0b2dd786203d4cce5927c5a3fc85a18f39a3f7406078860076ebb0bbae7119"
CANONICAL_MANIFEST_SHA256: Final = "25b53839e2546e95b2c43496dee8c8001294ac237916de7d7f0e7e8fee5760bb"
ARTIFACT_SHA256: Final = "bc283d19e750f41c5dde5f3293805c37757c46ffe262ad55e82d107e0cf34b8a"
ARTIFACT_CRC32: Final = 0xB7CE1120
ARTIFACT_SIZE: Final = 282628
ARTIFACT_BASE: Final = 0x8006F000
ACTIVATION_WINDOW_SHA256: Final = "f18c97fc2dedd254c9542832a7036dc576238270bbf50902696fe1232bb465f5"
CAPTURE_WINDOW_SHA256: Final = "19c6de2c55f08cdc8aa53457678c795f9047c54951ecf1ea798b70e5be9ece05"


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


@dataclass(frozen=True, slots=True)
class RuntimeVariant:
    identifier: str
    activation: HookSpec
    physical_producer_entry: int
    capture: HookSpec
    physical_return_site: int
    source_sites: tuple[SourceSite, ...]
    native_cutovers: tuple[NativeCutover, ...] = ()


@dataclass(frozen=True, slots=True)
class RuntimeVariantContract:
    canonical: CanonicalTuple
    artifact: ArtifactSpec
    variants: tuple[RuntimeVariant, ...]


def expect(value: int | Digest32, expected: int | Digest32, label: str) -> None:
    if value != expected:
        fail(f"field5 {label} does not match the immutable descriptor")


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
    expect(canonical.game_identity, digest(CANONICAL_GAME_SHA256, "field5 canonical game"), "canonical game")
    expect(canonical.manifest_identity, digest(CANONICAL_MANIFEST_SHA256, "field5 canonical manifest"), "canonical manifest")
    if (canonical.producer_record_id, canonical.site_record_id,
        canonical.producer_entry, canonical.capture_site,
        canonical.static_callee, canonical.return_site) != (
             3, 4, 0x80075B44, 0x800781BC, 0x8004B54C, 0x800781C4):
        fail("field5 canonical logical tuple is not exact")
    return canonical


def parse_artifact(raw: ManifestValue) -> ArtifactSpec:
    value = closed(raw, {
        "file", "full_sha256", "full_crc32", "full_size", "base_address",
        "range_offset", "range_size", "range_sha256", "range_crc32",
    }, "runtime-variants.artifact")
    file_name = value["file"]
    if not isinstance(file_name, str) or Path(file_name).name != file_name:
        fail("field5 artifact filename must be a basename")
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
    if (artifact.file_name, artifact.identity.sha256, artifact.identity.crc32,
        artifact.identity.size, artifact.base_address, artifact.range_offset,
        artifact.range_size, artifact.range_identity, artifact.range_crc32) != (
            "field5_runtime.bin", digest(ARTIFACT_SHA256, "field5 artifact sha"),
            ARTIFACT_CRC32, ARTIFACT_SIZE, ARTIFACT_BASE, 0, ARTIFACT_SIZE,
            digest(ARTIFACT_SHA256, "field5 range sha"), ARTIFACT_CRC32):
        fail("field5 artifact identity or range is not exact")
    return artifact


def parse_hook(value: dict[str, ManifestValue], prefix: str, digest_hex: str,
               site: int, target: int, delay: int) -> HookSpec:
    hook = HookSpec(
        address(value[f"{prefix}_window_start"], f"runtime-variants.{prefix}_window_start"),
        integer(value[f"{prefix}_window_size"], f"runtime-variants.{prefix}_window_size", True),
        digest(value[f"{prefix}_window_sha256"], f"runtime-variants.{prefix}_window_sha256"),
        address(value[f"{prefix}_site"], f"runtime-variants.{prefix}_site"),
        address(value[f"{prefix}_target"], f"runtime-variants.{prefix}_target"),
        address(value[f"{prefix}_delay_instruction"], f"runtime-variants.{prefix}_delay_instruction"),
    )
    if (hook.window_size, hook.window_identity, hook.site, hook.target,
        hook.delay_instruction) != (16, digest(digest_hex, f"field5 {prefix} sha"), site, target, delay):
        fail(f"field5 {prefix} hook constraints are not exact")
    return hook


def parse_variant(raw: ManifestValue) -> RuntimeVariant:
    keys = {
        "id", "activation_window_start", "activation_window_size", "activation_window_sha256",
        "activation_site", "activation_target", "activation_delay_instruction",
        "physical_producer_entry", "capture_window_start", "capture_window_size",
        "capture_window_sha256", "capture_site", "capture_target",
        "capture_delay_instruction", "physical_return_site", "native_cutovers",
        "source_sites",
    }
    value = closed(raw, keys, "runtime-variants.variant")
    activation = parse_hook(value, "activation", ACTIVATION_WINDOW_SHA256,
                            0x80075414, 0x800764B4, 0x248400CC)
    capture = parse_hook(value, "capture", CAPTURE_WINDOW_SHA256,
                          0x80075694, 0x8004B54C, 0x34040001)
    source_raw = value["source_sites"]
    if not isinstance(source_raw, list) or len(source_raw) != 14:
        fail("field5 source sites must contain exactly 14 entries")
    source_sites = tuple(parse_source_site(site) for site in source_raw)
    if len({(site.pc & 0x1fffffff, site.instruction) for site in source_sites}) != len(source_sites):
        fail("duplicate normalized field5 source sites are forbidden")
    cutovers_raw = value["native_cutovers"]
    if not isinstance(cutovers_raw, list) or not cutovers_raw:
        fail("field5 native cutovers must be a non-empty array")
    cutovers = tuple(parse_native_cutover(item) for item in cutovers_raw)
    if len({(item.pc & 0x1fffffff, item.instruction) for item in cutovers}) != len(cutovers):
        fail("duplicate normalized field5 native cutovers are forbidden")
    variant = RuntimeVariant(
        text(value["id"], "runtime-variants.variant.id"), activation,
        address(value["physical_producer_entry"], "runtime-variants.variant.physical_producer_entry"),
        capture,
        address(value["physical_return_site"], "runtime-variants.variant.physical_return_site"),
        source_sites, cutovers,
    )
    if (variant.identifier, variant.activation.window_start,
        variant.physical_producer_entry, variant.capture.window_start,
        variant.physical_return_site) != (
            "field5-runtime-v1", 0x8007540C, 0x800764B4, 0x8007568C, 0x8007569C):
        fail("field5 physical chain is not exact")
    return variant


def parse_native_cutover(raw: ManifestValue) -> NativeCutover:
    value = closed(raw, {"pc", "instruction", "transfer", "continuation"},
                   "runtime-variants.native-cutover")
    transfer = text(value["transfer"], "runtime-variants.native-cutover.transfer")
    if transfer not in {"local", "observe", "return"}:
        fail("field5 native cutover transfer is unsupported")
    cutover = NativeCutover(
        address(value["pc"], "runtime-variants.native-cutover.pc"),
        address(value["instruction"], "runtime-variants.native-cutover.instruction"),
        transfer,
        address(value["continuation"], "runtime-variants.native-cutover.continuation"),
    )
    if (transfer == "local") != (cutover.continuation != 0):
        fail("field5 native cutover continuation disagrees with transfer")
    return cutover


def parse_source_site(raw: ManifestValue) -> SourceSite:
    value = closed(raw, {"pc", "instruction", "operation", "width", "auxiliary"},
                   "runtime-variants.source-site")
    operation = text(value["operation"], "runtime-variants.source-site.operation")
    auxiliary = text(value["auxiliary"], "runtime-variants.source-site.auxiliary")
    width = integer(value["width"], "runtime-variants.source-site.width")
    if operation not in {"read", "write", "swc2", "call", "bucket"}:
        fail("field5 source operation is unsupported")
    if auxiliary not in {"effective-address", "none", "result-register"}:
        fail("field5 source auxiliary rule is unsupported")
    if operation == "call" and auxiliary != "none":
        fail("field5 source auxiliary rule does not match operation")
    if operation == "bucket" and auxiliary != "result-register":
        fail("field5 source auxiliary rule does not match operation")
    if auxiliary == "effective-address" and width not in {1, 2, 4}:
        fail("field5 source width is unsupported")
    if auxiliary != "effective-address" and width != 0:
        fail("field5 non-address source must not declare a width")
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


def load_contract(path: Path) -> RuntimeVariantContract:
    try:
        with path.open("rb") as source:
            raw = tomllib.load(source)
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise ManifestError("runtime variants manifest is unreadable") from error
    root = closed(raw, {"schema", "canonical", "artifact", "variants"}, "runtime-variants")
    if root["schema"] != SCHEMA:
        fail("runtime variants schema version is unsupported")
    variants_value = root["variants"]
    if not isinstance(variants_value, list) or not variants_value:
        fail("field5 runtime variants require one descriptor")
    variants = tuple(parse_variant(value) for value in variants_value)
    if len({normalized_signature(variant) for variant in variants}) != len(variants):
        fail("duplicate normalized field5 descriptors are forbidden")
    return RuntimeVariantContract(parse_canonical(root["canonical"]),
                                  parse_artifact(root["artifact"]), variants)
