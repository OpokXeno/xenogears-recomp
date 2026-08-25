#!/usr/bin/env python3
"""Finalize independent native overlay units and aggregate their dispatch."""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import json
import os
import re
import sys
import tomllib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "psxrecomp" / "tools"))
import compile_overlays  # noqa: E402


UNIT_SCHEMA = "xenogears-native-overlay-unit/v1"
BODY_SHARD_LINE_BUDGET = 40_000


def _write_text_if_changed(path: Path, content: str) -> None:
    encoded = content.encode("utf-8")
    if path.is_file() and path.read_bytes() == encoded:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + f".tmp.{os.getpid()}")
    temporary.write_bytes(encoded)
    temporary.replace(path)


def _one_capture(path: Path) -> dict:
    document = json.loads(path.read_text(encoding="ascii"))
    if (
        not isinstance(document, list)
        or len(document) != 1
        or not isinstance(document[0], dict)
    ):
        raise ValueError(f"{path}: expected one capture record")
    return document[0]


def _identity(game: str, manifest: str):
    return compile_overlays.parse_game_identity(game, manifest)


def _shard_path(prefix: Path, index: int) -> Path:
    return prefix.parent / f"{prefix.name}_{index:02d}.c"


def _write_shards(prefix: Path, sources: list[str], shard_count: int) -> None:
    if shard_count < 1 or len(sources) > shard_count:
        raise ValueError(
            f"{prefix}: {len(sources)} generated shards exceed {shard_count} slots"
        )
    for index in range(shard_count):
        source = (
            sources[index]
            if index < len(sources)
            else "/* Empty generated shard slot. */\n"
        )
        _write_text_if_changed(_shard_path(prefix, index), source)


def split_overlay_body(
    source: str,
    namespace: str,
    shard_count: int,
    line_budget: int = BODY_SHARD_LINE_BUDGET,
) -> list[str]:
    definition_re = re.compile(
        rf"^void {re.escape(namespace)}_(?:func|alias_body)_[0-9A-Fa-f]{{8}}"
        r"\(CPUState\* cpu(?:, uint32_t entry)?\)\n\{",
        re.MULTILINE,
    )
    definitions = list(definition_re.finditer(source))
    if not definitions:
        raise ValueError(f"{namespace}: generated source has no function definitions")

    prefix = source[: definitions[0].start()]
    functions = [
        source[match.start() : definitions[index + 1].start()]
        if index + 1 < len(definitions)
        else source[match.start() :]
        for index, match in enumerate(definitions)
    ]
    buckets: list[list[str]] = []
    bucket: list[str] = []
    bucket_lines = 0
    for function in functions:
        function_lines = function.count("\n")
        if bucket and bucket_lines + function_lines > line_budget:
            buckets.append(bucket)
            bucket = []
            bucket_lines = 0
        bucket.append(function)
        bucket_lines += function_lines
    if bucket:
        buckets.append(bucket)
    if len(buckets) > shard_count:
        raise ValueError(
            f"{namespace}: needs {len(buckets)} body shards but only "
            f"{shard_count} slots were declared"
        )

    return [
        f"/* Generated native overlay body shard {index}. DO NOT EDIT. */\n"
        + prefix
        + "".join(bucket)
        for index, bucket in enumerate(buckets)
    ]


def _normalized_variants(variants: list[dict]) -> list[dict]:
    unique = []
    seen = set()
    for variant in variants:
        ranges = tuple(
            (int(lo) & 0x1FFFFFFF, int(length))
            for lo, length in variant["ranges"]
        )
        resume = int(variant.get("resume", 0)) & 0xFFFFFFFF
        key = (int(variant["addr"]), int(variant["crc"]), ranges, resume)
        if key in seen:
            continue
        seen.add(key)
        unique.append({
            **variant,
            "addr": int(variant["addr"]),
            "crc": int(variant["crc"]),
            "ranges": ranges,
            "resume": resume,
        })
    unique.sort(
        key=lambda variant: (
            variant["addr"],
            variant["crc"],
            variant["ranges"],
            variant["resume"],
            variant["symbol"],
        )
    )
    return unique


def _dispatch_shard_source(index: int, variants: list[dict]) -> str:
    by_address: dict[int, list[dict]] = {}
    for variant in variants:
        by_address.setdefault(variant["addr"], []).append(variant)
    range_sets = sorted({variant["ranges"] for variant in variants})
    range_symbols = {
        ranges: f"psx_ov_dispatch_{index:02d}_ranges_{range_index:04d}"
        for range_index, ranges in enumerate(range_sets)
    }
    lines = [
        f"/* Generated native overlay dispatch shard {index}. DO NOT EDIT. */",
        '#include "psx_runtime.h"',
        "",
        "extern int psx_overlay_static_code_matches(const uint32_t *lo_len_pairs,",
        "                                           uint32_t count,",
        "                                           uint32_t expected_crc);",
        "extern uint64_t psx_ov_static_checks;",
        "extern uint64_t psx_ov_static_hits;",
        "extern uint64_t psx_ov_static_variant_misses;",
        "extern uint64_t psx_ov_static_address_misses;",
        "",
    ]
    for symbol in sorted({variant["symbol"] for variant in variants}):
        lines.append(f"extern void {symbol}(CPUState *cpu);")
    lines.append("")
    for ranges in range_sets:
        flat = [
            value
            for lo, length in ranges
            for value in (f"0x{lo:08X}u", f"0x{length:X}u")
        ]
        lines.append(
            f"static const uint32_t {range_symbols[ranges]}[] = "
            "{ " + ", ".join(flat) + " };"
        )
    lines += [
        "",
        f"int psx_overlay_dispatch_shard_{index:02d}(CPUState *cpu, uint32_t key) {{",
        "    switch (key) {",
    ]
    for address, address_variants in sorted(by_address.items()):
        lines.append(f"        case 0x{address:08X}u:")
        for variant in address_variants:
            lines += [
                "            psx_ov_static_checks++;",
                "            if (psx_overlay_static_code_matches("
                f"{range_symbols[variant['ranges']]}, "
                f"{len(variant['ranges'])}u, 0x{variant['crc']:08X}u)) {{",
                "                psx_ov_static_hits++;",
            ]
            if variant["resume"]:
                lines.append(
                    f"                cpu->pc = 0x{variant['resume']:08X}u;"
                )
            lines += [
                f"                {variant['symbol']}(cpu);",
                "                return 1;",
                "            }",
                "            psx_ov_static_variant_misses++;",
            ]
        lines.append("            return 0;")
    lines += [
        "        default:",
        "            psx_ov_static_address_misses++;",
        "            return 0;",
        "    }",
        "}",
        "",
    ]
    return "\n".join(lines)


def generate_dispatch_shards(
    variants: list[dict], identity, images: list[dict], shard_count: int
) -> tuple[str, list[str]]:
    unique = _normalized_variants(variants)
    addresses = sorted({variant["addr"] for variant in unique})
    if not addresses or shard_count < 1:
        raise ValueError("native overlay dispatch requires entries and shard slots")
    active_count = min(shard_count, len(addresses))
    addresses_per_shard = (len(addresses) + active_count - 1) // active_count
    address_groups = [
        addresses[index : index + addresses_per_shard]
        for index in range(0, len(addresses), addresses_per_shard)
    ]
    variants_by_address: dict[int, list[dict]] = {}
    for variant in unique:
        variants_by_address.setdefault(variant["addr"], []).append(variant)
    shard_variants = [
        [variant for address in group for variant in variants_by_address[address]]
        for group in address_groups
    ]
    shard_sources = [
        _dispatch_shard_source(index, group)
        for index, group in enumerate(shard_variants)
    ]

    unique_images = []
    seen_images = set()
    for image in images:
        ranges = tuple(
            (int(lo) & 0x1FFFFFFF, int(length))
            for lo, length in image["ranges"]
        )
        key = (
            int(image["load_addr"]) & 0x1FFFFFFF,
            int(image["size"]),
            int(image["crc"]),
            ranges,
        )
        if key in seen_images:
            continue
        seen_images.add(key)
        unique_images.append({
            **image,
            "load_addr": key[0],
            "size": key[1],
            "crc": key[2],
            "ranges": ranges,
        })
    unique_images.sort(
        key=lambda image: (
            image["load_addr"], image["size"], image["crc"], image["ranges"]
        )
    )

    game_identity = ", ".join(f"0x{value:02X}u" for value in identity.game_sha256)
    manifest_identity = ", ".join(
        f"0x{value:02X}u" for value in identity.manifest_sha256
    )
    lines = [
        compile_overlays.static_identity_metadata(identity).rstrip(),
        "/* Generated native overlay dispatch router. DO NOT EDIT. */",
        '#include "psx_runtime.h"',
        '#include "game_identity.h"',
        "",
        "static const PsxGameIdentity k_psx_overlay_static_identity = {",
        f"    {{{game_identity}}},",
        f"    {{{manifest_identity}}}",
        "};",
        "extern int psx_overlay_static_code_matches(const uint32_t *lo_len_pairs,",
        "                                           uint32_t count,",
        "                                           uint32_t expected_crc);",
        "uint64_t psx_ov_static_checks = 0;",
        "uint64_t psx_ov_static_hits = 0;",
        "uint64_t psx_ov_static_variant_misses = 0;",
        "uint64_t psx_ov_static_address_misses = 0;",
        "static uint64_t psx_ov_static_image_checks = 0;",
        "static uint64_t psx_ov_static_image_hits = 0;",
        "static uint64_t psx_ov_static_image_misses = 0;",
        "",
    ]
    for index in range(len(shard_sources)):
        lines.append(
            f"extern int psx_overlay_dispatch_shard_{index:02d}("
            "CPUState *cpu, uint32_t key);"
        )
    lines.append("")
    for index, image in enumerate(unique_images):
        flat = [
            value
            for lo, length in image["ranges"]
            for value in (f"0x{lo:08X}u", f"0x{length:X}u")
        ]
        image["range_symbol"] = f"psx_ov_static_image_ranges_{index:03d}"
        lines.append(
            f"static const uint32_t {image['range_symbol']}[] = "
            "{ " + ", ".join(flat) + " };"
        )
    lines += [
        "",
        "static int psx_ov_static_ranges_contain(const uint32_t *ranges,",
        "                                         uint32_t count,",
        "                                         uint32_t addr) {",
        "    for (uint32_t i = 0; i < count; i++) {",
        "        uint32_t lo = ranges[i * 2u];",
        "        uint32_t len = ranges[i * 2u + 1u];",
        "        if (addr >= lo && addr - lo < len) return 1;",
        "    }",
        "    return 0;",
        "}",
        "",
        "void psx_overlay_static_get_stats(uint64_t *checks, uint64_t *hits,",
        "                                  uint64_t *variant_misses,",
        "                                  uint64_t *address_misses) {",
        "    if (checks) *checks = psx_ov_static_checks;",
        "    if (hits) *hits = psx_ov_static_hits;",
        "    if (variant_misses) *variant_misses = psx_ov_static_variant_misses;",
        "    if (address_misses) *address_misses = psx_ov_static_address_misses;",
        "}",
        "",
        "void psx_overlay_static_image_get_stats(uint64_t *checks,",
        "                                        uint64_t *hits,",
        "                                        uint64_t *misses) {",
        "    if (checks) *checks = psx_ov_static_image_checks;",
        "    if (hits) *hits = psx_ov_static_image_hits;",
        "    if (misses) *misses = psx_ov_static_image_misses;",
        "}",
        "",
        "int psx_overlay_static_image_known(uint32_t addr) {",
        "    if (!psx_game_identity_bind_static(&k_psx_overlay_static_identity) ||",
        "        !psx_game_identity_gate(&k_psx_overlay_static_identity)) return 0;",
        "    const uint32_t key = addr & 0x1FFFFFFFu;",
    ]
    for image in unique_images:
        lines += [
            f"    if (psx_ov_static_ranges_contain({image['range_symbol']}, "
            f"{len(image['ranges'])}u, key)) {{",
            "        psx_ov_static_image_checks++;",
            f"        if (psx_overlay_static_code_matches({image['range_symbol']}, "
            f"{len(image['ranges'])}u, 0x{image['crc']:08X}u)) {{",
            "            psx_ov_static_image_hits++;",
            "            return 1;",
            "        }",
            "        psx_ov_static_image_misses++;",
            "    }",
        ]
    lines += [
        "    return 0;",
        "}",
        "",
        "int psx_overlay_dispatch(CPUState *cpu, uint32_t addr) {",
        "    if (!psx_game_identity_bind_static(&k_psx_overlay_static_identity) ||",
        "        !psx_game_identity_gate(&k_psx_overlay_static_identity)) return 0;",
        "    const uint32_t key = (addr & 0x1FFFFFFFu) | 0x80000000u;",
    ]
    for index, group in enumerate(address_groups):
        lines.append(
            f"    if (key <= 0x{group[-1]:08X}u) "
            f"return psx_overlay_dispatch_shard_{index:02d}(cpu, key);"
        )
    lines += [
        "    psx_ov_static_address_misses++;",
        "    return 0;",
        "}",
        "",
    ]
    return "\n".join(lines), shard_sources


def finalize_unit(args: argparse.Namespace) -> None:
    capture = _one_capture(args.capture)
    data = base64.b64decode(capture["bytes_b64"], validate=True)
    size = int(capture["size"])
    load_address = int(capture["load_addr"], 0)
    image_id = capture.get("image_id")
    if not isinstance(image_id, str) or not image_id:
        raise ValueError("capture has no image_id")
    if len(data) != size or hashlib.sha256(data).hexdigest() != args.input_sha256:
        raise ValueError(f"{image_id}: capture/raw identity mismatch")

    identity = _identity(args.game_identity_sha256, args.manifest_identity_sha256)
    provenance = json.loads(args.provenance.read_text(encoding="utf-8"))
    expected_provenance = {
        "schema": "psxrecomp-input-provenance-v1",
        "format": "raw",
        "sha256": args.input_sha256,
        "load_address": f"0x{load_address:08X}",
        "size": size,
        "entry_pc": args.entry_pc,
        "discovery": args.discovery,
        "game_identity_sha256": identity.game_sha256.hex(),
        "manifest_identity_sha256": identity.manifest_sha256.hex(),
    }
    if provenance != expected_provenance:
        raise ValueError(f"{image_id}: native input provenance mismatch")

    source = args.generated_c.read_text(encoding="utf-8")
    source, function_addresses = compile_overlays.patch_generated_c_static(
        source, load_address, size
    )
    continuation_owners = compile_overlays.parse_cps_continuation_owners(source)
    game_document = tomllib.loads(args.game_toml.read_text(encoding="utf-8-sig"))
    whole_crc = binascii.crc32(data) & 0xFFFFFFFF
    audit = compile_overlays.audit_generated_c(
        source, load_address, size, whole_crc, game_document
    )
    if audit["unknown_bad"] or audit["unsupported_todo_addrs"]:
        raise ValueError(
            f"{image_id}: generated-C audit failed with "
            f"{len(audit['unknown_bad'])} unknown targets and "
            f"{len(audit['unsupported_todo_addrs'])} unsupported instructions"
        )

    function_identities = compile_overlays.parse_overlay_func_ids(
        str(args.ranges), data, load_address, size
    )
    identities_by_address: dict[int, list[tuple[int, list[tuple[int, int]]]]] = {}
    for entry, crc, ranges in function_identities:
        identities_by_address.setdefault(entry, []).append((crc, ranges))
    missing_ranges = sorted(set(function_addresses) - set(identities_by_address))
    if missing_ranges:
        sample = ", ".join(f"0x{entry:08X}" for entry in missing_ranges[:8])
        raise ValueError(
            f"{image_id}: {len(missing_ranges)} functions lack exact ranges: {sample}"
        )

    safe_image_id = re.sub(r"[^A-Za-z0-9_]", "_", image_id)
    namespace = f"ov_{safe_image_id}_{args.input_sha256[:12]}"
    source, symbols = compile_overlays.namespace_generated_static(
        source, namespace, function_addresses
    )
    variants = []
    for entry in sorted(function_addresses):
        for crc, ranges in identities_by_address[entry]:
            variants.append({
                "addr": entry,
                "symbol": symbols[entry],
                "crc": crc,
                "ranges": ranges,
                "resume": 0,
            })

    # A continuation re-enters its owning function after the dispatcher sets
    # cpu->pc. This avoids generated wrappers and secondary recompiler passes.
    for entry, host in sorted(continuation_owners.items()):
        if entry in function_addresses or host not in identities_by_address:
            continue
        for crc, ranges in identities_by_address[host]:
            variants.append({
                "addr": entry,
                "symbol": symbols[host],
                "crc": crc,
                "ranges": ranges,
                "resume": entry,
            })

    available = {variant["addr"] for variant in variants}
    requested = {
        (int(entry, 0) & 0x1FFFFFFF) | 0x80000000
        for entry in capture.get("dispatch_entry_pcs", [])
    }
    unresolved = sorted(requested - available)
    if unresolved:
        sample = ", ".join(f"0x{entry:08X}" for entry in unresolved[:12])
        raise ValueError(
            f"{image_id}: {len(unresolved)} authenticated dispatch roots are not native: {sample}"
        )

    image = compile_overlays.static_image_identity(
        function_identities, data, load_address, size, image_id
    )
    metadata = {
        "schema": UNIT_SCHEMA,
        "image_id": image_id,
        "input_sha256": args.input_sha256,
        "discovery": args.discovery,
        "game_identity_sha256": identity.game_sha256.hex(),
        "manifest_identity_sha256": identity.manifest_sha256.hex(),
        "namespace": namespace,
        "body_shard_count": args.body_shard_count,
        "image": {
            "image_id": image_id,
            "load_addr": image["load_addr"],
            "size": image["size"],
            "crc": image["crc"],
            "ranges": image["ranges"],
            "chunks": image["chunks"],
        },
        "variants": variants,
    }
    body_shards = split_overlay_body(
        source, namespace, args.body_shard_count
    )
    _write_shards(args.body_out_prefix, body_shards, args.body_shard_count)
    _write_text_if_changed(
        args.metadata_out,
        json.dumps(metadata, sort_keys=True, separators=(",", ":")) + "\n",
    )
    print(
        f"{image_id}: finalized {len(function_addresses)} functions, "
        f"{len(variants)} exact native entries in {len(body_shards)} body shards"
    )


def aggregate(args: argparse.Namespace) -> None:
    identity = _identity(args.game_identity_sha256, args.manifest_identity_sha256)
    expected_game = identity.game_sha256.hex()
    expected_manifest = identity.manifest_sha256.hex()
    units = []
    seen_images = set()
    for path in args.unit:
        unit = json.loads(path.read_text(encoding="utf-8"))
        image_id = unit.get("image_id")
        if (
            unit.get("schema") != UNIT_SCHEMA
            or unit.get("game_identity_sha256") != expected_game
            or unit.get("manifest_identity_sha256") != expected_manifest
            or not isinstance(image_id, str)
            or image_id in seen_images
            or not isinstance(unit.get("variants"), list)
            or not isinstance(unit.get("image"), dict)
        ):
            raise ValueError(f"{path}: invalid or duplicate native overlay unit")
        seen_images.add(image_id)
        units.append(unit)
    if not units:
        raise ValueError("no native overlay units were supplied")

    variants = [variant for unit in units for variant in unit["variants"]]
    images = [unit["image"] for unit in units]
    dispatch, dispatch_shards = generate_dispatch_shards(
        variants, identity, images, args.dispatch_shard_count
    )
    coverage = compile_overlays.static_coverage_document(images, identity)
    _write_text_if_changed(args.dispatch_main_out, dispatch)
    _write_shards(
        args.dispatch_shard_prefix,
        dispatch_shards,
        args.dispatch_shard_count,
    )
    _write_text_if_changed(
        args.coverage_out,
        json.dumps(coverage, sort_keys=True, separators=(",", ":")) + "\n",
    )
    print(
        f"Aggregated {len(units)} native overlays with "
        f"{len(variants)} exact dispatch identities in "
        f"{len(dispatch_shards)} dispatch shards"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    unit = subparsers.add_parser("unit")
    unit.add_argument("--capture", required=True, type=Path)
    unit.add_argument("--generated-c", required=True, type=Path)
    unit.add_argument("--ranges", required=True, type=Path)
    unit.add_argument("--provenance", required=True, type=Path)
    unit.add_argument("--game-toml", required=True, type=Path)
    unit.add_argument("--input-sha256", required=True)
    unit.add_argument("--entry-pc", required=True)
    unit.add_argument(
        "--discovery", choices=("whole-image", "reachable"), required=True
    )
    unit.add_argument("--game-identity-sha256", required=True)
    unit.add_argument("--manifest-identity-sha256", required=True)
    unit.add_argument("--body-out-prefix", required=True, type=Path)
    unit.add_argument("--body-shard-count", required=True, type=int)
    unit.add_argument("--metadata-out", required=True, type=Path)
    unit.set_defaults(run=finalize_unit)

    aggregate_parser = subparsers.add_parser("aggregate")
    aggregate_parser.add_argument("--unit", action="append", required=True, type=Path)
    aggregate_parser.add_argument("--game-identity-sha256", required=True)
    aggregate_parser.add_argument("--manifest-identity-sha256", required=True)
    aggregate_parser.add_argument("--dispatch-main-out", required=True, type=Path)
    aggregate_parser.add_argument("--dispatch-shard-prefix", required=True, type=Path)
    aggregate_parser.add_argument("--dispatch-shard-count", required=True, type=int)
    aggregate_parser.add_argument("--coverage-out", required=True, type=Path)
    aggregate_parser.set_defaults(run=aggregate)

    args = parser.parse_args()
    args.run(args)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError, binascii.Error) as error:
        raise SystemExit(f"FATAL: {error}") from error
