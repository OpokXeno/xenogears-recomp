# Xenogears Field Script Tools

Self-contained Python 3.11+ tools for extracting, decompiling, recompiling and
repacking Field VM scripts from retail Xenogears discs. They use only the Python
standard library.

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
    script.xga
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

`script.xga` is lossless assembly containing complete instruction encodings,
symbolic references, all routine rows, the bitmap and byte-exact data. Both
source formats are self-contained. XGA preserves exact bytes; XGS may normalize
verified, behavior-neutral instruction encoding variants for readability.

## Compile And Repack

From this bundle directory:

```bash
python3 tools/field_script.py decompile /path/to/scripts.bin \
  --field 7 --xgs script.xgs --xga script.xga

# Edit script.xgs, then compile its semantic statements:
python3 tools/field_script.py compile script.xgs \
  --output scripts.modified.bin --xga script.modified.xga --map script.map.json

# Assembly can also be edited and built directly:
python3 tools/field_script.py assemble script.modified.xga \
  --output scripts.assembled.bin

python3 tools/field_script.py repack field.bin \
  --scripts scripts.modified.bin --output field.modified.bin
```

The `.xgs` is standalone: compilation needs no original binary or per-script
metadata file. Each entity contains its events and `code`; shared instructions
appear under `shared_code`. Original byte traces and event comments are retained.
Behavior descriptions are not emitted.

The linker recalculates positions, routine entries, jumps, calls and script-data
references. Comments are informational and ignored by compilation. VM variable bindings and
preserved data remain explicit because they have runtime meaning.
For fixed-address assertions in the low-level representation, use
`assemble --layout exact`. The optional link map reports final placement.

`field_script.py override` creates a `.psxmod` source package from a stock disc
and compiled ScriptsFile using the port's existing indexed-file replacement
mechanism. See [`docs/RECOMPILER_USAGE.md`](docs/RECOMPILER_USAGE.md) for the full
editing and runtime workflow. Regenerate older extractions before compiling;
there is one current source format, with no compatibility layer.

Regenerate all catalog `.xgs` files from the extracted assets, checking each
compiled result against its original (including verified neutral encodings):

```bash
python3 tools/field_script.py regenerate extracted-field-scripts
```

For focused maintenance of generated sources without full compilation, use
`--comments-only`, `--layout-only` or `--simplify-raw`. Full regeneration
overwrites the generated catalog source. Normal `compile` processes one file;
the corpus-wide `verify` commands below are separate, potentially lengthy tests.

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
- [`docs/RECOMPILER_DESIGN.md`](docs/RECOMPILER_DESIGN.md) describes the
  implemented standalone compiler, relocation, validation and binary guarantees.
- [`docs/RECOMPILER_USAGE.md`](docs/RECOMPILER_USAGE.md) documents compilation,
  assembly syntax, larger edits, repacking and runtime override packages.
- [`docs/RELOCATION_EVIDENCE.md`](docs/RELOCATION_EVIDENCE.md) records executable
  evidence and the scope of full-corpus and focused checks.

Each entity contains its event bindings followed by code owned exclusively by
that entity. Blocks reached by multiple entities, plus validated orphan event
code with no stored entrypoint, are emitted once under `shared_code`. This keeps
entity events together without duplicating shared instructions or inventing
routine lengths.

Control flow is followed from every stored routine entry and through computed
`A6` jump tables. Unreferenced regions are rendered as inferred orphan events
only when a linear decode has aligned branch targets or ends exactly at a
terminal instruction. Everything else remains preserved data. Instructions
retain original-PC/byte trace comments and event aliases, while operation
fields with no proven semantic schema remain explicit positional bytes.
Decoding conflicts remain diagnostics, so readability never depends on silently
discarding source bytes or inventing routine boundaries.

The per-resource report separates instruction coverage from total coverage:

```text
instruction_bytes + data_bytes = classified_bytes = bytecode_size
total_coverage_percent = 100.0
```

The out-of-band arrival table is rendered separately from executable events.
Other bytes inside the shared bytecode area that are not VM instructions appear
under `data` and remain byte-exact. The analysis report distinguishes them as
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

## Compiler Validation

```bash
python3 -m pytest tests -q
python3 tools/field_script.py verify extracted-field-scripts
python3 tools/field_script.py verify extracted-field-scripts --repack
python3 tools/field_script.py verify extracted-field-scripts --repack --resize
```

The corpus check compares exact XGA reconstruction, XGS reconstruction with only
verified neutral instruction-bit differences, and LZSS compression. `--repack`
uses the original disc paths in the manifest to rebuild Field containers.
The corpus has 729 unique assets: 4,453,533 ScriptsFile bytes, including 3,198,265
bytecode bytes. These checks are not run by a normal single-file compilation.

The resize stress test inserts a NOP before every instruction in every resource,
checks every original non-address operand and relocated symbolic reference,
assembles the linked IR again, and optionally repacks the modified containers.
Recorded implementation runs passed all 729 cases and produced 5,126,213 total
resized ScriptsFile bytes. See the evidence guide for validation scope; this is
not a claim that every later presentation change reran the complete corpus.
