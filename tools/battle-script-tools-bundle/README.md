# Xenogears Battle Script Tools

Self-contained Python 3.11+ tools for extracting, decompiling, recompiling and
repacking Battle Event VM scripts from retail Xenogears discs. They use only the
Python standard library.

Battle Event is the bytecode VM that runs scripted scenes inside turn-based
battles: dialogue, music, Event sprites, mecha animations, cameras, fades, the
post-battle return and outcome. Enemy AI programs stored in the enemy-set files
(directory `0x0D`) are a separate four-byte rule language and are not handled by
this bundle.

## Usage

From this bundle directory:

```bash
python3 tools/extract_disc_battle_scripts.py \
  /path/to/disc1.cue /path/to/disc2.cue
```

Inputs may be CUE files referring to MODE1/MODE2 BIN tracks, raw 2352-byte BIN
images, or 2048-byte ISO images. The default output is
`extracted-battle-scripts`; use `--output PATH` to override it. One or more
`--event N` options restrict extraction to selected Battle Event data indices.

## Output

```text
extracted-battle-scripts/
  manifest.json
  assets/<fat-index>_event-<NN>_<sha1>.script.bin
  assets/<fat-index>_event-<NN>_<sha1>.dialog.bin
  metadata/<fat-index>_event-<NN>_<sha1>.json
  catalog/catalog.json
  catalog/disc-01/event-00_<hash>/
    script.bin
    dialog.bin
    script.xgs
    script.xga
    bytecode.bin
    dialog.txt
    metadata.json
    sources.json
```

All Battle Event data lives in one indexed file, directory `0x20` file `2`: a
table of 96 LZSS streams forming 48 script/dialogue pairs. The formation byte
`+0x03` selects the pair (the Battle Event data index). `script.bin` is the
decompressed script resource: a `0x40`-byte prefix, the entity count, eight
`u16` entry offsets per entity, and the shared bytecode. `dialog.bin` is the
paired message bundle. `bytecode.bin` is a convenience view; `dialog.txt` lists
the decoded messages for reference.

`script.xgs` is a conservative event DSL rendering of the VM program. It uses
subsystem-qualified operations (`flow`, `state`, `dialogue`, `camera`,
`visual`, `audio`, `sprite`, `actor`, `battle`, `event`), named state
bindings, entity event tables, shared labels, conditional jumps, and preserved
data. Engine-defined variables carry their documented names; other variables are
named after what writes them or the operand they feed (`counter_0200`,
`x_0200`). Dialogue operations carry the decoded message text as a comment.

`script.xga` is independent lossless assembly containing complete instruction
encodings, symbolic references, all entity rows, the prefix and byte-exact data.
XGS compilation does not read it.

## Compile And Repack

From this bundle directory:

```bash
python3 tools/battle_script.py decompile /path/to/script.bin \
  --event 7 --dialog /path/to/dialog.bin --xgs script.xgs

# Edit script.xgs, then compile its statements:
python3 tools/battle_script.py compile script.xgs \
  --output script.modified.bin --xga script.modified.xga --map script.map.json

# Assembly can also be edited and built directly:
python3 tools/battle_script.py assemble script.modified.xga \
  --output script.assembled.bin

python3 tools/battle_script.py repack battle-event-data.bin \
  --event 7 --script script.modified.bin --output battle-event-data.modified.bin
```

The `.xgs` contains operations and symbolic arguments without encoding or layout
annotations. Each entity contains its event bindings and the code only it
reaches; code reached by several entities, and validated orphan code, appear
once under `shared_code`. Original byte traces, event comments and message text
are retained as comments. Removing or changing any comment produces the same
binary.

`decompile --xgs` writes only that source; XGA is written only when requested
with `--xga`. `--dialog` is optional and only adds message comments. Compilation
reads only the XGS file: no sibling file, original binary or sidecar is needed.

The linker recalculates positions, entity entry rows and jump targets on every
build, using one pipeline for edited and unedited source. Every one of the 48
retail scripts reproduces its original binary exactly from its XGS, also with
all comments removed; see [validation evidence](docs/RELOCATION_EVIDENCE.md).
For fixed-address assertions in the low-level representation, use
`assemble --layout exact`. The optional link map reports final placement and
`new` variable allocation.

`battle_script.py override` creates a `.psxmod` source package from a stock disc
and one or more compiled scripts, using the port's indexed-file replacement
mechanism. See [`docs/RECOMPILER_USAGE.md`](docs/RECOMPILER_USAGE.md) for the full
editing and runtime workflow.

Regenerate all catalog `.xgs`/`.xga` files from the extracted assets, checking
that each generated source compiles back to its asset byte for byte:

```bash
python3 tools/battle_script.py regenerate extracted-battle-scripts
```

## Documentation

- [`docs/FORMAT_AND_CONCEPTS.md`](docs/FORMAT_AND_CONCEPTS.md) explains the event
  data file, the script resource, entities, entries and slots, every top-level
  DSL section, shared code and non-instruction data.
- [`docs/DSL_REFERENCE.md`](docs/DSL_REFERENCE.md) documents values, variables,
  operand encodings, control flow, conditions, assignments, operations,
  diagnostics and source traces.
- [`docs/OBJECTS_AND_NAMESPACES.md`](docs/OBJECTS_AND_NAMESPACES.md) explains each
  namespace, the runtime state it affects and common cross-entity sequences.
- [`docs/OPERATION_CATALOG.md`](docs/OPERATION_CATALOG.md) is the generated
  per-opcode reference for all 76 dispatch entries: XGS spellings, operand
  layouts, handlers and documented behavior.
- [`docs/RECOMPILER_DESIGN.md`](docs/RECOMPILER_DESIGN.md) describes the compiler,
  relocation, LZSS and repacking rules, and the binary guarantees.
- [`docs/RECOMPILER_USAGE.md`](docs/RECOMPILER_USAGE.md) documents editing,
  assembly syntax, repacking and runtime override packages.
- [`docs/RELOCATION_EVIDENCE.md`](docs/RELOCATION_EVIDENCE.md) records the
  executable evidence for operand layouts and the corpus validation results.

## Format And Validation

Directory `0x20` file `2` begins with `u32 count`, `count` stream offsets and a
final offset equal to the file size. Streams use the resident LZSS decoder at
`0x80032E88`: control bits low-to-high, one selecting a two-byte back-reference
(12-bit distance, 4-bit length minus 3). The decoder handles complete
eight-token groups and only compares against the declared size between groups.
71 of the 96 retail streams therefore read up to seven bytes past their own
extent, into the next stream or the zero sector padding; those bytes are part of
the expanded resource and appear as preserved data at the end of the bytecode.

Control flow is followed from every stored entry through jumps and conditional
jumps. Unreferenced regions become orphan code only when a linear decode ends
exactly on a terminal instruction and contains more than stops and no-ops.
Everything else remains preserved data:

```text
instruction_bytes + data_bytes = classified_bytes = bytecode_size
total_coverage_percent = 100.0
```

The bundled `tools/battle_event_opcode_table.json` contains all 76 dispatch
names, sizes, handlers and behavior summaries. It is generated solely from the
Battle documentation:

```bash
python3 tools/build_opcode_table.py
python3 tools/build_dsl_documentation.py
```

## Compiler Validation

```bash
python3 tools/battle_script.py verify extracted-battle-scripts
python3 tools/battle_script.py verify extracted-battle-scripts --repack --resize
```

`verify` checks every unique script for byte-exact XGA and XGS reconstruction,
with and without comments, and linked XGA identity. `--resize` inserts a
`flow.nop()` before every source operation, checks that every existing operation
and reference is unchanged, and builds the relocated result. `--repack` rebuilds
the event data file from the stock disc recorded in `manifest.json` and checks
that it expands to the compiled script. These are structural checks, not proof
of gameplay equivalence; test authored changes in game.
