from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct
import tomllib


SCHEMA = "xg-render-overlay-ranges/v1"
PLAN_SCHEMA = "psxrecomp-source-observation-plan-v5"


class OverlayRangeError(ValueError):
    pass


@dataclass(frozen=True, slots=True)
class RequiredRange:
    start: int
    size: int
    sha256: bytes


@dataclass(frozen=True, slots=True)
class RangeCutover:
    pc: int
    instruction: int
    transfer: str
    continuation: int


@dataclass(frozen=True, slots=True)
class ProducerScope:
    entry: int
    return_pc: int
    writer: int
    opcode: int
    semantic_source: str


@dataclass(frozen=True, slots=True)
class ProducerCallerScope:
    entry: int
    call: int
    finish: int
    semantic_family: str
    semantic_source: str


@dataclass(frozen=True, slots=True)
class OverlayRangeVariant:
    identifier: str
    base_address: int
    required_ranges: tuple[RequiredRange, ...]
    cutovers: tuple[RangeCutover, ...]
    artifact_size: int | None = None
    artifact_sha256: bytes | None = None
    producer_scope: ProducerScope | None = None
    producer_callers: tuple[ProducerCallerScope, ...] = ()


def _closed(value: object, keys: set[str], label: str) -> dict[str, object]:
    if not isinstance(value, dict) or set(value) != keys:
        raise OverlayRangeError(f"{label} must contain exactly {sorted(keys)}")
    return value


def _integer(value: object, label: str, *, positive: bool = False) -> int:
    if isinstance(value, str):
        try:
            parsed = int(value, 0)
        except ValueError as error:
            raise OverlayRangeError(f"{label} is not an integer") from error
    elif isinstance(value, int) and not isinstance(value, bool):
        parsed = value
    else:
        raise OverlayRangeError(f"{label} is not an integer")
    if parsed < 0 or parsed > 0xFFFFFFFF or (positive and parsed == 0):
        raise OverlayRangeError(f"{label} is outside its closed range")
    return parsed


def _digest(value: object, label: str) -> bytes:
    if not isinstance(value, str) or len(value) != 64:
        raise OverlayRangeError(f"{label} must be lowercase SHA-256 hex")
    try:
        decoded = bytes.fromhex(value)
    except ValueError as error:
        raise OverlayRangeError(f"{label} must be lowercase SHA-256 hex") from error
    if value != value.lower() or len(decoded) != 32:
        raise OverlayRangeError(f"{label} must be lowercase SHA-256 hex")
    return decoded


def load_overlay_range_variants(path: Path) -> tuple[OverlayRangeVariant, ...]:
    try:
        raw = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise OverlayRangeError(str(error)) from error
    root = _closed(raw, {"schema", "variants"}, "overlay-ranges")
    if root["schema"] != SCHEMA:
        raise OverlayRangeError("overlay-ranges schema is unsupported")
    raw_variants = root["variants"]
    if not isinstance(raw_variants, list) or not raw_variants:
        raise OverlayRangeError("overlay-ranges variants must be non-empty")
    variants: list[OverlayRangeVariant] = []
    identifiers: set[str] = set()
    for index, raw_variant in enumerate(raw_variants):
        if not isinstance(raw_variant, dict):
            raise OverlayRangeError(f"overlay-ranges.variant[{index}] must be a table")
        allowed = {
            "id", "base_address", "required_ranges", "cutovers",
            "artifact_size", "artifact_sha256", "producer_scope",
            "producer_callers",
        }
        if not {"id", "base_address", "required_ranges", "cutovers"} <= set(raw_variant) or not set(raw_variant) <= allowed:
            raise OverlayRangeError(f"overlay-ranges.variant[{index}] fields are not closed")
        value = raw_variant
        identifier = value["id"]
        if not isinstance(identifier, str) or not identifier or identifier in identifiers:
            raise OverlayRangeError("overlay range variant id is invalid or duplicated")
        identifiers.add(identifier)
        base = _integer(value["base_address"], "variant.base_address")
        raw_ranges = value["required_ranges"]
        raw_cutovers = value["cutovers"]
        if not isinstance(raw_ranges, list) or not raw_ranges or not isinstance(raw_cutovers, list) or not raw_cutovers:
            raise OverlayRangeError("variant ranges and cutovers must be non-empty")
        ranges: list[RequiredRange] = []
        for raw_range in raw_ranges:
            item = _closed(raw_range, {"start", "size", "sha256"}, "required-range")
            start = _integer(item["start"], "required-range.start")
            size = _integer(item["size"], "required-range.size", positive=True)
            if start < base or start + size > 0x100000000 or start & 3 or size & 3:
                raise OverlayRangeError("required range is not aligned inside the artifact")
            ranges.append(RequiredRange(start, size, _digest(item["sha256"], "required-range.sha256")))
        cutovers: list[RangeCutover] = []
        for raw_cutover in raw_cutovers:
            item = _closed(raw_cutover, {"pc", "instruction", "transfer", "continuation"}, "range-cutover")
            transfer = item["transfer"]
            if transfer not in {"local", "observe", "return"}:
                raise OverlayRangeError("range cutover transfer is unsupported")
            cutover = RangeCutover(
                _integer(item["pc"], "range-cutover.pc"),
                _integer(item["instruction"], "range-cutover.instruction"),
                transfer,
                _integer(item["continuation"], "range-cutover.continuation"),
            )
            if (transfer == "local") != (cutover.continuation != 0):
                raise OverlayRangeError("range cutover continuation disagrees with transfer")
            cutovers.append(cutover)
        has_artifact_size = "artifact_size" in value
        has_artifact_sha256 = "artifact_sha256" in value
        if has_artifact_size != has_artifact_sha256:
            raise OverlayRangeError("variant full artifact identity is incomplete")
        artifact_size = (
            _integer(value["artifact_size"], "variant.artifact_size", positive=True)
            if has_artifact_size else None
        )
        artifact_sha256 = (
            _digest(value["artifact_sha256"], "variant.artifact_sha256")
            if has_artifact_sha256 else None
        )
        producer_scope = None
        if "producer_scope" in value:
            scope = _closed(value["producer_scope"], {
                "entry", "return", "writer", "opcode", "semantic_source",
            }, "producer-scope")
            semantic_source = scope["semantic_source"]
            if not isinstance(semantic_source, str):
                raise OverlayRangeError("producer scope semantic source is invalid")
            producer_scope = ProducerScope(
                entry=_integer(scope["entry"], "producer-scope.entry"),
                return_pc=_integer(scope["return"], "producer-scope.return"),
                writer=_integer(scope["writer"], "producer-scope.writer"),
                opcode=_integer(scope["opcode"], "producer-scope.opcode"),
                semantic_source=semantic_source,
            )
            scope_addresses = {
                producer_scope.entry, producer_scope.return_pc,
                producer_scope.writer,
            }
            cutover_pcs = {cutover.pc for cutover in cutovers}
            if (producer_scope.opcode > 0xff or
                    producer_scope.semantic_source != "producer-state" or
                    not all(any(
                        required.start <= address < required.start + required.size
                        for required in ranges
                    ) for address in scope_addresses) or
                    producer_scope.entry not in cutover_pcs or
                    producer_scope.return_pc not in cutover_pcs):
                raise OverlayRangeError("producer scope escapes its authenticated seams")
            if artifact_size is None:
                raise OverlayRangeError("producer scope requires full artifact identity")
        producer_callers: list[ProducerCallerScope] = []
        if "producer_callers" in value:
            raw_callers = value["producer_callers"]
            if producer_scope is None or not isinstance(raw_callers, list) or not raw_callers:
                raise OverlayRangeError("producer callers require a producer scope")
            caller_entries: set[int] = set()
            caller_calls: set[int] = set()
            for raw_caller in raw_callers:
                caller = _closed(raw_caller, {
                    "entry", "call", "finish", "semantic_family",
                    "semantic_source",
                }, "producer-caller")
                semantic_family = caller["semantic_family"]
                semantic_source = caller["semantic_source"]
                if (not isinstance(semantic_family, str) or not semantic_family or
                        semantic_source != "caller-state"):
                    raise OverlayRangeError("producer caller semantic contract is invalid")
                parsed = ProducerCallerScope(
                    entry=_integer(caller["entry"], "producer-caller.entry"),
                    call=_integer(caller["call"], "producer-caller.call"),
                    finish=_integer(caller["finish"], "producer-caller.finish"),
                    semantic_family=semantic_family,
                    semantic_source=semantic_source,
                )
                if (parsed.entry in caller_entries or parsed.call in caller_calls or
                        parsed.call not in cutover_pcs or parsed.finish not in cutover_pcs or
                        not all(any(required.start <= address < required.start + required.size
                                    for required in ranges)
                                for address in (parsed.entry, parsed.call, parsed.finish))):
                    raise OverlayRangeError("producer caller escapes its authenticated seams")
                caller_entries.add(parsed.entry)
                caller_calls.add(parsed.call)
                producer_callers.append(parsed)
        variants.append(OverlayRangeVariant(
            identifier, base, tuple(ranges), tuple(cutovers), artifact_size,
            artifact_sha256, producer_scope, tuple(producer_callers),
        ))
    return tuple(variants)


def source_plan_for_overlay_ranges(
    variants: tuple[OverlayRangeVariant, ...], data: bytes, load_address: int,
) -> str | None:
    lines = [PLAN_SCHEMA]
    seen: set[tuple[int, int]] = set()
    for variant in variants:
        if load_address != variant.base_address:
            continue
        if (variant.artifact_size is not None and
                (len(data) != variant.artifact_size or
                 hashlib.sha256(data).digest() != variant.artifact_sha256)):
            continue
        matched = True
        for required in variant.required_ranges:
            offset = required.start - load_address
            if offset < 0 or offset + required.size > len(data) or hashlib.sha256(
                data[offset:offset + required.size]
            ).digest() != required.sha256:
                matched = False
                break
        if not matched:
            continue
        for cutover in variant.cutovers:
            offset = cutover.pc - load_address
            if offset < 0 or offset + 4 > len(data) or struct.unpack_from("<I", data, offset)[0] != cutover.instruction:
                raise OverlayRangeError("authenticated range cutover instruction mismatch")
            key = (cutover.pc & 0x1FFFFFFF, cutover.instruction)
            if key in seen:
                continue
            seen.add(key)
            lines.append(
                f"cutover {cutover.pc:08X} {cutover.instruction:08X} "
                f"{cutover.transfer} {cutover.continuation:08X}"
            )
    return None if len(lines) == 1 else "\n".join(lines) + "\n"


def merge_source_plans(*plans: str | None) -> str | None:
    lines = [PLAN_SCHEMA]
    seen: set[str] = set()
    for plan in plans:
        if plan is None:
            continue
        parts = plan.splitlines()
        if not parts or parts[0] != PLAN_SCHEMA:
            raise OverlayRangeError("source observation plan schema mismatch")
        for line in parts[1:]:
            if line and line not in seen:
                seen.add(line)
                lines.append(line)
    return None if len(lines) == 1 else "\n".join(lines) + "\n"


def emit_cold_cutover_table(
    variants: tuple[OverlayRangeVariant, ...], output: Path,
) -> None:
    sites = sorted({(cutover.pc & 0x1FFFFFFF, cutover.instruction)
                    for variant in variants for cutover in variant.cutovers
                    if cutover.instruction >> 26 != 0x03 and not (
                        cutover.instruction >> 26 == 0 and
                        (cutover.instruction & 0x3f) == 0x09)})
    lookup_capacity = 1
    while lookup_capacity < len(sites) * 2:
        lookup_capacity <<= 1
    lookup_slots: list[tuple[int, int] | None] = [None] * lookup_capacity
    lookup_mask = lookup_capacity - 1
    for pc, instruction in sites:
        slot = (((pc >> 2) * 0x9E3779B1) ^
                (instruction * 0x85EBCA6B)) & lookup_mask
        while lookup_slots[slot] is not None:
            slot = (slot + 1) & lookup_mask
        lookup_slots[slot] = (pc, instruction)
    lines = [
        "/* Generated from xg_render_overlay_ranges.toml. */",
        "typedef struct XgRenderOverlayCutoverSite {",
        "    uint32_t pc;",
        "    uint32_t instruction;",
        "} XgRenderOverlayCutoverSite;",
        "static const XgRenderOverlayCutoverSite",
        "    xg_render_overlay_cutover_lookup[] = {",
    ]
    lines.extend(
        (f"    {{ UINT32_C(0x{pc:08x}), UINT32_C(0x{instruction:08x}) }},"
         if slot is not None else
         "    { UINT32_C(0xffffffff), UINT32_C(0) },")
        for slot in lookup_slots
        for pc, instruction in ((slot,) if slot is not None else ((0, 0),)))
    lines.extend([
        "};",
        "",
        "static bool xg_render_overlay_cutover_relevant(",
        "    uint32_t pc, uint32_t instruction) {",
        "    const uint32_t normalized = pc & UINT32_C(0x1fffffff);",
        "    const uint32_t mask = (uint32_t)(sizeof(",
        "        xg_render_overlay_cutover_lookup) /",
        "        sizeof(xg_render_overlay_cutover_lookup[0])) - 1u;",
        "    uint32_t slot = (((normalized >> 2u) * UINT32_C(0x9e3779b1)) ^",
        "        (instruction * UINT32_C(0x85ebca6b))) & mask;",
        "    for (uint32_t probe = 0u; probe <= mask; ++probe) {",
        "        const XgRenderOverlayCutoverSite candidate =",
        "            xg_render_overlay_cutover_lookup[slot];",
        "        if (candidate.pc == UINT32_C(0xffffffff)) return false;",
        "        if (candidate.pc == normalized &&",
        "            candidate.instruction == instruction)",
        "            return true;",
        "        slot = (slot + 1u) & mask;",
        "    }",
        "    return false;",
        "}",
        "",
    ])
    lines.extend([
        "#ifdef XG_RENDER_OVERLAY_AUTH_IMPLEMENTATION",
        "typedef struct XgRenderOverlayAuthVariant {",
        "    uint32_t base_address;",
        "    uint32_t artifact_size;",
        "    uint32_t producer_entry;",
        "    uint32_t producer_return;",
        "    uint32_t producer_writer;",
        "    const uint32_t *cutover_pcs;",
        "    uint32_t cutover_count;",
        "} XgRenderOverlayAuthVariant;",
        "",
    ])
    for index, variant in enumerate(variants):
        cutover_pcs = ", ".join(
            f"UINT32_C(0x{cutover.pc:08x})" for cutover in variant.cutovers)
        lines.extend([
            f"static const uint32_t xg_render_overlay_auth_cutovers_{index}[] = {{",
            f"    {cutover_pcs}",
            "};",
            "",
        ])
    lines.append(
        "static const XgRenderOverlayAuthVariant xg_render_overlay_auth_variants[] = {")
    for index, variant in enumerate(variants):
        scope = variant.producer_scope
        lines.append(
            "    { UINT32_C(0x%08x), %du, UINT32_C(0x%08x), "
            "UINT32_C(0x%08x), UINT32_C(0x%08x), "
            "xg_render_overlay_auth_cutovers_%d, %du }," % (
                variant.base_address,
                variant.artifact_size or 0,
                scope.entry if scope is not None else 0,
                scope.return_pc if scope is not None else 0,
                scope.writer if scope is not None else 0,
                index,
                len(variant.cutovers),
            ))
    lines.extend([
        "};",
        "",
        "static bool xg_render_overlay_auth_range_contains(",
        "    uint32_t start, uint32_t size, uint32_t value, uint32_t value_size) {",
        "    const uint64_t range_start = start & UINT32_C(0x1fffffff);",
        "    const uint64_t range_end = range_start + size;",
        "    const uint64_t value_start = value & UINT32_C(0x1fffffff);",
        "    return size != 0u && value_size != 0u &&",
        "           value_start >= range_start &&",
        "           value_start + value_size <= range_end;",
        "}",
        "",
        "static bool xg_render_overlay_auth_candidate_common(",
        "    const PsxXgRenderAuthCandidate *candidate) {",
        "    const uint32_t artifact_base = candidate != NULL",
        "        ? candidate->artifact_base & UINT32_C(0x1fffffff) : 0u;",
        "    return candidate != NULL && candidate->authority_provenance &&",
        "           candidate->pair_bound && candidate->pair_id != 0u &&",
        "           memcmp(candidate->identity.game_sha256,",
        "                  xg_render_game_identity,",
        "                  sizeof(candidate->identity.game_sha256)) == 0 &&",
        "           memcmp(candidate->identity.manifest_sha256,",
        "                  xg_render_manifest_identity,",
        "                  sizeof(candidate->identity.manifest_sha256)) == 0 &&",
        "           artifact_base < UINT32_C(0x00200000) &&",
        "           (artifact_base & UINT32_C(0xfff)) == 0u &&",
        "           candidate->artifact_size != 0u &&",
        "           candidate->artifact_size <= UINT32_C(0x00200000) - artifact_base &&",
        "           candidate->producer_entry != 0u &&",
        "           xg_render_overlay_auth_range_contains(",
        "               candidate->range_start, candidate->range_size,",
        "               candidate->producer_entry, 4u) &&",
        "           xg_render_overlay_auth_range_contains(",
        "               candidate->range_start, candidate->range_size,",
        "               candidate->dispatch_pc, 4u) &&",
        "           xg_render_overlay_auth_range_contains(",
        "               candidate->artifact_base, candidate->artifact_size,",
        "               candidate->range_start, candidate->range_size);",
        "}",
        "",
        "static bool xg_render_overlay_auth_candidate_matches_variant(",
        "    const XgRenderOverlayAuthVariant *variant,",
        "    const PsxXgRenderAuthCandidate *candidate) {",
        "    if ((candidate->artifact_base & UINT32_C(0x1fffffff)) !=",
        "            (variant->base_address & UINT32_C(0x1fffffff)) ||",
        "        (variant->artifact_size != 0u &&",
        "         candidate->artifact_size != variant->artifact_size))",
        "        return false;",
        "    for (uint32_t index = 0u; index < variant->cutover_count; ++index)",
        "        if (xg_render_overlay_auth_range_contains(",
        "                candidate->range_start, candidate->range_size,",
        "                variant->cutover_pcs[index], 4u))",
        "            return true;",
        "    return false;",
        "}",
        "",
        "static bool xg_render_overlay_artifact_candidate_matches(",
        "    const PsxXgRenderAuthCandidate *candidate) {",
        "    if (!xg_render_overlay_auth_candidate_common(candidate)) return false;",
        "    for (uint32_t index = 0u; index <",
        "         sizeof(xg_render_overlay_auth_variants) /",
        "         sizeof(xg_render_overlay_auth_variants[0]); ++index)",
        "        if (xg_render_overlay_auth_candidate_matches_variant(",
        "                &xg_render_overlay_auth_variants[index], candidate))",
        "            return true;",
        "    return false;",
        "}",
        "",
        "static bool xg_render_overlay_artifact_candidate_authorizes_pc(",
        "    const PsxXgRenderAuthCandidate *candidate, uint32_t pc) {",
        "    if (!xg_render_overlay_auth_candidate_common(candidate)) return false;",
        "    for (uint32_t index = 0u; index <",
        "         sizeof(xg_render_overlay_auth_variants) /",
        "         sizeof(xg_render_overlay_auth_variants[0]); ++index) {",
        "        const XgRenderOverlayAuthVariant *variant =",
        "            &xg_render_overlay_auth_variants[index];",
        "        if (!xg_render_overlay_auth_candidate_matches_variant(",
        "                variant, candidate))",
        "            continue;",
        "        for (uint32_t cutover = 0u; cutover < variant->cutover_count;",
        "             ++cutover)",
        "            if ((pc & UINT32_C(0x1fffffff)) ==",
        "                    (variant->cutover_pcs[cutover] & UINT32_C(0x1fffffff)) &&",
        "                xg_render_overlay_auth_range_contains(",
        "                    candidate->range_start, candidate->range_size, pc, 4u))",
        "                return true;",
        "    }",
        "    return false;",
        "}",
        "#endif",
        "",
    ])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines), encoding="ascii")


def _main() -> None:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    emit = subparsers.add_parser("emit-cold-cutover-table")
    emit.add_argument("--manifest", type=Path, required=True)
    emit.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    emit_cold_cutover_table(
        load_overlay_range_variants(arguments.manifest), arguments.output)


if __name__ == "__main__":
    _main()
