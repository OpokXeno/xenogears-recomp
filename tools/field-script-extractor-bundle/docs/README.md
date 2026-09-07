# Field Script DSL Documentation

Recommended reading order:

1. [`FORMAT_AND_CONCEPTS.md`](FORMAT_AND_CONCEPTS.md): physical structure,
   entities, events, actors, state, shared code, and coverage.
2. [`DSL_REFERENCE.md`](DSL_REFERENCE.md): syntax of instructions, values,
   conditions, calls, results, and diagnostics.
3. [`OBJECTS_AND_NAMESPACES.md`](OBJECTS_AND_NAMESPACES.md): meaning of
   `actor`, `movement`, `world`, `camera`, `dialogue`, `audio`, `visual`,
   `battle`, `inventory`, `input`, `state`, `flow`, and `event`.
4. [`OPERATION_CATALOG.md`](OPERATION_CATALOG.md): comprehensive reference for
   the 256 primary and 227 extended opcodes.
5. [`RECOMPILER_DESIGN.md`](RECOMPILER_DESIGN.md): proposed reversible
   `script.xgs` to `script.xga` to `scripts.bin` compiler and repacking pipeline.

The complete catalog is generated. The other guides explain modeling decisions
and must be updated manually when the DSL changes.

## Sources Of Truth

| Question | Source |
|---|---|
| What bytes a map contains | `scripts.bin` and `bytecode.bin` |
| Where each instruction came from | `// PC: bytes` comment in `script.xgs` |
| Which entries an entity stores | `metadata.json` and the `events` block |
| What a DSL construct means | `DSL_REFERENCE.md` |
| What a namespace represents | `OBJECTS_AND_NAMESPACES.md` |
| Which opcodes exist | `OPERATION_CATALOG.md` and `field_opcode_table.json` |
| Which regions were not verified as code | `non_instruction_regions` and the coverage report |

## Regeneration

```bash
python3 tools/build_opcode_table.py
python3 tools/build_dsl_documentation.py
```

The first command requires the original Field documentation tree. The second
requires only the self-contained table included in this bundle.
