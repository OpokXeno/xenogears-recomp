# Battle Event Script DSL Documentation

For editing, start with [`RECOMPILER_USAGE.md`](RECOMPILER_USAGE.md). For format
details, the recommended reading order is:

1. [`FORMAT_AND_CONCEPTS.md`](FORMAT_AND_CONCEPTS.md): event data file, script
   resource, entities, entries, slots, shared code and coverage.
2. [`DSL_REFERENCE.md`](DSL_REFERENCE.md): syntax of statements, values,
   variables, conditions, operations and diagnostics.
3. [`OBJECTS_AND_NAMESPACES.md`](OBJECTS_AND_NAMESPACES.md): meaning of `flow`,
   `state`, `dialogue`, `camera`, `visual`, `audio`, `sprite`, `actor`,
   `battle` and `event`.
4. [`OPERATION_CATALOG.md`](OPERATION_CATALOG.md): generated reference for the 76
   dispatch entries `0x00..0x4B`.
5. [`RECOMPILER_DESIGN.md`](RECOMPILER_DESIGN.md): compiler, relocating linker,
   LZSS and repacking pipeline, design decisions.
6. [`RECOMPILER_USAGE.md`](RECOMPILER_USAGE.md): editing, compilation, assembly,
   repacking, runtime overrides and corpus validation.
7. [`RELOCATION_EVIDENCE.md`](RELOCATION_EVIDENCE.md): executable evidence for
   operand layouts and recorded corpus results.

## Sources Of Truth

| Question | Source |
|---|---|
| What bytes an event contains | `script.bin` and `bytecode.bin` |
| How an instruction is encoded | `battle_event_codec.py` operand schemas and the source statement |
| Where an instruction is placed | Relocating link map / linked XGA |
| What its original bytes were | Informational byte traces, ignored by compilation |
| Which entries an entity stores | Editable `events` block; original rows are in extraction metadata |
| Which variable slots a script uses | `state` bindings and the link map's allocation report |
| What a DSL construct means | `DSL_REFERENCE.md` |
| What a namespace represents | `OBJECTS_AND_NAMESPACES.md` |
| Which opcodes exist | `OPERATION_CATALOG.md` and `battle_event_opcode_table.json` |
| Which bytes were not verified as code | `data` and the coverage report |
| What a message says | `dialog.txt` and message comments (informational) |
