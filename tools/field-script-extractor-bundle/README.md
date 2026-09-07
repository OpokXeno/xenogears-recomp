# Xenogears Field Script Extractor

Self-contained Python 3.11+ tool for extracting Field VM scripts from one or
more retail Xenogears discs. It uses only the Python standard library.

## Usage

From this bundle directory:

```bash
python3 tools/extract_disc_field_scripts.py \
  /path/to/disc1.cue /path/to/disc2.cue
```

Inputs may be CUE files referring to MODE1/MODE2 BIN tracks, raw 2352-byte BIN
images, or 2048-byte ISO images. The default output is
`extracted-field-scripts`; use `--output PATH` to override it. One or more
`--field ID` options restrict extraction to selected decimal or `0x`-prefixed
Field IDs.

## Output

```text
extracted-field-scripts/
  manifest.json
  assets/<scripts-file-sha256>.scripts.bin
  metadata/<scripts-file-sha256>.json
  catalog/catalog.json
  catalog/disc-01/field-0000_<hash>/
    scripts.bin
    script.xgs
    bytecode.bin
    variable-types.bin
    metadata.json
    sources.json
```

`scripts.bin` is the complete decompressed section 5 `ScriptsFile`, including
its `0x80`-byte variable type bitmap, routine rows, and shared bytecode.
`bytecode.bin` and `variable-types.bin` are convenience views. The metadata
records every one of the 32 entry-point offsets for each entity, together with
the source FAT route and container/LZSS sizes.

`script.xgs` is a conservative event DSL rendering of the Field VM program. It
uses subsystem-qualified operations, semantic state symbols, actor event
bindings, shared labels, calls, branches, trigger conditions, and an optional
arrival table. State declarations retain their VM offset and signedness. Known
global names come from the Field documentation; other names combine the
variable's observed role with the surrounding actor, movement, dialogue,
camera, or other subsystem operations.

## Documentation

- [`docs/FORMAT_AND_CONCEPTS.md`](docs/FORMAT_AND_CONCEPTS.md) explains the
  physical `ScriptsFile`, every top-level DSL section, entities, events, actors,
  party members, player control, shared code, and non-instruction regions.
- [`docs/DSL_REFERENCE.md`](docs/DSL_REFERENCE.md) documents values, state,
  control flow, calls, assignments, properties, operation results, triggers,
  diagnostics, and source traces.
- [`docs/OBJECTS_AND_NAMESPACES.md`](docs/OBJECTS_AND_NAMESPACES.md) explains
  each logical object, the runtime state it affects, representative operations,
  and common cross-subsystem sequences.
- [`docs/OPERATION_CATALOG.md`](docs/OPERATION_CATALOG.md) catalogs all 483
  primary and extended operations under `actor`, `movement`, `world`, `camera`,
  `dialogue`, `audio`, `visual`, `battle`, `inventory`, `input`, `state`,
  `flow`, and `event`.
- [`docs/RECOMPILER_DESIGN.md`](docs/RECOMPILER_DESIGN.md) specifies a proposed
  reversible `script.xgs` to `script.xga` to `scripts.bin` compiler and Field
  resource repacking pipeline.

Each entity contains its event bindings followed by code owned exclusively by
that entity. Blocks reached by multiple entities, plus validated orphan event
code with no stored entrypoint, are emitted once under `shared_code`. This keeps
entity events together without duplicating shared instructions or inventing
routine lengths.

Control flow is followed from every stored routine entry and through computed
`A6` jump tables. Unreferenced regions are rendered as inferred orphan events
only when a linear decode has aligned branch targets or ends exactly at a
terminal instruction. Everything else remains a typed data region. Every
instruction and data line retains its source offset and bytes, while operation
fields with no proven semantic schema remain explicit positional values.
Decoding conflicts remain diagnostics, so readability never depends on silently
discarding source bytes or inventing routine boundaries.

The per-resource report separates instruction coverage from total coverage:

```text
instruction_bytes + data_bytes = classified_bytes = bytecode_size
total_coverage_percent = 100.0
```

The out-of-band arrival table is rendered separately from executable events.
Other bytes inside the shared bytecode area that are not VM instructions appear
under `non_instruction_regions` and remain byte-exact. They are distinguished as
zero padding, short separator/padding bytes, embedded table-or-payload data, or
an unreferenced code candidate when most of the region decodes as instructions
but lacks sufficient control-flow evidence for promotion to code. This is not a
separate resource section: these bytes are interleaved with, or adjacent to, the
script instruction stream.

Assets are content-addressed and shared by identical occurrences. Catalog
files are links when the host supports them and copies otherwise.

## Format And Validation

Field containers are discovered through Xenogears directory `4` using:

```text
file_id = 0xB8 + 2 * field_id
```

The extractor reads section 5 from container directory entries `0x120`
(declared expanded size) and `0x144` (stream offset). LZSS control bits are
processed low-to-high, with zero selecting a literal and one selecting a
back-reference. The complete compressed extent, including final sector
padding, remains available as lookahead, while the expanded target must fit the
loader's `declared_size + 0x10` allocation.

The decompressed script section must contain the full bitmap and routine table,
and every routine entry must resolve inside the shared bytecode. Invalid
directory candidates and short placeholders are reported in each disc's
manifest record rather than emitted as scripts.

Routine entries are not exported as independent bodies. The game stores no
routine lengths, entries may alias, and control flow may share code; assigning
an end from the next sorted offset would lose those semantics.

The bundled `tools/field_opcode_table.json` contains all 256 primary and 227
extended dispatch names, sizes, handlers, and behavior summaries. It is generated
solely from the Field documentation with:

```bash
python3 tools/build_opcode_table.py
python3 tools/build_dsl_documentation.py
```
