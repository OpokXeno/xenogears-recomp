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
STATIC_AUTHORITY_PROVENANCE = "authenticated-static-image-v1"
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


def _range_sha256(data: bytes, load_address: int,
                  ranges: list[tuple[int, int]]) -> str:
    base = load_address & 0x1FFFFFFF
    digest = hashlib.sha256()
    for address, size in ranges:
        offset = (address & 0x1FFFFFFF) - base
        if offset < 0 or size <= 0 or offset + size > len(data):
            raise ValueError("static overlay code range escapes its artifact")
        digest.update(data[offset:offset + size])
    return digest.hexdigest()


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
        artifact_base = int(variant["artifact_base"]) & 0x1FFFFFFF
        artifact_size = int(variant["artifact_size"])
        code_sha256 = str(variant["code_sha256"]).lower()
        artifact_sha256 = str(variant["artifact_sha256"]).lower()
        capability_id = int(variant["capability_id"])
        producer_entry = int(variant["producer_entry"]) & 0xFFFFFFFF
        if (
            not ranges
            or artifact_size < 4
            or artifact_base >= 0x800000
            or artifact_size > 0x800000 - artifact_base
            or capability_id <= 0
            or capability_id > 0xFFFFFFFFFFFFFFFF
            or re.fullmatch(r"[0-9a-f]{64}", code_sha256) is None
            or re.fullmatch(r"[0-9a-f]{64}", artifact_sha256) is None
            or variant.get("authority_provenance")
            != STATIC_AUTHORITY_PROVENANCE
        ):
            raise ValueError("static overlay variant has invalid artifact authority")
        artifact_end = artifact_base + artifact_size
        producer_phys = producer_entry & 0x1FFFFFFF
        address_phys = int(variant["addr"]) & 0x1FFFFFFF
        if (
            producer_phys < artifact_base
            or producer_phys + 4 > artifact_end
            or address_phys < artifact_base
            or address_phys + 4 > artifact_end
            or any(
                length <= 0
                or lo < artifact_base
                or lo + length > artifact_end
                for lo, length in ranges
            )
        ):
            raise ValueError("static overlay variant escapes its exact artifact")
        resume = int(variant.get("resume", 0)) & 0xFFFFFFFF
        key = (
            int(variant["addr"]),
            code_sha256,
            ranges,
            resume,
            variant["symbol"],
            producer_entry,
            artifact_base,
            artifact_size,
            artifact_sha256,
            capability_id,
        )
        if key in seen:
            continue
        seen.add(key)
        unique.append({
            **variant,
            "addr": int(variant["addr"]),
            "code_sha256": code_sha256,
            "ranges": ranges,
            "resume": resume,
            "producer_entry": producer_entry,
            "artifact_base": artifact_base,
            "artifact_size": artifact_size,
            "artifact_sha256": artifact_sha256,
            "capability_id": capability_id,
        })
    unique.sort(
        key=lambda variant: (
            variant["addr"],
            variant["code_sha256"],
            variant["ranges"],
            variant["resume"],
            variant["symbol"],
            variant["producer_entry"],
            variant["artifact_base"],
            variant["artifact_size"],
            variant["artifact_sha256"],
            variant["capability_id"],
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
    code_identities = sorted({
        (variant["ranges"], variant["code_sha256"])
        for variant in variants
    })
    code_identity_symbols = {
        identity: f"psx_ov_dispatch_{index:02d}_code_sha256_{identity_index:04d}"
        for identity_index, identity in enumerate(code_identities)
    }
    artifact_ranges = sorted({
        (variant["artifact_base"], variant["artifact_size"])
        for variant in variants
    })
    artifact_range_symbols = {
        artifact: f"psx_ov_dispatch_{index:02d}_artifact_{artifact_index:04d}"
        for artifact_index, artifact in enumerate(artifact_ranges)
    }
    artifact_identities = sorted({variant["artifact_sha256"] for variant in variants})
    artifact_identity_symbols = {
        identity: f"psx_ov_dispatch_{index:02d}_artifact_sha256_{identity_index:04d}"
        for identity_index, identity in enumerate(artifact_identities)
    }
    lines = [
        f"/* Generated native overlay dispatch shard {index}. DO NOT EDIT. */",
        '#include "psx_runtime.h"',
        '#include "overlay_loader.h"',
        "",
        "extern int psx_overlay_static_code_matches(const uint32_t *lo_len_pairs,",
        "                                           uint32_t count,",
        "                                           const uint8_t expected_sha256[32]);",
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
    for identity in code_identities:
        digest = identity[1]
        initializer = ", ".join(f"0x{value:02x}u" for value in bytes.fromhex(digest))
        lines.append(
            f"static const uint8_t {code_identity_symbols[identity]}[32] = "
            "{ " + initializer + " };"
        )
    for artifact in artifact_ranges:
        lines.append(
            f"static const uint32_t {artifact_range_symbols[artifact]}[] = "
            f"{{ 0x{artifact[0]:08X}u, 0x{artifact[1]:X}u }};"
        )
    for identity in artifact_identities:
        initializer = ", ".join(f"0x{value:02x}u" for value in bytes.fromhex(identity))
        lines.append(
            f"static const uint8_t {artifact_identity_symbols[identity]}[32] = "
            "{ " + initializer + " };"
        )
    lines += [
        "",
        f"int psx_overlay_dispatch_shard_{index:02d}(CPUState *cpu, uint32_t key,",
        "                                         uint32_t dispatch_pc,",
        "                                         const PsxGameIdentity *identity) {",
        "    switch (key) {",
    ]
    for address, address_variants in sorted(by_address.items()):
        lines += [
            f"        case 0x{address:08X}u: {{",
            "            uint32_t selected = 0u;",
        ]
        for selection, variant in enumerate(address_variants, 1):
            code_identity = (variant["ranges"], variant["code_sha256"])
            lines += [
                "            psx_ov_static_checks++;",
                "            if (psx_overlay_static_code_matches("
                f"{range_symbols[variant['ranges']]}, "
                f"{len(variant['ranges'])}u, "
                f"{code_identity_symbols[code_identity]})) {{",
                "                if (selected != 0u) {",
                "                    psx_ov_static_variant_misses++;",
                "                    return 0;",
                "                }",
                f"                selected = {selection}u;",
                "            } else {",
                "                psx_ov_static_variant_misses++;",
                "            }",
            ]
        lines.append("            switch (selected) {")
        for selection, variant in enumerate(address_variants, 1):
            artifact = (variant["artifact_base"], variant["artifact_size"])
            code_identity = (variant["ranges"], variant["code_sha256"])
            lines += [
                f"            case {selection}u:",
                "                if (!psx_overlay_static_note_candidate_dispatch(",
                f"                        {range_symbols[variant['ranges']]}, "
                f"{len(variant['ranges'])}u, {code_identity_symbols[code_identity]},",
                f"                        {artifact_range_symbols[artifact]}, 1u, "
                f"{artifact_identity_symbols[variant['artifact_sha256']]},",
                "                        identity, "
                f"UINT64_C(0x{variant['capability_id']:016X}),",
                f"                        0x{variant['producer_entry']:08X}u, "
                "dispatch_pc))",
                "                    return 0;",
                "                psx_ov_static_hits++;",
            ]
            if variant["resume"]:
                lines.append(
                    f"                cpu->pc = 0x{variant['resume']:08X}u;"
                )
            lines += [
                f"                {variant['symbol']}(cpu);",
                "                return 1;",
            ]
        lines += [
            "            default:",
            "                return 0;",
            "            }",
            "        }",
        ]
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
        exact_artifact_ranges = tuple(
            (int(lo) & 0x1FFFFFFF, int(length))
            for lo, length in image.get("artifact_ranges", ())
        )
        key = (
            int(image["load_addr"]) & 0x1FFFFFFF,
            int(image["size"]),
            str(image["code_sha256"]).lower(),
            str(image["artifact_sha256"]).lower(),
            int(image["capability_id"]),
            ranges,
        )
        if (
            key[1] < 4
            or key[0] >= 0x800000
            or key[1] > 0x800000 - key[0]
            or re.fullmatch(r"[0-9a-f]{64}", key[2]) is None
            or re.fullmatch(r"[0-9a-f]{64}", key[3]) is None
            or key[4] <= 0
            or key[4] > 0xFFFFFFFFFFFFFFFF
            or image.get("authority_provenance")
            != STATIC_AUTHORITY_PROVENANCE
            or exact_artifact_ranges != ((key[0], key[1]),)
        ):
            raise ValueError("static overlay image has invalid artifact authority")
        if key in seen_images:
            continue
        seen_images.add(key)
        unique_images.append({
            **image,
            "load_addr": key[0],
            "size": key[1],
            "code_sha256": key[2],
            "artifact_sha256": key[3],
            "capability_id": key[4],
            "ranges": ranges,
        })
    unique_images.sort(
        key=lambda image: (
            image["load_addr"], image["size"], image["artifact_sha256"],
            image["capability_id"], image["code_sha256"], image["ranges"]
        )
    )
    known_code_identities = sorted({
        (variant["ranges"], variant["code_sha256"])
        for variant in unique
    })
    known_code_union = compile_overlays.merge_code_ranges(
        ranges for ranges, _digest in known_code_identities
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
        '#include <string.h>',
        "",
        "static const PsxGameIdentity k_psx_overlay_static_identity = {",
        f"    {{{game_identity}}},",
        f"    {{{manifest_identity}}}",
        "};",
        "extern int psx_overlay_static_code_matches(const uint32_t *lo_len_pairs,",
        "                                           uint32_t count,",
        "                                           const uint8_t expected_sha256[32]);",
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
            "CPUState *cpu, uint32_t key, uint32_t dispatch_pc, "
            "const PsxGameIdentity *identity);"
        )
    lines.append("")
    for index, (ranges, digest) in enumerate(known_code_identities):
        flat = [
            value
            for lo, length in ranges
            for value in (f"0x{lo:08X}u", f"0x{length:X}u")
        ]
        lines.append(
            f"static const uint32_t psx_ov_static_code_ranges_{index:03d}[] = "
            "{ " + ", ".join(flat) + " };"
        )
        initializer = ", ".join(
            f"0x{value:02x}u" for value in bytes.fromhex(digest)
        )
        lines.append(
            f"static const uint8_t psx_ov_static_code_sha256_{index:03d}[32] = "
            "{ " + initializer + " };"
        )
    flat_union = [
        value
        for lo, length in known_code_union
        for value in (f"0x{lo:08X}u", f"0x{length:X}u")
    ]
    lines.append(
        "static const uint32_t psx_ov_static_code_union[] = { "
        + ", ".join(flat_union) + " };"
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
        "    if (!psx_ov_static_ranges_contain(psx_ov_static_code_union, "
        f"{len(known_code_union)}u, key)) return 0;",
    ]
    for index, (ranges, _digest) in enumerate(known_code_identities):
        lines += [
            f"    if (psx_ov_static_ranges_contain(psx_ov_static_code_ranges_{index:03d}, "
            f"{len(ranges)}u, key)) {{",
            "        psx_ov_static_image_checks++;",
            f"        if (psx_overlay_static_code_matches("
            f"psx_ov_static_code_ranges_{index:03d}, {len(ranges)}u, "
            f"psx_ov_static_code_sha256_{index:03d})) {{",
            "            psx_ov_static_image_hits++;",
            "            return 1;",
            "        }",
            "        psx_ov_static_image_misses++;",
            "    }",
        ]
    lines += ["    return 0;", "}", ""]
    lines += [
        "int psx_overlay_static_artifact_code_write_overlaps(const uint8_t sha256[32],",
        "        uint32_t base, uint32_t size, uint32_t address, uint32_t write_size) {",
        "    const uint64_t begin = address & 0x1FFFFFFFu;",
        "    const uint64_t end = begin + write_size;",
        "    base &= 0x1FFFFFFFu;",
    ]
    for image in unique_images:
        digest = ", ".join(f"0x{v:02x}u" for v in bytes.fromhex(image["artifact_sha256"]))
        flat = ", ".join(f"0x{v:X}u" for pair in image["ranges"] for v in pair)
        lines += [
            "    {",
            f"        static const uint8_t digest[32] = {{{digest}}};",
            f"        static const uint32_t ranges[] = {{{flat}}};",
            f"        if (base == 0x{image['load_addr'] & 0x1fffffff:X}u && size == 0x{image['size']:X}u &&",
            "            memcmp(sha256, digest, sizeof(digest)) == 0) {",
            "            for (uint32_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i += 2u) {",
            "                const uint64_t lo = ranges[i] & 0x1FFFFFFFu;",
            "                if (write_size && begin < lo + ranges[i + 1u] && lo < end) return 1;",
            "            }",
            "            return 0;",
            "        }",
            "    }",
        ]
    lines += [
        "    return -1;",
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
            f"return psx_overlay_dispatch_shard_{index:02d}("
            f"cpu, key, addr, &k_psx_overlay_static_identity);"
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
    whole_sha256 = hashlib.sha256(data).hexdigest()
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
    capability_id = compile_overlays.overlay_pair_id(
        source,
        function_identities,
        identity=identity,
        artifact=(load_address, size, whole_sha256),
    )
    if capability_id == 0:
        raise ValueError(f"{image_id}: static artifact capability id is zero")
    variants = []
    for entry in sorted(function_addresses):
        for crc, ranges in identities_by_address[entry]:
            variants.append({
                "addr": entry,
                "symbol": symbols[entry],
                "code_sha256": _range_sha256(data, load_address, ranges),
                "ranges": ranges,
                "resume": 0,
                "producer_entry": entry,
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
                "code_sha256": _range_sha256(data, load_address, ranges),
                "ranges": ranges,
                "resume": entry,
                "producer_entry": host,
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
    image.update({
        "artifact_sha256": whole_sha256,
        "artifact_ranges": [(load_address & 0x1FFFFFFF, size)],
        "capability_id": capability_id,
        "authority_provenance": STATIC_AUTHORITY_PROVENANCE,
    })
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
            "code_sha256": image["code_sha256"],
            "artifact_sha256": image["artifact_sha256"],
            "artifact_ranges": image["artifact_ranges"],
            "capability_id": image["capability_id"],
            "authority_provenance": image["authority_provenance"],
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
        image = unit["image"]
        if (
            image.get("image_id") != image_id
            or image.get("authority_provenance")
            != STATIC_AUTHORITY_PROVENANCE
            or not isinstance(image.get("capability_id"), int)
            or image["capability_id"] <= 0
            or image["capability_id"] > 0xFFFFFFFFFFFFFFFF
            or not isinstance(image.get("artifact_sha256"), str)
            or re.fullmatch(r"[0-9a-f]{64}", image["artifact_sha256"]) is None
            or not isinstance(image.get("artifact_ranges"), list)
        ):
            raise ValueError(f"{path}: invalid static artifact authority metadata")
        seen_images.add(image_id)
        units.append(unit)
    if not units:
        raise ValueError("no native overlay units were supplied")

    variants = []
    for unit in units:
        image = unit["image"]
        for variant in unit["variants"]:
            variants.append({
                **variant,
                "artifact_base": image["load_addr"],
                "artifact_size": image["size"],
                "artifact_sha256": image["artifact_sha256"],
                "capability_id": image["capability_id"],
                "authority_provenance": image["authority_provenance"],
            })
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
