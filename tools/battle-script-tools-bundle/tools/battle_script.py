#!/usr/bin/env python3
"""Decompile, compile, assemble and repack Xenogears Battle Event scripts (Python 3.11+)."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from compile_battle_events import _strip_comment, compile_xgs, disassemble, parse_assembly
from decompile_battle_events import render_source
from extract_disc_battle_scripts import (
    EVENT_DATA_FILE, EVENT_DIRECTORY, USER_SECTOR, parse_dialog_resource, parse_event_data_file, parse_script_resource,
    read_indexed_file, write_if_changed,
)
from repack_battle_events import lzss_compress, rebuild_event_data_file, write_override


def _load_manifest(root: Path) -> dict:
    return json.loads((root / "manifest.json").read_text(encoding="utf-8"))


def _read_asset(root: Path, relative: str, digest: str) -> bytes:
    path = (root / relative).resolve()
    path.relative_to(root.resolve())
    payload = path.read_bytes()
    if hashlib.sha1(payload).hexdigest() != digest:
        raise ValueError(f"{path.name}: asset does not match its manifest digest")
    return payload


def _insert_nops(source: str) -> tuple[str, set[int]]:
    """Insert one flow.nop() before every source operation in code sections."""
    lines = []
    inserted = set()
    inside = False
    for line in source.splitlines():
        stripped = _strip_comment(line).strip()
        if stripped in {"code {", "shared_code {"}:
            inside = True
        elif stripped == "}":
            inside = False
        elif inside and stripped.endswith(";") and not stripped.startswith("fallthrough "):
            lines.append("        flow.nop();")
            inserted.add(len(lines))
        lines.append(line)
    return "\n".join(lines) + "\n", inserted


def verify_corpus(root: Path, *, repack: bool = False, resize: bool = False) -> dict:
    manifest = _load_manifest(root)
    count = total = bytecode_bytes = compressed_bytes = resized_bytes = 0
    containers: dict[int, tuple[bytes, int]] = {}
    for resource in manifest["resources"]:
        occurrence = resource["occurrences"][0]
        event_index = occurrence["event_index"]
        try:
            script = _read_asset(root, resource["script_path"], resource["script_sha1"])
            dialog = _read_asset(root, resource["dialog_path"], resource["dialog_sha1"])
            metadata = parse_script_resource(script)
            parse_dialog_resource(dialog)
            if parse_assembly(disassemble(script).text()).link("exact").build() != script:
                raise ValueError("assembly round-trip differs")
            source, _ = render_source(event_index, script, dialog)
            linked = compile_xgs(source).link()
            if linked.build() != script:
                raise ValueError("unedited XGS does not reproduce the original binary exactly")
            stripped = "\n".join(_strip_comment(line) for line in source.splitlines())
            if compile_xgs(stripped).link().build() != script:
                raise ValueError("XGS without comments does not reproduce the original binary")
            if parse_assembly(linked.text()).link("exact").build() != script:
                raise ValueError("linked XGA does not reproduce the compiled binary")
            compressed_bytes += len(lzss_compress(script))
            replacements = {event_index: {"script": script, "dialog": dialog}}
            if resize:
                edited, inserted = _insert_nops(source)
                original_ir = compile_xgs(source)
                edited_ir = compile_xgs(edited)
                before = [record.fingerprint() for record in original_ir.records]
                after = [record.fingerprint() for record in edited_ir.records if record.line not in inserted]
                if before != after:
                    raise ValueError("source insertion changed an existing operation or reference")
                edited_linked = edited_ir.link()
                resized = edited_linked.build()
                if parse_assembly(edited_linked.text()).link("exact").build() != resized:
                    raise ValueError("edited source assembly round-trip differs")
                stripped_edit = "\n".join(_strip_comment(line) for line in edited.splitlines())
                if compile_xgs(stripped_edit).link().build() != resized:
                    raise ValueError("edited XGS without comments builds differently")
                replacements = {event_index: {"script": resized}}
                resized_bytes += len(resized)
            if repack:
                disc_index = occurrence["disc_index"]
                if disc_index not in containers:
                    disc = Path(manifest["discs"][disc_index]["input_path"])
                    container, logical_size, _, _ = read_indexed_file(disc, EVENT_DIRECTORY, EVENT_DATA_FILE)
                    containers[disc_index] = (container, logical_size)
                container, logical_size = containers[disc_index]
                rebuilt = rebuild_event_data_file(container, logical_size, replacements)
                stream = parse_event_data_file(rebuilt)["streams"][event_index * 2]["payload"]
                expected = replacements[event_index]["script"]
                if stream[:len(expected)] != expected or any(stream[len(expected):]):
                    raise ValueError("repacked event data does not expand to the compiled script")
            count += 1
            total += len(script)
            bytecode_bytes += metadata["bytecode_size"]
        except ValueError as error:
            raise ValueError(f"{Path(resource['script_path']).name} (Battle Event {event_index}): {error}") from error
    return {
        "resources": count,
        "script_bytes": total,
        "bytecode_bytes": bytecode_bytes,
        "lzss_bytes": compressed_bytes,
        "event_files_repacked": count if repack else 0,
        "resized_resources": count if resize else 0,
        "resized_script_bytes": resized_bytes,
        "xgs_validation": "byte-exact unedited XGS with and without comments; XGA and linked XGA identity; not gameplay validation",
    }


def regenerate_xgs(root: Path) -> dict:
    """Re-render catalog sources from extracted assets, checking each round trip."""
    root = root.resolve()
    manifest = _load_manifest(root)
    catalog_root = (root / "catalog").resolve()
    catalog = json.loads((catalog_root / "catalog.json").read_text(encoding="utf-8"))
    assets = {}
    for resource in manifest["resources"]:
        assets[resource["sha1"]] = (_read_asset(root, resource["script_path"], resource["script_sha1"]),
                                    _read_asset(root, resource["dialog_path"], resource["dialog_sha1"]))
    seen = set()
    for entry in catalog["events"]:
        destination = (root / entry["script_path"]).resolve()
        destination.relative_to(catalog_root)
        if destination.suffix != ".xgs" or destination in seen:
            raise ValueError(f"invalid or duplicate catalog script path: {entry['script_path']}")
        if entry["sha1"] not in assets:
            raise ValueError(f"missing manifest asset for {entry['script_path']}")
        seen.add(destination)
        script, dialog = assets[entry["sha1"]]
        text, _ = render_source(entry["event_index"], script, dialog)
        if compile_xgs(text).link().build() != script:
            raise ValueError(f"{entry['script_path']}: generated XGS is not byte-exact")
        write_if_changed(destination, text.encode("utf-8"))
        write_if_changed(destination.with_suffix(".xga"), disassemble(script).text().encode("utf-8"))
    return {"regenerated_xgs": len(seen), "verified_scripts": len(seen), "unique_resources": len(assets)}


def _event_pairs(events: list[int] | None, scripts: list[Path] | None, dialogs: list[str] | None) -> dict[int, dict[str, bytes]]:
    events = events or []
    scripts = scripts or []
    if not events or len(events) != len(scripts):
        raise ValueError("give one --script for each --event, in the same order")
    if dialogs and len(dialogs) != len(events):
        raise ValueError("--dialog, when used, must be repeated once per --event ('-' keeps the original)")
    replacements: dict[int, dict[str, bytes]] = {}
    for position, (event, script) in enumerate(zip(events, scripts)):
        if event in replacements:
            raise ValueError(f"Battle Event {event} is given twice")
        replacements[event] = {"script": script.read_bytes()}
        if dialogs and dialogs[position] != "-":
            replacements[event]["dialog"] = Path(dialogs[position]).read_bytes()
    return replacements


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    integer = lambda value: int(value, 0)  # noqa: E731
    decompile = commands.add_parser("decompile", help="emit editable DSL and/or lossless assembly")
    decompile.add_argument("input", type=Path, help="decompressed Battle Event script resource (script.bin)")
    decompile.add_argument("--event", type=integer, default=0, help="Battle Event data index shown in the source")
    decompile.add_argument("--dialog", type=Path, help="matching dialog.bin, used only for message comments")
    decompile.add_argument("--xgs", type=Path)
    decompile.add_argument("--xga", type=Path)
    for name in ("assemble", "compile"):
        command = commands.add_parser(name)
        command.add_argument("input", type=Path)
        command.add_argument("--output", type=Path, required=True)
        if name == "assemble":
            command.add_argument("--layout", choices=("relocate", "exact"), default="relocate")
        command.add_argument("--map", type=Path, help="write the link map")
        if name == "compile":
            command.add_argument("--xga", type=Path, help="also write the linked assembly")
    repack = commands.add_parser("repack", help="rebuild directory 0x20 file 2 with replaced event streams")
    repack.add_argument("input", type=Path, help="stored Battle Event data file (directory 0x20 file 2)")
    repack.add_argument("--event", type=integer, action="append", required=True)
    repack.add_argument("--script", type=Path, action="append", required=True)
    repack.add_argument("--dialog", action="append", help="replacement dialog.bin per --event, or '-'")
    repack.add_argument("--output", type=Path, required=True)
    override = commands.add_parser("override", help="create a .psxmod source directory for the existing runtime")
    override.add_argument("input", type=Path, help="stock CUE/BIN/ISO")
    override.add_argument("--event", type=integer, action="append", required=True)
    override.add_argument("--script", type=Path, action="append", required=True)
    override.add_argument("--dialog", action="append", help="replacement dialog.bin per --event, or '-'")
    override.add_argument("--output", type=Path, required=True)
    override.add_argument("--disc-sha256", required=True, help="canonical digest from XenogearsRecomp --disc-hash")
    override.add_argument("--id", default="local.battle-event-script")
    override.add_argument("--game-id", choices=("SLUS-00664", "SLUS-00669"), default="SLUS-00664")
    verify = commands.add_parser("verify", help="require byte-exact XGA/XGS round trips and validate edited lowering")
    verify.add_argument("input", type=Path)
    verify.add_argument("--repack", action="store_true", help="also rebuild the event data file from the stock disc paths in manifest.json")
    verify.add_argument("--resize", action="store_true", help="also insert one NOP before every operation and verify relocation")
    regenerate = commands.add_parser("regenerate", help="overwrite catalog .xgs/.xga from the extracted assets and verify them")
    regenerate.add_argument("input", type=Path, help="extracted-battle-scripts directory")
    args = parser.parse_args()
    try:
        if args.command not in {"override", "verify", "regenerate"}:
            requested = [path for name in ("output", "xgs", "xga", "map") if (path := getattr(args, name, None)) is not None]
            resolved = [path.resolve() for path in requested]
            inputs = {args.input.resolve()} | {path.resolve() for path in (getattr(args, "script", None) or [])}
            if getattr(args, "dialog", None):
                dialog = args.dialog if isinstance(args.dialog, list) else [args.dialog]
                inputs |= {Path(path).resolve() for path in dialog if str(path) != "-"}
            if len(set(resolved)) != len(resolved) or inputs.intersection(resolved):
                raise ValueError("outputs must be distinct from each other and from all inputs")
        outputs = {}
        if args.command == "decompile":
            if not args.xgs and not args.xga:
                raise ValueError("decompile requires --xgs and/or --xga")
            script = args.input.read_bytes()
            parse_script_resource(script)
            if args.xgs:
                dialog = args.dialog.read_bytes() if args.dialog else None
                text, _ = render_source(args.event, script, dialog)
                outputs[args.xgs] = text.encode("utf-8")
            if args.xga:
                outputs[args.xga] = disassemble(script).text().encode("utf-8")
        elif args.command in {"assemble", "compile"}:
            text = args.input.read_text(encoding="utf-8")
            assembly = parse_assembly(text) if args.command == "assemble" else compile_xgs(text)
            linked = assembly.link(args.layout if args.command == "assemble" else "relocate")
            outputs[args.output] = linked.build()
            if args.map:
                outputs[args.map] = (json.dumps(linked.link_report, indent=2) + "\n").encode("utf-8")
            if args.command == "compile" and args.xga:
                outputs[args.xga] = linked.text().encode("utf-8")
        elif args.command == "repack":
            data = args.input.read_bytes()
            # Retail streams may read a few bytes past the file's logical end;
            # on disc those are the zero bytes completing the final sector.
            padded = data + bytes(-len(data) % USER_SECTOR)
            outputs[args.output] = rebuild_event_data_file(padded, len(data), _event_pairs(args.event, args.script, args.dialog))
        elif args.command == "override":
            path = write_override(args.input, _event_pairs(args.event, args.script, args.dialog), args.output,
                                  disc_sha256=args.disc_sha256, package_id=args.id, game_id=args.game_id)
            print(f"Runtime override source: {path}")
        elif args.command == "regenerate":
            print(json.dumps(regenerate_xgs(args.input), indent=2))
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
