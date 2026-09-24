#!/usr/bin/env python3
"""Exhaustive, non-fail-fast standalone XGS validation with per-resource evidence.

Byte identity is reported separately from successful structural compilation.
The edited-source check inserts a NOP before every operation, checks the source
IR for unintended changes, then links and serializes the edited program. No XGA
or original binary is ever supplied to compile_xgs.
Both original and edited XGS are also compiled with all comments removed and
with misleading comments. Each output must match its commented counterpart.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import time
import traceback
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

from compile_field_scripts import compile_xgs, parse_assembly
from editable_field_scripts import equivalent_scripts, render_source
from extract_disc_field_scripts import parse_scripts_file


def insert_operations(text):
    lines, inserted = [], set()
    inside = False
    for line in text.splitlines():
        code = line.split("//", 1)[0].strip()
        if code in {"code {", "shared_code {"} or (code.startswith("block ") and code.endswith("{")):
            inside = True
        elif code == "}":
            inside = False
        if inside and code.endswith(";") and not code.startswith("fallthrough ") and code != "unreachable;":
            indent = line[:len(line) - len(line.lstrip())]
            lines.append(indent + "nop;")
            inserted.add(len(lines))
        lines.append(line)
    return "\n".join(lines) + "\n", inserted


def fingerprint(record):
    return (record.kind, record.raw, record.references, record.block, record.alternate,
             record.jump_target, record.semantic, record.terminal, record.relative)


def comment_variants(text):
    """Remove even blank lines, then inject false provenance and encoding traces."""
    code = [line.split('//', 1)[0].rstrip() for line in text.splitlines()]
    code = [line for line in code if line.strip()]
    stripped = '\n'.join(code) + '\n'
    poisoned = ['// ScriptsFile SHA-256: ' + 'f' * 64]
    for index, line in enumerate(code):
        poisoned.append('// view: FFFF')
        poisoned.append('// 0000: FE 00 FF FF FF FF FF FF')
        poisoned.append(line + (' // FFFF: FE' if index % 2 else ' // entry: FFFF'))
        poisoned.append('// ignored syntax: } @encoding("FF") @layout(missing); ñ')
    return {'no_comments': stripped, 'false_comments': '\n'.join(poisoned) + '\n'}


def check_comments(source, expected, folder, prefix):
    hashes = {}
    for name, text in comment_variants(source).items():
        (folder / f'{prefix}-{name}.xgs').write_text(text, encoding='utf-8')
        compiled = compile_xgs(text).build()
        if compiled != expected:
            raise ValueError(f'{prefix}: {name} changed compiled bytes')
        hashes[name] = hashlib.sha256(compiled).hexdigest()
    return hashes


def check_resource(job):
    index, resource, root, output, edit = job
    started = time.monotonic()
    occurrence = resource["occurrences"][0]
    result = {"index": index, "asset": resource["asset_path"],
              "occurrences": [{"disc_index": item["disc_index"], "field_id": item["field_id"]}
                              for item in resource["occurrences"]],
              "field_id": occurrence["field_id"], "roundtrip": "error", "edit": "skipped"}
    folder = Path(output) / f"resource-{index:04d}"
    folder.mkdir()
    stage = "input"
    try:
        original = (Path(root) / resource["asset_path"]).read_bytes()
        algorithm = "sha1" if "sha1" in resource else "sha256"
        if hashlib.new(algorithm, original).hexdigest() != resource[algorithm]:
            raise ValueError("asset digest disagrees with manifest")
        result["original_size"] = len(original)
        result["original_sha256"] = hashlib.sha256(original).hexdigest()
        metadata = parse_scripts_file(original)
        stage = "decompile"
        source, analysis = render_source(occurrence["field_id"], original, metadata)
        (folder / "source.xgs").write_text(source, encoding="utf-8")
        result["decompile_diagnostics"] = analysis["diagnostics"]
        result["data_image_bytes"] = analysis.get("script_data_image_bytes", 0)
        result["cloned_interior_terminals"] = analysis.get("cloned_interior_terminals", 0)
        stage = "compile"
        ir = compile_xgs(source)
        before = [fingerprint(record) for record in ir.records]
        stage = "link"
        linked = ir.link()
        result["encoding_pipeline"] = linked.link_report.get("encoding_pipeline")
        if result["encoding_pipeline"] != "per-instruction-relaxation":
            raise ValueError("source bypassed the instruction encoder/linker")
        result["relaxation_passes"] = linked.link_report.get("relaxation_passes")
        result["relaxed_choices"] = linked.link_report.get("relaxed_choices")
        compiled = linked.build("exact")
        (folder / "recompiled.bin").write_bytes(compiled)
        result["compiled_size"] = len(compiled)
        result["compiled_sha256"] = hashlib.sha256(compiled).hexdigest()
        result["byte_identical"] = compiled == original
        result["bitmap_identical"] = compiled[:128] == original[:128]
        result["entity_count_identical"] = compiled[128:132] == original[128:132]
        result["size_delta"] = len(compiled) - len(original)
        if compiled[:132] != original[:132]:
            raise ValueError("standalone compilation changed VM variable types or entity count")
        result["roundtrip"] = "exact" if compiled == original else "different"
        result["first_difference"] = next((i for i, (a, b) in enumerate(zip(original, compiled)) if a != b),
                                          min(len(original), len(compiled)) if original != compiled else None)
        if original != compiled:
            result["verified_neutral_only"] = equivalent_scripts(compiled, original)
        stage = "linked_assembly"
        if parse_assembly(linked.text()).build("exact") != compiled:
            raise ValueError("linked XGA serialization disagrees with compiled binary")
        result["linked_structure"] = "passed"
        stage = "comments"
        result["comment_variants"] = check_comments(source, compiled, folder, "source")
        result["comments"] = "passed"
        if edit:
            stage = "edit_compile"
            edited, inserted = insert_operations(source)
            (folder / "edited.xgs").write_text(edited, encoding="utf-8")
            result["inserted_operations"] = len(inserted)
            edited_ir = compile_xgs(edited)
            if edited_ir.bitmap != ir.bitmap or edited_ir.rows != ir.rows:
                raise ValueError("inserting operations changed variable types or symbolic event bindings")
            after = [fingerprint(record) for record in edited_ir.records if record.line not in inserted]
            if before != after:
                raise ValueError("inserting NOPs changed an existing semantic operation or reference")
            stage = "edit_link"
            edited_linked = edited_ir.link()
            if edited_linked.link_report.get("encoding_pipeline") != result["encoding_pipeline"]:
                raise ValueError("edited source used a different encoding pipeline")
            result["edited_relaxation_passes"] = edited_linked.link_report.get("relaxation_passes")
            edited_binary = edited_linked.build("exact")
            (folder / "edited.bin").write_bytes(edited_binary)
            if parse_assembly(edited_linked.text()).build("exact") != edited_binary:
                raise ValueError("edited program disagrees with its linked assembly")
            if inserted and edited_binary == compiled:
                raise ValueError("inserted operations did not affect output")
            result["same_encoding_pipeline"] = True
            result.update(edit="passed", edited_size=len(edited_binary),
                          edited_sha256=hashlib.sha256(edited_binary).hexdigest())
            stage = "edit_comments"
            result["edited_comment_variants"] = check_comments(edited, edited_binary, folder, "edited")
            result["edited_comments"] = "passed"
    except Exception as error:
        result["failure_stage"] = stage
        result["error"] = f"{type(error).__name__}: {error}"
        result["traceback"] = traceback.format_exc()
        if stage.startswith("edit_"):
            result["edit"] = "error"
        else:
            result["roundtrip"] = "error"
    result["seconds"] = round(time.monotonic() - started, 3)
    (folder / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def isolated_resource(job, timeout):
    """A resource failure, including a Python crash, cannot abort the corpus."""
    index, resource, root, output, edit = job
    folder = Path(output) / f"resource-{index:04d}"
    command = [sys.executable, "-X", "faulthandler", str(Path(__file__).resolve()), root,
               "--output", output, "--worker-index", str(index)]
    if not edit:
        command.append("--no-edit")
    started = time.monotonic()
    try:
        process = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        stderr = process.stderr
        result_path = folder / "result.json"
        if result_path.exists():
            result = json.loads(result_path.read_text(encoding="utf-8"))
            if process.returncode == 0 or "error" in result:
                return result
        message = f"worker exited with status {process.returncode}"
    except subprocess.TimeoutExpired as error:
        message = f"worker exceeded {timeout} seconds"
        stderr = error.stderr or ""
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", errors="replace")
    except Exception as error:
        message, stderr = f"worker launcher: {error}", traceback.format_exc()
    folder.mkdir(exist_ok=True)
    (folder / "worker-stderr.txt").write_text(stderr, encoding="utf-8")
    result = {"index": index, "asset": resource["asset_path"],
              "field_id": resource["occurrences"][0]["field_id"],
              "occurrences": [{"disc_index": item["disc_index"], "field_id": item["field_id"]}
                              for item in resource["occurrences"]],
              "roundtrip": "error", "edit": "skipped", "failure_stage": "worker",
              "error": message, "seconds": round(time.monotonic() - started, 3)}
    (folder / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=120, help="seconds per isolated resource")
    parser.add_argument("--no-edit", action="store_true")
    parser.add_argument("--require-exact", action="store_true", help="fail unless every reconstruction is byte-identical to its input")
    parser.add_argument("--retry-report", type=Path, help="rerun only resources with errors in this report")
    parser.add_argument("--worker-index", type=int, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.workers < 1:
        parser.error("workers must be positive")
    root, output = args.input.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    if args.worker_index is not None:
        result = check_resource((args.worker_index, manifest["resources"][args.worker_index],
                                 str(root), str(output), not args.no_edit))
        return int("error" in result)
    if (output / "results.jsonl").exists():
        parser.error("output directory already contains a run")
    resources = list(enumerate(manifest["resources"]))
    if args.retry_report:
        previous = json.loads(args.retry_report.read_text(encoding="utf-8"))
        failed = {item["index"] for item in previous["results"] if "error" in item}
        resources = [(index, item) for index, item in resources if index in failed]
    started = time.monotonic()
    results = []
    print(f"Resources: {len(resources)}; occurrences: {sum(len(r['occurrences']) for _, r in resources)}; workers: {args.workers}", flush=True)
    jobs = [(index, resource, str(root), str(output), not args.no_edit) for index, resource in resources]
    with (output / "results.jsonl").open("w", encoding="utf-8") as log:
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            futures = [pool.submit(isolated_resource, job, args.timeout) for job in jobs]
            for future in as_completed(futures):
                result = future.result()
                results.append(result)
                log.write(json.dumps(result) + "\n")
                log.flush()
                if len(results) % 25 == 0 or "error" in result:
                    print(f"{len(results)}/{len(jobs)}: " + json.dumps(dict(Counter(r['roundtrip'] for r in results)))
                          + (f"; resource {result['index']} field {result['field_id']}: {result['error']}" if "error" in result else ""), flush=True)
    summary = {"resource_count": len(results),
               "occurrence_count": sum(len(r["occurrences"]) for r in results),
               "roundtrip": dict(Counter(r["roundtrip"] for r in results)),
               "edit": dict(Counter(r["edit"] for r in results)),
               "comments": dict(Counter(r.get("comments", "skipped") for r in results)),
               "edited_comments": dict(Counter(r.get("edited_comments", "skipped") for r in results)),
               "error_stages": dict(Counter(r["failure_stage"] for r in results if "error" in r)),
               "neutral_only_differences": sum(r.get("verified_neutral_only", False) for r in results),
               "same_size_nonidentical": sum(r.get("byte_identical") is False and r.get("size_delta") == 0 for r in results),
               "size_changed": sum(r.get("size_delta", 0) != 0 for r in results),
               "resources_with_diagnostics": sum(bool(r.get("decompile_diagnostics")) for r in results),
               "bitmap_changes": sum(r.get("bitmap_identical") is False for r in results),
               "entity_count_changes": sum(r.get("entity_count_identical") is False for r in results),
               "inserted_operations": sum(r.get("inserted_operations", 0) for r in results),
               "original_total_bytes": sum(r.get("original_size", 0) for r in results),
               "compiled_total_bytes": sum(r.get("compiled_size", 0) for r in results),
               "edited_total_bytes": sum(r.get("edited_size", 0) for r in results),
               "exact_required": args.require_exact,
               "exact_passed": all(r["roundtrip"] == "exact" for r in results),
               "same_encoding_pipeline": sum(r.get("same_encoding_pipeline", False) for r in results),
               "seconds": round(time.monotonic() - started, 3)}
    report = {"summary": summary, "python": sys.version, "execution": "isolated process per resource",
              "scope": "standalone XGS; exact identity reported independently; edits preserve semantic IR; no gameplay validation",
              "input": str(root), "results": sorted(results, key=lambda item: item["index"])}
    report["tool_sha256"] = {name: hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
                            for name in ("validate_field_script_corpus.py", "compile_field_scripts.py",
                                         "decompile_field_scripts.py", "editable_field_scripts.py",
                                         "field_instruction_codec.py", "field_semantic_ops.py",
                                         "field_linker.py", "field_source_fidelity.py", "field_native_selection.py", "field_opcode_table.json")}
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2), flush=True)
    print(f"Report: {output / 'report.json'}", flush=True)
    if any("error" in result for result in results):
        return 1
    return 2 if args.require_exact and not summary["exact_passed"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
