"""Instruction selection and monotonic layout relaxation for every XGS build.

No whole-source comparison or original-binary return is permitted here. Native
forms are accepted only when their current operands and current layout satisfy
the VM's reads. Failed forms relax independently to compiler-owned operands.
"""

from __future__ import annotations

import copy

from field_semantic_ops import SCRIPT_IMAGE_MAGIC, lower_semantic_records


def prepare(source, disabled):
    trial = copy.deepcopy(source)
    alias_checks = []
    for name, terminal in sorted(trial.alias_fallbacks.items()):
        key = ('entry', name)
        if key in disabled:
            from compile_field_scripts import Record

            del trial.aliases[name]
            trial.records.append(Record(None, terminal, labels=[name], site=len(trial.records)))
        else:
            alias_checks.append((key, name, terminal))
    source_bindings = {name: record for record in trial.records for name in record.labels}

    def referenced_record(name):
        visited = set()
        while name in trial.aliases and name not in visited:
            visited.add(name)
            name = trial.aliases[name][0]
        return source_bindings.get(name)

    entered = set()
    names = [name for row in trial.rows.values() for name in row]
    for record in trial.records:
        if record.kind == "edge":
            continue
        if record.kind == "op" and record.raw[0] in {0x48, 0x49}:
            continue
        names.extend(record.references.values())
        names.extend(name for name in (record.alternate, record.jump_target) if name is not None)
    for name in names:
        record = referenced_record(name)
        if record is not None:
            entered.add(record.site)

    image_checks = []
    removed = set()
    for block in sorted(trial.script_images):
        if ("image", block) in disabled:
            continue
        items = [record for record in trial.records if record.block == block]
        if not items or not items[0].raw.startswith(SCRIPT_IMAGE_MAGIC):
            continue
        if any(record.kind != "data" or record.references for record in items):
            continue
        content = b"".join(record.raw for record in items[1:])
        offset = 0
        for record in items[1:]:
            for name in record.labels:
                trial.absolute_symbols[name] = offset
            offset += len(record.raw)
        removed.update(id(record) for record in items)
        image_checks.append((("image", block), content))
    trial.records = [record for record in trial.records if id(record) not in removed]

    semantic_disabled = {site for kind, site in disabled if kind == "semantic"}
    trial.records = lower_semantic_records(trial.records, disabled=semantic_disabled, externally_entered=entered)
    return trial, image_checks, alias_checks


def invalid_choices(linked, image_checks, alias_checks):
    # Same serializer that builds the output; constraints observe relocations,
    # not stale placeholder bytes or original trace bytes.
    code = linked.encode_bytecode()
    changed = False
    for record in linked.records:
        if record.kind == "op" and record.raw == b"\xfe":
            if record.pc + 1 >= len(code) or code[record.pc + 1] not in {0, *range(0x78, 0x7F)}:
                record.raw = b"\x13"
                changed = True
    if changed:
        code = linked.encode_bytecode()
    failed = set()
    at = {record.pc: record for record in linked.records}
    for record in linked.records:
        for rule in record.constraints:
            kind, offset, *values = rule
            address = record.pc + offset
            valid = False
            if kind == "bytes":
                expected, masks = values
                actual = code[address:address + len(expected)] if address >= 0 else b""
                valid = len(actual) == len(expected) and all((a & m) == (b & m) for a, b, m in zip(actual, expected, masks))
            elif kind == "label":
                valid = linked.labels[values[0]] == address
            elif kind == "nonzero":
                valid = 0 <= address < len(code) and code[address] != 0
            elif kind == "limit":
                expected, can_be_initialized = values
                valid = code[address:address + 2] == expected
                previous = at.get(record.pc - 9)
                if (not valid and can_be_initialized and previous is not None and previous.kind == "op"
                        and previous.raw[:2] == b"\x11\0" and len(previous.raw) == 9
                        and code[record.pc + 3] & 0x80):
                    valid = True
            if not valid:
                failed.add(("semantic", record.site))
    for key, expected in image_checks:
        if code[:len(expected)] != expected:
            failed.add(key)
    for key, name, terminal in alias_checks:
        address = linked.labels[name]
        if code[address:address + len(terminal)] != terminal:
            failed.add(key)
    return failed


def link_source(source, native_linker):
    disabled = set()
    for iteration in range(len(source.records) + len(source.script_images) + len(source.alias_fallbacks) + 2):
        trial, images, aliases = prepare(source, disabled)
        linked = native_linker(trial, "relocate", defer_validation=True)
        failed = invalid_choices(linked, images, aliases)
        if not failed:
            linked.link_report.update({"encoding_pipeline": "per-instruction-relaxation",
                                       "relaxation_passes": iteration + 1,
                                       "relaxed_choices": sorted(map(str, disabled)),
                                        "shared_data_views": len(images), "shared_terminal_entries": len(aliases)})
            linked.build("exact")
            return linked
        new = failed - disabled
        if not new:
            raise ValueError(f"native operand constraints cannot be satisfied: {sorted(map(str, failed))}")
        disabled.update(new)
    raise ValueError("native instruction selection did not converge")
