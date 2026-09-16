#!/usr/bin/env python3
"""Decompile, compile, assemble and repack Xenogears Field scripts (Python 3.11+)."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

from compile_field_scripts import Record, compile_xgs, disassemble, parse_assembly
from decompile_field_scripts import address_operand_offsets, decode_instruction
from editable_field_scripts import equivalent_scripts, group_source_by_entity, render_source, restore_source_comments
from extract_disc_field_scripts import parse_scripts_file, write_if_changed
from repack_field_scripts import lzss_compress, read_field_container, repack_container, write_override


def _resource_digest(resource: dict) -> tuple[str, object]:
    """Return (expected_hex_digest, hashlib_constructor).

    New corpora store ``sha1``; pre-change corpora stored ``sha256``.
    """
    if "sha1" in resource:
        return resource["sha1"], hashlib.sha1
    return resource["sha256"], hashlib.sha256


def _catalog_digest(entry: dict) -> str:
    return entry.get("sha1", entry.get("sha256"))


def verify_corpus(root: Path, *, repack: bool = False, resize: bool = False) -> dict:
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    count = total = bytecode_bytes = 0
    compressed_bytes = 0
    resized_count = resized_bytes = 0
    source_resized_count = 0
    for resource in manifest["resources"]:
        path = root / resource["asset_path"]
        try:
            scripts = path.read_bytes()
            expected, hasher = _resource_digest(resource)
            if hasher(scripts).hexdigest() != expected:
                raise ValueError("asset does not match its manifest digest")
            metadata = parse_scripts_file(scripts)
            occurrence = resource["occurrences"][0]
            field_id = occurrence["field_id"]
            xga = disassemble(scripts).text()
            if parse_assembly(xga).build() != scripts:
                raise ValueError("assembly round-trip differs")
            source, _ = render_source(field_id, scripts, metadata)
            compiled = compile_xgs(source).build()
            if not equivalent_scripts(compiled, scripts):
                first = next((i for i, (left, right) in enumerate(zip(compiled, scripts)) if left != right), min(len(compiled), len(scripts)))
                raise ValueError(f"DSL round-trip differs at ScriptsFile offset 0x{first:X}")
            compressed_bytes += len(lzss_compress(compiled))
            if repack:
                disc = Path(manifest["discs"][occurrence["disc_index"]]["input_path"])
                container, logical_size, _ = read_field_container(disc, field_id)
                repack_container(container, compiled, logical_size=logical_size)
            if resize:
                # Exercise the standalone public editing workflow:
                # each visible source operation receives a new preceding NOP.
                edited_lines = []
                inserted_lines = set()
                inside_block = False
                for line in source.splitlines():
                    stripped = line.split("//", 1)[0].strip()
                    if stripped.startswith("block ") and stripped.endswith("{"):
                        inside_block = True
                    elif stripped == "}":
                        inside_block = False
                    if inside_block and stripped.endswith(";") and not stripped.startswith("fallthrough "):
                        edited_lines.append("          nop;")
                        inserted_lines.add(len(edited_lines))
                    edited_lines.append(line)
                original_ir = compile_xgs(source)
                edited_ir = compile_xgs("\n".join(edited_lines))
                def fingerprint(record):
                    return (record.kind, record.raw, record.references, record.block, record.alternate, record.jump_target)
                before = [fingerprint(record) for record in original_ir.records]
                after = [fingerprint(record) for record in edited_ir.records if record.line not in inserted_lines]
                if before != after:
                    raise ValueError("source insertion changed an existing operation or reference")
                source_linked = edited_ir.link()
                source_resized = source_linked.build("exact")
                if parse_assembly(source_linked.text()).build() != source_resized:
                    raise ValueError("edited source assembly round-trip differs")
                if repack:
                    repack_container(container, source_resized, logical_size=logical_size)
                source_resized_count += 1
                expanded = disassemble(scripts)
                old_records = list(expanded.records)
                expanded.records = []
                for record in old_records:
                    if record.kind == "op":
                        names = [name for name, pc in expanded.labels.items() if pc == record.pc]
                        expanded.records.append(Record(None, b"\x13", block=record.block, labels=names))
                        record.labels = []
                    expanded.records.append(record)
                linked = expanded.link()
                resized = linked.build("exact")
                new_code = resized[parse_scripts_file(resized)["bytecode_offset"]:]
                mapping = {item["source"]: item["linked"] for item in linked.link_report["address_map"]}
                for record in old_records:
                    if record.kind != "op" or record.raw == b"\xfe":
                        continue
                    old = decode_instruction(record.raw, 0)
                    new = decode_instruction(new_code, mapping[record.pc])
                    addresses = {position for offset in address_operand_offsets(old) for position in (offset, offset + 1)}
                    if old.opcode != new.opcode or any(a != b for i, (a, b) in enumerate(zip(old.raw, new.raw)) if i not in addresses):
                        raise ValueError(f"resizing changed a non-address operand at 0x{record.pc:04X}")
                    for offset, name in record.references.items():
                        if struct.unpack_from("<H", new.raw, offset)[0] != linked.labels[name]:
                            raise ValueError("resizing did not preserve a symbolic reference")
                if parse_assembly(linked.text()).build() != resized:
                    raise ValueError("resized assembly round-trip differs")
                if repack:
                    repack_container(container, resized, logical_size=logical_size)
                resized_count += 1
                resized_bytes += len(resized)
            count += 1
            total += len(scripts)
            bytecode_bytes += metadata["bytecode_size"]
        except ValueError as error:
            raise ValueError(f"{path.name} (Field {resource['occurrences'][0]['field_id']}): {error}") from error
    return {"resources": count, "scripts_file_bytes": total, "bytecode_bytes": bytecode_bytes, "lzss_bytes": compressed_bytes, "containers_repacked": count if repack else 0, "resized_resources": resized_count, "resized_scripts_bytes": resized_bytes, "edited_source_resources": source_resized_count}


def regenerate_xgs(root: Path, *, comments_only: bool = False, layout_only: bool = False, simplify_raw: bool = False) -> dict:
    """Re-render catalog sources from extracted assets, checking each round trip.

    Only script_path entries are rewritten. This deliberately avoids rebuilding
    or deleting the catalog, so binary views and other catalog files survive.
    """
    root = root.resolve()
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    catalog_root = (root / "catalog").resolve()
    catalog = json.loads((catalog_root / "catalog.json").read_text(encoding="utf-8"))
    assets = {}
    for resource in manifest["resources"]:
        path = (root / resource["asset_path"]).resolve()
        path.relative_to(root)
        payload = path.read_bytes()
        digest, hasher = _resource_digest(resource)
        if hasher(payload).hexdigest() != digest:
            raise ValueError(f"{path.name}: asset does not match its manifest digest")
        assets[digest] = (payload, parse_scripts_file(payload))
    destinations = set()
    work = []
    for entry in catalog["fields"]:
        destination = (root / entry["script_path"]).resolve()
        destination.relative_to(catalog_root)
        if destination.suffix != ".xgs" or destination in destinations:
            raise ValueError(f"invalid or duplicate catalog script path: {entry['script_path']}")
        if _catalog_digest(entry) not in assets:
            raise ValueError(f"missing manifest asset for {entry['script_path']}")
        destinations.add(destination)
        work.append((entry, destination))
    resources = set()
    for entry, destination in work:
        scripts, metadata = assets[_catalog_digest(entry)]
        if layout_only:
            text = group_source_by_entity(destination.read_text(encoding="utf-8"), scripts)
        elif comments_only or simplify_raw:
            text = restore_source_comments(destination.read_text(encoding="utf-8"), scripts, simplify_raw=simplify_raw)
        else:
            text, _ = render_source(entry["field_id"], scripts, metadata)
            if not equivalent_scripts(compile_xgs(text).build(), scripts):
                raise ValueError(f"{entry['script_path']}: regenerated DSL round-trip differs")
        content = text.encode("utf-8")
        write_if_changed(destination, content)
        if destination.read_bytes() != content:
            raise ValueError(f"{destination}: regenerated source write verification failed")
        resources.add(_catalog_digest(entry))
    return {"regenerated_xgs": len(work), "verified_scripts": 0 if comments_only or layout_only or simplify_raw else len(work), "unique_resources": len(resources), **({"comments_only": True} if comments_only else {}), **({"layout_only": True} if layout_only else {}), **({"simplify_raw": True} if simplify_raw else {})}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    decompile = commands.add_parser("decompile", help="emit editable DSL and/or lossless assembly")
    decompile.add_argument("input", type=Path)
    decompile.add_argument("--field", type=lambda value: int(value, 0), default=0)
    decompile.add_argument("--xgs", type=Path)
    decompile.add_argument("--xga", type=Path)
    for name in ("assemble", "compile"):
        command = commands.add_parser(name)
        command.add_argument("input", type=Path)
        command.add_argument("--output", type=Path, required=True)
        if name == "assemble":
            command.add_argument("--layout", choices=("relocate", "exact"), default="relocate")
        command.add_argument("--map", type=Path, help="write the link map and generated table/landing-pad report")
        if name == "compile":
            command.add_argument("--xga", type=Path, help="also write the lowered assembly")
    repack = commands.add_parser("repack", help="replace section 5 of a decompressed-header Field container")
    repack.add_argument("input", type=Path)
    repack.add_argument("--scripts", type=Path, required=True)
    repack.add_argument("--output", type=Path, required=True)
    override = commands.add_parser("override", help="create a .psxmod source directory for the existing runtime")
    override.add_argument("input", type=Path, help="stock CUE/BIN/ISO")
    override.add_argument("--field", type=lambda value: int(value, 0), required=True)
    override.add_argument("--scripts", type=Path, required=True)
    override.add_argument("--output", type=Path, required=True)
    override.add_argument("--disc-sha256", required=True, help="canonical digest from XenogearsRecomp --disc-hash")
    override.add_argument("--id", default="local.field-script")
    override.add_argument("--game-id", choices=("SLUS-00664", "SLUS-00669"), default="SLUS-00664")
    verify = commands.add_parser("verify", help="byte-exact round-trip validation of an extracted corpus")
    verify.add_argument("input", type=Path)
    verify.add_argument("--repack", action="store_true", help="also repack containers from the stock disc paths recorded in manifest.json")
    verify.add_argument("--resize", action="store_true", help="also insert one NOP before every instruction and verify all relocated encodings")
    regenerate = commands.add_parser("regenerate", help="overwrite catalog .xgs from the original extracted assets and verify their compilation")
    regenerate.add_argument("input", type=Path, help="extracted-field-scripts directory")
    quick = regenerate.add_mutually_exclusive_group()
    quick.add_argument("--comments-only", action="store_true", help="restore original byte/event traces without recompiling the corpus")
    quick.add_argument("--layout-only", action="store_true", help="restore entity-owned code presentation without recompiling the corpus")
    quick.add_argument("--simplify-raw", action="store_true", help="replace raw operations with verified semantic forms, preserving trace comments")
    args = parser.parse_args()
    try:
        if args.command not in {"override", "verify", "regenerate"}:
            requested = [path for name in ("output", "xgs", "xga", "map") if (path := getattr(args, name, None)) is not None]
            resolved = [path.resolve() for path in requested]
            inputs = {args.input.resolve()}
            if hasattr(args, "scripts"):
                inputs.add(args.scripts.resolve())
            if len(set(resolved)) != len(resolved) or inputs.intersection(resolved):
                raise ValueError("outputs must be distinct from each other and from all inputs")
        outputs = {}
        if args.command == "decompile":
            if not args.xgs and not args.xga:
                raise ValueError("decompile requires --xgs and/or --xga")
            scripts = args.input.read_bytes()
            metadata = parse_scripts_file(scripts)
            if args.xgs:
                text, _ = render_source(args.field, scripts, metadata)
                outputs[args.xgs] = text.encode("utf-8")
            if args.xga:
                outputs[args.xga] = disassemble(scripts).text().encode("utf-8")
        elif args.command in {"assemble", "compile"}:
            text = args.input.read_text(encoding="utf-8")
            if args.command == "assemble":
                assembly = parse_assembly(text)
            else:
                assembly = compile_xgs(text)
            assembly = assembly.link(args.layout if args.command == "assemble" else "relocate")
            outputs[args.output] = assembly.build("exact")
            if args.map:
                outputs[args.map] = (json.dumps(assembly.link_report, indent=2) + "\n").encode("utf-8")
            if args.command == "compile" and args.xga:
                outputs[args.xga] = assembly.text().encode("utf-8")
        elif args.command == "repack":
            outputs[args.output] = repack_container(args.input.read_bytes(), args.scripts.read_bytes())
        elif args.command == "override":
            path = write_override(args.input, args.field, args.scripts.read_bytes(), args.output, disc_sha256=args.disc_sha256, package_id=args.id, game_id=args.game_id)
            print(f"Runtime override source: {path}")
        elif args.command == "regenerate":
            print(json.dumps(regenerate_xgs(args.input, comments_only=args.comments_only, layout_only=args.layout_only, simplify_raw=args.simplify_raw), indent=2))
        else:
            print(json.dumps(verify_corpus(args.input, repack=args.repack, resize=args.resize), indent=2))
        for path, payload in outputs.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)
            print(f"Wrote {path} ({len(payload)} bytes)")
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
