# Field Script DSL Documentation

For editing, start with [`RECOMPILER_USAGE.md`](RECOMPILER_USAGE.md). For format
details, the recommended reading order is:

1. [`FORMAT_AND_CONCEPTS.md`](FORMAT_AND_CONCEPTS.md): physical structure,
   entities, events, actors, state, shared code, and coverage.
2. [`DSL_REFERENCE.md`](DSL_REFERENCE.md): syntax of instructions, values,
   conditions, calls, results, and diagnostics.
3. [`OBJECTS_AND_NAMESPACES.md`](OBJECTS_AND_NAMESPACES.md): meaning of
   `actor`, `movement`, `world`, `camera`, `dialogue`, `audio`, `visual`,
   `battle`, `inventory`, `input`, `state`, `flow`, and `event`.
4. [`OPERATION_CATALOG.md`](OPERATION_CATALOG.md): comprehensive reference for
   the 256 primary and 227 extended opcodes.
5. [`RECOMPILER_DESIGN.md`](RECOMPILER_DESIGN.md): implemented compiler,
   relocating linker, repacking pipeline and design decisions.
6. [`RECOMPILER_USAGE.md`](RECOMPILER_USAGE.md): editing, compilation, assembly,
   repacking, runtime overrides and corpus validation.
7. [`RELOCATION_EVIDENCE.md`](RELOCATION_EVIDENCE.md): executable-level reference
    evidence, generated routing code and whole-corpus resize proofs.
8. [`SHARED_SCRIPT_BYTES.md`](SHARED_SCRIPT_BYTES.md): cross-instruction reads,
   standalone semantic operations and native operand-island lowering.

The complete catalog is generated. The other guides explain modeling decisions
and must be updated manually when the DSL changes.

## Sources Of Truth

| Question | Source |
|---|---|
| What bytes a map contains | `scripts.bin` and `bytecode.bin` |
| How an instruction is encoded | Shared operand form and semantic source statement |
| How fixed native operand offsets are satisfied | Compiler-owned operand islands |
| Where an instruction is placed | Relocating link map / linked XGA |
| What its original bytes were | Informational byte/entry traces and checksum, ignored by compilation |
| Which entries an entity stores | Editable `events` block; original entries are recorded in extraction metadata |
| Which VM slots variables use | `state` bindings and the compiler's allocation report |
| What a DSL construct means | `DSL_REFERENCE.md` |
| What a namespace represents | `OBJECTS_AND_NAMESPACES.md` |
| Which opcodes exist | `OPERATION_CATALOG.md` and `field_opcode_table.json` |
| Which bytes were not verified as code | `data` and the coverage report |


## Regeneration

```bash
python3 tools/build_opcode_table.py
python3 tools/build_dsl_documentation.py
```

The first command requires the original Field documentation tree. The second
requires only the self-contained table included in this bundle.
